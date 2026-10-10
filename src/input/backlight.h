// Keeps the screen and keyboard backlight levels across power cycles.
//
// The keyboard chip runs the backlights and forgets their levels whenever it
// loses power, so the levels live in the client settings (sd/settings.h,
// saved on the SD card): any change, from the console, the UI or the
// keyboard's own brightness keys, is saved, and the saved levels are put
// back at boot, when the settings are loaded from a card, and when the
// keyboard chip comes back after losing power.
#pragma once

// After sd_init() (which loads the settings) and kbd_init().
void backlight_init(void);
// Every main loop pass, after kbd_task().
void backlight_task(void);
