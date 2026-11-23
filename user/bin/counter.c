/* counter [n] [ms] - counts to n, sleeping between steps; try 'counter 10 &' */
#include "ntos.h"

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 5;
    int ms = argc > 2 ? atoi(argv[2]) : 1000;
    int pid = getpid();
    for (int i = 1; i <= n; i++) {
        printf("[counter %d] %d/%d\n", pid, i, n);
        if (i < n) sleep_ms((uint64_t)ms);
    }
    return 0;
}
