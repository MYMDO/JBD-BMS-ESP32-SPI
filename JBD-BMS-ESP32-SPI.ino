#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h> 

// --- НАЛАШТУВАННЯ WI-FI ---
const char* ssid = "WIFI SSID";
const char* password = "password";

// --- ПІНИ ---
#define BUTTON_PIN 15
#define BUZZER_PIN 13
#define BMS_RX_PIN 16
#define BMS_TX_PIN 17

// --- ПІНИ ДИСПЛЕЯ (SPI) ---
#define OLED_MOSI 23
#define OLED_CLK  18
#define OLED_DC   2
#define OLED_CS   5
#define OLED_RST  4

// Налаштування сну
#define IDLE_TIMEOUT_MS  300000  // 5 хвилин
#define SLEEP_DURATION_S 1800    // 30 хвилин сну
#define WAKEUP_PIN       GPIO_NUM_15 

unsigned long idleStartTime = 0; // Змінна лічильника

Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &SPI, OLED_DC, OLED_RST, OLED_CS);
HardwareSerial bmsSerial(2);

// Команди JBD
const byte cmdReadBasic[] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
const byte cmdReadCells[] = {0xDD, 0xA5, 0x04, 0x00, 0xFF, 0xFC, 0x77};

// --- ЗМІННІ ДАНИХ ---
float voltage = 0.0;
float current = 0.0;
float power = 0.0;
float capacityRemain = 0.0;
float capacityFull = 0.0;
int soc = 0;
char timeStr[16] = "--:--";
int cycleCount = 0;
uint16_t protectionStatus = 0;

uint16_t cellVoltages[32];
int cellCount = 0;
bool bmsOnline = false;
uint16_t delta = 0;
uint32_t balanceStatus = 0;

// Температура та MOSFET
float temp1 = -99.0;
float temp2 = -99.0;
float temp3 = -99.0;
bool fetCharge = false;
bool fetDischarge = false;

// --- ЛОГІКА КНОПКИ ---
volatile bool buttonPressed = false;
volatile unsigned long lastInterruptTime = 0;

// Інтерфейс
int displayPage = 0;

// Таймери
unsigned long previousMillis = 0;
const long interval = 2000;
unsigned long lastBuzzerTime = 0;
unsigned long lastWifiCheck = 0;
int dataCycleCounter = 0;

WebServer server(80);
char jsonBuffer[4096]; 

// Прототипи
void drawCells(int startIdx, int endIdx);
void updateDisplay();
void requestBMSData();
void sendBMSCommand(const byte* cmd);
bool readBMSResponse(byte type);
void handleBuzzer();
void IRAM_ATTR isrButton();
void setupOTA();
void goToDeepSleep();

// --- HTML (Дизайн: Soft UI + Блискавка) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>JBD BMS Monitor</title>
  <style>
    /* SOFT DARK THEME */
    body { font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; text-align: center; background-color: #121212; color: #b0bec5; margin:0; padding:10px;}
    
    .card { 
      background: #1e1e1e; 
      max-width: 500px; 
      margin: 10px auto; 
      padding: 20px; 
      border-radius: 12px; 
      border: 1px solid #333; 
      box-shadow: 0 4px 10px rgba(0,0,0,0.3);
    }
    
    h2 { color: #eceff1; margin-top: 0; margin-bottom: 15px; font-weight: 500; font-size: 1.5rem; letter-spacing: 1px;}

    /* БЛОК СТАТУСУ СИСТЕМИ */
    .system-status {
      background: #263238;
      padding: 8px;
      border-radius: 6px;
      margin-bottom: 20px;
      font-weight: bold;
      border: 1px solid #37474f;
      font-size: 0.9rem;
      text-transform: uppercase;
      letter-spacing: 1px;
    }
    .status-ok { color: #a5d6a7; border-color: #2e7d32; background: rgba(46, 125, 50, 0.2); } 
    .status-err { color: #ef9a9a; border-color: #c62828; background: rgba(198, 40, 40, 0.2); animation: pulse 2s infinite; }

    @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.7; } 100% { opacity: 1; } }

    /* Гнучкі рядки */
    .flex-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; }
    
    .unit-lbl { font-size: 0.75rem; color: #78909c; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 2px; }
    
    .val-huge { font-size: 2.2rem; font-weight: 600; line-height: 1; }
    .val-big { font-size: 1.8rem; font-weight: 600; }
    .val-med { font-size: 1.4rem; font-weight: 500; color: #eceff1; }
    
    .color-v { color: #80cbc4; } 
    .color-c { color: #ef9a9a; } 
    .color-s { color: #90caf9; } 
    .color-p { color: #ffe082; } 

    .fet-container { display: flex; flex-direction: column; gap: 5px; }
    .mosfet { font-weight: 700; padding: 4px 10px; border-radius: 4px; font-size: 0.8rem; min-width: 40px; }
    .on { background-color: #2e7d32; color: #e8f5e9; } 
    .off { background-color: #37474f; color: #78909c; }

    .temps-row { 
      display: flex; justify-content: space-around; 
      background: #263238; padding: 10px; border-radius: 6px; 
      font-size: 0.85rem; color: #b0bec5;
      margin-bottom: 20px;
    }
    .t-val { font-weight: bold; color: #fff; font-size: 1rem;}

    .cells-header {
      display: flex; justify-content: space-between; align-items: flex-end;
      border-bottom: 1px solid #333; padding-bottom: 5px; margin-bottom: 10px;
    }
    .cy-box { color: #ce93d8; font-weight: bold; font-size: 0.9rem; }

    .grid-container { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; }
    .cell-box { background-color: #263238; padding: 8px 2px; border-radius: 4px; text-align: center; position: relative; border: 1px solid #37474f; }
    .cell-num { font-size: 0.65rem; color: #546e7a; display:block; margin-bottom:1px;}
    .cell-val { font-size: 0.95rem; font-weight: 500; color: #eceff1; }
    .low { color: #ef9a9a; } .high { color: #a5d6a7; }
    
    .balancing { border-color: #ffca28; background-color: #3e2723; }
    .bal-icon { position: absolute; top: -5px; right: -2px; color: #ffca28; font-size: 14px; display: none; text-shadow: 1px 1px 0 #000; }
    .balancing .bal-icon { display: block; }
  </style>
</head>
<body>
  <div class="card">
    <h2>JBD BMS Monitor</h2>
    <div id="sys-stat-box" class="system-status status-ok">SYSTEM NORMAL</div>
    
    <div class="flex-row">
        <div style="text-align: left;">
            <div class="unit-lbl">Voltage</div>
            <div class="val-huge color-v"><span id="volt">--</span><span style="font-size:1rem">V</span></div>
        </div>
        <div class="fet-container">
            <span id="fetC" class="mosfet off">CHG</span>
            <span id="fetD" class="mosfet off">DSG</span>
        </div>
        <div style="text-align: right;">
            <div class="unit-lbl">SOC</div>
            <div class="val-huge color-s"><span id="soc">--</span><span style="font-size:1rem">%</span></div>
        </div>
    </div>
    
    <hr style="border: 0; border-top: 1px solid #333; margin: 15px 0;">

    <div class="flex-row">
        <div style="text-align: left;">
            <div class="unit-lbl">Current</div>
            <div class="val-big color-c"><span id="curr">--</span><span style="font-size:1rem">A</span></div>
        </div>
        
        <div style="text-align: center;">
            <div class="unit-lbl">TIME</div>
            <div class="val-med"><span id="time">--</span></div>
        </div>

        <div style="text-align: right;">
            <div class="unit-lbl">Power</div>
            <div class="val-big color-p"><span id="pwr">--</span><span style="font-size:1rem">W</span></div>
        </div>
    </div>

    <div style="margin-bottom: 20px;"></div>

    <div class="temps-row">
        <div>T1 - MFET: <span id="t1" class="t-val">--</span>°</div>
        <div style="border-left: 1px solid #455a64; padding-left:10px;">T2 - bat: <span id="t2" class="t-val">--</span>°</div>
        <div style="border-left: 1px solid #455a64; padding-left:10px;">T3 - bat: <span id="t3" class="t-val">--</span>°</div>
    </div>

    <div class="cells-header">
        <span style="color: #78909c; font-size: 0.8rem; font-weight: bold;">CELL VOLTAGES</span>
        <span class="cy-box">CYCLES: <span id="cy">--</span></span>
        <span style="color: #ffca28; font-size: 0.9rem;">Δ <span id="delta">--</span> mV</span>
    </div>
    <div id="cells-container" class="grid-container"></div>
  </div>

<script>
function getErrorText(err) {
    if (err === 0) return "SYSTEM NORMAL";
    let errors = [];
    if (err & 0x0001) errors.push("Cell OVP");
    if (err & 0x0002) errors.push("Cell UVP");
    if (err & 0x0004) errors.push("Pack OVP");
    if (err & 0x0008) errors.push("Pack UVP");
    if (err & 0x0010) errors.push("Chg Temp High");
    if (err & 0x0020) errors.push("Chg Temp Low");
    if (err & 0x0040) errors.push("Dsg Temp High");
    if (err & 0x0080) errors.push("Dsg Temp Low");
    if (err & 0x0100) errors.push("Chg Overcurrent");
    if (err & 0x0200) errors.push("Dsg Overcurrent");
    if (err & 0x0400) errors.push("Short Circuit");
    if (err & 0x0800) errors.push("IC Error");
    if (err & 0x1000) errors.push("MOSFET Lock");
    return errors.join(", ");
}

function fetchLoop() {
  fetch("/data")
    .then(r => r.json())
    .then(data => {
      document.getElementById("volt").innerHTML = data.v.toFixed(2);
      document.getElementById("curr").innerHTML = data.c.toFixed(1);
      document.getElementById("pwr").innerHTML = data.p.toFixed(0);
      document.getElementById("soc").innerHTML = data.s;
      document.getElementById("time").innerHTML = data.t;
      document.getElementById("delta").innerHTML = data.d;
      document.getElementById("cy").innerHTML = data.cy;
      document.getElementById("t1").innerHTML = data.t1.toFixed(0);
      document.getElementById("t2").innerHTML = data.t2.toFixed(0);
      document.getElementById("t3").innerHTML = data.t3.toFixed(0);

      let statBox = document.getElementById("sys-stat-box");
      if (data.err === 0) {
          statBox.className = "system-status status-ok";
          statBox.innerHTML = "SYSTEM NORMAL";
      } else {
          statBox.className = "system-status status-err";
          statBox.innerHTML = "WARNING: " + getErrorText(data.err);
      }

      let fc = document.getElementById("fetC");
      let fd = document.getElementById("fetD");
      if(fc) fc.className = data.fc ? "mosfet on" : "mosfet off";
      if(fd) fd.className = data.fd ? "mosfet on" : "mosfet off";

      let cellsHtml = "";
      for (let i = 0; i < data.cells.length; i++) {
        let colorClass = "";
        if(data.cells[i] < 3000) colorClass = "low";
        if(data.cells[i] > 4150) colorClass = "high";
        let balClass = ((data.bal >> i) & 1) ? "balancing" : "";
        let balIcon = ((data.bal >> i) & 1) ? "<span class='bal-icon'>⚡</span>" : "";
        cellsHtml += `<div class="cell-box ${balClass}">
            ${balIcon}<span class="cell-num">#${i+1}</span>
            <span class="cell-val ${colorClass}">${data.cells[i]}</span>
        </div>`;
      }
      document.getElementById("cells-container").innerHTML = cellsHtml;
    })
    .catch(e => console.log(e));
}
setInterval(fetchLoop, 2000);
fetchLoop();
</script>
</body>
</html>
)rawliteral";

// --- ISR ---
void IRAM_ATTR isrButton() {
  unsigned long interruptTime = millis();
  if (interruptTime - lastInterruptTime > 250) {
    buttonPressed = true;
    lastInterruptTime = interruptTime;
  }
}

void setup() {
  Serial.begin(115200);

  // --- Перевірка причини запуску ---
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
     Serial.println("Woke up by BUTTON!");
     // Можна тут одразу увімкнути екран, код далі це зробить
  } else if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
     Serial.println("Woke up by TIMER!");
  }
  
  idleStartTime = millis(); // Скидаємо таймер при запуску

  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 30000, 
      .idle_core_mask = (1 << 0),
      .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  bmsSerial.begin(9600, SERIAL_8N1, BMS_RX_PIN, BMS_TX_PIN);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), isrButton, FALLING);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  if(!display.begin(0, true)) {
    Serial.println("OLED failed");
    for(;;);
  }
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0,0);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
      display.println("Connected!");
      display.print("IP: "); display.println(WiFi.localIP());
      setupOTA();
  } else {
      display.println("WiFi Failed");
  }
  display.display();
  delay(2000);

  // Маршрути
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", index_html);
  });

  server.on("/data", HTTP_GET, []() {

    idleStartTime = millis(); // Скидаємо таймер сну, бо клієнт запитує дані

    int len = snprintf(jsonBuffer, sizeof(jsonBuffer), 
        "{\"v\":%.2f,\"c\":%.1f,\"p\":%.0f,\"s\":%d,\"d\":%d,\"t\":\"%s\",\"cy\":%d,\"err\":%u,"
        "\"t1\":%.1f,\"t2\":%.1f,\"t3\":%.1f,\"fc\":%d,\"fd\":%d,\"bal\":%u,\"cells\":[",
        voltage, current, power, soc, delta, timeStr, cycleCount, protectionStatus,
        temp1, temp2, temp3, fetCharge, fetDischarge, balanceStatus);

    for(int i=0; i < cellCount; i++) {
        if (len >= sizeof(jsonBuffer) - 10) break; 
        len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, "%d", cellVoltages[i]);
        if(i < cellCount - 1) {
            len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, ",");
        }
    }
    snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, "]}");
    server.send(200, "application/json", jsonBuffer);
  });

  server.begin();
}

void loop() {
  esp_task_wdt_reset(); 
  ArduinoOTA.handle();
  server.handleClient();

  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        WiFi.reconnect();
    }
  }

  if (buttonPressed) {
    buttonPressed = false;
    displayPage++;
    int maxPage = 3; 
    if (cellCount > 11) maxPage = 4; 
    if (displayPage > maxPage) displayPage = 0;
    updateDisplay();
  }

  handleBuzzer();

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    requestBMSData();
    updateDisplay();

// --- ЛОГІКА СНУ ---
    // Якщо струм є (більше 0.2А в будь-яку сторону) АБО йде балансування АБО є помилки
    if (abs(current) > 0.2 || balanceStatus > 0 || protectionStatus > 0) {
       // Активність є, скидаємо таймер простою
       idleStartTime = millis();
    } else {
       // Активності немає. Перевіряємо, як довго ми простоюємо
       if (millis() - idleStartTime > IDLE_TIMEOUT_MS) {
          // Якщо ми тут - значить 5 хвилин струму не було.
          goToDeepSleep();
       }
    }
    // ------------------
  }
}

void setupOTA() {
  ArduinoOTA.setHostname("JBD-BMS-ESP32");
  ArduinoOTA.setPassword("admin");
  
  ArduinoOTA.onStart([]() {
    display.clearDisplay(); display.setTextSize(2); display.setCursor(0, 10);
    display.println("OTA UPDATE"); display.display();
  });
  
  ArduinoOTA.onEnd([]() {
    display.clearDisplay(); display.setCursor(0, 20); display.println("DONE!"); display.display();
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    
    esp_task_wdt_reset();   // ВАЖЛИВО: Кажемо таймеру, що ми живі, поки завантажується
    
    int percent = (progress / (total / 100));
    display.drawRect(14, 45, 100, 10, SH110X_WHITE);
    display.fillRect(14, 45, percent, 10, SH110X_WHITE);
    display.display();
  });
  
  ArduinoOTA.begin();
}

void handleBuzzer() {
  const int alarmSoc = 10;
  const unsigned long activeDuration = 60000;
  const unsigned long totalCycle = 240000; 
  const int beepInterval = 10000;
  static unsigned long cycleStartTime = 0;
  static bool inAlarmState = false;
  static bool wasBuzzing = false;

  if (bmsOnline && soc <= alarmSoc && current <= 0) {
    unsigned long currentMillis = millis();
    if (!inAlarmState) { inAlarmState = true; cycleStartTime = currentMillis; }
    unsigned long elapsed = currentMillis - cycleStartTime;
    if ((elapsed % totalCycle) < activeDuration) {
      if (currentMillis - lastBuzzerTime >= beepInterval) {
        lastBuzzerTime = currentMillis;
        tone(BUZZER_PIN, 5000, 50);
        wasBuzzing = true;
      }
    }
  } else {
    inAlarmState = false;
    if (wasBuzzing) { noTone(BUZZER_PIN); wasBuzzing = false; }
  }
}

void requestBMSData() {
  sendBMSCommand(cmdReadBasic);
  readBMSResponse(0x03);

  unsigned long t = millis();
  while((millis() - t) < 50) {
    server.handleClient();
    ArduinoOTA.handle();
  }

  dataCycleCounter++;
  if (dataCycleCounter >= 5) {
    dataCycleCounter = 0;
    sendBMSCommand(cmdReadCells);
    readBMSResponse(0x04);
  }
}

void sendBMSCommand(const byte* cmd) {
  while (bmsSerial.available()) bmsSerial.read(); 
  bmsSerial.write(cmd, 7);
}

bool readBMSResponse(byte type) {
  unsigned long startWait = millis();
  while(bmsSerial.available() < 4 && (millis() - startWait < 150)) {
    server.handleClient(); ArduinoOTA.handle(); 
  }

  if (bmsSerial.available()) {
    byte response[128];
    int len = bmsSerial.readBytes(response, 128);

    if (len > 4 && response[0] == 0xDD && response[1] == type && response[2] == 0x00) {
      bmsOnline = true;
      if (type == 0x03) {
        voltage = ((response[4] << 8) | response[5]) / 100.0;
        current = (int16_t)((response[6] << 8) | response[7]) / 100.0; 
        soc = response[23];
        power = voltage * current;
        uint16_t balLow = (response[16] << 8) | response[17];
        uint16_t balHigh = (response[18] << 8) | response[19];
        balanceStatus = ((uint32_t)balHigh << 16) | balLow;
        protectionStatus = (response[20] << 8) | response[21]; 
        
        byte mosfetByte = response[24];
        fetCharge = (mosfetByte & 0x01);
        fetDischarge = (mosfetByte & 0x02);

        int ntcCount = response[26];
        if (ntcCount > 0) temp1 = (((response[27] << 8) | response[28]) - 2731) / 10.0;
        if (ntcCount > 1) temp2 = (((response[29] << 8) | response[30]) - 2731) / 10.0;
        if (ntcCount > 2) temp3 = (((response[31] << 8) | response[32]) - 2731) / 10.0;

        capacityRemain = ((response[8] << 8) | response[9]) / 100.0;
        capacityFull = ((response[10] << 8) | response[11]) / 100.0;
        cycleCount = (response[12] << 8) | response[13];

        float absCurrent = abs(current);
        if (absCurrent > 0.1) {
            float timeHours = (current < 0) ? (capacityRemain / absCurrent) : ((capacityFull - capacityRemain) / absCurrent);
            if (timeHours > 99) {
                snprintf(timeStr, sizeof(timeStr), ">99h");
            } else {
                int h = (int)timeHours;
                int m = (int)((timeHours - h) * 60);
                snprintf(timeStr, sizeof(timeStr), "%dh %dm", h, m);
            }
        } else {
            snprintf(timeStr, sizeof(timeStr), "--:--");
        }
      }
      else if (type == 0x04) {
        int dataLen = response[3];
        cellCount = dataLen / 2;
        if(cellCount > 32) cellCount = 32;
        uint16_t minV = 65535; uint16_t maxV = 0;
        for(int i=0; i<cellCount; i++) {
          cellVoltages[i] = (response[4 + i*2] << 8) | response[5 + i*2];
          if (cellVoltages[i] > maxV) maxV = cellVoltages[i];
          if (cellVoltages[i] < minV) minV = cellVoltages[i];
        }
        delta = (cellCount > 0) ? (maxV - minV) : 0;
      }
      return true;
    }
  }
  if (type == 0x03) bmsOnline = false;
  return false;
}

void updateDisplay() {
  display.clearDisplay();
  if (!bmsOnline) {
    display.setTextSize(2); display.setCursor(16, 24); display.println("BMS LOST");
    display.setTextSize(1); display.setCursor(15, 55); display.print("IP:"); display.print(WiFi.localIP());
    display.display(); return;
  }
  if (displayPage == 0) {
    display.setTextSize(2); display.setCursor(0, 0); display.print(soc); display.print("%");
    String pwrStr = String((int)abs(power)) + "W";
    int pwrX = 128 - (pwrStr.length() * 12); if (pwrX < 60) pwrX = 60;
    display.setCursor(pwrX, 0); display.print(pwrStr);
    display.setCursor(0, 25); if (abs(current) > 0.1) display.print((current > 0) ? "C " : "D "); display.print(timeStr);
    display.setTextSize(1); int bottomY = 54;
    display.setCursor(0, bottomY); display.print(voltage, 1); display.print("V");
    String currStr = String(abs(current), 1) + "A";
    int currX = 128 - (currStr.length() * 6); display.setCursor(currX, bottomY); display.print(currStr);
  } else if (displayPage == 1) {
    display.setTextSize(1); display.setCursor(0,0);
    display.printf("Cells 1-%d", (cellCount < 11 && cellCount > 0) ? cellCount : 11);
    display.setCursor(80, 0); display.printf("D:%dmV", delta);
    drawCells(0, 11);
  } else if (displayPage == 2 && cellCount > 11) {
    display.setTextSize(1); display.setCursor(0,0);
    display.print("Cells 12-22");
    display.setCursor(80, 0); display.printf("D:%dmV", delta);
    drawCells(11, 22);
  } else if (displayPage == (cellCount > 11 ? 3 : 2)) { 
    display.setTextSize(2);
    display.setCursor(0, 0); display.print("T1: "); (temp1 > -90) ? display.print(temp1, 1) : display.print("--");
    display.setCursor(0, 22); display.print("T2: "); (temp2 > -90) ? display.print(temp2, 1) : display.print("--");
    display.setCursor(0, 44); display.print("T3: "); (temp3 > -90) ? display.print(temp3, 1) : display.print("--");
  } else {
    display.setTextSize(1); display.setCursor(0, 0); display.println("--- SYSTEM INFO ---");
    display.setCursor(0, 20); display.print("IP: "); display.println(WiFi.localIP());
    display.setCursor(0, 36); display.print("Cycles: "); display.println(cycleCount);
    display.setCursor(0, 52); long upSec = millis() / 1000; int upMin = upSec / 60; int upHour = upMin / 60;
    display.print("Uptime: "); display.print(upHour); display.print("h "); display.print(upMin % 60); display.println("m");
  }
  display.display();
}

void drawCells(int startIdx, int endIdx) {
  int col = 0; int row = 10;
  for(int i = startIdx; i < endIdx; i++) {
    if (i >= cellCount) break;
    display.setCursor(col == 0 ? 0 : 64, row);
    if(i+1 < 10) display.print("0"); display.print(i+1);
    display.print(((balanceStatus >> i) & 1) ? "*" : ":"); display.print(cellVoltages[i]);
    if (col == 1) { col = 0; row += 9; } else { col = 1; }
  }
}

void goToDeepSleep() {
  Serial.println("Going to sleep...");
  
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20, 25);
  display.println("SLEEPING");
  display.display();
  delay(1000); 
  
  display.oled_command(0xAE); // 0xAE = Display OFF

  // Налаштування пробудження
  esp_sleep_enable_ext0_wakeup(WAKEUP_PIN, 0); 
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_S * 1000000ULL);

  Serial.flush(); 
  esp_deep_sleep_start();
}