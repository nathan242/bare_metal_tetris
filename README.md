# Bare Metal Tetris

Bootable bare metal tetris game for X86.

### Compiling

Run ./build.sh to compile and build a bootable floppy image (boot.img).

Test using a virtual machine such as QEMU:

```
qemu-system-i386 -fda boot.img
```
