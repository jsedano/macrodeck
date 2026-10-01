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
#define ROTARY_FAST_WINDOW_MS 80
#define ROTARY_BEEP_COOLDOWN_MS 60
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

enum RotaryBehavior {
  ROTARY_BEHAVIOR_VOLUME,
  ROTARY_BEHAVIOR_TRACK,
  ROTARY_BEHAVIOR_UP_DOWN,
  ROTARY_BEHAVIOR_NONE
};

struct MacroAction {
  const char* label;
  uint8_t key;
  const uint8_t* mediaKeyReport;
  bool useCtrl;
  bool useGui;
  bool useShift;
};

struct MacroProfile {
  const char* name;
  uint8_t actionIndexForButton[4];
  RotaryBehavior rotaryBehavior;
};

const MacroAction kActions[] = {
  // Label,       Key,  MediaKey,                       Ctrl,  Gui,   Shift
  {"Mute Mic",          'a',  nullptr,                  false, true,  true},
  {"Web Cam",           'v',  nullptr,                  false, true,  true},
  {"Share Screen",      's',  nullptr,                  false, true,  true},
  {"Pause S. Share",    't',  nullptr,                  false, true,  true},
  {"Zoom Full",         'f',  nullptr,                  false, true,  true},
  {"Lock Screen",       'q',  nullptr,                  true,  true,  false},
  {"Prev",              '\0', KEY_MEDIA_PREVIOUS_TRACK, false, false, false},
  {"Play",              '\0', KEY_MEDIA_PLAY_PAUSE,     false, false, false},
  {"Next",              '\0', KEY_MEDIA_NEXT_TRACK,     false, false, false},
  {"Mute",              '\0', KEY_MEDIA_MUTE,           false, false, false},
  {"Enter",           KEY_RETURN,  nullptr,           false, false, false},
  {"Ctrl+C",           'c',  nullptr,           true, false, false},
  {"Tab",           KEY_TAB,  nullptr,           false, false, false},
  {"Circle tabs",           KEY_RIGHT_ARROW,  nullptr,           false, true, false}
};

const MacroProfile kProfiles[] = {
  {"Zoom Core", {0, 1, 2, 3}, ROTARY_BEHAVIOR_VOLUME},
  {"Media", {6, 7, 8, 9}, ROTARY_BEHAVIOR_VOLUME},
  {"Claude", {10, 11, 12, 13}, ROTARY_BEHAVIOR_UP_DOWN}
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
unsigned long lastEncoderStepMs = 0;
unsigned long lastRotaryBeepMs = 0;

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

void playRotaryActionBeep() {
  playPassiveTone(1650, 12, 0);
}

void playRotaryActionBeepThrottled() {
  unsigned long now = millis();
  if (now - lastRotaryBeepMs >= ROTARY_BEEP_COOLDOWN_MS) {
    playRotaryActionBeep();
    lastRotaryBeepMs = now;
  }
}

void sendGuiArrow(uint8_t arrowKey) {
  // Use a tight GUI+Arrow chord so terminals/apps interpret it as app scroll.
  bleKeyboard.press(KEY_LEFT_GUI);
  bleKeyboard.press(arrowKey);
  bleKeyboard.releaseAll();
}

const char* rotaryBehaviorLabel(RotaryBehavior behavior) {
  switch (behavior) {
    case ROTARY_BEHAVIOR_VOLUME:
      return "Volume";
    case ROTARY_BEHAVIOR_TRACK:
      return "Track";
    case ROTARY_BEHAVIOR_UP_DOWN:
      return "Up/Down";  
    case ROTARY_BEHAVIOR_NONE:
      return "None";
    default:
      return "Unknown";
  }
}

void triggerRotaryBehavior(RotaryBehavior behavior, int direction, bool bleConnected) {
  if (!bleConnected) {
    playActionErrorBeep();
    return;
  }

  switch (behavior) {
    case ROTARY_BEHAVIOR_VOLUME:
      if (direction > 0) {
        bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
        Serial.println(F("Rotary -> Volume Up"));
      } else {
        bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
        Serial.println(F("Rotary -> Volume Down"));
      }
      playRotaryActionBeep();
      break;
    case ROTARY_BEHAVIOR_TRACK:
      if (direction > 0) {
        bleKeyboard.write(KEY_MEDIA_NEXT_TRACK);
        Serial.println(F("Rotary -> Next Track"));
      } else {
        bleKeyboard.write(KEY_MEDIA_PREVIOUS_TRACK);
        Serial.println(F("Rotary -> Previous Track"));
      }
      playRotaryActionBeep();
      break;
    case ROTARY_BEHAVIOR_UP_DOWN:
      if (direction > 0) {
        sendGuiArrow(KEY_DOWN_ARROW);
      } else {
        sendGuiArrow(KEY_UP_ARROW);
      }
      playRotaryActionBeepThrottled();
      break;

    case ROTARY_BEHAVIOR_NONE:
      break;
  }
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
  if (action.mediaKeyReport != nullptr) {
    bleKeyboard.write(action.mediaKeyReport);
  } else {
    bleKeyboard.press(action.key);
    delay(100);
    bleKeyboard.releaseAll();
  }

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
  display.println(F("Macrodeck"));
  display.print(F("Profile: "));
  display.println(profile.name);
  display.print(F("1: "));
  display.println(action1.label);
  display.print(F("2: "));
  display.println(action2.label);
  display.print(F("3: "));
  display.println(action3.label);
  display.print(F("4: "));
  display.println(action4.label);
  display.print(F("ROT: "));
  display.println(rotaryBehaviorLabel(profile.rotaryBehavior));
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
  display.print(F("Rotary: "));
  display.println(rotaryBehaviorLabel(selected.rotaryBehavior));
  display.println(bleConnected ? F("Click: save") : F("Click: save (offline)"));
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
    sendShortcut(action);
    playActionOkBeep();
  } else {

    playActionErrorBeep();
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
  if (uiMode == UI_NORMAL) {
    const MacroProfile& profile = kProfiles[currentProfile];
    unsigned long now = millis();
    int repeats = 1;
    if (profile.rotaryBehavior == ROTARY_BEHAVIOR_UP_DOWN && (now - lastEncoderStepMs) < ROTARY_FAST_WINDOW_MS) {
      repeats = 2;
    }
    lastEncoderStepMs = now;

    for (int i = 0; i < repeats; ++i) {
      triggerRotaryBehavior(profile.rotaryBehavior, direction, bleConnected);
    }
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
  drawNormalScreen(bleKeyboard.isConnected());
  Serial.println(F("Setup complete."));
}

void loop() {
  bool bleConnected = bleKeyboard.isConnected();
  if (bleConnected && !lastBleConnected) {
    Serial.println(F("BLE connected"));
    playBleConnectedBeep();
    drawNormalScreen(bleKeyboard.isConnected());
  } else if (!bleConnected && lastBleConnected) {
    Serial.println(F("BLE disconnected"));
    playBleDisconnectedBeep();
    drawNormalScreen(bleKeyboard.isConnected());
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

  delay(1);
}

/*
Pin notes:
- ESP32-C3 profile and ESP32 profile are selected at compile time.
- Rotary switch and buttons are active LOW with INPUT_PULLUP.
- If your ESP32-C3 SuperMini exposes different preferred pins, only update
  the constants in the board profile section near the top of this file.
*/