#!/usr/bin/env bash
#
# KIVFS – scénář "3 uzly na jednom stroji" (conf/local)
# =====================================================
#
# Použití:  scripts/cluster-local.sh <adresář s binárkami> [adresář conf/local]
#
# 1. spustí 15 procesů (3 uzly × auth, sync, vfs, db, fs)
# 2. kivfs-check --wait: vše se musí propojit (15 modulů, 18 spojení)
# 3. VÝPADEK: zastaví Sync uzlu node2 -> kivfs-check musí hlásit chybu
# 4. OBNOVA: spustí ho znovu -> vše se musí znovu propojit a ostatní
#    Synce musí v logu poznat restart (nová inkarnace)
# 5. řádně ukončí všechny procesy (SIGTERM) – každý musí skončit s 0
#    a v logu nesmí být nález sanitizeru
#
# Exit 0 = scénář prošel. Logy procesů zůstanou v adresáři vypsaném na konci.
set -u

BIN=${1:?použití: $0 <adresář s binárkami> [conf/local]}
CONF=${2:-$(cd "$(dirname "$0")/.." && pwd)/conf/local}
LOGS=$(mktemp -d /tmp/kivfs-cluster.XXXXXX)
MODULES="auth sync vfs db fs"
NODES="1 2 3"
CFGS="-c $CONF/node1.ini -c $CONF/node2.ini -c $CONF/node3.ini"
declare -A PID
FAILED=0

step() { printf '\n### %s\n' "$*"; }
fail() { printf 'SCÉNÁŘ SELHAL: %s\n' "$*"; FAILED=1; }

start() {   # start <node> <modul>
    "$BIN/kivfs-$2" -c "$CONF/node$1.ini" >>"$LOGS/node$1-$2.log" 2>&1 &
    PID[$1-$2]=$!
}

stop() {    # stop <node> <modul> – SIGTERM a kontrola exit kódu
    local p=${PID[$1-$2]:-}
    [ -n "$p" ] || return 0
    kill -TERM "$p" 2>/dev/null
    wait "$p"
    local rc=$?
    unset "PID[$1-$2]"
    [ $rc -eq 0 ] || fail "kivfs-$2 node$1 skončil s kódem $rc"
}

cleanup() {
    for n in $NODES; do for m in $MODULES; do stop "$n" "$m"; done; done
}
trap cleanup EXIT

step "start 15 procesů ($CONF)"
for n in $NODES; do for m in $MODULES; do start "$n" "$m"; done; done

step "propojení"
"$BIN/kivfs-check" $CFGS --wait 20 || fail "cluster se nepropojil"

step "výpadek: zastavuji sync@node2"
stop 2 sync
sleep 0.5
if "$BIN/kivfs-check" $CFGS >"$LOGS/check-outage.txt" 2>&1; then
    fail "kivfs-check výpadek nepoznal"
else
    grep -E "VÝSLEDEK|sync@node2" "$LOGS/check-outage.txt" | head -8
fi

step "obnova: spouštím sync@node2"
start 2 sync
"$BIN/kivfs-check" $CFGS --wait 20 >"$LOGS/check-recovery.txt" 2>&1 \
    || { cat "$LOGS/check-recovery.txt"; fail "po obnově se cluster nepropojil"; }
grep "VÝSLEDEK" "$LOGS/check-recovery.txt"
for n in 1 3; do
    grep -q "sync@node2 nahoře – protistrana byla RESTARTOVÁNA" "$LOGS/node$n-sync.log" \
        || fail "sync@node$n nepoznal restart sync@node2"
done
grep -q "vfs@node2 nahoře" "$LOGS/node2-sync.log" || fail "nový sync@node2 se nepřipojil k vfs"

step "ukončení"
trap - EXIT
cleanup
if grep -lE "AddressSanitizer|runtime error|ThreadSanitizer|LeakSanitizer" "$LOGS"/*.log; then
    fail "nález sanitizeru (viz logy výše)"
fi

echo
echo "logy: $LOGS"
if [ $FAILED -eq 0 ]; then echo "SCÉNÁŘ OK"; else echo "SCÉNÁŘ SELHAL"; fi
exit $FAILED
