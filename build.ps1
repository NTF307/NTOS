<#
.SYNOPSIS
    Build NTOS (UEFI loader + kernel) into a bootable ISO.

.DESCRIPTION
    Every successful build gets a new build number and is saved as
    ISO\NTOS-<version>-buildNNNN.iso, plus a copy at ISO\NTOS-latest.iso
    (the file the VMware VM in vmware\NTOS.vmx boots from).

.EXAMPLE
    .\build.ps1                 # build a new ISO
    .\build.ps1 -Run            # build, then boot it in a QEMU window
    .\build.ps1 -Test           # build, boot headless in QEMU, type commands, save a screenshot
#>
param(
    [switch]$Run,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot

# ---------------------------------------------------------------- tools
$LLVM    = 'C:\Program Files\LLVM\bin'
$Clang   = Join-Path $LLVM 'clang.exe'
$LldLink = Join-Path $LLVM 'lld-link.exe'
$LdLld   = Join-Path $LLVM 'ld.lld.exe'
$Nasm    = Join-Path $env:LOCALAPPDATA 'bin\NASM\nasm.exe'
if (-not (Test-Path $Nasm)) { $Nasm = 'C:\Program Files\NASM\nasm.exe' }
$Qemu    = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$Python  = (Get-Command python -ErrorAction SilentlyContinue).Source

foreach ($t in @($Clang, $LldLink, $LdLld, $Nasm)) {
    if (-not (Test-Path $t)) { throw "Missing tool: $t (install with: winget install LLVM.LLVM NASM.NASM)" }
}
if (-not $Python) { throw 'Missing tool: python (3.8+)' }

function Invoke-Tool([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$([IO.Path]::GetFileName($Exe)) failed (exit $LASTEXITCODE)" }
}

# -------------------------------------------------------------- version
$Version  = (Get-Content (Join-Path $Root 'VERSION') -Raw).Trim()
$BuildFile = Join-Path $Root '.buildnum'
$Build    = 1
if (Test-Path $BuildFile) { $Build = [int](Get-Content $BuildFile -Raw).Trim() + 1 }
$BuildStr = '{0:D4}' -f $Build
$Date     = Get-Date -Format 'yyyy-MM-dd HH:mm'

$Out   = Join-Path $Root 'build'
$Obj   = Join-Path $Out 'obj'
$Gen   = Join-Path $Out 'gen'
$Esp   = Join-Path $Out 'esp'
if (Test-Path $Obj) { Remove-Item $Obj -Recurse -Force }
if (Test-Path $Esp) { Remove-Item $Esp -Recurse -Force }
foreach ($d in @($Obj, $Gen, "$Esp\EFI\BOOT", "$Esp\NTOS", (Join-Path $Root 'ISO'))) {
    New-Item -ItemType Directory -Force $d | Out-Null
}

@"
#pragma once
#define NTOS_VERSION     "$Version"
#define NTOS_BUILD       $Build
#define NTOS_BUILD_STR   "$BuildStr"
#define NTOS_BUILD_DATE  "$Date"
"@ | Set-Content -Encoding ascii (Join-Path $Gen 'version.h')

Write-Host "== NTOS $Version build $BuildStr" -ForegroundColor Cyan

# --------------------------------------------------------------- loader
$LoaderFlags = @('--target=x86_64-unknown-windows', '-std=c17', '-O2', '-Wall', '-Wextra',
                 '-ffreestanding', '-fno-stack-protector', '-fshort-wchar', '-mno-red-zone',
                 '-mno-stack-arg-probe', '-mgeneral-regs-only', '-nostdlibinc', "-I$Gen")
Write-Host '-- loader'
Invoke-Tool $Clang ($LoaderFlags + @('-c', "$Root\boot\loader.c", '-o', "$Obj\loader.obj"))
Invoke-Tool $LldLink @('/nologo', '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
                       '/machine:x64', "/out:$Esp\EFI\BOOT\BOOTX64.EFI", "$Obj\loader.obj")

# --------------------------------------------------------------- kernel
$KernelFlags = @('--target=x86_64-unknown-none-elf', '-std=c17', '-O2', '-g', '-Wall', '-Wextra',
                 '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-mno-red-zone',
                 '-mcmodel=kernel', '-mgeneral-regs-only', '-fno-omit-frame-pointer', '-nostdlibinc',
                 "-I$Root\kernel\include", "-I$Gen")
Write-Host '-- kernel'
$Objects = @()
$Sources = Get-ChildItem "$Root\kernel" -Recurse -Include *.c, *.asm
foreach ($src in $Sources) {
    $rel = $src.FullName.Substring($Root.Length + 1)
    $o = Join-Path $Obj ($rel + '.o')
    New-Item -ItemType Directory -Force (Split-Path $o) | Out-Null
    Write-Host "   $rel"
    if ($src.Extension -eq '.asm') { Invoke-Tool $Nasm @('-f', 'elf64', '-o', $o, $src.FullName) }
    else { Invoke-Tool $Clang ($KernelFlags + @('-c', $src.FullName, '-o', $o)) }
    $Objects += $o
}
Invoke-Tool $LdLld (@('-T', "$Root\kernel\linker.ld", '-nostdlib', '-static', '-z', 'max-page-size=0x1000',
                      '-o', "$Esp\NTOS\KERNEL.ELF") + $Objects)

# ---------------------------------------------------------- user programs
$UserFlags = @('--target=x86_64-unknown-none-elf', '-std=c17', '-O2', '-Wall', '-Wextra',
               '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-fno-builtin',
               '-mgeneral-regs-only', '-nostdlibinc', "-I$Root\user\include")
Write-Host '-- user'
$UserObj = Join-Path $Obj 'user'
$UserOut = Join-Path $Out 'user'
foreach ($d in @($UserObj, $UserOut)) { New-Item -ItemType Directory -Force $d | Out-Null }
Get-ChildItem $UserOut -Filter *.elf | Remove-Item -Force
Invoke-Tool $Nasm @('-f', 'elf64', '-o', "$UserObj\crt0.o", "$Root\user\lib\crt0.asm")
Invoke-Tool $Clang ($UserFlags + @('-c', "$Root\user\lib\libc.c", '-o', "$UserObj\libc.o"))
$Programs = @()
foreach ($src in Get-ChildItem "$Root\user\bin" -Filter *.c) {
    Write-Host "   user\bin\$($src.Name)"
    $o = Join-Path $UserObj ($src.BaseName + '.o')
    $elf = Join-Path $UserOut ($src.BaseName + '.elf')
    Invoke-Tool $Clang ($UserFlags + @('-c', $src.FullName, '-o', $o))
    Invoke-Tool $LdLld @('-T', "$Root\user\user.ld", '-nostdlib', '-static', '-z', 'max-page-size=0x1000',
                         '-o', $elf, "$UserObj\crt0.o", $o, "$UserObj\libc.o")
    $Programs += $elf
}
Invoke-Tool $Python (@("$Root\tools\mkinitrd.py", '--out', "$Esp\NTOS\INITRD.TAR") + $Programs)

# ------------------------------------------------------------------ ISO
Write-Host '-- iso'
$IsoTmp = Join-Path $Out 'ntos.iso'
Invoke-Tool $Python @("$Root\tools\mkiso.py", '--esp', $Esp, '--out', $IsoTmp, '--label', 'NTOS')

$IsoName = "NTOS-$Version-build$BuildStr.iso"
$IsoPath = Join-Path $Root "ISO\$IsoName"
$Latest  = Join-Path $Root 'ISO\NTOS-latest.iso'
Copy-Item $IsoTmp $IsoPath -Force
try { Copy-Item $IsoTmp $Latest -Force }
catch { Write-Warning "Could not update NTOS-latest.iso (is the VM running?). Use $IsoName instead." }
Set-Content -Encoding ascii $BuildFile $Build

# --------------------------------------------------------- VMware config
$VmDir = Join-Path $Root 'vmware'
$Vmx = Join-Path $VmDir 'NTOS.vmx'
if (-not (Test-Path $Vmx)) {
    New-Item -ItemType Directory -Force $VmDir | Out-Null
    @"
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "NTOS"
guestOS = "other-64"
firmware = "efi"
uefi.secureBoot.enabled = "FALSE"
memsize = "1024"
numvcpus = "1"
pciBridge0.present = "TRUE"
pciBridge4.present = "TRUE"
pciBridge4.virtualDev = "pcieRootPort"
pciBridge4.functions = "8"
svga.present = "TRUE"
sata0.present = "TRUE"
sata0:1.present = "TRUE"
sata0:1.deviceType = "cdrom-image"
sata0:1.fileName = "$Latest"
sata0:1.startConnected = "TRUE"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "serial.log"
floppy0.present = "FALSE"
ethernet0.present = "FALSE"
sound.present = "FALSE"
usb.present = "FALSE"
"@ | Set-Content -Encoding utf8 $Vmx
    Write-Host "-- created VMware VM: $Vmx"
}

$size = [math]::Round((Get-Item $IsoPath).Length / 1MB, 1)
Write-Host "== OK: ISO\$IsoName ($size MiB)" -ForegroundColor Green

# ------------------------------------------------------------- run/test
if ($Test) {
    Invoke-Tool $Python @("$Root\tools\qemu_test.py", $IsoPath, '--out', "$Out\test",
                          '--mouse', '300,200',
                          '--type', 'heaptest', '--type', 'threadtest', '--type', 'ls',
                          '--type', 'hello one two', '--type', 'usertest',
                          '--type', 'fault kernel', '--type', 'fault badptr',
                          '--type', 'ask', '--type', 'NTOS', '--type', 'spin', '--type', '^C',
                          '--type', 'counter 2 200 &', '--type', 'ps', '--type', 'mouse',
                          '--expect', 'Scheduler: preemptive', '--expect', 'PS/2 keyboard ready',
                          '--expect', 'PS/2 mouse ready', '--expect', 'System calls: SYSCALL/SYSRET',
                          '--expect', 'Initrd:', '--expect', 'heaptest: 64 allocations verified',
                          '--expect', 'threadtest: PASS', '--expect', 'Hello from user mode',
                          '--expect', 'argv[2] = ', '--expect', 'usertest: PASS',
                          '--expect', 'fault[', '--expect', 'process killed', '--expect', 'EFAULT - ok',
                          '--expect', 'Hello, NTOS!', '--expect', '[killed]', '--expect', '[counter',
                          '--expect', 'context switches since boot', '--expect', 'packets')
}
elseif ($Run) {
    Copy-Item 'C:\Program Files\qemu\share\edk2-i386-vars.fd' "$Out\vars.fd" -Force
    & $Qemu -machine q35 -m 512M `
        -drive "if=pflash,format=raw,unit=0,readonly=on,file=C:\Program Files\qemu\share\edk2-x86_64-code.fd" `
        -drive "if=pflash,format=raw,unit=1,file=$Out\vars.fd" `
        -cdrom $IsoPath -serial stdio
}
