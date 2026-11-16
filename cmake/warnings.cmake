# blockclonia compiler warning policy.
#
# Included by the top-level CMakeLists.txt, which applies MC_WARNINGS to our
# own targets through the mc_warnings interface library. CI configures with
# -DMC_WERROR=ON (the ci preset).
#
# Everything below is clean on clang 14 to 23, on x86_64, arm64 and 32-bit
# ARM (ILP32), once the fixes in audits/lint.md are applied. The build is
# clang-only (cmake/toolchains/llvm.cmake), so there are no gcc-only flags.

option(MC_WERROR "Treat compiler warnings as errors (use in CI)" OFF)

set(MC_WARNINGS
  -Wall -Wextra -Wpedantic
  -Wshadow
  -Wconversion -Wsign-conversion   # implicit narrowing / sign changes
  -Wdouble-promotion               # float code must not silently compute in double
  -Wcast-qual -Wwrite-strings -Wpointer-arith
  -Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations -Wredundant-decls
  -Wformat=2
  -Wundef
  -Wvla
  -Wimplicit-fallthrough
  -Wframe-larger-than=65536        # threads get OS_THREAD_STACK (4 MB); keep bigger buffers on the heap
  -Wcomma
  -Wunreachable-code-aggressive
  -Wmissing-noreturn
  -Wshorten-64-to-32               # catches VkDeviceSize -> size_t on 32-bit ARM
  -Wformat-signedness              # was gcc-only; clang has it from 19
)
include(CheckCCompilerFlag)
check_c_compiler_flag(-Wformat-signedness MC_HAS_WFORMAT_SIGNEDNESS)
if(NOT MC_HAS_WFORMAT_SIGNEDNESS)
  list(REMOVE_ITEM MC_WARNINGS -Wformat-signedness)
endif()
# ASan pads every stack object with redzones, so its frames say nothing
# about the real ones (clang 18 puts the test runner's main at 117 KB);
# every other build still holds the limit.
if(MC_SANITIZE)
  list(REMOVE_ITEM MC_WARNINGS -Wframe-larger-than=65536)
endif()

if(MC_WERROR)
  list(APPEND MC_WARNINGS -Werror)
endif()

# Deliberately NOT enabled (measured on commit 69726bc; noise for this code):
#   -Wfloat-equal             13 hits, all intentional exact compares (sweep results, dir == 0)
#   -Wbad-function-cast       36 hits, the (int)floor(x) idiom
#   -Wcast-align(=strict)     4 hits, casts into mapped Vulkan memory that is 16-byte aligned
#   -Wunsuffixed-float-constants  124 hits, physics is double on purpose
#   -Wswitch-enum             VK_*_MAX_ENUM sentinels
#   -Wpadded, -Winline, -Wdeclaration-after-statement, -Wunsafe-buffer-usage
#   -Wformat-truncation=2     1 hit, the window title (truncation is harmless there)
#   -Wconditional-uninitialized  2 false positives in mathlib.h loops
