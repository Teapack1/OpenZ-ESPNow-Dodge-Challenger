#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <ESP32Servo.h>
#include <cmath>

#include <BLEGamepadClient.h>   // BLE-Gamepad-Client [page:0]

// ----------------- Pins -----------------
#define STEERING_SERVO_PIN 18
#define ESC_PIN 19

// External 2-position switch:
// - HIGH (default via pullup) = Xbox BLE mode
// - LOW  (switch to GND)      = ESP-NOW mode
#define MODE_SWITCH_PIN 27

Servo steeringServo;
Servo esc;

// ----------------- Original struct (kept) -----------------
typedef struct struct_message {
  int joy1_x;
  int joy1_y; // active
  int joy2_x; // active
  int joy2_y;
} struct_message;

struct_message receivedData;
bool dataReceived = false;
int packetLossCounter = 0;

// ----------------- Shared data safety -----------------
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

static inline void setReceivedData(const struct_message &m) {
  portENTER_CRITICAL(&dataMux);
  receivedData = m;
  dataReceived = true;
  packetLossCounter = 0;
  portEXIT_CRITICAL(&dataMux);
}

// ----------------- ESP-NOW (original behavior) -----------------
bool espnowInitialized = false;

void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  if (len < (int)sizeof(struct_message)) return;
  struct_message m;
  memcpy(&m, incomingData, sizeof(m));
  setReceivedData(m);
}

void initEspNowIfNeeded() {
  if (espnowInitialized) return;

  Serial.println("[MODE] ESP-NOW selected -> initializing WiFi+ESP-NOW");

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERR ] Error initializing ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);

  espnowInitialized = true;
  Serial.println("[OK  ] ESP-NOW ready");
}

void deinitEspNowIfNeeded() {
  if (!espnowInitialized) return;

  Serial.println("[MODE] Xbox BLE selected -> shutting down ESP-NOW/WiFi");
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);

  espnowInitialized = false;
  Serial.println("[OK  ] ESP-NOW/WiFi off");
}

// ----------------- Xbox BLE (default) -----------------
BLEAutoScan *pAutoScan = BLEGamepadClient::getAutoScan();
XboxController xboxController;

// minimal connection logging + “what to do” line (brief)
volatile bool xboxConnected = false;
uint32_t lastXboxLogMs = 0;

void onScanStarted()     { Serial.println("[XBOX] scan started"); }
void onScanStopped()     { Serial.println("[XBOX] scan stopped"); }
void onConnecting(XboxController &) { Serial.println("[XBOX] connecting..."); }
void onConnectionFailed(XboxController &) { Serial.println("[XBOX] connection failed (controller: Xbox + hold Pair 3s)"); }
void onConnected(XboxController &)  { Serial.println("[XBOX] connected"); xboxConnected = true; }
void onDisconnected(XboxController &) {
  Serial.println("[XBOX] disconnected -> will keep scanning (controller: Xbox + hold Pair 3s)");
  xboxConnected = false;
}

// ----------------- Mapping (kept) -----------------
int mapSteeringPosition(int joy_value) {
  int offsetValue = joy_value - 0; // Adjust this value to fine-tune the left offset
  return map(offsetValue, -255, 255, 45, 165); // Increased range and offset
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

// BLE-Gamepad-Client example reads sticks as floats (-1..+1) [page:0]
static inline int stickToJoy255(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return (int)lroundf(v * 255.0f);
}

static inline int applyDeadzone(int v, int dz) {
  return (abs(v) < dz) ? 0 : v;
}

// ----------------- Smoothing (new, but light-touch) -----------------
float steeringOut = 90.0f;
float throttleOut = 90.0f;

const float STEER_ALPHA = 0.1f;     // higher = faster response
const float THROTTLE_ALPHA = 0.01f;  // slightly smoother than steering
const float STEER_MAX_STEP = 1.5f;   // deg per loop
const float THROTTLE_MAX_STEP = 1.0f;

static inline float clampStep(float current, float target, float maxStep) {
  float d = target - current;
  if (d >  maxStep) d =  maxStep;
  if (d < -maxStep) d = -maxStep;
  return current + d;
}

static inline float ema(float current, float target, float alpha) {
  return current + alpha * (target - current);
}

static inline int clampServoDeg(float v) {
  if (v < 0) v = 0;
  if (v > 180) v = 180;
  return (int)lroundf(v);
}

// ----------------- Mode handling -----------------
bool lastEspNowMode = false;

static inline bool isEspNowMode() {
  // LOW = ESP-NOW, HIGH = Xbox (default)
  return digitalRead(MODE_SWITCH_PIN) == LOW;
}

// ----------------- Setup -----------------
void setup() {
  Serial.begin(115200);

  pinMode(MODE_SWITCH_PIN, INPUT_PULLUP);

  steeringServo.attach(STEERING_SERVO_PIN);
  esc.attach(ESC_PIN);
  esc.write(90);

  Serial.println("Boot.");
  Serial.println("Default: Xbox BLE mode. (Switch MODE pin LOW for ESP-NOW.)");
  Serial.println("Xbox pairing: press Xbox, hold Pair ~3s, release. [brief]"); // [page:0]

  // Register BLE logs (from library example style) [page:0]
  pAutoScan->onScanStarted(onScanStarted);
  pAutoScan->onScanStopped(onScanStopped);

  xboxController.onConnecting(onConnecting);
  xboxController.onConnectionFailed(onConnectionFailed);
  xboxController.onConnected(onConnected);
  xboxController.onDisconnected(onDisconnected);

  // Start in the selected mode (default is Xbox because INPUT_PULLUP)
  lastEspNowMode = isEspNowMode();
  if (lastEspNowMode) {
    initEspNowIfNeeded();
  } else {
    deinitEspNowIfNeeded();
    Serial.println("[XBOX] begin() -> scanning/connecting now");
    xboxController.begin(); // starts scanning/connection loop [page:0]
  }
}

// ----------------- Main loop (kept style) -----------------
void loop() {
  // Handle mode switch changes quickly
  bool espNowMode = isEspNowMode();
  if (espNowMode != lastEspNowMode) {
    lastEspNowMode = espNowMode;

    // Immediately neutral throttle when switching modes
    esc.write(90);

    if (espNowMode) {
      initEspNowIfNeeded();
      Serial.println("[MODE] Using ESP-NOW receiver (original behavior)");
    } else {
      deinitEspNowIfNeeded();
      Serial.println("[MODE] Using Xbox BLE controller (default)");
      Serial.println("[XBOX] controller: Xbox + hold Pair 3s");
      xboxController.begin(); // safe to call again; ensures scanning starts [page:0]
    }
  }

  // Feed receivedData depending on mode
  if (!espNowMode) {
    // Xbox mode: always try to read when connected (keeps behavior similar to “always receiving packets”)
    if (xboxController.isConnected()) {  // shown in example usage [page:0]
      XboxControlsState s;
      xboxController.read(&s);           // shown in example usage [page:0]

      struct_message m{};
      m.joy2_x = applyDeadzone(stickToJoy255(s.rightStickX), 8); // steering input
      m.joy1_y = applyDeadzone(stickToJoy255(s.leftStickY), 10); // throttle input
      setReceivedData(m);
    } else {
      // No BLE connection -> behave like “no packets”
      dataReceived = false;
      if (millis() - lastXboxLogMs > 700) {
        lastXboxLogMs = millis();
        Serial.println("[XBOX] not connected (press Xbox, hold Pair 3s)");
      }
    }
  } else {
    // ESP-NOW mode: dataReceived is set only by OnDataRecv()
    // (no extra work here)
  }

  // ---- Original control logic with minor smoothing ----
  if (!dataReceived) {
    packetLossCounter++;
    if (packetLossCounter > 5) {
      esc.write(90);
      // keep the log rate low (avoid spamming)
      static uint32_t lastStopLogMs = 0;
      if (millis() - lastStopLogMs > 400) {
        lastStopLogMs = millis();
        Serial.println("No data received for multiple cycles. Stopping the car.");
      }
      packetLossCounter = 0;
    }
  } else {
    int steeringTarget = mapSteeringPosition(-receivedData.joy2_x);
    int throttleTarget = mapThrottlePosition(receivedData.joy1_y);

    // Smooth + rate-limit (reduces twitch + perceived lag spikes)
    steeringOut = ema(steeringOut, (float)steeringTarget, STEER_ALPHA);
    throttleOut = ema(throttleOut, (float)throttleTarget, THROTTLE_ALPHA);

    steeringOut = clampStep(steeringOut, (float)steeringTarget, STEER_MAX_STEP);
    throttleOut = clampStep(throttleOut, (float)throttleTarget, THROTTLE_MAX_STEP);

    steeringServo.write(clampServoDeg(steeringOut));
    esc.write(clampServoDeg(throttleOut));

    // Keep original “Reset the flag” behavior
    dataReceived = false;
  }

  delay(5);
}
