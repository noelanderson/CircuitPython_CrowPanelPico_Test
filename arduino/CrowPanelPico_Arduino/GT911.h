// SPDX-License-Identifier: MIT
/*!
 * @file GT911.h
 *
 * Arduino/Wire port of gt911.py - driver for the Goodix GT911 capacitive
 * touch controller used on the CrowPanel Pico Display.
 */
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "TouchPoint.h"

/*! Default GT911 I2C address (used when the INT pin is held low during
    reset). This is the only address this port configures for - the
    CrowPanel Pico Display's touch controller is wired for this address. */
static const uint8_t GT911_DEFAULT_I2C_ADDR = 0x5D;

/*!
  @brief  Driver for the Goodix GT911 capacitive touch controller, talking
          to it over Wire using the same register protocol as gt911.py.
*/
class GT911 {
public:
    /*!
      @brief  GT911 constructor. Stores configuration; performs no I/O
              until begin() is called.
      @param  wire     I2C bus the controller is attached to (already
                        configured with Wire.setSDA()/setSCL() and begun).
      @param  address  I2C address (GT911_DEFAULT_I2C_ADDR unless the
                        board wires INT high during reset).
      @param  resetPin GPIO connected to GT911 RESET, or -1 if not wired.
      @param  intPin   GPIO connected to GT911 INT, or -1 if not wired.
      @param  width    Target screen width, used to (re)configure the
                        touch controller's resolution.
      @param  height   Target screen height.
    */
    GT911(TwoWire &wire, uint8_t address, int resetPin, int intPin,
          uint16_t width, uint16_t height);

    /*!
      @brief  Perform the hardware reset sequence, verify/update the touch
              controller's configured resolution, and switch it into
              coordinate reading mode. Call once from setup(), after
              Wire.begin().
      @param  updateConfig  Verify and update the stored configuration
                            during startup; disable for runtime recovery.
      @return True when the controller accepts coordinate-reading mode.
    */
    bool begin(bool updateConfig = true);

    /*!
      @brief   Read and format the controller's product/firmware/vendor
               identification block.
      @return  Human-readable identification string.
    */
    String productId();

    /*!
      @brief  Read the resolution currently stored in the controller's
              configuration block.
      @param  outWidth   Set to the configured X resolution.
      @param  outHeight  Set to the configured Y resolution.
      @return True when the resolution registers are read successfully.
    */
    bool configuredResolution(uint16_t &outWidth, uint16_t &outHeight);

    /*!
      @brief   Read a fresh touch frame without treating "not ready" as a
               zero-touch release.
      @param   outPoints     Array to fill, capacity maxPoints.
      @param   maxPoints     Size of outPoints.
      @param   outTouchCount Set to the number of touches in the fresh frame.
      @return  True when a fresh frame was read, including a valid release
               frame containing zero touches; false when no frame was ready.
    */
    bool readTouches(TouchPoint *outPoints, uint8_t maxPoints,
                     uint8_t &outTouchCount);

    /*!
      @brief  Attach a GPIO interrupt on the INT pin so new touch data can
              be noticed as soon as the controller signals it, instead of
              only ever finding out on the next scheduled poll. Call after
              begin(). A no-op if this instance wasn't given an intPin.

              The GT911's INT trigger mode (edge/level, and which
              polarity) is config-dependent and this driver does not
              force a specific mode, so dataReady() is a *hint*, not a
              guarantee - callers should still poll at a modest fallback
              interval in case a particular module's trigger
              configuration doesn't produce an edge this pin change
              interrupt catches (e.g. on touch release).
    */
    void attachTouchInterrupt();

    /*!
      @brief  Detach the interrupt attached by attachTouchInterrupt();
              dataReady() reverts to always returning false. A no-op if
              no intPin was supplied.
    */
    void detachTouchInterrupt();

    /*!
      @brief   True if the interrupt has fired since the last call (and
               clears the flag). Always false if attachTouchInterrupt()
               was never called.
      @return  True once per interrupt occurrence, false otherwise.
    */
    bool dataReady();

    /*! Number of consecutive failed I2C operations since the last
        successful high-level controller operation. */
    uint8_t consecutiveI2cFailures() const { return _consecutiveI2cFailures; }

private:
    /*!
      @brief  Hardware reset sequence, also used to select the
              controller's I2C address (via the INT pin's level while
              reset is released - this driver always selects the default
              0x5D address). A no-op beyond configuring the INT pin as an
              input if no reset pin was supplied.
    */
    void performReset();

    /*!
      @brief  Read the controller's configuration block and, if its
              stored resolution doesn't match the constructor's
              width/height, rewrite the resolution bytes, recompute the
              checksum, write the block back, and tell the controller to
              reload it.
    */
    bool checkConfig();

    /*!
      @brief   Two's-complement checksum over the config block, excluding
               the checksum byte itself: (sum of all other bytes) +
               checksum == 0 (mod 256).
      @param   buffer  Config block; the last byte is treated as the
                        checksum slot and is not included in the sum.
      @param   length  Size of buffer.
      @return  Checksum byte to store in that last slot.
    */
    uint8_t checksum(const uint8_t *buffer, size_t length);

    /*!
      @brief  Read length bytes starting at reg via a write-then-read I2C
              transaction (write the 16-bit big-endian register address,
              repeated start, read back).
      @param  reg     16-bit register address.
      @param  buffer  Destination, capacity length; cleared before reading.
      @param  length  Number of bytes to read.
      @return True only when the register address and every requested byte
              were transferred successfully.
    */
    bool readRegister(uint16_t reg, uint8_t *buffer, size_t length);

    /*!
      @brief  Write a single byte to reg (register address followed by
              one data byte in a single I2C transaction).
      @param  reg   16-bit register address.
      @param  data  Byte to write.
      @return True when the I2C transaction succeeds.
    */
    bool write8(uint16_t reg, uint8_t data);

    /*!
      @brief  Write length bytes starting at reg in one transaction.
      @param  reg     16-bit starting register address.
      @param  data    Bytes to write.
      @param  length  Number of bytes in data.
      @return True when all bytes are buffered and transmitted successfully.
    */
    bool writeBytes(uint16_t reg, const uint8_t *data, size_t length);

    //! Record an I2C failure and return true when it should be logged.
    bool recordI2cFailure();

    //! Clear the consecutive failure count after a complete operation.
    void recordI2cSuccess();

    /*!
      @brief  ISR attached by attachTouchInterrupt(). Kept minimal per
              Arduino ISR conventions (no I2C/Serial/etc. here) - just
              flags that dataReady() should report new data on next check.
    */
    static void isrTrampoline();

    TwoWire &_wire;
    uint8_t _address;
    int _resetPin;
    int _intPin;
    uint16_t _width;
    uint16_t _height;
    volatile bool _irqFlag = false;
    uint8_t _consecutiveI2cFailures = 0;
    uint32_t _totalI2cFailures = 0;
    unsigned long _lastI2cErrorLogTime = 0;

    /*! attachInterrupt() needs a plain function pointer, so only one
        GT911 instance can have its interrupt attached at a time. Fine for
        this hardware, which has exactly one touch controller. */
    static GT911 *_activeInstance;
};
