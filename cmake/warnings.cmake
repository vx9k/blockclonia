# blockclonia compiler warning policy.
#
# Included by the top-level CMakeLists.txt, which applies MC_WARNINGS to our
# own targets through the mc_warnings interface library. CI configures with
# -DMC_WERROR=ON (the ci-* presets).
#
# Everything below is clean on gcc 13 and clang 18, on x86_64 and on a 32-bit
# (ILP32) syntax pass, once the fixes in audits/lint.md are applied.

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
  -Wframe-larger-than=16384        # largest frame today is 7.2 KB (mesher, on worker threads)
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  list(APPEND MC_WARNINGS
    -Wduplicated-cond -Wduplicated-branches -Wlogical-op
    -Wjump-misses-init
    -Wformat-signedness
    -Wnested-externs -Wold-style-definition)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  list(APPEND MC_WARNINGS
    -Wcomma
    -Wunreachable-code-aggressive
    -Wmissing-noreturn
    -Wshorten-64-to-32)            # catches VkDeviceSize -> size_t on 32-bit ARM
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
