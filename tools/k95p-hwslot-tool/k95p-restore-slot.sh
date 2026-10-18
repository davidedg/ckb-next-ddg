#!/usr/bin/env bash
# k95p-restore-slot.sh - writes one hardware slot of a K95 RGB Platinum (1b1c:1b2d) from an iCUE local copy of that slot
# (hw_profiles/{GUID}.cueprofiledata). Three steps, each with its own directory, never reused:
#
#   ./k95p-restore-slot.sh dump NEW_DUMP     (the image in NEW_DUMP/dump: the DUMP of prepare)
#       Read-only dump of the keyboard (k95p-hwslot-tool dump: reads only, and a final 07 04 01). Daemon and GUI stopped.
#   ./k95p-restore-slot.sh prepare SOURCE SLOT NAME DUMP NEW_WORK [K95P_HWSLOT_TOOL]
#       Offline. Converts SOURCE (icue-local-to-xml.py), compiles the slot over a frozen copy of DUMP (k95p-hwslot-tool zip
#       --new, check), rehearses the write on the simulator loaded with DUMP and dumps the simulated result: the prediction of
#       the files, of the packets and of the cookie. Seals tool, source, base, image and prediction in NEW_WORK/SHA256SUMS.
#       K95P_HWSLOT_TOOL defaults to a new build (build.sh) into NEW_WORK/bin.
#   K95P_RESTORE_GO=1 ./k95p-restore-slot.sh go WORK NEW_RUN
#       The write to the keyboard's flash; K95P_RESTORE_GO=1 says it is meant. Verifies the seal, that daemon/GUI/service are
#       stopped and exactly one K95P is connected; starts dumpcap on its bus before any packet (K95P_RESTORE_CAPTURE=0: no
#       capture); writes and verifies the slot from a pristine copy of the image with the base of the dump (k95p-hwslot-tool
#       convert --go); dumps the keyboard again read-only; stops the capture by exact PID; compares packets and files with
#       the prediction.
#
# Every other Corsair device is never opened: k95p-hwslot-tool selects only HID_ID 0003:1B1C:1B2D and refuses while a
# ckb-next daemon runs. No automatic retry of a write: a failed go leaves NEW_RUN for analysis. dump and go need root for
# hidraw (sudo, asked once at the start); the files they make are given back to the user.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$(cd -- "$here/../.." && pwd)

die(){ echo "STOP: $*" >&2; exit 3; }

stopped(){
    for name in ckb-next-daemon ckb-next; do
        if pgrep -x "$name" >/dev/null; then die "$name is running"; fi
    done
    if command -v systemctl >/dev/null; then
        [[ $(systemctl is-active ckb-next-daemon || true) == inactive ]] || die 'ckb-next-daemon service not inactive'
        [[ $(systemctl is-enabled ckb-next-daemon 2>/dev/null || true) != enabled ]] \
            || echo 'note: the ckb-next-daemon service is enabled (it is inactive now; it starts again at boot)'
    fi
    mapfile -t devices < <(lsusb -d 1b1c:1b2d)
    [[ ${#devices[@]} == 1 ]] || die "expected exactly one K95P 1b1c:1b2d, found ${#devices[@]}"
    read -r bus dev < <(sed -n 's/^Bus \([0-9]*\) Device \([0-9]*\):.*/\1 \2/p' <<<"${devices[0]}")
    bus=$((10#$bus)); dev=$((10#$dev))
}

# The tool on the keyboard needs root for hidraw; the files it makes are given back to the user
need_root(){ [[ $EUID == 0 ]] || sudo -n true 2>/dev/null || sudo -v || die 'sudo is needed for hidraw'; }
as_root(){ if [[ $EUID == 0 ]]; then "$@"; else sudo -n "$@"; fi; }
give_back(){ as_root chown -R "$(id -u):$(id -g)" -- "$@"; }

source_rev(){
    local rev
    rev=$(git -C "$src" rev-parse HEAD 2>/dev/null) || { echo 'not a git checkout'; return; }
    printf '%s%s\n' "$rev" "$(git -C "$src" status --porcelain 2>/dev/null | grep -q . && echo ' DIRTY')"
}

tx_lines(){ grep '^TX ' -- "$1" || true; }

compare_dumps(){   # A B: every slot file the same, byte for byte
    local a=$1 b=$2 bad=0 n=0
    while read -r f; do
        n=$((n + 1))
        if [[ ! -e "$b/$f" ]]; then echo "MISSING in $b: $f"; bad=1
        elif ! cmp -s -- "$a/$f" "$b/$f"; then echo "DIFF $f"; bad=1; fi
    done < <(cd -- "$a" && ls | grep '^slot[0-9]_.*_read_[0-9]*\.bin$')
    while read -r f; do
        [[ -e "$a/$f" ]] || { echo "EXTRA in $b: $f"; bad=1; }
    done < <(cd -- "$b" && ls | grep '^slot[0-9]_.*_read_[0-9]*\.bin$')
    echo "$n files compared, $([[ $bad == 0 ]] && echo 'all the same' || echo 'DIFFERENCES')"
    return $bad
}

cmd_dump(){
    [[ $# == 1 ]] || die 'dump NEW_DUMP'
    local out; out=$(realpath -m -- "$1")
    [[ ! -e "$out" ]] || die "$out already exists"
    stopped
    # NEW_DUMP holds the tool and the logs; the image itself is NEW_DUMP/dump, a new directory as k95p-hwslot-tool dump requires
    mkdir -p -- "$out"
    local tool; tool=$(OUT="$out/bin" "$here/build.sh" 2>"$out/build.log") || { cat "$out/build.log"; die 'k95p-hwslot-tool build failed'; }
    source_rev > "$out/source-rev.txt"
    need_root
    as_root "$tool" --hidraw --transcript "$out/transcript.txt" dump "$out/dump" > "$out/dump.log" 2>&1 || { give_back "$out"; cat "$out/dump.log"; die 'dump failed'; }
    give_back "$out"
    cat "$out/dump.log"
    tx_lines "$out/transcript.txt" | cut -c4-11 | sort | uniq -c
}

cmd_prepare(){
    [[ $# == 5 || $# == 6 ]] || die 'prepare SOURCE SLOT NAME DUMP NEW_WORK [K95P_HWSLOT_TOOL]'
    local source=$1 slot=$2 name=$3 dump; dump=$(realpath -- "$4")
    local work; work=$(realpath -m -- "$5")
    [[ "$slot" =~ ^[012]$ ]] || die 'SLOT is 0, 1 or 2 (M1, M2, M3)'
    [[ ! -e "$work" ]] || die "$work already exists"
    mkdir -p -- "$work"
    local tool
    if [[ $# == 6 ]]; then
        mkdir -p -- "$work/bin"; cp -- "$6" "$work/bin/k95p-hwslot-tool"
    else
        OUT="$work/bin" "$here/build.sh" > /dev/null
    fi
    tool=$work/bin/k95p-hwslot-tool
    echo "source $(source_rev)" > "$work/bin/provenance.txt"
    cp -- "$source" "$work/source.cueprofiledata"
    printf '%s\n' "$(realpath -- "$source")" > "$work/source-path.txt"
    cp -a -- "$dump" "$work/base"
    rm -f -- "$work/base/attempts.txt"

    python3 -I "$here/icue-local-to-xml.py" "$work/source.cueprofiledata" -o "$work/profile.xml" --meta "$work/meta.json" > "$work/convert-xml.log"
    local guid meta_slot meta_cookie
    read -r guid meta_slot meta_cookie < <(python3 -I -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m["id"].strip("{}"), m["slotIndex"], m["cookie"])' "$work/meta.json")
    [[ "$meta_slot" == "$slot" ]] || die "the local copy says slotIndex $meta_slot, not $slot"

    # A new profile is made in an empty image with the keyboard's layout, as the daemon makes one
    [[ -s "$work/base/layout.txt" ]] || die 'the dump has no layout.txt'
    mkdir -p -- "$work/empty"
    cp -- "$work/base/layout.txt" "$work/empty/layout.txt"
    printf '# slot, GUID, cookie, flag\n0 %032d 0 0\n1 %032d 0 0\n2 %032d 0 0\n' 0 0 0 > "$work/empty/slot-table.txt"
    "$tool" zip "$work/empty" --slot "$slot" --xml "$work/profile.xml" --new "$guid" "$name" -o "$work/img" > "$work/zip.log"
    "$tool" check "$work/img" --slot "$slot" > "$work/check.log"
    grep -q ' 0 differences' "$work/check.log" || { cat "$work/check.log"; die 'the image does not check clean'; }

    # The base the write must find: the dump's entry of the slot, or an empty slot
    local now
    # (convert without --go only reads; it refuses, as expected, a slot that is neither empty nor the image)
    now=$({ "$tool" --sim "$work/base" convert --slot "$slot" "$work/img" --base retry 2>&1 || true; } \
        | sed -n 's/.*on the keyboard now \([^,]*\), the image.*/\1/p')
    [[ -n "$now" ]] || die 'the slot entry of the dump was not read'
    local base=(--base from "$work/base")
    [[ "$now" == empty ]] && base=(--base empty)
    printf '%s\n' "${base[@]}" > "$work/base-args.txt"
    printf '%s\n' "$meta_cookie" > "$work/cookie-at-least.txt"
    printf '%s\n' "$slot" > "$work/slot.txt"

    # Rehearsal on the simulator, from a copy of the image: convert records its attempts in the image directory
    cp -a -- "$work/img" "$work/img-sim"
    mkdir -p -- "$work/predicted"
    "$tool" --sim "$work/base" --sim-out "$work/predicted/keyboard" --transcript "$work/predicted/transcript.txt" \
        convert --slot "$slot" "$work/img-sim" "${base[@]}" --cookie-at-least "$meta_cookie" --dump "$work/base" --go \
        > "$work/predicted/convert.log" 2>&1 || { cat "$work/predicted/convert.log"; die 'the rehearsal failed'; }
    "$tool" --sim "$work/predicted/keyboard" --transcript "$work/predicted/dump-transcript.txt" dump "$work/predicted/dump" \
        > "$work/predicted/dump.log" 2>&1
    rm -rf -- "$work/img-sim"

    # The prediction holds the other slots as they are and the image in the slot, PROFILE.I with its new cookie apart
    local other=0
    for f in $(cd -- "$work/base" && ls | grep '^slot[0-9]_.*_read_[0-9]*\.bin$'); do
        [[ $f == slot${slot}_* ]] && continue
        cmp -s -- "$work/base/$f" "$work/predicted/dump/$f" || { echo "DIFF other slot $f"; other=1; }
    done
    [[ $other == 0 ]] || die 'the rehearsal changed another slot'
    { echo "Prediction of the restore (simulator loaded with the dump)"
      echo "slot $slot (M$((slot + 1))), GUID $guid, name $name; on the keyboard now $now; ${base[*]/$work/WORK}"
      grep -h '^convert\|^write\|^verify' "$work/predicted/convert.log"
      echo "TX packets: $(tx_lines "$work/predicted/transcript.txt" | wc -l)"
      tx_lines "$work/predicted/transcript.txt" | cut -c4-11 | sort | uniq -c
      echo "files of the slot after the write:"
      (cd -- "$work/predicted/dump" && for f in slot${slot}_*_read_*.bin; do
           b=${f/_read_/_write_}; s=same
           [[ -e "$work/img/$b" ]] || s=absent-from-image
           [[ -e "$work/img/$b" ]] && ! cmp -s "$f" "$work/img/$b" && s=differs
           echo "  $f $(stat -c %s "$f") bytes, $s"; done)
      echo "other slots: byte-identical to the dump"; } > "$work/prediction.txt"
    (cd -- "$work" && find bin source.cueprofiledata base empty img profile.xml meta.json predicted base-args.txt cookie-at-least.txt slot.txt -type f -print0 \
        | sort -z | xargs -0 sha256sum) > "$work/SHA256SUMS"
    cat "$work/prediction.txt"
}

proc_start(){ python3 -I -c 'import sys; print(open("/proc/"+sys.argv[1]+"/stat").read().rsplit(")",1)[1].split()[19])' "$1"; }

cmd_go(){
    [[ $# == 2 ]] || die 'go WORK NEW_RUN'
    [[ ${K95P_RESTORE_GO:-0} == 1 ]] || die 'go writes the flash of the keyboard: K95P_RESTORE_GO=1 to mean it'
    local work run; work=$(realpath -- "$1"); run=$(realpath -m -- "$2")
    [[ ! -e "$run" ]] || die "$run already exists"
    (cd -- "$work" && sha256sum --quiet -c SHA256SUMS) || die 'the sealed work directory changed'
    local capture=${K95P_RESTORE_CAPTURE:-1}
    [[ $capture == 0 ]] || command -v dumpcap >/dev/null || die 'dumpcap not found (Wireshark); K95P_RESTORE_CAPTURE=0 for no capture'
    stopped
    need_root
    local tool=$work/bin/k95p-hwslot-tool slot; slot=$(cat "$work/slot.txt")
    mapfile -t base < "$work/base-args.txt"
    mkdir -p -- "$run"
    printf '%s\n' "$work" > "$run/work.txt"
    printf '%s.%s\n' "$bus" "$dev" > "$run/address.txt"
    cp -a -- "$work/img" "$run/img"

    capture_pid= capture_start= pcap=
    finish(){
        local code=$?
        if [[ -n "$capture_pid" && -r /proc/$capture_pid/stat ]]; then
            local now_start; now_start=$(proc_start "$capture_pid")
            if [[ "$now_start" == "$capture_start" && $(ps -o comm= -p "$capture_pid") == dumpcap ]]; then
                as_root kill -INT "$capture_pid"
                timeout 20 tail --pid="$capture_pid" -f /dev/null || { echo 'STOP: capture did not stop; staging file kept'; return; }
            else
                echo 'STOP: capture identity changed; no signal sent'; return
            fi
        fi
        if [[ -n "$pcap" && -s "$pcap" ]]; then
            as_root chown "$(id -u):$(id -g)" "$pcap"
            cp -- "$pcap" "$run/usbmon.pcapng" && cmp -- "$pcap" "$run/usbmon.pcapng" && rm -- "$pcap"
            sha256sum "$run/usbmon.pcapng" > "$run/pcap.sha256"
        fi
        return "$code"
    }
    if [[ $capture != 0 ]]; then
        as_root modprobe usbmon
        pcap=$(mktemp --suffix=.pcapng /var/tmp/k95p-restore-slot.XXXXXXXX); rm -- "$pcap"
        printf '%s\n' "$pcap" > "$run/pcap-staging.txt"
        trap finish EXIT
        # sudo before setsid: a detached process has no terminal for sudo's cached credentials
        as_root setsid -f sh -c 'echo $$ > "$1"; shift; exec "$@"' sh "$run/capture.pid" \
            "$(command -v dumpcap)" -q -i "usbmon$bus" -w "$pcap" > "$run/capture.log" 2>&1
        for _ in $(seq 100); do
            if [[ -s "$run/capture.pid" && -s "$pcap" ]] && grep -Fq "File: $pcap" "$run/capture.log"; then break; fi
            sleep .1
        done
        [[ -s "$run/capture.pid" && -s "$pcap" ]] || die 'capture not ready; nothing was sent'
        capture_pid=$(cat "$run/capture.pid")
        [[ $(ps -o comm= -p "$capture_pid") == dumpcap ]] || die 'capture process not dumpcap; nothing was sent'
        capture_start=$(proc_start "$capture_pid")
    else
        echo 'no USB capture (K95P_RESTORE_CAPTURE=0)' | tee "$run/capture.log"
    fi

    local rc=0
    as_root "$tool" --hidraw --transcript "$run/transcript.txt" convert --slot "$slot" "$run/img" "${base[@]}" \
        --cookie-at-least "$(cat "$work/cookie-at-least.txt")" --dump "$work/base" --go > "$run/convert.log" 2>&1 || rc=$?
    give_back "$run"
    cat "$run/convert.log"
    if [[ $rc == 0 ]]; then
        as_root "$tool" --hidraw --transcript "$run/dump-transcript.txt" dump "$run/after" > "$run/dump.log" 2>&1 || rc=$?
        give_back "$run"
        cat "$run/dump.log"
    fi
    finish; trap - EXIT
    [[ $rc == 0 ]] || die "k95p-hwslot-tool exit $rc; see $run (no retry)"

    local ok=0
    echo '--- packets sent, run against prediction'
    if diff <(tx_lines "$work/predicted/transcript.txt") <(tx_lines "$run/transcript.txt") > "$run/tx.diff"; then
        echo "TX the same: $(tx_lines "$run/transcript.txt" | wc -l) packets"
    else
        echo "TX DIFFERENT: $run/tx.diff"; ok=1
    fi
    echo '--- dump after the write, against the predicted dump'
    compare_dumps "$work/predicted/dump" "$run/after" | tee "$run/files.txt" || ok=1
    return $ok
}

case ${1:-} in
    dump) shift; cmd_dump "$@" ;;
    prepare) shift; cmd_prepare "$@" ;;
    go) shift; cmd_go "$@" ;;
    *) die 'usage: k95p-restore-slot.sh dump NEW_DUMP | prepare SOURCE SLOT NAME DUMP NEW_WORK [K95P_HWSLOT_TOOL] | go WORK NEW_RUN' ;;
esac
