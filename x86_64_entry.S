// SPDX-License-Identifier: GPL-2.0-only
/* MinGW stack probe: preserve all GPRs; RAX holds the byte count. */
.text
.globl ___chkstk_ms
___chkstk_ms:
.globl x86_64_stack_probe
x86_64_stack_probe:
pushq %rcx
pushq %rax
leaq 24(%rsp),%rcx
1: cmpq $4096,%rax
jb 2f
subq $4096,%rcx
testq %rcx,(%rcx)
subq $4096,%rax
jmp 1b
2: subq %rax,%rcx
testq %rcx,(%rcx)
popq %rax
popq %rcx
ret

.text
.altmacro
.macro MAKE_ISR n
ki_\n:
.if (\n != 8) && (\n != 10) && (\n != 11) && (\n != 12) && (\n != 13) && (\n != 14) && (\n != 17) && (\n != 21) && (\n != 29) && (\n != 30)
pushq $0
.endif
pushq $\n
jmp irq_common
.endm
.set vector,0
.rept 256
MAKE_ISR %vector
.set vector,vector+1
.endr
irq_common:
testb $3,24(%rsp)
jz 1f
swapgs
1:
irq_save_regs:
pushq %rax
pushq %rbx
pushq %rcx
pushq %rdx
pushq %rbp
pushq %rsi
pushq %rdi
pushq %r8
pushq %r9
pushq %r10
pushq %r11
pushq %r12
pushq %r13
pushq %r14
pushq %r15
cld
movq %rsp,%rcx
andq $-16,%rsp
subq $32,%rsp
call x86_64_irq_dispatch
movq %rax,%rsp
cmpq $2,120(%rsp)
je 3f
movq %gs:24,%rax
movq %rax,%gs:16
3:
popq %r15
popq %r14
popq %r13
popq %r12
popq %r11
popq %r10
popq %r9
popq %r8
popq %rdi
popq %rsi
popq %rbp
popq %rdx
popq %rcx
popq %rbx
popq %rax
addq $16,%rsp
testb $3,8(%rsp)
jz 2f
swapgs
2:
iretq
.globl x86_64_syscall_entry
x86_64_syscall_entry:
swapgs
movq %rsp,%gs:8
movq %gs:0,%rsp
pushq $0x1b
pushq %gs:8
pushq %r11
pushq $0x23
pushq %rcx
pushq $0
pushq $128
jmp irq_save_regs
.section .rdata,"dr"
.balign 8
.globl x86_64_irq_stubs
x86_64_irq_stubs:
.macro ISR_PTR n
.quad ki_\n
.endm
.set vector,0
.rept 256
ISR_PTR %vector
.set vector,vector+1
.endr
.noaltmacro
.text

.text
.balign 16
.globl x86_64_ap_blob
x86_64_ap_blob:
.code16
cli
cld
movw %cs,%ax
movw %ax,%ds
movw %ax,%es
movw %ax,%ss
movl x86_64_ap_base-x86_64_ap_blob,%esi
lgdt x86_64_ap_gdtr-x86_64_ap_blob
movl %cr0,%eax
orl $1,%eax
movl %eax,%cr0
ljmpl *x86_64_ap_pm_far-x86_64_ap_blob
.code32
.globl x86_64_ap_pm
x86_64_ap_pm:
movw $0x10,%ax
movw %ax,%ds
movw %ax,%es
movw %ax,%ss
movl %cr4,%eax
andl $0xfff9efff,%eax
orl $0x20,%eax
movl %eax,%cr4
movl x86_64_ap_cr3-x86_64_ap_blob(%esi),%eax
movl %eax,%cr3
movl $0xc0000080,%ecx
rdmsr
orl x86_64_ap_efer-x86_64_ap_blob(%esi),%eax
wrmsr
movl %cr0,%eax
orl $0x80010001,%eax
movl %eax,%cr0
ljmp *x86_64_ap_lm_far-x86_64_ap_blob(%esi)
.code64
.globl x86_64_ap_lm
x86_64_ap_lm:
movw $0x10,%ax
movw %ax,%ds
movw %ax,%es
movw %ax,%ss
movq x86_64_ap_stack-x86_64_ap_blob(%rsi),%rsp
movq x86_64_ap_argument-x86_64_ap_blob(%rsi),%rcx
movq x86_64_ap_entry-x86_64_ap_blob(%rsi),%rax
xorq %rbp,%rbp
subq $32,%rsp
call *%rax
1: cli
hlt
jmp 1b
.balign 8
.globl x86_64_ap_gdt
x86_64_ap_gdt:
.quad 0,0x00cf9a000000ffff,0x00cf92000000ffff,0x00af9a000000ffff
.globl x86_64_ap_gdtr
x86_64_ap_gdtr:
.word 31
.long 0
.globl x86_64_ap_pm_far
x86_64_ap_pm_far:
.long 0
.word 8
.globl x86_64_ap_lm_far
x86_64_ap_lm_far:
.long 0
.word 0x18
.balign 8
.globl x86_64_ap_base
x86_64_ap_base:
.long 0
.globl x86_64_ap_cr3
x86_64_ap_cr3:
.long 0
.globl x86_64_ap_efer
x86_64_ap_efer:
.long 0
.balign 8
.globl x86_64_ap_stack
x86_64_ap_stack:
.quad 0
.globl x86_64_ap_entry
x86_64_ap_entry:
.quad 0
.globl x86_64_ap_argument
x86_64_ap_argument:
.quad 0
.globl x86_64_ap_blob_end
x86_64_ap_blob_end:
