/*
 * void swap_ctx(void **save_sp, void *load_sp)        rdi = save_sp, rsi = load_sp
 *
 * Save the callee-saved registers on the current stack, store the stack pointer, load the other
 * stack pointer, restore its callee-saved registers, and return into whatever it was doing.
 * The System V ABI makes every other register caller-saved, so the compiler has already dealt
 * with them at the call site. (MXCSR and the x87 control word are technically callee-saved too;
 * nothing here changes them, so they are not preserved.)
 *
 * Hidden: the scheduler's switch is not a symbol anything outside libioxd may interpose.
 *
 * The CFI describes one frame of six pushes. It is still right after the stack swap, because the
 * stack being loaded was left in exactly that shape by its own trip through here (or forged that
 * way by coro_create), so gdb and perf unwind out of either coroutine.
 *
 * No .note.gnu.property on purpose: an unmarked object leaves the linked binary without the
 * IBT/SHSTK flags, so a shadow stack is never enabled around this hand-built frame.
 */
	.text
	.globl	swap_ctx
	.hidden	swap_ctx
	.type	swap_ctx, @function
swap_ctx:
	.cfi_startproc
	pushq	%rbp
	.cfi_adjust_cfa_offset 8
	.cfi_offset %rbp, -16
	pushq	%rbx
	.cfi_adjust_cfa_offset 8
	.cfi_offset %rbx, -24
	pushq	%r12
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r12, -32
	pushq	%r13
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r13, -40
	pushq	%r14
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r14, -48
	pushq	%r15
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r15, -56
	movq	%rsp, (%rdi)
	movq	%rsi, %rsp
	popq	%r15
	.cfi_restore %r15
	.cfi_adjust_cfa_offset -8
	popq	%r14
	.cfi_restore %r14
	.cfi_adjust_cfa_offset -8
	popq	%r13
	.cfi_restore %r13
	.cfi_adjust_cfa_offset -8
	popq	%r12
	.cfi_restore %r12
	.cfi_adjust_cfa_offset -8
	popq	%rbx
	.cfi_restore %rbx
	.cfi_adjust_cfa_offset -8
	popq	%rbp
	.cfi_restore %rbp
	.cfi_adjust_cfa_offset -8
	ret
	.cfi_endproc
	.size	swap_ctx, .-swap_ctx

	.section .note.GNU-stack,"",@progbits
