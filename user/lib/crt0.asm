; crt0.asm - user program entry. The kernel starts us with RDI = argc, RSI = argv.
bits 64

section .text.start
global _start
extern main
extern exit

_start:
    xor rbp, rbp
    and rsp, -16
    call main
    mov edi, eax
    call exit
.hang:
    jmp .hang
