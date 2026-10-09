// The PicoCalc's 320x320 IPS panel: an ILI9488 (or the register-compatible
// ST7365P) on spi1, write-only, 18-bit pixels. It shows the UI framebuffer
// (ui/fb.h) and nothing else.
//
// Rows go out by DMA from two row buffers that a DMA interrupt refills, so a
// flush returns at once and the main loop keeps polling the radio and the
// keyboard while the panel catches up (a full screen is ~120 ms at 21 MHz).
//
//     lcd_init();                       // once, before the first flush
//     ...
//     ui_render();
//     if (!lcd_busy())
//         fb_flush(lcd_rows, NULL);
//
// The backlight is not ours: the keyboard MCU drives LCD_BL and sets its
// level over I2C (register 0x05 on the keyboard's bus).
#pragma once

#include <stdbool.h>

// Set up spi1 and the pins, reset and configure the panel, clear it to
// black, and turn it on. Blocks for about half a second. Marks the whole
// framebuffer changed, so the next fb_flush() redraws everything.
void lcd_init(void);

// fb_flush() sink: queue rows y0..y1 (inclusive) for the panel. Starts the
// transfer if the panel is idle. Called while busy, it queues behind what is
// in flight; it only waits if the queue is full, which takes ignoring
// lcd_busy() across several flushes.
void lcd_rows(int y0, int y1, void *ctx);

// Rows are still going out.
bool lcd_busy(void);

// Wait until everything queued has reached the panel.
void lcd_wait(void);
