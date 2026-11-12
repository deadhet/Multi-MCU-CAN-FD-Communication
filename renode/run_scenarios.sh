#!/usr/bin/env bash
# Runs the Design Document's scenarios S1-S4 in Renode and checks each node's log against the
# events the design expects, in order. Prints PASS or FAIL per check and a summary; exits 1 if
# any check fails. Logs are kept in build/scenarios/<scenario>/mcuN.log.
#   renode/run_scenarios.sh            all four
#   renode/run_scenarios.sh s2 s4      some of them
# Build the firmware first (cmake --build build), and do not rebuild while this runs.
set -u

repo="$(cd "$(dirname "$0")/.." && pwd)"
out="$repo/build/scenarios"
pass=0
fail=0

run() { # name script: runs one scenario and keeps its four logs
    local name="$1" script="$2"
    echo "## $name ($script)"
    rm -f "$repo"/logs/mcu[1-4].log
    timeout 900 renode --disable-gui --console -e "include @$repo/renode/$script" </dev/null >"$repo/logs/$name.console" 2>&1
    rm -rf "${out:?}/$name"
    mkdir -p "$out/$name"
    cp "$repo"/logs/mcu[1-4].log "$out/$name/"
}

check() { # description command...: one PASS/FAIL line
    local what="$1"
    shift
    if "$@"; then
        echo "  PASS $what"
        pass=$((pass + 1))
    else
        echo "  FAIL $what"
        fail=$((fail + 1))
    fi
}

log() { echo "$out/$1/mcu$2.log"; }

in_order() { # file text...: each text appears, each after the previous one (fixed strings)
    local f="$1" line=0 n
    shift
    for p in "$@"; do
        n=$(awk -v start="$line" -v p="$p" 'NR > start && index($0, p) { print NR; exit }' "$f")
        [ -n "$n" ] || return 1
        line="$n"
    done
}

count_is() { # file text n: the text occurs on exactly n lines
    [ "$(grep -cF -- "$2" "$1")" -eq "$3" ]
}

last_report_has() { # file text...: the node's last diag and db report lines contain every text
    local f="$1" r
    shift
    r="$(grep -E ' diag self=| db synced=' "$f" | tail -2 | tr '\n' ' ')"
    for p in "$@"; do
        [[ "$r" == *"$p"* ]] || return 1
    done
}

peer_counts() { # file node r rec: the last report shows these restart and recovery counts
    grep ' diag self=' "$1" | tail -1 | grep -qE "n$2=[A-Z_]+/[A-Z-]+ age=[0-9]+ miss=[0-9]+ r=$3 rec=$4( |$)"
}

report_at() { # file t text: the report printed at t ms (or up to 4 ms later) contains the text
    awk -v a="$2" -v p="$3" '/ t=[0-9]+ tx / { t = substr($2, 3) + 0; inside = (t >= a && t <= a + 4); next } inside && index($0, p) { found = 1 } END { exit !found }' "$1"
}

clean_logs() { # name: no frame rejected, no debug text dropped, on any node
    ! grep -qF 'rx rejected' "$out/$1"/mcu[1-4].log && ! grep -h 'log dropped=' "$out/$1"/mcu[1-4].log | grep -qv 'dropped=0$'
}

end_state() { # file node r rec: last report OPERATIONAL and synced 20, and node shows r restarts, rec recoveries
    last_report_has "$1" "self=OPERATIONAL" "synced=20" && peer_counts "$1" "$2" "$3" "$4"
}

no_fault_lines() { # file: no peer ever LATE, OFFLINE, RECOVERING, NO_HEARTBEAT or DATA_INVALID
    ! grep -qE " diag n[1-4] (LATE|OFFLINE|RECOVERING|NO_HEARTBEAT|DATA_INVALID)" "$1"
}

frame_counts_match() { # name: every receiver accepted every frame each sender queued
    awk -f "$repo/renode/stage4_crosscheck.awk" "$out/$1"/mcu[1-4].log | grep -q " 0 mismatch"
}

crosscheck_outside() { # name from to: parameter copies match their owner at every report outside [from, to] ms
    awk -f "$repo/renode/stage5_params_crosscheck.awk" "$out/$1"/mcu[1-4].log |
        awk -v a="$2" -v b="$3" '/^(MISMATCH|NOT SYNCED)/ { match($0, /t=[0-9]+/); t = substr($0, RSTART + 2, RLENGTH - 2) + 0; if (t < a || t > b) bad++ } END { exit bad > 0 }'
}

s1() {
    run s1 s1_normal.resc
    for n in 1 2 3 4; do
        check "MCU$n: self OPERATIONAL, synced 20, nothing never-received" last_report_has "$(log s1 $n)" "self=OPERATIONAL" "synced=20" "never=0"
        check "MCU$n: no LATE, OFFLINE, RECOVERING, NO_HEARTBEAT or DATA_INVALID" no_fault_lines "$(log s1 $n)"
    done
    check "every parameter copy equals its owner's value at every report" crosscheck_outside s1 0 0
    check "frame counts: no frame lost after boot (boot losses are listed by the check)" frame_counts_match s1
    check "no frame rejected, no debug text dropped" clean_logs s1
}

s2() {
    run s2 s2_slave_failure.resc
    for n in 1 2 4; do
        local f
        f="$(log s2 $n)"
        check "MCU$n: n3 LATE, OFFLINE, flag, RESTART, RECOVERING, flag cleared, ONLINE recovered" \
            in_order "$f" "diag n3 LATE" "diag n3 OFFLINE" "db OWNER_OFFLINE 11 12 13 14 15" "diag n3 RESTART" "diag n3 RECOVERING" "db OWNER_ONLINE 11 12 13 14 15" "diag n3 ONLINE recovered"
        check "MCU$n: stays OPERATIONAL and keeps exchanging (P11-P15 stale only) during the fault" report_at "$f" 7005 "db synced=15"
        check "MCU$n: own state never left OPERATIONAL" count_is "$f" "diag self RESYNCING" 1
        check "MCU$n: end synced 20, n3 restarted once, recovered once" end_state "$f" 3 1 1
    done
    check "MCU3 after its reset: OPERATIONAL, synced 20" last_report_has "$(log s2 3)" "self=OPERATIONAL" "synced=20"
}

s3() {
    run s3 s3_disconnect.resc
    for n in 1 2 4; do
        local f
        f="$(log s3 $n)"
        check "MCU$n: n3 LATE, OFFLINE, flag, RECOVERING, flag cleared, ONLINE recovered" \
            in_order "$f" "diag n3 LATE" "diag n3 OFFLINE" "db OWNER_OFFLINE 11 12 13 14 15" "diag n3 RECOVERING" "db OWNER_ONLINE 11 12 13 14 15" "diag n3 ONLINE recovered"
        check "MCU$n: no restart counted for n3 (it never reset)" count_is "$f" "diag n3 RESTART" 0
        check "MCU$n: end synced 20, n3 recovered once" end_state "$f" 3 0 1
    done
    local f3
    f3="$(log s3 3)"
    check "MCU3 alone: all peers OFFLINE, self RESYNCING, then OPERATIONAL after reconnect" \
        in_order "$f3" "diag n1 OFFLINE" "diag self RESYNCING" "diag n1 RECOVERING" "diag self OPERATIONAL" "diag n1 ONLINE recovered"
    check "MCU3: end OPERATIONAL, synced 20" last_report_has "$f3" "self=OPERATIONAL" "synced=20"
    check "parameter copies equal their owner at every report outside the outage (5-15 s)" crosscheck_outside s3 5000 15005
}

s4() {
    run s4 s4_master_failure.resc
    for n in 2 3 4; do
        local f
        f="$(log s4 $n)"
        check "MCU$n: n1 LATE, OFFLINE, flag, RESTART, RECOVERING, flag cleared, ONLINE recovered" \
            in_order "$f" "diag n1 LATE" "diag n1 OFFLINE" "db OWNER_OFFLINE 1 2 3 4 5" "diag n1 RESTART" "diag n1 RECOVERING" "db OWNER_ONLINE 1 2 3 4 5" "diag n1 ONLINE recovered"
        check "MCU$n: keeps exchanging without the master (P1-P5 stale only)" report_at "$f" 7005 "db synced=15"
        check "MCU$n: own state never left OPERATIONAL (no failover needed)" count_is "$f" "diag self RESYNCING" 1
        check "MCU$n: end synced 20, n1 restarted once, recovered once" end_state "$f" 1 1 1
    done
    check "MCU1 after its reset: OPERATIONAL, synced 20" last_report_has "$(log s4 1)" "self=OPERATIONAL" "synced=20"
}

mkdir -p "$repo/logs"
todo=("$@")
[ ${#todo[@]} -gt 0 ] || todo=(s1 s2 s3 s4)
for s in "${todo[@]}"; do
    case "$s" in
    s1 | s2 | s3 | s4) "$s" ;;
    *) echo "unknown scenario $s" >&2; exit 2 ;;
    esac
done
echo "## $pass passed, $fail failed"
[ "$fail" -eq 0 ]
