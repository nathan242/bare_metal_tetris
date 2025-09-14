; kernel_entry.asm — 32-bit entry stub that calls main()
; Assemble: nasm -f elf32 kernel_entry.asm -o kernel_entry.o

bits 32
global _start
extern main

section .text
_start:
    ; At this point: flat 32-bit mode, segments already flat from bootloader
    ; (If you want your own stack here, you can set ESP again.)
    call main
.hang:
    hlt
    jmp .hang
