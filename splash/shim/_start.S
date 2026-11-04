.section .text._start, "ax"
.global _start
.type _start, %function

_start:
    mov x0, sp
    b shim_main

.size _start, . - _start
