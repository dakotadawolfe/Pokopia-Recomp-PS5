// SPDX-License-Identifier: GPL-3.0-or-later
// Original ARM programs exercising the lifted SVC ABI, not game instructions.
.text
.global svc_clock
.type svc_clock, %function
svc_clock:
    svc #0x1e
    mrs x1, cntfrq_el0
    ret
.size svc_clock, .-svc_clock

.org 0x40
.global svc_parent
.type svc_parent, %function
svc_parent:
    mov x19, x0
    adr x1, svc_child
    mov x2, x19
    ldr x3, [x19]
    mov w4, #44
    mov w5, #0
    svc #0x08
    str w0, [x19, #8]
    str w1, [x19, #12]
    cbnz w0, parent_done
    mov w0, w1
    svc #0x09
    str w0, [x19, #16]
    cbnz w0, parent_done
    add x1, x19, #12
    mov w2, #1
    movz x3, #0xca00
    movk x3, #0x3b9a, lsl #16
    svc #0x18
    str w0, [x19, #20]
    str w1, [x19, #24]
    ldr w0, [x19, #12]
    svc #0x16
    str w0, [x19, #28]
parent_done:
    ret
.size svc_parent, .-svc_parent

.org 0x200
.global svc_child
.type svc_child, %function
svc_child:
    mrs x1, tpidrro_el0
    str x1, [x0, #32]
    mrs x1, tpidr_el0
    str x1, [x0, #40]
    mov w1, #0x55aa
    str w1, [x0, #48]
    svc #0x0a
    mov w1, #0xffff
    str w1, [x0, #48]
    ret
.size svc_child, .-svc_child

.org 0x300
.global svc_events
.type svc_events, %function
svc_events:
    mov x19, x0
    svc #0x45
    str w0, [x19]
    str w1, [x19, #4]
    str w2, [x19, #8]
    cbnz w0, events_done
    mov w0, w1
    svc #0x11
    str w0, [x19, #12]
    add x1, x19, #8
    mov w2, #1
    mov x3, #0
    svc #0x18
    str w0, [x19, #16]
    str w1, [x19, #20]
    ldr w0, [x19, #8]
    svc #0x17
    str w0, [x19, #24]
    add x1, x19, #8
    mov w2, #1
    mov x3, #0
    svc #0x18
    str w0, [x19, #28]
    ldr w0, [x19, #4]
    svc #0x16
    ldr w0, [x19, #8]
    svc #0x16
events_done:
    ret
.size svc_events, .-svc_events

.org 0x400
.global svc_memory
.type svc_memory, %function
svc_memory:
    mov x19, x0
    mov x1, #0x200000
    svc #0x01
    str w0, [x19]
    cbnz w0, memory_done
    mov x20, x1
    mov x2, #0x55aa
    str x2, [x20]
    mov x0, x20
    mov x1, #0x1000
    mov x2, #1
    svc #0x02
    str w0, [x19, #4]
    cbnz w0, memory_done
    add x0, x19, #32
    mov x2, x20
    svc #0x06
    str w0, [x19, #8]
    cbnz w0, memory_done
    ldr x2, [x20]
    str x2, [x19, #24]
    mov x0, x20
    mov x1, #0x1000
    mov x2, #3
    svc #0x02
    str w0, [x19, #12]
    cbnz w0, memory_done
    mov x1, #0
    svc #0x01
    str w0, [x19, #16]
memory_done:
    ret
.size svc_memory, .-svc_memory
