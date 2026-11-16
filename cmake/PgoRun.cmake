# Run by cmake -P (see Pgo.cmake): run the instrumented game through --bench
# and merge what it wrote into one profile. Inputs: PROFDATA_TOOL
# (llvm-profdata), EXE (the instrumented game), WORK (a scratch directory in
# the build tree), OUT (the merged profile).

file(REMOVE_RECURSE "${WORK}/raw" "${WORK}/home")
file(MAKE_DIRECTORY "${WORK}/raw" "${WORK}/home")

function(run what)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _r OUTPUT_VARIABLE _o ERROR_VARIABLE _o)
  if(NOT _r EQUAL 0)
    message(FATAL_ERROR "PGO: ${what} failed (${_r}):\n${_o}")
  endif()
endfunction()

# A private home, so the run reads no settings and creates no directories
# outside the build tree. %p: one file per process, should --bench ever fork.
set(ENV{HOME} "${WORK}/home")
set(ENV{XDG_CONFIG_HOME} "${WORK}/home/config")
set(ENV{XDG_DATA_HOME} "${WORK}/home/data")
set(ENV{XDG_STATE_HOME} "${WORK}/home/state")
set(ENV{XDG_CACHE_HOME} "${WORK}/home/cache")
set(ENV{LLVM_PROFILE_FILE} "${WORK}/raw/bench-%p.profraw")
run("the --bench training run" "${EXE}" --bench --no-sound)

file(GLOB _raw "${WORK}/raw/*.profraw")
if(NOT _raw)
  message(FATAL_ERROR "PGO: the training run wrote no profile in ${WORK}/raw")
endif()
# Written beside the old profile and renamed over it, so an interrupted
# merge never leaves a half-written one for the compiler to read.
run("merging the profile" "${PROFDATA_TOOL}" merge -o "${OUT}.tmp" ${_raw})
file(RENAME "${OUT}.tmp" "${OUT}")
