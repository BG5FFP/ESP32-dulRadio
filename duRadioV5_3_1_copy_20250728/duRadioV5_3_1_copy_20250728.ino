#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <TFT_eSPI.h>
#include <OneButton.h>
#include <RDA5807.h>
#include <RotaryEncoder.h>
#include <Preferences.h>
#include <time.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include "logo.h"
#include "ZdyLwFont_20.h"
#include "frame.h"


// Pin definitions
#define I2S_DOUT      6
#define I2S_BCLK      7
#define I2S_LRC       21
#define AUDIO_SEL     3    // 74HC4053D audio channel select (0=FM, 1=Internet)

#define BTN_PLAY      0
#define BTN_PREV      47
#define BTN_NEXT      1
#define BTN_VOL_UP    2
#define BTN_VOL_DOWN  42

#define ROT_CLK       4
#define ROT_DT        48

#define I2C_SDA       18
#define I2C_SCL       8

#define BATTERY_PIN   5    // ADC pin for battery voltage monitoring


// Objects
Audio audio;
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr  = TFT_eSprite(&tft); 
TFT_eSprite ani  = TFT_eSprite(&tft); //music bar sprite

RDA5807 radio;
RotaryEncoder encoder(ROT_CLK, ROT_DT, RotaryEncoder::LatchMode::TWO03);
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;


// NTP Client
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp1.aliyun.com", 60*60*8, 60000); // UTC-5 (EST), update every minute

// Button objects
OneButton btnPlay(BTN_PLAY, true);
OneButton btnPrev(BTN_PREV, true);
OneButton btnNext(BTN_NEXT, true);
OneButton btnVolUp(BTN_VOL_UP, true);
OneButton btnVolDown(BTN_VOL_DOWN, true);
// OneButton btnRotary(ROT_SW, true);

// System variables
enum RadioMode { INTERNET_RADIO, FM_RADIO };
enum SystemMode { NORMAL_MODE, CONFIG_MODE };

RadioMode currentRadioMode = INTERNET_RADIO;
SystemMode currentSystemMode = NORMAL_MODE;

// Internet radio stations
struct Station {
  String name;
  String url;
};

std::vector<Station> internetStations;
int currentInternetStation = 0;

// Default stations (fallback if no CSV loaded)
Station defaultStations[] = {
  {"上海经典947", "https://lhttp.qtfm.cn/live/267/64k.mp3"},
  {"北京音乐广播", "https://lhttp.qtfm.cn/live/332/64k.mp3"},
  {"内蒙古音乐之声", "https://lhttp.qtfm.cn/live/1886/64k.mp3"},
  {"浙江音乐调频", "https://lhttp.qtfm.cn/live/4866/64k.mp3"},
  {"久久金曲 FM99.9", "https://lhttp.qtfm.cn/live/20211694/64k.mp3"}
};

// WiFi configuration
String wifiSSID = "";
String wifiPassword = "";
bool wifiConfigured = false;
bool wifiConnected = false;

// Config mode variables
const char* configSSID = "ESP32-Radio-Config";
const char* configPassword = "12345678";
unsigned long configModeTimer = 0;
const unsigned long configModeTimeout = 300000; // 5 minutes


// FM Radio variables
uint16_t currentFMFreq = 9410; // 94.1 MHz in 10kHz units
uint16_t minFreq = 8750;        // 87.5 MHz
uint16_t maxFreq = 10350;       // 103.5 MHz
int fmVolume = 8;               // FM volume (0-15)

// Audio control
int internetVolume = 15;        // Internet radio volume (0-21)
bool isPlaying = false;
bool fmMuted = false;

// Display variables
unsigned long lastDisplayUpdate = 0;
const unsigned long displayUpdateInterval = 500;
unsigned long lastTimeUpdate = 0;
const unsigned long timeUpdateInterval = 1000;

// Signal and battery monitoring variables
int wifiRSSI = 0;
float batteryVoltage = 0.0;
int batteryPercent = 0;
unsigned long lastSignalUpdate = 0;
const unsigned long signalUpdateInterval = 2000; // Update every 2 seconds

// Battery voltage divider constants (adjust based on your voltage divider)
const float VOLTAGE_DIVIDER_RATIO = 2.0; // R1=R2, so Vout = Vin/2
const float ADC_REFERENCE_VOLTAGE = 3.3;
const float ADC_RESOLUTION = 4095.0;
const float BATTERY_MIN_VOLTAGE = 3.0; // Minimum battery voltage
const float BATTERY_MAX_VOLTAGE = 4.2; // Maximum battery voltage (Li-ion)

// // WiFi credentials
// const char* ssid = "ASUS";
// const char* password = "88288595";

void setup() {
   Serial.begin(115200);

  // Initialize SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS Mount Failed");
    return;
  }
    
  // Initialize preferences
  preferences.begin("radio", false);
  loadSettings();
  loadStationList();
  
  // Initialize pins
  pinMode(AUDIO_SEL, OUTPUT);
  // pinMode(BATTERY_PIN, INPUT);
  digitalWrite(AUDIO_SEL, currentRadioMode); // 1=FM, 0=Internet
  
  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  
  // Initialize display
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("ESP32 duRadio", 10, 10);
  tft.setTextSize(1);
  tft.drawString("Initializing...", 10, 30);

  spr.createSprite(320, 170);
  spr.setSwapBytes(true);
  //music bar
  ani.createSprite(47, 60);
  ani.setSwapBytes(true);
 
  // Initialize buttons
  setupButtons();
  
    
  // Check for config mode (hold rotary button during boot)
  if (digitalRead(BTN_PLAY) == LOW) {
    enterConfigMode();
    return;
  }

  // Try to connect to WiFi
  if (wifiConfigured) {
    connectToWiFi();
  } else {
    Serial.println("WiFi not configured, entering config mode");
    enterConfigMode();
    return;
  }

  // Initialize NTP client
  timeClient.begin();
  timeClient.update();

  delay(1000);

  // Initialize audio
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(internetVolume);

    
  // Initialize FM radio
  radio.setup();
  radio.setVolume(fmVolume);
  radio.setFrequency(currentFMFreq);
  radio.setMute(false);
  radio.setSeekThreshold(70);           // Sets RSSI Seek Threshold (0 to 127)

  // I2S Setup
  radio.setI2SOn(true);
  radio.setI2SMaster(true);
  radio.setI2SSpeed(I2S_WS_STEP_44_1);
  radio.setI2SDataSigned(true);
  Serial.println("FM Radio initialized");
  
  // Start with internet radio if WiFi connected
  if (wifiConnected) {
    switchToInternetRadio();
  } else {
    switchToFMRadio();
  }
  
  currentSystemMode = NORMAL_MODE;
  
  updateDisplay();
}

float n=0;

void loop() {
  if (currentSystemMode == CONFIG_MODE) {
    dnsServer.processNextRequest();
    server.handleClient();
    
    // Check for config mode timeout
    if (millis() - configModeTimer > configModeTimeout) {
      Serial.println("Config mode timeout, restarting...");
      ESP.restart();
    }
    
    // Check for exit config mode
    btnPlay.tick();
    delay(10);
    return;
  }
  
  // Normal operation mode
  handleNormalMode();
}

void handleNormalMode() {
  // Handle button presses
  btnPlay.tick();
  btnPrev.tick();
  btnNext.tick();
  btnVolUp.tick();
  btnVolDown.tick();
    
  // Handle rotary encoder (FM frequency tuning)
  encoder.tick();
  if (currentRadioMode == FM_RADIO) {
    int newPos = encoder.getPosition();
    static int lastPos = 0;
    
    if (newPos != lastPos) {
      int diff = newPos - lastPos;
      currentFMFreq += (diff * 10); // 0.1 MHz steps
      
      if (currentFMFreq < minFreq) currentFMFreq = minFreq;//radio.setFrequencyUp();
      if (currentFMFreq > maxFreq) currentFMFreq = maxFreq;//radio.setFrequencyDown();
      
      radio.setFrequency(currentFMFreq);
      lastPos = newPos;
      updateDisplay();
    }
  }
  
  // Handle audio streaming for internet radio
  if (currentRadioMode == INTERNET_RADIO && wifiConnected) {
    audio.loop();
  }
  
  // Update NTP time periodically
  if (millis() - lastTimeUpdate > timeUpdateInterval) {
    timeClient.update();
    // updateBatteryLevel();
    lastTimeUpdate = millis();
  }

  // Check WiFi connection
  if (wifiConfigured && WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    if (currentRadioMode == INTERNET_RADIO) {
      switchToFMRadio(); // Auto switch to FM if WiFi lost
    }
  }

  // Update display periodically
  if (millis() - lastDisplayUpdate > displayUpdateInterval) {
    updateDisplay();
    lastDisplayUpdate = millis();
  }
  
  // Update signal strength and battery periodically
  if (millis() - lastSignalUpdate > signalUpdateInterval) {
    updateSignalStrength();
    updateBatteryLevel();
    lastSignalUpdate = millis();
  }
  
  delay(10);
}
  
void enterConfigMode() {
  currentSystemMode = CONFIG_MODE;
  configModeTimer = millis();
  
  Serial.println("Entering configuration mode");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("CONFIG MODE", 10, 10);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("Connect to WiFi:", 10, 40);
  tft.drawString(configSSID, 10, 55);
  tft.drawString("Password: 12345678", 10, 70);
  tft.drawString("Open: 192.168.4.1", 10, 85);
  tft.drawString("Press rotary to exit", 10, 110);
  
  // Start Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP(configSSID, configPassword);
  
  // Start DNS server for captive portal
  dnsServer.start(53, "*", WiFi.softAPIP());
  
  // Setup web server routes
  setupWebServer();
  server.begin();
  
  Serial.print("Config AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupWebServer() {
  // Main configuration page
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getConfigPage());
  });
  
  // WiFi configuration endpoint
  server.on("/wifi", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      wifiSSID = server.arg("ssid");
      wifiPassword = server.arg("password");
      
      preferences.putString("wifiSSID", wifiSSID);
      preferences.putString("wifiPassword", wifiPassword);
      preferences.putBool("wifiConfigured", true);
      
      server.send(200, "text/html; charset=utf-8", 
        "<html><body><h2>WiFi设置已保存!</h2>"
        "<p>SSID: " + wifiSSID + "</p>"
        "<p>设备将在3秒后重启...</p>"
        "<script>setTimeout(function(){window.location.href='/';},3000);</script>"
        "</body></html>");
      
      delay(3000);
      ESP.restart();
    } else {
      server.send(400, "text/html", "<html><body><h2>错误: 缺少参数</h2></body></html>");
    }
  });
  
  // Station list upload endpoint
  server.on("/upload", HTTP_POST, 
    []() { server.send(200, "text/plain", ""); },
    handleFileUpload
  );
  
  // Get current stations as JSON
  server.on("/stations", HTTP_GET, []() {
    String json = "[";
    for (size_t i = 0; i < internetStations.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"name\":\"" + internetStations[i].name + "\",\"url\":\"" + internetStations[i].url + "\"}";
    }
    json += "]";
    server.send(200, "application/json", json);
  });
  
  // Delete station endpoint
  server.on("/delete", HTTP_POST, []() {
    if (server.hasArg("index")) {
      int index = server.arg("index").toInt();
      if (index >= 0 && index < internetStations.size()) {
        internetStations.erase(internetStations.begin() + index);
        saveStationList();
        server.send(200, "text/plain", "电台已删除");
      } else {
        server.send(400, "text/plain", "无效索引");
      }
    } else {
      server.send(400, "text/plain", "缺少索引参数");
    }
  });
  
  // Captive portal redirect
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });
}

void handleFileUpload() {
  HTTPUpload& upload = server.upload();
  static File uploadFile;
  
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Upload Start: %s\n", upload.filename.c_str());
    uploadFile = SPIFFS.open("/stations.csv", "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("Upload End: %s (%u bytes)\n", upload.filename.c_str(), upload.totalSize);
      
      // Parse the uploaded CSV
      if (parseStationCSV()) {
        server.send(200, "text/html", 
          "<html><body><h2>电台列表已更新!</h2>"
          "<p>Loaded " + String(internetStations.size()) + " stations</p>"
          "<p><a href='/'>返回主页</a></p>"
          "</body></html>");
      } else {
        server.send(400, "text/html", 
          "<html><body><h2>解析CSV文件时出错</h2>"
          "<p><a href='/'>返回主页e</a></p>"
          "</body></html>");
      }
    }
  }
}

bool parseStationCSV() {
  File file = SPIFFS.open("/stations.csv", "r");
  if (!file) {
    Serial.println("Failed to open stations.csv");
    return false;
  }
  
  internetStations.clear();
  
  String line;
  bool isFirstLine = true;
  
  while (file.available()) {
    line = file.readStringUntil('\n');
    line.trim();
    
    // Skip header line
    if (isFirstLine) {
      isFirstLine = false;
      continue;
    }
    
    if (line.length() == 0) continue;
    
    // Parse CSV line (expecting: name,url)
    int commaIndex = line.indexOf(',');
    if (commaIndex > 0 && commaIndex < line.length() - 1) {
      String name = line.substring(0, commaIndex);
      String url = line.substring(commaIndex + 1);
      
      // Remove quotes if present
      name.replace("\"", "");
      url.replace("\"", "");
      
      if (name.length() > 0 && url.length() > 0) {
        internetStations.push_back({name, url});
        Serial.println("Added station: " + name + " -> " + url);
      }
    }
  }
  
  file.close();
  
  if (internetStations.size() == 0) {
    // Load default stations if CSV parsing failed
    loadDefaultStations();
    return false;
  }
  
  saveStationList();
  return true;
}

void loadDefaultStations() {
  internetStations.clear();
  int numDefault = sizeof(defaultStations) / sizeof(defaultStations[0]);
  for (int i = 0; i < numDefault; i++) {
    internetStations.push_back({defaultStations[i].name, defaultStations[i].url});
  }
}

void saveStationList() {
  File file = SPIFFS.open("/stations.json", "w");
  if (!file) {
    Serial.println("Failed to save station list");
    return;
  }
  
  DynamicJsonDocument doc(8192);
  JsonArray stations = doc.createNestedArray("stations");
  
  for (const auto& station : internetStations) {
    JsonObject stationObj = stations.createNestedObject();
    stationObj["name"] = station.name;
    stationObj["url"] = station.url;
  }
  
  serializeJson(doc, file);
  file.close();
  Serial.println("Station list saved");
}

void loadStationList() {
  File file = SPIFFS.open("/stations.json", "r");
  if (!file) {
    Serial.println("No saved station list, loading defaults");
    loadDefaultStations();
    return;
  }
  
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.println("Failed to parse station list, loading defaults");
    loadDefaultStations();
    return;
  }
  
  internetStations.clear();
  JsonArray stations = doc["stations"];
  
  for (JsonObject station : stations) {
    internetStations.push_back({
      station["name"].as<String>(),
      station["url"].as<String>()
    });
  }
  
  if (internetStations.size() == 0) {
    loadDefaultStations();
  }
  
  Serial.println("Loaded " + String(internetStations.size()) + " stations");
}

String getConfigPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
    <title>ESP32 duRadio配置页面</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; margin: 20px; background: #f0f0f0; }
        .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }
        h1 { color: #333; text-align: center; }
        .section { margin: 20px 0; padding: 15px; border: 1px solid #ddd; border-radius: 5px; }
        input[type="text"], input[type="password"] { width: 100%; padding: 8px; margin: 5px 0; }
        button { background: #007cba; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; }
        button:hover { background: #005a8b; }
        .station-list { max-height: 300px; overflow-y: auto; }
        .station-item { padding: 5px; border-bottom: 1px solid #eee; display: flex; justify-content: space-between; align-items: center; }
        .delete-btn { background: #dc3545; padding: 5px 10px; font-size: 12px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>ESP32 duRadio 配置</h1>
        
        <div class="section">
            <h3>WiFi配置</h3>
            <form action="/wifi" method="post">
                <label>WiFi SSID:</label>
                <input type="text" name="ssid" required>
                <label>WiFi Password:</label>
                <input type="password" name="password" required>
                <button type="submit">保存WiFi设置</button>
            </form>
        </div>
        
        <div class="section">
            <h3>上传电台列表</h3>
            <p>上传CSV文件格式: 电台名,流媒体地址</p>
            <form action="/upload" method="post" enctype="multipart/form-data">
                <input type="file" name="file" accept=".csv" required>
                <button type="submit">上传CSV文件</button>
            </form>
        </div>
        
        <div class="section">
            <h3>当前电台</h3>
            <div class="station-list" id="stationList">
                Loading stations...
            </div>
            <button onclick="refreshStations()">刷新列表</button>
        </div>
        
        <div class="section">
            <h3>CSV文件格式示例</h3>
            <pre>name,url
BBC World Service,http://stream.live.vc.bbcmedia.co.uk/bbc_world_service
NPR News,http://npr-ice.streamguys1.com/live.mp3</pre>
        </div>
    </div>
    
    <script>
        function refreshStations() {
            fetch('/stations')
                .then(response => response.json())
                .then(stations => {
                    const list = document.getElementById('stationList');
                    list.innerHTML = '';
                    stations.forEach((station, index) => {
                        const div = document.createElement('div');
                        div.className = 'station-item';
                        div.innerHTML = `
                            <span><strong>${station.name}</strong><br><small>${station.url}</small></span>
                            <button class="delete-btn" onclick="deleteStation(${index})">删除</button>
                        `;
                        list.appendChild(div);
                    });
                })
                .catch(err => {
                    document.getElementById('stationList').innerHTML = '电台读取错误';
                });
        }
        
        function deleteStation(index) {
            if (confirm('删除此电台?')) {
                fetch('/delete', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                    body: 'index=' + index
                })
                .then(() => refreshStations())
                .catch(err => alert('电台删除错误'));
            }
        }
        
        // Load stations on page load
        refreshStations();
    </script>
</body>
</html>
  )rawliteral";
  return html;
}

void connectToWiFi() {
  tft.drawString("Connecting WiFi...", 10, 55);
  Serial.println("Connecting to WiFi: " + wifiSSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    tft.drawString("WiFi Connected!", 10, 70);
  } else {
    wifiConnected = false;
    Serial.println("\nWiFi connection failed");
    tft.drawString("WiFi Failed!", 10, 70);
  }
}

void setupButtons() {
  btnPlay.attachClick(onPlayPause);
  btnPrev.attachClick(onPrevious);
  btnNext.attachClick(onNext);
  btnVolUp.attachClick(onVolumeUp);
  btnVolDown.attachClick(onVolumeDown);
    
  // Long press for mode switch on play button
  btnPlay.attachLongPressStart(onModeSwitch);

  // Long press for config mode
  // btnPlay.attachLongPressStart(onConfigMode);
  // btnRotary.attachLongPressStart(onConfigMode);
}

void onConfigMode() {
  if (currentSystemMode == CONFIG_MODE) {
    // Exit config mode
    ESP.restart();
  } else {
    // Enter config mode
    enterConfigMode();
  }
}


void onPlayPause() {
  if (currentRadioMode == INTERNET_RADIO) {
    if (wifiConnected) {
      if (isPlaying) {
        audio.stopSong();
        isPlaying = false;
      } else {
        playCurrentInternetStation();
      }
    }
  } else {
    // FM Radio mute/unmute
    fmMuted = !fmMuted;
    radio.setMute(fmMuted);
  }
  updateDisplay();
}

void onPrevious() {
  if (currentRadioMode == INTERNET_RADIO && wifiConnected) {
    currentInternetStation--;
    if (currentInternetStation < 0) {
      currentInternetStation = internetStations.size() - 1;
    }
    playCurrentInternetStation();
  } else {
    // FM: seek down
    radio.seek(RDA_SEEK_WRAP,RDA_SEEK_DOWN, showFrequency);
  }
  updateDisplay();
}

void onNext() {
  if (currentRadioMode == INTERNET_RADIO && wifiConnected) {
    currentInternetStation++;
    if (currentInternetStation >= internetStations.size()) {
      currentInternetStation = 0;
    }
    playCurrentInternetStation();
  } else {
    // FM: seek up
    radio.seek(RDA_SEEK_WRAP, RDA_SEEK_UP, showFrequency);
  }
  updateDisplay();
}

void onVolumeUp() {
  if (currentRadioMode == INTERNET_RADIO) {
    if (internetVolume < 21) {
      internetVolume++;
      audio.setVolume(internetVolume);
    }
  } else {
    if (fmVolume < 15) {
      fmVolume++;
      radio.setVolume(fmVolume);
    }
  }
  updateDisplay();
}

void onVolumeDown() {
  if (currentRadioMode == INTERNET_RADIO) {
    if (internetVolume > 0) {
      internetVolume--;
      audio.setVolume(internetVolume);
    }
  } else {
    if (fmVolume > 0) {
      fmVolume--;
      radio.setVolume(fmVolume);
    }
  }
  updateDisplay();
}

void onModeSwitch() {
  if (currentSystemMode == CONFIG_MODE) return;
  
  if (currentRadioMode == INTERNET_RADIO) {
    switchToFMRadio();
  } else {
    if (wifiConnected) {
      switchToInternetRadio();
    }
  }
  saveSettings();
  updateDisplay();
}

void switchToInternetRadio() {
  if (!wifiConnected) return;
  currentRadioMode = INTERNET_RADIO;
  digitalWrite(AUDIO_SEL, LOW); // Switch audio mux to internet
  radio.setMute(true); // Mute FM
  playCurrentInternetStation();
}

void switchToFMRadio() {
  currentRadioMode = FM_RADIO;
  audio.stopSong(); // Stop internet stream
  isPlaying = false;
  digitalWrite(AUDIO_SEL, HIGH); // Switch audio mux to FM
  radio.setMute(fmMuted); // Restore FM mute state
}

void playCurrentInternetStation() {
  if (wifiConnected && currentInternetStation < internetStations.size()) {
    audio.connecttohost(internetStations[currentInternetStation].url.c_str());
    isPlaying = true;
  }
}

void showFrequency() {
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextSize(1);
  spr.setTextDatum(MR_DATUM);
  currentFMFreq = radio.getFrequency();
  char freqStr[10];  
  sprintf(freqStr, "%.1f", currentFMFreq / 100.0);
  spr.drawString(freqStr, 205, 85, 7);
  // spr.drawFloat(freqStr, 1, 5, 60, 7);
  spr.setTextSize(1);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("MHz", 210, 95);  
}

void updateDisplay() {
  if (currentSystemMode == CONFIG_MODE) return;
  spr.fillSprite(TFT_BLACK);
  
  // Title and status bar
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.setTextSize(2);
  // spr.drawString("ESP32 Radio", 5, 5);
  
  // WiFi and Battery status (top right)
  drawStatusBar();
  
  // Mode indicator
  spr.setTextColor(TFT_YELLOW, TFT_BLACK);
  spr.setTextSize(2);
  
    
  if (currentRadioMode == INTERNET_RADIO) {
    spr.loadFont(ZdyLwFont_20);
    spr.drawString("网络电台", 225, 120);
    spr.pushImage(240,70,48,48,NET);//Internet_radio logo
    

    if (wifiConnected && internetStations.size() > 0) {
      // Station info
      spr.setTextColor(TFT_CYAN, TFT_BLACK);
      spr.setTextSize(1);
      
      // spr.drawString("Station:", 10, 70);
      String stationName = internetStations[currentInternetStation].name;
      if (stationName.length() > 35) {
        stationName = stationName.substring(0, 32) + "...";
      }
      spr.setTextDatum(MC_DATUM);
      spr.drawString(stationName, (tft.width())/2, 40);
    
    // Status
    spr.setTextDatum(TL_DATUM);
    // spr.drawString("Status:", 5, 95);
    spr.setTextColor(isPlaying ? TFT_GREEN : TFT_RED, TFT_BLACK);
    spr.drawString(isPlaying ? "播放中" : "暂停", 5, 80);
    spr.unloadFont();//unloadfonts to free memory
    
    // Volume
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    char volStr[20];
    sprintf(volStr, "Volume: %d/21", internetVolume);
    spr.drawString(volStr, 5, 125);

    // Station count
    sprintf(volStr, "Station: %d/%d", currentInternetStation + 1, (int)internetStations.size());
    spr.drawString(volStr, 5, 110);

    //draw musci spectrum
    drawSpectrum();
    
    
    } else {
      spr.setTextColor(TFT_RED, TFT_BLACK);
      spr.setTextSize(1);
      spr.drawString("No WiFi or Stations", 10, 70);
    }
    
  } else {
    spr.loadFont(ZdyLwFont_20);
    // spr.setTextDatum(TC_DATUM); // Set the datum to the middle right of the text
    spr.drawString("FM收音机", 220, 120);
    spr.pushImage(240,70,49,48,FM);//FM_radio logo    

    // Status
    spr.setTextSize(1);
    // spr.drawString("Status:", 5, 95);
    spr.setTextColor(fmMuted ? TFT_RED : TFT_GREEN, TFT_BLACK);
    spr.drawString(fmMuted ? "静音" : "播放中", 5, 80);
    spr.unloadFont();//unloadfonts to free memory
    // Volume
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    char volStr[20];
    sprintf(volStr, "Volume: %d/15", fmVolume);
    spr.drawString(volStr, 5, 110);
        
    // Frequency (large display)
    showFrequency();
    // FM Signal strength (RSSI)
    drawFMSignalStrength(5, 125);
  }
  
  // WiFi status

  spr.setTextColor(TFT_BLUE, TFT_BLACK);
  spr.drawString("WiFi:", 220, 150);
  spr.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED, TFT_BLACK);
  spr.drawString(wifiConnected ? "Connected" : "Disconnected", 260, 150);

  // Controls help
  // spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  // spr.drawString("Long Press Play: Config Mode", 5, 160);

  // spr.pushImage(5,5,24,24,speaker);//speaker logo

  spr.pushSprite(0,0);
}

void saveSettings() {
  preferences.putInt("radioMode", currentRadioMode);
  preferences.putInt("station", currentInternetStation);
  preferences.putInt("fmFreq", currentFMFreq);
  preferences.putInt("fmVol", fmVolume);
  preferences.putInt("inetVol", internetVolume);
}

void loadSettings() {
  wifiSSID = preferences.getString("wifiSSID", "");
  wifiPassword = preferences.getString("wifiPassword", "");
  wifiConfigured = preferences.getBool("wifiConfigured", false);
  
  currentRadioMode = (RadioMode)preferences.getInt("radioMode", INTERNET_RADIO);
  currentInternetStation = preferences.getInt("station", 0);
  currentFMFreq = preferences.getInt("fmFreq", 10110);
  fmVolume = preferences.getInt("fmVol", 8);
  internetVolume = preferences.getInt("inetVol", 15);
}

// Audio event handlers
void audio_info(const char *info) {
  Serial.print("info        "); Serial.println(info);
}

void audio_id3data(const char *info) {
  Serial.print("id3data     "); Serial.println(info);
}

void audio_eof_mp3(const char *info) {
  Serial.print("eof_mp3     "); Serial.println(info);
}

void audio_showstation(const char *info) {
  Serial.print("station     "); Serial.println(info);
}

void audio_showstreamtitle(const char *info) {
  Serial.print("streamtitle "); Serial.println(info);
}

void audio_bitrate(const char *info) {
  Serial.print("bitrate     "); Serial.println(info);
}

void audio_commercial(const char *info) {
  Serial.print("commercial  "); Serial.println(info);
}

void audio_icyurl(const char *info) {
  Serial.print("icyurl      "); Serial.println(info);
}

void audio_lasthost(const char *info) {
  Serial.print("lasthost    "); Serial.println(info);
}

// Signal strength and battery monitoring functions
void updateSignalStrength() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiRSSI = WiFi.RSSI();
  } else {
    wifiRSSI = -100; // No signal
  }
}

void updateBatteryLevel() {
  // Read ADC value and convert to voltage
  int adcValue = analogRead(BATTERY_PIN);
  // delay(200);//读数为0问题
  // Serial.print("adcValue=");
  // Serial.println(adcValue);
  batteryVoltage = (adcValue* ADC_REFERENCE_VOLTAGE * VOLTAGE_DIVIDER_RATIO  / ADC_RESOLUTION);
  // Serial.print("batteryvoltage=");
  // Serial.println(batteryVoltage);
  // Convert voltage to percentage
  batteryPercent = map(batteryVoltage * 100, BATTERY_MIN_VOLTAGE * 100, BATTERY_MAX_VOLTAGE * 100, 0, 100);
  batteryPercent = constrain(batteryPercent, 0, 100);
}

void drawStatusBar() {
  //Draw status bar background
  spr.fillRect(0, 0, 320, 25, (currentRadioMode == INTERNET_RADIO) ? TFT_NAVY : TFT_PURPLE);

  // Draw volume icon
  drawVolumeIcon();

  // Draw time (center)
  drawTime();
    
  // Battery indicator (top right)
  drawBatteryIcon();
    
  // WiFi signal strength indicator
  drawWiFiSignalIndicator();

  // Draw separator line
  // spr.drawLine(0, 25, 320, 25, TFT_DARKGREY);
}

void drawVolumeIcon() {
  int x = 5;
  int y = 5;
  int volume = (currentRadioMode == INTERNET_RADIO) ? internetVolume : fmVolume;
  int maxVol = (currentRadioMode == INTERNET_RADIO) ? 21 : 15;
  
  // Draw speaker icon
  spr.fillRect(x, y + 4, 4, 8, TFT_WHITE);
  spr.fillRect(x + 4, y + 2, 4, 12, TFT_WHITE);
  
  // Draw volume bars
  int volumeBars = map(volume, 0, maxVol, 0, 3);
  for (int i = 0; i < 3; i++) {
    uint16_t color = (i < volumeBars) ? TFT_CYAN : TFT_DARKGREY;
    spr.fillRect(x + 7 + i * 2, y + 7 - i, 1, 2 + i * 2, color);
  }
  
  // Show mute indicator
  if ((currentRadioMode == FM_RADIO && fmMuted) || volume == 0) {
    spr.drawLine(x - 1, y + 1, x + 12, y + 12, TFT_RED);
  }
}


// Draw time
void drawTime() {
  spr.setTextColor(TFT_WHITE);
  spr.setTextSize(1);
  spr.setTextDatum(TC_DATUM); // Centre text on x,y position
  String timeStr = timeClient.getFormattedTime();
  
  // Calculate center position
  int textWidth = timeStr.length() * 6; // Approximate width
  int x = (320 - textWidth) / 2;
  
  spr.drawString(timeStr, x, 2, 4);
}


// Draw battery icon
void drawBatteryIcon() {
  int battX = 270;
  int battY = 5;
  
  // Battery outline
  spr.drawRect(battX, battY, 35, 15, TFT_WHITE);
  spr.drawRect(battX + 35, battY + 4, 3, 7, TFT_WHITE);
  
  // Battery fill based on percentage
  int fillWidth = map(batteryPercent, 0, 100, 0, 33);
  uint16_t battColor = TFT_GREEN;
  if (batteryPercent < 20) battColor = TFT_RED;
  else if (batteryPercent < 50) battColor = TFT_YELLOW;
  
  if (fillWidth > 0) {
    spr.fillRect(battX + 1, battY + 1, fillWidth, 13, battColor);
  }
  
  // Battery percentage text
  spr.setTextColor(TFT_WHITE);
  spr.setTextSize(1);
  spr.setTextDatum(TL_DATUM);
  char battStr[10];
  sprintf(battStr, "%d%%", batteryPercent);
  spr.drawString(battStr, battX - 25, battY + 4);
} 

// Draw wifi signal
void drawWiFiSignalIndicator() {
  int x = 220;
  int y = 5;
  // WiFi signal strength bars (4 bars)
  int barWidth = 3;
  int barSpacing = 2;
  int maxHeight = 15;
  
  // Determine signal strength level (1-4 bars)
  int signalBars = 0;
  if (wifiRSSI > -50) signalBars = 4;      // Excellent
  else if (wifiRSSI > -60) signalBars = 3; // Good
  else if (wifiRSSI > -70) signalBars = 2; // Fair
  else if (wifiRSSI > -80) signalBars = 1; // Poor
  
  uint16_t signalColor = TFT_GREEN;
  if (signalBars <= 1) signalColor = TFT_RED;
  else if (signalBars <= 2) signalColor = TFT_YELLOW;
  
  // Draw 4 bars with increasing height
  for (int i = 0; i < 4; i++) {
    int barHeight = ((i + 1) * maxHeight) / 4;
    int barX = x + i * (barWidth + barSpacing);
    int barY = y + maxHeight - barHeight;
    
    uint16_t color = (i < signalBars) ? signalColor : TFT_DARKGREY;
    spr.fillRect(barX, barY, barWidth, barHeight, color);
  }
}

// Draw FM signal strength
void drawFMSignalStrength(int x, int y) {
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextSize(1);
  
  // Get RSSI from FM radio
  int fmRssi = 0;
  if (radio.getRssi()) {
    fmRssi = radio.getRssi();
    char rssiStr[25];
    sprintf(rssiStr, "RSSI: %d dBuV", fmRssi);
    spr.drawString(rssiStr, x+2, y);
    
    // Signal quality bar
    int barWidth = 100;
    int barHeight = 8;
    
    // RSSI typically ranges from 10-75 dBuV for FM
    int signalPercent = map(fmRssi, 10, 75, 0, 100);
    signalPercent = constrain(signalPercent, 0, 100);
    
    // Bar outline
    spr.drawRect(x, y + 15, barWidth, barHeight, TFT_WHITE);
    
    // Fill bar based on signal strength
    int fillWidth = map(signalPercent, 0, 100, 0, barWidth - 2);
    uint16_t signalColor = TFT_GREEN;
    if (signalPercent < 30) signalColor = TFT_RED;
    else if (signalPercent < 60) signalColor = TFT_YELLOW;
    
    if (fillWidth > 0) {
      spr.fillRect(x + 1, y + 16, fillWidth, barHeight - 2, signalColor);
    }
    
    // Signal quality text
    char qualityStr[15];
    if (signalPercent >= 80) strcpy(qualityStr, "Excellent");
    else if (signalPercent >= 60) strcpy(qualityStr, "Good");
    else if (signalPercent >= 40) strcpy(qualityStr, "Fair");
    else if (signalPercent >= 20) strcpy(qualityStr, "Poor");
    else strcpy(qualityStr, "Weak");
    
    spr.drawString(qualityStr, x + barWidth + 5, y);
  } else {
    spr.drawString("FM Signal: Searching...", x, y);
  }
}

// draw music bar Spectrum 
void drawSpectrum(){


ani.fillSprite(TFT_BLACK);
 
 if (isPlaying == 1) {
  ani.pushImage(0, 0,  animation_width, animation_height, frame[int(n)]);
  n=n+0.05  ;
  if(int(n)==frames)
  n=0; }
  else
  {ani.pushImage(0, 0,  animation_width, animation_height, frame[frames-1]);}
  
ani.pushToSprite(&spr, 140, 70);
  // bar.deleteSprite();
}