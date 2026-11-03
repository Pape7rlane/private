# Local patch to vendored Dobby

File: source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.asm

Upstream uses ARM Compiler 6 syntax for the closure bridge call:

    adrp x17, sym@PAGE
    add  x17, x17, sym@PAGEOFF

LLVM 15+ (NDK r26) rejects this with "invalid symbol kind for ADRP relocation".
Rewritten to standard GAS syntax:

    adrp x17, sym
    add  x17, x17, :lo12:sym

Keep this patch when bumping the Dobby revision.
