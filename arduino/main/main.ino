#include <WiFi.h>
#include <twilio.hpp>
#include "../secrets.h" // contains sensitive data like twilio credentials

// Constants
#define BUTTON_PIN_BITMASK 0x9000000000 // 2^36 + 2^39 in hex (wakeup GPIOs)

// Pin declaration
const int LED = 13;
const int SMS_ME = 33;                // if high then send SMS to me
const int SEND_MANUAL_ALARM_ALL = 32; // if high then send manual test alarm to all, currently w/o function
const int SEND_TEST_ALARM = 35;       // if high send test alarm to all
const int DEBUGGING = 34;             // if high ignore MAX_ALARMS_PER_DAY and don't send SMS
const int MANUAL_ALARM_BUTTON = 39;   // Button to trigger manual test alarm
const int ALARM_RELAIS = 36;          // Input of alarm relay for pager

// Variables
bool send_sms = true;
bool success = false; // SMS sending success
String response;      // Twilio response

String message = "";

// To determine which GPIO pin woke up the ESP32, either due to a button press or the pager relay
int getWakeUpPin()
{
  uint64_t GPIO_reason = esp_sleep_get_ext1_wakeup_status();
  int GPIO_num = log(GPIO_reason) / log(2); // Calculate the GPIO number
  return GPIO_num;                          // Return the GPIO number
}

void sleep()
{
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
  delay(100);

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
      errorMessage(1);
    }
  }
  Serial.println(" CONNECTED");

  // update message text
  if (wake_up_pin == ALARM_RELAIS)
  {
    message += "SEG-Alarm";
  }
  else if (wake_up_pin == MANUAL_ALARM_BUTTON)
  {
    message += "Testalarm";
  }

  if (send_sms == true && digitalRead(SMS_ME) == HIGH)
  {
    Twilio *twilio;
    twilio = new Twilio(ACCOUNT_SID, AUTH_TOKEN);
    success = twilio->send_message(TO_NUMBER, FROM_NUMBER, message, response);
  }

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

  // wait for 10 seconds to avoid double alarms as the alarm relay is still closed
  delay(11000);
  sleep();
}
void loop()
{
}