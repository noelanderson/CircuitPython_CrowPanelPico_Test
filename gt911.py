import struct
import time
import microcontroller
from digitalio import DigitalInOut, Direction, DriveMode
from micropython import const
from adafruit_bus_device.i2c_device import I2CDevice

try:
    from types import TracebackType
    from typing import NoReturn, Optional, Type

    from busio import I2C
except ImportError:
    pass

# GT911 I2C Device Addresses
_GT911_DEFAULT_I2C_ADDR = 0x5D    # Primary I2C address (INT pin low during reset)
_GT911_SECONDARY_I2C_ADDR = 0x14  # Secondary I2C address (INT pin high during reset)

# GT911 Register Map (16-bit addresses, big-endian transmission)
_REG_COMMAND = const(0x8040)           # Device command register (read/write)
_REG_CONFIG_START = const(0x8047)      # Configuration data block start address
_REG_X_OUTPUT_MAX_LOW = const(0x8048)  # X resolution low byte in config
_REG_X_OUTPUT_MAX_HIGH = const(0x8049) # X resolution high byte in config
_REG_Y_OUTPUT_MAX_LOW = const(0x804A)  # Y resolution low byte in config
_REG_Y_OUTPUT_MAX_HIGH = const(0x804B) # Y resolution high byte in config
_REG_CONFIG_CHKSUM = const(0x80FF)     # Configuration checksum byte
_REG_CONFIG_FRESH = const(0x8100)      # Configuration update flag register

_REG_PRODUCT_ID = const(0x8140)        # Product identification data (11 bytes)
_REG_POINT_STATUS = const(0x814E)      # Touch status and count register
_REG_POINT_START = const(0x814F)       # First touch point coordinate data

# Configuration constants
_REG_CONFIG_SIZE = const(_REG_CONFIG_FRESH - _REG_CONFIG_START)  # Configuration block size (185 bytes)


class GT911:
    """Driver for Goodix GT911 capacitive touch controller.

    The GT911 is a multi-touch capacitive touch controller commonly used in
    embedded displays, tablets, and industrial touch panels. This driver provides
    a comprehensive interface for touch detection, device configuration, and
    hardware control.

    Key Features:
    - Up to 5 simultaneous touch points with coordinate and pressure data
    - Configurable screen resolution and touch sensitivity
    - Hardware reset and I2C address configuration via control pins
    - Automatic configuration management with checksum validation
    - Support for both polling and interrupt-driven touch detection

    Hardware Requirements:
    - I2C bus connection (SCL/SDA)
    - Optional: RESET pin for hardware reset and address configuration
    - Optional: INT pin for interrupt signaling and address configuration

    I2C Address Configuration:
    The GT911 supports two I2C addresses selectable via the INT pin state during reset:
    - 0x5D (default): INT pin held low during reset
    - 0x14 (secondary): INT pin held high during reset

    :param i2c: I2C bus interface for device communication
    :type i2c: I2C
    :param address: Override I2C address. If None, determined by pin configuration
    :type address: int, optional
    :param width: Target screen width in pixels for touch coordinate scaling
    :type width: int, optional
    :param height: Target screen height in pixels for touch coordinate scaling
    :type height: int, optional
    :param reset_pin: GPIO pin connected to GT911 RESET line
    :type reset_pin: microcontroller.Pin, optional
    :param int_pin: GPIO pin connected to GT911 INT line
    :type int_pin: microcontroller.Pin, optional
    :param use_secondary_i2c_address: Configure for 0x14 address (True) or 0x5D (False)
    :type use_secondary_i2c_address: bool, optional

    :raises OSError: If I2C communication fails during initialization
    :raises ValueError: If invalid resolution or address parameters provided

    Example Usage:
        >>> import busio
        >>> import board
        >>> from gt911 import GT911
        >>>
        >>> # Basic setup with I2C only
        >>> i2c = busio.I2C(board.SCL, board.SDA)
        >>> touch = GT911(i2c, width=800, height=480)
        >>>
        >>> # Setup with hardware control pins
        >>> touch = GT911(i2c, width=800, height=480,
        ...               reset_pin=board.GP15, int_pin=board.GP16)
        >>>
        >>> # Read device information
        >>> print(touch.product_id)
        >>>
        >>> # Monitor for touch events
        >>> while True:
        ...     touches = touch.touches
        ...     for x, y, size in touches:
        ...         print(f"Touch at ({x}, {y}) size {size}")
        ...     time.sleep(0.1)
    """

    # pylint: disable=too-many-arguments
    def __init__(
        self,
        i2c: I2C,
        address: int = None,
        width: int  = 320,
        height: int  = 240,
        reset_pin: Optional[microcontroller.Pin] = None,
        int_pin: Optional[microcontroller.Pin] = None,
        use_secondary_i2c_address: bool = False
    ):
        self._width = width
        self._height = height

        # Initialize pin objects if provided
        self._reset = DigitalInOut(reset_pin) if reset_pin is not None else None
        self._interrupt = DigitalInOut(int_pin) if int_pin is not None else None

        # Determine I2C address based on provided address or pin configuration
        if address is None:
            if self._reset and self._interrupt and use_secondary_i2c_address:
                address = _GT911_SECONDARY_I2C_ADDR
            else:
                address = _GT911_DEFAULT_I2C_ADDR

        # Perform hardware reset sequence if reset pin is available
        self._perform_reset(use_secondary_i2c_address)

        # Initialize I2C communication with the GT911 device
        self.i2c_device = I2CDevice(i2c, address)

        self._check_config()

        # print(f"GT911 initialized with I2C address: {hex(address)}")

        # Set device to coordinate reading mode
        self._write_8(_REG_COMMAND, 0x00)


    @property
    def product_id(self) -> str:
        """Get GT911 device identification information.

        Reads the product identification register block to extract device metadata.
        This information is useful for device verification and debugging.

        Register layout (11 bytes starting at 0x8140):
        - Bytes 0-3: 4-character ASCII product identifier
        - Bytes 4-5: 16-bit firmware version (little-endian)
        - Bytes 6-7: X resolution from device config (little-endian)
        - Bytes 8-9: Y resolution from device config (little-endian)
        - Byte 10: Vendor identification code

        :return: Formatted string with device identification details
        :rtype: str

        Example:
            >>> touch = GT911(i2c)
            >>> print(touch.product_id)
            Product ID: 911 Version: 1060 Vendor: 00 Size: 800x480
        """
        # Read 11-byte product information block
        data = self._read(_REG_PRODUCT_ID, 11)
        config_data = self._read(_REG_CONFIG_START, 1)

        # Parse device information from register data
        product_name = ''.join([chr(byte) for byte in data[:4]])  # ASCII product name
        version = (data[5] << 8) | data[4]                        # Little-endian version
        x_resolution = (data[7] << 8) | data[6]                   # Device-configured X resolution
        y_resolution = (data[9] << 8) | data[8]                   # Device-configured Y resolution
        vendor_id = data[10]                                      # Vendor identification
        config_version_ascii = chr(config_data[0]) if 32 <= config_data[0] <= 126 else f"\\x{config_data[0]:02x}"

        return f"Product ID: {product_name} Version: {version:04x} Vendor: {vendor_id:02x} Size: {x_resolution}x{y_resolution} Config: {config_version_ascii}"


    @property
    def touches(self) -> list[tuple]:
        """Get current touch point data from the GT911 sensor.

        Reads the touch status register and retrieves coordinate data for all
        currently active touch points. The GT911 supports up to 5 simultaneous touches.

        Touch data processing:
        1. Check touch status register (0x814E) for data ready flag and touch count
        2. For each active touch, read 8-byte coordinate block starting at 0x814F
        3. Extract X, Y coordinates and touch size from the coordinate data
        4. Clear status register to acknowledge data read and prepare for next cycle

        Touch coordinate format (per 8-byte block):
        - Byte 0: Touch ID and status flags
        - Bytes 1-2: X coordinate (little-endian, 0-configured_width)
        - Bytes 3-4: Y coordinate (little-endian, 0-configured_height)
        - Bytes 5-6: Touch size/pressure (little-endian)
        - Byte 7: Reserved

        :return: List of active touch points as (x, y, size) tuples.
                 Empty list if no touches detected or data not ready.
        :rtype: list[tuple[int, int, int]]

        :raises OSError: If I2C communication with device fails

        Example:
            >>> touch = GT911(i2c)
            >>> current_touches = touch.touches
            >>> for x, y, size in current_touches:
            ...     print(f"Touch at ({x}, {y}) with size {size}")
        """
        # Initialize touch data storage for up to 5 simultaneous touches
        touch_data = [tuple()] * 5
        num_touch_points = 0

        # Read touch status register to check for ready data
        touch_status = self._read(_REG_POINT_STATUS, 1)[0]

        # Process touch data if ready flag (bit 7) is set
        if touch_status & 0x80:  # Touch data ready flag
            num_touch_points = touch_status & 0x0F  # Extract touch count (bits 0-3)

            # Read coordinate data for each active touch point
            for i in range(num_touch_points):
                # Each touch point uses 8 bytes of coordinate data
                coordinate_data = self._read(_REG_POINT_START + i * 8, 8)

                # Extract X, Y, and size from bytes 1-6 (skip touch ID in byte 0)
                # Format: little-endian 16-bit values for x, y, size
                touch_data[i] = struct.unpack("<HHH", coordinate_data[1:7])

        # Clear touch status register to acknowledge read and prepare for next cycle
        self._write_8(_REG_POINT_STATUS, 0x00)

        # Return only the active touch points (slice to actual count)
        return touch_data[:num_touch_points]



    def _check_config(self) -> None:
        """Verify and update GT911 device configuration if needed.

        Reads the current device configuration to check if the configured resolution
        matches the desired width/height. If not, updates the configuration with
        the target resolution and recalculates the configuration checksum.

        Configuration process:
        1. Read 185-byte configuration block from device
        2. Extract current X/Y resolution from config bytes
        3. Compare with target resolution (self._width, self._height)
        4. If different, update resolution bytes in config buffer
        5. Recalculate and update configuration checksum
        6. Write updated configuration back to device
        7. Signal device to reload configuration

        Register layout for resolution:
        - 0x8048: X resolution low byte
        - 0x8049: X resolution high byte
        - 0x804A: Y resolution low byte
        - 0x804B: Y resolution high byte
        - 0x80FF: Configuration checksum
        - 0x8100: Configuration fresh flag
        """
        # Read complete configuration block from device (185 bytes)
        config_buffer = self._read(_REG_CONFIG_START, _REG_CONFIG_SIZE)

        # Extract currently configured resolution from config buffer
        # Resolution bytes are at specific offsets within the config block
        x_low_offset = _REG_X_OUTPUT_MAX_LOW - _REG_CONFIG_START
        x_high_offset = _REG_X_OUTPUT_MAX_HIGH - _REG_CONFIG_START
        y_low_offset = _REG_Y_OUTPUT_MAX_LOW - _REG_CONFIG_START
        y_high_offset = _REG_Y_OUTPUT_MAX_HIGH - _REG_CONFIG_START

        current_width = (config_buffer[x_high_offset] << 8) | config_buffer[x_low_offset]
        current_height = (config_buffer[y_high_offset] << 8) | config_buffer[y_low_offset]

        # print(f"GT911 current resolution: {current_width} x {current_height}")

        # Update configuration if resolution doesn't match target values
        if current_width != self._width or current_height != self._height:
            print(f"Updating GT911 resolution to {self._width} x {self._height}")

            # Update resolution values in configuration buffer (little-endian format)
            config_buffer[x_low_offset] = self._width & 0xFF           # X low byte
            config_buffer[x_high_offset] = (self._width >> 8) & 0xFF   # X high byte
            config_buffer[y_low_offset] = self._height & 0xFF          # Y low byte
            config_buffer[y_high_offset] = (self._height >> 8) & 0xFF  # Y high byte

            # Recalculate configuration checksum (sum of all config bytes except checksum)
            checksum = 0
            for i in range(_REG_CONFIG_SIZE - 1):  # Exclude checksum byte from calculation
                checksum += config_buffer[i]

            # GT911 uses two's complement checksum: (~sum + 1) & 0xFF
            checksum = ((~checksum) + 1) & 0xFF
            config_buffer[_REG_CONFIG_CHKSUM - _REG_CONFIG_START] = checksum

            # Write updated configuration to device
            self._write_bytes(_REG_CONFIG_START, config_buffer)

            # Signal device to reload configuration (requires >10ms delay)
            time.sleep(0.01)
            self._write_8(_REG_CONFIG_FRESH, 0x01)


    def _perform_reset(self, use_secondary_i2c_address: bool) -> None:
        """Execute hardware reset sequence to initialize GT911 and configure I2C address.

        Performs the GT911 reset protocol which allows configuration of the I2C address
        based on the interrupt pin state during reset. This method handles both
        cases where reset pin is available and where only interrupt pin is present.

        Reset sequence (when reset pin available):
        1. Set reset pin to output mode, initially high
        2. If interrupt pin available, pulse reset briefly to prepare INT configuration
        3. Assert reset (low) for >10ms to halt device operation
        4. Configure interrupt pin state to set desired I2C address:
           - Low (False): Device will use address 0x5D (GT911_DEFAULT_I2C_ADDR)
           - High (True): Device will use address 0x14 (GT911_SECONDARY_I2C_ADDR)
        5. Release reset (high) and wait >5ms for device startup
        6. Switch interrupt pin to input mode for normal operation

        If no reset pin: Simply configure interrupt pin for input if available.

        :param use_secondary_i2c_address: If True, configure for 0x14 address via INT pin.
                                        If False, configure for 0x5D address.
        :type use_secondary_i2c_address: bool

        Note:
            The GT911 datasheet specifies precise timing requirements for this sequence.
            Deviating from these timings may result in communication failures.
        """
        if self._reset is None:
            # No reset pin available - just configure interrupt pin if present
            if self._interrupt:
                self._interrupt.switch_to_input()  # Set up for interrupt monitoring
            return

        # Initialize reset pin for output control
        self._reset.switch_to_output(True)  # Start with reset deasserted (high)

        if self._interrupt:
            # Brief reset pulse to prepare for interrupt pin configuration
            # This ensures the device is in a known state before the main reset
            self._reset.value = False
            time.sleep(0.005)  # Wait >5ms for device to recognize reset
            self._reset.value = True

        # Main reset sequence: halt device operation
        self._reset.value = False  # Assert reset (device stopped)
        time.sleep(0.01)  # Wait >10ms as required by GT911 specification

        # Configure I2C address via interrupt pin state during reset release
        if self._interrupt:
            # Set interrupt pin to desired state using open-drain mode
            # Pin state determines I2C address: High=0x14, Low=0x5D
            self._interrupt.switch_to_output(use_secondary_i2c_address,
                                           drive_mode=DriveMode.OPEN_DRAIN)
            time.sleep(0.0001)  # Wait >10μs for pin state to stabilize

        # Release reset and complete initialization
        self._reset.value = True  # Release reset (start device)

        if self._interrupt:
            time.sleep(0.005)  # Wait >5ms for device startup completion
            self._interrupt.switch_to_input()  # Switch to interrupt monitoring mode


    def _read(self, register: int, length: int) -> bytearray:
        """Read data from GT911 register(s) using I2C write-then-read transaction.

        The GT911 uses 16-bit register addresses transmitted in big-endian format
        (high byte first). This method performs the standard I2C sequence:
        1. Write 2-byte register address to device
        2. Read specified number of data bytes from that address

        :param register: 16-bit register address to read from (0x8000-0x81FF range)
        :type register: int
        :param length: Number of consecutive bytes to read (1-255)
        :type length: int
        :return: Raw data bytes read from the device registers
        :rtype: bytearray
        :raises OSError: If I2C communication fails (device not responding, bus error, etc.)

        Example:
            >>> # Read 4 bytes starting from product ID register
            >>> data = self._read(0x8140, 4)
            >>> product_name = ''.join([chr(b) for b in data])
        """
        # Prepare 16-bit register address in big-endian format for GT911
        register_bytes = bytes([register >> 8, register & 0xFF])
        result_buffer = bytearray(length)

        # Execute I2C write-then-read transaction
        with self.i2c_device as i2c:
            i2c.write_then_readinto(register_bytes, result_buffer)

        return result_buffer

    def _write_8(self, register: int, data: int) -> None:
        """Write a single byte to a GT911 register.

        Writes one data byte to the specified GT911 register using I2C write transaction.
        The register address is transmitted as 16-bit big-endian value followed by
        the 8-bit data byte.

        Transaction format: [addr_high, addr_low, data_byte]

        :param register: 16-bit register address to write to (0x8000-0x81FF range)
        :type register: int
        :param data: Single byte value to write (0-255, will be masked to 8 bits)
        :type data: int
        :raises OSError: If I2C communication fails (device not responding, bus error, etc.)

        Example:
            >>> # Clear touch status register
            >>> self._write_8(0x814E, 0x00)
            >>> # Send device command
            >>> self._write_8(0x8040, 0x02)
        """
        # Construct 3-byte I2C write packet: [addr_high, addr_low, data]
        write_buffer = bytearray(3)
        write_buffer[0] = (register >> 8) & 0xFF  # Register address high byte
        write_buffer[1] = register & 0xFF         # Register address low byte
        write_buffer[2] = data & 0xFF             # Data byte (masked to 8 bits)

        # Execute I2C write transaction
        with self.i2c_device as i2c:
            i2c.write(bytes(write_buffer))

    def _write_bytes(self, register: int, data: bytearray) -> None:
        """Write multiple bytes to consecutive GT911 registers.

        Writes a block of data to consecutive GT911 registers starting at the
        specified address. This is commonly used for updating configuration data
        or writing large data structures to the device.

        The GT911 auto-increments the internal register pointer, so consecutive
        bytes are written to sequential register addresses.

        Transaction format: [addr_high, addr_low, data_byte_0, data_byte_1, ...]

        :param register: 16-bit starting register address (0x8000-0x81FF range)
        :type register: int
        :param data: Array of bytes to write sequentially to consecutive registers
        :type data: bytearray
        :raises OSError: If I2C communication fails (device not responding, bus error, etc.)

        Example:
            >>> # Write 4-byte configuration block
            >>> config_data = bytearray([0x01, 0x02, 0x03, 0x04])
            >>> self._write_bytes(0x8047, config_data)
        """
        # Construct I2C write packet: [addr_high, addr_low, data_bytes...]
        write_buffer = bytearray(2 + len(data))
        write_buffer[0] = (register >> 8) & 0xFF  # Register address high byte
        write_buffer[1] = register & 0xFF         # Register address low byte
        write_buffer[2:] = data                   # Sequential data bytes

        # Execute I2C write transaction
        with self.i2c_device as i2c:
            i2c.write(bytes(write_buffer))