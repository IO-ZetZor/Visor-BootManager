/*
 * early_start_x86_64.S - optional pre-relocation console checkpoint.
 *
 * The firmware enters a PE image at ENTRY(_visor_start) with ImageHandle in
 * %rcx and SystemTable in %rdx (Microsoft x64 ABI).  gnu-efi's _start then
 * self-relocates the ELF image and calls _entry.  A hang in the PE loader or
 * in _relocate therefore happens before any C code and before the normal
 * "Visor loading..." banner.
 *
 * With VISOR_EARLY_DIAG=1 this stub writes one line through ConOut before
 * touching anything else.  It deliberately uses only RIP-relative code and
 * the firmware-provided SystemTable, so it is safe before self-relocation.
 * With VISOR_EARLY_DIAG=0 it is just a tail jump to gnu-efi's _start.
 */

	.text
	.globl _visor_start
	.type _visor_start, @function
_visor_start:
#if VISOR_EARLY_DIAG
	subq	$8, %rsp
	pushq	%rcx			/* ImageHandle */
	pushq	%rdx			/* SystemTable */

	movq	(%rsp), %rax		/* SystemTable */
	testq	%rax, %rax
	jz	1f
	movq	0x40(%rax), %rcx	/* SystemTable->ConOut */
	testq	%rcx, %rcx
	jz	1f
	movq	0x08(%rcx), %rax	/* ConOut->OutputString */
	testq	%rax, %rax
	jz	1f

	leaq	.Learly_msg(%rip), %rdx
	subq	$32, %rsp		/* MS x64 shadow space */
	call	*%rax
	addq	$32, %rsp

1:
	popq	%rdx
	popq	%rcx
	addq	$8, %rsp
#endif
	jmp	_start
	.size	_visor_start, .-_visor_start

#if VISOR_EARLY_DIAG
	.section .rodata
	.align 2
.Learly_msg:
	.word	0x0056, 0x0069, 0x0073, 0x006f, 0x0072, 0x003a, 0x0020
	.word	0x0050, 0x0045, 0x0020, 0x0065, 0x006e, 0x0074, 0x0072
	.word	0x0079, 0x0020, 0x0072, 0x0065, 0x0061, 0x0063, 0x0068
	.word	0x0065, 0x0064, 0x000d, 0x000a, 0x0000
#endif
