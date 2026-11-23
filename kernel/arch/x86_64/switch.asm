; switch.asm - kernel thread context switch
bits 64

section .text
global context_switch
global thread_trampoline
extern thread_exit

; void context_switch(uint64_t *save_rsp, uint64_t new_rsp)
; Saves the callee-saved registers on the current stack, stores RSP in *save_rsp,
; loads new_rsp and restores the registers saved there. Interrupts must be off.
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First code a new thread runs: thread_create() leaves R12 = entry, R13 = argument.
thread_trampoline:
    sti                         ; schedule() switched to us with interrupts off
    xor rbp, rbp                ; terminate stack traces here
    and rsp, -16
    mov rdi, r13
    call r12
    call thread_exit            ; never returns
.hang:
    hlt
    jmp .hang
