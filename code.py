# Hardware and System Imports
import gc
import time
import board
import digitalio


# Display and Graphics Imports
import picodvi
import displayio
import framebufferio

# Touch and Communication Imports
import gt911
import busio


# Local Module Imports
from buttons import Button
from buzzer import Buzzer

# Audio Setup
buzzer = Buzzer(board.GP19)

# Display Backlight Control
backlight_PWM = digitalio.DigitalInOut(board.GP24)
backlight_PWM.direction = digitalio.Direction.OUTPUT
backlight_PWM.value = False  # Set low (0V) for backlight off

# Touch Controller Setup
i2c = busio.I2C(board.GP21, board.GP20)  # SCL, SDA pins for I2C
gt = gt911.GT911(i2c, reset_pin=board.GP29, int_pin=board.GP25, width=320, height=240)

print("Touch Controller Initialized")
print(gt.product_id)

# Display Setup
displayio.release_displays()
frame_buffer = picodvi.Framebuffer(320, 240,
               clk_dp=board.GP9, clk_dn=board.GP8,
               red_dp=board.GP11, red_dn=board.GP10,
               green_dp=board.GP13, green_dn=board.GP12,
               blue_dp=board.GP15, blue_dn=board.GP14,
               color_depth=8)

display = framebufferio.FramebufferDisplay(frame_buffer)
group = displayio.Group()
display.root_group = group

# Button layout configuration (3x2 grid)
button_config = [
    # Row 0 - Latching buttons (status indicators)
    {"name": "panda",    "latching": True},
    {"name": "pig",      "latching": True},
    {"name": "deer",     "latching": False},
    {"name": "tiger",    "latching": False},
    {"name": "elephant", "latching": False},
    {"name": "fox",      "latching": False},

]

# Create button objects from configuration (3x2 grid: 3 rows, 2 columns)
buttons = []
for i, config in enumerate(button_config):
    row = i // 2  # Integer division for row (every 2 buttons = new row)
    col = i % 2   # Modulo for column (0, 1 within each row)
    x = col * 80
    y = row * 80
    button = Button(x, y, group, config["name"], config["latching"], buzzer=buzzer)
    buttons.append(button)
    gc.collect()
    print( "Available memory: {} bytes".format(gc.mem_free()) )

# MAIN LOOP
while True:
    gc.collect()  # Run garbage collection to free up memory

    # Get current touch points from the touch controller
    touches = gt.touches
    for button in buttons:
        pressed = button.is_pressed(touches)
        if pressed:
            print(f"Button {button.name} pressed")

    time.sleep(0.05)  # 20Hz update rate for responsive touch detection
