// SPDX-License-Identifier: MIT
/*!
 * @file Button.h
 *
 * Arduino port of buttons.py - a touch-enabled button widget with multiple
 * visual states and optional latching behaviour, rendered as an 80x80
 * pixel region of the DVI framebuffer.
 *
 * State machine (identical to the CircuitPython original):
 *   Non-latching: NORMAL -> PRESSED -> DEBOUNCED -> NORMAL      (press detected on release)
 *   Latching:     NORMAL -> PRESSED -> DEBOUNCED -> INDICATOR (on)
 *                 INDICATOR -> INDICATOR_PRESSED -> INDICATOR_DEBOUNCED -> NORMAL (off)
 */
#pragma once

#include <Arduino.h>
#include <PicoDVI.h>
#include <functional>

#include "Buzzer.h"
#include "TouchPoint.h"

class Button;

/*! Callback invoked when a press is confirmed (release after debounce).
    Takes the button itself so one handler can serve several buttons and
    still query name()/latching()/indicator(). */
using ButtonCallback = std::function<void(Button &)>;

/*!
  @brief  A touch-enabled button widget with debounced state tracking and
          optional latching (on/off indicator) behaviour.
*/
class Button {
public:
    static const int SIZE = 80; //!< Button width/height in pixels, matches image tiles

    /*!
      @brief  Button constructor.
      @param  display          Framebuffer to draw the button's tiles into.
      @param  x                Screen X position of the button's top-left corner.
      @param  y                Screen Y position of the button's top-left corner.
      @param  tileData         Source pixel data - a horizontal strip of
                                80x80 RGB565 tiles, as produced by
                                tools/convert_images.py.
      @param  tileCount        Number of 80x80 tiles in tileData (2 for
                                momentary buttons, 4 for latching buttons).
      @param  name              Identifier used for logging.
      @param  latching         True for toggle behaviour with indicator
                                state, false for momentary.
      @param  buzzer           Optional buzzer for audio feedback on touch
                                confirmation.
      @param  debounceDelayMs  Touch debounce delay in milliseconds.
    */
    Button(DVIGFX16 &display, int x, int y, const uint16_t *tileData, uint8_t tileCount,
           const char *name, bool latching, Buzzer *buzzer = nullptr,
           unsigned long debounceDelayMs = 150);

    /*!
      @brief   Process touch input, updating visuals/state and firing
               onPress() if set. Call once per main loop iteration for
               every button.
      @param   touches     Currently active touch points.
      @param   touchCount  Number of entries in touches.
      @return  True if a button press was confirmed this call (touch
               released after debounce) - the same event onPress() fires for.
    */
    bool isPressed(const TouchPoint *touches, uint8_t touchCount);

    /*!
      @brief  Register a handler to run when a press is confirmed, instead
              of checking isPressed()'s return value in the caller.
              Replaces any previously registered handler.
      @param  callback  Handler to invoke on a confirmed press.
    */
    void onPress(ButtonCallback callback) { _onPress = std::move(callback); }

    /*!
      @brief   True if a latching button is currently "on".
      @return  True if in any indicator state, false otherwise (always
               false for non-latching buttons).
    */
    bool indicator() const;

    /*!
      @brief  Programmatically set a latching button's indicator state.
      @param  state  True to turn the indicator on, false to turn it off.
    */
    void setIndicator(bool state);

    /*!
      @brief   Whether this button uses latching (toggle) behaviour.
      @return  True for latching, false for momentary.
    */
    bool latching() const { return _latching; }

    /*!
      @brief   This button's identifier, as given to the constructor.
      @return  The button's name.
    */
    const char *name() const { return _name; }

private:
    enum State {
        STATE_NORMAL,
        STATE_PRESSED,
        STATE_DEBOUNCED,
        STATE_INDICATOR,
        STATE_INDICATOR_PRESSED,
        STATE_INDICATOR_DEBOUNCED,
    };

    //! Tile indices within tileData, matching the layout used by buttons.py.
    enum Icon {
        ICON_NORMAL = 0,
        ICON_PRESSED = 1,
        ICON_INDICATOR = 2,
        ICON_INDICATOR_PRESSED = 3,
    };

    /*!
      @brief   True if any touch point falls within this button's 80x80
               screen-space bounding box.
      @param   touches     Currently active touch points.
      @param   touchCount  Number of entries in touches.
      @return  True if touched, false otherwise.
    */
    bool checkTouch(const TouchPoint *touches, uint8_t touchCount) const;

    /*!
      @brief  Blit one 80x80 tile from tileData directly into the
              display's framebuffer at this button's screen position.
      @param  tileIndex  Which tile to draw (see the Icon enum).
    */
    void drawTile(uint8_t tileIndex);

    //! STATE_NORMAL: button idle, waiting for an initial touch.
    bool handleNormalState();

    /*!
      @brief   STATE_PRESSED: touch detected, verifying it's not a false
               trigger.
      @param   timeSinceLastTouch  Milliseconds elapsed since the touch
                                    started.
      @return  Always false (a press is only confirmed on release).
    */
    bool handlePressedState(unsigned long timeSinceLastTouch);

    /*!
      @brief   STATE_DEBOUNCED: touch confirmed, waiting for release to
               complete the press.
      @return  True if the press was just confirmed (touch released).
    */
    bool handleDebouncedState();

    //! STATE_INDICATOR: latching button "on", waiting for a touch to begin turning it off.
    bool handleIndicatorState();

    /*!
      @brief   STATE_INDICATOR_PRESSED: indicator active and being
               touched, verifying debounce before confirming the "turn
               off" press.
      @param   timeSinceLastTouch  Milliseconds elapsed since the touch
                                    started.
      @return  Always false (a press is only confirmed on release).
    */
    bool handleIndicatorPressedState(unsigned long timeSinceLastTouch);

    /*!
      @brief   STATE_INDICATOR_DEBOUNCED: indicator touch confirmed,
               waiting for release to turn the latching button off.
      @return  True if the press was just confirmed (touch released).
    */
    bool handleIndicatorDebouncedState();

    DVIGFX16 &_display;
    int _x;
    int _y;
    const uint16_t *_tileData;
    uint8_t _tileCount;
    const char *_name;
    bool _latching;
    Buzzer *_buzzer;
    unsigned long _debounceDelay;
    ButtonCallback _onPress;

    State _state;
    bool _isTouched;
    unsigned long _lastTouchTime;
};

