/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy XCover5 (SM-G525F/N, Exynos 850 "s5e3830") XNU loader entry.
 *
 * arm64 Linux Image header so Samsung S-Boot treats us like a kernel. The
 * stock BOOT kernel is a gzip'd Image with this header; tools/xcover5/
 * mkboot.py gzips ours the same way. Expected entry state (same S-Boot
 * family as the S8, not yet measured on the XCover5): EL1, MMU off,
 * x0 = DTB. The loader prints the real values first.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset, as the stock kernel */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"		/* magic */
	.long	0

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB */
	mrs	x20, CurrentEL
	lsr	x20, x20, #2
	cmp	x20, #1
	b.ne	1f
	mrs	x21, sctlr_el1
	msr	spsel, #1
	adr	x1, vectors
	msr	vbar_el1, x1
	b	2f
1:	mov	x21, #0			/* EL2/EL3: report it, do not touch EL1 state */
2:	adr	x1, _stack_top
	mov	sp, x1
	isb
	adr	x1, _bss_start
	adr	x2, _bss_end
3:	cmp	x1, x2
	b.hs	4f
	str	xzr, [x1], #8
	b	3b
4:	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	bl	dl_main			/* returns XNU entry PA, or 0 */
	cbz	x0, 6f
	mov	x16, x0
	adr	x1, dl_boot_args
	ldr	x0, [x1]		/* boot_args PA for start_first_cpu */
	/* XNU's entry expects the MMU and caches off. Data was written with the
	 * MMU off (Device memory) and the copies were cleaned to PoC anyway. */
	mrs	x14, sctlr_el1
	bic	x14, x14, #1		/* M */
	bic	x14, x14, #4		/* C */
	bic	x14, x14, #0x1000	/* I */
	msr	sctlr_el1, x14
	ic	iallu
	dsb	sy
	isb
	br	x16
6:	wfe
	b	6b

.macro vent slot
	.balign	0x80
	mov	x0, #\slot
	mov	x1, sp
	b	exc_common
.endm

	.balign	0x800
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	bl	dl_exception		/* never returns */
5:	wfe
	b	5b
