// SPDX-License-Identifier: MIT
//
// Arduino port of code.py for the Elecrow CrowPanel Pico Display 4.3"
// (320x240 IPS touchscreen, RP2040, true DVI/TMDS panel output on GP8-GP15).
//
// Required libraries (Arduino IDE Library Manager or manual install):
//   - Adafruit PicoDVI ("PicoDVI - Adafruit Fork", github.com/adafruit/PicoDVI)
//   - Adafruit GFX Library
// Required board package:
//   - "Raspberry Pi Pico/RP2040" by Earle Philhower (arduino-pico core)
// Board selection: Raspberry Pi Pico (or Pico W), matching the CrowPanel's
// RP2040 module. See README.md in this folder for full setup instructions.
#include <PicoDVI.h>
#include <Wire.h>

#include "Button.h"
#include "Buzzer.h"
#include "CrowPanelDVIConfig.h"
#include "GT911.h"
#include "Images.h"

#ifndef CROWPANEL_TOUCH_DEBUG
#define CROWPANEL_TOUCH_DEBUG 1
#endif

static const int SCREEN_RESOLUTION_X = 320;
static const int SCREEN_RESOLUTION_Y = 240;
// Only referenced when CROWPANEL_TOUCH_DEBUG gates the serial-wait in setup().
[[maybe_unused]] static const unsigned long SERIAL_WAIT_TIMEOUT_MS = 15000;
static const uint8_t TOUCH_RECOVERY_FAILURE_THRESHOLD = 3;
static const uint8_t TOUCH_MAX_RECOVERY_ATTEMPTS = 3;
static const unsigned long TOUCH_RECOVERY_COOLDOWN_MS = 1000;
static const unsigned long TOUCH_RECOVERY_BUDGET_RESET_MS = 60000;

// ---- Pin assignments (mirrors code.py) ----
static const uint8_t PIN_BUZZER = 19;
static const uint8_t PIN_BACKLIGHT = 24;

static const uint8_t PIN_TOUCH_SDA = 20;
static const uint8_t PIN_TOUCH_SCL = 21;
static const uint8_t PIN_TOUCH_RESET = 29;
static const uint8_t PIN_TOUCH_INT = 25;

// True DVI/TMDS output on GP8-GP15, using this board's own pin config
// (see CrowPanelDVIConfig.h for why it isn't PicoDVI's `picodvi_dvi_cfg`).
DVIGFX16 display(DVI_RES_320x240p60, crowpanel_dvi_cfg);

Buzzer buzzer(PIN_BUZZER);
GT911 touchController(Wire, GT911_DEFAULT_I2C_ADDR, PIN_TOUCH_RESET, PIN_TOUCH_INT,
                       SCREEN_RESOLUTION_X, SCREEN_RESOLUTION_Y);

// Interrupts provide low-latency reads; 20 Hz polling remains a safety net.
static const unsigned long TOUCH_POLL_INTERVAL_MS = 50;

// Same 3x2 button set as code.py, positioned on the responsive right half.
struct ButtonConfig {
    const char *name;
    const uint16_t *tileData;
    uint8_t tileCount;
    bool latching;
};

static const ButtonConfig BUTTON_CONFIG[] = {
    // Row 0 - Latching buttons (status indicators)
    {"panda", panda_data, 4, true},
    {"pig", pig_data, 4, true},
    {"deer", deer_data, 2, false},
    {"tiger", tiger_data, 2, false},
    {"elephant", elephant_data, 2, false},
    {"fox", fox_data, 2, false},
};
static const int NUM_BUTTONS = sizeof(BUTTON_CONFIG) / sizeof(BUTTON_CONFIG[0]);

Button *buttons[NUM_BUTTONS];
TouchPoint touchPoints[5]; // GT911 supports up to 5 simultaneous touches
unsigned long lastTouchPollTime = 0;
unsigned long lastTouchRecoveryTime = 0;
uint8_t touchRecoveryAttempts = 0;
bool touchInterruptAttached = false;

void playDiagnosticBeeps(uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        buzzer.playTone(1760, 100);
        while (buzzer.isPlaying()) {
            buzzer.update();
            delay(1);
        }
        delay(100);
    }
}

bool touchResolutionMatches(bool beepOnMismatch = true) {
    uint16_t configuredWidth;
    uint16_t configuredHeight;
    if (!touchController.configuredResolution(configuredWidth, configuredHeight)) {
        Serial.println("Unable to read GT911 configured resolution");
        return false;
    }

    Serial.printf("GT911 configured resolution: %u x %u\n",
                  configuredWidth, configuredHeight);
    if (configuredWidth == SCREEN_RESOLUTION_X &&
        configuredHeight == SCREEN_RESOLUTION_Y) {
        return true;
    }

    Serial.printf(
        "Touch controller resolution %ux%u does not match required "
        "resolution (%dx%d)\n",
        configuredWidth, configuredHeight,
        SCREEN_RESOLUTION_X, SCREEN_RESOLUTION_Y);
    if (beepOnMismatch) {
        playDiagnosticBeeps(5);
    }
    return false;
}

void attachTouchInterruptIfNeeded() {
    if (!touchInterruptAttached) {
        touchController.attachTouchInterrupt();
        touchInterruptAttached = true;
    }
}

void recoverTouchControllerIfNeeded() {
    if (touchController.consecutiveI2cFailures() <
            TOUCH_RECOVERY_FAILURE_THRESHOLD ||
        touchRecoveryAttempts >= TOUCH_MAX_RECOVERY_ATTEMPTS ||
    (touchRecoveryAttempts > 0 &&
     (millis() - lastTouchRecoveryTime) < TOUCH_RECOVERY_COOLDOWN_MS)) {
        return;
    }

    touchRecoveryAttempts++;
    lastTouchRecoveryTime = millis();
    Serial.printf("Recovering GT911 (attempt %u/%u)\n",
                  touchRecoveryAttempts, TOUCH_MAX_RECOVERY_ATTEMPTS);

    touchController.detachTouchInterrupt();
    touchInterruptAttached = false;
    if (touchController.begin(false)) {
        touchResolutionMatches(false);
        attachTouchInterruptIfNeeded();
        Serial.println("GT911 recovery completed");
    } else {
        Serial.println("GT911 recovery failed");
    }
}

void setup() {
    Serial.begin(115200);
#if CROWPANEL_TOUCH_DEBUG
    // Leave time to attach a terminal before touch and display diagnostics run.
    unsigned long serialWaitStart = millis();
    while (!Serial &&
           (millis() - serialWaitStart) < SERIAL_WAIT_TIMEOUT_MS) {
        delay(10);
    }
    if (Serial) {
        delay(250);
    }
#endif
    Serial.println("Starting CrowPanel initialization");
    pinMode(PIN_BACKLIGHT, OUTPUT);
    digitalWrite(PIN_BACKLIGHT, LOW); // GP24 is active-low on this panel.

    // Touch controller setup
    Wire.setSDA(PIN_TOUCH_SDA);
    Wire.setSCL(PIN_TOUCH_SCL);
    Wire.begin();
    bool touchInitialized = touchController.begin();
    if (touchInitialized) {
        Serial.print("Touch Controller Initialized: ");
        Serial.println(touchController.productId());
        touchResolutionMatches();
    } else {
        Serial.println("Touch controller initialization failed");
    }

    if (touchInitialized) {
        attachTouchInterruptIfNeeded();
    }

    // Display setup
    if (!display.begin()) {
        Serial.println("Display initialization failed");
        pinMode(LED_BUILTIN, OUTPUT);
        for (;;) {
            digitalWrite(LED_BUILTIN, (millis() / 500) & 1); // Blink LED on failure
        }
    }
    Serial.println("Display initialized");
    display.fillScreen(0x0000); // Black

    digitalWrite(PIN_BACKLIGHT, LOW); // Keep the backlight enabled after DVI setup.

    // Create the 3x2 grid in the screen's responsive right-hand 160 pixels.
    for (int i = 0; i < NUM_BUTTONS; i++) {
        int row = i / 2; // Integer division for row (every 2 buttons = new row)
        int col = i % 2; // Modulo for column (0, 1 within each row)
        int x = SCREEN_RESOLUTION_X / 2 + col * Button::SIZE;
        int y = row * Button::SIZE;
        buttons[i] = new Button(display, x, y, BUTTON_CONFIG[i].tileData, BUTTON_CONFIG[i].tileCount,
                                 BUTTON_CONFIG[i].name, BUTTON_CONFIG[i].latching, &buzzer);
        buttons[i]->onPress([](Button &button) {
            if (button.latching()) {
                Serial.printf("Button %s pressed - %s\n", button.name(),
                              button.indicator() ? "on" : "off");
            } else {
                Serial.printf("Button %s pressed\n", button.name());
            }
        });
    }
}

void loop() {
    buzzer.update(); // Non-blocking: stops the tone once its duration has elapsed

    bool interruptDue = touchController.dataReady();
    bool fallbackDue = (millis() - lastTouchPollTime) >= TOUCH_POLL_INTERVAL_MS;
    if (!interruptDue && !fallbackDue) {
        return;
    }
    lastTouchPollTime = millis();

    uint8_t touchCount;
    if (!touchController.readTouches(touchPoints, 5, touchCount)) {
        if (touchController.consecutiveI2cFailures() == 0 &&
            touchRecoveryAttempts > 0 &&
            (millis() - lastTouchRecoveryTime) >=
                TOUCH_RECOVERY_BUDGET_RESET_MS) {
            touchRecoveryAttempts = 0;
        }
        recoverTouchControllerIfNeeded();
        return;
    }
    touchRecoveryAttempts = 0;
    attachTouchInterruptIfNeeded();
#if defined(CROWPANEL_TOUCH_DEBUG) && CROWPANEL_TOUCH_DEBUG
    if (touchCount > 0) {
        Serial.printf("Touch count: %u\n", touchCount);
        for (uint8_t i = 0; i < touchCount; i++) {
            Serial.printf("  Touch %u: x=%u y=%u size=%u\n",
                          i, touchPoints[i].x, touchPoints[i].y,
                          touchPoints[i].size);
        }
    }
#endif
    for (int i = 0; i < NUM_BUTTONS; i++) {
        buttons[i]->isPressed(touchPoints, touchCount); // onPress() fires internally
    }
}
