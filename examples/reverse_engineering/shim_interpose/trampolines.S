/*
 * trampolines.S - 为原型未知的导出函数生成"尾跳转"跳板
 *
 * 跳板不碰任何参数寄存器和栈，直接 jmp 到原函数：无论原函数有几个参数、是否有浮点参数、
 * 是否变参，都原样到达原函数，原函数的返回值也直接回到调用者。
 * hezi-hack 里的 ARM32 版本见 shim/trampoline.inc（ldr ip, [pc, ip]; bxne ip）。
 * fwd_<name> 为 NULL（原库没加载）时返回 -1。
 */
#if defined(__x86_64__)
    .text
    .globl  vendor_secret
    .type   vendor_secret, @function
vendor_secret:
    movq    fwd_vendor_secret(%rip), %r11     /* r11 是调用者保存的临时寄存器，可以随便用 */
    testq   %r11, %r11
    jz      1f
    jmp     *%r11
1:  movl    $-1, %eax
    ret
    .size   vendor_secret, . - vendor_secret

#elif defined(__aarch64__)
    .text
    .globl  vendor_secret
    .type   vendor_secret, %function
vendor_secret:
    adrp    x16, fwd_vendor_secret             /* x16/x17 = IP0/IP1，AAPCS64 留给跳板用的 */
    ldr     x16, [x16, :lo12:fwd_vendor_secret]
    cbz     x16, 1f
    br      x16
1:  mov     w0, #-1
    ret
    .size   vendor_secret, . - vendor_secret

#elif defined(__arm__)
    .syntax unified
    .arm
    .text
    .globl  vendor_secret
    .type   vendor_secret, %function
vendor_secret:
    ldr     ip, 2f                             /* ip = r12，AAPCS 的过程间临时寄存器 */
1:  ldr     ip, [pc, ip]
    cmp     ip, #0
    bxne    ip
    mvn     r0, #0
    bx      lr
2:  .word   fwd_vendor_secret - (1b + 8)
    .size   vendor_secret, . - vendor_secret
#else
#error "add a trampoline for this architecture"
#endif
    .section .note.GNU-stack, "", %progbits
