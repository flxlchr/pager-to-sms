#include <WiFi.h>
#include "time.h"
#include "../secrets.h"

// Time server
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;

void printLocalTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  Serial.println(&timeinfo, "%A, %b %d %H:%M:%S");
  Serial.println(timeinfo.tm_wday);
  Serial.println(timeinfo.tm_hour);
  Serial.println(timeinfo.tm_min);
}

void setup()
{
  Serial.begin(115200);

  // connect to WiFi
  Serial.printf("Connecting to %s ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" CONNECTED");

  // init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

void loop()
{
  delay(3000);
  printLocalTime();
}