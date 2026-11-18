#!/usr/bin/env bash
# blockclonia lint gate: a warnings-as-errors build (clang, lld), unit tests,
# clang-tidy, cppcheck, shader checks, the CI and release tools' tests,
# clang-format on the working-tree diff.
# Usage: [MC_FORMAT_BASE=<commit>] tools/lint.sh [build-root]   (default: ./build-lint)
# Needs: cmake, ninja, clang, lld, llvm, clang-tidy 18, cppcheck 2.13+, glslc,
#        glslangValidator (optional), clang-format 18, git (for the format step),
#        python3 3.8+.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$src/build-lint}
jobs=$(nproc 2>/dev/null || echo 4)
fail=0
step() { printf '\n== %s\n' "$*"; }

step "build (clang, -Werror)"
cmake -S "$src" -B "$out/clang" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMC_WERROR=ON >/dev/null
cmake --build "$out/clang" || fail=1
"$out/clang/mc_tests" || fail=1

step "clang-tidy (config: .clang-tidy)"
tidy_log="$out/clang-tidy.log"
run-clang-tidy -p "$out/clang" -j "$jobs" -quiet "$src/(src|tests)/.*\\.c\$" >"$tidy_log" 2>&1 || true
if grep -E 'warning:|error:' "$tidy_log"; then fail=1; else echo "clean"; fi

step "cppcheck"
mkdir -p "$out/cppcheck"
# The project file also lists GLFW and mimalloc when MC_DEPS=AUTO builds
# them from source; only the project's own files are checked. The file
# filter only selects sources, so findings in fetched headers (mimalloc.h's
# inline functions) are suppressed separately.
cppcheck --project="$out/clang/compile_commands.json" --enable=all --inconclusive --std=c11 \
  --file-filter="$src/src/*" --file-filter="$src/tests/*" --suppress='*:*/_deps/*' \
  --library=posix --inline-suppr --cppcheck-build-dir="$out/cppcheck" -j "$jobs" -q \
  --suppressions-list="$src/tools/cppcheck-suppressions.txt" --error-exitcode=1 \
  --template='{file}:{line}: {severity}: {message} [{id}]' || fail=1

step "shaders (glslc -Werror, cross-stage link)"
for s in block.vert block.frag entity.vert entity.frag line.vert line.frag ui.vert ui.frag star.vert star.frag sky.vert sky.frag post.vert post.frag bloom.frag \
  block_shadow.vert block_shadow.frag shadow.vert; do
  glslc -Werror -O --target-env=vulkan1.2 -I "$src/shaders" -o /dev/null "$src/shaders/$s" || fail=1
done
if command -v glslangValidator >/dev/null; then
  for pair in "block.vert block.frag" "entity.vert entity.frag" "line.vert line.frag" "ui.vert ui.frag" "star.vert star.frag" "sky.vert sky.frag" "post.vert post.frag" "post.vert bloom.frag" "block_shadow.vert block_shadow.frag"; do
    (cd "$src/shaders" && glslangValidator -V --target-env vulkan1.2 -l $pair -o /dev/null >/dev/null) \
      || { echo "link failed: $pair"; fail=1; }
  done
fi

step "CI and release tools (tools/tests)"
python3 -m unittest discover -s "$src/tools/tests" || fail=1

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
