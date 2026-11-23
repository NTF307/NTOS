/* hello - prints its arguments from ring 3 */
#include "ntos.h"

int main(int argc, char **argv)
{
    printf("Hello from user mode! I am '%s', pid %d.\n", argv[0], getpid());
    for (int i = 1; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 0;
}
