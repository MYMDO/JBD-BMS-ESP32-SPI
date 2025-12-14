#    Як це працює:

#    Скрипт "стукає" на вашу ESP32 і бере JSON-дані.
#    Сповіщення: Якщо заряд нижче 20% і йде розряд (струм мінусовий), Windows покаже спливаюче вікно.
#    Вимкнення: Якщо заряд 5% або менше, запускається команда shutdown /s /t 60.
#    Це дасть вам 60 секунд на збереження файлів.
#    Якщо світло раптом дали, ви можете скасувати вимкнення командою shutdown /a у командному рядку.

#    Як запустити:

#    Просто двічі клацніть на файл BMS.py.
#    Відкриється чорне вікно, де буде писатися поточний статус.

#    Як зробити, щоб запускалося саме і працювало у фоні:

#    Щоб чорне вікно не муляло очі, змініть розширення файлу з .py на .pyw.
#    Тепер при запуску скрипт працюватиме невидимо.

#    Щоб він стартував разом з Windows: Натисніть Win + R. Введіть shell:startup і натисніть Enter.
#    Скопіюйте ваш файл BMS.pyw у цю папку.

import time
import logging
import subprocess
import requests
import sys
import threading
import tkinter as tk
from dataclasses import dataclass
from plyer import notification
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# --- КОНФІГУРАЦІЯ ---
@dataclass
class Config:
    ESP_URL: str = "http://192.168.1.186/data"
    LOW_BATTERY_LIMIT: int = 20
    SHUTDOWN_LIMIT: int = 5     # Поріг запуску таймера
    NORMAL_INTERVAL: int = 60
    EMERGENCY_INTERVAL: int = 5
    TIMEOUT: int = 5
    
    SNOOZE_TIME: int = 300      # 5 хвилин
    WARNING_THRESHOLD: int = 60 # Коли показувати вікно знову

# Логування
logger = logging.getLogger("BMS_Monitor")
logger.setLevel(logging.INFO)
handler = logging.StreamHandler(sys.stdout)
handler.setFormatter(logging.Formatter('%(asctime)s - %(message)s', datefmt='%H:%M:%S'))
logger.addHandler(handler)

class BatteryMonitor:
    def __init__(self, config: Config):
        self.cfg = config
        self.warned_low = False
        self.shutdown_pending = False
        self.user_cancelled_forever = False 
        self.session = self._init_session()
        self.popup_root = None 
        self.timer_seconds_left = 0

    def _init_session(self) -> requests.Session:
        session = requests.Session()
        retry = Retry(total=3, backoff_factor=1, status_forcelist=[500, 502, 503, 504])
        adapter = HTTPAdapter(max_retries=retry)
        session.mount('http://', adapter)
        return session

    def fetch_data(self):
        try:
            response = self.session.get(
                self.cfg.ESP_URL, 
                timeout=self.cfg.TIMEOUT, 
                headers={'Connection': 'close'}
            )
            response.raise_for_status()
            return response.json()
        except Exception:
            return None

    # --- GUI ---
    def show_emergency_window(self, soc):
        def run_gui():
            if self.popup_root is not None:
                return

            self.popup_root = tk.Tk()
            self.popup_root.title("System Power Critical")
            
            # Стиль
            bg_color = "#2b2b2b"
            text_color = "#ff4444"
            
            w, h = 450, 240
            ws = self.popup_root.winfo_screenwidth()
            hs = self.popup_root.winfo_screenheight()
            x = int((ws/2) - (w/2))
            y = int((hs/2) - (h/2))
            
            self.popup_root.geometry(f"{w}x{h}+{x}+{y}")
            self.popup_root.configure(bg=bg_color) 
            self.popup_root.attributes('-topmost', True) 
            self.popup_root.overrideredirect(True) 

            frame = tk.Frame(self.popup_root, bg=bg_color, highlightbackground="red", highlightthickness=2)
            frame.pack(fill=tk.BOTH, expand=True)

            tk.Label(frame, text="⚠️ КРИТИЧНИЙ РОЗРЯД ⚠️", font=("Segoe UI", 16, "bold"), 
                     bg=bg_color, fg=text_color).pack(pady=(20, 5))

            self.lbl_info = tk.Label(
                frame, 
                text=f"Заряд: {soc}%\nВимкнення через {self.timer_seconds_left} с", 
                font=("Segoe UI", 12), 
                bg=bg_color, fg="white"
            )
            self.lbl_info.pack(pady=10)

            btn_frame = tk.Frame(frame, bg=bg_color)
            btn_frame.pack(pady=20)

            # Кнопки
            mins = self.cfg.SNOOZE_TIME // 60
            btn_snooze = tk.Button(
                btn_frame, 
                text=f"ВІДКЛАСТИ\n(+{mins} хв)", 
                font=("Segoe UI", 10, "bold"), 
                bg="#005a9e", fg="white", 
                activebackground="#004578", activeforeground="white",
                relief="flat",
                command=self.confirm_snooze,
                width=18, height=2
            )
            btn_snooze.pack(side=tk.LEFT, padx=10)

            btn_cancel = tk.Button(
                btn_frame, 
                text="СКАСУВАТИ\nповністю", 
                font=("Segoe UI", 10, "bold"), 
                bg="#107c10", fg="white",
                activebackground="#0b5a0b", activeforeground="white",
                relief="flat",
                command=self.manual_cancel,
                width=18, height=2
            )
            btn_cancel.pack(side=tk.LEFT, padx=10)

            self.update_gui_timer()
            self.popup_root.mainloop()

        if not self.popup_root:
            self.timer_seconds_left = 60
            gui_thread = threading.Thread(target=run_gui, daemon=True)
            gui_thread.start()

    def update_gui_timer(self):
        """Цей цикл працює у GUI-потоці й перевіряє стан."""
        if not self.popup_root: 
            return

        # 1. ГОЛОВНА ПЕРЕВІРКА: Якщо основний потік зняв тривогу (shutdown_pending = False)
        #    то вікно закриває саме себе.
        if not self.shutdown_pending:
            self.close_window_internal()
            return

        # 2. Оновлення тексту
        try:
            self.lbl_info.config(text=f"Вимкнення системи через:\n{self.timer_seconds_left} сек")
        except:
            pass 

        # 3. Показати/Сховати
        if self.timer_seconds_left > self.cfg.WARNING_THRESHOLD:
            self.popup_root.withdraw()
        else:
            self.popup_root.deiconify()
            self.popup_root.attributes('-topmost', True)

        # 4. Відлік
        if self.timer_seconds_left > 0:
            self.timer_seconds_left -= 1
            self.popup_root.after(1000, self.update_gui_timer)
        else:
            logger.critical("ЧАС ВИЙШОВ. ВИМИКАЄМО ПК.")
            self.close_window_internal()
            subprocess.run("shutdown /s /t 0", shell=True)
            sys.exit()

    def close_window_internal(self):
        """Закриття вікна зсередини GUI-потоку (безпечно)."""
        if self.popup_root:
            try:
                self.popup_root.destroy()
            except:
                pass
            finally:
                self.popup_root = None

    def confirm_snooze(self):
        logger.info(f"Дія: ВІДКЛАСТИ на {self.cfg.SNOOZE_TIME} с.")
        self.timer_seconds_left += self.cfg.SNOOZE_TIME
        if self.popup_root:
            self.popup_root.withdraw()

    def manual_cancel(self):
        logger.info("КОРИСТУВАЧ СКАСУВАВ ЗАХИСТ.")
        self.user_cancelled_forever = True
        self.shutdown_pending = False
        self.warned_low = False
        self.close_window_internal()

    # --- ЛОГІКА ---
    def trigger_shutdown(self, soc):
        if not self.shutdown_pending:
            logger.critical(f"КРИТИЧНИЙ РОЗРЯД ({soc}%). ЗАПУСК GUI ТАЙМЕРА.")
            self.shutdown_pending = True
            self.show_emergency_window(soc)

    def process_logic(self, soc, current):
        msg = f"Заряд: {soc}%, Струм: {current}A"
        
        if self.user_cancelled_forever:
            msg += " [ЗАХИСТ ВИМКНЕНО]"
        elif self.shutdown_pending:
            status = "СХОВАНО" if self.timer_seconds_left > self.cfg.WARNING_THRESHOLD else "НА ЕКРАНІ"
            msg += f" [ТАЙМЕР: {self.timer_seconds_left}с | {status}]"
        
        logger.info(msg)

        is_charging = current >= 0

        # --- КЛЮЧОВИЙ МОМЕНТ: ОБРОБКА ПОЯВИ СВІТЛА ---
        if is_charging:
            # Якщо до цього йшов таймер вимкнення
            if self.shutdown_pending:
                logger.info("⚡ ЖИВЛЕННЯ ВІДНОВЛЕНО! Скасування таймера...")
                self.shutdown_pending = False 
                # МИ НЕ ВИКЛИКАЄМО close_window ТУТ!
                # Ми просто поставили прапорець False, а вікно побачить це через 1 сек і закриється саме.
                self.notify("Живлення є!", "Вимкнення скасовано.")
            
            # Якщо було повне скасування захисту
            if self.user_cancelled_forever:
                logger.info("⚡ ЖИВЛЕННЯ ВІДНОВЛЕНО! Поновлюємо захист.")
                self.user_cancelled_forever = False
            
            if soc > self.cfg.LOW_BATTERY_LIMIT:
                self.warned_low = False
            return

        if self.user_cancelled_forever:
            return 

        if soc <= self.cfg.SHUTDOWN_LIMIT:
            self.trigger_shutdown(soc)
        elif soc <= self.cfg.LOW_BATTERY_LIMIT and not self.warned_low:
            self.warned_low = True
            self.notify("Увага!", f"Заряд {soc}%.")

    def run(self):
        logger.info(f"Моніторинг запущено. IP: {self.cfg.ESP_URL}")
        while True:
            data = self.fetch_data()
            if data:
                soc = data.get('s')
                current = data.get('c')
                if soc is not None:
                    self.process_logic(soc, current)
            else:
                logger.warning("Немає зв'язку з ESP")
            
            time.sleep(self.cfg.EMERGENCY_INTERVAL if self.shutdown_pending else self.cfg.NORMAL_INTERVAL)

    def notify(self, title, message):
        try:
            notification.notify(title=title, message=message, app_name='BMS Monitor', timeout=5)
        except:
            pass

if __name__ == "__main__":
    monitor = BatteryMonitor(Config())
    monitor.run()