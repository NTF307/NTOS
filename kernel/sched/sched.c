/*
 * sched.c - preemptive round-robin scheduler for kernel threads.
 *
 * Every thread has its own kernel stack. A switch happens either voluntarily
 * (yield, sleep, block, exit) or when the timer IRQ uses up the running
 * thread's quantum; in that case the switch happens at the end of the IRQ,
 * after EOI, and the interrupted thread later resumes through iretq.
 * schedule() always runs with interrupts disabled.
 */
#include "sched.h"
#include "arch.h"
#include "lib.h"
#include "mm.h"

#define STACK_CANARY 0x4E544F53535441CBULL

extern void    context_switch(uint64_t *save_rsp, uint64_t new_rsp);
extern void    thread_trampoline(void);
extern uint8_t boot_stack_bottom[];
extern uint8_t boot_stack_top[];

static thread_t  boot_thread;
static thread_t *current;
static thread_t *idle_thread;
static thread_t *rq_head, *rq_tail;     /* ready queue (FIFO) */
static thread_t *sleepers;              /* unordered; scanned each tick */
static thread_t *zombies;               /* exited, waiting for the idle thread to free them */
static thread_t *all_threads;
static uint32_t  next_tid = 1;
static uint32_t  quantum, quantum_ticks = 1;
static volatile bool need_resched;
static uint64_t  total_switches;
static bool      active;

/* ------------------------------------------------------------ queues */
static void rq_push(thread_t *t)
{
    t->next = NULL;
    if (rq_tail) rq_tail->next = t; else rq_head = t;
    rq_tail = t;
}

static thread_t *rq_pop(void)
{
    thread_t *t = rq_head;
    if (t) {
        rq_head = t->next;
        if (!rq_head) rq_tail = NULL;
        t->next = NULL;
    }
    return t;
}

/* Remove t from a singly linked FIFO; fixes up the tail pointer. */
static void fifo_remove(thread_t **head, thread_t **tail, thread_t *t)
{
    thread_t *prev = NULL;
    for (thread_t *c = *head; c; prev = c, c = c->next) {
        if (c != t) continue;
        if (prev) prev->next = c->next; else *head = c->next;
        if (*tail == c) *tail = prev;
        c->next = NULL;
        return;
    }
}

static void make_ready(thread_t *t)
{
    t->state = THREAD_READY;
    t->wq = NULL;
    rq_push(t);
}

/* ---------------------------------------------------------- schedule */
static void check_stack(const thread_t *t)
{
    if (t->stack && *(const uint64_t *)t->stack != STACK_CANARY)
        panic("kernel stack overflow in thread %u (%s)", t->tid, t->name);
}

static void schedule(void)
{
    thread_t *prev = current;
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        if (prev != idle_thread) rq_push(prev);
    }
    thread_t *next = rq_pop();
    if (!next) next = idle_thread;
    need_resched = false;
    quantum = quantum_ticks;
    next->state = THREAD_RUNNING;
    if (next == prev) return;

    check_stack(prev);
    total_switches++;
    next->switches++;
    current = next;
    context_switch(&prev->rsp, next->rsp);
    /* back in prev, possibly much later */
}

/* --------------------------------------------------------- idle/reap */
static void reap_zombies(void)
{
    cpu_cli();
    thread_t *list = zombies;
    zombies = NULL;
    for (thread_t *z = list; z; z = z->next)
        for (thread_t **p = &all_threads; *p; p = &(*p)->all_next)
            if (*p == z) { *p = z->all_next; break; }
    cpu_sti();
    while (list) {
        thread_t *n = list->next;
        pmm_free_pages((uint64_t)list->stack - g_hhdm, list->stack_size / PAGE_SIZE);
        kfree(list);
        list = n;
    }
}

static void idle_main(void *arg)
{
    (void)arg;
    for (;;) {
        if (zombies) reap_zombies();
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

/* -------------------------------------------------------------- init */
static void register_thread(thread_t *t, const char *name)
{
    strncpy(t->name, name, THREAD_NAME_MAX - 1);
    t->name[THREAD_NAME_MAX - 1] = 0;
    t->tid = next_tid++;
    t->all_next = all_threads;
    all_threads = t;
}

void sched_init(void)
{
    uint64_t f = irq_save();
    quantum_ticks = (SCHED_QUANTUM_MS * timer_hz() + 999) / 1000;
    if (!quantum_ticks) quantum_ticks = 1;

    /* The code running right now (kmain, then the shell) becomes thread 1. */
    boot_thread.stack = boot_stack_bottom;
    boot_thread.stack_size = (size_t)(boot_stack_top - boot_stack_bottom);
    *(uint64_t *)boot_thread.stack = STACK_CANARY;
    boot_thread.state = THREAD_RUNNING;
    register_thread(&boot_thread, "kernel");
    current = &boot_thread;

    idle_thread = thread_create("idle", idle_main, NULL);
    if (!idle_thread) panic("sched: cannot create idle thread");
    fifo_remove(&rq_head, &rq_tail, idle_thread);   /* idle only runs when nothing else can */
    quantum = quantum_ticks;
    active = true;
    irq_restore(f);
}

bool        sched_active(void)           { return active; }
uint64_t    sched_context_switches(void) { return total_switches; }
thread_t   *thread_current(void)         { return current; }

const char *thread_state_name(thread_state s)
{
    static const char *const names[] = { "ready", "running", "sleeping", "blocked", "dead" };
    return (unsigned)s < ARRAY_SIZE(names) ? names[s] : "?";
}

/* ----------------------------------------------------------- threads */
thread_t *thread_create(const char *name, thread_fn fn, void *arg)
{
    thread_t *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    uint64_t phys = pmm_alloc_pages(THREAD_STACK_SIZE / PAGE_SIZE);
    if (!phys) { kfree(t); return NULL; }
    t->stack = phys_to_virt(phys);
    t->stack_size = THREAD_STACK_SIZE;
    *(uint64_t *)t->stack = STACK_CANARY;

    /* Initial frame, popped by context_switch: r15 r14 r13 r12 rbx rbp, ret -> trampoline. */
    uint64_t *sp = (uint64_t *)(t->stack + THREAD_STACK_SIZE);
    *--sp = 0;
    *--sp = (uint64_t)thread_trampoline;
    *--sp = 0;                  /* rbp */
    *--sp = 0;                  /* rbx */
    *--sp = (uint64_t)fn;       /* r12 */
    *--sp = (uint64_t)arg;      /* r13 */
    *--sp = 0;                  /* r14 */
    *--sp = 0;                  /* r15 */
    t->rsp = (uint64_t)sp;

    uint64_t f = irq_save();
    register_thread(t, name);
    make_ready(t);
    irq_restore(f);
    return t;
}

void thread_yield(void)
{
    if (!active) return;
    uint64_t f = irq_save();
    schedule();
    irq_restore(f);
}

void thread_sleep_ms(uint64_t ms)
{
    uint64_t ticks = (ms * timer_hz() + 999) / 1000;
    if (!ticks) ticks = 1;
    if (!active || current == idle_thread) {
        uint64_t target = timer_ticks() + ticks;
        while (timer_ticks() < target) cpu_hlt();
        return;
    }
    uint64_t f = irq_save();
    current->wake_tick = timer_ticks() + ticks;
    current->state = THREAD_SLEEPING;
    current->next = sleepers;
    sleepers = current;
    schedule();
    irq_restore(f);
}

void thread_exit(void)
{
    cpu_cli();
    if (current == &boot_thread || current == idle_thread) panic("sched: thread %u tried to exit", current->tid);
    current->state = THREAD_DEAD;
    current->next = zombies;
    zombies = current;
    schedule();
    panic("sched: dead thread resumed");
}

int thread_kill(uint32_t tid)
{
    uint64_t f = irq_save();
    thread_t *t = all_threads;
    while (t && (t->tid != tid || t->state == THREAD_DEAD)) t = t->all_next;
    if (!t) { irq_restore(f); return -1; }
    if (t == &boot_thread || t == idle_thread) { irq_restore(f); return -2; }
    if (t == current) thread_exit();

    switch (t->state) {
    case THREAD_READY:    fifo_remove(&rq_head, &rq_tail, t); break;
    case THREAD_SLEEPING: { thread_t *dummy = NULL; fifo_remove(&sleepers, &dummy, t); break; }
    case THREAD_BLOCKED:  if (t->wq) fifo_remove(&t->wq->head, &t->wq->tail, t); break;
    default: break;
    }
    t->state = THREAD_DEAD;
    t->wq = NULL;
    t->next = zombies;
    zombies = t;
    irq_restore(f);
    return 0;
}

size_t thread_list(thread_info *out, size_t max)
{
    size_t n = 0;
    uint64_t f = irq_save();
    for (thread_t *t = all_threads; t && n < max; t = t->all_next, n++) {
        out[n].tid = t->tid;
        out[n].state = t->state;
        memcpy(out[n].name, t->name, THREAD_NAME_MAX);
        out[n].cpu_ticks = t->cpu_ticks;
        out[n].switches = t->switches;
        out[n].stack_size = t->stack_size;
    }
    irq_restore(f);
    /* all_threads is newest-first; show oldest first */
    for (size_t i = 0; i < n / 2; i++) {
        thread_info tmp = out[i]; out[i] = out[n - 1 - i]; out[n - 1 - i] = tmp;
    }
    return n;
}

/* ------------------------------------------------------ IRQ context */
void sched_tick(void)
{
    if (!active) return;
    current->cpu_ticks++;

    uint64_t now = timer_ticks();
    for (thread_t **p = &sleepers; *p;) {
        thread_t *t = *p;
        if (t->wake_tick <= now) { *p = t->next; make_ready(t); }
        else p = &t->next;
    }

    if (current == idle_thread) { if (rq_head) need_resched = true; }
    else if (quantum && --quantum == 0) need_resched = true;
}

void sched_irq_exit(void)
{
    if (active && need_resched) schedule();
}

/* ------------------------------------------------------- wait queues */
void waitq_wait(waitq_t *wq)
{
    if (!active || current == idle_thread) {          /* before the scheduler: just wait for an IRQ */
        __asm__ volatile("sti; hlt; cli" ::: "memory");
        return;
    }
    current->state = THREAD_BLOCKED;
    current->wq = wq;
    current->next = NULL;
    if (wq->tail) wq->tail->next = current; else wq->head = current;
    wq->tail = current;
    schedule();
}

static thread_t *wq_pop(waitq_t *wq)
{
    thread_t *t = wq->head;
    if (t) {
        wq->head = t->next;
        if (!wq->head) wq->tail = NULL;
        t->next = NULL;
    }
    return t;
}

/* A woken thread is usually interactive (input arrived): let it run soon. */
void waitq_wake_one(waitq_t *wq)
{
    uint64_t f = irq_save();
    thread_t *t = wq_pop(wq);
    if (t) { make_ready(t); need_resched = true; }
    irq_restore(f);
}

void waitq_wake_all(waitq_t *wq)
{
    uint64_t f = irq_save();
    thread_t *t;
    bool any = false;
    while ((t = wq_pop(wq))) { make_ready(t); any = true; }
    if (any) need_resched = true;
    irq_restore(f);
}

/* ----------------------------------------------------------- mutexes */
void mutex_lock(mutex_t *m)
{
    if (!active || g_panicking) return;
    uint64_t f = irq_save();
    if (m->owner == current) { m->depth++; irq_restore(f); return; }
    while (m->owner) waitq_wait(&m->wq);
    m->owner = current;
    m->depth = 1;
    irq_restore(f);
}

void mutex_unlock(mutex_t *m)
{
    if (!active || g_panicking) return;
    uint64_t f = irq_save();
    if (m->owner == current && --m->depth == 0) {
        m->owner = NULL;
        thread_t *t = wq_pop(&m->wq);
        if (t) { make_ready(t); need_resched = true; }
    }
    irq_restore(f);
}
