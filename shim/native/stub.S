// Entry-hook stub template for nhook.c (copied next to the lifted code, its two literals filled in): saves the caller-saved
// registers, calls handler(x0 = the guest thread's context), restores them, runs the displaced first instruction of the
// lifted function and continues at its second.
    .text
    .p2align 4
    .globl _nh_tpl_start, _nh_tpl_displaced, _nh_tpl_handler, _nh_tpl_return, _nh_tpl_end
_nh_tpl_start:
    sub  sp, sp, #0x120
    stp  x0, x1, [sp, #0x00]
    stp  x2, x3, [sp, #0x10]
    stp  x4, x5, [sp, #0x20]
    stp  x6, x7, [sp, #0x30]
    stp  x8, x9, [sp, #0x40]
    stp  x10, x11, [sp, #0x50]
    stp  x12, x13, [sp, #0x60]
    stp  x14, x15, [sp, #0x70]
    stp  x16, x17, [sp, #0x80]
    stp  x29, x30, [sp, #0x90]
    stp  q0, q1, [sp, #0xa0]
    stp  q2, q3, [sp, #0xc0]
    stp  q4, q5, [sp, #0xe0]
    stp  q6, q7, [sp, #0x100]
    ldr  x16, _nh_tpl_handler
    blr  x16
    ldp  q6, q7, [sp, #0x100]
    ldp  q4, q5, [sp, #0xe0]
    ldp  q2, q3, [sp, #0xc0]
    ldp  q0, q1, [sp, #0xa0]
    ldp  x29, x30, [sp, #0x90]
    ldp  x16, x17, [sp, #0x80]
    ldp  x14, x15, [sp, #0x70]
    ldp  x12, x13, [sp, #0x60]
    ldp  x10, x11, [sp, #0x50]
    ldp  x8, x9, [sp, #0x40]
    ldp  x6, x7, [sp, #0x30]
    ldp  x4, x5, [sp, #0x20]
    ldp  x2, x3, [sp, #0x10]
    ldp  x0, x1, [sp, #0x00]
    add  sp, sp, #0x120
_nh_tpl_displaced:
    nop
    ldr  x16, _nh_tpl_return
    br   x16
    .p2align 3
_nh_tpl_handler:
    .quad 0
_nh_tpl_return:
    .quad 0
_nh_tpl_end:

// Census stub (NHOOK_CENSUS): count the calls of a lifted function and keep its last caller's context, with x16/x17 only
// (scratch at a lifted entry); then the displaced first instruction and back to the second. 48 bytes, literals at +32.
    .p2align 4
    .globl _nh_cen_start, _nh_cen_displaced, _nh_cen_slot, _nh_cen_return, _nh_cen_end
_nh_cen_start:
    ldr  x16, _nh_cen_slot
    ldr  x17, [x16]
    add  x17, x17, #1
    stp  x17, x0, [x16]
_nh_cen_displaced:
    nop
    ldr  x16, _nh_cen_return
    br   x16
    .p2align 3
_nh_cen_slot:
    .quad 0
_nh_cen_return:
    .quad 0
_nh_cen_end:

// Stub for a hook on the app's OWN code (its __TEXT, nhook.c text_hook): the same register save / handler call / restore as
// _nh_tpl, then the FOUR displaced instructions of the function's start (replaced there by `ldr x16, #8; br x16; .quad stub`;
// they must not be PC-relative) and back to the function's fifth instruction.
    .p2align 4
    .globl _nh_tp4_start, _nh_tp4_displaced, _nh_tp4_handler, _nh_tp4_return, _nh_tp4_end
_nh_tp4_start:
    sub  sp, sp, #0x120
    stp  x0, x1, [sp, #0x00]
    stp  x2, x3, [sp, #0x10]
    stp  x4, x5, [sp, #0x20]
    stp  x6, x7, [sp, #0x30]
    stp  x8, x9, [sp, #0x40]
    stp  x10, x11, [sp, #0x50]
    stp  x12, x13, [sp, #0x60]
    stp  x14, x15, [sp, #0x70]
    stp  x16, x17, [sp, #0x80]
    stp  x29, x30, [sp, #0x90]
    stp  q0, q1, [sp, #0xa0]
    stp  q2, q3, [sp, #0xc0]
    stp  q4, q5, [sp, #0xe0]
    stp  q6, q7, [sp, #0x100]
    ldr  x16, _nh_tp4_handler
    blr  x16
    ldp  q6, q7, [sp, #0x100]
    ldp  q4, q5, [sp, #0xe0]
    ldp  q2, q3, [sp, #0xc0]
    ldp  q0, q1, [sp, #0xa0]
    ldp  x29, x30, [sp, #0x90]
    ldp  x16, x17, [sp, #0x80]
    ldp  x14, x15, [sp, #0x70]
    ldp  x12, x13, [sp, #0x60]
    ldp  x10, x11, [sp, #0x50]
    ldp  x8, x9, [sp, #0x40]
    ldp  x6, x7, [sp, #0x30]
    ldp  x4, x5, [sp, #0x20]
    ldp  x2, x3, [sp, #0x10]
    ldp  x0, x1, [sp, #0x00]
    add  sp, sp, #0x120
_nh_tp4_displaced:
    nop
    nop
    nop
    nop
    ldr  x16, _nh_tp4_return
    br   x16
    .p2align 3
_nh_tp4_handler:
    .quad 0
_nh_tp4_return:
    .quad 0
_nh_tp4_end:

// A text-hook stub with a filter in front (nhook.c text_hook_filtered): only calls whose guest ecx has the wanted parameter id
// in bits 16-26 (halo3+0x2af478: set a shader parameter, ~700k calls a second) reach the handler; the rest go straight to
// the displaced instructions with x16 / x17 and the flags touched only (scratch at a function's entry).
    .p2align 4
    .globl _nh_tpf_start, _nh_tpf_want, _nh_tpf_displaced, _nh_tpf_handler, _nh_tpf_return, _nh_tpf_end
_nh_tpf_start:
    ldr  w16, [x0, #8]
    ubfx w16, w16, #16, #11
    ldr  w17, _nh_tpf_want
    cmp  w16, w17
    b.ne _nh_tpf_displaced
    sub  sp, sp, #0x120
    stp  x0, x1, [sp, #0x00]
    stp  x2, x3, [sp, #0x10]
    stp  x4, x5, [sp, #0x20]
    stp  x6, x7, [sp, #0x30]
    stp  x8, x9, [sp, #0x40]
    stp  x10, x11, [sp, #0x50]
    stp  x12, x13, [sp, #0x60]
    stp  x14, x15, [sp, #0x70]
    stp  x16, x17, [sp, #0x80]
    stp  x29, x30, [sp, #0x90]
    stp  q0, q1, [sp, #0xa0]
    stp  q2, q3, [sp, #0xc0]
    stp  q4, q5, [sp, #0xe0]
    stp  q6, q7, [sp, #0x100]
    ldr  x16, _nh_tpf_handler
    blr  x16
    ldp  q6, q7, [sp, #0x100]
    ldp  q4, q5, [sp, #0xe0]
    ldp  q2, q3, [sp, #0xc0]
    ldp  q0, q1, [sp, #0xa0]
    ldp  x29, x30, [sp, #0x90]
    ldp  x16, x17, [sp, #0x80]
    ldp  x14, x15, [sp, #0x70]
    ldp  x12, x13, [sp, #0x60]
    ldp  x10, x11, [sp, #0x50]
    ldp  x8, x9, [sp, #0x40]
    ldp  x6, x7, [sp, #0x30]
    ldp  x4, x5, [sp, #0x20]
    ldp  x2, x3, [sp, #0x10]
    ldp  x0, x1, [sp, #0x00]
    add  sp, sp, #0x120
_nh_tpf_displaced:
    nop
    nop
    nop
    nop
    ldr  x16, _nh_tpf_return
    br   x16
    .p2align 3
_nh_tpf_handler:
    .quad 0
_nh_tpf_return:
    .quad 0
_nh_tpf_want:
    .word 0
    .word 0
_nh_tpf_end:
