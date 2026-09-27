#!/usr/bin/env bash
# blockclonia lint gate: warnings-as-errors builds (clang + gcc), unit tests,
# clang-tidy, cppcheck, shader checks, clang-format on the working-tree diff.
# Usage: [MC_FORMAT_BASE=<commit>] tools/lint.sh [build-root]   (default: ./build-lint)
# Needs: cmake, ninja, gcc, clang, clang-tidy 18, cppcheck 2.13+, glslc,
#        glslangValidator (optional), clang-format 18, git (for the format step).
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$src/build-lint}
jobs=$(nproc 2>/dev/null || echo 4)
fail=0
step() { printf '\n== %s\n' "$*"; }

for cc in clang gcc; do
  step "build ($cc, -Werror)"
  CC=$cc cmake -S "$src" -B "$out/$cc" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMC_WERROR=ON >/dev/null
  cmake --build "$out/$cc" || fail=1
  "$out/$cc/mc_tests" || fail=1
done

step "clang-tidy (config: .clang-tidy)"
# The clang build's compile_commands.json has no gcc-only warning flags.
tidy_log="$out/clang-tidy.log"
run-clang-tidy -p "$out/clang" -j "$jobs" -quiet "$src/(src|tests)/.*\\.c\$" >"$tidy_log" 2>&1 || true
if grep -E 'warning:|error:' "$tidy_log"; then fail=1; else echo "clean"; fi

step "cppcheck"
mkdir -p "$out/cppcheck"
cppcheck --project="$out/gcc/compile_commands.json" --enable=all --inconclusive --std=c11 \
  --library=posix --inline-suppr --cppcheck-build-dir="$out/cppcheck" -j "$jobs" -q \
  --suppressions-list="$src/tools/cppcheck-suppressions.txt" --error-exitcode=1 \
  --template='{file}:{line}: {severity}: {message} [{id}]' || fail=1

step "shaders (glslc -Werror, cross-stage link)"
for s in block.vert block.frag entity.vert line.vert line.frag ui.vert ui.frag; do
  glslc -Werror -O --target-env=vulkan1.0 -I "$src/shaders" -o /dev/null "$src/shaders/$s" || fail=1
done
if command -v glslangValidator >/dev/null; then
  for pair in "block.vert block.frag" "entity.vert block.frag" "line.vert line.frag" "ui.vert ui.frag"; do
    (cd "$src/shaders" && glslangValidator -V --target-env vulkan1.0 -l $pair -o /dev/null >/dev/null) \
      || { echo "link failed: $pair"; fail=1; }
  done
fi

base=${MC_FORMAT_BASE:-HEAD}   # in CI: the merge base with the main branch
step "clang-format (lines changed since $base only)"
if git -C "$src" rev-parse --git-dir >/dev/null 2>&1; then
  diff=$(cd "$src" && git clang-format --diff --extensions c,h "$base" -- src tests 2>/dev/null || true)
  if [ -n "$diff" ] && ! printf '%s' "$diff" | grep -qE '^(no modified files|clang-format did not modify)'; then
    printf '%s\n' "$diff"; fail=1
  else
    echo "clean"
  fi
fi

exit $fail
