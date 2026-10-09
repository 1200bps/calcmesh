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

// Screen areas
#define UI_TABS_H 20
#define UI_BODY_Y (UI_TABS_H + 1)
#define UI_STATUS_Y (FB_H - UI_LH - 2) // the status line's rule
#define UI_BODY_H (UI_STATUS_Y - UI_BODY_Y - 1)
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

void ui_switch_tab(int tab);
