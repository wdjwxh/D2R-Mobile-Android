// dm_gate 与未实现桩 — 自上游 dm/host/entry.S 移植（ELF 版：去掉 Mach-O 下划线前缀）。
// dm_gate：改写器把 svc / cntpct 读换成「bl 到本入口」。进来时 lr = 门块返回点，
// gate_dispatch 据此查是哪个门。保存 x0..x30 + q0..q31 + 状态寄存器，调 C，回写。
    .data
    .globl dm_no_fp_state
    .hidden dm_no_fp_state
dm_no_fp_state:
    .byte 0

    .text
    .globl unimpl_stubs
    .p2align 3
unimpl_stubs:
    .set k, 0
    .rept 4096
    movz x9, #k
    b unimpl_entry
    .set k, k + 1
    .endr

    .globl unimpl_entry
    .p2align 2
unimpl_entry:
    stp x29, x30, [sp, #-96]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    str x18, [sp, #88]
    mov x0, x9
    add x1, sp, #16
    bl unimpl_c
    ldr x18, [sp, #88]
    ldp x29, x30, [sp], #96
    ret

    .globl dm_gate
    .p2align 2
dm_gate:
    // Capture every guest register before borrowing any scratch register.
    // This frame remains on the valid guest stack exactly like upstream.
    sub sp, sp, #800
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    stp q0, q1, [sp, #256]
    stp q2, q3, [sp, #288]
    stp q4, q5, [sp, #320]
    stp q6, q7, [sp, #352]
    stp q8, q9, [sp, #384]
    stp q10, q11, [sp, #416]
    stp q12, q13, [sp, #448]
    stp q14, q15, [sp, #480]
    stp q16, q17, [sp, #512]
    stp q18, q19, [sp, #544]
    stp q20, q21, [sp, #576]
    stp q22, q23, [sp, #608]
    stp q24, q25, [sp, #640]
    stp q26, q27, [sp, #672]
    stp q28, q29, [sp, #704]
    stp q30, q31, [sp, #736]
    mrs x9, nzcv
    str x9, [sp, #768]
    adrp x9, dm_no_fp_state
    ldrb w9, [x9, :lo12:dm_no_fp_state]
    cbnz w9, 20f
    mrs x9, fpsr
    str x9, [sp, #776]
    mrs x9, fpcr
    str x9, [sp, #784]
20:
    // x19 is callee-saved by gate_dispatch; its guest value is already saved.
    mov x19, sp
    mov x0, x30
    mov x1, x19
    add x2, x19, #800
    adrp x9, dm_no_fp_state
    ldrb w9, [x9, :lo12:dm_no_fp_state]
    cbz w9, 21f
    // Translation TLS400 is a descriptor {top, low, depth}, offsets 0/8/16
    // asserted by tsd.c. x19/x20 were saved above and are callee-saved by C.
    // Only depth zero switches stacks. Guest callbacks can consume large
    // frames: inferring nesting from SP bounds could overwrite the outer call.
    mrs x9, tpidr_el0
    ldr x20, [x9, #3200]
    cbz x20, 24f
    ldr x10, [x20, #16]
    cbz x10, 22f
    // The lower guard is 64 KiB, larger than this complete 800-byte gate
    // frame. Crossing it faults instead of silently writing adjacent memory.
    ldr x11, [x20, #8]
    cmp x19, x11
    b.lo 24f
    ldr x11, [x20, #0]
    add x9, x19, #800
    cmp x9, x11
    b.hi 24f
    b 25f
22: ldr x11, [x20, #0]
    cbz x11, 24f
    mov sp, x11
25: add x10, x10, #1
    str x10, [x20, #16]
21: bl gate_dispatch
    adrp x9, dm_no_fp_state
    ldrb w9, [x9, :lo12:dm_no_fp_state]
    cbz w9, 26f
    ldr x10, [x20, #16]
    cbz x10, 24f
    sub x10, x10, #1
    str x10, [x20, #16]
26:
    mov sp, x19
    adrp x9, dm_no_fp_state
    ldrb w9, [x9, :lo12:dm_no_fp_state]
    cbnz w9, 23f
    ldr x9, [sp, #784]
    msr fpcr, x9
    ldr x9, [sp, #776]
    msr fpsr, x9
23: ldr x9, [sp, #768]
    msr nzcv, x9
    ldp q0, q1, [sp, #256]
    ldp q2, q3, [sp, #288]
    ldp q4, q5, [sp, #320]
    ldp q6, q7, [sp, #352]
    ldp q8, q9, [sp, #384]
    ldp q10, q11, [sp, #416]
    ldp q12, q13, [sp, #448]
    ldp q14, q15, [sp, #480]
    ldp q16, q17, [sp, #512]
    ldp q18, q19, [sp, #544]
    ldp q20, q21, [sp, #576]
    ldp q22, q23, [sp, #608]
    ldp q24, q25, [sp, #640]
    ldp q26, q27, [sp, #672]
    ldp q28, q29, [sp, #704]
    ldp q30, q31, [sp, #736]
    // x0..x17
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    // x18 也恢复（Android/翻译层无苹果的平台寄存器约束；翻译器可能用 x18 存客体上下文）
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]          // ★ 修复：恢复客体返回地址（此前丢失，ret 跳进 libc 垃圾）
    // No register or flags may be clobbered after restoration.
    add sp, sp, #800
    ret
24: // A broken translation stack is fatal; never reset an active stack top.
    brk #0x474
    b 24b

// dm_thread_enter(entry, arg, sp)：切到客体栈执行客体入口（Horizon 线程 ABI：x0 = arg）。
// x29/x30 清零：入口若返回即跳 0（客体必须经 ExitThread 退出）。
    .globl dm_thread_enter
    .hidden dm_thread_enter
    .p2align 2
dm_thread_enter:
    // All entry paths (boot and guest pthreads) get an independent signal
    // stack before leaving the host stack. Preserve the guest entry arguments.
    stp x0, x1, [sp, #-32]!
    stp x2, x30, [sp, #16]
    bl crash_thread_init
    ldp x2, x30, [sp, #16]
    ldp x0, x1, [sp], #32
    mov x5, x0
    mov sp, x2
    mov x29, xzr
    mov x30, xzr
    mov x0, x1
    br x5
