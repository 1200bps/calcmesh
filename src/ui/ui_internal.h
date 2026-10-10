// Shared between the UI's views.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fb.h"
#include "input/keys.h"
#include "msgs.h"
#include "nodedb.h"

#define UI_FONT (&font_6x12)
#define UI_CW 6  // cell width
#define UI_LH 12 // line height

// Screen areas: status line on top, softkey tabs along the bottom (one
// slot per function key, after the HP 48/49/50 menu row)
#define UI_STATUS_RULE (UI_LH + 1)
#define UI_BODY_Y (UI_STATUS_RULE + 2)
#define UI_SLOTS 5
#define UI_SLOT_W (FB_W / UI_SLOTS)
#define UI_KEYS_H (UI_LH + 3)
#define UI_KEYS_Y (FB_H - UI_KEYS_H)
#define UI_BODY_H (UI_KEYS_Y - 2 - UI_BODY_Y)
#define UI_COLS (FB_W / UI_CW)

uint32_t ui_now(void);

// Strings for nodes; never NULL
const char *ui_node_short(uint32_t num); // short name, or last 4 hex of the id
const char *ui_node_long(uint32_t num);  // long name, or !id

// "5s" "12m" "3h" "9d", or "-" if never; into buf (>= 6 bytes)
const char *ui_ago(uint32_t then, char *buf);

// Text in cell columns: draw s left- or right-aligned in [col, col + cells).
void ui_cells(int col, int y, int cells, const char *s, uint8_t fg, uint8_t bg);
void ui_cells_right(int col, int y, int cells, const char *s, uint8_t fg, uint8_t bg);

// A scrolling list: keeps sel in range and visible in rows lines.
typedef struct {
    int sel, top;
} ui_list_t;
bool ui_list_key(ui_list_t *l, int count, int rows, uint16_t key);
void ui_list_fix(ui_list_t *l, int count, int rows);

// Views
void ui_nodes_draw(void);
bool ui_nodes_key(const kbd_event_t *ev);

void ui_chats_draw(void);
bool ui_chats_key(const kbd_event_t *ev);
void ui_chats_open(mesh_conv_t c); // switch to the Chats tab, in conversation c
int ui_chats_unread(void);

void ui_position_draw(void);
bool ui_position_key(const kbd_event_t *ev);

void ui_settings_draw(void);
bool ui_settings_key(const kbd_event_t *ev);
bool ui_settings_pending(void); // node changes not written yet
bool ui_settings_modal(void);   // editing a field or confirming: keys stay here

void ui_switch_tab(int tab); // a tab that doesn't exist is ignored
