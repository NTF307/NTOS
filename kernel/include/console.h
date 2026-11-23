/* console.h - serial port + framebuffer text console */
#pragma once
#include "types.h"
#include "../../include/bootinfo.h"

void serial_init(void);
bool serial_present(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);

void     console_init(const ntos_framebuffer *fb, uint64_t hhdm);
bool     console_enable_backbuffer(void);
void     console_putc(char c);
void     console_write(const char *s, size_t n);
void     console_clear(void);
void     console_set_color(uint32_t fg, uint32_t bg);
void     console_get_color(uint32_t *fg, uint32_t *bg);
void     console_set_cursor(bool visible);
uint32_t console_cols(void);
uint32_t console_rows(void);

/* font.c - built-in 5x7 bitmap font (+2 descender rows) */
#define FONT_GLYPH_W    5
#define FONT_GLYPH_H    9
void           font_init(void);
const uint8_t *font_glyph(char c);   /* FONT_GLYPH_H rows, bit 4 = leftmost pixel */
