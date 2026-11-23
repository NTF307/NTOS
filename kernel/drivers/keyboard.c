/*
 * keyboard.c - PS/2 keyboard (IRQ 1). The controller translates to scancode
 * set 1. Produces press/release events with modifier state and US-layout
 * characters, drives the Caps/Num/Scroll LEDs, and handles Ctrl+Alt+Del.
 */
#include "drivers.h"
#include "arch.h"
#include "sched.h"

#define BUF_SIZE 128

/* Set-1 make code -> key id. Printable keys hold their unshifted ASCII. */
static const uint16_t keymap[0x59] = {
    [0x01] = KEY_ESC,
    [0x02] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    [0x10] = 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', KEY_LCTRL,
    [0x1E] = 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', KEY_LSHIFT, '\\',
    [0x2C] = 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', KEY_RSHIFT, KEY_KPSTAR,
    [0x38] = KEY_LALT, ' ', KEY_CAPSLOCK,
    [0x3B] = KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    [0x45] = KEY_NUMLOCK, KEY_SCROLLLOCK,
    [0x47] = KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPMINUS, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KPPLUS,
    [0x4F] = KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP0, KEY_KPDOT,
    [0x56] = '\\',                                   /* ISO 102nd key */
    [0x57] = KEY_F11, KEY_F12,
};

static uint16_t extended_key(uint8_t code)
{
    switch (code) {
    case 0x1C: return KEY_KPENTER;
    case 0x1D: return KEY_RCTRL;
    case 0x35: return KEY_KPSLASH;
    case 0x37: return KEY_PRINTSCREEN;
    case 0x38: return KEY_RALT;
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PGUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x5B: return KEY_LGUI;
    case 0x5C: return KEY_RGUI;
    case 0x5D: return KEY_MENU;
    default:   return 0;                           /* multimedia keys etc.: ignored */
    }
}

static const char shift_from[] = "`1234567890-=[]\\;',./";
static const char shift_to[]   = "~!@#$%^&*()_+{}|:\"<>?";

static key_event         buf[BUF_SIZE];
static volatile uint32_t head, tail;
static waitq_t           waiters;

static bool    held[KEY_KPSLASH + 1];
static uint8_t locks = KMOD_NUM;                  /* NumLock on by default */
static bool    extended;
static int     skip_bytes;                        /* rest of the Pause sequence */

/* LED update: send 0xED, wait for ACK (in the IRQ), then send the LED byte. */
static volatile int led_stage;                    /* 0 idle, 1 sent 0xED, 2 sent value */
static bool         led_dirty;
static uint64_t     led_sent_tick;

static uint8_t led_byte(void)
{
    return (uint8_t)(((locks & KMOD_SCROLL) ? 1 : 0) | ((locks & KMOD_NUM) ? 2 : 0) | ((locks & KMOD_CAPS) ? 4 : 0));
}

static void leds_update(void)
{
    if (led_stage && timer_ticks() - led_sent_tick < timer_hz() / 5) { led_dirty = true; return; }
    led_stage = 1;
    led_dirty = false;
    led_sent_tick = timer_ticks();
    ps2_send_nowait(PS2_KEYBOARD, 0xED);
}

static uint8_t current_mods(void)
{
    uint8_t m = locks;
    if (held[KEY_LSHIFT] || held[KEY_RSHIFT]) m |= KMOD_SHIFT;
    if (held[KEY_LCTRL] || held[KEY_RCTRL])   m |= KMOD_CTRL;
    if (held[KEY_LALT] || held[KEY_RALT])     m |= KMOD_ALT;
    if (held[KEY_LGUI] || held[KEY_RGUI])     m |= KMOD_GUI;
    return m;
}

static char to_ascii(uint16_t key, uint8_t mods)
{
    if (mods & KMOD_ALT) return 0;
    switch (key) {
    case KEY_KPENTER: return '\n';
    case KEY_KPSLASH: return '/';
    case KEY_KPSTAR:  return '*';
    case KEY_KPMINUS: return '-';
    case KEY_KPPLUS:  return '+';
    }
    if (key >= KEY_KP0 && key <= KEY_KP9) return (mods & KMOD_NUM) ? (char)('0' + key - KEY_KP0) : 0;
    if (key == KEY_KPDOT) return (mods & KMOD_NUM) ? '.' : 0;
    if (key >= 0x100) return 0;

    char c = (char)key;
    bool shift = mods & KMOD_SHIFT;
    if (c >= 'a' && c <= 'z') {
        if (shift != !!(mods & KMOD_CAPS)) c -= 32;
        if (mods & KMOD_CTRL) c &= 0x1F;               /* Ctrl+C = 3 ... */
        return c;
    }
    if (shift)
        for (int i = 0; shift_from[i]; i++)
            if (shift_from[i] == c) return shift_to[i];
    return c;
}

static void push(uint16_t key, uint16_t scancode, bool pressed)
{
    uint8_t mods = current_mods();
    uint32_t next = (head + 1) % BUF_SIZE;
    if (next == tail) return;                          /* full: drop */
    buf[head] = (key_event){ key, scancode, pressed, mods, pressed ? to_ascii(key, mods) : 0 };
    head = next;
    waitq_wake_all(&waiters);
}

static void kbd_irq(interrupt_frame *f)
{
    (void)f;
    uint8_t sc = inb(0x60);

    if (sc == 0xFA) {                                  /* ACK: continue an LED update */
        if (led_stage == 1) { led_stage = 2; ps2_send_nowait(PS2_KEYBOARD, led_byte()); }
        else if (led_stage == 2) { led_stage = 0; if (led_dirty) leds_update(); }
        return;
    }
    if (sc == 0xFE || sc == 0x00 || sc == 0xFF) { led_stage = 0; return; }   /* resend / errors */

    if (skip_bytes) { skip_bytes--; return; }
    if (sc == 0xE1) {                                  /* Pause: E1 1D 45 E1 9D C5, press only */
        skip_bytes = 5;
        push(KEY_PAUSE, 0xE11D, true);
        push(KEY_PAUSE, 0xE11D, false);
        return;
    }
    if (sc == 0xE0) { extended = true; return; }

    bool pressed = !(sc & 0x80);
    uint8_t code = sc & 0x7F;
    uint16_t key, scancode;
    if (extended) {
        extended = false;
        if (code == 0x2A || code == 0x36) return;      /* fake shifts around PrtSc etc. */
        key = extended_key(code);
        scancode = 0xE000 | code;
    } else {
        key = code < ARRAY_SIZE(keymap) ? keymap[code] : 0;
        scancode = code;
    }
    if (!key) return;

    bool was_held = key < ARRAY_SIZE(held) && held[key];
    if (key < ARRAY_SIZE(held)) held[key] = pressed;

    if (pressed && !was_held) {                        /* lock keys toggle on the first press only */
        uint8_t old = locks;
        if (key == KEY_CAPSLOCK)   locks ^= KMOD_CAPS;
        if (key == KEY_NUMLOCK)    locks ^= KMOD_NUM;
        if (key == KEY_SCROLLLOCK) locks ^= KMOD_SCROLL;
        if (locks != old) leds_update();
    }
    if (pressed && key == KEY_DELETE && (current_mods() & (KMOD_CTRL | KMOD_ALT)) == (KMOD_CTRL | KMOD_ALT))
        cpu_reboot();

    push(key, scancode, pressed);
}

bool keyboard_init(void)
{
    if (!ps2_present()) return false;
    ps2_command(PS2_KEYBOARD, 0xF4);                   /* enable scanning (harmless if already on) */
    if (ps2_command(PS2_KEYBOARD, 0xED) == 0) ps2_command(PS2_KEYBOARD, led_byte());
    irq_install(1, kbd_irq);
    pic_unmask(1);
    return true;
}

bool keyboard_get_event(key_event *ev, bool block)
{
    uint64_t f = irq_save();
    while (head == tail) {
        if (!block) { irq_restore(f); return false; }
        waitq_wait(&waiters);
    }
    *ev = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    irq_restore(f);
    return true;
}

/* Map an event to what a line editor wants: a character or a navigation key. */
static int event_to_char(const key_event *ev)
{
    if (!ev->pressed) return -1;
    if (ev->ascii) return (unsigned char)ev->ascii;
    if (!(ev->mods & KMOD_NUM)) {
        switch (ev->key) {                             /* keypad as navigation keys */
        case KEY_KP7: return KEY_HOME;
        case KEY_KP8: return KEY_UP;
        case KEY_KP9: return KEY_PGUP;
        case KEY_KP4: return KEY_LEFT;
        case KEY_KP6: return KEY_RIGHT;
        case KEY_KP1: return KEY_END;
        case KEY_KP2: return KEY_DOWN;
        case KEY_KP3: return KEY_PGDN;
        case KEY_KP0: return KEY_INSERT;
        case KEY_KPDOT: return KEY_DELETE;
        }
    }
    if ((ev->key >= KEY_UP && ev->key <= KEY_F12) || ev->key == KEY_ESC) return ev->key;
    return -1;
}

int keyboard_getchar(void)
{
    key_event ev;
    for (;;) {
        keyboard_get_event(&ev, true);
        int c = event_to_char(&ev);
        if (c >= 0) return c;
    }
}

bool keyboard_poll(int *key)
{
    key_event ev;
    while (keyboard_get_event(&ev, false)) {
        int c = event_to_char(&ev);
        if (c >= 0) { *key = c; return true; }
    }
    return false;
}

uint8_t keyboard_mods(void)
{
    uint64_t f = irq_save();
    uint8_t m = current_mods();
    irq_restore(f);
    return m;
}

const char *keyboard_key_name(uint16_t key)
{
    static const char *const names[] = {
        "Up", "Down", "Left", "Right", "Home", "End", "Delete", "PgUp", "PgDn", "Insert",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
        "LShift", "RShift", "LCtrl", "RCtrl", "LAlt", "RAlt", "LWin", "RWin", "Menu",
        "CapsLock", "NumLock", "ScrollLock", "PrtSc", "Pause",
        "KP0", "KP1", "KP2", "KP3", "KP4", "KP5", "KP6", "KP7", "KP8", "KP9",
        "KP.", "KPEnter", "KP+", "KP-", "KP*", "KP/",
    };
    static char one[2];
    switch (key) {
    case KEY_ESC: return "Esc";
    case '\b':    return "Backspace";
    case '\t':    return "Tab";
    case '\n':    return "Enter";
    case ' ':     return "Space";
    }
    if (key >= KEY_UP && key - KEY_UP < ARRAY_SIZE(names)) return names[key - KEY_UP];
    if (key >= 33 && key < 127) { one[0] = (char)key; one[1] = 0; return one; }
    return "?";
}
