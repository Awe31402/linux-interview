/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pic_asm.S —— 卷2 第3章 Q3：位置無關 (PIC) vs 位置有關 (non-PIC) 的 ARM64 指令
 *
 * 這裡的每個函式都只做一件事：回報「某個位址」。
 * 差別在於位址是怎麼算出來的：
 *   - PIC   ：adr / adrp / b / bl —— 全部是 [PC + offset]，跟載入位置無關
 *   - 非 PIC：ldr x0,=sym / blr（經由文字池的絕對位址）—— 寫死鏈接位址
 *
 * pic_demo.c 會把 blob_start..blob_end 這整段程式碼 memcpy 到另一個
 * 位址（= 改變「運行地址」但不改「鏈接地址」）再呼叫，
 * 兩類函式的回傳值就會分道揚鑣。
 */
	.text
	.balign 8
	.global blob_start
blob_start:

/* ---- (1) adr：PC 相對，回傳「自己此刻真正在哪裡執行」 ---------------- */
	.global pic_where
	.type	pic_where, %function
pic_where:
	adr	x0, pic_where		/* x0 = 執行時位址（運行地址） */
	ret
	.size	pic_where, . - pic_where

/* ---- (2) ldr =sym：絕對位址，永遠回傳「編譯鏈接時決定的位址」 -------- */
	.global abs_where
	.type	abs_where, %function
abs_where:
	ldr	x0, =pic_where		/* x0 = 文字池裡的 64 位元絕對位址 */
	ret
	.ltorg				/* 文字池就放在 blob 內，一起被複製 */
	.size	abs_where, . - abs_where

/* ---- (3) 被呼叫者：回報自己的執行位址 -------------------------------- */
	.global callee
	.type	callee, %function
callee:
	adr	x0, callee
	ret
	.size	callee, . - callee

/* ---- (4) bl：PC 相對呼叫（±128 MB），呼叫的是「複製後的 callee」 ----- */
	.global pic_call
	.type	pic_call, %function
pic_call:
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	bl	callee
	ldp	x29, x30, [sp], #16
	ret
	.size	pic_call, . - pic_call

/* ---- (5) blr + 絕對位址：跳回「原本那份 callee」 --------------------- */
	.global abs_call
	.type	abs_call, %function
abs_call:
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	ldr	x1, =callee
	blr	x1
	ldp	x29, x30, [sp], #16
	ret
	.ltorg
	.size	abs_call, . - abs_call

/* ---- (6) adrp+add：PC 相對（±4 GB），指向固定的全域資料 ------------- */
	.global pic_data
	.type	pic_data, %function
pic_data:
	adrp	x0, gvar
	add	x0, x0, :lo12:gvar
	ret
	.size	pic_data, . - pic_data

	.global blob_end
blob_end:
	.balign 8

	.data
	.global gvar
gvar:
	.quad	0x1234567890abcdef
