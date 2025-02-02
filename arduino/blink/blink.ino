// const int led = LED_BUILTIN;
const int LED = 13; // Additional LED

void setup()
{
    pinMode(LED, OUTPUT);
    Serial.begin(115200);
}

void loop()
{
    Serial.println("Hello!");
    digitalWrite(LED, HIGH);
    delay(500);
    digitalWrite(LED, LOW);
    delay(500);
}