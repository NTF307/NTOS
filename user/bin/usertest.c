/* usertest - self-test of the system call interface, run by the QEMU boot test */
#include "ntos.h"

static int failures;

static void check(bool ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    printf("usertest: pid %d\n", getpid());
    check(getpid() > 0, "getpid");

    /* heap */
    void *before = sbrk(0);
    char *blocks[64];
    bool heap_ok = true;
    for (int i = 0; i < 64; i++) {
        blocks[i] = malloc(100 + (size_t)i * 300);
        if (!blocks[i]) { heap_ok = false; break; }
        memset(blocks[i], i, 100 + (size_t)i * 300);
    }
    for (int i = 0; heap_ok && i < 64; i++)
        for (int j = 0; j < 100 + i * 300; j++)
            if (blocks[i][j] != (char)i) { heap_ok = false; break; }
    for (int i = 0; heap_ok && i < 64; i++) free(blocks[i]);
    check(heap_ok && sbrk(0) > before, "malloc/free over sbrk (64 blocks)");
    check(sbrk(1L << 40) == (void *)-1, "sbrk beyond the heap limit fails");

    /* timing */
    uint64_t t0 = uptime_ms();
    sleep_ms(50);
    uint64_t dt = uptime_ms() - t0;
    check(dt >= 50 && dt < 80, "sleep_ms(50)");

    /* bad pointers and numbers are rejected, not fatal */
    check(write(1, (void *)0xFFFF800000000000ULL, 8) == -EFAULT, "write() rejects a kernel pointer");
    check(write(1, (void *)0x10, 8) == -EFAULT, "write() rejects an unmapped pointer");
    check(read(0, (void *)0x400000, 4) == -EFAULT, "read() rejects a read-only buffer");
    check(write(7, "x", 1) == -EBADF, "write() to a bad fd");
    check(syscall3(999, 0, 0, 0) == -ENOSYS, "unknown system call");

    /* processes */
    const char *hello_argv[] = { "hello", "from", "usertest", 0 };
    int status = -100;
    int pid = spawn("hello", hello_argv);
    check(pid > 0 && wait(pid, &status) == pid && status == 0, "spawn + wait: hello exits with 0");

    const char *fault_argv[] = { "fault", "null", 0 };
    pid = spawn("fault", fault_argv);
    check(pid > 0 && wait(pid, &status) == pid && status == -1, "spawn + wait: faulting child is killed (-1)");

    check(spawn("does-not-exist", 0) == -ENOENT, "spawn of a missing program fails");
    check(wait(1, 0) == -ECHILD, "wait for a non-child fails");

    printf("usertest: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
