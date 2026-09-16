#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="${source_dir}/build-linux"
runtime_dir="${source_dir}/linux_e2e/runtime"

cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}" -j"$(nproc)"
ctest --test-dir "${build_dir}" --output-on-failure

cmake -E remove_directory "${runtime_dir}"
cmake -E make_directory "${runtime_dir}"

"${build_dir}/linux_reconstruct_receiver" \
  19090 \
  "${runtime_dir}/receiver_spool" \
  "${script_dir}/spots_example.csv" \
  "${runtime_dir}/output" \
  256 256 >"${runtime_dir}/receiver.log" 2>&1 &
receiver_pid=$!
trap 'kill "${receiver_pid}" 2>/dev/null || true' EXIT
sleep 1

"${build_dir}/linux_pipeline_sender" \
  127.0.0.1 19090 \
  "${runtime_dir}/sender_spool" \
  "${script_dir}/scan_example.csv" \
  -1 640 480 30

wait "${receiver_pid}"
trap - EXIT

test -s "${runtime_dir}/output/reconstruction.tif"
test -s "${runtime_dir}/output/weight_map.tif"
test -s "${runtime_dir}/output/spot_signals.csv"
echo "Linux synthetic capture -> V2 transfer -> reconstruction: PASS"
