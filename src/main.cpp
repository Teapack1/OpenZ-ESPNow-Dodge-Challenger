#include <esp_now.h>
#include <WiFi.h>
#include <ESP32Servo.h>
#include <cmath>

// Define pins
#define STEERING_SERVO_PIN 18
#define ESC_PIN 19

Servo steeringServo;
Servo esc;

typedef struct struct_message {
  int joy1_x;
  int joy1_y; //active
  int joy2_x; //active
  int joy2_y;
} struct_message;

struct_message receivedData;
bool dataReceived = false;
int packetLossCounter = 0;

void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  memcpy(&receivedData, incomingData, sizeof(receivedData));
  dataReceived = true;  // Packet received successfully
  packetLossCounter = 0; // Reset the packet loss counter
}

int mapSteeringPosition(int joy_value) {
  int offsetValue = joy_value - 50; // Adjust this value to fine-tune the left offset
  return map(offsetValue, -255, 255, 45, 120); // Increased range and offset
}

int mapThrottlePosition(int joy_value) {
  if (joy_value < -10) {
    return map(joy_value, -250, -11, 0, 89); // Reverse throttle
  } else if (joy_value > 0) {
    return map(joy_value, 1, 270, 90, 180); // Forward throttle
  } else {
    return 90; // Neutral throttle
  }
}

void setup() {
  Serial.begin(115200);

  steeringServo.attach(STEERING_SERVO_PIN);
  esc.attach(ESC_PIN);

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {
  Serial.println("Looping");
  if (!dataReceived) {
    packetLossCounter++;
    Serial.println("No Data; Packet loss counter: " + String(packetLossCounter) + "/6");
    if (packetLossCounter > 5) {
      // Stop the car by setting throttle to neutral
      esc.write(90);
      Serial.println("No data received for multiple cycles. Stopping the car.");
      packetLossCounter = 0; // Reset the counter to prevent repeated stops
    }
  } else {
    int steeringPosition = mapSteeringPosition(-receivedData.joy2_x);
    int throttlePosition = mapThrottlePosition(-receivedData.joy1_y);

    steeringServo.write(steeringPosition);
    esc.write(throttlePosition);

    Serial.print("Steering: ");
    Serial.print(receivedData.joy2_x);
    Serial.print(" -> ");
    Serial.println(steeringPosition);

    Serial.print("Throttle: ");
    Serial.print(receivedData.joy1_y);
    Serial.print(" -> ");
    Serial.println(throttlePosition);

    dataReceived = false; // Reset the flag for the next cycle

  }

  delay(5);
}
