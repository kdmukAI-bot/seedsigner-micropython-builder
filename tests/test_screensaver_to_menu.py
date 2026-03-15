"""Test: screensaver → dismiss via tap → main menu.

Copy this file to the ESP32 device and run it from the MicroPython REPL:
    import test_screensaver_to_menu
"""

import time
import display_manager
import seedsigner_lvgl

display_manager.init()

# Show the screensaver (bouncing logo).
seedsigner_lvgl.screensaver_screen()

# Poll until the user taps the screen to dismiss.
print("Screensaver running — tap screen to dismiss...")
while True:
    result = seedsigner_lvgl.poll_for_result()
    if result is not None:
        kind, index, label = result
        print(f"Dismissed: {kind}, index=0x{index:08x}, label={label!r}")
        break
    time.sleep_ms(10)

seedsigner_lvgl.clear_result_queue()

# Now show the main menu.
print("Loading main menu...")
seedsigner_lvgl.main_menu_screen()
