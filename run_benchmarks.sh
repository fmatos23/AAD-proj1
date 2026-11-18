#!/bin/bash
#
# AAD 2026/2027, assignment A1 --- benchmarks (each run lasts SECONDS) collected in a CSV file
#
# usage: ./run_benchmarks.sh [seconds] [results.csv] [group ...]
#   groups: cpu cuda omp scaling mpi net wasm sha opencl ispc   (default: all of them)
#     cpu     : every single-core engine (scalar, avx, avx2, ...) on every format
#     cuda    : the CUDA engine on every format
#     omp     : best SIMD engine + OpenMP with all hardware threads, every format
#     scaling : best SIMD engine + OpenMP with 1, 2, 4, ... threads (PDF only)
#     mpi     : best SIMD engine + OpenMP + MPI (2 ranks, half of the threads each), every format
#     net     : server + 2 local clients (half of the threads each), every format
#     wasm    : WebAssembly scalar and SIMD engines under node, every format
#     sha     : SHA1 instructions engine (sha_ni or neon_sha1) + OpenMP with all hardware threads, every format
#     opencl  : the OpenCL engine (GPU) on every format (sha1_find_opencl -L lists the devices; choose one
#               with AAD_OPENCL_PLATFORM=p AAD_OPENCL_DEVICE=d)
#     ispc    : the ISPC engine, one thread and all hardware threads, every format
#   (the cpu group also runs the sha_ni / neon_sha1 engine on one core, when the processor has it)
#   groups whose tools are missing are skipped (nvcc, mpicc, emcc/node, OpenCL driver, ispc)
#
#   energy_J     : CPU package energy (RAPL), only if /sys/class/powercap/intel-rapl:0/energy_uj is readable
#                  (sudo chmod o+r /sys/class/powercap/intel-rapl:0/energy_uj)
#   gpu_energy_J : average nvidia-smi power.draw (sampled every 200 ms) x time (cuda only)
#
set -e
export LC_ALL=C # numbers with a decimal point (a pt_PT locale makes awk print "1630,444", which breaks the CSV)
cd "$(dirname "$0")"
SECONDS_PER_RUN=${1:-60}
CSV=${2:-benchmarks.csv}
shift 2 || true
GROUPS_TO_RUN=${*:-cpu cuda omp scaling mpi net wasm sha opencl ispc}
INPUTS="examples/deti_4_2_37_original.pdf examples/example_frame_001_original.png examples/example_frame_001.ppm results/sample_original.zip"
NPROC=$(nproc)
HALF=$((NPROC / 2))

[ -f "$CSV" ] || echo "version,format,host,device,threads,lanes,tail_chunks,seconds,attempts,attempts_per_min,chunk_updates_per_min,energy_J,gpu_energy_J" > "$CSV"
# number of fields of the last CSV line (a line written by the programs has 12, a finished line has 13)
last_fields() { python3 -c 'import csv,sys; r=[l for l in csv.reader(open(sys.argv[1])) if l]; print(len(r[-1]))' "$CSV"; }
# add the (empty) gpu_energy_J column, only to a line that was just written (if a run failed, nothing is changed)
no_gpu() { if [ "$(last_fields)" = 12 ]; then sed -i '$ s/$/,/' "$CSV"; fi; }
make -s sha1_find
ENGINES=$(./sha1_find 2>&1 | sed -n 's/^engines: //p')
SIMD=$(echo "$ENGINES" | tr ' ' '\n' | grep -v -x -e sha_ni -e neon_sha1 | tail -1)          # widest SIMD engine
SHA=$(echo "$ENGINES" | tr ' ' '\n' | grep -x -e sha_ni -e neon_sha1 | head -1 || true)    # SHA1 instructions engine

for g in $GROUPS_TO_RUN; do
  case $g in
    cpu)
      for e in $(./sha1_find 2>&1 | sed -n 's/^engines: //p'); do
        for f in $INPUTS; do
          echo "== cpu $e $f"; ./sha1_find -e "$e" -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1; no_gpu
        done
      done ;;
    cuda)
      command -v nvcc > /dev/null || { echo "no nvcc, skipping cuda"; continue; }
      make -s sha1_find_cuda
      for f in $INPUTS; do
        echo "== cuda $f"
        POWER_LOG=$(mktemp)
        nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits -lms 200 > "$POWER_LOG" &
        SAMPLER=$!
        ./sha1_find_cuda -e cuda -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1
        kill $SAMPLER; wait $SAMPLER 2> /dev/null || true
        GPU_J=$(awk -v t="$SECONDS_PER_RUN" '{s += $1; n++} END {if (n > 0) printf "%.3f", s / n * t}' "$POWER_LOG")
        rm -f "$POWER_LOG"
        if [ "$(last_fields)" = 12 ]; then sed -i '$ s/$/,'"$GPU_J"'/' "$CSV"; fi
      done ;;
    omp)
      for f in $INPUTS; do
        echo "== omp $SIMD x $NPROC $f"; ./sha1_find -e "$SIMD" -t 0 -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1; no_gpu
      done ;;
    scaling)
      for t in 1 2 4 6 8 12 16 $NPROC; do
        [ "$t" -le "$NPROC" ] || continue
        echo "== scaling $SIMD x $t"; ./sha1_find -e "$SIMD" -t "$t" -b "$SECONDS_PER_RUN" -c "$CSV" examples/deti_4_2_37_original.pdf | tail -1; no_gpu
      done ;;
    mpi)
      command -v mpicc > /dev/null || { echo "no mpicc, skipping mpi"; continue; }
      make -s sha1_find_mpi
      for f in $INPUTS; do
        echo "== mpi 2 x $HALF $f"
        mpirun -np 2 --bind-to none ./sha1_find_mpi -e "$SIMD" -t "$HALF" -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1; no_gpu
      done ;;
    net)
      make -s sha1_server sha1_client
      PORT=5199
      for f in $INPUTS; do
        echo "== server + 2 clients x $HALF $f"
        ./sha1_server -p $PORT -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1 &
        SERVER=$!
        sleep 0.5
        ./sha1_client -e "$SIMD" -t "$HALF" localhost $PORT > /dev/null &
        ./sha1_client -e "$SIMD" -t "$HALF" localhost $PORT > /dev/null &
        wait
        no_gpu
        PORT=$((PORT + 1))
      done ;;
    wasm)
      command -v emcc > /dev/null && command -v node > /dev/null || { echo "no emcc/node, skipping wasm"; continue; }
      make -s sha1_find_wasm.js
      for e in scalar wasm_simd; do
        for f in $INPUTS; do
          echo "== wasm $e $f"
          node sha1_find_wasm.js -e "$e" -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1
          # label the line (scalar -> wasm_scalar) and fix the host name (emscripten reports "emscripten")
          if [ "$(last_fields)" = 12 ]; then sed -i '$ s/^scalar,/wasm_scalar,/; $ s/,emscripten,/,'"$(hostname)"',/' "$CSV"; fi; no_gpu
        done
      done ;;
    sha)
      [ -n "$SHA" ] || { echo "no SHA1 instructions on this processor, skipping sha"; continue; }
      for f in $INPUTS; do
        echo "== sha $SHA x $NPROC $f"; ./sha1_find -e "$SHA" -t 0 -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1; no_gpu
      done ;;
    opencl)
      [ -f /usr/include/CL/cl.h ] || { echo "no OpenCL headers (sudo apt install ocl-icd-opencl-dev), skipping opencl"; continue; }
      make -s sha1_find_opencl || { echo "unable to build sha1_find_opencl, skipping opencl"; continue; }
      ./sha1_find_opencl -L
      for f in $INPUTS; do
        echo "== opencl $f"; ./sha1_find_opencl -e opencl -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1; no_gpu
      done ;;
    ispc)
      command -v ispc > /dev/null || { echo "no ispc (sudo apt install ispc), skipping ispc"; continue; }
      make -s sha1_find_ispc || { echo "unable to build sha1_find_ispc, skipping ispc"; continue; }
      for f in $INPUTS; do
        echo "== ispc $f"; ./sha1_find_ispc -e ispc -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1; no_gpu
        echo "== ispc x $NPROC $f"; ./sha1_find_ispc -e ispc -t 0 -b "$SECONDS_PER_RUN" -c "$CSV" "$f" | tail -1; no_gpu
      done ;;
    *) echo "unknown group $g" ;;
  esac
done
echo "results in $CSV"
