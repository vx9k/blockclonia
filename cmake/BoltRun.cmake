# Run by cmake -P after the game links (see Bolt.cmake): instrument it, run
# the instrumented copy through --bench for a profile, then rewrite the
# binary with that profile. Inputs: BOLT (llvm-bolt), EXE (the linked game),
# WORK (a scratch directory in the build tree).

set(_inst "${WORK}/blockclonia.inst")
set(_fdata "${WORK}/bench.fdata")
set(_out "${WORK}/blockclonia.bolt")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/home")

function(run what)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _r OUTPUT_VARIABLE _o ERROR_VARIABLE _o)
  if(NOT _r EQUAL 0)
    message(FATAL_ERROR "BOLT: ${what} failed (${_r}):\n${_o}")
  endif()
endfunction()

run("instrumenting" "${BOLT}" "${EXE}" -instrument "--instrumentation-file=${_fdata}" -o "${_inst}")

# The training run: a private home, so it reads no settings and creates no
# directories outside the build tree.
set(ENV{HOME} "${WORK}/home")
set(ENV{XDG_CONFIG_HOME} "${WORK}/home/config")
set(ENV{XDG_DATA_HOME} "${WORK}/home/data")
set(ENV{XDG_STATE_HOME} "${WORK}/home/state")
set(ENV{XDG_CACHE_HOME} "${WORK}/home/cache")
run("the --bench training run" "${_inst}" --bench --no-sound)
if(NOT EXISTS "${_fdata}")
  message(FATAL_ERROR "BOLT: the training run wrote no profile (${_fdata})")
endif()

# ext-tsp lays out each function's blocks for fall-through on the hot path;
# cdsort orders functions by call density; split-functions moves the cold
# blocks to a separate section, away from the hot pages. icf folds
# identical functions, and gnu-stack keeps the stack non-executable.
# By default BOLT puts the new code in a fresh segment aligned for 2 MB
# pages, which file-backed code does not get without hugify: the stripped
# binary grew from 1 MB to 4 MB of mostly padding, for no measured gain.
# Regular pages let the new code go back into the old .text instead.
run("optimising" "${BOLT}" "${EXE}" "-data=${_fdata}" -o "${_out}"
    -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions -split-all-cold
    -split-eh -icf -use-gnu-stack -no-huge-pages -use-old-text -dyno-stats)

file(RENAME "${EXE}" "${EXE}.prebolt")
file(RENAME "${_out}" "${EXE}")
