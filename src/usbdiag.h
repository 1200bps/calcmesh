// DEBUG ONLY: USB state logged to the SD card (usbdiag.c).
#pragma once

void usbdiag_loop(void); // once per main loop pass
void usbdiag_task(void);
void usbdiag_print(int lines);
