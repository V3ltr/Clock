#include <Arduino.h>
#include <MD_MAX72xx.h>
#include <WiFi.h>
#include <time.h>

const char WIFI_SSID[] = "Muffin";
const char WIFI_PASSWORD[] = "coolninja";

const char TZ_INFO[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";

constexpr uint8_t MATRIX_DATA_PIN = 23; 
constexpr uint8_t MATRIX_CLK_PIN  = 18;  
constexpr uint8_t MATRIX_CS_PIN   = 5;   
constexpr uint8_t ENCODER_A_PIN  = 26;  
constexpr uint8_t ENCODER_B_PIN  = 12; 
constexpr uint8_t ALARM_BUTTON_PIN = 27; 
constexpr uint8_t MATRIX_MODULES = 4;
constexpr MD_MAX72XX::moduleType_t MATRIX_HARDWARE = MD_MAX72XX::FC16_HW;
constexpr uint8_t MATRIX_INTENSITY = 2; 
constexpr bool REVERSE_ENCODER_DIRECTION = false;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t ALARM_DURATION_MS = 60000UL;

MD_MAX72XX matrix(MATRIX_HARDWARE, MATRIX_DATA_PIN, MATRIX_CLK_PIN,
                  MATRIX_CS_PIN, MATRIX_MODULES);

enum class UiMode : uint8_t { Clock, SetAlarm };
UiMode uiMode = UiMode::Clock;
bool alarmArmed = false;
bool alarmRinging = false;
uint8_t alarmHour = 0;
uint8_t alarmMinute = 0;

uint32_t alarmStartedAt = 0;
uint32_t lastAlarmFlashPhase = UINT32_MAX;
int64_t lastAlarmMinute = -1;

bool buttonRawLast = HIGH;
bool buttonStable = HIGH;
uint32_t buttonChangedAt = 0;
uint8_t previousEncoderState = 0;
int8_t encoderQuarterSteps = 0;

bool displayDirty = true;
bool hadValidTime = false;
int lastDisplayedSecond = -1;
uint32_t lastIndicatorHalfSecond = UINT32_MAX;

const uint8_t DIGIT_GLYPHS[10][5] = {
  {0b111, 0b101, 0b101, 0b101, 0b111}, 
  {0b010, 0b110, 0b010, 0b010, 0b111}, 
  {0b111, 0b001, 0b111, 0b100, 0b111}, 
  {0b111, 0b001, 0b111, 0b001, 0b111}, 
  {0b101, 0b101, 0b111, 0b001, 0b001}, 
  {0b111, 0b100, 0b111, 0b001, 0b111}, 
  {0b111, 0b100, 0b111, 0b101, 0b111}, 
  {0b111, 0b001, 0b001, 0b001, 0b001}, 
  {0b111, 0b101, 0b111, 0b101, 0b111}, 
  {0b111, 0b101, 0b111, 0b001, 0b111}  
};
const uint8_t GLYPH_A[5]     = {0b010, 0b101, 0b111, 0b101, 0b101};
const uint8_t GLYPH_L[5]     = {0b100, 0b100, 0b100, 0b100, 0b111};
const uint8_t GLYPH_MINUS[5] = {0b000, 0b000, 0b111, 0b000, 0b000};

bool getClockTime(time_t &epoch, struct tm &localTime) {
  epoch = time(nullptr);
  if (epoch < 1700000000) return false; 
  localtime_r(&epoch, &localTime);
  return true;
}

uint8_t glyphWidth(char c) {
  return (c == ':' || c == ' ') ? 1 : 3;
}

int16_t textWidth(const char *text) {
  int16_t width = 0;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    width += glyphWidth(text[i]);
    if (text[i + 1] != '\0') ++width;
  }
  return width;
}

const uint8_t *glyphRows(char c) {
  if (c >= '0' && c <= '9') return DIGIT_GLYPHS[c - '0'];
  if (c == 'A') return GLYPH_A;
  if (c == 'L') return GLYPH_L;
  if (c == '-') return GLYPH_MINUS;
  return nullptr;
}

void drawText(const char *text, bool showColon) {
  matrix.clear();
  int16_t x = (static_cast<int16_t>(matrix.getColumnCount()) - textWidth(text)) / 2;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    const char c = text[i];
    if (c == ':') {
      if (showColon) {
        matrix.setPoint(2, x, true);
        matrix.setPoint(4, x, true);
      }
    } else if (c != ' ') {
      const uint8_t *rows = glyphRows(c);
      if (rows != nullptr) {
        for (uint8_t row = 0; row < 5; ++row) {
          for (uint8_t col = 0; col < 3; ++col) {
            if (rows[row] & (1U << (2 - col))) {
              matrix.setPoint(row + 1, x + col, true);
            }
          }
        }
      }
    }
    x += glyphWidth(c);
    if (text[i + 1] != '\0') ++x;
  }
}

void renderDisplay(bool timeValid, const struct tm &localTime) {
  char text[12];
  bool colonOn = false;
  if (uiMode == UiMode::SetAlarm) {
    snprintf(text, sizeof(text), "A %02u:%02u", alarmHour, alarmMinute);
    colonOn = (millis() / 1000UL) % 2 == 0;
  } else if (timeValid) {
    snprintf(text, sizeof(text), "%02d:%02d", localTime.tm_hour, localTime.tm_min);
    colonOn = (localTime.tm_sec % 2) == 0;
  } else {
    snprintf(text, sizeof(text), "--:--");
  }

  drawText(text, colonOn);
  if (alarmArmed) {
    const bool indicatorOn = ((millis() / 500UL) & 1U) == 0;
    matrix.setPoint(3, 31, indicatorOn);
  }
}

void fillDisplay(bool on) {
  if (!on) {
    matrix.clear();
    return;
  }
  for (uint16_t column = 0; column < matrix.getColumnCount(); ++column) {
    matrix.setColumn(column, 0xFF);
  }
}

void startAlarm(int64_t minuteSerial) {
  alarmRinging = true;
  alarmStartedAt = millis();
  lastAlarmMinute = minuteSerial;
  lastAlarmFlashPhase = 0;
  fillDisplay(true);
  Serial.println("Alarm ringing: full display flashes for up to 60 seconds.");
}

void cancelOrFinishAlarm() {
  alarmRinging = false;
  alarmArmed = false;
  fillDisplay(false);
  displayDirty = true;
  Serial.println("Alarm stopped; it is no longer armed.");
}

void onButtonPress(const struct tm &localTime, bool timeValid) {
  if (alarmRinging) return; 

  if (alarmArmed) {
    alarmArmed = false;
    uiMode = UiMode::Clock;
    displayDirty = true;
    Serial.println("Alarm cancelled.");
    return;
  }

  if (uiMode == UiMode::SetAlarm) {
    uiMode = UiMode::Clock;
    alarmArmed = true;
    displayDirty = true;
    Serial.printf("Alarm armed for %02u:%02u.\n", alarmHour, alarmMinute);
    return;
  }

  uiMode = UiMode::SetAlarm;
  alarmHour = timeValid ? static_cast<uint8_t>(localTime.tm_hour) : 0;
  alarmMinute = timeValid ? static_cast<uint8_t>(localTime.tm_min) : 0;
  displayDirty = true;
  Serial.printf("Set alarm: %02u:%02u\n", alarmHour, alarmMinute);
}

void readButton(uint32_t nowMs, const struct tm &localTime, bool timeValid) {
  const bool raw = digitalRead(ALARM_BUTTON_PIN);
  if (raw != buttonRawLast) {
    buttonRawLast = raw;
    buttonChangedAt = nowMs;
  }
  if ((nowMs - buttonChangedAt) >= BUTTON_DEBOUNCE_MS && raw != buttonStable) {
    buttonStable = raw;
    if (buttonStable == LOW) onButtonPress(localTime, timeValid);
  }
}

void applyEncoderStep(int8_t signedStep) {
  if (uiMode != UiMode::SetAlarm) return;
  const bool positive = (signedStep > 0) != REVERSE_ENCODER_DIRECTION;
  if (positive) {
    alarmMinute = static_cast<uint8_t>((alarmMinute + 1) % 60); 
  } else {
    alarmHour = static_cast<uint8_t>((alarmHour + 1) % 24);
  }
  displayDirty = true;
}

void readEncoder() {
  const uint8_t a = digitalRead(ENCODER_A_PIN) ? 1 : 0;
  const uint8_t b = digitalRead(ENCODER_B_PIN) ? 1 : 0;
  const uint8_t current = static_cast<uint8_t>((a << 1) | b);
  static const int8_t transitions[16] = {
     0,  1, -1,  0,
    -1,  0,  0,  1,
     1,  0,  0, -1,
     0, -1,  1,  0
  };
  const uint8_t index = static_cast<uint8_t>((previousEncoderState << 2) | current);
  encoderQuarterSteps += transitions[index];
  previousEncoderState = current;

  if (encoderQuarterSteps >= 4) {
    encoderQuarterSteps = 0;
    applyEncoderStep(1);
  } else if (encoderQuarterSteps <= -4) {
    encoderQuarterSteps = 0;
    applyEncoderStep(-1);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(ALARM_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);
  previousEncoderState = static_cast<uint8_t>(
      ((digitalRead(ENCODER_A_PIN) ? 1 : 0) << 1) |
      (digitalRead(ENCODER_B_PIN) ? 1 : 0));

  matrix.begin();
  matrix.control(MD_MAX72XX::INTENSITY, MATRIX_INTENSITY);
  matrix.clear();

  if (WIFI_SSID[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
    Serial.println("Wi-Fi started; waiting for network time synchronization.");
  } else {
    Serial.println("Set WIFI_SSID and WIFI_PASSWORD in the sketch to enable NTP time.");
  }
  Serial.println("Clock ready. Button: set/arm/cancel; encoder: adjust while setting.");
}

void loop() {
  const uint32_t nowMs = millis();
  time_t epoch = 0;
  struct tm localTime = {};
  const bool timeValid = getClockTime(epoch, localTime);

  readButton(nowMs, localTime, timeValid);
  readEncoder();

  if (alarmRinging) {
    const uint32_t elapsed = nowMs - alarmStartedAt;
    if (elapsed >= ALARM_DURATION_MS) {
      cancelOrFinishAlarm();
    } else {
      const uint32_t phase = elapsed / 1000UL;
      if (phase != lastAlarmFlashPhase) {
        fillDisplay((phase & 1U) == 0); 
        lastAlarmFlashPhase = phase;
      }
    }
    return;
  }

  if (alarmArmed && timeValid && localTime.tm_sec <= 1) {
    const int64_t minuteSerial = static_cast<int64_t>(epoch) / 60;
    if (minuteSerial != lastAlarmMinute &&
        localTime.tm_hour == alarmHour && localTime.tm_min == alarmMinute) {
      startAlarm(minuteSerial);
      return;
    }
  }

  if (timeValid != hadValidTime || (timeValid && localTime.tm_sec != lastDisplayedSecond)) {
    displayDirty = true;
    hadValidTime = timeValid;
    lastDisplayedSecond = timeValid ? localTime.tm_sec : -1;
  }
  const uint32_t halfSecond = nowMs / 500UL;
  if (alarmArmed && halfSecond != lastIndicatorHalfSecond) {
    lastIndicatorHalfSecond = halfSecond;
    displayDirty = true;
  }
  if (displayDirty) {
    renderDisplay(timeValid, localTime);
    displayDirty = false;
  }
}
