# KIVFS – jak to sestavit, spustit a otestovat

Server (reactor/epoll) běží jen na Linuxu, takže se všechno pouští v Dockeru.
Na macOS se dá přeložit jen klient a přenositelné testy (viz konec).

Jsou dva způsoby:

| | Kdy | Kód v kontejneru |
|---|---|---|
| **A. Vývojový kontejner** (kořenový `Dockerfile`) | běžná práce: překlad, testy, ruční zkoušení | adresář projektu je **připojený** – změna na Macu je hned vidět |
| **B. Docker compose** (`docker/`) | 3 uzly jako 3 „stroje“ | zdrojáky se **kopírují do obrazu** – po změně kódu `--build` |

Všechny příkazy se spouští z kořene projektu (`kivfs/`).

> **zsh na macOS:** v interaktivním terminálu zsh **nebere `#` jako komentář**
> (`kc ps  # co běží` pošle `#` jako argument → `no such service: #`).
> Bloky pro Mac proto komentáře nemají. Chceš-li je povolit trvale:
> `echo 'setopt interactivecomments' >> ~/.zshrc` a otevřít nový terminál.
> Uvnitř kontejneru běží bash, tam komentáře fungují.

---

## A. Vývojový kontejner

### A1. Jednou: obraz

```sh
docker build -t kivfs-dev .
```

### A2. Spustit kontejner

```sh
docker run --rm -it --name kivfs-dev -v "$PWD":/kivfs -w /kivfs kivfs-dev bash
```

- `-v "$PWD":/kivfs` připojí projekt – v kontejneru i na Macu jsou stejné soubory.
- `--name kivfs-dev` – kvůli druhému terminálu (A6).
- `--rm` – kontejner se po `exit` smaže (soubory projektu zůstanou, smaže se jen
  to, co bylo mimo `/kivfs`, např. logy v `/tmp`).

Vše dál v sekci A se píše **uvnitř kontejneru**.

### A3. Překlad

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Binárky jsou v `build/`: `kivfs-auth`, `kivfs-sync`, `kivfs-vfs`, `kivfs-db`,
`kivfs-fs`, `kivfs-client`, `kivfs-check` a `test_*`.
Adresář `build/` je v `.gitignore`. (Starý `build-docker/` je z předchozí verze
projektu – nepoužívat, má v cache jinou konfiguraci.)

### A4. Všechny testy najednou

```sh
ctest --test-dir build --output-on-failure
```

Co se spustí (celkem 16 testů, ~15 s; na konci `100% tests passed`):

| Test | Co ověřuje | Doba |
|---|---|---|
| `protocol`, `channel`, `queue` | formát paketů, blokující kanál, fronta mezi vlákny | < 1 s |
| `hello` | HELLO a bloky STATUS (bez sítě) | okamžitě |
| `fsproto` | attr/entry/ctx na drátě, pravidla cesty | okamžitě |
| `syncproto` | zprávy Syncu na drátě (sync_hdr, dbop, arch_hdr…), přenos DB operace | okamžitě |
| `lane` | pruh Syncu: Lamportovy hodiny, seřazená fronta, stabilita + simulace hlasování 3 uzlů (300 náhodných běhů) | < 1 s |
| `db_mock` | strom metadat v paměti (EXISTS, NOT_EMPTY, řazení, 2000 položek) | okamžitě |
| `vfs_perm` | kontrola práv rwx | okamžitě |
| `config` | INI parser, validace, všechny šablony v `conf/`, `op_timeout_ms` | okamžitě |
| `reactor`, `reactor_out`, `events` | epoll reactor, odchozí spojení, timeouty, fronta událostí | ~1 s každý |
| `cluster_local` | **scénář**: 3 uzly × 5 modulů na jednom stroji se propojí; výpadek a obnova Syncu | ~2 s |
| `fs_local` | **scénář**: mkdir/ls/stat/rmdir přes klienta (cluster s 1 uzlem, `conf/single`), chyby, výpadek a obnova DB | ~1 s |
| `sync_local` | **scénář**: kolo hlasování na 3 uzlech – souběh 3 klientů, výpadek / obnova / zamrznutí (SIGSTOP) Syncu | ~6 s |

Užitečné varianty:

```sh
ctest --test-dir build -R fs_local -V        # jen jeden test, s celým výstupem
ctest --test-dir build -E 'reactor|cluster'  # vše kromě pomalých
ctest --test-dir build --rerun-failed --output-on-failure
```

Scénáře lze pustit i přímo (hezčí výstup, vypíší adresář s logy procesů):

```sh
tests/scenarios/fs-local.sh build
tests/scenarios/cluster-local.sh build
tests/scenarios/sync-local.sh build
```

Výsledek je na posledním řádku: `SCÉNÁŘ OK`, nebo `SCÉNÁŘ SELHAL` + řádky
`!! ...` s důvodem. Logy jednotlivých modulů jsou v `/tmp/kivfs-fs.XXXX/`
nebo `/tmp/kivfs-cluster.XXXX/` (název se vypíše) – po `exit` z kontejneru
zmizí, takže je prohlédni předtím.

### A5. Sanitizery (hledání chyb paměti a souběhu)

Překlad do vlastního podadresáře `build/`:

```sh
# AddressSanitizer + UndefinedBehaviorSanitizer (paměť, přetečení, use-after-free)
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/asan -j && ctest --test-dir build/asan --output-on-failure

# ThreadSanitizer (souběh mezi vlákny) – nejde kombinovat s ASan
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=thread"
cmake --build build/tsan -j && ctest --test-dir build/tsan --output-on-failure
```

Scénáře samy kontrolují, že v logu modulů není nález sanitizeru.
Valgrind je v obrazu také: `valgrind --leak-check=full build/test_db_mock`.

### A6. Ruční zkoušení – jeden uzel a klient

V kontejneru spusť 5 modulů uzlu node1 na pozadí (logy do souboru).
Konfigurace `conf/single/` je cluster s jediným uzlem: Sync hlasuje
sám se sebou, takže operace projdou i bez ostatních uzlů (s
`conf/local/node1.ini` by bez node2 a node3 zápisy končily UNAVAILABLE):

```sh
for m in auth sync vfs db fs; do
    build/kivfs-$m -c conf/single/node1.ini 2>>/tmp/node1.log &
done
```

**Druhý terminál** na Macu – sledování logu:

```sh
docker exec -it kivfs-dev tail -f /tmp/node1.log
```

Uvidíš řádky jako `22:29:29.123 [node1 vfs] linka na db@node1 nahoře`.

Zpět v prvním terminálu – klient (smí jen na Auth, node1 má port 31001):

```sh
build/kivfs-client 127.0.0.1 31001
```

```
kivfs> help
kivfs> status              # stav všech 5 modulů uzlu a jejich linek
kivfs> mkdir /docs
kivfs> mkdir /docs/bp 700  # mód osmičkově
kivfs> ls /
kivfs> stat /docs/bp
kivfs> rmdir /docs         # -> NOT_EMPTY
kivfs> ping 10
kivfs> quit
```

Neinteraktivně (vhodné do skriptů):

```sh
printf 'mkdir /a\nls /\nquit\n' | build/kivfs-client 127.0.0.1 31001
```

Výpadek a obnova – např. zastavit DB a zkusit operaci:

```sh
pkill -f 'kivfs-db '                       # zastavit DB
printf 'ls /\nquit\n' | build/kivfs-client 127.0.0.1 31001   # -> UNAVAILABLE
build/kivfs-db -c conf/single/node1.ini 2>>/tmp/node1.log &  # znovu spustit
```

Po restartu je mock DB prázdná (data drží jen v paměti) – to je zatím záměr.

Ukončení všech modulů (řádné, SIGTERM):

```sh
pkill -f 'kivfs-(auth|sync|vfs|db|fs) '
```

### A7. Ruční zkoušení – 3 uzly v jednom kontejneru

```sh
for n in 1 2 3; do for m in auth sync vfs db fs; do
    build/kivfs-$m -c conf/local/node$n.ini 2>>/tmp/node$n.log &
done; done

build/kivfs-check -c conf/local/node1.ini -c conf/local/node2.ini \
                  -c conf/local/node3.ini --wait 20
```

`kivfs-check` vypíše tabulku modulů a spojení a na konci
`VÝSLEDEK: moduly 15/15, spojení 24/24 – OK`.

Porty v `conf/local`: node1 = 310xx, node2 = 311xx, node3 = 312xx
(Auth = …01, Sync …02, VFS …03, DB …04, FS …05). Klient na node2:
`build/kivfs-client 127.0.0.1 31101`.

Pozor: **uzly zatím nesdílejí metadata** – `mkdir` na node1 na node2 neuvidíš.
To přijde se Syncem.

---

## B. Docker compose – 3 „stroje“

Každý uzel je samostatný kontejner (`node1`, `node2`, `node3`) se všemi
5 moduly a konfigurací `conf/3nodes/nodeN.ini` (stejné porty 30001–30005,
liší se jen jméno stroje). Příkazy se píší **na Macu** v kořeni projektu.
Pro kratší zápis:

```sh
alias kc='docker compose -f docker/docker-compose.yml -p kivfs'
```

### B1. Automatický test propojení

```sh
kc up --build --abort-on-container-exit --exit-code-from check
echo $?
```

`echo $?` vypíše exit kód: 0 = vše propojeno.

Postaví obraz, spustí 3 uzly, kontejner `check` počká (max 30 s), až se vše
propojí, vypíše tabulku a skončí – tím se zastaví i uzly.

### B2. Cluster nechat běžet a zkoušet ručně

Spustit uzly na pozadí:

```sh
kc up --build -d node1 node2 node3
```

Co běží, logy uzlu (Ctrl-C ukončí jen výpis, uzel běží dál) a tabulka propojení:

```sh
kc ps
kc logs -f node1
kc run --rm check
```

Klient uvnitř sítě (porty nejsou vystavené ven na Mac):

```sh
kc exec node1 /kivfs/build/kivfs-client node1 30001
```

Výpadek celého uzlu a obnova:

```sh
kc stop node2
kc run --rm check
```

node2 chybí, linky na něj jsou DOWN, `check` skončí s 1. Pak obnova:

```sh
kc start node2
kc run --rm check
```

Znovu OK; ostatní uzly v logu hlásí RESTART (nová inkarnace).

Úklid:

```sh
kc down
```

### B3. Po změně kódu

Compose kopíruje zdrojáky do obrazu, takže po každé změně:

```sh
kc up --build ...
```

(Docker použije cache – znovu se přeloží jen to, co se změnilo.)

---

## Jak všechno vypnout

`quit` v klientovi ukončí **jen klienta** – server (moduly) běží dál, dokud
ho nevypneš. Co běží, ukáže na Macu:

```sh
docker ps
```

- **Vývojový kontejner (A):** v jeho terminálu `exit` (nebo Ctrl-D). Moduly
  spuštěné na pozadí skončí s ním a díky `--rm` se kontejner i smaže.
  Když jsi terminál zavřel jinak: `docker stop kivfs-dev`.
  Jen moduly, kontejner nechat: `pkill -f 'kivfs-(auth|sync|vfs|db|fs) '`.
- **Compose (B):** `kc down` – zastaví a smaže kontejnery i síť.
  Hlášku `Found orphan containers` (zbytky starších pokusů) uklidí
  `kc down --remove-orphans`.
  (`kc stop` je jen zastaví, `kc start` je pak znovu spustí.)
  Pozor na jméno projektu: když jsi spouštěl **bez** `-p kivfs`, jmenuje se
  projekt `docker` a vypne se `docker compose -f docker/docker-compose.yml down`.
- **Cokoli jiného:** `docker stop <ID nebo jméno z docker ps>`.
  Zastavené kontejnery ukáže `docker ps -a`, smaže je `docker container prune`.

---

## C. Na macOS bez Dockeru (jen část)

Klient a přenositelné testy se přeloží i nativně (reactor a moduly ne):

```sh
cmake -S . -B cmake-build-mac && cmake --build cmake-build-mac -j
ctest --test-dir cmake-build-mac --output-on-failure
```

Spustí se protocol, channel, queue, hello, fsproto, db_mock a vfs_perm.

---

## Když něco selže

1. **Překlad** – chyba z `cmake --build` (zkopíruj celou).
2. **Unit test** – `ctest ... -R <jméno> -V` ukáže řádky `FAIL soubor:řádek: podmínka`.
3. **Scénář** – řádky `!! ...` řeknou, který krok; logy modulů jsou v adresáři
   vypsaném na konci (`/tmp/kivfs-*`). Hledej `odmítnuto`, `nedostupný`, `spadla`, `BUG`.
4. **Propojení** – `kivfs-check` u každého spojení napíše důvod
   (`volající neodpověděl`, `linka není nahoře`, `nesedí inkarnace` …).
