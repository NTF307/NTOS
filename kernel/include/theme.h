/*
 * theme.h - the only place colors are defined. All values are 0xRRGGBB;
 * the console converts them to the framebuffer's pixel format.
 */
#pragma once

#define THEME_BG        0x0F1419   /* console background */
#define THEME_FG        0xD8DEE9   /* normal text */
#define THEME_DIM       0x6B7A8F   /* secondary text */
#define THEME_ACCENT    0x4FC1FF   /* logo, headings */
#define THEME_PROMPT    0x7FD962   /* shell prompt */
#define THEME_OK        0x7FD962   /* [ OK ] */
#define THEME_WARN      0xFFB454   /* [WARN] */
#define THEME_ERROR     0xFF5F5F   /* errors */
#define THEME_PANIC_BG  0x5C0F14   /* kernel panic background */
#define THEME_PANIC_FG  0xFFFFFF   /* kernel panic text */
