/*
 * proc.h - user processes, ELF loading, system calls, initial ramdisk.
 * A process is one address space plus (for now) exactly one user thread;
 * its PID is that thread's TID.
 */
#pragma once
#include "types.h"
#include "mm.h"
#include "sched.h"
#include "arch.h"

/* User address space layout */
#define USER_STACK_TOP    0x00007FFFFFFF0000ULL
#define USER_STACK_PAGES  16                    /* 64 KiB, unmapped guard below */
#define USER_HEAP_MAX     (64ULL << 20)         /* sbrk limit */
#define PROC_ARGS_MAX     16
#define PROC_ARG_BYTES    512

typedef enum { PROC_RUNNING, PROC_ZOMBIE } proc_state;

typedef struct process {
    uint32_t        pid;
    char            name[THREAD_NAME_MAX];
    proc_state      state;
    address_space  *as;
    thread_t       *thread;
    uint32_t        parent;                     /* 0 = started by the kernel shell */
    bool            detached;                   /* nobody will wait: free at exit */
    int             exit_code;
    uint64_t        entry, user_sp, argc, argv;
    uint64_t        brk_start, brk;
    waitq_t         exit_wq;
    struct process *next;
} process_t;

typedef struct {
    uint32_t   pid, parent;
    proc_state state;
    char       name[THREAD_NAME_MAX];
    uint64_t   pages;
} process_info;

/* Errors returned by system calls (negated). */
enum { E_NOENT = 2, E_INTR = 4, E_NOEXEC = 8, E_BADF = 9, E_CHILD = 10, E_NOMEM = 12,
       E_FAULT = 14, E_INVAL = 22, E_NOSYS = 38 };

/* process.c */
int           process_spawn(const char *path, int argc, const char *const *argv, uint32_t parent, bool detached);
int           process_wait(uint32_t pid, uint32_t parent, int *exit_code);   /* pid or -E_CHILD/-E_INTR */
void          process_kill(uint32_t pid);
NORETURN void process_exit(int code);
void          process_check_kill(void);        /* exits the current process if it was killed */
NORETURN void process_fault(interrupt_frame *f, const char *what);
void          process_set_foreground(uint32_t pid);
bool          process_interrupt_foreground(void);   /* Ctrl+C, IRQ context */
size_t        process_list(process_info *out, size_t max);
process_t    *process_current(void);

/* elf.c */
int elf_load(address_space *as, const uint8_t *data, uint64_t size, uint64_t *entry, uint64_t *end);

/* syscall.c + syscall.asm */
void syscall_init(void);
NORETURN void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1);

/* initrd.c */
bool initrd_init(uint64_t phys, uint64_t size);
bool initrd_find(const char *name, const uint8_t **data, uint64_t *size);
typedef void (*initrd_cb)(const char *name, uint64_t size, void *ctx);
void initrd_list(initrd_cb cb, void *ctx);
