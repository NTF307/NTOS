/* spin - burns CPU forever; stop it with Ctrl+C (or 'kill <pid>' if in the background) */
#include "ntos.h"

int main(void)
{
    printf("spin: pid %d burning CPU - press Ctrl+C to stop\n", getpid());
    uint64_t next = uptime_ms() + 1000;
    for (volatile uint64_t n = 0;; n++) {
        if (uptime_ms() >= next) { write(1, ".", 1); next += 1000; }
    }
}
