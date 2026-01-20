/*
 * Sydney Trains - Bondi Junction Departures
 * FIXED VERSION
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>

// ===== WiFi Credentials =====
const char* ssid = "ssid";        
const char* password = "password"; 

// ===== Transport NSW API =====
const char* apiKey = "key";

// ===== OLED Pins =====
#define OLED_SCK   18
#define OLED_MOSI  23
#define OLED_CS    15
#define OLED_DC    4
#define OLED_RST   5

// ===== Colors =====
#define BLACK      0x0000
#define BLUE       0x001F
#define RED        0xF800
#define GREEN      0x07E0
#define CYAN       0x07FF
#define MAGENTA    0xF81F
#define YELLOW     0xFFE0
#define WHITE      0xFFFF
#define ORANGE     0xFD20

// ===== OLED Display =====
Adafruit_SSD1351 oled = Adafruit_SSD1351(128, 128, OLED_CS, OLED_DC, OLED_MOSI, OLED_SCK, OLED_RST);

// Structure to hold departure info
struct Departure {
  String destination;
  String platform;
  String time;
  int minutesUntil;
  bool valid;
};

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== BONDI JUNCTION DEPARTURES ===");
  
  // Initialize OLED
  oled.begin();
  oled.fillScreen(BLACK);
  oled.setTextWrap(false);
  
  // Startup
  oled.setCursor(10, 50);
  oled.setTextColor(CYAN);
  oled.setTextSize(1);
  oled.println("Starting...");
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected!");
    Serial.println("IP: " + WiFi.localIP().toString());
    
    oled.fillScreen(BLACK);
    oled.setCursor(20, 50);
    oled.setTextColor(GREEN);
    oled.println("Online!");
    delay(1000);
  } else {
    Serial.println("\nWiFi Failed!");
    oled.fillScreen(BLACK);
    oled.setCursor(5, 50);
    oled.setTextColor(RED);
    oled.println("WiFi Failed!");
    while(1) { delay(1000); }
  }
}

void loop() {
  getDepartures();
  delay(30000);  // Refresh every 30 seconds
}

void getDepartures() {
  HTTPClient http;
  
  // CORRECT API call for Bondi Junction departures
  String url = "https://api.transport.nsw.gov.au/v1/tp/departure_mon";
  url += "?outputFormat=rapidJSON";
  url += "&coordOutputFormat=EPSG%3A4326";
  url += "&mode=direct";
  url += "&type_dm=stop";
  url += "&name_dm=207410";  // BONDI JUNCTION CORRECT STOP ID
  url += "&departureMonitorMacro=true";
  url += "&TfNSWDM=true";
  url += "&version=10.2.1.42";
  
  Serial.println("\n=== Fetching Bondi Junction Departures ===");
  
  http.begin(url);
  http.addHeader("Authorization", String("apikey ") + apiKey);
  
  int httpCode = http.GET();
  
  if (httpCode != 200) {
    Serial.println("HTTP Error: " + String(httpCode));
    showError("Error " + String(httpCode));
    http.end();
    return;
  }
  
  String payload = http.getString();
  http.end();
  
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.println("JSON Parse Error");
    showError("Parse Error");
    return;
  }
  
  // Extract departures into array
  Departure departures[10];
  int depCount = 0;
  
  // Check if we have stopEvents
  if (!doc["stopEvents"].is<JsonArray>()) {
    Serial.println("No stopEvents found");
    showError("No Data");
    return;
  }
  
  JsonArray stopEvents = doc["stopEvents"].as<JsonArray>();
  Serial.println("Found " + String(stopEvents.size()) + " events");
  
  // Get current time for calculating minutes
  unsigned long currentMillis = millis();
  
  for (JsonVariant eventVar : stopEvents) {
    if (depCount >= 10) break;
    
    JsonObject event = eventVar.as<JsonObject>();
    Departure dep;
    dep.valid = false;
    
    // Get transportation info
    if (event["transportation"].is<JsonObject>()) {
      JsonObject transport = event["transportation"].as<JsonObject>();
      
      // Destination
      if (transport["destination"]["name"].is<const char*>()) {
        dep.destination = transport["destination"]["name"].as<String>();
        dep.destination.replace(" Station", "");
      }
      
      // Platform
      if (transport["properties"]["PlatformName"].is<const char*>()) {
        dep.platform = transport["properties"]["PlatformName"].as<String>();
      } else {
        dep.platform = "?";
      }
    }
    
    // Get departure time
    String timeStr = "";
    if (event["departureTimeEstimated"].is<const char*>()) {
      timeStr = event["departureTimeEstimated"].as<String>();
    } else if (event["departureTimePlanned"].is<const char*>()) {
      timeStr = event["departureTimePlanned"].as<String>();
    }
    
    if (timeStr.length() >= 16) {
      dep.time = timeStr.substring(11, 16);  // Extract HH:MM
      dep.valid = true;
      
      // Calculate minutes until departure (rough estimate)
      int hour = timeStr.substring(11, 13).toInt();
      int minute = timeStr.substring(14, 16).toInt();
      // This is simplified - just for sorting
      dep.minutesUntil = hour * 60 + minute;
    }
    
    if (dep.valid && dep.destination.length() > 0) {
      departures[depCount] = dep;
      depCount++;
      
      Serial.println(dep.destination + " @ " + dep.time + " Platform " + dep.platform);
    }
  }
  
  // Sort by time
  for (int i = 0; i < depCount - 1; i++) {
    for (int j = i + 1; j < depCount; j++) {
      if (departures[j].minutesUntil < departures[i].minutesUntil) {
        Departure temp = departures[i];
        departures[i] = departures[j];
        departures[j] = temp;
      }
    }
  }
  
  // Display on screen
  oled.fillScreen(BLACK);
  
  // Header
  oled.fillRect(0, 0, 128, 13, BLUE);
  oled.setCursor(2, 3);
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  oled.println("BONDI JUNCTION");
  
  // Show departures
  int yPos = 16;
  int displayCount = min(depCount, 5);
  
  for (int i = 0; i < displayCount; i++) {
    Departure dep = departures[i];
    
    // Truncate destination if too long
    String dest = dep.destination;
    if (dest.length() > 16) {
      dest = dest.substring(0, 16);
    }
    
    // Line 1: Destination
    oled.setCursor(2, yPos);
    oled.setTextColor(WHITE);
    oled.setTextSize(1);
    oled.println(dest);
    
    // Line 2: Time and Platform
    oled.setCursor(2, yPos + 9);
    oled.setTextColor(CYAN);
    oled.print(dep.time);
    
    oled.setTextColor(YELLOW);
    oled.print("  Plat ");
    oled.setTextColor(GREEN);
    oled.println(dep.platform);
    
    // Separator
    oled.drawFastHLine(0, yPos + 20, 128, BLUE);
    
    yPos += 22;
  }
  
  if (depCount == 0) {
    oled.setCursor(15, 60);
    oled.setTextColor(YELLOW);
    oled.println("No departures");
  }
  
  Serial.println("Displayed " + String(displayCount) + " trains");
}

void showError(String msg) {
  oled.fillScreen(BLACK);
  oled.setCursor(10, 50);
  oled.setTextColor(RED);
  oled.setTextSize(1);
  oled.println(msg);
}