// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
.syntax unified
.cpu cortex-m4
.fpu fpv4-sp-d16
.thumb
.section .isr_vector,"a",%progbits
.balign 1024
.word _estack, Reset_Handler, Default_Handler, Default_Handler
.word Default_Handler, Default_Handler, Default_Handler, 0, 0, 0, 0
.word SVC_Handler, Default_Handler, 0, PendSV_Handler, SysTick_Handler
.rept 128
.word Default_Handler
.endr

.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
	cpsid i
	ldr sp, =_estack
	movs r0, #0
	msr control, r0
	isb
	ldr r0, =0xE000ED88
	ldr r1, [r0]
	orr r1, r1, #(0xF << 20)
	str r1, [r0]
	dsb
	isb
	ldr r0, =_sidata
	ldr r1, =_sdata
	ldr r2, =_edata
1:
	cmp r1, r2
	bcs 2f
	ldr r3, [r0], #4
	str r3, [r1], #4
	b 1b
2:
	ldr r1, =_sbss
	ldr r2, =_ebss
	movs r3, #0
3:
	cmp r1, r2
	bcs 4f
	str r3, [r1], #4
	b 3b
4:
	bl main
	b Default_Handler
.size Reset_Handler, .-Reset_Handler
