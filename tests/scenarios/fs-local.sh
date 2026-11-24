#!/usr/bin/env bash
#
# KIVFS – scénář "operace souborového systému na jednom uzlu"
# ===========================================================
#
# Použití:  tests/scenarios/fs-local.sh <adresář s binárkami> [conf/local/node1.ini]
#
# Spustí 5 modulů uzlu node1 a přes kivfs-client (klient -> Auth -> Sync
# -> VFS -> DB) ověří:
#   1. mkdir / ls / stat / rmdir a jejich chyby (EXISTS, NOT_FOUND,
#      ACCESS_DENIED, NOT_EMPTY, MALFORMED)
#   2. zastavená DB -> klient dostane UNAVAILABLE (nečeká se na timeout)
#   3. DB znovu spuštěná -> VFS se připojí samo; mock DB je prázdná
#   4. řádné ukončení: exit 0, v logu žádný nález sanitizeru ani "BUG"
# Každý příkaz běží v novém spojení klienta.
set -u

BIN=${1:?použití: $0 <adresář s binárkami> [node1.ini]}
CFG=${2:-$(cd "$(dirname "$0")/../.." && pwd)/conf/local/node1.ini}
PORT=$(awk '/^\[auth\]/{a=1;next} /^\[/{a=0} a && $1=="port"{print $3}' "$CFG")
LOGS=$(mktemp -d /tmp/kivfs-fs.XXXXXX)
declare -A PID
FAILED=0
ERRWORDS='EXISTS|NOT_FOUND|NOT_DIR|NOT_EMPTY|ACCESS_DENIED|UNAVAILABLE|MALFORMED|INTERNAL|BUSY|UNKNOWN'

fail() { printf '  !! %s\n' "$*"; FAILED=1; }

start() { "$BIN/kivfs-$1" -c "$CFG" >>"$LOGS/$1.log" 2>&1 & PID[$1]=$!; }
stop() {
    local p=${PID[$1]:-}; [ -n "$p" ] || return 0
    kill -TERM "$p" 2>/dev/null; wait "$p"; local rc=$?
    unset "PID[$1]"
    [ $rc -eq 0 ] || fail "kivfs-$1 skončil s kódem $rc"
}
cleanup() { for m in auth sync vfs db fs; do stop "$m"; done; }
trap cleanup EXIT

# Jeden příkaz klienta v novém spojení; výstup (stdout + stderr) do $OUT.
run() { OUT=$(printf '%s\nquit\n' "$1" | "$BIN/kivfs-client" 127.0.0.1 "$PORT" 2>&1); }

# expect "<příkaz>" "<očekávaný podřetězec>"   (prázdný = bez chyby)
expect() {
    run "$1"
    if [ -z "$2" ]; then
        if grep -qE "$ERRWORDS" <<<"$OUT"; then fail "$1: čekal jsem úspěch, přišlo: $OUT"; return; fi
    elif ! grep -qF -- "$2" <<<"$OUT"; then
        fail "$1: čekal jsem '$2', přišlo: $OUT"; return
    fi
    printf '  OK  %-22s %s\n' "$1" "${2:-(úspěch)}"
}

# čeká, až všech 5 modulů hlásí "připraven" (STATUS přes Auth)
wait_ready() {
    for _ in $(seq 1 60); do
        run status
        [ "$(grep -c 'připraven$' <<<"$OUT")" -eq 5 ] && return 0
        sleep 0.25
    done
    fail "moduly nejsou připravené: $OUT"; return 1
}

echo "### start uzlu ($CFG, Auth port $PORT)"
for m in auth sync vfs db fs; do start "$m"; done
wait_ready || exit 1

echo "### operace a chyby"
expect "mkdir /a"            ""
expect "mkdir /a"            "EXISTS"
expect "mkdir /a/b 700"      ""
expect "mkdir /c 555"        ""
expect "mkdir /c/x"          "ACCESS_DENIED"
expect "mkdir /x/y"          "NOT_FOUND"
expect "mkdir /"             "EXISTS"
expect "mkdir relativni"     "MALFORMED"
expect "ls /"                "položek: 2"
expect "ls /"                "drwxr-xr-x"
expect "ls /a"               "b/"
expect "stat /a/b"           "rwx------ (0700)"
expect "stat /a/b"           "uid 1000"
expect "stat /a/.."          "MALFORMED"
expect "ls /c/x"             "NOT_FOUND"
expect "rmdir /a"            "NOT_EMPTY"
expect "rmdir /a/b"          ""
expect "rmdir /a"            ""
expect "rmdir /"             "ACCESS_DENIED"
expect "stat /a"             "NOT_FOUND"
expect "ls /"                "položek: 1"

echo "### výpadek DB"
stop db
sleep 0.3
expect "stat /"              "UNAVAILABLE"
expect "mkdir /z"            "UNAVAILABLE"
expect "ls /"                "UNAVAILABLE"

echo "### obnova DB (mock je po restartu prázdný)"
start db
wait_ready
expect "stat /"              "typ:    adresář"
expect "ls /"                "položek: 0"
expect "mkdir /znovu"        ""

echo "### ukončení"
trap - EXIT
cleanup
if grep -lE "AddressSanitizer|runtime error|ThreadSanitizer|LeakSanitizer|BUG" "$LOGS"/*.log; then
    fail "nález sanitizeru nebo BUG v logu (soubory výše)"
fi

echo
echo "logy: $LOGS"
if [ $FAILED -eq 0 ]; then echo "SCÉNÁŘ OK"; else echo "SCÉNÁŘ SELHAL"; fi
exit $FAILED
