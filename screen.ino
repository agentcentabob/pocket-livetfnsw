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
int displayMode = 1;
const char* modeNames[] = {"ALL", "TRAINS", "BUSES", "LR", "FERRY", "OTHER"};

// departure types
#define TYPE_ALL 0
#define TYPE_RAIL 1
#define TYPE_BUS 2
#define TYPE_LR 3
#define TYPE_FERRY 4
#define TYPE_OTHER 5

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

// TO REPLACE WITH API PULL
String getStationName(const char* stopID) {
  if (strcmp(stopID, "207053") == 0) return "BONDI JUNCTION";
  if (strcmp(stopID, "207210") == 0) return "TURRAMURRA";
  if (strcmp(stopID, "207020") == 0) return "CENTRAL";
  if (strcmp(stopID, "207029") == 0) return "REDFERN";
  if (strcmp(stopID, "207037") == 0) return "MAROUBRA";
  if (strcmp(stopID, "207006") == 0) return "TOWN HALL";
  if (strcmp(stopID, "207014") == 0) return "COOGEE";
  if (strcmp(stopID, "207046") == 0) return "CRONULLA";
  if (strcmp(stopID, "207007") == 0) return "WYNYARD";
  if (strcmp(stopID, "207010") == 0) return "EDGECLIFF";
  if (strcmp(stopID, "207011") == 0) return "WATSONS BAY";
  if (strcmp(stopID, "207015") == 0) return "TAMARAMA";
  if (strcmp(stopID, "207016") == 0) return "BRONTE";
  if (strcmp(stopID, "207017") == 0) return "WAVERLEY";
  return "STATION";
}

// TO REPLACE WITH API PULL
int classifyTransport(String route) {
  if (route.length() > 0) {
    char first = route[0];
    if (first == 'T' || first == 't') return TYPE_RAIL;
    if (isdigit(first)) return TYPE_BUS;
  }
  return TYPE_OTHER;
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
      displayMode = (displayMode + 1) % 5;
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
  
  // parse json
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.println("JSON Parse Error");
    showError("Parse Error");
    return;
  }
  
  // extract departures into array
  totalCount = 0;
  
  // check for stopEvents
  if (!doc["stopEvents"].is<JsonArray>()) {
    Serial.println("No stopEvents found");
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
  
  for (JsonVariant eventVar : stopEvents) {
    if (totalCount >= 30) break;
    
    JsonObject event = eventVar.as<JsonObject>();
    Departure dep;
    dep.valid = false;
    dep.route = "";
    dep.platform = "";
    dep.type = TYPE_OTHER;
    
    // determine transport type
    if (event["transportation"].is<JsonObject>()) {
      JsonObject transport = event["transportation"].as<JsonObject>();
      
      // check type (train vs bus)
      if (transport["properties"]["DM_TransportType"].is<const char*>()) {
        String transType = transport["properties"]["DM_TransportType"].as<String>();
        if (transType.indexOf("Train") != -1 || transType.indexOf("train") != -1) {
          dep.type = TYPE_RAIL;
        } else if (transType.indexOf("Bus") != -1 || transType.indexOf("bus") != -1) {
          dep.type = TYPE_BUS;
        }
      }
      
      // desto
      if (transport["destination"]["name"].is<const char*>()) {
        dep.destination = transport["destination"]["name"].as<String>();
        dep.destination.replace(" Station", "");
        dep.destination.trim();
      }
      
      // route time (name/number)
      if (transport["disassembledName"].is<const char*>()) {
        dep.route = transport["disassembledName"].as<String>();
      } else if (transport["number"].is<const char*>()) {
        dep.route = transport["number"].as<String>();
      }
      
      // classify mode by route name
      int routeClassify = classifyTransport(dep.route);
      if (routeClassify != TYPE_OTHER) {
        dep.type = routeClassify;
      }
      
      // platform
      if (transport["properties"]["PlatformName"].is<const char*>()) {
        dep.platform = transport["properties"]["PlatformName"].as<String>();
      } else if (event["platformName"].is<const char*>()) {
        dep.platform = event["platformName"].as<String>();
      } else if (transport["platform"].is<const char*>()) {
        dep.platform = transport["platform"].as<String>();
      }
      
      // clean platform parsing?
      if (dep.platform.length() > 0) {
        String cleanPlatform = "";
        for (int i = 0; i < dep.platform.length(); i++) {
          if (isdigit(dep.platform[i])) {
            cleanPlatform += dep.platform[i];
          }
        }
        if (cleanPlatform.length() > 0) {
          dep.platform = cleanPlatform;
        }
      }
    }
    
    // get departure times, including planned and estimated, to calculate delay
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
    
    if (dep.valid && dep.destination.length() > 0 && dep.minutesUntil >= 0) {
      allDepartures[totalCount] = dep;
      totalCount++;
      
      String typeStr = (dep.type == TYPE_RAIL) ? "TRAIN" : (dep.type == TYPE_BUS) ? "BUS" : "OTHER";
      Serial.println(typeStr + ": " + dep.route + " to " + dep.destination + " @ " + dep.time + " (in " + dep.minutesUntil + " min) Platform " + dep.platform);
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
  
  Serial.println("Total parsed: " + String(totalCount));
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
    if (displayMode == TYPE_RAIL && allDepartures[i].type == TYPE_RAIL) {
      filtered[filteredCount++] = allDepartures[i];
    } else if (displayMode == TYPE_BUS && allDepartures[i].type == TYPE_BUS) {
      filtered[filteredCount++] = allDepartures[i];
    } else if (displayMode == TYPE_ALL) {
      filtered[filteredCount++] = allDepartures[i];
    }
  }
  
  // display on screen
  oled.fillScreen(BLACK);
  
  // header - black background with mode-colored text
  oled.fillRect(0, 0, 128, 14, BLACK);
  oled.setCursor(2, 3);
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  String stationName = getStationName(stopID);
  oled.print(stationName);
  oled.print(" ");
  
  // color mode text based on transport type
  int modeColor = WHITE;
  if (displayMode == TYPE_RAIL) modeColor = CLNSWTL;
  else if (displayMode == TYPE_BUS) modeColor = CLBUS;
  else if (displayMode == TYPE_LR) modeColor = CLLR;
  else if (displayMode == TYPE_FERRY) modeColor = CLFERRY;
  
  oled.setTextColor(modeColor);
  oled.println(modeNames[displayMode]);
  
  // underline instead of blue bar
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
    if (dest.length() > 20) {
      dest = dest.substring(0, 20);
    }
    
    // determine line color based on route
    int lineColor = WHITE;
    if (dep.type == TYPE_RAIL) {
      // rail line colors
      if (dep.route == "T1") lineColor = CLT1;
      else if (dep.route == "T2") lineColor = CLT2;
      else if (dep.route == "T3") lineColor = CLT3;
      else if (dep.route == "T4") lineColor = CLT4;
      else if (dep.route == "T5") lineColor = CLT5;
      else if (dep.route == "T6") lineColor = CLT6;
      else if (dep.route == "T7") lineColor = CLT7;
      else if (dep.route == "T8") lineColor = CLT8;
      else if (dep.route == "T9") lineColor = CLT9;
      else lineColor = CLNSWST;  // default rail color
    } else if (dep.type == TYPE_BUS) {
      lineColor = CLBUS;
    } else if (dep.type == TYPE_LR) {
      lineColor = CLLR;
    } else if (dep.type == TYPE_FERRY) {
      lineColor = CLFERRY;
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