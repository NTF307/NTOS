; entry.asm - kernel entry point. The loader jumps here with RDI = boot info.
bits 64

section .text.entry
global _start
global boot_stack_bottom
global boot_stack_top
extern kmain

_start:
    cli
    cld
    mov rsp, boot_stack_top          ; switch to the kernel's own stack
    xor rbp, rbp
    call kmain                  ; kmain(ntos_boot_info *bi) - never returns
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
boot_stack_bottom:
    resb 65536
boot_stack_top:
