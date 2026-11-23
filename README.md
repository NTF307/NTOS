# NTOS

A 64-bit operating system written from scratch in C and assembly. Boots on UEFI
x86_64 machines: VMware, QEMU, and real PCs (Secure Boot off).

## Build

1. Open PowerShell in `C:\Users\Admin\Documents\Code\NTOS`.
2. Run `.\build.ps1`
   Success: the last line is `== OK: ISO\NTOS-<version>-buildNNNN.iso`.

Every build is kept in `ISO\` and also copied to `ISO\NTOS-latest.iso`.

## Test in QEMU

1. In `C:\Users\Admin\Documents\Code\NTOS`, run `.\build.ps1 -Test`
   Success: `RESULT: PASS - kernel reached the shell, 8/8 checks passed`; screenshot at `build\test\screen.png`.
   The test injects mouse motion and runs `heaptest`, `threadtest`, `spawn`, `ps` and `mouse`.
2. Or run `.\build.ps1 -Run` for an interactive QEMU window (serial log in the terminal).

## Boot in VMware Workstation

1. In VMware: **File → Open…** → `C:\Users\Admin\Documents\Code\NTOS\vmware\NTOS.vmx`
2. Click **Power on this virtual machine**.
   Success: the NTOS logo, `[ OK ]` lines, and an `ntos:/>` prompt.

The VM boots `ISO\NTOS-latest.iso` (UEFI, Secure Boot off). The kernel's serial log
is written to `vmware\serial.log`. To test an older build, point the VM's CD drive
at any file in `ISO\`.

## Boot a real PC

1. Write `ISO\NTOS-latest.iso` to a USB stick with Rufus (ISO or DD mode).
2. In the PC's firmware setup: disable Secure Boot, enable UEFI boot.
3. Boot from the USB stick.

Keyboard and mouse currently need PS/2 devices or a firmware with USB legacy
emulation; native USB comes later.

## Shell commands

`help`, `info`, `mem`, `uptime`, `heaptest`, `ps` (threads), `kill <tid>`,
`spawn [n]` (CPU-bound test threads), `sleep <ms>`, `threadtest` (scheduler + mutex
self-test), `mouse [live]`, `keys` (raw key events), `crash [ud|pf|div]`, `reboot`, `halt`.
Ctrl+Alt+Del reboots.

## Layout

| Path | Contents |
|---|---|
| `boot/` | UEFI loader (`BOOTX64.EFI`): GOP, ELF loading, page tables, ExitBootServices |
| `include/bootinfo.h` | Loader ↔ kernel handoff structure and memory layout |
| `kernel/arch/x86_64/` | Entry, GDT/TSS, IDT + exception handler, PIC/LAPIC, PIT timer, context switch |
| `kernel/drivers/` | Framebuffer console + font + mouse pointer, serial, 8042 PS/2 controller, keyboard, mouse |
| `kernel/sched/` | Kernel threads, preemptive round-robin scheduler (1000 Hz timer, 10 ms quantum), wait queues, mutexes |
| `kernel/mm/` | Bitmap physical allocator, kernel heap |
| `kernel/shell/` | Built-in shell (`help` lists commands) |
| `tools/mkiso.py` | Builds the hybrid UEFI ISO (ISO 9660 + El Torito + FAT16 + MBR) |
| `tools/qemu_test.py` | Headless QEMU boot test with keystrokes + screenshot |

## Memory layout

| Virtual address | Maps |
|---|---|
| `0x0000000000000000` | Identity map of the low physical memory (temporary) |
| `0xFFFF800000000000` | All physical memory (HHDM), 2 MiB pages |
| `0xFFFFFFFF80000000` | Kernel image |
