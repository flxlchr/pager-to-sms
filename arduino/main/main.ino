#include <WiFi.h>
#include <time.h>
#include <twilio.hpp>
#include <Preferences.h> // save data permanently
#include "../secrets.h"  // contains sensitive data like twilio credentials

// Constants
const int MAX_ALARMS_PER_DAY = 5;       // Avoid high SMS costs in case of a bug
const int TIME_DIFF = 25;               // Avoid resending alarm if another one comes in within TIME_DIFF minutes
const int TEST_ALARM_DELTA = 5;         // Interpret as test alarm if it comes in within TEST_ALARM_DELTA minutes of the TEST_ALARM_HOUR
const int TEST_ALARM_DAY = 6;           // Day of the test alarm, where 1 = Monday, 2 = Tuesday...
const int TEST_ALARM_HOUR = 13;         // Time of day for test alarm
#define BUTTON_PIN_BITMASK 0x9000000000 // 2^36 + 2^39 in hex (wakeup GPIOs)

// Pin declaration
const int LED = 13;
const int SMS_ME = 33;                // if high then send SMS to me
const int SEND_MANUAL_ALARM_ALL = 32; // if high then send manual test alarm to all
const int SEND_TEST_ALARM = 35;       // if high send test alarm to all
const int DEBUGGING = 34;             // if high ignore MAX_ALARMS_PER_DAY
const int MANUAL_ALARM_BUTTON = 39;   // Button to trigger manual test alarm
const int ALARM_RELAIS = 36;          // Input of alarm relay for pager

// Settings to get time
const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600;     // Adjust to correct time zone
const int DAYLIGHT_OFFSET_SEC = 3600; // To account for summer time

bool send_sms = true;
int day_now = 9;
int hour_now = 9;
int min_now = 99;
unsigned int per_day_counter; // alarms received per day
unsigned int per_day_weekday; // weekday of last recorder allarm for the counter
int error_code = 0;           // 0 = no error, 1 = wifi error, 2 = time error, 3 = sms error
bool success = true;          // SMS sending success
String response;              // Twilio response

String message = MESSAGE_HEADER;

// Set up permanent storage
Preferences storage;
String getLocalTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    // return "Time: network error"; // Return an error message if time retrieval fails
    Serial.println("Time: network error"); // Return an error message if time retrieval fails
    error_code = 2;
    errorMessage();
  }
  // save for later
  day_now = timeinfo.tm_wday;
  hour_now = timeinfo.tm_hour;
  min_now = timeinfo.tm_min;
  // Buffer to hold the formatted date string
  char buffer[21];
  if (strftime(buffer, sizeof(buffer), "%d.%m.%Y, %H:%M:%S", &timeinfo))
  {
    // Convert the char array to a String and return
    return String(buffer);
  }
  else
  {
    return "Time: format error"; // Return an error message if formatting fails
  }
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

// check if manual test alarm and if max alarms per day is reached
bool testalarm()
{
  if (day_now == TEST_ALARM_DAY)
  {
    if (hour_now == TEST_ALARM_HOUR && min_now < TEST_ALARM_DELTA)
    {
      Serial.println("Test alarm");
      return true;
    }
    if (hour_now == TEST_ALARM_HOUR - 1 && 60 - min_now < TEST_ALARM_DELTA)
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
  if (day_now == per_day_weekday)
  {
    per_day_counter++;
  }
  else
  {
    per_day_counter = 0;
    storage.putUInt("per_day_weekday", day_now);
  }
  if (per_day_counter > MAX_ALARMS_PER_DAY)
  {
    send_sms = false;
  }
  storage.putUInt("per_day_counter", per_day_counter);
}

void sleep()
{
  storage.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_deep_sleep_start();
}

void errorMessage() // display error code with LED
{
  while (true)
  {
    for (int i = 0; i < error_code; i++)
    {
      digitalWrite(LED, HIGH);
      delay(200);
      digitalWrite(LED, LOW);
      delay(250);
    }
    delay(3000);
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
  if (wake_up_pin != 36 && wake_up_pin != 39)
  {
    esp_deep_sleep_start();
  }

  // Retrieve stored values
  storage.begin("my-app", false);                                       // False = write and read mode
  unsigned int counter = storage.getUInt("counter", 0);                 // AlarmID
  unsigned int counter_probe = storage.getUInt("counter_probe", 0);     // AlarmID
  unsigned int counter_sms_all = storage.getUInt("counter_sms_all", 0); // AlarmID
  unsigned int counter_test = storage.getUInt("counter_test", 0);       // AlarmID
  unsigned int time_alarm_old = storage.getUInt("time_alarm_old", 0);   // time of last alarm
  per_day_counter = storage.getUInt("per_day_counter", 0);
  per_day_weekday = storage.getUInt("per_day_weekday", 0);

  // Debounce Buttons
  if (digitalRead(ALARM_RELAIS) == LOW && digitalRead(MANUAL_ALARM_BUTTON) == LOW)
  {
    storage.end();
    esp_deep_sleep_start();
  }

  Serial.begin(115200);
  Serial.printf("Connecting to %s ", SSID);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(200);
    Serial.print(".");
    if (millis() - time_alarm_esp > 180000)
    {
      error_code = 1;
      storage.end();
      errorMessage();
    }
    else if (wake_up_pin == ALARM_RELAIS && millis() - time_alarm_esp < 4000 && digitalRead(ALARM_RELAIS) == LOW)
    { // check within 4 seconds if alarm is still active
      storage.end();
      esp_deep_sleep_start();
    }
  }
  Serial.println(" CONNECTED");

  // init and get the time
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  String time_alarm_online = getLocalTime();                              // time and date String
  int time_alarm_online_seconds = timeStringToSeconds(time_alarm_online); // convert to seconds since start of day

  // update message text
  if (wake_up_pin == ALARM_RELAIS)
  {
    int time_diff = calculateTimeDifferenceInSeconds(time_alarm_old, time_alarm_online_seconds);
    if (testalarm() == true)
    {
      message += MESSAGE_TEST_ALARM;
      counter_probe++;
      storage.putUInt("counter_probe", counter_probe);
      if (digitalRead(SEND_TEST_ALARM) == LOW)
      {
        send_sms = false;
      }
    }
    else if (time_diff > 60 * TIME_DIFF)
    {
      message += MESSAGE_INCOMING_ALARMseg;
      counter++;
      storage.putUInt("counter", counter);
      storage.putUInt("time_alarm_old", time_alarm_online_seconds);
    }
    else
    { // if no test alarm but time diff is too small
      message += String(time_diff);
      message += "timediff\n";
      send_sms = false;
    }
  }
  else if (wake_up_pin == MANUAL_ALARM_BUTTON)
  {
    message += "Testalarm\n";
    counter_test++;
    storage.putUInt("counter_test", counter_test);
  }
  message += time_alarm_online;
  message += "\nID: D" + String(per_day_counter) + "-P" + String(counter_probe) + "-A" + String(counter) + "-";

  // checks if more than 4 alarms were received today
  if (digitalRead(DEBUGGING) == LOW)
  {
    maxAlarms();
  }

  message += "S" + String(counter_sms_all) + "-";
  if (digitalRead(DEBUGGING) == LOW && send_sms == true)
  {
    Twilio *twilio;
    twilio = new Twilio(ACCOUNT_SID, AUTH_TOKEN);

    if (wake_up_pin == ALARM_RELAIS)
    {
      counter_sms_all++,
          storage.putUInt("counter_sms_all", counter_sms_all);
      if (digitalRead(SMS_ME) == HIGH)
      {
        success = twilio->send_message(TO_NUMBER, FROM_NUMBER, message + String((millis() - time_alarm_esp)), response);
      }

      // send to all
      for (int i = 0; i < sizeof(TO_NUMBERS) / sizeof(TO_NUMBERS[0]); i++)
      {
        success = twilio->send_message(TO_NUMBERS[i], FROM_NUMBER, message + String((millis() - time_alarm_esp)), response);
      }
    }
    else if (digitalRead(SMS_ME) == HIGH)
    { // send testmessage
      success = twilio->send_message(TO_NUMBER, FROM_NUMBER, message + String((millis() - time_alarm_esp)), response);

      if (digitalRead(SEND_MANUAL_ALARM_ALL) == HIGH)
      {
        counter_sms_all++,
            storage.putUInt("counter_sms_all", counter_sms_all);
        for (int i = 0; i < sizeof(TO_NUMBERS) / sizeof(TO_NUMBERS[0]); i++)
        {
          success = twilio->send_message(TO_NUMBERS[i], FROM_NUMBER, message + String((millis() - time_alarm_esp)), response);
        }
      }
    }
  }
  else
  {
    Serial.println("_________________\n");
    Serial.println(message + String((millis() - time_alarm_esp)));
    Serial.println("_________________");
  }
  if (success)
  {
    Serial.printf("Sent message successfully to %s!\n", TO_NUMBER);
  }
  else
  {
    Serial.printf("Failed to send message to %s. Response: %s\n", TO_NUMBER, response.c_str());
    if (digitalRead(SMS_ME) == HIGH)
    {
      error_code = 3;
      errorMessage();
    }
  }

  // wait for 10 seconds to avoid double alarms as the alarm relay is still closed
  delay(10000);

  if (error_code == 0)
  {
    sleep();
  }
}
void loop()
{
}