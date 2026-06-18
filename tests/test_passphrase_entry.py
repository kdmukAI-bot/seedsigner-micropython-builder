"""Test: BIP39 passphrase entry screen.

Copy this file to the ESP32 device and run it from the MicroPython REPL:
    import test_passphrase_entry

Tap the on-screen keyboard to type, then the in-grid OK key to confirm
(or the top-nav back button to cancel).
"""

import time
import seedsigner_lvgl

seedsigner_lvgl.init()

# Show the passphrase entry screen. The screen-side C++ validates this config.
seedsigner_lvgl.seed_add_passphrase_screen({
    "top_nav": {
        "title": "Enter Passphrase",
        "show_back_button": True,
        "show_power_button": False,
    },
})

# One poll loop sees both the confirmed text (OK) and a back-button press.
print("Passphrase screen running — type, then OK to confirm (or Back to cancel)...")
while True:
    result = seedsigner_lvgl.poll_for_result()
    if result is not None:
        kind, index, label = result
        if kind == "text_entered":
            print(f"Passphrase entered: {label!r}")
            break
        elif kind == "button_selected":
            # index 1000 == RET_CODE__BACK_BUTTON
            print(f"Cancelled / button: index=0x{index:08x}, label={label!r}")
            break
    time.sleep_ms(10)

seedsigner_lvgl.clear_result_queue()
print("Done.")
