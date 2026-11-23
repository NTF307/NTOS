/* ask - reads a line from the keyboard */
#include "ntos.h"

int main(void)
{
    char name[64];
    printf("What is your name? ");
    long n = read(0, name, sizeof name - 1);
    if (n < 0) return 1;
    while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == ' ')) n--;
    name[n] = 0;
    printf("Hello, %s! Your name has %d letters.\n", n ? name : "stranger", (int)n);
    return 0;
}
