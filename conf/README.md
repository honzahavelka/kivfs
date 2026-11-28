# Konfigurace KIVFS

Jeden INI soubor na **uzel**; stejný soubor dostanou všechny moduly
(a všechny stroje) daného uzlu. Modul se spouští `kivfs-<modul> -c soubor.ini`
a čte z něj `[cluster]`, svou sekci a sekce modulů, které volá
(Auth → Sync → VFS, DB; VFS → DB, FS; DB → Sync). Formát a validace: `common/util/config.h`,
`server/core/modconf.h`.

| Šablona | Kdy |
|---|---|
| `3nodes/node1..3.ini` | 3 stroje (kontejnery), na každém všech 5 modulů; stejné porty 30001–30005, liší se jen adresa (`node1..node3` musí být přeložitelné – Docker síť nebo `/etc/hosts`). |
| `local/node1..3.ini` | všechny 3 uzly na jednom stroji (vývoj); `127.0.0.1`, porty 310xx / 311xx / 312xx (pod 32768 – výš jádro přiděluje zdrojové porty odchozím spojením a port by mohl být obsazený). |
| `single/node1.ini` | cluster s jediným uzlem (scénář `fs-local`, ruční zkoušení); kolo hlasování proběhne bez sítě. Porty jako `local/node1.ini`. |
| `examples/split-node1.ini` | ukázka uzlu rozděleného na víc strojů + volitelné klíče. |

Pravidla, která se snadno přehlédnou:
- `nodeN` je adresa **Sync** modulu uzlu N; N ≥ 1 je node id (bez úvodních nul).
- Seznam `nodeN` musí být na všech uzlech stejný – konfigurace to sama
  nepozná, odhalí to až HELLO.
- Neznámý klíč ve vlastní sekci = chyba (překlepy se neodpouštějí).
- Adresy se překládají až při připojování, ne při startu.

## Spuštění scénářů

- **Jeden stroj** (`local/`): `tests/scenarios/cluster-local.sh <build>` – spustí
  15 procesů, ověří propojení, zastaví a znovu spustí `sync@node2`
  (výpadek a obnova), vše ukončí. Běží i v `ctest` jako `cluster_local`.
- **Operace FS na jednom uzlu** (`single/node1.ini`): `tests/scenarios/fs-local.sh <build>`
  – mkdir/ls/stat/rmdir přes klienta, chyby, výpadek a obnova DB (ctest `fs_local`).
- Ručně: spustit 5 modulů uzlu a `kivfs-client 127.0.0.1 31001`
  (příkazy `mkdir`, `ls`, `stat`, `rmdir`, `status`, `help`).
- **Tři kontejnery** (`3nodes/`):
  `docker compose -f docker/docker-compose.yml up --build --abort-on-container-exit --exit-code-from check`
  – exit kód je výsledek `kivfs-check`.
- Ruční kontrola běžícího clusteru:
  `kivfs-check -c node1.ini -c node2.ini -c node3.ini --wait 20`
  (klient smí jen do Auth, STATUS projde kaskádou Auth → Sync → VFS → DB, FS).
  Kontroluje na každém uzlu 6 linek: auth→sync, sync→vfs, sync→db, vfs→db,
  vfs→fs, db→sync, plus sync↔sync mezi uzly (3 uzly = 24 spojení).
- **Uzel na víc strojích:** DB a Sync se volají navzájem (DB → Sync = `SYNC_SUBMIT`,
  Sync → DB = `DB_APPLY`), takže stroj s DB musí dosáhnout na port `[sync]`
  a stroj se Syncem na port `[db]`. Bez linky DB → Sync DB odpovídá na zápisy
  UNAVAILABLE, čtení fungují dál.

## Klíče specifické pro moduly

| Sekce | Klíč | Význam |
|---|---|---|
| `[cluster]` | `op_timeout_ms` | časový rámec jedné operace klienta, 400–600000 (výchozí 30000 jako `Timeout` v původní práci, kap. 7.4.3). Ostatní timeouty se z něj odvozují (`kivfs_modconf_timeouts`, `server/core/modconf.h`) – viz tabulka níže. Klient musí mít timeout větší (`kivfs-client -t <ms>`, výchozí 35000). |
| `[sync]` | `order_cli` | `yes` (výchozí) = každá klientská operace nejdřív dostane časovou značku Tcli hlasováním všech uzlů (kap. 7.4.1) a provádí se sériově v jejím pořadí; `no` = rovnou do VFS (pro měření rozdílu). |
| `[sync]` | `max_pending` | (obecný klíč) u Syncu zvýšit, šablony mají 256 – na jednom spojení k jinému Syncu čekají souběžná kola i COMMIT/ABORT. |
| `[db]` | `backend` | úložiště metadat: `mock` (výchozí, strom v paměti – po restartu prázdný) |

### Odvozené timeouty (O = `op_timeout_ms`)

Každý vnitřní timeout vyprší dřív než vnější, aby vnější krok dostal skutečnou
odpověď: UNAVAILABLE = operace jistě neproběhla, TIMEOUT = výsledek neznámý
(zápis mohl proběhnout – kolo Tdb je dohodnuté a dokončí se).

| Krok | Timeout | O = 30 s | Poznámka |
|---|---|---|---|
| Auth → Sync | O | 30 s | bez odpovědi → TIMEOUT |
| kolo PROPOSE (CLI i DB), outbox, QUERY, CLOCK | 2/16 O | 3,75 s | selhání kola → UNAVAILABLE |
| Sync → VFS | 8/16 O | 15 s | zbytek O = čekání ve frontě CLI (sériové provádění) |
| VFS → DB čtení (GET, LIST) | 2/16 O | 3,75 s | bez odpovědi → UNAVAILABLE |
| VFS → DB zápis (MKNODE, RMNODE) | 5/16 O | 9,4 s | bez odpovědi → TIMEOUT |
| DB → Sync SUBMIT | 4/16 O | 7,5 s | kolo + potvrzení COMMIT + fronta pruhu DB + APPLY; bez odpovědi → TIMEOUT |
| Sync → DB APPLY, LOG_HEAD | 1/16 O | 1,9 s | vypršení = APPLY se zopakuje (DB pozná opakování) |
