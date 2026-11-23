/* keyboard.c - PS/2 keyboard (IRQ 1), scancode set 1, US layout */
#include "drivers.h"
#include "arch.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define BUF_SIZE   128

static const char map_normal[58] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char map_shift[58] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static volatile int buf[BUF_SIZE];
static volatile uint32_t head, tail;
static bool shift_l, shift_r, ctrl, caps, extended;

static void push(int key)
{
    uint32_t next = (head + 1) % BUF_SIZE;
    if (next != tail) { buf[head] = key; head = next; }
}

static void kbd_irq(interrupt_frame *f)
{
    (void)f;
    uint8_t sc = inb(KBD_DATA);

    if (sc == 0xE0) { extended = true; return; }
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (extended) {
        extended = false;
        if (code == 0x1D) { ctrl = !release; return; }           /* right ctrl */
        if (release) return;
        switch (code) {
        case 0x48: push(KEY_UP); break;
        case 0x50: push(KEY_DOWN); break;
        case 0x4B: push(KEY_LEFT); break;
        case 0x4D: push(KEY_RIGHT); break;
        case 0x47: push(KEY_HOME); break;
        case 0x4F: push(KEY_END); break;
        case 0x53: push(KEY_DELETE); break;
        case 0x49: push(KEY_PGUP); break;
        case 0x51: push(KEY_PGDN); break;
        case 0x1C: push('\n'); break;                            /* keypad enter */
        case 0x35: push('/'); break;                             /* keypad slash */
        }
        return;
    }

    switch (code) {
    case 0x2A: shift_l = !release; return;
    case 0x36: shift_r = !release; return;
    case 0x1D: ctrl = !release; return;
    case 0x3A: if (!release) caps = !caps; return;
    }
    if (release || code >= sizeof map_normal) return;

    bool shift = shift_l || shift_r;
    char c = shift ? map_shift[code] : map_normal[code];
    if (!c) return;
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1F;   /* Ctrl+C = 3 ... */
    push((unsigned char)c);
}

static bool wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(KBD_STATUS) & 2)) return true; return false; }
static bool wait_read(void)  { for (int i = 0; i < 100000; i++) if (inb(KBD_STATUS) & 1) return true; return false; }

void keyboard_init(void)
{
    for (int i = 0; i < 32 && (inb(KBD_STATUS) & 1); i++) inb(KBD_DATA);   /* flush */

    /* Make sure the controller raises IRQ1 and translates to set 1; firmware may poll instead. */
    if (wait_write()) {
        outb(KBD_STATUS, 0x20);
        if (wait_read()) {
            uint8_t cfg = inb(KBD_DATA);
            cfg |= 0x01 | 0x40;          /* port 1 IRQ, translation */
            cfg &= (uint8_t)~0x10;       /* port 1 clock enabled */
            if (wait_write()) { outb(KBD_STATUS, 0x60); if (wait_write()) outb(KBD_DATA, cfg); }
        }
    }
    if (wait_write()) outb(KBD_STATUS, 0xAE);   /* enable port 1 */

    irq_install(1, kbd_irq);
    pic_unmask(1);
}

bool keyboard_poll(int *key)
{
    cpu_cli();
    if (head == tail) { cpu_sti(); return false; }
    *key = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    cpu_sti();
    return true;
}

int keyboard_getchar(void)
{
    for (;;) {
        cpu_cli();
        if (head != tail) {
            int k = buf[tail];
            tail = (tail + 1) % BUF_SIZE;
            cpu_sti();
            return k;
        }
        __asm__ volatile("sti; hlt" ::: "memory");   /* atomic: no lost wakeup */
    }
}
