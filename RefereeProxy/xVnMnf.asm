; xVnMnf trampoline.
;
; The client passes its arguments straight through to the protector's xVnMnf
; (unknown arity, unknown types), so we forward the machine state untouched.
; A plain tail jump keeps everything -- registers, stack arguments, shadow
; space, return address -- in place; the protector never knows we existed.

        EXTERN g_realXNfn:QWORD

        .code

xVnMnf PROC
        mov     rax, [g_realXNfn]
        jmp     rax
xVnMnf ENDP

        END
