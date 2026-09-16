// SPDX-License-Identifier: MIT
#include "Buzzer.h"

/**************************************************************************/
/*!
  @brief  Buzzer constructor. Configures the pin as a digital output; no
          touch-feedback or diagnostic tone plays until playTone().
  @param  pin  GPIO connected to the buzzer/speaker.
*/
/**************************************************************************/
Buzzer::Buzzer(uint8_t pin)
    : _pin(pin), _isPlaying(false), _toneStartTime(0), _toneDuration(0) {
    pinMode(_pin, OUTPUT);
}

/**************************************************************************/
/*!
  @brief  Start a square wave at frequencyHz via the core's tone(), and
          record when it should be stopped. Does not block; see update().
  @param  frequencyHz  Tone frequency, in Hz.
  @param  durationMs   How long the tone should play, in milliseconds.
*/
/**************************************************************************/
void Buzzer::playTone(unsigned int frequencyHz, unsigned long durationMs) {
    tone(_pin, frequencyHz);
    _isPlaying = true;
    _toneStartTime = millis();
    _toneDuration = durationMs;
}

/**************************************************************************/
/*!
  @brief  Call once per loop() iteration. Stops the current tone once its
          duration (set by playTone()) has elapsed; a no-op otherwise.
*/
/**************************************************************************/
void Buzzer::update() {
    // Unsigned subtraction wraps around safely even if millis() overflows.
    if (_isPlaying && (millis() - _toneStartTime) >= _toneDuration) {
        stopTone();
    }
}

/**************************************************************************/
/*!
  @brief  Immediately silence the buzzer via noTone().
*/
/**************************************************************************/
void Buzzer::stopTone() {
    noTone(_pin);
    _isPlaying = false;
}

