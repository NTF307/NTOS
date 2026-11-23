/* serial.c - COM1 (0x3F8) 115200 8N1, used as a debug log mirror */
#include "console.h"
#include "arch.h"

#define COM1 0x3F8

static bool present;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4, 0x1E);   /* loopback test */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) { present = false; return; }
    outb(COM1 + 4, 0x0F);   /* normal operation */
    present = true;
}

bool serial_present(void) { return present; }

void serial_putc(char c)
{
    if (!present) return;
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, size_t n)
{
    while (n--) serial_putc(*s++);
}
