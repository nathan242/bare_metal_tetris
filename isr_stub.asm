; isr_stub.s -- very small IRQ0 wrapper (NASM)
; Exports: irq0_stub

BITS 32
GLOBAL irq0_stub
GLOBAL irq1_stub

SECTION .text
extern irq_dispatch
; IRQ0 entry (vector 0x20)
; Preserve general-purpose registers, call C dispatcher with IRQ number (0)
irq0_stub:
    cli
    pusha
    push dword 0        ; push IRQ number
    call irq_dispatch   ; C function will handle and EOI
    add  esp, 4
    popa
    sti
    iret

irq1_stub:
    cli
    pusha
    push dword 1        ; push IRQ number
    call irq_dispatch   ; C function will handle and EOI
    add  esp, 4
    popa
    sti
    iret
