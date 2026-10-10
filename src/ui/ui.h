// The calcmesh screen: a status line, the active view, and a row of five
// softkey tabs along the bottom, one per function key (Nodes, Chats,
// Position, Metrics, Settings), drawn into the framebuffer (fb.h). The
// UI reads mesh_db and the message store and is driven by keyboard events;
// it never touches hardware.
//
// Main loop:
//     while (kbd_get(&ev)) ui_key(&ev);
//     ui_render();                // redraw into fb (cheap, all in RAM)
//     if (!lcd_busy())            // lcd/lcd.h: DMA, returns at once
//         fb_flush(lcd_rows, NULL); // push only the rows that changed
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/keys.h"

// Tabs, by softkey slot: tab n sits over F(n+1). Slots without a tab stay
// empty.
typedef enum {
    UI_TAB_NODES = 0,
    UI_TAB_CHATS = 1,
    UI_TAB_POSITION = 2,
    UI_TAB_METRICS = 3,
    UI_TAB_SETTINGS = 4,
} ui_tab_t;

void ui_init(void);
void ui_key(const kbd_event_t *ev);
void ui_render(void);

// Wall clock for "heard 5m ago", epoch seconds. Unset (0), the UI uses the
// link's estimate of the node's clock, or failing that the newest last_heard
// in mesh_db.
void ui_set_now(uint32_t epoch);

// The PicoCalc's own battery for the status line, as kbd_battery() reports
// it: percent, or -1 if not known yet (nothing is shown).
//     bool chg; int pct = kbd_battery(&chg); ui_set_battery(pct, chg);
void ui_set_battery(int percent, bool charging);

ui_tab_t ui_tab(void);
