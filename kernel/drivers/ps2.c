/*
 * ps2.c - Intel 8042 PS/2 controller: port 1 = keyboard (IRQ 1), port 2 = mouse (IRQ 12).
 * All device I/O here is polled and only used during initialisation (IRQs off);
 * after ps2_enable_irqs() the keyboard and mouse drivers read port 0x60 in their IRQs.
 */
#include "drivers.h"
#include "arch.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define ST_OUT_FULL 0x01
#define ST_IN_FULL  0x02
#define ST_AUX_DATA 0x20

#define CFG_IRQ1      0x01
#define CFG_IRQ12     0x02
#define CFG_CLK1_OFF  0x10
#define CFG_CLK2_OFF  0x20
#define CFG_TRANSLATE 0x40

static bool present, port2, port_ok[3];

static bool wait_input_empty(void)
{
    for (int i = 0; i < 200000; i++) { if (!(inb(PS2_STATUS) & ST_IN_FULL)) return true; io_wait(); }
    return false;
}

static bool wait_output_full(void)
{
    for (int i = 0; i < 200000; i++) { if (inb(PS2_STATUS) & ST_OUT_FULL) return true; io_wait(); }
    return false;
}

static void ctl_cmd(uint8_t c) { if (wait_input_empty()) outb(PS2_CMD, c); }
static int  ctl_read(void)      { return wait_output_full() ? inb(PS2_DATA) : -1; }

static void ctl_write_data(uint8_t v) { if (wait_input_empty()) outb(PS2_DATA, v); }

static void flush(void)
{
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & ST_OUT_FULL); i++) { inb(PS2_DATA); io_wait(); }
}

static int read_config(void) { ctl_cmd(0x20); return ctl_read(); }
static void write_config(uint8_t v) { ctl_cmd(0x60); ctl_write_data(v); }

bool ps2_init(void)
{
    if (inb(PS2_STATUS) == 0xFF) return false;      /* floating bus: no controller */

    ctl_cmd(0xAD);                                  /* disable port 1 */
    ctl_cmd(0xA7);                                  /* disable port 2 */
    flush();

    int cfg = read_config();
    if (cfg < 0) return false;
    cfg &= ~(CFG_IRQ1 | CFG_IRQ12);                 /* no IRQs while we set up */
    cfg |= CFG_TRANSLATE;                           /* keyboard delivers scancode set 1 */
    write_config((uint8_t)cfg);

    /* Dual-channel check: enabling port 2 must clear its clock-disable bit. */
    ctl_cmd(0xA8);
    int c2 = read_config();
    port2 = c2 >= 0 && !(c2 & CFG_CLK2_OFF);
    ctl_cmd(0xA7);
    flush();

    /* Interface tests (0 = pass). Emulated controllers sometimes don't answer: */
    /* treat a timeout on port 1 as "probably fine" so USB legacy keyboards work. */
    ctl_cmd(0xAB);
    int r1 = ctl_read();
    port_ok[1] = r1 == 0 || r1 < 0;
    if (port2) {
        ctl_cmd(0xA9);
        port_ok[2] = ctl_read() == 0;
        port2 = port_ok[2];
    }
    flush();

    if (port_ok[1]) ctl_cmd(0xAE);
    if (port2) ctl_cmd(0xA8);
    flush();
    present = true;
    return true;
}

bool ps2_present(void)        { return present; }
bool ps2_has_port2(void)      { return port2; }
bool ps2_port_ok(int port)    { return port >= 1 && port <= 2 && port_ok[port]; }

void ps2_send_nowait(int port, uint8_t byte)
{
    if (port == PS2_MOUSE) ctl_cmd(0xD4);
    ctl_write_data(byte);
}

int ps2_read(int port)
{
    for (int tries = 0; tries < 16; tries++) {
        if (!wait_output_full()) return -1;
        uint8_t st = inb(PS2_STATUS);
        uint8_t b = inb(PS2_DATA);
        bool aux = st & ST_AUX_DATA;
        if (aux == (port == PS2_MOUSE)) return b;   /* drop bytes from the other device */
    }
    return -1;
}

int ps2_command(int port, uint8_t byte)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        ps2_send_nowait(port, byte);
        int r = ps2_read(port);
        if (r == 0xFA) return 0;                     /* ACK */
        if (r != 0xFE) return -1;                    /* anything but "resend" */
    }
    return -1;
}

void ps2_enable_irqs(void)
{
    int cfg = read_config();
    if (cfg < 0) return;
    cfg &= ~CFG_CLK1_OFF;
    cfg |= CFG_IRQ1 | CFG_TRANSLATE;
    if (port2) { cfg &= ~CFG_CLK2_OFF; cfg |= CFG_IRQ12; }
    write_config((uint8_t)cfg);
    flush();
}
