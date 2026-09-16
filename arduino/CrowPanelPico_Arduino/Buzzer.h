// SPDX-License-Identifier: MIT
/*!
 * @file Buzzer.h
 *
 * Arduino port of buzzer.py - PWM tone playback for touch feedback and
 * startup diagnostics. Uses
 * the core's tone()/noTone(), which drive a hardware PWM slice on RP2040
 * the same way pwmio did in CircuitPython.
 *
 * Unlike buzzer.py's play_tone() (which blocks for the whole duration via
 * time.sleep()), this is non-blocking: playTone() starts the tone and
 * returns immediately, and update() - called once per main loop iteration -
 * stops it once the duration has elapsed.
 */
#pragma once

#include <Arduino.h>

/*!
  @brief  Non-blocking PWM tone playback for touch feedback and diagnostics.
*/
class Buzzer {
public:
    /*!
      @brief  Buzzer constructor.
      @param  pin  GPIO connected to the buzzer/speaker.
    */
    explicit Buzzer(uint8_t pin);

    /*!
      @brief  Start a square-wave tone at frequencyHz, to be automatically
              stopped after durationMs milliseconds by update(). Does not
              block.
      @param  frequencyHz  Tone frequency, in Hz.
      @param  durationMs   How long the tone should play, in milliseconds.
    */
    void playTone(unsigned int frequencyHz, unsigned long durationMs);

    /*!
      @brief  Immediately silence the buzzer.
    */
    void stopTone();

    /*!
      @brief  Call once per loop() iteration. Stops the tone once its
              duration has elapsed; a no-op otherwise.
    */
    void update();

    /*!
      @brief   Whether a tone is currently playing.
      @return  True if playing, false otherwise.
    */
    bool isPlaying() const { return _isPlaying; }

private:
    uint8_t _pin;
    bool _isPlaying;
    unsigned long _toneStartTime;
    unsigned long _toneDuration;
};
