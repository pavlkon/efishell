/* MAPEX lazy reservation; both stores must resolve through user page faults. */
.section .rdata,"dr"
.balign 16
.global os_vm_probe
.global os_vm_probe_end
os_vm_probe:
    movl $46,%eax
    movl $8192,%edi
    movl $4096,%esi
    movl $3,%edx
    movl $1,%r10d
    syscall
    testq %rax,%rax
    js 1f
    movq $0x1234,(%rax)
    movq $0x5678,4096(%rax)
    cmpq $0x1234,(%rax)
    jne 1f
    cmpq $0x5678,4096(%rax)
    jne 1f
    xorl %edi,%edi
    jmp 2f
1:  movl $1,%edi
2:  xorl %eax,%eax
    syscall
    ud2
os_vm_probe_end:
