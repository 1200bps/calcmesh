// The calcmesh screen: a tab bar (Nodes, Chats), the active view, and a
// status line, drawn into the framebuffer (fb.h). The UI reads mesh_db and
// the message store and is driven by keyboard events; it never touches
// hardware.
//
// Main loop:
//     while (kbd_get(&ev)) ui_key(&ev);
//     ui_render();                // redraw into fb (cheap, all in RAM)
//     fb_flush(lcd_rows, NULL);   // push only the rows that changed
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/keys.h"

typedef enum {
    UI_TAB_NODES,
    UI_TAB_CHATS,
    UI_TAB_COUNT,
} ui_tab_t;

void ui_init(void);
void ui_key(const kbd_event_t *ev);
void ui_render(void);

// Wall clock for "heard 5m ago", epoch seconds. Unset (0), the UI uses the
// link's estimate of the node's clock, or failing that the newest last_heard
// in mesh_db.
void ui_set_now(uint32_t epoch);

ui_tab_t ui_tab(void);
