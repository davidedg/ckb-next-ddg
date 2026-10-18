#!/bin/bash
# build.sh - builds k95p-hwslot-tool from this directory and the daemon sources of the same checkout (src/daemon), into
# ./build (or OUT=...). Prints the path of the binary. Needs a C compiler and zlib.
#   ./build.sh [CFLAGS...]      (e.g. -fsanitize=address,undefined; WERROR=1 makes warnings errors)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
out=${OUT:-$here/build}
mkdir -p "$out"
d=$src/src/daemon
[[ -f $d/cape.c ]] || { echo "build.sh: $d has no cape.c (run it from a ckb-next checkout with K95P support)" >&2; exit 1; }
werror=()
[[ ${WERROR:-0} == 1 ]] && werror=(-Werror)
${CC:-cc} -std=gnu11 -g -O1 -Wall -Wextra "${werror[@]}" "$@" -I"$d" -I"$here" \
    "$here/k95p-hwslot-tool.c" "$here/hidraw_io.c" "$here/cape_sim.c" \
    "$d/cape.c" "$d/cape_fw.c" "$d/cape_format.c" "$d/cape_progress.c" "$d/cape_write.c" "$d/cape_slot.c" "$d/cape_binding.c" \
    "$d/cape_lightmodel.c" "$d/cape_hwslot.c" "$d/cape_light.c" "$d/cape_compile.c" "$d/cape_profile.c" "$d/cape_names.c" "$d/cape_xml.c" "$d/cape_qz.c" "$d/cape_zipslot.c" \
    -lz -lm -o "$out/k95p-hwslot-tool"
printf '%s\n' "$out/k95p-hwslot-tool"
