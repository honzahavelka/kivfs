# Konfigurace KIVFS

Jeden INI soubor na **uzel**; stejný soubor dostanou všechny moduly
(a všechny stroje) daného uzlu. Modul se spouští `kivfs-<modul> -c soubor.ini`
a čte z něj `[cluster]`, svou sekci a sekce modulů, které volá
(Auth → Sync → VFS → DB, FS). Formát a validace: `common/util/config.h`,
`server/core/modconf.h`.

| Šablona | Kdy |
|---|---|
| `3nodes/node1..3.ini` | 3 stroje (kontejnery), na každém všech 5 modulů; stejné porty 30001–30005, liší se jen adresa (`node1..node3` musí být přeložitelné – Docker síť nebo `/etc/hosts`). |
| `local/node1..3.ini` | všechny 3 uzly na jednom stroji (vývoj); `127.0.0.1`, porty 310xx / 320xx / 330xx. |
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
- **Tři kontejnery** (`3nodes/`):
  `docker compose -f docker/docker-compose.yml up --build --abort-on-container-exit --exit-code-from check`
  – exit kód je výsledek `kivfs-check`.
- Ruční kontrola běžícího clusteru:
  `kivfs-check -c node1.ini -c node2.ini -c node3.ini --wait 20`
  (klient smí jen do Auth, STATUS projde kaskádou Auth → Sync → VFS → DB, FS).
