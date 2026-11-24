#!/usr/bin/env bash
#
# Spustí všech 5 modulů jednoho uzlu v jednom kontejneru.
# Použití: start-node.sh <soubor.ini>
#
# Když kterýkoli modul skončí, ukončí i ostatní a kontejner skončí
# s chybou (ať výpadek není schovaný v běžícím kontejneru).
# SIGTERM (docker stop) se přepošle všem modulům.
set -u
CFG=${1:?použití: $0 <soubor.ini>}
BIN=/kivfs/build

for m in auth sync vfs db fs; do
    "$BIN/kivfs-$m" -c "$CFG" &
done

trap 'kill -TERM $(jobs -p) 2>/dev/null; wait; exit 0' TERM INT
wait -n
rc=$?
echo "start-node: některý modul skončil (kód $rc), ukončuji ostatní" >&2
kill -TERM $(jobs -p) 2>/dev/null
wait
exit 1
