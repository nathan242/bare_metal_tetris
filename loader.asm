bits 16
org 0x7c00 ; Start address for boot sector

; Set up the stack
mov bp, 0x9000
mov sp, bp

call load_program
call pm_run_program
jmp $

print_string:
    mov ah, 0x0e ; Teletype output

.print_loop:
    lodsb ; Load byte at SI into AL
    cmp al, 0 ; Check if byte is a NULL
    je .print_done
    int 0x10
    jmp .print_loop

.print_done:
    ret

halt_error:
    mov si, errormsg
    call print_string
    jmp $ ; Jump to self - infinite loop

load_program:
    mov si, loadingmsg
    call print_string

    xor ax, ax ; Zeros out AX
    mov ds, ax ; Zeros out DS
    ; mov es, ax ; Zeros out ES
    mov ax, 0x1000
    mov es, ax
    xor bx, bx
    cld ; Clear direction flag

    mov ah, 0x02 ; Read sectors
    ; mov bx, 0x1000 ; In ES
    mov al, 22 ; Number of sectors to read
    mov dl, [boot_drive] ; Drive number
    mov ch, 0 ; Cylinder number
    mov dh, 0 ; Head number
    mov cl, 2 ; Sector number

    int 0x13
    jc halt_error ; Jump if carry flag set. Will happen if 0x13 errors

    mov si, okmsg
    call print_string

    ret

pm_run_program:
    mov si, startmsg

    cli ; Disable interrupts

    ; === Enable A20 (fast gate at port 0x92) ===
    in  al, 0x92
    or  al, 00000010b
    out 0x92, al

    ; === Build and load GDT ===
    lgdt [gdt_descriptor]

    ; === Enter protected mode ===
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:program_entry ; far jump to flush prefetch & load CS

    ret

; ---------------- 32-bit code here ----------------
bits 32
program_entry:
    ; Set data segments to flat data selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; mov esp, 0x0090000             ; simple 32-bit stack (below 1MB, A20 already on)
    mov esp, 0x200000

    ; Jump to loaded kernel's entry (flat, no paging)
    mov eax, 0x0010000
    jmp eax                        ; kernel entry at physical/linear 0x0010000

; GDT
align 8
gdt_start:
    ; null descriptor
    dq 0x0000000000000000

    ; code: base=0, limit=4GB, type=0x9A, gran=0xCF
    dq 0x00CF9A000000FFFF

    ; data: base=0, limit=4GB, type=0x92, gran=0xCF
    dq 0x00CF92000000FFFF

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

boot_drive db 0
loadingmsg db "Loading ... ", 0
okmsg db "OK", 13, 10, 0
errormsg db "Error", 13, 10, 0
startmsg db "Entering protected mode and jumping to entry point ...", 13, 10, 0

times 510 - ($-$$) db 0
dw 0xaa55

