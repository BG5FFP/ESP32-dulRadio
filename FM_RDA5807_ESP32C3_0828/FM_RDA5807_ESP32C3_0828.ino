#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <RDA5807.h>
#include <RotaryEncoder.h>
#include <OneButton.h>
#include <FastLED.h>
#include <time.h>
#include "ICON.h"
#include "ZdyLwFont_20.h"
#include "NotoSansMonoSCB20.h"

// Pin definitions
#define ENCODER_PIN_A     10
#define ENCODER_PIN_B     6
#define ENCODER_BUTTON    9
#define RGB_LED_PIN       8
#define BATTERY_PIN       3
#define I2C_SDA           18
#define I2C_SCL           19

//LCD
// #define TFT_MOSI          0
// #define TFT_SCK           1
// #define TFT_CS            7
// #define TFT_RST           -1
// #define TFT_DC            2
// #define TFT_BL            5

// LED configuration
#define NUM_LEDS          8
#define LED_BRIGHTNESS    50

// Radio configuration
#define MIN_FREQUENCY     8700  // 87.0 MHz
#define MAX_FREQUENCY     10800 // 108.0 MHz
#define STEP_FREQUENCY    10    // 0.1 MHz
#define MIN_VOLUME        0
#define MAX_VOLUME        15

// UI States
enum UIState {
  STATE_FREQUENCY,
  STATE_VOLUME,
  STATE_SEEK,
  STATE_STATION
};

// Global objects
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
RDA5807 radio;
RotaryEncoder encoder(ENCODER_PIN_A, ENCODER_PIN_B, RotaryEncoder::LatchMode::FOUR3);
OneButton button(ENCODER_BUTTON, true);
CRGB leds[NUM_LEDS];
Preferences prefs;
WebServer server(80);

// Global variables
UIState currentState = STATE_FREQUENCY;
uint16_t currentFreq = 10130; // 101.3 MHz
uint8_t currentVolume = 8;
bool isMuted = false;
bool wifiConnected = false;
bool apMode = false;
String ssid = "";
String password = "";
unsigned long lastUpdate = 0;
unsigned long stateTimeout = 0;
int encoderPos = 0;
float batteryVoltage = 0.0;
uint16_t rssi = 0;

// Station presets structure
struct Station {
  uint16_t frequency;  // in 0.1MHz units
  String name;
};

Station stations[] = {
  {9410, "绍兴交通广播"},
  {9680, "浙江音乐调频"}, 
  {9960, "浙江民生"},
  {9180, "杭州交通918电台"},
  {9500, "浙江经济广播"},
  {9300, "浙江交通之声"},
  {10450, "浙江女主播电台"},
  {10540, "杭州西湖之声"}
};

const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);
int currentStation = 5; // Start with 101.3 FM

// Time variables
struct tm timeinfo;
bool timeValid = false;

// Function prototypes
void initDisplay();
void initRadio();
void initWiFi();
void initNTP();
void initWebServer();
void saveSettings();
void loadSettings();
void saveWiFiCredentials();
void loadWiFiCredentials();
void saveStations();
void loadStations();
void updateDisplay();
void updateLEDs();
void handleEncoder();
void handleButton();
void onButtonClick();
void onButtonDoubleClick();
void onButtonLongPress();
void seekStation(bool up);
void setFrequency(uint16_t freq);
void setVolume(uint8_t vol);
void toggleMute();
void readBattery();
void updateTime();
String getStationName(uint16_t frequency);
void handleWebServer();
void handleRoot();
void handleConfig();
void handleSaveConfig();
void handleGetStatus();
void handleNotFound();
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
// Add to your code for debugging

void check_stack() {
    // For the current task
    UBaseType_t free_stack = uxTaskGetStackHighWaterMark(NULL);
    printf("Free stack: %u bytes\n", free_stack * sizeof(StackType_t));
    
    // Alternatively, for the main loop task
    TaskHandle_t loopTask = xTaskGetHandle("loopTask");
    if (loopTask != NULL) {
        free_stack = uxTaskGetStackHighWaterMark(loopTask);
        printf("Loop task free stack: %u bytes\n", free_stack * sizeof(StackType_t));
    }
}
void setup() {
  Serial.begin(115200);
  check_stack();
  
  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  
  // Initialize preferences
  prefs.begin("fm_radio", false);
  loadWiFiCredentials();
  loadStations();
  loadSettings();
  
  // Initialize display
  initDisplay();
  
  // Initialize FastLED
  FastLED.addLeds<WS2812, RGB_LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  
  // Initialize radio
  initRadio();
  
  // Initialize WiFi and NTP
  initWiFi();
  if (wifiConnected) {
    initNTP();
    initWebServer();
  }
  
  // Setup button callbacks
  button.attachClick(onButtonClick);
  button.attachDoubleClick(onButtonDoubleClick);
  button.attachLongPressStart(onButtonLongPress);
  
  // Initial display update
  updateDisplay();
  updateLEDs();
  
  Serial.println("FM Radio initialized!");
}

void loop() {
  // Update encoder
  encoder.tick();
  handleEncoder();
  
  // Update button
  button.tick();
  
  // Update display every 100ms
  if (millis() - lastUpdate > 100) {
    readBattery();
    rssi = radio.getRssi();
    updateDisplay();
    updateLEDs();
    lastUpdate = millis();
  }
  
  // Update time every minute
  static unsigned long lastTimeUpdate = 0;
  if (millis() - lastTimeUpdate > 60000) {
    updateTime();
    lastTimeUpdate = millis();
  }
  
  // Handle web server
  if (wifiConnected || apMode) {
    server.handleClient();
  }
  
  // Return to frequency mode after timeout
  if (stateTimeout > 0 && millis() > stateTimeout) {
    currentState = STATE_FREQUENCY;
    stateTimeout = 0;
  }
  
  delay(10);
}

void initDisplay() {
  tft.init();
  tft.setRotation(3); // Landscape mode
  tft.fillScreen(TFT_BLACK);
  spr.setSwapBytes(true);
  spr.createSprite(320, 170);//crate sprite
  
  // Turn on backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  
  // Show startup screen
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawCentreString("FM RADIO", 160, 60, 4);  
  tft.setTextSize(1);
  tft.drawCentreString("RDA5807 + ESP32-C3", 160, 100, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString("by BG5FFP, 2025", 160, 115, 2);
  delay(2000);
  tft.fillScreen(TFT_BLACK);
}

void initRadio() {
  radio.setup();
  radio.setFrequency(currentFreq);
  radio.setVolume(currentVolume);
  radio.setMute(isMuted);
  radio.setMono(false);
  radio.setBass(true);
  radio.setSeekThreshold(50);           // Sets RSSI Seek Threshold (0 to 127)
}

void initWiFi() {
  // Load WiFi credentials
  loadWiFiCredentials();
  
  if (ssid.length() > 0) {
    // Try to connect to saved WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    unsigned long timeout = millis() + 10000; // 10 second timeout
    while (WiFi.status() != WL_CONNECTED && millis() < timeout) {
      delay(500);
      Serial.printf("WiFi connection failed");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.println("WiFi connected!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      return;
    }
  }
  
  // If connection failed or no credentials, start AP mode
  Serial.println("Starting AP mode for configuration...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FM_Radio_Config", "12345678");
  apMode = true;
  
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  
  // Initialize web server in AP mode
  initWebServer();
}

void initNTP() {
  configTime(8*3600, 0, "pool.ntp.org", "time.nist.gov");
  updateTime();
}

void updateTime() {
  if (wifiConnected && getLocalTime(&timeinfo)) {
    timeValid = true;
  }
}

void saveSettings() {
  prefs.putUShort("frequency", currentFreq);
  prefs.putUChar("volume", currentVolume);
  prefs.putBool("muted", isMuted);
  prefs.putInt("station", currentStation);
}

void loadSettings() {
  currentFreq = prefs.getUShort("frequency", 10130);
  currentVolume = prefs.getUChar("volume", 8);
  isMuted = prefs.getBool("muted", false);
  currentStation = prefs.getInt("station", 5);
}

void saveWiFiCredentials() {
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
}

void loadWiFiCredentials() {
  ssid = prefs.getString("ssid", "");
  password = prefs.getString("password", "");
}

void saveStations() {
  // Save number of stations
  prefs.putInt("numStations", NUM_STATIONS);
  
  // Save each station
  for (int i = 0; i < NUM_STATIONS; i++) {
    String freqKey = "freq_" + String(i);
    String nameKey = "name_" + String(i);
    prefs.putUShort(freqKey.c_str(), stations[i].frequency);
    prefs.putString(nameKey.c_str(), stations[i].name);
  }
}

void loadStations() {
  int savedStations = prefs.getInt("numStations", 0);
  
  // Only load if we have saved stations, otherwise keep defaults
  if (savedStations > 0 && savedStations <= 16) { // Reasonable limit
    for (int i = 0; i < savedStations && i < NUM_STATIONS; i++) {
      String freqKey = "freq_" + String(i);
      String nameKey = "name_" + String(i);
      stations[i].frequency = prefs.getUShort(freqKey.c_str(), stations[i].frequency);
      stations[i].name = prefs.getString(nameKey.c_str(), stations[i].name);
    }
  }
}

void updateDisplay() {
  // Clear display
  spr.fillSprite(TFT_BLACK);   

  spr.fillRectVGradient(80, 25, 230, 2, TFT_NAVY, TFT_SKYBLUE);//station name
  spr.pushImage(280, 30, 32, 32, ICON_RADIO);

  spr.drawRect(80, 61, 100, 35, TFT_DARKGREY);//mode
  spr.drawRect(81, 62, 98, 33, TFT_DARKGREY);

  spr.drawRect(185, 61, 125, 35, TFT_DARKGREY);//rssi
  spr.drawRect(186, 62, 123, 33, TFT_DARKGREY);
  
  // spr.drawRect(80, 101, 170, 54, TFT_DARKGREY);//freq
  // spr.drawRect(81, 102, 168, 52, TFT_DARKGREY);
  for(int i=0; i<=3; i++){
    spr.drawFastHLine(83, 105+15*i, 160, TFT_DARKGREY);
  }
  for(int i=0; i<=4; i++){
    spr.drawFastVLine(93+36*i, 100, 56, TFT_DARKGREY);
  }

  spr.drawRect(10, 100, 48, 54, TFT_DARKGREY);//vol
  spr.drawRect(11, 101, 46, 52, TFT_DARKGREY);
  spr.fillRect(10, 92, 48, 5, TFT_NAVY);

  // spr.fillRect(255, 101, 50, 16, TFT_RED);//Mhz
  spr.setTextColor(TFT_WHITE, TFT_RED);
  spr.drawCentreString("EEPROM", 280, 101, 2);
  spr.setTextColor(TFT_BLUE, TFT_BLACK);
  spr.loadFont(NotoSansMonoSCB20);
  spr.drawCentreString("MHZ", 280, 120, 4);
  spr.unloadFont();

  spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  spr.drawString("FM radio", 10, 0, 4);//Radio

  spr.fillRect(65, 60, 7, 95, TFT_DARKGREY);

  drawBattery(240,5);
  drawTime(160, 0);
  drawWifi(34, 75);  
  drawRssi(190, 78);
  drawStereo(280, 145);

  showModestate();
  showFrequency();
  showVolumelevel();
  showStationname();  

  spr.pushSprite(0, 0);
  Serial.println("DRAW UI DONE!");
}

void updateLEDs() {
  FastLED.clear();
  
  switch (currentState) {
    case STATE_FREQUENCY: {
      // Show frequency as position on LED ring
      int freqPos = map(currentFreq, MIN_FREQUENCY, MAX_FREQUENCY, 0, NUM_LEDS);
      freqPos = constrain(freqPos, 0, NUM_LEDS - 1);
      leds[freqPos] = CRGB::Blue;
      
      // Show adjacent LEDs dimmed
      if (freqPos > 0) leds[freqPos - 1] = CRGB::Blue; leds[freqPos - 1].fadeToBlackBy(150);
      if (freqPos < NUM_LEDS - 1) leds[freqPos + 1] = CRGB::Blue; leds[freqPos + 1].fadeToBlackBy(150);
      break;
    }
    
    case STATE_VOLUME: {
      // Show volume level
      int volLeds = map(currentVolume, 0, MAX_VOLUME, 0, NUM_LEDS);
      for (int i = 0; i < volLeds; i++) {
        leds[i] = CRGB::Green;
      }
      if (isMuted) {
        fill_solid(leds, NUM_LEDS, CRGB::Red);
        for (int i = 0; i < NUM_LEDS; i += 2) {
          leds[i] = CRGB::Black;
        }
      }
      break;
    }
    
    case STATE_SEEK: {
      // Spinning animation during seek
      static uint8_t seekPos = 0;
      leds[seekPos] = CRGB::Yellow;
      leds[(seekPos + 4) % NUM_LEDS] = CRGB::Orange;
      seekPos = (seekPos + 1) % NUM_LEDS;
      break;
    }
    
    case STATE_STATION: {
      // Show current station number
      int stationLed = currentStation % NUM_LEDS;
      leds[stationLed] = CRGB::Purple;
      break;
    }
  }
  
  FastLED.show();
}

//Update display when encoder or key changes
void handleEncoder() {
  encoder.tick();
  int newPos = encoder.getPosition();
  
  if (newPos != encoderPos) {
    int delta = newPos - encoderPos;
    encoderPos = newPos;
    
    switch (currentState) {
      case STATE_FREQUENCY:
        currentFreq += (delta * STEP_FREQUENCY);
        currentFreq = constrain(currentFreq, MIN_FREQUENCY, MAX_FREQUENCY);
        setFrequency(currentFreq);
        break;
        
      case STATE_VOLUME:
        if(isMuted){
          toggleMute();
        }
        currentVolume += delta;
        currentVolume = constrain(currentVolume, MIN_VOLUME, MAX_VOLUME);
        setVolume(currentVolume);
        stateTimeout = millis() + 3000; // Return to frequency after 3 seconds
        break;
        
      case STATE_STATION:
        currentStation += delta;
        currentStation = constrain(currentStation, 0, NUM_STATIONS - 1);
        currentFreq = stations[currentStation].frequency;
        setFrequency(currentFreq);
        stateTimeout = millis() + 3000;
        break;
    }
    
    saveSettings();
  }
}

void onButtonClick() {
  // Cycle through states
  switch (currentState) {
    case STATE_FREQUENCY:
      currentState = STATE_VOLUME;
      stateTimeout = millis() + 5000;
      break;
    case STATE_VOLUME:
      currentState = STATE_STATION;
      stateTimeout = millis() + 5000;
      break;
    case STATE_STATION:
      currentState = STATE_FREQUENCY;
      stateTimeout = 0;
      break;
  }
  //update mode state
  showModestate();
}

void onButtonDoubleClick() {
  toggleMute();
}

void onButtonLongPress() {
  // Start seek up
  currentState = STATE_SEEK;
  seekStation(true);
  currentState = STATE_FREQUENCY;  
}

void seekStation(bool up) {
  // Use library's built-in seek function
  radio.seek(RDA_SEEK_WRAP, up ? RDA_SEEK_UP : RDA_SEEK_DOWN, 
             NULL); // NULL callback means blocking seek
  
  // Get the new frequency after seek completes
  currentFreq = radio.getFrequency();
  saveSettings();
}

void setFrequency(uint16_t freq) {
  currentFreq = freq;
  radio.setFrequency(freq);
}

void setVolume(uint8_t vol) {
  currentVolume = vol;
  radio.setVolume(vol);
}

void toggleMute() {
  isMuted = !isMuted;
  radio.setMute(isMuted);
  saveSettings();
}

void readBattery() {
  // Read battery voltage (assuming voltage divider)
  int adcReading = analogRead(BATTERY_PIN);
  batteryVoltage = (adcReading * 3.3 * 2) / 4095.0; // Assuming 2:1 voltage divider
}

// Main frequency display
void showFrequency() {
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  float freqMHz = currentFreq / 100.0;
  spr.drawRightString(String(freqMHz, 1), 228, 105, 7);

}
  
// Display station name if in station mode or frequecny close to preset station
void showStationname() {   
  // Station name display (for preset stations or station mode)
  spr.loadFont(ZdyLwFont_20);
  String stationName = getStationName(currentFreq);
  if (stationName != "" || currentState == STATE_STATION) {
    spr.setTextColor(TFT_YELLOW, TFT_BLACK);
    String displayName = (currentState == STATE_STATION) ? stations[currentStation].name : stationName;
    spr.drawString(displayName, 92, 37, 4);
  }
  spr.unloadFont();
}
  
// Volume display
void showVolumelevel() {
  
  if(!isMuted){   
    spr.setTextColor(TFT_BLUE, TFT_BLACK);
    spr.drawCentreString(String(currentVolume), 33, 106, 4);
    spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    spr.loadFont(NotoSansMonoSCB20);  
    spr.drawCentreString("VOL", 35, 134, 2);
    spr.unloadFont();
  }else 
  spr.pushImage(20, 110, 32, 32, ICON_MUTE);
  } 
  
// State indicator
void showModestate() {  
  spr.setTextColor(TFT_MAGENTA, TFT_BLACK);
  spr.loadFont(ZdyLwFont_20);
  String stateStr = "";
  switch (currentState) {
    case STATE_FREQUENCY: 
      stateStr = "频率"; 
      spr.fillRect(200, 159, 28, 6, TFT_SKYBLUE); //freq indicator
      break;
    case STATE_VOLUME: 
      stateStr = "音量"; 
      spr.fillRect(18, 159, 28, 6, TFT_SKYBLUE); //vol indicator
      break;
    case STATE_SEEK: 
      stateStr = "搜台";       
      spr.pushImage(84, 63, 32, 32, ICON_SEEK);//TEST
      break;
    case STATE_STATION: 
      stateStr = "预设"; 
      spr.fillRect(80, 30, 7, 28, TFT_SKYBLUE); //station indicator
      break;
  }
  spr.drawCentreString(stateStr, 135, 72, 4);
  spr.unloadFont();
}

String getStationName(uint16_t frequency) {
  // Check if current frequency is close to any preset station (within ±0.1 MHz)
  for (int i = 0; i < NUM_STATIONS; i++) {
    if (abs((int)frequency - (int)stations[i].frequency) <= 1) { // Within ±0.1 MHz (1 unit)
      return stations[i].name;
    }
  }
  return ""; // No matching station found
}

// Battery indicator
void drawBattery(int x, int y){
  // Top bar - Battery and WiFi
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextSize(1);
  
  int battPercent = constrain(map(batteryVoltage * 100, 320, 420, 0, 100), 0, 100);
  spr.drawString(String(battPercent) + "%", x, y, 2);

  // Draw battery icon
  spr.drawRect(x+40, y+2, 20, 12, TFT_WHITE);
  spr.drawRect(x+60, y+5, 3, 6, TFT_WHITE);
  int fillWidth = map(battPercent, 0, 100, 0, 18);
  if (battPercent > 20) {
    spr.fillRect(x+41, y+3, fillWidth, 10, TFT_GREEN);
    } else {
    spr.fillRect(x+41, y+3, fillWidth, 10, TFT_RED);
    }                                                                                                                  
  
}
  
// WiFi icon and IP
void drawWifi(int x, int y){
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  if (wifiConnected) {
  spr.setTextColor(TFT_GREEN, TFT_BLACK);
  spr.drawCentreString("WiFi", x, y, 2);
  spr.pushImage(10, 26, 48, 48, ICON_WIFI);

  } else if (apMode) {
    spr.setTextColor(TFT_ORANGE, TFT_BLACK);
    spr.drawCentreString("AP", x, y, 2);
    spr.pushImage(10, 26, 48, 48, ICON_AP);

    //WiFi connection Help info
    // tft.drawString("Connect phone/computer to FM_Radio_Config network ", 10, 15, 2);
    // tft.drawString("password: 12345678", 10, 25, 2);
    }

}
  
  
// RSSI
void drawRssi(int x, int y){
  spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  spr.drawString("RSSI:     " + String(rssi), 200, 63, 2);
    
  // Rssi bar
  if (!isMuted) {
    int barWidth = map(rssi, 0, 64, 0, 100);
    // spr.drawRect(x, y, 102, 15, TFT_WHITE);
    spr.fillRect(x, y, 116, 14, TFT_NAVY);
    spr.fillRect(x+2, y+2, barWidth, 10, TFT_SKYBLUE);
  }    

}

// Center - Time (if available)
void drawTime(int x, int y){
  if (timeValid) {
    spr.setTextColor(TFT_CYAN, TFT_BLACK);
    char timeStr[10];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    spr.drawCentreString(timeStr, x, y, 4);
  }
  // Current date (bottom)
  if (timeValid) {
    spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char dateStr[20];
    strftime(dateStr, sizeof(dateStr), "%b, %Y", &timeinfo);
    spr.drawCentreString(dateStr, 100, 154, 2);
  }
}
  
//Stereo or MONO indicator
void drawStereo(int x, int y){
  spr.loadFont(NotoSansMonoSCB20); 
  spr.setTextColor(TFT_WHITE, TFT_NAVY);
  String ST = radio.isStereo()? "stereo" : "mono";
  spr.drawCentreString(ST, x, y, 4 );
  spr.unloadFont();

}

void initWebServer() {
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/status", handleGetStatus);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("Web server started");
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>FM Radio Configuration</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta charset="UTF-8">
  <style>
    body { font-family: Arial; margin: 20px; background: #f0f0f0; }
    .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }
    h1 { color: #333; text-align: center; }
    .section { margin: 20px 0; padding: 15px; border: 1px solid #ddd; border-radius: 5px; }
    input, select { width: 100%; padding: 8px; margin: 5px 0; border: 1px solid #ccc; border-radius: 3px; }
    button { background: #007bff; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; margin: 5px; }
    button:hover { background: #0056b3; }
    .status { background: #e8f5e8; padding: 10px; border-radius: 5px; margin: 10px 0; }
    .station-row { display: flex; gap: 10px; align-items: center; margin: 5px 0; }
    .freq-input { width: 100px; }
    .name-input { flex: 1; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🎵 FM Radio Configuration</h1>
    
    <div class="status">
      <h3>Current Status</h3>
      <div id="status">Loading...</div>
    </div>
    
    <div class="section">
      <h3>📶 WiFi Settings</h3>
      <form id="wifiForm">
        <label>Network Name (SSID):</label>
        <input type="text" id="ssid" name="ssid" placeholder="Enter WiFi network name">
        
        <label>Password:</label>
        <input type="password" id="password" name="password" placeholder="Enter WiFi password">
        
        <button type="button" onclick="saveWiFi()">Save WiFi Settings</button>
      </form>
    </div>
    
    <div class="section">
      <h3>📻 Station Presets</h3>
      <div id="stations"></div>
      <button type="button" onclick="saveStations()">Save Station Presets</button>
    </div>
    
    <div class="section">
      <h3>⚡ Actions</h3>
      <button type="button" onclick="restart()">Restart Device</button>
      <button type="button" onclick="location.reload()">Refresh Page</button>
    </div>
  </div>

  <script>
    function loadStatus() {
      fetch('/status')
        .then(response => response.json())
        .then(data => {
          document.getElementById('status').innerHTML = `
            <strong>Frequency:</strong> ${(data.frequency/100).toFixed(1)} MHz<br>
            <strong>Volume:</strong> ${data.volume}/15 ${data.muted ? '(MUTED)' : ''}<br>
            <strong>WiFi:</strong> ${data.wifiConnected ? 'Connected' : 'Disconnected'}<br>
            <strong>IP Address:</strong> ${data.ipAddress}<br>
            <strong>Battery:</strong> ${data.battery}%<br>
            <strong>RSSI:</strong> ${data.rssi}
          `;
          
          document.getElementById('ssid').value = data.ssid;
          
          let stationsHtml = '';
          for (let i = 0; i < data.stations.length; i++) {
            stationsHtml += `
              <div class="station-row">
                <label>Station ${i+1}:</label>
                <input type="number" class="freq-input" id="freq_${i}" 
                       value="${(data.stations[i].frequency/100).toFixed(1)}" 
                       min="87.0" max="108.0" step="0.1">
                <span>MHz</span>
                <input type="text" class="name-input" id="name_${i}" 
                       value="${data.stations[i].name}" placeholder="Station Name">
              </div>
            `;
          }
          document.getElementById('stations').innerHTML = stationsHtml;
        })
        .catch(error => {
          document.getElementById('status').innerHTML = 'Error loading status';
        });
    }
    
    function saveWiFi() {
      const ssid = document.getElementById('ssid').value;
      const password = document.getElementById('password').value;
      
      fetch('/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'wifi', ssid: ssid, password: password })
      })
      .then(response => response.json())
      .then(data => {
        alert(data.message);
        if (data.success) {
          setTimeout(() => location.reload(), 2000);
        }
      });
    }
    
    function saveStations() {
      const stations = [];
      for (let i = 0; i < 8; i++) {
        const freqElement = document.getElementById('freq_' + i);
        const nameElement = document.getElementById('name_' + i);
        if (freqElement && nameElement) {
          stations.push({
            frequency: Math.round(parseFloat(freqElement.value) * 100),
            name: nameElement.value
          });
        }
      }
      
      fetch('/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'stations', stations: stations })
      })
      .then(response => response.json())
      .then(data => {
        alert(data.message);
        loadStatus();
      });
    }
    
    function restart() {
      if (confirm('Are you sure you want to restart the device?')) {
        fetch('/save', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ type: 'restart' })
        });
        alert('Device is restarting...');
      }
    }
    
    // Load status on page load
    loadStatus();
    setInterval(loadStatus, 10000); // Refresh every 10 seconds
  </script>
</body>
</html>
  )rawliteral";
  
  server.send(200, "text/html", html);
}

void handleConfig() {
  handleRoot(); // Redirect to main page
}

void handleSaveConfig() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, body);
    
    String type = doc["type"];
    
    if (type == "wifi") {
      ssid = doc["ssid"].as<String>();
      password = doc["password"].as<String>();
      saveWiFiCredentials();
      
      server.send(200, "application/json", "{\"success\": true, \"message\": \"WiFi settings saved. Restarting...\"}");
      
      delay(1000);
      ESP.restart();
      
    } else if (type == "stations") {
      JsonArray stationsArray = doc["stations"];
      
      for (int i = 0; i < stationsArray.size() && i < NUM_STATIONS; i++) {
        stations[i].frequency = stationsArray[i]["frequency"];
        stations[i].name = stationsArray[i]["name"].as<String>();
      }
      
      saveStations();
      server.send(200, "application/json", "{\"success\": true, \"message\": \"Station presets saved!\"}");
      
    } else if (type == "restart") {
      server.send(200, "application/json", "{\"success\": true, \"message\": \"Restarting device...\"}");
      delay(1000);
      ESP.restart();
    }
  } else {
    server.send(400, "application/json", "{\"success\": false, \"message\": \"Invalid request\"}");
  }
}

void handleGetStatus() {
  DynamicJsonDocument doc(1536);
  
  doc["frequency"] = currentFreq;
  doc["volume"] = currentVolume;
  doc["muted"] = isMuted;
  doc["wifiConnected"] = wifiConnected;
  doc["ipAddress"] = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["ssid"] = ssid;
  doc["battery"] = (int)constrain(map(batteryVoltage * 100, 320, 420, 0, 100), 0, 100);
  doc["rssi"] = rssi;
  
  JsonArray stationsArray = doc.createNestedArray("stations");
  for (int i = 0; i < NUM_STATIONS; i++) {
    JsonObject station = stationsArray.createNestedObject();
    station["frequency"] = stations[i].frequency;
    station["name"] = stations[i].name;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleNotFound() {
  server.send(404, "text/plain", "Page not found");
}

float mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}