#!/bin/bash

nasm -f bin loader.asm -o loader.bin
nasm -f elf32 kernel_entry.asm -o kernel_entry.o
nasm -f elf32 isr_stub.asm -o isr_stub.o
gcc -m32 -ffreestanding -fno-pic -fno-pie -nostdlib -c kernel.c -o kernel.o
ld -m elf_i386 -T linker.ld -nostdlib kernel_entry.o isr_stub.o kernel.o -o kernel.elf
objcopy -O binary kernel.elf kernel.bin

cat loader.bin kernel.bin > boot.img

truncate -s 1474560 boot.img
