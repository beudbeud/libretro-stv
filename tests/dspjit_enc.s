// Reference encodings for SS_SCSP_DSPJIT::EncodeSelfTest (same order).
// Assemble with: aarch64-linux-gnu-as -o x.o dspjit_enc.s && objcopy -O binary -j .text x.o x.bin
	.text
	movz	w3, #0xffff
	movk	w3, #0x7f, lsl #16
	add	w5, w26, #0x55
	sub	w14, w14, #1
	add	x27, x19, #3072
	add	w22, w8, w9
	sub	w13, w11, w13
	neg	w9, w7
	cmp	w11, #12
	cmp	w2, w3
	csel	w2, w3, w2, gt
	csel	w12, w13, w12, lo
	cset	w13, eq
	and	w10, w10, w11
	orr	w10, w10, w12, lsl #11
	eor	w12, w12, w13
	mov	w9, wzr
	mov	x19, x0
	lsl	w21, w21, #4
	lsr	w23, w2, #11
	asr	w11, w10, #31
	sbfx	w0, w21, #0, #24
	ubfx	w1, w24, #4, #12
	asr	w12, w12, w11
	lsl	w14, w14, w2
	clz	w12, w12
	smull	x8, w1, w7
	asr	x8, x8, #12
	ldr	w21, [x19, #3200]
	str	w10, [x19, #3084]
	ldrh	w23, [x19, #3600]
	strh	w10, [x19, #3602]
	ldrb	w10, [x19, #3611]
	strb	wzr, [x19, #3611]
	ldr	w5, [x27, w5, uxtw #2]
	str	w2, [x27, w11, uxtw #2]
	ldrh	w10, [x20, w10, uxtw #1]
	strh	w11, [x20, w10, uxtw #1]
	stp	x29, x30, [sp, #-96]!
	stp	x19, x20, [sp, #16]
	ldp	x27, x28, [sp, #80]
	ldp	x29, x30, [sp], #96
	ret
	b	. + 20
	cbz	w10, . + 28
	cbnz	w26, . + 8
	tbnz	w10, #18, . + 12
	and	w2, w2, #0xffffff
	and	w22, w22, #0x3ffffff
	and	w5, w5, #0x7f
	and	w23, w2, #0xfff
	and	w12, w10, #0x7ff
	orr	w13, w12, #0x800
	and	w13, w13, #0xc0000000
	orr	w12, w12, #0x80000
	and	w12, w12, #0xffff
	and	w12, w12, #0x7ffff
	and	w10, w12, #0xffffff
