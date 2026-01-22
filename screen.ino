#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <time.h>
#include "colours.h"

// wifi: insert your wifi login here
const char* ssid = "network name";        
const char* password = "network password"; 

// opendata api: insert your key here
const char* apiKey = "key";
const char* stopID = "202210";  // stop id (currently bondi junction)

// oled pins
#define OLED_SCK   18
#define OLED_MOSI  23
#define OLED_CS    15
#define OLED_DC    4
#define OLED_RST   5

// button pin
#define BUTTON_PIN 13  // GPIO13 on ESP32-WROVER-CAM

// oled display
Adafruit_SSD1351 oled = Adafruit_SSD1351(128, 128, OLED_CS, OLED_DC, OLED_MOSI, OLED_SCK, OLED_RST);

// time zone adjustment (ust --> aedt)
const int UTC_OFFSET_HOURS = 11;

// button debounce and mode
unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;
int displayMode = 0;
const char* modeNames[] = {"ALL MODES", "RAIL", "BUSES", "LIGHT RAIL", "FERRY"};

// departure types
#define TYPE_ALL 0
#define TYPE_RAIL 1
#define TYPE_BUS 2
#define TYPE_LR 3
#define TYPE_FERRY 4

// structure to hold departure info
struct Departure {
  String destination;
  String platform;
  String time;
  String route;
  int minutesUntil;
  int delayMinutes;
  int type;
  bool valid;
};

Departure allDepartures[30];
int totalCount = 0;
unsigned long lastFetchTime = 0;
const unsigned long FETCH_INTERVAL = 10000;  // refreshes every 10 seconds
String currentStationName = "";  // Store station name from API

// station name lookup
String getStationName(const char* stopID) {
  // Use API-provided name if we have it
  if (currentStationName.length() > 0) {
    return currentStationName;
  }
  
  // Fallback to stop ID
  return "#" + String(stopID);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== DEPARTURES ===");
  
  // initialise button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // initialise oled
  oled.begin();
  oled.fillScreen(BLACK);
  oled.setTextWrap(false);
  
  // startup
  oled.setCursor(10, 50);
  oled.setTextColor(CYAN);
  oled.setTextSize(1);
  oled.println("Starting...");
  
  // connect to wifi
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
    
    // set time from ntp and offset acordingly
    configTime(UTC_OFFSET_HOURS * 3600, 0, "pool.ntp.org", "time.nist.gov");
    
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
  // detect button press
  checkButton();
  
  // fetch departures at fixed interval
  unsigned long now = millis();
  if (now - lastFetchTime > FETCH_INTERVAL) {
    getDepartures();
    lastFetchTime = now;
    displayDepartures();
  }
  
  delay(50);
}

void checkButton() {
  if (digitalRead(BUTTON_PIN) == LOW) {  // button pressed
    unsigned long now = millis();
    if (now - lastButtonPress > DEBOUNCE_DELAY) {
      lastButtonPress = now;
      displayMode = (displayMode + 1) % 5;  // 5 modes
      Serial.println("Mode switched to: " + String(modeNames[displayMode]));
      displayDepartures();
    }
  }
}

void getDepartures() {
  HTTPClient http;
  
  // api call for departures at assigned stop id
  String url = "https://api.transport.nsw.gov.au/v1/tp/departure_mon";
  url += "?outputFormat=rapidJSON";
  url += "&coordOutputFormat=EPSG%3A4326";
  url += "&mode=direct";
  url += "&type_dm=stop";
  url += "&name_dm=" + String(stopID);
  url += "&departureMonitorMacro=true";
  url += "&TfNSWDM=true";
  url += "&version=10.2.1.42";
  
  Serial.println("\n=== Fetching Departures ===");
  Serial.println("Stop ID: " + String(stopID));
  
  http.begin(url);
  http.setTimeout(15000);
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
  
  Serial.println("Payload size: " + String(payload.length()) + " bytes");
  
  // parse json
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.println("JSON Parse Error: " + String(error.c_str()));
    Serial.println("Error code: " + String(error.code()));
    showError("Parse Error");
    return;
  }
  
  // Extract station name from locations if available
  if (doc["locations"].is<JsonArray>()) {
    JsonArray locations = doc["locations"].as<JsonArray>();
    if (locations.size() > 0 && locations[0]["disassembledName"].is<const char*>()) {
      String stationNameFromAPI = locations[0]["disassembledName"].as<String>();
      stationNameFromAPI.replace(" Station", "");
      stationNameFromAPI.toUpperCase();
      currentStationName = stationNameFromAPI;
      Serial.println("Station name from API: " + currentStationName);
    }
  }
  
  // check for stopEvents
  if (!doc["stopEvents"].is<JsonArray>()) {
    Serial.println("No stopEvents found in response");
    
    // Display station name even with no departures
    if (currentStationName.length() > 0) {
      oled.fillScreen(BLACK);
      oled.setCursor(2, 3);
      oled.setTextColor(WHITE);
      oled.setTextSize(1);
      oled.print(currentStationName);
      oled.setCursor(15, 60);
      oled.setTextColor(YELLOW);
      oled.println("No departures");
    } else {
      showError("No departures");
    }
    
    totalCount = 0;
    return;
  }
  
  JsonArray stopEvents = doc["stopEvents"].as<JsonArray>();
  Serial.println("Found " + String(stopEvents.size()) + " events");
  
  // get current time for calculating minutes
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  int currentHour = timeinfo->tm_hour;
  int currentMin = timeinfo->tm_min;
  int currentTotalMin = currentHour * 60 + currentMin;
  
  Serial.println("Current time: " + String(currentHour) + ":" + String(currentMin));
  
  // extract departures into array
  totalCount = 0;
  
  for (JsonVariant eventVar : stopEvents) {
    if (totalCount >= 30) break;
    
    JsonObject event = eventVar.as<JsonObject>();
    Departure dep;
    dep.valid = false;
    dep.route = "";
    dep.platform = "";
    dep.type = TYPE_BUS;
    
    // determine transport type using API product class
    if (event["transportation"].is<JsonObject>()) {
      JsonObject transport = event["transportation"].as<JsonObject>();
      
      // Try to get product class first (most reliable)
      if (transport["product"].is<JsonObject>()) {
        JsonObject product = transport["product"].as<JsonObject>();
        if (product["class"].is<int>()) {
          int productClass = product["class"].as<int>();
          // TfNSW product classes: 1=Train, 5=Bus, 4=Ferry, 7=Light Rail
          if (productClass == 1) dep.type = TYPE_RAIL;
          else if (productClass == 5) dep.type = TYPE_BUS;
          else if (productClass == 4) dep.type = TYPE_FERRY;
          else if (productClass == 7) dep.type = TYPE_LR;
        }
      }
      
      // Fallback to transport mode name if product class not available
      if (dep.type == TYPE_BUS && transport["properties"]["DM_TransportType"].is<const char*>()) {
        String transType = transport["properties"]["DM_TransportType"].as<String>();
        transType.toLowerCase();
        if (transType.indexOf("train") != -1 || transType.indexOf("metro") != -1) dep.type = TYPE_RAIL;
        else if (transType.indexOf("bus") != -1) dep.type = TYPE_BUS;
        else if (transType.indexOf("ferry") != -1) dep.type = TYPE_FERRY;
        else if (transType.indexOf("lightrail") != -1 || transType.indexOf("light rail") != -1) dep.type = TYPE_LR;
      }
      
      // destination
      if (transport["destination"]["name"].is<const char*>()) {
        dep.destination = transport["destination"]["name"].as<String>();
        dep.destination.replace(" Station", "");
        dep.destination.replace(" Wharf", "");
        dep.destination.trim();
      }
      
      // route name/number
      if (transport["disassembledName"].is<const char*>()) {
        dep.route = transport["disassembledName"].as<String>();
      } else if (transport["number"].is<const char*>()) {
        dep.route = transport["number"].as<String>();
      }
      
      // platform
      if (transport["properties"]["PlatformName"].is<const char*>()) {
        dep.platform = transport["properties"]["PlatformName"].as<String>();
      } else if (event["platformName"].is<const char*>()) {
        dep.platform = event["platformName"].as<String>();
      } else if (transport["platform"].is<const char*>()) {
        dep.platform = transport["platform"].as<String>();
      }
      
      // clean platform - extract only digits
      if (dep.platform.length() > 0) {
        String cleanPlatform = "";
        for (unsigned int i = 0; i < dep.platform.length(); i++) {
          if (isdigit(dep.platform[i])) {
            cleanPlatform += dep.platform[i];
          }
        }
        if (cleanPlatform.length() > 0) {
          dep.platform = cleanPlatform;
        }
      }
    }
    
    // get departure times
    String timeEstimated = "";
    String timePlanned = "";
    
    if (event["departureTimeEstimated"].is<const char*>()) {
      timeEstimated = event["departureTimeEstimated"].as<String>();
    }
    if (event["departureTimePlanned"].is<const char*>()) {
      timePlanned = event["departureTimePlanned"].as<String>();
    }
    
    // use estimated if available, otherwise timetable
    String timeStr = timeEstimated.length() > 0 ? timeEstimated : timePlanned;
    
    if (timeStr.length() >= 16) {
      // parse time
      int hour = timeStr.substring(11, 13).toInt();
      int minute = timeStr.substring(14, 16).toInt();
      
      // convert time zones
      hour = (hour + UTC_OFFSET_HOURS) % 24;
      
      // time formatting
      char timeBuffer[6];
      sprintf(timeBuffer, "%02d:%02d", hour, minute);
      dep.time = String(timeBuffer);
      
      // calculate minutes until departure
      int depTotalMin = hour * 60 + minute;
      dep.minutesUntil = depTotalMin - currentTotalMin;
      if (dep.minutesUntil < 0) {
        dep.minutesUntil += 1440;  // next day
      }
      
      // calculate delay (estimated vs planned)
      dep.delayMinutes = 0;
      if (timeEstimated.length() >= 16 && timePlanned.length() >= 16) {
        int estHour = timeEstimated.substring(11, 13).toInt();
        int estMin = timeEstimated.substring(14, 16).toInt();
        int planHour = timePlanned.substring(11, 13).toInt();
        int planMin = timePlanned.substring(14, 16).toInt();
        
        int estTotal = estHour * 60 + estMin;
        int planTotal = planHour * 60 + planMin;
        dep.delayMinutes = estTotal - planTotal;
      }
      
      dep.valid = true;
    }
    
    // Only add departures within next 2 hours
    if (dep.valid && dep.destination.length() > 0 && dep.minutesUntil >= 0 && dep.minutesUntil <= 120) {
      allDepartures[totalCount] = dep;
      totalCount++;
      
      String typeStr = (dep.type == TYPE_RAIL) ? "RAIL" : (dep.type == TYPE_BUS) ? "BUS" : (dep.type == TYPE_LR) ? "LR" : (dep.type == TYPE_FERRY) ? "FERRY" : "OTHER";
      Serial.println(typeStr + ": " + dep.route + " to " + dep.destination + " @ " + dep.time + " (in " + dep.minutesUntil + " min) P" + dep.platform);
    }
  }
  
  // sort by minutes until departure
  for (int i = 0; i < totalCount - 1; i++) {
    for (int j = i + 1; j < totalCount; j++) {
      if (allDepartures[j].minutesUntil < allDepartures[i].minutesUntil) {
        Departure temp = allDepartures[i];
        allDepartures[i] = allDepartures[j];
        allDepartures[j] = temp;
      }
    }
  }
  
  Serial.println("Total parsed (within 2 hours): " + String(totalCount));
}

void showError(String msg) {
  oled.fillScreen(BLACK);
  oled.setCursor(10, 50);
  oled.setTextColor(RED);
  oled.setTextSize(1);
  oled.println(msg);
}

void displayDepartures() {
  // filter departures based on mode
  Departure filtered[30];
  int filteredCount = 0;
  
  for (int i = 0; i < totalCount; i++) {
    if (displayMode == TYPE_ALL) {
      filtered[filteredCount++] = allDepartures[i];
    } else if (displayMode == TYPE_RAIL && allDepartures[i].type == TYPE_RAIL) {
      filtered[filteredCount++] = allDepartures[i];
    } else if (displayMode == TYPE_BUS && allDepartures[i].type == TYPE_BUS) {
      filtered[filteredCount++] = allDepartures[i];
    } else if (displayMode == TYPE_LR && allDepartures[i].type == TYPE_LR) {
      filtered[filteredCount++] = allDepartures[i];
    } else if (displayMode == TYPE_FERRY && allDepartures[i].type == TYPE_FERRY) {
      filtered[filteredCount++] = allDepartures[i];
    }
  }
  
  // display on screen
  oled.fillScreen(BLACK);
  
  // header with station name and mode
  oled.fillRect(0, 0, 128, 14, BLACK);
  oled.setCursor(2, 3);
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  String stationName = getStationName(stopID);
  oled.print(stationName);
  oled.print(" ");
  
  // color mode text based on transport type
  int modeColor = WHITE;
  if (displayMode == TYPE_RAIL) modeColor = CLST;
  else if (displayMode == TYPE_BUS) modeColor = CLBUS;
  else if (displayMode == TYPE_LR) modeColor = CLLR;
  else if (displayMode == TYPE_FERRY) modeColor = CLFERRY;
  
  oled.setTextColor(modeColor);
  oled.println(modeNames[displayMode]);
  
  // underline
  oled.drawFastHLine(0, 13, 128, WHITE);
  
  if (filteredCount == 0) {
    oled.setCursor(15, 60);
    oled.setTextColor(YELLOW);
    oled.setTextSize(1);
    oled.println("No departures");
    return;
  }
  
  // show departures
  int yPos = 16;
  int displayCount = min(filteredCount, 4);
  
  for (int i = 0; i < displayCount; i++) {
    Departure dep = filtered[i];
    
    String dest = dep.destination;
    if (dest.length() > 18) {
      dest = dest.substring(0, 18);
    }
    
    // determine line color based on route
    int lineColor = WHITE;
    if (dep.type == TYPE_RAIL) {
      if (dep.route == "T1") lineColor = CLT1;
      else if (dep.route == "T2") lineColor = CLT2;
      else if (dep.route == "T3") lineColor = CLT3;
      else if (dep.route == "T4") lineColor = CLT4;
      else if (dep.route == "T5") lineColor = CLT5;
      else if (dep.route == "T6") lineColor = CLT6;
      else if (dep.route == "T7") lineColor = CLT7;
      else if (dep.route == "T8") lineColor = CLT8;
      else if (dep.route == "T9") lineColor = CLT9;
      else if (dep.route == "M1") lineColor = CLMETRO;
      else lineColor = CLST;
    } else if (dep.type == TYPE_BUS) {
      lineColor = CLBUS;
    } else if (dep.type == TYPE_LR) {
      if (dep.route == "L1") lineColor = CLL1;
      else if (dep.route == "L2") lineColor = CLL2;
      else if (dep.route == "L3") lineColor = CLL3;
      else if (dep.route == "L4") lineColor = CLL4;
      else lineColor = CLLR;
    } else if (dep.type == TYPE_FERRY) {
      if (dep.route == "F1") lineColor = CLF1;
      else if (dep.route == "F2") lineColor = CLF2;
      else if (dep.route == "F3") lineColor = CLF3;
      else if (dep.route == "F4") lineColor = CLF4;
      else if (dep.route == "F5") lineColor = CLF5;
      else if (dep.route == "F6") lineColor = CLF6;
      else if (dep.route == "F7") lineColor = CLF7;
      else if (dep.route == "F8") lineColor = CLF8;
      else if (dep.route == "F9") lineColor = CLF9;
      else if (dep.route == "F10") lineColor = CLSF;
      else if (dep.route == "SCO") lineColor = CLT4;
      else if (dep.route == "CCN") lineColor = CLT9;
      else if (dep.route == "SHL") lineColor = CLT8;
      else if (dep.route == "BMT") lineColor = CLT1;
      else lineColor = CLFERRY;
    }
    
    // route number and destination in line color
    oled.setCursor(2, yPos);
    oled.setTextColor(lineColor);
    oled.setTextSize(1);
    if (dep.route.length() > 0) {
      oled.print(dep.route);
      oled.print(" ");
    }
    oled.println(dest);
    
    // time in white
    oled.setCursor(2, yPos + 9);
    oled.setTextColor(WHITE);

    if (dep.minutesUntil == 0) {
      oled.print("Now");
    } else if (dep.minutesUntil >= 60) {
      int hours = dep.minutesUntil / 60;
      int mins = dep.minutesUntil % 60;
      oled.print(hours);
      oled.print("h");
      if (mins > 0) {
        oled.print(" ");
        oled.print(mins);
        oled.print("m");
      }
    } else {
      oled.print(dep.minutesUntil);
      oled.print("m");
    }
    
    // delay in colored brackets
    if (dep.delayMinutes != 0) {
      int delayColor = ONTIME;
      if (dep.delayMinutes < 0) delayColor = EARLY;
      else if (dep.delayMinutes > 0 && dep.delayMinutes <= 2) delayColor = MINOR;
      else if (dep.delayMinutes > 2) delayColor = MAJOR;
      
      oled.setTextColor(delayColor);
      oled.print(" (");
      if (dep.delayMinutes > 0) {
        oled.print("+");
      }
      oled.print(dep.delayMinutes);
      oled.print(")");
    }
    
    // platform
    if (dep.platform.length() > 0) {
      oled.setTextColor(WHITE);
      oled.print("  P");
      oled.print(dep.platform);
    } else {
      oled.setTextColor(MAJOR);
      oled.print("  P?");
    }
    oled.println();
    
    // separator
    oled.drawFastHLine(0, yPos + 20, 128, DARK_GRAY);
    
    yPos += 22;
  }
  
  Serial.println("Displayed " + String(displayCount) + " departures in " + String(modeNames[displayMode]) + " mode");
}