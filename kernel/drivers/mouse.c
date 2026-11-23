/*
 * mouse.c - PS/2 mouse on the 8042 aux port (IRQ 12). Detects the
 * IntelliMouse scroll wheel (4-byte packets), tracks an absolute pointer
 * position clamped to the screen, and queues events for consumers.
 */
#include "drivers.h"
#include "arch.h"
#include "sched.h"

#define EVT_SIZE 64

static bool     present, has_wheel;
static int32_t  pos_x, pos_y, max_x, max_y, speed = 1;
static uint8_t  buttons;
static uint64_t packets;

static uint8_t  pkt[4];
static int      pkt_len, pkt_idx;
static uint64_t last_byte_tick;

static mouse_event       evq[EVT_SIZE];
static volatile uint32_t head, tail;
static waitq_t           waiters;

static int set_rate(uint8_t rate)
{
    if (ps2_command(PS2_MOUSE, 0xF3)) return -1;
    return ps2_command(PS2_MOUSE, rate);
}

static void queue_event(int32_t dx, int32_t dy, int32_t wheel)
{
    uint32_t next = (head + 1) % EVT_SIZE;
    if (next == tail) {                               /* full: merge into the newest event */
        mouse_event *last = &evq[(head + EVT_SIZE - 1) % EVT_SIZE];
        last->dx += dx; last->dy += dy; last->wheel += wheel;
        last->x = pos_x; last->y = pos_y; last->buttons = buttons;
    } else {
        evq[head] = (mouse_event){ pos_x, pos_y, dx, dy, wheel, buttons };
        head = next;
    }
    waitq_wake_all(&waiters);
}

static void mouse_irq(interrupt_frame *f)
{
    (void)f;
    if (!(inb(0x64) & 0x01)) return;
    uint8_t b = inb(0x60);

    /* Resynchronise if a packet stalls half-way (lost byte). */
    uint64_t now = timer_ticks();
    if (pkt_idx && now - last_byte_tick > timer_hz() / 20) pkt_idx = 0;
    last_byte_tick = now;

    if (pkt_idx == 0 && !(b & 0x08)) return;          /* byte 0 always has bit 3 set */
    pkt[pkt_idx++] = b;
    if (pkt_idx < pkt_len) return;
    pkt_idx = 0;
    packets++;

    if (pkt[0] & 0xC0) return;                        /* X/Y overflow: discard */
    int32_t dx = (int32_t)pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
    int32_t dy = (int32_t)pkt[2] - ((pkt[0] & 0x20) ? 256 : 0);
    int32_t wz = has_wheel ? (int8_t)(uint8_t)(pkt[3] << 4) >> 4 : 0;
    dy = -dy;                                         /* PS/2 +y is up; screen +y is down */
    dx *= speed; dy *= speed;

    uint8_t old_buttons = buttons;
    buttons = pkt[0] & 0x07;
    pos_x = MIN(MAX(pos_x + dx, 0), max_x);
    pos_y = MIN(MAX(pos_y + dy, 0), max_y);
    if (dx || dy || wz || buttons != old_buttons) queue_event(dx, dy, wz);
}

bool mouse_init(uint32_t screen_w, uint32_t screen_h)
{
    if (!ps2_present() || !ps2_has_port2()) return false;
    if (ps2_command(PS2_MOUSE, 0xF6)) return false;   /* defaults: stream mode, reporting off */

    /* IntelliMouse knock: rates 200, 100, 80 then the ID changes from 0 to 3. */
    has_wheel = false;
    if (set_rate(200) == 0 && set_rate(100) == 0 && set_rate(80) == 0 && ps2_command(PS2_MOUSE, 0xF2) == 0) {
        int id = ps2_read(PS2_MOUSE);
        has_wheel = id == 3 || id == 4;
    }
    set_rate(100);
    if (ps2_command(PS2_MOUSE, 0xE8) == 0) ps2_command(PS2_MOUSE, 3);   /* 8 counts/mm */
    if (ps2_command(PS2_MOUSE, 0xF4)) return false;   /* enable data reporting */

    pkt_len = has_wheel ? 4 : 3;
    max_x = (int32_t)screen_w - 1;
    max_y = (int32_t)screen_h - 1;
    pos_x = max_x / 2;
    pos_y = max_y / 2;
    speed = screen_w >= 2560 ? 3 : screen_w >= 1600 ? 2 : 1;
    present = true;

    irq_install(12, mouse_irq);
    pic_unmask(12);
    return true;
}

void mouse_get_state(mouse_state *s)
{
    uint64_t f = irq_save();
    s->present = present;
    s->has_wheel = has_wheel;
    s->x = pos_x;
    s->y = pos_y;
    s->buttons = buttons;
    s->packets = packets;
    irq_restore(f);
}

bool mouse_get_event(mouse_event *ev, bool block)
{
    uint64_t f = irq_save();
    while (head == tail) {
        if (!block || !present) { irq_restore(f); return false; }
        waitq_wait(&waiters);
    }
    *ev = evq[tail];
    tail = (tail + 1) % EVT_SIZE;
    irq_restore(f);
    return true;
}
