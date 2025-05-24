#include <WiFi.h>
#include <time.h>
#include <twilio.hpp>
#include <Preferences.h> // save data permanently
#include "../secrets.h"  // contains sensitive data like twilio credentials

// Constants
const int MAX_ALARMS_PER_DAY = 6;       // Avoid high SMS costs in case of a bug
const int TIME_DIFFERENCE = 5;          // Avoid resending alarm if another one comes in within TIME_DIFFERENCE minutes
const int TEST_ALARM_DELTA = 5;         // Interpret as test alarm if it comes in within TEST_ALARM_DELTA minutes of the TEST_ALARM_HOUR
const int TEST_ALARM_DAY = 6;           // Day of the test alarm, where 1 = Monday, 2 = Tuesday...
const int TEST_ALARM_HOUR = 13;         // Time of day for test alarm
#define BUTTON_PIN_BITMASK 0x9000000000 // 2^36 + 2^39 in hex (wakeup GPIOs)

// Pin declaration
const int LED = 13;
const int SMS_ME = 33;                // if high then send SMS to me
const int SEND_MANUAL_ALARM_ALL = 32; // if high then send manual test alarm to all, currently w/o function
const int SEND_TEST_ALARM = 35;       // if high send test alarm to all
const int DEBUGGING = 34;             // if high ignore MAX_ALARMS_PER_DAY and don't send SMS
const int MANUAL_ALARM_BUTTON = 39;   // Button to trigger manual test alarm
const int ALARM_RELAIS = 36;          // Input of alarm relay for pager

// Settings to get time
const char *NTP_SERVER = "pool.ntp.org";
const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3"; // Middle european time zone with daylight saving time

bool send_sms = true;
bool time_error = false;
unsigned int weekday_now = 9;
unsigned int hour_now = 9;
unsigned int min_now = 99;
unsigned int day_of_year_now; // day of year
unsigned int yearday_last;    // day of year of last alarm for the counter
unsigned int per_day_counter; // alarms received per day
bool success = false;         // SMS sending success
String response;              // Twilio response

String message = MESSAGE_HEADER;

// Set up permanent storage
Preferences storage;

String getTimeOnline() // renamed to avoid conflict with existing function
{
  struct tm timeinfo;
  char buffer[21];
  const int maxRetries = 10; // Maximum number of retries
  int attempt = 0;

  while (attempt < maxRetries)
  {
    if (attempt > 1 && getLocalTime(&timeinfo)) // Try to get the local time, multiple attems to enhance relibility
    {
      // Success: save for later
      weekday_now = timeinfo.tm_wday;
      day_of_year_now = timeinfo.tm_yday;
      hour_now = timeinfo.tm_hour;
      min_now = timeinfo.tm_min;

      // Format time
      if (strftime(buffer, sizeof(buffer), "%d.%m.%Y, %H:%M:%S", &timeinfo))
      {
        time_error = false;
        return String(buffer);
      }
      else
      {
        Serial.println("Time: format error");
        time_error = true;
        return "Time: format error";
      }
    }
    else
    {
      Serial.println("Time: network error or attempt < 2, retrying...");
      time_error = true;
      attempt++;
      delay(500); // Wait a little before retrying
    }
  }
  // If all retries fail
  Serial.println("Time: network error (final)");
  return "Time: network error";
}

// To determine which GPIO pin woke up the ESP32, either due to a button press or the pager relay
int getWakeUpPin()
{
  uint64_t GPIO_reason = esp_sleep_get_ext1_wakeup_status();
  int GPIO_num = log(GPIO_reason) / log(2); // Calculate the GPIO number
  return GPIO_num;                          // Return the GPIO number
}

int timeStringToSeconds(String timeinput)
{
  timeinput = timeinput.substring(12, 20);
  int hours = timeinput.substring(0, 2).toInt();
  int minutes = timeinput.substring(3, 5).toInt();
  int seconds = timeinput.substring(6, 8).toInt();
  return (hours * 3600) + (minutes * 60) + seconds;
}

int calculateTimeDifferenceInSeconds(int seconds1, int seconds2)
{
  // If time2 is earlier in the day than time1, we assume it crosses midnight
  if (seconds2 < seconds1)
  {
    seconds2 += 24 * 3600; // Add 24 hours in seconds to time2
  }
  return abs(seconds2 - seconds1);
}

// check if weekly test alarm
bool testalarm()
{
  if (weekday_now == TEST_ALARM_DAY)
  {
    if (hour_now == TEST_ALARM_HOUR && min_now < TEST_ALARM_DELTA) // after TEST_ALARM_HOUR
    {
      Serial.println("Test alarm");
      return true;
    }
    else if (hour_now == TEST_ALARM_HOUR - 1 && 60 - min_now < TEST_ALARM_DELTA) // before TEST_ALARM_HOUR
    {
      Serial.println("Test alarm");
      return true;
    }
  }
  Serial.println("No test alarm");
  return false;
}

void maxAlarms()
{
  if (day_of_year_now == yearday_last)
  {
    per_day_counter++;
  }
  else
  {
    per_day_counter = 1;
    yearday_last = day_of_year_now;
    storage.putUInt("yearday_last", yearday_last);
  }
  if (per_day_counter > MAX_ALARMS_PER_DAY)
  {
    send_sms = false;
    per_day_counter--;
    Serial.println("Max alarms per day reached");
  }
  storage.putUInt("per_day_counter", per_day_counter);
  storage.end(); // flush to NVS
  storage.begin("my-app", false);
}

void sleep()
{
  storage.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_deep_sleep_start();
}

// input 0 = no error, 1 = wifi error, 2 = time error, 3 = sms error, 4 = time diff too small
void errorMessage(int error_code) // display error code with LED
{
  bool new_alarm = false;
  while (true)
  {
    new_alarm = false;
    for (int i = 0; i < error_code; i++)
    {
      digitalWrite(LED, HIGH);
      delay(200);
      digitalWrite(LED, LOW);
      delay(250);
    }
    if (digitalRead(ALARM_RELAIS) + digitalRead(MANUAL_ALARM_BUTTON) > 0)
    {
      new_alarm = true;
    }
    delay(3000);
    if (new_alarm == true && digitalRead(ALARM_RELAIS) + digitalRead(MANUAL_ALARM_BUTTON) > 0)
    {
      ESP.restart();
    }
    Serial.println(error_code);
  }
}

//-----------------------------------------------------------------------------
void setup()
{
  int wake_up_pin = getWakeUpPin();
  unsigned long time_alarm_esp = millis();
  pinMode(LED, OUTPUT);
  pinMode(SMS_ME, INPUT);
  pinMode(SEND_MANUAL_ALARM_ALL, INPUT);
  pinMode(SEND_TEST_ALARM, INPUT);
  pinMode(DEBUGGING, INPUT);
  pinMode(MANUAL_ALARM_BUTTON, INPUT);
  pinMode(ALARM_RELAIS, INPUT);
  digitalWrite(LED, HIGH);
  esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
  delay(100);

  // When first connecting to power, go to sleep instantly
  if (wake_up_pin != ALARM_RELAIS && wake_up_pin != MANUAL_ALARM_BUTTON)
  {
    esp_deep_sleep_start();
  }

  // Retrieve stored values
  storage.begin("my-app", false);                                       // False = write and read mode
  unsigned int counter = storage.getUInt("counter", 0);                 // AlarmID
  unsigned int counter_probe = storage.getUInt("counter_probe", 0);     // AlarmID
  unsigned int counter_sms_all = storage.getUInt("counter_sms_all", 0); // AlarmID: SMS sent in total
  unsigned int counter_test = storage.getUInt("counter_test", 0);       // AlarmID
  unsigned int time_alarm_old = storage.getUInt("time_alarm_old", 0);   // time of last alarm
  per_day_counter = storage.getUInt("per_day_counter", 0);
  yearday_last = storage.getUInt("yearday_last", 0);

  // Debounce Buttons
  if (digitalRead(ALARM_RELAIS) == LOW && digitalRead(MANUAL_ALARM_BUTTON) == LOW)
  {
    esp_deep_sleep_start();
  }

  Serial.begin(115200);
  Serial.println("_________________");
  Serial.printf("Alarm relais: %d\n", digitalRead(ALARM_RELAIS));
  Serial.printf("Manual alarm: %d\n", digitalRead(MANUAL_ALARM_BUTTON));
  Serial.printf("SMS me: %d\n", digitalRead(SMS_ME));
  Serial.printf("Send manual alarm: %d\n", digitalRead(SEND_MANUAL_ALARM_ALL));
  Serial.printf("Send test alarm: %d\n", digitalRead(SEND_TEST_ALARM));
  Serial.printf("Debugging: %d\n\n", digitalRead(DEBUGGING));

  Serial.printf("Connecting to %s ", SSID);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(200);
    Serial.print(".");
    if (millis() - time_alarm_esp > 18000)
    {
      storage.end();
      errorMessage(1);
    }
    // else if (wake_up_pin == ALARM_RELAIS && millis() - time_alarm_esp < 4000 && digitalRead(ALARM_RELAIS) == LOW)
    // { // check within 4 seconds if alarm is still active
    //   storage.end();
    //   esp_deep_sleep_start();
    // }
  }
  Serial.println(" CONNECTED");

  // init and get the time
  configTime(0, 0, NTP_SERVER);
  setenv("TZ", TIMEZONE, 1); // Set the timezone and daylight saving time
  tzset();                   // Update the timezone
  Serial.printf("TZ set to: %s\n", getenv("TZ"));
  String time_alarm_online = getTimeOnline();                             // time and date String
  int time_alarm_online_seconds = timeStringToSeconds(time_alarm_online); // convert to seconds since start of day

  // update message text
  if (wake_up_pin == ALARM_RELAIS)
  {
    int time_diff = calculateTimeDifferenceInSeconds(time_alarm_old, time_alarm_online_seconds);
    if (testalarm() == true)
    {
      Serial.println("Weekly test alarm detected");
      message += MESSAGE_TEST_ALARM;
      counter_probe++;
      storage.putUInt("counter_probe", counter_probe);
      if (digitalRead(SEND_TEST_ALARM) == LOW)
      {
        send_sms = false;
      }
    }
    else if (time_diff > 60 * TIME_DIFFERENCE)
    {
      message += MESSAGE_INCOMING_ALARM;
      counter++;
      storage.putUInt("counter", counter);
      storage.putUInt("time_alarm_old", time_alarm_online_seconds);
    }
    else
    { // if no test alarm but time diff is too small
      message += String(time_diff);
      message += "timediff\n";
      send_sms = false;
      errorMessage(4);
    }
  }
  else if (wake_up_pin == MANUAL_ALARM_BUTTON)
  {
    message += "Testalarm\n";
    counter_test++;
    storage.putUInt("counter_test", counter_test);
  }

  // checks if more than 4 alarms were received today
  if (digitalRead(DEBUGGING) == LOW)
  {
    maxAlarms();
  }

  message += time_alarm_online;
  message += "\nID: D" + String(per_day_counter) + "-P" + String(counter_probe) + "-A" + String(counter) + "-";

  message += "S" + String(counter_sms_all) + "-";
  if (send_sms == true && digitalRead(SMS_ME) == HIGH)
  {
    Twilio *twilio;
    twilio = new Twilio(ACCOUNT_SID, AUTH_TOKEN);
    counter_sms_all++,
        storage.putUInt("counter_sms_all", counter_sms_all);
    success = twilio->send_message(TO_NUMBER, FROM_NUMBER, message + String((millis() - time_alarm_esp)), response);
  }

  Serial.println("_________________");
  Serial.printf("Hour: %d\n", hour_now);
  Serial.printf("Mins: %d\n", min_now);
  Serial.printf("Weekday now: %d\n", weekday_now);
  Serial.printf("Day of year now: %d\n", day_of_year_now);
  Serial.printf("Day of year last: %d\n\n", storage.getUInt("yearday_last", 2));

  Serial.printf("Send sms: %d\n", send_sms);
  Serial.println("_________________\n");
  Serial.println(message + String((millis() - time_alarm_esp)));
  Serial.println("_________________");

  if (success)
  {
    Serial.printf("Sent message successfully to %s!\n", TO_NUMBER);
  }
  else
  {
    if (digitalRead(SMS_ME) == HIGH)
    {
      Serial.printf("Failed to send message to %s. Response: %s\n", TO_NUMBER, response.c_str());
      errorMessage(3);
    }
  }

  if (time_error)
  {
    errorMessage(2);
  }
  // wait for 10 seconds to avoid double alarms as the alarm relay is still closed
  delay(10000);
  sleep();
}
void loop()
{
}