#!/usr/bin/env bash

dump_sanitizer_logs() {
  local base_dir
  local f
  local found=0
  local -a dirs=()

  if [ -n "${SANITIZER_LOG_DIR:-}" ]; then
    dirs+=("${SANITIZER_LOG_DIR}")
  fi
  dirs+=("${ROOT_DIR}/build-sanitizer/sanitizer-logs")
  dirs+=("${ROOT_DIR}/build/sanitizer-logs")

  for base_dir in "${dirs[@]}"; do
    if [ ! -d "${base_dir}" ]; then
      continue
    fi
    for f in "${base_dir}"/asan* "${base_dir}"/ubsan*; do
      if [ ! -f "${f}" ]; then
        continue
      fi
      found=1
      echo "[DEBUG] ----- sanitizer log: ${f} -----" >&2
      cat "${f}" >&2 || true
      echo >&2
      echo "[DEBUG] -----------------------------------" >&2
    done
  done

  if [ "${found}" -eq 1 ]; then
    echo "[DEBUG] sanitizer logs dumped." >&2
  fi
}
