// SPDX-License-Identifier: MIT
#pragma once

#include <Arduino.h>

// A single active touch point, as reported by the GT911 controller.
// Mirrors the (x, y, area) tuples returned by gt911.py's `touches` property.
struct TouchPoint {
    uint16_t x;
    uint16_t y;
    uint16_t size;
};
