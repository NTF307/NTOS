/* drivers.h - device drivers */
#pragma once
#include "types.h"

/* Special keys returned by keyboard_getchar() (ASCII is returned as-is). */
enum {
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_DELETE, KEY_PGUP, KEY_PGDN,
};

void keyboard_init(void);
int  keyboard_getchar(void);          /* blocks until a key is pressed */
bool keyboard_poll(int *key);         /* non-blocking */
