#!/usr/bin/env bash
#
# KIVFS – scénář "replikace metadat, 3 uzly na jednom stroji" (conf/local)
# ========================================================================
#
# Použití:  tests/scenarios/repl-local.sh <adresář s binárkami> [adresář conf/local]
#
# Konfigurace se zkopíruje s [cluster] op_timeout_ms = 2000 (kolo 250 ms,
# DB_APPLY 125 ms – kivfs_modconf_timeouts). Ověří pruh DB (SUBMIT -> kolo
# Tdb -> DB_APPLY na všech uzlech):
#   1. mkdir na node1 -> stat na node2, node3 stejný (id, práva, časy);
#      rmdir z node3 -> všude pryč, mtime rodiče všude stejný
#   2. SOUBĚH STEJNÝCH JMEN: 10× "mkdir /sN" z node1 i node2 najednou ->
#      u každého N přesně jeden EXISTS, jinak úspěch
#   3. SOUBĚH 3 klientů × 30 mkdir -> bez chyb, strom na všech uzlech stejný
#   4. ZAMRZLÁ DB node2 (SIGSTOP): zápisy jinde projdou; Sync node2 DB_APPLY
#      zopakuje, po SIGCONT DB opakování pozná -> strom všude stejný
#   5. RESTART sync@node3 v klidu: nový Sync se ověří u ostatních
#      (SYNC_CLOCK) a pokračuje; zápisy z node3 i jinde projdou
#   6. RESTART db@node2 (mock = prázdná DB): Sync node2 se zastaví (STOPPED,
#      chybí operace), node2 zápisy UNAVAILABLE, ostatní zapisují dál
#   7. řádné ukončení, v logu žádný nález sanitizeru ani "BUG"
# Strom se porovnává otiskem: ls / + stat a ls každého adresáře v kořeni
# (ls ukazuje id, stat id, práva a časy). Neověřuje: úklid po pádu
# původce uprostřed kola pruhu DB (časování; jen jednotky v test_lane).
set -u

BIN=${1:?použití: $0 <adresář s binárkami> [conf/local]}
SRC=${2:-$(cd "$(dirname "$0")/../.." && pwd)/conf/local}
LOGS=$(mktemp -d /tmp/kivfs-repl.XXXXXX)
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
ok()   { printf '  OK  %s\n' "$*"; }
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
trap 'exit 130' INT TERM

dead_logs() {
    for n in $NODES; do for m in $MODULES; do
        local p=${PID[$n-$m]:-}
        if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then
            printf '  kivfs-%s node%s neběží, konec logu:\n' "$m" "$n"
            tail -3 "$LOGS/node$n-$m.log" | sed 's/^/      /'
        fi
    done; done
}

# client <uzel> "<příkazy oddělené \n>" -> výstup v $OUT (bez řádku "připojeno",
# který obsahuje node_id – liší se mezi uzly)
client() {
    OUT=$(printf '%b\nquit\n' "$2" | "$BIN/kivfs-client" -t 5000 127.0.0.1 "$(port "$1")" 2>&1 \
          | grep -v 'připojeno k')
}

# expect <uzel> "<příkaz>" "<očekávaný podřetězec>"   (prázdný = bez chyby)
expect() {
    client "$1" "$2"
    if [ -z "$3" ]; then
        if grep -qE "$ERRWORDS" <<<"$OUT"; then fail "node$1 $2: čekal jsem úspěch, přišlo: $OUT"; return; fi
    elif ! grep -qF -- "$3" <<<"$OUT"; then
        fail "node$1 $2: čekal jsem '$3', přišlo: $OUT"; return
    fi
    ok "node$1 $(printf '%-22s' "${2//\\n/; }") ${3:-(úspěch)}"
}

# eventually <uzel> "<příkaz>": opakovat až 5 s, dokud neprojde bez chyby
# (po restartu se Sync nejdřív ověřuje – mezitím zápisy UNAVAILABLE)
eventually() {
    for _ in $(seq 1 20); do
        client "$1" "$2"
        if ! grep -qE "$ERRWORDS" <<<"$OUT"; then ok "node$1 $(printf '%-22s' "${2//\\n/; }") (úspěch)"; return; fi
        sleep 0.25
    done
    fail "node$1 $2: ani po 5 s neprošlo: $OUT"
}

# Otisk stromu uzlu: ls / a pro každý adresář v kořeni (seznam z node1)
# stat + ls. Výstup otisku do $FP.
tree_cmds() {
    client 1 "ls /"
    local cmds="ls /\n"
    for d in $(awk '$NF ~ /\/$/ {sub(/\/$/, "", $NF); print $NF}' <<<"$OUT"); do
        cmds="${cmds}stat /$d\nls /$d\n"
    done
    TREE_CMDS=$cmds
}
fp() { client "$1" "$TREE_CMDS"; FP=$(md5sum <<<"$OUT" | cut -c1-12); FP_RAW=$OUT; }

# same_tree "<popis>": otisky všech uzlů se musí shodovat
same_tree() {
    tree_cmds
    fp 1; local f1=$FP r1=$FP_RAW
    fp 2; local f2=$FP r2=$FP_RAW
    fp 3; local f3=$FP
    if [ "$f1" = "$f2" ] && [ "$f1" = "$f3" ] && ! grep -qE 'UNAVAILABLE|TIMEOUT|INTERNAL' <<<"$r1"; then
        ok "strom stejný na node1/2/3 ($1, otisk $f1)"
    else
        fail "strom se liší ($1): node1 $f1, node2 $f2, node3 $f3"
        diff <(echo "$r1") <(echo "$r2") | head -10 | sed 's/^/      /'
    fi
}

# čekat jen na zadané PID klientů (moduly běží dál)
wait_pids() { local rc=0; for p in "$@"; do wait "$p" || rc=1; done; return $rc; }

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
for n in $NODES; do eventually "$n" "stat /"; done      # pruh DB ověřen na všech

step "1. replikace mkdir / rmdir"
expect 1 "mkdir /a\nmkdir /a/b 700" ""
client 1 "stat /a/b"; S1=$OUT
for n in 2 3; do
    client "$n" "stat /a/b"
    if [ "$OUT" = "$S1" ] && grep -q "id:" <<<"$OUT"; then ok "node$n stat /a/b = node1 (id, práva, časy)"
    else fail "node$n stat /a/b se liší od node1:"; diff <(echo "$S1") <(echo "$OUT") | sed 's/^/      /'; fi
done
expect 3 "rmdir /a/b" ""
for n in $NODES; do expect "$n" "ls /a" "položek: 0"; done
same_tree "po rmdir"

step "2. souběh stejných jmen: 10× mkdir /sN z node1 i node2"
P=()
for i in $(seq 1 10); do
    printf 'mkdir /s%s\nquit\n' "$i" | "$BIN/kivfs-client" -t 5000 127.0.0.1 "$(port 1)" >"$LOGS/same1-$i.txt" 2>&1 & P+=($!)
    printf 'mkdir /s%s\nquit\n' "$i" | "$BIN/kivfs-client" -t 5000 127.0.0.1 "$(port 2)" >"$LOGS/same2-$i.txt" 2>&1 & P+=($!)
done
wait_pids "${P[@]}"
bad=0
for i in $(seq 1 10); do
    a=$(grep -c EXISTS "$LOGS/same1-$i.txt"); b=$(grep -c EXISTS "$LOGS/same2-$i.txt")
    other=$(cat "$LOGS/same1-$i.txt" "$LOGS/same2-$i.txt" | grep -v EXISTS | grep -cE "$ERRWORDS")
    if [ $((a + b)) -ne 1 ] || [ "$other" -ne 0 ]; then
        bad=1; fail "/s$i: node1 '$(grep -E "$ERRWORDS" "$LOGS/same1-$i.txt")' node2 '$(grep -E "$ERRWORDS" "$LOGS/same2-$i.txt")'"
    fi
done
[ $bad -eq 0 ] && ok "u všech 10 jmen přesně jeden OK a jeden EXISTS"
same_tree "po souběhu stejných jmen"

step "3. souběh: 3 klienti × 30 mkdir"
expect 1 "mkdir /p1\nmkdir /p2\nmkdir /p3" ""
P=()
for n in $NODES; do
    cmds=""
    for i in $(seq 1 30); do cmds="$cmds""mkdir /p$n/d$i\n"; done
    printf '%b\nquit\n' "$cmds" | "$BIN/kivfs-client" -t 5000 127.0.0.1 "$(port "$n")" \
        >"$LOGS/burst-$n.txt" 2>&1 & P+=($!)
done
wait_pids "${P[@]}"
for n in $NODES; do
    errs=$(grep -cE "$ERRWORDS" "$LOGS/burst-$n.txt")
    [ "$errs" -eq 0 ] || fail "node$n: $errs chyb v souběhu (viz $LOGS/burst-$n.txt)"
done
for n in $NODES; do expect "$n" "ls /p$n" "položek: 30"; done
same_tree "po souběhu 3 klientů"

step "4. zamrzlá db@node2 (SIGSTOP)"
kill -STOP "${PID[2-db]}"
expect 1 "mkdir /f1" ""
expect 3 "mkdir /f3" ""
expect 3 "stat /f1" "adresář"
sleep 0.5
kill -CONT "${PID[2-db]}"
sleep 1
grep -q "DB_APPLY .* zopakuji" "$LOGS/node2-sync.log" \
    && ok "sync@node2 DB_APPLY zopakoval" || fail "sync@node2 DB_APPLY nezopakoval (timeout APPLY 125 ms)"
grep -q "opakování indexu" "$LOGS/node2-db.log" \
    && ok "db@node2 opakování poznala (uložený výsledek)" \
    || echo "  --  db@node2 opakování nehlásí (původní APPLY se po reconnectu nedoručil – i to je v pořádku)"
expect 2 "stat /f1" "adresář"
same_tree "po rozmrznutí DB"

step "5. restart sync@node3 v klidu"
n_ok=$(grep -c "pruh DB ověřen" "$LOGS/node3-sync.log")
stop 3 sync
start 3 sync
"$BIN/kivfs-check" $CFGS --wait 20 >"$LOGS/check-restart.txt" 2>&1 \
    || { cat "$LOGS/check-restart.txt"; dead_logs; fail "po restartu se cluster nepropojil"; }
eventually 3 "mkdir /po-restartu3"
eventually 1 "mkdir /po-restartu1"
[ "$(grep -c "pruh DB ověřen" "$LOGS/node3-sync.log")" -gt "$n_ok" ] \
    && ok "sync@node3 se po restartu ověřil u ostatních" || fail "sync@node3 se po restartu neověřil"
grep -q "ZASTAVEN" "$LOGS/node3-sync.log" && fail "sync@node3 se po klidném restartu zastavil"
same_tree "po restartu Syncu"

step "6. restart db@node2 (mock -> prázdná DB)"
stop 2 db
start 2 db
for _ in $(seq 1 20); do grep -q "ZASTAVEN" "$LOGS/node2-sync.log" && break; sleep 0.25; done
grep -q "pruh DB ZASTAVEN.*DB má index 0" "$LOGS/node2-sync.log" \
    && ok "sync@node2 zastaven: DB ztratila data" || fail "sync@node2 se nezastavil"
expect 2 "mkdir /z2" "UNAVAILABLE"
expect 1 "mkdir /z1" ""
expect 3 "stat /z1" "adresář"

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
