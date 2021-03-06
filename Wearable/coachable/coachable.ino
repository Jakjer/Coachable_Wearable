/*
 * PROGRAMMER:    William Bicknell
 * FIRST VERSION: Feb 9, 2020
 * DESCRIPTION:   Tracks skiing metrics
 */

#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "SPIFFS.h"
#include "BluetoothSerial.h"
#include <Adafruit_GPS.h>
#include <Adafruit_MPL3115A2.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Metrics.h>
#include <vector>
#include <cmath>

#define GPSSerial Serial2
#define MAX_WIFI_CONNECT_DELAYS 10
#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  10

const String SSID_SEP = "-SSID-";
const String PASSWORD_SEP = "-PWORD-";
const String DEVICE_NAME = "ABC123";
const String WIFI_PATH = "/wifi.txt";
const float MIN_ALT_DIFF = 0.3f;
const int MIN_SPD = 4;
const int LED_PIN = 5;
const int CS_PIN = 33;
const char MESSAGE_END_CHAR = '\0';
const String CLEAR_MSG = "CLEAR_WIFI";
const String SSID_MSG = "SSID|";
const String PASS_MSG = "PASS|";
const int VALID_MSG_LEN = 12;
const uint8_t VALID_MSG[VALID_MSG_LEN] = "INFO_VALID@";
const int INVALID_MSG_LEN = 14;
const uint8_t INVALID_MSG[INVALID_MSG_LEN] = "INFO_INVALID@";

uint32_t timer = millis();
int stopCount = 0;
bool addDataSample = false;
bool waitWifi = false;
bool useSD = false;
int runStartMillis = 0;

float currentAltitude = 0.0f;
float lastAltitude = 0.0f;
float currentSpeed = 0.0f;

Metrics metrics = Metrics();
Adafruit_GPS GPS(&GPSSerial);
Adafruit_MPL3115A2 baro = Adafruit_MPL3115A2();

String btIn = "";
String ssid = "";
String password = "";
BluetoothSerial SerialBT;
int btnPin = 25;

// Initialize
void setup() {
  Serial.begin(115200);
  pinMode(btnPin, INPUT);
  pinMode(LED_PIN, OUTPUT);

  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  int buttonState = digitalRead(btnPin);
  if (buttonState == LOW) {
    Serial.println("Sleeping...");
    Serial.flush();
    esp_deep_sleep_start();
  }
  
  // Init SD
  useSD = SD.begin(CS_PIN, SPI, 1000000, "/sd");
  if (!useSD) {
    Serial.println("Initial SD mount failed!");
  }

  // Init SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS Mount Failed");
  }

  getWifiCredentials();

  metrics.Init(SD, useSD, SPIFFS);

  SerialBT.begin("ESP32");

  // Init GPS
  GPS.begin(9600);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_10HZ);
  GPS.sendCommand(PGCMD_ANTENNA);
  delay(1000);
  GPSSerial.println(PMTK_Q_RELEASE);
  
  delay(3000);
}

// Main loop, runs repeatedly
void loop() {
  // Check for deep sleep
  int buttonState = digitalRead(btnPin);
  if (buttonState == LOW) {
    Serial.println("Sleeping...");
    Serial.flush();
    esp_deep_sleep_start();
  }
  
  // Get new GPS data
  char c = GPS.read();
  if (GPS.newNMEAreceived()) {
    if (!GPS.parse(GPS.lastNMEA())) {
      return;
    }
  }

  if (!metrics.IsRunOngoing()) {
    // Get wifi info
    while (SerialBT.available()) {
      char t = SerialBT.read();
      if (t != MESSAGE_END_CHAR) {
        btIn += t;
      }
      else {
        if (btIn == CLEAR_MSG) {
          Serial.println("Clear wifi info");
          WiFi.disconnect();
          ssid = "";
          password = "";
        }
        else if (btIn.startsWith(SSID_MSG)) {
          ssid = btIn.substring(SSID_MSG.length());
        }
        else if (btIn.startsWith(PASS_MSG)) {
          password = btIn.substring(PASS_MSG.length());
        }
  
        // Send response
        if (ssid != "" && password != "") {
          Serial.println("Test wifi");
          WiFi.begin(ssid.c_str(), password.c_str());
          int count = 0;
          while (WiFi.status() != WL_CONNECTED) {
            if (count == MAX_WIFI_CONNECT_DELAYS) {
              break;
            }
            
            count++;
            delay(2000);
          }
  
          if (WiFi.status() == WL_CONNECTED) {
            SerialBT.write(VALID_MSG, VALID_MSG_LEN);
            saveWifiCredentials(ssid, password);
          }
          else {
            SerialBT.write(INVALID_MSG, INVALID_MSG_LEN);
            getWifiCredentials();
          }
        }
        
        btIn = "";
      }
    }

    if (ssid != "" && password != "") {
      uploadData();
    }
  }
  
  // Catch when millis wraps and reset timer
  if (millis() < timer) {
    timer = millis();
  }

  // Runs every ~0.5 secs
  if (millis() - timer >= 500) {
    timer = millis();
    readBaro();

    if (GPS.fix) {
      // Convert speed from knots to km/h
      currentSpeed = GPS.speed * 1.852f;

      if (!metrics.IsRunOngoing()) {
        if (checkRunStart()) {
          startRun();
        }
      }
      else {
        addDataSamples();

        if (checkRunStop()) {
          stopCount++;

          if (stopCount >= 4) {
            finishRun();
          }
        }
        else {
          stopCount = 0;
        }
      }
    }
    // If run is ongoing and GPS connection is lost, end run
    else if (metrics.IsRunOngoing()) {
      finishRun();
    }

    printMetrics(GPS.fix);
  }
}


// Get altitude from barometer
void readBaro() {
  if (baro.begin()) {
    lastAltitude = currentAltitude;
    currentAltitude = baro.getAltitude();
  }
}


// Send data if there are saved runs
void uploadData() {
  if (!waitWifi && metrics.GetNumSavedRuns() > 0 && WiFi.status() == WL_CONNECTED) {
    delay(2000);
    SerialBT.end();
    delay(5000);
    Serial.println("Sending http request...");
    sendData();
    waitWifi = true;
    SerialBT.begin("ESP32");
    delay(2000);
  }
  else {
    waitWifi = false;
  }
}


// Prints the current metrics
void printMetrics(bool gpsFix) {
  Serial.print("Time: "); Serial.println(getTime());
  Serial.print("Date: "); Serial.println(getDate());
  Serial.print("Altitude(m): "); Serial.println(currentAltitude);

  if (gpsFix) {
    Serial.print("Num Satellites: "); Serial.println((int)GPS.satellites);
    Serial.print("Speed (km/h): "); Serial.println(currentSpeed);
    Serial.print("Speed (knots): "); Serial.println(String(GPS.speed, 5));
    Serial.print("Latitude: "); Serial.println(String(GPS.latitudeDegrees, 7));
    Serial.print("Longitude: "); Serial.println(String(GPS.longitudeDegrees, 7));
  }
  else {
    Serial.println("Waiting for GPS satellites...");
  }

  Serial.println();
}


// Gets date string from GPS in ISO format yyyy-mm-dd
String getDate() {
  String date = "20" + String(GPS.year) + "-";
  if (GPS.month < 10) { date += "0"; }
  date += String(GPS.month) + "-";
  if (GPS.day < 10) { date += "0"; }
  date += String(GPS.day);

  return date;
}


// Gets time string from GPS in 24h format UTC time
String getTime() {
  String time = "";
  if (GPS.hour < 10) { time += "0"; }
  time += String(GPS.hour) + ":";
  if (GPS.minute < 10) { time += "0"; }
  time += String(GPS.minute) + ":";
  if (GPS.seconds < 10) { time += "0"; }
  time += String(GPS.seconds);
  time += " UTC";

  return time;
}


// Starts the run
void startRun() {
  metrics.StartRun(getDate(), getTime(), currentAltitude, GPS.latitudeDegrees, GPS.longitudeDegrees);
  runStartMillis = millis();
  
  digitalWrite(LED_PIN, HIGH);
  Serial.println("--RUN START--");
}


// Ends the run
void finishRun() {
  stopCount = 0;
  
  if (!useSD) {
    metrics.FinishRun(DEVICE_NAME, getTime(), currentAltitude, GPS.latitudeDegrees, GPS.longitudeDegrees, SPIFFS, useSD);
  }
  else {
    metrics.FinishRun(DEVICE_NAME, getTime(), currentAltitude, GPS.latitudeDegrees, GPS.longitudeDegrees, SD, useSD);
  }

  digitalWrite(LED_PIN, LOW);
  Serial.println("--RUN STOP--");
}


// Check if start of run conditions met
bool checkRunStart() {
  // Using speed
  if (currentSpeed >= MIN_SPD && lastAltitude - currentAltitude >= MIN_ALT_DIFF) {
    return true;
  }

  return false;
}


// Check if start of run conditions met
bool checkRunStop() {
  // Using speed
  if (currentSpeed < MIN_SPD) {
    return true;
  }

  return false;
}


// Adds metrics data samples
void addDataSamples() {
  metrics.AddSpeedSample(currentSpeed);
  // Add data sample every other run
  if (addDataSample) {
    metrics.AddDataSample(GPS.latitudeDegrees, GPS.longitudeDegrees, currentSpeed, currentAltitude, (float)(millis() - runStartMillis) / 1000.0f);
    addDataSample = false;
  }
  else {
    addDataSample = true;
  }
}


// Sends the saved JSON data to the API
void sendData() {
  // SD
  if (useSD) {
    int count = 0;
    for (int i = 0; i < metrics.GetNumJsonFiles(true); i++) {
      Serial.println("Sending sd file " + String(count));
      String jsonStr = metrics.GetJsonStr(SD, true, 0);
      if (sendHttp(jsonStr)) {
        metrics.ClearJson(SD, true, 0);
        count++;
      }
      delay(500);
    }
  }

  // SPIFFS
  int count = 0;
  for (int i = 0; i < metrics.GetNumJsonFiles(false); i++) {
    Serial.println("Sending spiffs file " + String(count));
    String jsonStr = metrics.GetJsonStr(SPIFFS, false, 0);
    if (sendHttp(jsonStr)) {
      metrics.ClearJson(SPIFFS, false, 0);
      count++;
    }
    delay(500);
  }
}

// Sends an http request
bool sendHttp(String jsonStr) {
  bool success = false;
  HTTPClient http;
  //http.begin("https://webhook.site/115894b1-47ae-4609-9714-521a604ff2ba");
  http.begin("https://stardustapi.azurewebsites.net/run/");
  http.setUserAgent("Wearable");
  http.addHeader("Content-Type", "application/json");

  int response = http.POST(jsonStr);
  if (response > 0) {
    if (response == HTTP_CODE_OK) {
      success = true;
      Serial.println("SUCCESS");
    }
  }
  else {
    Serial.println("HTTP error: " + http.errorToString(response));
  }
  
  http.end();

  return success;
}

// Save wifi credentials
void saveWifiCredentials(String ssid, String password) {
  File file = SPIFFS.open(WIFI_PATH.c_str(), FILE_WRITE);
  file.println(SSID_SEP.c_str());
  file.println(ssid.c_str());
  file.println(PASSWORD_SEP.c_str());
  file.println(password.c_str());
  file.close();
}

// Get wifi credentials
void getWifiCredentials() {
  String str = "";
  if (SPIFFS.exists(WIFI_PATH.c_str())) {
    File file = SPIFFS.open(WIFI_PATH.c_str());
    while (file.available()) {
      str = file.readStringUntil('\n');
      str.trim();
      
      if (file.available()) {
        if (str.compareTo(SSID_SEP) == 0) {
          ssid = file.readStringUntil('\n');
          ssid.trim();
        }
        else if (str.compareTo(PASSWORD_SEP) == 0) {
          password = file.readStringUntil('\n');
          password.trim();
        }
      }
    }
    file.close();
  }

  if (ssid != "" && password != "") {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
}
