#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BleKeyboard.h>
#include <pitches.h>

// Define OLED screen resolution
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
// Define OLED I2C address (default is usually 0x3C or 0x3D)
#define OLED_ADDR   0x3C
#define ALT_OLED_ADDR 0x3D
#define SERIAL_BAUD 115200
#define DEBOUNCE_MS 40
#define ROTARY_DEBOUNCE_MS 25
#define BUZZER_CHANNEL 0
#define BUZZER_RESOLUTION 8

#if defined(CONFIG_IDF_TARGET_ESP32C3)
const uint8_t kButtonPins[] = {0, 1, 2, 3};
const uint8_t ROTARY_CLK_PIN = 4;
const uint8_t ROTARY_DT_PIN = 5;
const uint8_t ROTARY_SW_PIN = 6;
const uint8_t BUZZER_PIN = 7;
const uint8_t I2C_SDA_PIN = 8;
const uint8_t I2C_SCL_PIN = 9;
const char* kBoardLabel = "ESP32-C3";
#else
const uint8_t kButtonPins[] = {27, 14, 18, 19};
const uint8_t ROTARY_CLK_PIN = 32;
const uint8_t ROTARY_DT_PIN = 33;
const uint8_t ROTARY_SW_PIN = 25;
const uint8_t BUZZER_PIN = 26;
const uint8_t I2C_SDA_PIN = 21;
const uint8_t I2C_SCL_PIN = 22;
const char* kBoardLabel = "ESP32";
#endif

const uint8_t BUTTON_COUNT = sizeof(kButtonPins) / sizeof(kButtonPins[0]);

enum UiMode {
  UI_NORMAL,
  UI_MENU
};

struct MacroAction {
  const char* label;
  uint8_t key;
  bool useCtrl;
  bool useGui;
  bool useShift;
};

struct MacroProfile {
  const char* name;
  uint8_t actionIndexForButton[4];
};

const MacroAction kActions[] = {
  {"Zoom Mute",   'a', false, true, true},
  {"Zoom Video",  'v', false, true, true},
  {"Zoom Share",  's', false, true, true},
  {"Zoom Pause",  't', false, true, true},
  {"Zoom Full",   'f', false, true, true},
  {"Lock Screen", 'q', true,  true, false}
};

const MacroProfile kProfiles[] = {
  {"Meeting Core", {0, 1, 2, 3}},
  {"Meeting View", {4, 0, 3, 2}},
  {"Privacy", {1, 5, 0, 4}},
  {"Present", {2, 4, 1, 3}},
  {"Mixed", {5, 2, 0, 1}}
};

const uint8_t PROFILE_COUNT = sizeof(kProfiles) / sizeof(kProfiles[0]);
// Create OLED object
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BleKeyboard bleKeyboard;

int lastButtonReading[] = {HIGH, HIGH, HIGH, HIGH};
int stableButtonState[] = {HIGH, HIGH, HIGH, HIGH};
unsigned long lastDebounceTime[] = {0, 0, 0, 0};
bool lastBleConnected = false;
UiMode uiMode = UI_NORMAL;
uint8_t currentProfile = 0;
uint8_t menuSelection = 0;

int lastRotarySwReading = HIGH;
int stableRotarySwState = HIGH;
unsigned long lastRotarySwDebounceTime = 0;
uint8_t lastRotaryState = 0;
int rotaryTransitionCount = 0;

const int kBleConnectMelody[] = {
  NOTE_C5, NOTE_D5, NOTE_E5, NOTE_G5, NOTE_C6
};
const size_t kBleConnectMelodyLen = sizeof(kBleConnectMelody) / sizeof(kBleConnectMelody[0]);

void playPassiveTone(unsigned int freqHz, unsigned int onMs, unsigned int offMs = 0) {
  ledcWriteTone(BUZZER_CHANNEL, freqHz);
  delay(onMs);
  ledcWriteTone(BUZZER_CHANNEL, 0);
  if (offMs > 0) {
    delay(offMs);
  }
}

void playMelody(const int* notes, size_t noteCount, unsigned int noteMs, unsigned int gapMs) {
  for (size_t i = 0; i < noteCount; ++i) {
    playPassiveTone(notes[i], noteMs, gapMs);
  }
}

void playBleConnectedBeep() {
  playMelody(kBleConnectMelody, kBleConnectMelodyLen, 90, 25);
}

void playBleDisconnectedBeep() {
  playPassiveTone(500, 180, 0);
}

void playActionOkBeep() {
  playPassiveTone(1400, 35, 0);
}

void playActionErrorBeep() {
  playPassiveTone(280, 120, 30);
  playPassiveTone(220, 160, 0);
}

void playMenuMoveBeep() {
  playPassiveTone(1800, 15, 0);
}

void playMenuSelectBeep() {
  playPassiveTone(1300, 40, 0);
}

void sendShortcut(const MacroAction& action) {
  if (action.useCtrl) {
    bleKeyboard.press(KEY_LEFT_CTRL);
  }
  if (action.useGui) {
    bleKeyboard.press(KEY_LEFT_GUI);
  }
  if (action.useShift) {
    bleKeyboard.press(KEY_LEFT_SHIFT);
  }

  bleKeyboard.press(action.key);
  delay(100);
  bleKeyboard.releaseAll();
}

void drawNormalScreen(bool bleConnected) {
  const MacroProfile& profile = kProfiles[currentProfile];
  const MacroAction& action1 = kActions[profile.actionIndexForButton[0]];
  const MacroAction& action2 = kActions[profile.actionIndexForButton[1]];
  const MacroAction& action3 = kActions[profile.actionIndexForButton[2]];
  const MacroAction& action4 = kActions[profile.actionIndexForButton[3]];

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("Macrodeck C3"));
  display.print(F("Profile: "));
  display.println(profile.name);
  display.print(F("BTN1: "));
  display.println(action1.label);
  display.print(F("BTN2: "));
  display.println(action2.label);
  display.print(F("BTN3: "));
  display.println(action3.label);
  display.print(F("BTN4: "));
  display.println(action4.label);
  display.println(bleConnected ? F("BLE OK") : F("BLE OFF"));
  display.display();
}

void drawMenuScreen(bool bleConnected) {
  const MacroProfile& selected = kProfiles[menuSelection];
  const MacroAction& action1 = kActions[selected.actionIndexForButton[0]];
  const MacroAction& action2 = kActions[selected.actionIndexForButton[1]];
  const MacroAction& action3 = kActions[selected.actionIndexForButton[2]];
  const MacroAction& action4 = kActions[selected.actionIndexForButton[3]];

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("Select Profile"));
  display.print(F("> "));
  display.println(selected.name);
  display.print(F("1:"));
  display.println(action1.label);
  display.print(F("2:"));
  display.println(action2.label);
  display.print(F("3:"));
  display.println(action3.label);
  display.print(F("4:"));
  display.println(action4.label);
  display.println(bleConnected ? F("Click: save") : F("Click: save (offline)"));
  display.display();
}

void updateActionDisplay(uint8_t buttonIndex, const char* label, bool connected) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("BTN"));
  display.print(buttonIndex + 1);
  display.print(F(": "));
  display.println(label);

  if (connected) {
    display.println(F("BLE connected"));
    display.println(F("Macro sent"));
  } else {
    display.println(F("BLE not connected"));
    display.println(F("Macro not sent"));
  }
  display.display();
}

void handleButtonPressed(uint8_t buttonIndex, bool bleConnected) {
  const MacroProfile& profile = kProfiles[currentProfile];
  if (buttonIndex >= BUTTON_COUNT) {
    return;
  }
  uint8_t actionIndex = profile.actionIndexForButton[buttonIndex];
  const MacroAction& action = kActions[actionIndex];

  if (bleConnected) {
    Serial.print(F("BTN"));
    Serial.print(buttonIndex + 1);
    Serial.print(F(" -> "));
    Serial.println(action.label);
    sendShortcut(action);
    playActionOkBeep();
    updateActionDisplay(buttonIndex, action.label, true);
  } else {
    Serial.print(F("BTN"));
    Serial.print(buttonIndex + 1);
    Serial.print(F(" pressed, BLE disconnected: "));
    Serial.println(action.label);
    playActionErrorBeep();
    updateActionDisplay(buttonIndex, action.label, false);
  }
}

void handleEncoderClick(bool bleConnected) {
  if (uiMode == UI_NORMAL) {
    uiMode = UI_MENU;
    menuSelection = currentProfile;
    Serial.println(F("Menu opened"));
    playMenuSelectBeep();
    drawMenuScreen(bleConnected);
    return;
  }

  currentProfile = menuSelection;
  uiMode = UI_NORMAL;
  Serial.print(F("Profile selected: "));
  Serial.println(kProfiles[currentProfile].name);
  playMenuSelectBeep();
  drawNormalScreen(bleConnected);
}

void handleEncoderStep(int direction, bool bleConnected) {
  if (uiMode != UI_MENU) {
    return;
  }

  int next = static_cast<int>(menuSelection) + direction;
  if (next < 0) {
    next = PROFILE_COUNT - 1;
  } else if (next >= PROFILE_COUNT) {
    next = 0;
  }

  if (next != menuSelection) {
    menuSelection = static_cast<uint8_t>(next);
    playMenuMoveBeep();
    drawMenuScreen(bleConnected);
  }
}


bool initDisplayAtAddress(uint8_t addr) {
  if (display.begin(SSD1306_SWITCHCAPVCC, addr)) {
    Serial.print(F("OLED found at 0x"));
    Serial.println(addr, HEX);
    return true;
  }
  return false;

}

void setup() {
  // Initialize serial first so boot logs are visible in monitor.
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println();
  Serial.println(F("Booting macrodeck..."));
  Serial.println(F("Hello world"));
  bleKeyboard.begin();
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    pinMode(kButtonPins[i], INPUT_PULLUP);
  }
  pinMode(ROTARY_CLK_PIN, INPUT_PULLUP);
  pinMode(ROTARY_DT_PIN, INPUT_PULLUP);
  pinMode(ROTARY_SW_PIN, INPUT_PULLUP);
  lastRotaryState = (static_cast<uint8_t>(digitalRead(ROTARY_CLK_PIN)) << 1) |
                    static_cast<uint8_t>(digitalRead(ROTARY_DT_PIN));

  ledcSetup(BUZZER_CHANNEL, 2000, BUZZER_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, 0);
  Serial.println(F("4 buttons ready (active LOW)"));
  Serial.print(F("Rotary pins: CLK="));
  Serial.print(ROTARY_CLK_PIN);
  Serial.print(F(" DT="));
  Serial.print(ROTARY_DT_PIN);
  Serial.print(F(" SW="));
  Serial.println(ROTARY_SW_PIN);
  Serial.print(F("Passive buzzer on GPIO "));
  Serial.println(BUZZER_PIN);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.print(F("I2C started on SDA="));
  Serial.print(I2C_SDA_PIN);
  Serial.print(F(" SCL="));
  Serial.println(I2C_SCL_PIN);

  // Initialize OLED screen
  bool oledReady = initDisplayAtAddress(OLED_ADDR) || initDisplayAtAddress(ALT_OLED_ADDR);
  if (!oledReady) {
    Serial.println(F("SSD1306 init failed at 0x3C and 0x3D"));
    Serial.println(F("Check wiring and OLED voltage (3V3/GND)."));
    for (;;) {
      Serial.println(F("Waiting: OLED not detected"));
      delay(2000);
    }
  }

  // Clear display buffer
  display.clearDisplay();

  // Set text size and color
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Display initialization information
  display.setCursor(0, 0);
  display.println(F("Macrodeck Ready"));
  display.println(kBoardLabel);
  display.println(F("4 Buttons + Rotary"));
  display.println(F("Click: menu"));
  display.println(F("Rotate: choose profile"));

  // Update display
  display.display();

  // Delay for 2 seconds
  delay(2000);
  playPassiveTone(1000, 40, 0);
  drawNormalScreen(false);
  Serial.println(F("Setup complete."));
}

void loop() {
  bool bleConnected = bleKeyboard.isConnected();
  if (bleConnected && !lastBleConnected) {
    Serial.println(F("BLE connected"));
    playBleConnectedBeep();
  } else if (!bleConnected && lastBleConnected) {
    Serial.println(F("BLE disconnected"));
    playBleDisconnectedBeep();
  }
  lastBleConnected = bleConnected;

  uint8_t rotaryState = (static_cast<uint8_t>(digitalRead(ROTARY_CLK_PIN)) << 1) |
                        static_cast<uint8_t>(digitalRead(ROTARY_DT_PIN));
  if (rotaryState != lastRotaryState) {
    uint8_t transition = (lastRotaryState << 2) | rotaryState;
    if (transition == 0b1101 || transition == 0b0100 || transition == 0b0010 || transition == 0b1011) {
      rotaryTransitionCount++;
    } else if (transition == 0b1110 || transition == 0b0111 || transition == 0b0001 || transition == 0b1000) {
      rotaryTransitionCount--;
    }

    if (rotaryTransitionCount >= 4) {
      handleEncoderStep(1, bleConnected);
      rotaryTransitionCount = 0;
    } else if (rotaryTransitionCount <= -4) {
      handleEncoderStep(-1, bleConnected);
      rotaryTransitionCount = 0;
    }
    lastRotaryState = rotaryState;
  }

  int rotarySwReading = digitalRead(ROTARY_SW_PIN);
  if (rotarySwReading != lastRotarySwReading) {
    lastRotarySwDebounceTime = millis();
  }
  if ((millis() - lastRotarySwDebounceTime) > ROTARY_DEBOUNCE_MS) {
    if (rotarySwReading != stableRotarySwState) {
      stableRotarySwState = rotarySwReading;
      if (stableRotarySwState == LOW) {
        handleEncoderClick(bleConnected);
      }
    }
  }
  lastRotarySwReading = rotarySwReading;

  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    int reading = digitalRead(kButtonPins[i]);

    if (reading != lastButtonReading[i]) {
      lastDebounceTime[i] = millis();
    }

    if ((millis() - lastDebounceTime[i]) > DEBOUNCE_MS) {
      if (reading != stableButtonState[i]) {
        stableButtonState[i] = reading;
        if (stableButtonState[i] == LOW) {
          if (uiMode == UI_NORMAL) {
            handleButtonPressed(i, bleConnected);
          }
        }
      }
    }

    lastButtonReading[i] = reading;
  }

  delay(5);
}

/*
Pin notes:
- ESP32-C3 profile and ESP32 profile are selected at compile time.
- Rotary switch and buttons are active LOW with INPUT_PULLUP.
- If your ESP32-C3 SuperMini exposes different preferred pins, only update
  the constants in the board profile section near the top of this file.
*/