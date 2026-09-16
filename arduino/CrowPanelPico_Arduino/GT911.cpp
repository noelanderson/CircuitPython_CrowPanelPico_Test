// SPDX-License-Identifier: MIT
#include "GT911.h"

// GT911 register map (16-bit addresses, transmitted big-endian) - mirrors
// the constants at the top of gt911.py.
static const uint16_t REG_COMMAND = 0x8040;
static const uint16_t REG_CONFIG_START = 0x8047;
static const uint16_t REG_X_OUTPUT_MAX_LOW = 0x8048;
static const uint16_t REG_X_OUTPUT_MAX_HIGH = 0x8049;
static const uint16_t REG_Y_OUTPUT_MAX_LOW = 0x804A;
static const uint16_t REG_Y_OUTPUT_MAX_HIGH = 0x804B;
static const uint16_t REG_CONFIG_CHKSUM = 0x80FF;
static const uint16_t REG_CONFIG_FRESH = 0x8100;
static const uint16_t REG_PRODUCT_ID = 0x8140;
static const uint16_t REG_POINT_STATUS = 0x814E;
static const uint16_t REG_POINT_START = 0x814F;

static const size_t REG_CONFIG_SIZE = REG_CONFIG_FRESH - REG_CONFIG_START; // 185 bytes

// Static member definition backing the single-instance pointer declared in
// GT911.h; starts unset until attachTouchInterrupt() runs.
GT911 *GT911::_activeInstance = nullptr;

/**************************************************************************/
/*!
  @brief  GT911 constructor. Stores the bus/pin/resolution configuration;
          performs no I/O until begin() is called.
  @param  wire      I2C bus the controller is attached to.
  @param  address   I2C address to use.
  @param  resetPin  GPIO connected to GT911 RESET, or -1 if not wired.
  @param  intPin    GPIO connected to GT911 INT, or -1 if not wired.
  @param  width     Target screen width.
  @param  height    Target screen height.
*/
/**************************************************************************/
GT911::GT911(TwoWire &wire, uint8_t address, int resetPin, int intPin,
             uint16_t width, uint16_t height)
    : _wire(wire), _address(address), _resetPin(resetPin), _intPin(intPin),
      _width(width), _height(height) {}

/**************************************************************************/
/*!
  @brief  Reset the controller, verify/update its configured resolution,
          and put it into coordinate reading mode. Call once from setup(),
          after Wire.begin().
*/
/**************************************************************************/
bool GT911::begin(bool updateConfig) {
    _consecutiveI2cFailures = 0;
    _irqFlag = false;
    performReset();
    bool configReady = !updateConfig || checkConfig();
    if (!write8(REG_COMMAND, 0x00)) {
        return false;
    }
    if (!configReady) {
        Serial.println(
            "GT911 configuration update failed; continuing with stored configuration");
    }
    recordI2cSuccess();
    return true;
}

/**************************************************************************/
/*!
  @brief   Read and format the controller's product/firmware/vendor
           identification block.
  @return  Human-readable string, e.g. "Product ID: GT911 Version: ...".
*/
/**************************************************************************/
String GT911::productId() {
    uint8_t data[11];
    uint8_t configData[1];
    if (!readRegister(REG_PRODUCT_ID, data, sizeof(data)) ||
        !readRegister(REG_CONFIG_START, configData, sizeof(configData))) {
        return String("Unavailable (I2C read failed)");
    }
    recordI2cSuccess();

    char name[5] = {0};
    memcpy(name, data, 4);
    uint16_t version = (data[5] << 8) | data[4];
    uint16_t xResolution = (data[7] << 8) | data[6];
    uint16_t yResolution = (data[9] << 8) | data[8];
    uint8_t vendorId = data[10];

    char configVersion[6];
    if (configData[0] >= 32 && configData[0] <= 126) {
        snprintf(configVersion, sizeof(configVersion), "%c", (char)configData[0]);
    } else {
        snprintf(configVersion, sizeof(configVersion), "\\x%02x", configData[0]);
    }

    char out[128];
    snprintf(out, sizeof(out),
             "Product ID: %s Version: %04x Vendor: %02x Size: %ux%u Config: %s",
             name, version, vendorId, xResolution, yResolution, configVersion);
    return String(out);
}

/**************************************************************************/
/*!
  @brief  Read the resolution currently stored in the controller's
          configuration block (as opposed to the width/height passed to
          the constructor, which checkConfig() may still need to push
          into that block).
  @param  outWidth   Set to the configured X resolution.
  @param  outHeight  Set to the configured Y resolution.
*/
/**************************************************************************/
bool GT911::configuredResolution(uint16_t &outWidth, uint16_t &outHeight) {
    uint8_t data[10];
    if (!readRegister(REG_PRODUCT_ID, data, sizeof(data))) {
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    outWidth = (data[7] << 8) | data[6];
    outHeight = (data[9] << 8) | data[8];
    recordI2cSuccess();
    return true;
}

/**************************************************************************/
/*!
  @brief   Read and acknowledge one fresh GT911 touch frame.
  @param   outPoints     Array to fill, capacity maxPoints.
  @param   maxPoints     Size of outPoints.
  @param   outTouchCount Set to the number of touches in the frame.
  @return  True if a fresh frame was available, false otherwise.
*/
/**************************************************************************/
bool GT911::readTouches(TouchPoint *outPoints, uint8_t maxPoints,
                        uint8_t &outTouchCount) {
    outTouchCount = 0;
    uint8_t touchStatus = 0;
    if (!readRegister(REG_POINT_STATUS, &touchStatus, 1)) {
        return false;
    }

    if (!(touchStatus & 0x80)) {
        recordI2cSuccess();
        return false;
    }

    outTouchCount = touchStatus & 0x0F;
    if (outTouchCount > maxPoints) {
        outTouchCount = maxPoints;
    }
    for (uint8_t i = 0; i < outTouchCount; i++) {
        uint8_t coordinateData[8];
        if (!readRegister(REG_POINT_START + i * 8, coordinateData,
                          sizeof(coordinateData))) {
            write8(REG_POINT_STATUS, 0x00);
            outTouchCount = 0;
            return false;
        }
        // Bytes 1-2: X, 3-4: Y, 5-6: size (all little-endian). Byte 0 is
        // the touch track ID, which this port does not need.
        outPoints[i].x = coordinateData[1] | (coordinateData[2] << 8);
        outPoints[i].y = coordinateData[3] | (coordinateData[4] << 8);
        outPoints[i].size = coordinateData[5] | (coordinateData[6] << 8);
    }

    if (!write8(REG_POINT_STATUS, 0x00)) {
        outTouchCount = 0;
        return false;
    }
    recordI2cSuccess();
    return true;
}

/**************************************************************************/
/*!
  @brief  Read the controller's configuration block and, if its stored
          resolution doesn't match the constructor's width/height,
          rewrite the resolution bytes, recompute the checksum, write the
          block back, and tell the controller to reload it.
*/
/**************************************************************************/
bool GT911::checkConfig() {
    uint8_t configBuffer[REG_CONFIG_SIZE];
    if (!readRegister(REG_CONFIG_START, configBuffer, REG_CONFIG_SIZE)) {
        Serial.println("GT911 configuration read failed; update skipped");
        return false;
    }
    delay(500); // Allow device time to stabilize after config read

    const size_t xLowOffset = REG_X_OUTPUT_MAX_LOW - REG_CONFIG_START;
    const size_t xHighOffset = REG_X_OUTPUT_MAX_HIGH - REG_CONFIG_START;
    const size_t yLowOffset = REG_Y_OUTPUT_MAX_LOW - REG_CONFIG_START;
    const size_t yHighOffset = REG_Y_OUTPUT_MAX_HIGH - REG_CONFIG_START;

    uint16_t currentWidth = (configBuffer[xHighOffset] << 8) | configBuffer[xLowOffset];
    uint16_t currentHeight = (configBuffer[yHighOffset] << 8) | configBuffer[yLowOffset];

    if (currentWidth != _width || currentHeight != _height) {
        Serial.printf("Updating GT911 resolution to %u x %u\n", _width, _height);

        configBuffer[xLowOffset] = _width & 0xFF;
        configBuffer[xHighOffset] = (_width >> 8) & 0xFF;
        configBuffer[yLowOffset] = _height & 0xFF;
        configBuffer[yHighOffset] = (_height >> 8) & 0xFF;

        configBuffer[REG_CONFIG_CHKSUM - REG_CONFIG_START] = checksum(configBuffer, REG_CONFIG_SIZE);

        if (!writeBytes(REG_CONFIG_START, configBuffer, REG_CONFIG_SIZE)) {
            return false;
        }

        if (!write8(REG_CONFIG_FRESH, 0x01)) {
            return false;
        }
        delay(1000);

        uint8_t verifyBuffer[REG_CONFIG_SIZE];
        if (!readRegister(REG_CONFIG_START, verifyBuffer, REG_CONFIG_SIZE)) {
            Serial.println("GT911 configuration verification read failed");
            return false;
        }

        uint16_t verifyWidth =
            (verifyBuffer[xHighOffset] << 8) | verifyBuffer[xLowOffset];
        uint16_t verifyHeight =
            (verifyBuffer[yHighOffset] << 8) | verifyBuffer[yLowOffset];
        uint8_t verifyChecksum = checksum(verifyBuffer, REG_CONFIG_SIZE);
        bool completeBlockMatches =
            memcmp(configBuffer, verifyBuffer, REG_CONFIG_SIZE) == 0;
        if (!completeBlockMatches) {
            Serial.println(
                "GT911 normalized configuration bytes while reloading");
        }
        if (verifyWidth != _width || verifyHeight != _height ||
            verifyBuffer[REG_CONFIG_CHKSUM - REG_CONFIG_START] != verifyChecksum) {
            Serial.printf(
                "GT911 configuration verification failed: resolution %ux%u, "
                "checksum stored 0x%02X calculated 0x%02X\n",
                verifyWidth, verifyHeight,
                verifyBuffer[REG_CONFIG_CHKSUM - REG_CONFIG_START],
                verifyChecksum);
            return false;
        }
    }
    return true;
}

/**************************************************************************/
/*!
  @brief   Two's-complement checksum over the config block, excluding the
           checksum byte itself: (sum of all other bytes) + checksum == 0
           (mod 256).
  @param   buffer  Config block; the last byte is treated as the checksum
                    slot and is not included in the sum.
  @param   length  Size of buffer.
  @return  Checksum byte to store in that last slot.
*/
/**************************************************************************/
uint8_t GT911::checksum(const uint8_t *buffer, size_t length) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length - 1; i++) { // Skip the checksum byte itself
        sum += buffer[i];
    }
    return (uint8_t)(~sum + 1);
}

/**************************************************************************/
/*!
  @brief  Hardware reset sequence, also used to select the controller's
          I2C address (via the INT pin's level while reset is released -
          this driver always selects the default 0x5D address). A no-op
          beyond configuring the INT pin as an input if no reset pin was
          supplied.
*/
/**************************************************************************/
void GT911::performReset() {
    if (_resetPin < 0) {
        if (_intPin >= 0) {
            pinMode(_intPin, INPUT);
        }
        return;
    }

    pinMode(_resetPin, OUTPUT);
    digitalWrite(_resetPin, HIGH); // Reset deasserted

    if (_intPin >= 0) {
        pinMode(_intPin, OUTPUT);
        // Brief reset pulse before the main reset, per GT911 datasheet.
        digitalWrite(_resetPin, LOW);
        delay(5);
        digitalWrite(_resetPin, HIGH);
    }

    digitalWrite(_resetPin, LOW); // Assert reset (halt device)
    delay(10);

    if (_intPin >= 0) {
        // LOW during reset release selects the default 0x5D I2C address.
        digitalWrite(_intPin, LOW);
        delayMicroseconds(100);
    }

    digitalWrite(_resetPin, HIGH); // Release reset (start device)

    if (_intPin >= 0) {
        delay(5);
        pinMode(_intPin, INPUT);
    }
}

/**************************************************************************/
/*!
  @brief  Read length bytes starting at reg via a write-then-read I2C
          transaction (write the 16-bit big-endian register address,
          repeated start, read back).
  @param  reg     16-bit register address.
  @param  buffer  Destination, capacity length; cleared before reading.
  @param  length  Number of bytes to read.
  @return True only after a complete transfer.
*/
/**************************************************************************/
bool GT911::readRegister(uint16_t reg, uint8_t *buffer, size_t length) {
    memset(buffer, 0, length);
    _wire.beginTransmission(_address);
    _wire.write((uint8_t)(reg >> 8));
    _wire.write((uint8_t)(reg & 0xFF));
    uint8_t addressResult = _wire.endTransmission(false);
    if (addressResult != 0) {
        if (recordI2cFailure()) {
            Serial.printf(
                "GT911 I2C address write failed at 0x%04X: status %u "
                "(consecutive %u, total %lu)\n",
                reg, addressResult, _consecutiveI2cFailures,
                (unsigned long)_totalI2cFailures);
        }
        return false;
    }

    size_t received = _wire.requestFrom((int)_address, (int)length);
    for (size_t i = 0; i < received && i < length; i++) {
        buffer[i] = _wire.read();
    }
    if (received != length) {
        if (recordI2cFailure()) {
            Serial.printf(
                "GT911 I2C short read at 0x%04X: received %u/%u bytes "
                "(consecutive %u, total %lu)\n",
                reg, (unsigned int)received, (unsigned int)length,
                _consecutiveI2cFailures, (unsigned long)_totalI2cFailures);
        }
        return false;
    }
    return true;
}

/**************************************************************************/
/*!
  @brief  Write a single byte to reg (register address followed by one
          data byte in a single I2C transaction).
  @param  reg   16-bit register address.
  @param  data  Byte to write.
  @return True when the transaction succeeds.
*/
/**************************************************************************/
bool GT911::write8(uint16_t reg, uint8_t data) {
    _wire.beginTransmission(_address);
    _wire.write((uint8_t)(reg >> 8));
    _wire.write((uint8_t)(reg & 0xFF));
    _wire.write(data);
    uint8_t result = _wire.endTransmission();
    if (result != 0) {
        if (recordI2cFailure()) {
            Serial.printf(
                "GT911 I2C write failed at 0x%04X: status %u "
                "(consecutive %u, total %lu)\n",
                reg, result, _consecutiveI2cFailures,
                (unsigned long)_totalI2cFailures);
        }
        return false;
    }
    return true;
}

/**************************************************************************/
/*!
  @brief  Write length bytes starting at reg in one transaction, matching
          gt911.py. The RP2040 Wire buffer is 256 bytes, which accommodates
          the 185-byte GT911 configuration plus its register address.
  @param  reg     16-bit starting register address.
  @param  data    Bytes to write.
  @param  length  Number of bytes in data.
  @return True when all bytes are buffered and transmitted successfully.
*/
/**************************************************************************/
bool GT911::writeBytes(uint16_t reg, const uint8_t *data, size_t length) {
    _wire.beginTransmission(_address);
    _wire.write((uint8_t)(reg >> 8));
    _wire.write((uint8_t)(reg & 0xFF));
    size_t written = _wire.write(data, length);
    uint8_t result = _wire.endTransmission();
    if (written != length || result != 0) {
        if (recordI2cFailure()) {
            Serial.printf(
                "GT911 config write failed: wrote %u/%u bytes, I2C status %u "
                "(consecutive %u, total %lu)\n",
                (unsigned int)written, (unsigned int)length, result,
                _consecutiveI2cFailures, (unsigned long)_totalI2cFailures);
        }
        return false;
    }
    return true;
}

/**************************************************************************/
bool GT911::recordI2cFailure() {
    if (_consecutiveI2cFailures < UINT8_MAX) {
        _consecutiveI2cFailures++;
    }
    _totalI2cFailures++;

    unsigned long now = millis();
    if (_totalI2cFailures == 1 ||
        (now - _lastI2cErrorLogTime) >= 5000) {
        _lastI2cErrorLogTime = now;
        return true;
    }
    return false;
}

/**************************************************************************/
void GT911::recordI2cSuccess() {
    if (_consecutiveI2cFailures > 0) {
        Serial.printf("GT911 I2C communication restored after %u failures\n",
                      _consecutiveI2cFailures);
        _consecutiveI2cFailures = 0;
    }
}

/**************************************************************************/
/*!
  @brief  Register this instance as the active one and attach a CHANGE
          interrupt on the INT pin. A no-op if no intPin was supplied.
*/
/**************************************************************************/
void GT911::attachTouchInterrupt() {
    if (_intPin < 0) {
        return;
    }
    _activeInstance = this;
    pinMode(_intPin, INPUT);
    // CHANGE (rather than a specific edge) catches whichever trigger
    // polarity the controller's configuration happens to use, at the cost
    // of the odd spurious wake - cheap, since the ISR only sets a flag.
    attachInterrupt(digitalPinToInterrupt(_intPin), isrTrampoline, CHANGE);
}

/**************************************************************************/
/*!
  @brief  Detach the interrupt attached by attachTouchInterrupt() and
          clear this instance's active-instance registration if it holds
          it.
*/
/**************************************************************************/
void GT911::detachTouchInterrupt() {
    if (_intPin < 0) {
        return;
    }
    detachInterrupt(digitalPinToInterrupt(_intPin));
    if (_activeInstance == this) {
        _activeInstance = nullptr;
    }
}

/**************************************************************************/
/*!
  @brief  ISR attached by attachTouchInterrupt(). Kept minimal per Arduino
          ISR conventions (no I2C/Serial/etc. here) - just flags that
          dataReady() should report new data on next check.
*/
/**************************************************************************/
void GT911::isrTrampoline() {
    if (_activeInstance) {
        _activeInstance->_irqFlag = true;
    }
}

/**************************************************************************/
/*!
  @brief   True if the interrupt has fired since the last call; clears
           the flag atomically so a fresh interrupt during the check
           isn't lost.
  @return  True once per interrupt occurrence, false otherwise.
*/
/**************************************************************************/
bool GT911::dataReady() {
    noInterrupts();
    bool ready = _irqFlag;
    _irqFlag = false;
    interrupts();
    return ready;
}
