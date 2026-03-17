# inject_arm64_ftz.cmake — Inject ARM64 FPCR.FZ (flush-to-zero) setup into
# the Faust-generated compute() method.
#
# Faust's -ftz 1 only guards recursive signal assignments (fRec). This injects
# hardware-level FTZ at the top of compute() to catch ALL denormals including
# intermediates, matching x86's MXCSR FTZ/DAZ behavior.
#
# Usage: cmake -DFILE=<path> -P inject_arm64_ftz.cmake

if(NOT DEFINED FILE)
    message(FATAL_ERROR "FILE not specified")
endif()

file(READ "${FILE}" content)

# Match both with and without RESTRICT macro in the signature
string(REGEX REPLACE
    "(virtual void compute\\(int count, FAUSTFLOAT\\*\\*[^)]*\\) \\{)"
    "\\1\n#if defined(__aarch64__) || defined(_M_ARM64)\n\t\t{ uint64_t __fpcr; asm volatile(\"mrs %0, fpcr\" : \"=r\"(__fpcr)); __fpcr |= (1ULL << 24); asm volatile(\"msr fpcr, %0\" :: \"r\"(__fpcr)); }\n#endif\n"
    content "${content}"
)

file(WRITE "${FILE}" "${content}")
