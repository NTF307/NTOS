/*
 * sched.h - kernel threads, preemptive round-robin scheduler, wait queues, mutexes.
 * Single CPU: "locking" against interrupts means irq_save()/irq_restore().
 */
#pragma once
#include "types.h"

#define THREAD_NAME_MAX   24
#define THREAD_STACK_SIZE (16 * 1024)
#define SCHED_QUANTUM_MS  10

typedef enum {
    THREAD_READY, THREAD_RUNNING, THREAD_SLEEPING, THREAD_BLOCKED, THREAD_DEAD,
} thread_state;

struct waitq;

typedef struct thread {
    uint64_t       rsp;                     /* saved stack pointer while switched out */
    uint32_t       tid;
    thread_state   state;
    char           name[THREAD_NAME_MAX];
    uint8_t       *stack;                   /* lowest address of the stack (canary lives here) */
    size_t         stack_size;
    uint64_t       wake_tick;               /* THREAD_SLEEPING: timer tick to wake at */
    uint64_t       cpu_ticks;               /* timer ticks spent running */
    uint64_t       switches;                /* times scheduled in */
    struct thread *next;                    /* run queue / sleep list / wait queue / zombie list */
    struct thread *all_next;                /* list of every thread */
    struct waitq  *wq;                      /* THREAD_BLOCKED: queue it waits on */
} thread_t;

typedef struct waitq { thread_t *head, *tail; } waitq_t;
typedef struct { thread_t *owner; uint32_t depth; waitq_t wq; } mutex_t;   /* recursive */

typedef void (*thread_fn)(void *arg);

typedef struct {
    uint32_t     tid;
    thread_state state;
    char         name[THREAD_NAME_MAX];
    uint64_t     cpu_ticks, switches;
    size_t       stack_size;
} thread_info;

void        sched_init(void);               /* adopts the boot context as thread 1 */
bool        sched_active(void);
uint64_t    sched_context_switches(void);

thread_t   *thread_create(const char *name, thread_fn fn, void *arg);
thread_t   *thread_current(void);
void        thread_yield(void);
void        thread_sleep_ms(uint64_t ms);
NORETURN void thread_exit(void);
int         thread_kill(uint32_t tid);      /* 0 = killed, -1 = no such thread, -2 = protected */
size_t      thread_list(thread_info *out, size_t max);
const char *thread_state_name(thread_state s);

/* Called from interrupt context. */
void sched_tick(void);                      /* timer IRQ: accounting, wake sleepers, quantum */
void sched_irq_exit(void);                  /* end of every IRQ, after EOI: preempt if needed */

/* waitq_wait must be called with interrupts disabled; re-check the condition in a loop. */
void waitq_wait(waitq_t *wq);
void waitq_wake_one(waitq_t *wq);
void waitq_wake_all(waitq_t *wq);

void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
