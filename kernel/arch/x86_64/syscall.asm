; syscall.asm - SYSCALL/SYSRET entry and the first jump into ring 3.
;
; User ABI: RAX = number, args in RDI RSI RDX R10 R8 R9, result in RAX.
; SYSCALL leaves the user RIP in RCX and RFLAGS in R11 and masks IF, so the
; scratch slot below is safe on a single CPU until we are on the kernel stack.
bits 64

section .text
global syscall_entry
global enter_user
extern syscall_dispatch

syscall_entry:
    mov [rel syscall_user_rsp], rsp
    mov rsp, [rel syscall_kernel_rsp]   ; this thread's kernel stack (set by the scheduler)
    push qword [rel syscall_user_rsp]   ; user RSP
    push r11                            ; user RFLAGS
    push rcx                            ; user RIP
    push rax
    push rbx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r12
    push r13
    push r14
    push r15                            ; 16 pushes: stack stays 16-byte aligned
    mov rdi, rsp                        ; syscall_frame*
    sti
    cld
    call syscall_dispatch
    cli
    pop r15
    pop r14
    pop r13
    pop r12
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    pop rax
    pop rcx
    pop r11
    pop rsp
    o64 sysret

; NORETURN void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1)
; Builds an IRET frame for ring 3; arg0/arg1 arrive in the program's RDI/RSI.
enter_user:
    cli
    push qword 0x1B                     ; SS  = user data | RPL 3
    push rsi                            ; RSP
    push qword 0x202                    ; RFLAGS: IF set
    push qword 0x23                     ; CS  = user code | RPL 3
    push rdi                            ; RIP
    mov rdi, rdx
    mov rsi, rcx
    xor eax, eax                        ; leak no kernel values into user registers
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

section .data
global syscall_kernel_rsp
align 8
syscall_kernel_rsp: dq 0
syscall_user_rsp:   dq 0
