; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=i686-unknown -mattr=+sse2 | FileCheck %s --check-prefixes=CHECK,X86
; RUN: llc < %s -mtriple=x86_64-unknown -mattr=+sse2 | FileCheck %s --check-prefixes=CHECK,X64

; This test makes sure that the compiler does not crash with an
; assertion failure when trying to fold a vector shift left
; by immediate count if the type of the input vector is different
; to the result type.
;
; This happens for example when lowering a shift left of a MVT::v16i8 vector.
; This is custom lowered into the following sequence:
;     count << 5
;     A =  VSHLI(MVT::v8i16, r & (char16)15, 4)
;     B = BITCAST MVT::v16i8, A
;     VSELECT(r, B, count);
;     count += count
;     C = VSHLI(MVT::v8i16, r & (char16)63, 2)
;     D = BITCAST MVT::v16i8, C
;     r = VSELECT(r, C, count);
;     count += count
;     VSELECT(r, r+r, count);
;     count = count << 5;
;
; Where 'r' is a vector of type MVT::v16i8, and
; 'count' is the vector shift count.

define <16 x i8> @do_not_crash(i8*, i32*, i64*, i32, i64, i8) {
; X86-LABEL: do_not_crash:
; X86:       # %bb.0: # %entry
; X86-NEXT:    movl {{[0-9]+}}(%esp), %eax
; X86-NEXT:    movl {{[0-9]+}}(%esp), %ecx
; X86-NEXT:    movb %al, (%ecx)
; X86-NEXT:    movd %eax, %xmm0
; X86-NEXT:    psllq $56, %xmm0
; X86-NEXT:    movdqa {{.*#+}} xmm2 = [255,255,255,255,255,255,255,0,255,255,255,255,255,255,255,255]
; X86-NEXT:    movdqa %xmm2, %xmm1
; X86-NEXT:    pandn %xmm0, %xmm1
; X86-NEXT:    por %xmm2, %xmm1
; X86-NEXT:    pcmpeqd %xmm2, %xmm2
; X86-NEXT:    psllw $5, %xmm1
; X86-NEXT:    pxor %xmm3, %xmm3
; X86-NEXT:    pxor %xmm0, %xmm0
; X86-NEXT:    pcmpgtb %xmm1, %xmm0
; X86-NEXT:    pxor %xmm0, %xmm2
; X86-NEXT:    pand {{\.LCPI.*}}, %xmm0
; X86-NEXT:    por %xmm2, %xmm0
; X86-NEXT:    paddb %xmm1, %xmm1
; X86-NEXT:    pxor %xmm2, %xmm2
; X86-NEXT:    pcmpgtb %xmm1, %xmm2
; X86-NEXT:    movdqa %xmm2, %xmm4
; X86-NEXT:    pandn %xmm0, %xmm4
; X86-NEXT:    psllw $2, %xmm0
; X86-NEXT:    pand %xmm2, %xmm0
; X86-NEXT:    pand {{\.LCPI.*}}, %xmm0
; X86-NEXT:    por %xmm4, %xmm0
; X86-NEXT:    paddb %xmm1, %xmm1
; X86-NEXT:    pcmpgtb %xmm1, %xmm3
; X86-NEXT:    movdqa %xmm3, %xmm1
; X86-NEXT:    pandn %xmm0, %xmm1
; X86-NEXT:    paddb %xmm0, %xmm0
; X86-NEXT:    pand %xmm3, %xmm0
; X86-NEXT:    por %xmm1, %xmm0
; X86-NEXT:    retl
;
; X64-LABEL: do_not_crash:
; X64:       # %bb.0: # %entry
; X64-NEXT:    movb %r9b, (%rdi)
; X64-NEXT:    movd %r9d, %xmm0
; X64-NEXT:    psllq $56, %xmm0
; X64-NEXT:    movdqa {{.*#+}} xmm2 = [255,255,255,255,255,255,255,0,255,255,255,255,255,255,255,255]
; X64-NEXT:    movdqa %xmm2, %xmm1
; X64-NEXT:    pandn %xmm0, %xmm1
; X64-NEXT:    por %xmm2, %xmm1
; X64-NEXT:    pcmpeqd %xmm2, %xmm2
; X64-NEXT:    psllw $5, %xmm1
; X64-NEXT:    pxor %xmm3, %xmm3
; X64-NEXT:    pxor %xmm0, %xmm0
; X64-NEXT:    pcmpgtb %xmm1, %xmm0
; X64-NEXT:    pxor %xmm0, %xmm2
; X64-NEXT:    pand {{.*}}(%rip), %xmm0
; X64-NEXT:    por %xmm2, %xmm0
; X64-NEXT:    paddb %xmm1, %xmm1
; X64-NEXT:    pxor %xmm2, %xmm2
; X64-NEXT:    pcmpgtb %xmm1, %xmm2
; X64-NEXT:    movdqa %xmm2, %xmm4
; X64-NEXT:    pandn %xmm0, %xmm4
; X64-NEXT:    psllw $2, %xmm0
; X64-NEXT:    pand %xmm2, %xmm0
; X64-NEXT:    pand {{.*}}(%rip), %xmm0
; X64-NEXT:    por %xmm4, %xmm0
; X64-NEXT:    paddb %xmm1, %xmm1
; X64-NEXT:    pcmpgtb %xmm1, %xmm3
; X64-NEXT:    movdqa %xmm3, %xmm1
; X64-NEXT:    pandn %xmm0, %xmm1
; X64-NEXT:    paddb %xmm0, %xmm0
; X64-NEXT:    pand %xmm3, %xmm0
; X64-NEXT:    por %xmm1, %xmm0
; X64-NEXT:    retq
entry:
  store i8 %5, i8* %0
  %L5 = load i8, i8* %0
  %I8 = insertelement <16 x i8> <i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1>, i8 %L5, i32 7
  %B51 = shl <16 x i8> <i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1, i8 -1>, %I8
  ret <16 x i8> %B51
}
