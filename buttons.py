import gc
import displayio
import adafruit_ticks
from micropython import const
from buzzer import Buzzer


class Button:
    """A memory-optimized touch-enabled button widget with multiple visual states and optional latching behavior.

    This class creates an 80x80 pixel button that can detect touch events and display
    different visual states based on user interaction and status indicators. Each button
    uses a tile grid to efficiently switch between different bitmap states without
    reloading graphics.

    Memory Optimizations:
    - Uses __slots__ to prevent __dict__ creation (~200+ bytes saved per instance)
    - Calculates touch boundaries on-demand instead of storing them (~16 bytes saved)
    - Uses if/elif chains instead of dispatch tables (~150+ bytes saved per call)
    - Direct icon updates eliminate method call overhead
    - Minimal instance variables with efficient state management

    Features:
    - Touch detection with configurable debouncing
    - Multiple visual states (normal, pressed, indicator variants)
    - Latching mode for toggle-style buttons with indicator state
    - Optional buzzer feedback on touch confirmation
    - State machine with proper debouncing to prevent false triggers

    State Machine:
    Non-latching buttons: NORMAL ↔ PRESSED → DEBOUNCED → NORMAL (press detected and released)
    Latching buttons: Add INDICATOR states for persistent on/off toggle behavior
    - NORMAL → PRESSED → DEBOUNCED → INDICATOR (first press, toggles on) → INDICATOR_PRESSED → INDICATOR_DEBOUNCED → NORMAL (second press, toggles off)
    """

    # Button state constants for touch handling
    STATE_NORMAL = const(0)                   # Button idle, no touch detected
    STATE_PRESSED = const(1)                  # Touch detected, waiting for debounce confirmation
    STATE_DEBOUNCED = const(15)               # Touch confirmed, waiting for release to register press
    STATE_INDICATOR = const(2)                # Latching mode: button in "on" state with indicator active
    STATE_INDICATOR_PRESSED = const(3)        # Latching mode: indicator active and being touched
    STATE_INDICATOR_DEBOUNCED = const(35)     # Latching mode: touch confirmed while in indicator state

    # Tile grid indices for different visual states
    ICON_NORMAL = const(0)                    # Normal state: unpressed, no indicator
    ICON_PRESSED = const(1)                   # Pressed state: being touched (any mode)
    ICON_INDICATOR = const(2)                 # Indicator state: unpressed with status indicator
    ICON_INDICATOR_PRESSED = const(3)         # Indicator state: being touched with status indicator

    BUTTON_SIZE = const(80)  # Size of the button in pixels (80x80)

    # Use __slots__ to reduce memory overhead per instance
    # Only allows these specific attributes, preventing __dict__ creation
    __slots__ = ('_x', '_y', '_name', '_latching', '_buzzer', '_state', '_is_touched', '_last_touch_time', '_debounce_delay', 'icon')

    def __init__(self, x:int, y:int, group:displayio.Group, name:str, latching:bool=None, buzzer: Buzzer = None, debounce_delay:int=150):
        """Initialize a button widget at the specified screen coordinates.

        Creates a touch-sensitive button with configurable latching behavior and visual
        feedback. The button automatically loads its graphics from bitmap files based
        on the provided name.

        :param x: X coordinate of button's top-left corner (pixels)
        :param y: Y coordinate of button's top-left corner (pixels)
        :param group: DisplayIO group to add this button's graphics to
        :param name: Button identifier used for bitmap loading (e.g., "mic" loads "/image/mic.bmp")
        :param latching: True for toggle behavior with indicator state, False for momentary, None for non-latching
        :param buzzer: Buzzer instance for audio feedback on touch confirmation (optional)
        :param debounce_delay: Touch debounce delay in milliseconds (default: 150ms)

        Graphics Loading:
        - Loads a 4-tile bitmap from "/image/{name}.bmp"
        - Tile 0: Normal unpressed state
        - Tile 1: Pressed state (any mode)
        - Tile 2: Unpressed with status indicator (latching mode)
        - Tile 3: Pressed with status indicator (latching mode)
        """
        # Store position coordinates (boundaries calculated on-demand to save memory)
        self._x = x
        self._y = y

        # Button properties
        self._name = name
        self._latching = latching
        self._buzzer = buzzer

        # Touch state management
        self._state = Button.STATE_NORMAL
        self._is_touched = False  # Current touch state
        self._last_touch_time = 0
        self._debounce_delay = debounce_delay

        gc.collect()  # Free memory before graphics loading

        # Load button graphics as a tile grid for efficient state switching
        # Renamed from tile_grid to icon for clarity and memory optimization
        bitmap = displayio.OnDiskBitmap(f"/image/{name}.bmp")
        self.icon = displayio.TileGrid(bitmap, pixel_shader=bitmap.pixel_shader, tile_width=Button.BUTTON_SIZE, tile_height=Button.BUTTON_SIZE)
        self.icon.x = x
        self.icon.y = y
        group.append(self.icon)



    def _check_touch(self, touches: list[tuple]) -> bool:
        """Check if any touch point intersects with this button's boundaries.

        :param touches: List of touch points as (x, y, area) tuples
        :return: True if button is being touched, False otherwise
        """
        # Calculate boundaries on-demand to save memory
        x_max = self._x + Button.BUTTON_SIZE
        y_max = self._y + Button.BUTTON_SIZE
        
        for x, y, a in touches:  # Unpack directly in loop
            if (self._x <= x <= x_max and self._y <= y <= y_max):
                return True
        return False

    def is_pressed(self, touches: list[tuple]) -> bool:
        """Process touch input and return True if button press is confirmed.

        Implements a state machine for reliable touch detection with debouncing.
        Handles both momentary and latching button behaviors:

        Momentary buttons (latching=False/None):
        NORMAL → PRESSED → DEBOUNCED → NORMAL (returns True)

        Latching buttons (latching=True):
        NORMAL → PRESSED → DEBOUNCED → INDICATOR (first press, returns True)
        INDICATOR → INDICATOR_PRESSED → INDICATOR_DEBOUNCED → NORMAL (second press, returns True)

        Visual feedback and optional buzzer feedback are provided when touch is confirmed.

        :param touches: List of touch points as (x, y, area) tuples from touch controller
        :return: True if button press is confirmed (touch released after debounce), False otherwise
        """
        self._is_touched = self._check_touch(touches)
        time_since_last_touch = adafruit_ticks.ticks_diff(adafruit_ticks.ticks_ms(), self._last_touch_time)

        # Use optimized if/elif chain instead of dispatch table to save memory
        # Avoids creating a dictionary with function references on every call
        if self._state == Button.STATE_NORMAL:
            return self._handle_normal_state()
        elif self._state == Button.STATE_PRESSED:
            return self._handle_pressed_state(time_since_last_touch)
        elif self._state == Button.STATE_DEBOUNCED:
            return self._handle_debounced_state()
        elif self._state == Button.STATE_INDICATOR:
            return self._handle_indicator_state()
        elif self._state == Button.STATE_INDICATOR_PRESSED:
            return self._handle_indicator_pressed_state(time_since_last_touch)
        elif self._state == Button.STATE_INDICATOR_DEBOUNCED:
            return self._handle_indicator_debounced_state()
        return False

    def _handle_normal_state(self) -> bool:
        """Handle STATE_NORMAL: Button idle, waiting for initial touch."""
        if self._is_touched:
            self._last_touch_time = adafruit_ticks.ticks_ms()
            self._state = Button.STATE_PRESSED
        return False

    def _handle_pressed_state(self, time_since_last_touch: int) -> bool:
        """Handle STATE_PRESSED: Touch detected, verifying it's not a false trigger.
        
        :param time_since_last_touch: Time elapsed since touch started (milliseconds)
        """
        if self._is_touched:
            if time_since_last_touch > self._debounce_delay:
                # Touch confirmed after debounce period
                self._state = Button.STATE_DEBOUNCED
                self.icon[0] = Button.ICON_PRESSED  # Direct icon update for memory efficiency
                if self._buzzer:
                    self._buzzer.play_tone(1760, 2)  # Inline buzzer code to eliminate method call overhead
        else:
            # Touch released too early - return to normal
            self._state = Button.STATE_NORMAL
            self.icon[0] = Button.ICON_NORMAL
        return False

    def _handle_debounced_state(self) -> bool:
        """Handle STATE_DEBOUNCED: Touch confirmed, waiting for release to complete press."""
        if not self._is_touched:
            # Touch released - button press confirmed!
            if self._latching:
                self._state = Button.STATE_INDICATOR
                self.icon[0] = Button.ICON_INDICATOR  # Direct icon update
            else:
                self._state = Button.STATE_NORMAL
                self.icon[0] = Button.ICON_NORMAL
            return True
        return False

    def _handle_indicator_state(self) -> bool:
        """Handle STATE_INDICATOR: Latching button in 'on' state, waiting for touch to turn off."""
        if self._is_touched:
            self._last_touch_time = adafruit_ticks.ticks_ms()
            self._state = Button.STATE_INDICATOR_PRESSED
        return False

    def _handle_indicator_pressed_state(self, time_since_last_touch: int) -> bool:
        """Handle STATE_INDICATOR_PRESSED: Indicator active, touch detected, verifying debounce.
        
        :param time_since_last_touch: Time elapsed since touch started (milliseconds)
        """
        if self._is_touched:
            if time_since_last_touch > self._debounce_delay:
                # Touch confirmed after debounce period
                self._state = Button.STATE_INDICATOR_DEBOUNCED
                self.icon[0] = Button.ICON_INDICATOR_PRESSED  # Direct icon update
                if self._buzzer:
                    self._buzzer.play_tone(1760, 2)  # Inline buzzer code
        else:
            # Touch released too early - return to indicator state
            self._state = Button.STATE_INDICATOR
            self.icon[0] = Button.ICON_INDICATOR
        return False

    def _handle_indicator_debounced_state(self) -> bool:
        """Handle STATE_INDICATOR_DEBOUNCED: Indicator touch confirmed, waiting for release to turn off."""
        if not self._is_touched:
            # Touch released - button press confirmed, turn off latching button
            self._state = Button.STATE_NORMAL
            self.icon[0] = Button.ICON_NORMAL  # Direct icon update
            return True
        return False


    @property
    def indicator(self) -> bool:
        """Get the current indicator state for latching buttons.

        Returns True if the button is currently in any indicator state (on/active).
        For non-latching buttons, this will always return False.

        :return: True if button is in indicator state (latching mode "on"), False otherwise
        """
        return self._state in (Button.STATE_INDICATOR, Button.STATE_INDICATOR_PRESSED, Button.STATE_INDICATOR_DEBOUNCED)

    @indicator.setter
    def indicator(self, state: bool) -> None:
        """Set the indicator state for latching buttons.

        Allows programmatic control of the button's indicator state. Only works
        for buttons configured with latching=True. Visual state is updated accordingly.

        :param state: True to activate indicator (turn "on"), False to deactivate (turn "off")
        """
        if self._latching:
            if state:
                self._state = Button.STATE_INDICATOR
                self.icon[0] = Button.ICON_INDICATOR  # Direct icon update for efficiency
            else:
                self._state = Button.STATE_NORMAL
                self.icon[0] = Button.ICON_NORMAL

    @property
    def latching(self) -> bool:
        """Get whether this button uses latching behavior.

        :return: True if button latches (toggle mode with indicator state), False for momentary mode
        """
        return self._latching

    @property
    def name(self) -> str:
        """Get the button's identifier name.

        :return: Button name used for bitmap loading and identification
        """
        return self._name

