; entry.asm - kernel entry point. The loader jumps here with RDI = boot info.
bits 64

section .text.entry
global _start
extern kmain

_start:
    cli
    cld
    mov rsp, stack_top          ; switch to the kernel's own stack
    xor rbp, rbp
    call kmain                  ; kmain(ntos_boot_info *bi) - never returns
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 65536
stack_top:
