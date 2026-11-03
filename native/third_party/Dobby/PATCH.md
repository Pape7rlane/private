# Local patches to vendored Dobby

Vendored at commit e9fe7fb (parent of the 2023 "Refactor? Yes" commit).
Master after that commit does not compile (missing source/core/arch/Cpu.h).

## 1. external/logging/logging/logging.h

Logger::Shared() is defined in a header without the inline keyword, so every
translation unit that includes it emits a definition and the link fails with
"duplicate symbol: Logger::Shared()". Added inline.

## 2. closure_bridge_arm64.asm (not present at this commit)

The newer tree uses ARM Compiler 6 syntax (sym@PAGE / sym@PAGEOFF) which
LLVM 15+ rejects. If Dobby is bumped, rewrite those two lines to:
    adrp x17, sym
    add  x17, x17, :lo12:sym
