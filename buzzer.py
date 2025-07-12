import time
import board
import pwmio
import microcontroller


class Buzzer:
    """PWM-based buzzer controller for audio feedback.
    
    This class provides simple tone generation using PWM (Pulse Width Modulation)
    to create square wave audio signals. Designed for providing user feedback
    such as button press confirmations, alerts, and status notifications.
    
    Features:
    - Variable frequency tone generation
    - Precise duration control
    - Lazy PWM initialization for resource efficiency
    - Clean resource management with proper cleanup
    - Non-blocking tone playback with automatic stopping
    
    Hardware Requirements:
    - GPIO pin connected to a buzzer or speaker
    - Compatible with passive buzzers (generates own frequency)
    - Default configuration for CrowPanel Pico (GP19)
    """
    
    def __init__(self, pin: microcontroller.Pin = board.GP19):
        """Initialize the buzzer controller with specified GPIO pin.
        
        Creates a buzzer instance with lazy PWM initialization - the actual PWM
        object is created only when first needed to conserve system resources.
        
        :param pin: GPIO pin connected to the buzzer/speaker
                   Default: board.GP19 (CrowPanel Pico Terminal buzzer pin)
                   
        Hardware Notes:
        - Use with passive buzzers that require frequency generation
        - Active buzzers may work but won't benefit from frequency control
        - Ensure adequate current capacity if driving speakers directly
        """
        self.pin = pin              # GPIO pin for buzzer connection
        self.pwm = None            # PWM instance (created lazily)
        self.is_playing = False    # Current playback state tracking
        
    def play_tone(self, frequency: int, duration_ms: int) -> None:
        """Generate and play a tone at the specified frequency and duration.
        
        Creates a square wave PWM signal at the given frequency for the specified
        duration. The method blocks for the entire duration and automatically
        stops the tone when complete.
        
        :param frequency: Tone frequency in Hz (20-20000 Hz typical range)
                         Higher frequencies = higher pitch tones
                         Common values: 440Hz (A4), 1760Hz (A6), 2000Hz (high beep)
        :param duration_ms: Playback duration in milliseconds
                           Short durations (1-50ms) for quick feedback
                           Longer durations (100-1000ms) for alerts
                           
        Technical Details:
        - Uses 50% duty cycle for optimal square wave generation
        - PWM frequency range limited by hardware capabilities
        - Blocking operation - call returns after tone completes
        
        Example Usage:
        - buzzer.play_tone(1760, 25)  # Quick high-pitched button feedback
        - buzzer.play_tone(440, 200)  # Longer notification tone
        """
        # Lazy initialization of PWM - only create when first needed
        if self.pwm is None:
            self.pwm = pwmio.PWMOut(self.pin, variable_frequency=True)
        
        # Configure PWM for square wave tone generation
        self.pwm.frequency = int(frequency)  # Set tone frequency
        self.pwm.duty_cycle = 32768         # 50% duty cycle (65535 / 2)
        self.is_playing = True              # Update state tracking
        
        # Play tone for specified duration (blocking operation)
        time.sleep(duration_ms / 1000.0)
        
        # Automatically stop tone when duration expires
        self.stop_tone()
    
    def stop_tone(self) -> None:
        """Immediately stop any currently playing tone.
        
        Silences the buzzer by setting the PWM duty cycle to 0%, effectively
        turning off the signal while keeping the PWM instance active for
        future use. This is more efficient than deinitializing the PWM.
        
        Safe to call even when no tone is playing or PWM is not initialized.
        Updates the is_playing state for accurate status tracking.
        """
        if self.pwm is not None:
            self.pwm.duty_cycle = 0  # Set to 0% duty cycle = silence
        self.is_playing = False      # Update state tracking
    
    def deinit(self) -> None:
        """Clean up PWM resources and prepare for object destruction.
        
        Properly releases the PWM hardware resources and resets the internal
        state. Should be called when the buzzer is no longer needed to free
        up system resources for other uses.
        
        After calling this method, the buzzer can still be used - the PWM
        will be automatically reinitialized on the next play_tone() call.
        
        Safe to call multiple times or when PWM is not initialized.
        """
        if self.pwm is not None:
            self.pwm.deinit()        # Release PWM hardware resources
            self.pwm = None          # Clear PWM instance reference
        self.is_playing = False      # Reset state tracking


