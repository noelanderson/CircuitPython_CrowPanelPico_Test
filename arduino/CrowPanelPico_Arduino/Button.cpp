// SPDX-License-Identifier: MIT
#include "Button.h"

#include <string.h>

/**************************************************************************/
/*!
  @brief  Button constructor. Stores configuration and renders tile 0
          (normal/unpressed) so the button is visible immediately,
          without waiting for the first isPressed() call.
  @param  display          Framebuffer to draw the button's tiles into.
  @param  x                Screen X position of the button's top-left corner.
  @param  y                Screen Y position of the button's top-left corner.
  @param  tileData         Source pixel data - a horizontal strip of 80x80
                            RGB565 tiles.
  @param  tileCount        Number of 80x80 tiles in tileData.
  @param  name             Identifier used for logging.
  @param  latching         True for toggle behaviour, false for momentary.
  @param  buzzer           Optional buzzer for audio feedback.
  @param  debounceDelayMs  Touch debounce delay in milliseconds.
*/
/**************************************************************************/
Button::Button(DVIGFX16 &display, int x, int y, const uint16_t *tileData, uint8_t tileCount,
               const char *name, bool latching, Buzzer *buzzer, unsigned long debounceDelayMs)
    : _display(display), _x(x), _y(y), _tileData(tileData), _tileCount(tileCount), _name(name),
      _latching(latching), _buzzer(buzzer), _debounceDelay(debounceDelayMs),
      _state(STATE_NORMAL), _isTouched(false), _lastTouchTime(0) {
    drawTile(ICON_NORMAL); // Render initial state
}

/**************************************************************************/
/*!
  @brief   True if any touch point in touches falls within this button's
           80x80 screen-space bounding box.
  @param   touches     Currently active touch points.
  @param   touchCount  Number of entries in touches.
  @return  True if touched, false otherwise.
*/
/**************************************************************************/
bool Button::checkTouch(const TouchPoint *touches, uint8_t touchCount) const {
    int xMax = _x + SIZE;
    int yMax = _y + SIZE;
    for (uint8_t i = 0; i < touchCount; i++) {
        if (touches[i].x >= (uint16_t)_x && touches[i].x <= (uint16_t)xMax &&
            touches[i].y >= (uint16_t)_y && touches[i].y <= (uint16_t)yMax) {
            return true;
        }
    }
    return false;
}

/**************************************************************************/
/*!
  @brief  Blit one 80x80 tile from tileData (a horizontal strip of
          tileCount tiles) directly into the display's framebuffer at
          this button's screen position, row by row.
  @param  tileIndex  Which tile to draw (see the Icon enum in Button.h).
*/
/**************************************************************************/
void Button::drawTile(uint8_t tileIndex) {
    uint16_t *buffer = _display.getBuffer();
    int displayWidth = _display.width();
    int srcRowStride = _tileCount * SIZE;
    int srcXOffset = tileIndex * SIZE;

    for (int row = 0; row < SIZE; row++) {
        const uint16_t *srcRow = _tileData + (size_t)row * srcRowStride + srcXOffset;
        uint16_t *dstRow = buffer + (size_t)(_y + row) * displayWidth + _x;
        memcpy(dstRow, srcRow, SIZE * sizeof(uint16_t));
    }
}

/**************************************************************************/
/*!
  @brief   Update the touch state, dispatch to the handler for the
           current state, and fire onPress() if a press was confirmed
           this call.
  @param   touches     Currently active touch points.
  @param   touchCount  Number of entries in touches.
  @return  True if a press was confirmed this call.
*/
/**************************************************************************/
bool Button::isPressed(const TouchPoint *touches, uint8_t touchCount) {
    _isTouched = checkTouch(touches, touchCount);
    unsigned long timeSinceLastTouch = millis() - _lastTouchTime; // Unsigned wraparound-safe

    bool pressed = false;
    switch (_state) {
        case STATE_NORMAL:
            pressed = handleNormalState();
            break;
        case STATE_PRESSED:
            pressed = handlePressedState(timeSinceLastTouch);
            break;
        case STATE_DEBOUNCED:
            pressed = handleDebouncedState();
            break;
        case STATE_INDICATOR:
            pressed = handleIndicatorState();
            break;
        case STATE_INDICATOR_PRESSED:
            pressed = handleIndicatorPressedState(timeSinceLastTouch);
            break;
        case STATE_INDICATOR_DEBOUNCED:
            pressed = handleIndicatorDebouncedState();
            break;
    }

    if (pressed && _onPress) {
        _onPress(*this);
    }
    return pressed;
}

/**************************************************************************/
/*!
  @brief   STATE_NORMAL: button idle, waiting for an initial touch.
  @return  Always false (a press is only confirmed on release).
*/
/**************************************************************************/
bool Button::handleNormalState() {
    if (_isTouched) {
        _lastTouchTime = millis();
        _state = STATE_PRESSED;
    }
    return false;
}

/**************************************************************************/
/*!
  @brief   STATE_PRESSED: touch detected, verifying it's not a false
           trigger.
  @param   timeSinceLastTouch  Milliseconds elapsed since the touch started.
  @return  Always false (a press is only confirmed on release).
*/
/**************************************************************************/
bool Button::handlePressedState(unsigned long timeSinceLastTouch) {
    if (_isTouched) {
        if (timeSinceLastTouch > _debounceDelay) {
            _state = STATE_DEBOUNCED;
            drawTile(ICON_PRESSED);
            if (_buzzer) {
                _buzzer->playTone(1760, 2);
            }
        }
    } else {
        // Touch released too early - return to normal
        _state = STATE_NORMAL;
        drawTile(ICON_NORMAL);
    }
    return false;
}

/**************************************************************************/
/*!
  @brief   STATE_DEBOUNCED: touch confirmed, waiting for release to
           complete the press.
  @return  True if the press was just confirmed (touch released).
*/
/**************************************************************************/
bool Button::handleDebouncedState() {
    if (!_isTouched) {
        // Touch released - button press confirmed!
        if (_latching) {
            _state = STATE_INDICATOR;
            drawTile(ICON_INDICATOR);
        } else {
            _state = STATE_NORMAL;
            drawTile(ICON_NORMAL);
        }
        return true;
    }
    return false;
}

/**************************************************************************/
/*!
  @brief   STATE_INDICATOR: latching button in its "on" state, waiting
           for a touch to begin turning it off.
  @return  Always false (a press is only confirmed on release).
*/
/**************************************************************************/
bool Button::handleIndicatorState() {
    if (_isTouched) {
        _lastTouchTime = millis();
        _state = STATE_INDICATOR_PRESSED;
    }
    return false;
}

/**************************************************************************/
/*!
  @brief   STATE_INDICATOR_PRESSED: indicator active and being touched,
           verifying debounce before confirming the "turn off" press.
  @param   timeSinceLastTouch  Milliseconds elapsed since the touch started.
  @return  Always false (a press is only confirmed on release).
*/
/**************************************************************************/
bool Button::handleIndicatorPressedState(unsigned long timeSinceLastTouch) {
    if (_isTouched) {
        if (timeSinceLastTouch > _debounceDelay) {
            _state = STATE_INDICATOR_DEBOUNCED;
            drawTile(ICON_INDICATOR_PRESSED);
            if (_buzzer) {
                _buzzer->playTone(1760, 2);
            }
        }
    } else {
        // Touch released too early - return to indicator state
        _state = STATE_INDICATOR;
        drawTile(ICON_INDICATOR);
    }
    return false;
}

/**************************************************************************/
/*!
  @brief   STATE_INDICATOR_DEBOUNCED: indicator touch confirmed, waiting
           for release to turn the latching button off.
  @return  True if the press was just confirmed (touch released).
*/
/**************************************************************************/
bool Button::handleIndicatorDebouncedState() {
    if (!_isTouched) {
        // Touch released - button press confirmed, turn off latching button
        _state = STATE_NORMAL;
        drawTile(ICON_NORMAL);
        return true;
    }
    return false;
}

/**************************************************************************/
/*!
  @brief   Whether a latching button is currently in any "on" state.
  @return  True if in any indicator state, false otherwise (always false
           for non-latching buttons).
*/
/**************************************************************************/
bool Button::indicator() const {
    return _state == STATE_INDICATOR || _state == STATE_INDICATOR_PRESSED ||
           _state == STATE_INDICATOR_DEBOUNCED;
}

/**************************************************************************/
/*!
  @brief  Programmatically force a latching button's indicator on/off,
          updating its visual state to match. A no-op for non-latching
          buttons.
  @param  state  True to turn the indicator on, false to turn it off.
*/
/**************************************************************************/
void Button::setIndicator(bool state) {
    if (!_latching) {
        return;
    }
    if (state) {
        _state = STATE_INDICATOR;
        drawTile(ICON_INDICATOR);
    } else {
        _state = STATE_NORMAL;
        drawTile(ICON_NORMAL);
    }
}
