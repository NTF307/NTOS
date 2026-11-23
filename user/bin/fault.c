/*
 * fault <kind> - misbehaves on purpose; the kernel must kill only this process.
 *   null    read address 0              kernel  read kernel memory
 *   ro      write to read-only code     nx      execute the stack
 *   priv    run a privileged instruction (cli)
 *   ud      invalid opcode              div     divide by zero
 *   badptr  pass a kernel pointer to write() - must fail with EFAULT, not crash
 */
#include "ntos.h"

static const char *kinds = "null kernel ro nx priv ud div badptr";

int main(int argc, char **argv)
{
    const char *k = argc > 1 ? argv[1] : "";
    printf("fault: trying '%s'\n", k);
    if (!strcmp(k, "null"))        { volatile int *p = 0; return *p; }
    else if (!strcmp(k, "kernel")) { volatile uint64_t *p = (uint64_t *)0xFFFFFFFF80000000ULL; return (int)*p; }
    else if (!strcmp(k, "ro"))     { volatile uint8_t *p = (uint8_t *)main; *p = 0xC3; }
    else if (!strcmp(k, "nx"))     { uint8_t code[16] = { 0xC3 }; ((void (*)(void))code)(); }
    else if (!strcmp(k, "priv"))   { __asm__ volatile("cli"); }
    else if (!strcmp(k, "ud"))     { __asm__ volatile("ud2"); }
    else if (!strcmp(k, "div"))    { __asm__ volatile("xor %%edx, %%edx; div %0" :: "r"(0u), "a"(1u) : "rdx"); }
    else if (!strcmp(k, "badptr")) {
        long r = write(1, (const void *)0xFFFF800000000000ULL, 16);
        printf("fault: write() with a kernel pointer returned %ld (%s)\n", r, r == -EFAULT ? "EFAULT - ok" : "WRONG");
        return r == -EFAULT ? 0 : 1;
    } else {
        printf("usage: fault <%s>\n", kinds);
        return 2;
    }
    printf("fault: survived '%s' - this should not happen\n", k);
    return 1;
}
