#!/usr/bin/env bash
#
# KIVFS – scénář "kolo hlasování Syncu, 3 uzly na jednom stroji" (conf/local)
# ===========================================================================
#
# Použití:  tests/scenarios/sync-local.sh <adresář s binárkami> [adresář conf/local]
#
# Konfigurace se zkopíruje s [cluster] op_timeout_ms = 2000 (kolo PROPOSE
# pak vyprší za 2/16 = 250 ms, viz kivfs_modconf_timeouts). Ověří:
#   1. každá klientská operace projde kolem Tcli přes všechny 3 uzly
#   2. SOUBĚH: 3 klienti (na každém uzlu jeden) najednou po 40 mkdir –
#      vše projde, bez BUSY/UNAVAILABLE
#   3. VÝPADEK sync@node3: operace na node1 hned UNAVAILABLE (linka dole,
#      nečeká se na timeout), čtení také (order_cli = yes)
#   4. OBNOVA sync@node3: operace zase projdou, ostatní Synce poznají
#      restart a spustí úklid
#   5. ZAMRZLÝ sync@node3 (SIGSTOP): kolo vyprší -> UNAVAILABLE; po SIGCONT
#      outbox doručí ABORT a operace zase projdou
#   6. řádné ukončení, v logu žádný nález sanitizeru ani "BUG"
# Pozn.: metadata se replikují (pruh DB, 26. 11.) – zápisy jdou kolem Tdb
# na všechny uzly; po restartu sync@node3 (krok 4) se Sync nejdřív ověří
# u ostatních (SYNC_CLOCK). Replikaci samotnou ověřuje repl-local (úkol 5).
set -u

BIN=${1:?použití: $0 <adresář s binárkami> [conf/local]}
SRC=${2:-$(cd "$(dirname "$0")/../.." && pwd)/conf/local}
LOGS=$(mktemp -d /tmp/kivfs-sync.XXXXXX)
CONF=$LOGS/conf
MODULES="auth sync vfs db fs"
NODES="1 2 3"
declare -A PID
FAILED=0
ERRWORDS='EXISTS|NOT_FOUND|NOT_DIR|NOT_EMPTY|ACCESS_DENIED|UNAVAILABLE|MALFORMED|INTERNAL|BUSY|TIMEOUT|UNKNOWN|chyba'

mkdir -p "$CONF"
for n in $NODES; do
    sed 's/^\[cluster\]$/[cluster]\nop_timeout_ms = 2000/' "$SRC/node$n.ini" >"$CONF/node$n.ini"
done
CFGS="-c $CONF/node1.ini -c $CONF/node2.ini -c $CONF/node3.ini"

step() { printf '\n### %s\n' "$*"; }
fail() { printf '  !! %s\n' "$*"; FAILED=1; }
port() { awk '/^\[auth\]/{a=1;next} /^\[/{a=0} a && $1=="port"{print $3}' "$CONF/node$1.ini"; }

start() { "$BIN/kivfs-$2" -c "$CONF/node$1.ini" >>"$LOGS/node$1-$2.log" 2>&1 & PID[$1-$2]=$!; }
stop() {
    local p=${PID[$1-$2]:-}; [ -n "$p" ] || return 0
    kill -CONT "$p" 2>/dev/null; kill -TERM "$p" 2>/dev/null; wait "$p"; local rc=$?
    unset "PID[$1-$2]"
    [ $rc -eq 0 ] || fail "kivfs-$2 node$1 skončil s kódem $rc"
}
cleanup() { for n in $NODES; do for m in $MODULES; do stop "$n" "$m"; done; done; }
trap cleanup EXIT
trap 'exit 130' INT TERM         # i při Ctrl-C / SIGTERM proběhne cleanup (vč. SIGCONT)

# Moduly, které mezitím skončily: konec jejich logu (důvod bývá na posledním řádku).
dead_logs() {
    for n in $NODES; do for m in $MODULES; do
        local p=${PID[$n-$m]:-}
        if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then
            printf '  kivfs-%s node%s neběží, konec logu:\n' "$m" "$n"
            tail -3 "$LOGS/node$n-$m.log" | sed 's/^/      /'
        fi
    done; done
}

# client <uzel> "<příkazy oddělené \n>" -> výstup v $OUT
client() { OUT=$(printf '%b\nquit\n' "$2" | "$BIN/kivfs-client" -t 5000 127.0.0.1 "$(port "$1")" 2>&1); }

# expect <uzel> "<příkaz>" "<očekávaný podřetězec>"   (prázdný = bez chyby)
expect() {
    client "$1" "$2"
    if [ -z "$3" ]; then
        if grep -qE "$ERRWORDS" <<<"$OUT"; then fail "node$1 $2: čekal jsem úspěch, přišlo: $OUT"; return; fi
    elif ! grep -qF -- "$3" <<<"$OUT"; then
        fail "node$1 $2: čekal jsem '$3', přišlo: $OUT"; return
    fi
    printf '  OK  node%s %-18s %s\n' "$1" "$2" "${3:-(úspěch)}"
}

ms() { date +%s%3N; }

# Zbytky předchozího běhu (např. přerušeného mezi SIGSTOP a SIGCONT v kroku 5)
# drží porty 310xx–312xx: nový modul pak skončí "Address already in use".
# (podle názvu procesu, ne -f: celé příkazové řádky by chytily i shell se skriptem)
if OLD=$(pgrep -a '^kivfs-(auth|sync|vfs|db|fs)$' 2>/dev/null) && [ -n "$OLD" ]; then
    echo "SCÉNÁŘ NELZE SPUSTIT: běží (nebo jsou zastavené) staré procesy KIVFS:"
    sed 's/^/    /' <<<"$OLD"
    echo "  ukliď je:  pkill -CONT '^kivfs-'; pkill -TERM '^kivfs-'"
    trap - EXIT
    exit 1
fi

step "start 15 procesů (op_timeout_ms = 2000)"
for n in $NODES; do for m in $MODULES; do start "$n" "$m"; done; done
"$BIN/kivfs-check" $CFGS --wait 20 >"$LOGS/check-start.txt" 2>&1 \
    || { cat "$LOGS/check-start.txt"; dead_logs; fail "cluster se nepropojil"; }

step "1. kolo přes všechny uzly"
for n in $NODES; do
    expect "$n" "mkdir /n$n" ""
    expect "$n" "stat /n$n" "adresář"
done

step "2. souběh: 3 klienti × 40 mkdir najednou"
for n in $NODES; do
    cmds=""
    for i in $(seq 1 40); do cmds="$cmds""mkdir /n$n/d$i\n"; done
    printf '%b\nquit\n' "$cmds" | "$BIN/kivfs-client" -t 5000 127.0.0.1 "$(port "$n")" \
        >"$LOGS/burst-$n.txt" 2>&1 &
done
wait_bursts=0
for job in $(jobs -p); do
    # čekat jen na klienty (moduly běží dál)
    case " ${PID[*]} " in *" $job "*) ;; *) wait "$job" || wait_bursts=1 ;; esac
done
for n in $NODES; do
    errs=$(grep -cE "$ERRWORDS" "$LOGS/burst-$n.txt")
    [ "$errs" -eq 0 ] || fail "node$n: $errs chyb v souběhu (viz $LOGS/burst-$n.txt)"
    expect "$n" "ls /n$n" "položek: 40"
done

step "3. výpadek sync@node3"
stop 3 sync
sleep 0.3
t0=$(ms); expect 1 "mkdir /po-vypadku" "UNAVAILABLE"; dt=$(( $(ms) - t0 ))
[ $dt -lt 1500 ] || fail "UNAVAILABLE trvalo $dt ms – čekalo se na timeout místo linky dole"
expect 1 "stat /n1" "UNAVAILABLE"

step "4. obnova sync@node3"
start 3 sync
"$BIN/kivfs-check" $CFGS --wait 20 >"$LOGS/check-recovery.txt" 2>&1 \
    || { cat "$LOGS/check-recovery.txt"; dead_logs; fail "po obnově se cluster nepropojil"; }
expect 1 "mkdir /po-obnove" ""
expect 3 "mkdir /n3-znovu" ""
for n in 1 2; do
    grep -q "node3 restartoval" "$LOGS/node$n-sync.log" || fail "sync@node$n nespustil úklid po restartu node3"
done

step "5. zamrzlý sync@node3 (SIGSTOP)"
kill -STOP "${PID[3-sync]}"
t0=$(ms); expect 1 "mkdir /zamrzly" "UNAVAILABLE"; dt=$(( $(ms) - t0 ))
[ $dt -ge 200 ] || fail "UNAVAILABLE za $dt ms – kolo mělo čekat na timeout (250 ms)"
kill -CONT "${PID[3-sync]}"
ok=0
for _ in $(seq 1 20); do
    client 1 "mkdir /rozmrzly"
    if ! grep -qE "$ERRWORDS" <<<"$OUT"; then ok=1; break; fi
    sleep 0.25
done
[ $ok -eq 1 ] && echo "  OK  node1 mkdir /rozmrzly     (úspěch po SIGCONT)" \
              || fail "po SIGCONT operace neprojdou: $OUT"
expect 1 "ls /" "rozmrzly"

step "ukončení"
trap - EXIT
cleanup
if grep -lE "AddressSanitizer|runtime error|ThreadSanitizer|LeakSanitizer" "$LOGS"/*.log; then
    fail "nález sanitizeru (viz logy výše)"
fi
if grep -l "BUG" "$LOGS"/*.log; then fail "BUG v logu (viz výše)"; fi

echo
echo "logy: $LOGS"
if [ $FAILED -eq 0 ]; then echo "SCÉNÁŘ OK"; else echo "SCÉNÁŘ SELHAL"; fi
exit $FAILED
