/* process.c - create, run, kill and reap user processes */
#include "proc.h"
#include "lib.h"
#include "console.h"
#include "theme.h"

static process_t        *procs;         /* every process not yet reaped */
static volatile uint32_t foreground;    /* pid that Ctrl+C kills, 0 = none */

process_t *process_current(void) { return thread_current()->proc; }

static process_t *find(uint32_t pid)
{
    for (process_t *p = procs; p; p = p->next) if (p->pid == pid) return p;
    return NULL;
}

static void unlink_proc(process_t *p)
{
    for (process_t **pp = &procs; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; return; }
}

/* Kernel side of a new user thread: the scheduler already loaded its page tables. */
static void user_thread_start(void *arg)
{
    process_t *p = arg;
    enter_user(p->entry, p->user_sp, p->argc, p->argv);
}

static const char *basename(const char *path)
{
    const char *b = path;
    for (const char *s = path; *s; s++) if (*s == '/') b = s + 1;
    return b;
}

int process_spawn(const char *path, int argc, const char *const *argv, uint32_t parent, bool detached)
{
    const uint8_t *image;
    uint64_t image_size;
    if (!initrd_find(path, &image, &image_size)) return -E_NOENT;
    if (argc < 0 || argc > PROC_ARGS_MAX) return -E_INVAL;
    size_t arg_bytes = 0;
    for (int i = 0; i < argc; i++) arg_bytes += strlen(argv[i]) + 1;
    if (arg_bytes > PROC_ARG_BYTES) return -E_INVAL;

    address_space *as = vmm_create();
    if (!as) return -E_NOMEM;
    uint64_t entry, image_end;
    int r = elf_load(as, image, image_size, &entry, &image_end);
    if (r) goto fail;

    r = -E_NOMEM;
    for (int i = 1; i <= USER_STACK_PAGES; i++)
        if (vmm_alloc(as, USER_STACK_TOP - (uint64_t)i * PAGE_SIZE, VMM_USER | VMM_WRITE)) goto fail;

    /* Arguments at the top of the user stack: strings, then the argv[] array. */
    uint64_t sp = USER_STACK_TOP, uargv[PROC_ARGS_MAX + 1];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        if (vmm_write(as, sp, argv[i], len)) goto fail;
        uargv[i] = sp;
    }
    uargv[argc] = 0;
    sp = ALIGN_DOWN(sp, 16) - ALIGN_UP((uint64_t)(argc + 1) * 8, 16);
    if (vmm_write(as, sp, uargv, (uint64_t)(argc + 1) * 8)) goto fail;
    uint64_t argv_user = sp;
    sp -= 8;                                     /* fake return address: ABI alignment at entry */
    uint64_t zero = 0;
    if (vmm_write(as, sp, &zero, 8)) goto fail;

    process_t *p = kzalloc(sizeof *p);
    if (!p) goto fail;
    strncpy(p->name, basename(path), THREAD_NAME_MAX - 1);
    p->as = as;
    p->parent = parent;
    p->detached = detached;
    p->entry = entry;
    p->user_sp = sp;
    p->argc = (uint64_t)argc;
    p->argv = argv_user;
    p->brk_start = p->brk = image_end;

    thread_t *t = thread_create_stopped(p->name, user_thread_start, p);
    if (!t) { kfree(p); goto fail; }
    p->thread = t;
    p->pid = t->tid;
    t->proc = p;
    t->cr3 = as->pml4;

    uint64_t f = irq_save();
    p->next = procs;
    procs = p;
    irq_restore(f);
    thread_start(t);
    return (int)p->pid;

fail:
    vmm_destroy(as);
    return r;
}

void process_exit(int code)
{
    thread_t *t = thread_current();
    process_t *p = t->proc;
    if (!p) thread_exit();

    /* Detach the thread from the address space before freeing it. */
    uint64_t f = irq_save();
    t->proc = NULL;
    t->cr3 = 0;
    write_cr3(vmm_kernel_pml4());
    irq_restore(f);
    vmm_destroy(p->as);
    p->as = NULL;

    f = irq_save();
    p->exit_code = code;
    p->state = PROC_ZOMBIE;
    p->thread = NULL;
    if (foreground == p->pid) foreground = 0;

    /* Orphans: nobody will wait for them any more. */
    for (process_t *c = procs, *n; c; c = n) {
        n = c->next;
        if (c->parent != p->pid) continue;
        c->parent = 0;
        c->detached = true;
        if (c->state == PROC_ZOMBIE) { unlink_proc(c); kfree(c); }
    }

    if (p->detached) { unlink_proc(p); kfree(p); }
    else waitq_wake_all(&p->exit_wq);
    thread_exit();                               /* interrupts stay off until we are switched out */
}

int process_wait(uint32_t pid, uint32_t parent, int *exit_code)
{
    uint64_t f = irq_save();
    process_t *p = find(pid);
    if (!p || p->detached || p->parent != parent) { irq_restore(f); return -E_CHILD; }
    while (p->state != PROC_ZOMBIE) {
        if (thread_kill_pending()) { irq_restore(f); return -E_INTR; }
        waitq_wait(&p->exit_wq);
    }
    if (exit_code) *exit_code = p->exit_code;
    unlink_proc(p);
    irq_restore(f);
    kfree(p);
    return (int)pid;
}

void process_kill(uint32_t pid)
{
    uint64_t f = irq_save();
    process_t *p = find(pid);
    if (p && p->state == PROC_RUNNING && p->thread) {
        p->thread->kill_pending = true;
        thread_interrupt(p->thread);
    }
    irq_restore(f);
}

void process_check_kill(void)
{
    thread_t *t = thread_current();
    if (t->proc && t->kill_pending) process_exit(-9);
}

void process_fault(interrupt_frame *f, const char *what)
{
    process_t *p = process_current();
    uint32_t fg, bg;
    console_get_color(&fg, &bg);
    console_set_color(THEME_ERROR, bg);
    kprintf("\n%s[%u]: %s", p->name, p->pid, what);
    if (f->vector == 14)
        kprintf(" at 0x%lx (%s, %s)", read_cr2(), (f->error & 1) ? "protection" : "not present",
                (f->error & 2) ? "write" : (f->error & 16) ? "execute" : "read");
    kprintf(", rip 0x%lx - process killed\n", f->rip);
    console_set_color(fg, bg);
    process_exit(-1);
}

void process_set_foreground(uint32_t pid) { foreground = pid; }

bool process_interrupt_foreground(void)
{
    uint32_t pid = foreground;
    if (!pid) return false;
    process_kill(pid);
    return true;
}

size_t process_list(process_info *out, size_t max)
{
    size_t n = 0;
    uint64_t f = irq_save();
    for (process_t *p = procs; p && n < max; p = p->next, n++) {
        out[n].pid = p->pid;
        out[n].parent = p->parent;
        out[n].state = p->state;
        memcpy(out[n].name, p->name, THREAD_NAME_MAX);
        out[n].pages = 0;
    }
    irq_restore(f);
    return n;
}
