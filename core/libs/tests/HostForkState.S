.text
.globl CaptureHostCloneState
.def CaptureHostCloneState; .scl 2; .type 32; .endef
.seh_proc CaptureHostCloneState
CaptureHostCloneState:
    pushq %rbx
    .seh_pushreg %rbx
    subq $192, %rsp
    .seh_stackalloc 192
    movaps %xmm6, 32(%rsp)
    .seh_savexmm %xmm6, 32
    movaps %xmm7, 48(%rsp)
    .seh_savexmm %xmm7, 48
    movaps %xmm8, 64(%rsp)
    .seh_savexmm %xmm8, 64
    movaps %xmm9, 80(%rsp)
    .seh_savexmm %xmm9, 80
    movaps %xmm10, 96(%rsp)
    .seh_savexmm %xmm10, 96
    movaps %xmm11, 112(%rsp)
    .seh_savexmm %xmm11, 112
    movaps %xmm12, 128(%rsp)
    .seh_savexmm %xmm12, 128
    movaps %xmm13, 144(%rsp)
    .seh_savexmm %xmm13, 144
    movaps %xmm14, 160(%rsp)
    .seh_savexmm %xmm14, 160
    movaps %xmm15, 176(%rsp)
    .seh_savexmm %xmm15, 176
    .seh_endprologue
    movq %r8, %rbx
    pcmpeqd %xmm6, %xmm6
    movdqa %xmm6, %xmm7
    pslld $1, %xmm7
    movdqa %xmm7, %xmm8
    pslld $1, %xmm8
    movdqa %xmm8, %xmm9
    pslld $1, %xmm9
    movdqa %xmm9, %xmm10
    pslld $1, %xmm10
    movdqa %xmm10, %xmm11
    pslld $1, %xmm11
    movdqa %xmm11, %xmm12
    pslld $1, %xmm12
    movdqa %xmm12, %xmm13
    pslld $1, %xmm13
    movdqa %xmm13, %xmm14
    pslld $1, %xmm14
    movdqa %xmm14, %xmm15
    pslld $1, %xmm15
    fxsave64 (%rdx)
    call TestHostClone_nid_no_patch
    fxsave64 (%rbx)
    movaps 32(%rsp), %xmm6
    movaps 48(%rsp), %xmm7
    movaps 64(%rsp), %xmm8
    movaps 80(%rsp), %xmm9
    movaps 96(%rsp), %xmm10
    movaps 112(%rsp), %xmm11
    movaps 128(%rsp), %xmm12
    movaps 144(%rsp), %xmm13
    movaps 160(%rsp), %xmm14
    movaps 176(%rsp), %xmm15
    addq $192, %rsp
    popq %rbx
    ret
.seh_endproc
