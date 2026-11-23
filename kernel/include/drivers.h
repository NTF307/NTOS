/* drivers.h - device drivers */
#pragma once
#include "types.h"

/* ---- ps2.c: 8042 PS/2 controller --------------------------------------- */
#define PS2_KEYBOARD 1
#define PS2_MOUSE    2

bool ps2_init(void);                         /* false if no controller responds */
bool ps2_present(void);
bool ps2_has_port2(void);
bool ps2_port_ok(int port);                  /* interface test passed */
int  ps2_command(int port, uint8_t byte);    /* send, wait for ACK: 0 = ok, -1 = fail */
int  ps2_read(int port);                     /* polled read from that port, -1 = timeout */
void ps2_send_nowait(int port, uint8_t byte);
void ps2_enable_irqs(void);                  /* after the device drivers are configured */

/* ---- keyboard.c --------------------------------------------------------- */
/*
 * Key identifiers. Keys that type a character use the unshifted ASCII value
 * ('a', '1', ' ', '\n', '\b', '\t', KEY_ESC); everything else is >= 0x100.
 */
enum {
    KEY_ESC = 27,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_DELETE, KEY_PGUP, KEY_PGDN, KEY_INSERT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT, KEY_LGUI, KEY_RGUI, KEY_MENU,
    KEY_CAPSLOCK, KEY_NUMLOCK, KEY_SCROLLLOCK, KEY_PRINTSCREEN, KEY_PAUSE,
    KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9,
    KEY_KPDOT, KEY_KPENTER, KEY_KPPLUS, KEY_KPMINUS, KEY_KPSTAR, KEY_KPSLASH,
};

/* Modifier / lock state bits (key_event.mods, keyboard_mods()). */
#define KMOD_SHIFT  0x01
#define KMOD_CTRL   0x02
#define KMOD_ALT    0x04
#define KMOD_GUI    0x08
#define KMOD_CAPS   0x10
#define KMOD_NUM    0x20
#define KMOD_SCROLL 0x40

typedef struct {
    uint16_t key;        /* KEY_* or unshifted ASCII */
    uint16_t scancode;   /* set-1 make code, 0xE0xx for extended keys */
    bool     pressed;    /* false = released */
    uint8_t  mods;       /* KMOD_* at the time of the event */
    char     ascii;      /* character this press types (0 if none) */
} key_event;

bool        keyboard_init(void);
bool        keyboard_get_event(key_event *ev, bool block);
int         keyboard_getchar(void);          /* blocks; ASCII or KEY_* for special keys */
bool        keyboard_poll(int *key);         /* non-blocking keyboard_getchar */
uint8_t     keyboard_mods(void);
const char *keyboard_key_name(uint16_t key);

/* ---- mouse.c ------------------------------------------------------------ */
#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

typedef struct {
    int32_t x, y;        /* pointer position after this event (pixels) */
    int32_t dx, dy;      /* movement, screen orientation (+y = down) */
    int32_t wheel;       /* + = towards the user */
    uint8_t buttons;     /* MOUSE_* */
} mouse_event;

typedef struct {
    bool     present, has_wheel;
    int32_t  x, y;
    uint8_t  buttons;
    uint64_t packets;
} mouse_state;

bool mouse_init(uint32_t screen_w, uint32_t screen_h);
void mouse_get_state(mouse_state *s);
bool mouse_get_event(mouse_event *ev, bool block);
