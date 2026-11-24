#ifndef KIVFS_PROTOCOL_H
#define KIVFS_PROTOCOL_H

/*
 * KIVFS – binární protokol (formát na drátě)
 * ==========================================
 *
 * Tento soubor popisuje JEN to, jak vypadají bajty na síti. Neví nic
 * o socketech ani o alokaci paměti. Práce s celými pakety je v packet.h.
 *
 * Paket = hlavička (20 B) + payload (data_size B).
 *
 *   offset  size  pole
 *   0       4     magic_number   = KIVFS_MAGIC
 *   4       8     timestamp      Lamportův čas odesílatele
 *                                (odesílatel inkrementuje hodiny PŘED každým odesláním)
 *   12      2     request_code   kód požadavku; odpověď má navíc KIVFS_RESPONSE_FLAG
 *   14      2     return_value   int16 (two's complement);
 *                                požadavek: 0, odpověď: enum kivfs_status
 *   16      4     data_size      délka payloadu vč. 4B délek jednotlivých záznamů
 *   20      ...   payload        0..N záznamů: [u32 record_size][record_size bajtů]
 *
 * Příklad (hodnoty z původního návrhu): požadavek 27 s jedním záznamem "/kivfs"
 *
 *   00 00 7A 69                 magic_number  = 31337
 *   00 00 00 00 4F B1 40 DB     timestamp     = 1337016539
 *   00 1B                       request_code  = 27
 *   00 00                       return_value  = 0
 *   00 00 00 0A                 data_size     = 10  (= 4 + 6)
 *   00 00 00 06                 record_size   = 6
 *   2F 6B 69 76 66 73           "/kivfs"      (bez '\0')
 *                               ----------------------------- celkem 30 B
 *
 * Proč BIG-ENDIAN a ruční kódování (a ne packed struct poslaný přes send)?
 *   - Pořadí bajtů musí být dané protokolem, ne procesorem odesílatele.
 *     Big-endian je tradiční "network byte order".
 *   - Packed struct má nezarovnaná pole (timestamp na offsetu 4). Přístup
 *     přes ukazatel na takové pole je na některých CPU pomalý nebo pád.
 *   - Pro 64bitová čísla neexistuje přenosné htonll (Linux má htobe64,
 *     macOS ne). Ruční zápis po bajtech (posuny >> 8) funguje všude stejně.
 *   Proto: v paměti běžná struktura, na drát vždy přes encode/decode.
 *
 * Konvence záznamů v payloadu:
 *   - řetězce NEJSOU ukončené '\0' (délku nese record_size)
 *   - celá čísla jsou big-endian, u32 = záznam o délce 4 B, u64 = 8 B
 *   - záznamy nemají typ ani jméno; jejich význam určuje POŘADÍ, které je
 *     definované pro každý request_code zvlášť (viz komentáře u enumu níže)
 *
 * Odpovědi a párování požadavek <-> odpověď:
 *   - request_code odpovědi = request_code požadavku | KIVFS_RESPONSE_FLAG
 *   - PRVNÍ záznam každé odpovědi (i chybové) = u64 timestamp původního
 *     požadavku (tzv. "ref"); teprve za ním následují data odpovědi
 *   - ID uzlu odesílatele se v paketech nenese; je známé ze spojení,
 *     protože ho protistrana sdělila v úvodním HELLO
 *   - dvojice (node_id spojení, timestamp požadavku) jednoznačně určuje
 *     požadavek: každý uzel před každým odesláním zvýší své hodiny, takže
 *     tentýž uzel nikdy nepošle dvě zprávy se stejným timestampem
 *     (POZOR: platí jen dokud uzel nerestartuje s hodinami od nuly)
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Konstanty formátu                                                   */
/* ------------------------------------------------------------------ */

#define KIVFS_MAGIC             31337u

/* Velikost hlavičky NA DRÁTĚ. Nepoužívat sizeof(struct kivfs_header) –
 * ta je kvůli zarovnání v paměti větší (typicky 24 B). */
#define KIVFS_HEADER_WIRE_SIZE  20u                     /* 4 + 8 + 2 + 2 + 4 */

/* Každý záznam začíná u32 délkou. */
#define KIVFS_RECORD_HDR_SIZE   4u

/* Horní mez data_size. Bez ní by vadný nebo zlý peer poslal data_size =
 * 0xFFFFFFFF a my bychom zkusili alokovat 4 GiB. Hodnotu dolaď podle
 * velikosti datových chunků. */
#define KIVFS_MAX_PAYLOAD       (16u * 1024u * 1024u)   /* 16 MiB */

/* Nejvyšší bit request_code označuje odpověď. Kódy požadavků jsou proto
 * omezené na 0..0x7FFF. Výhoda: odpověď poznáš na první pohled i v hex
 * výpisu (0x001B = požadavek 27, 0x801B = odpověď na 27). */
#define KIVFS_RESPONSE_FLAG     0x8000u

/* ID uzlu 0 je rezervované: CLI klient (není to uzel) nebo spojení,
 * na kterém ještě neproběhlo HELLO. Skutečné uzly mají ID >= 1. */
#define KIVFS_NODE_ID_UNKNOWN   0u

/* Verze protokolu posílaná v HELLO; strany s různou verzí se odmítnou.
 * Zvýšit při každé nekompatibilní změně formátu. */
#define KIVFS_PROTO_VERSION     1u

/* Druh protistrany – serverové moduly (kap. 7.1) a CLI klient.
 * Bude se posílat v HELLO, proto patří k protokolu; hodnoty se nesmí měnit.
 * Textové názvy (= názvy sekcí konfigurace) vrací kivfs_module_name. */
enum kivfs_module {
    KIVFS_MOD_CLIENT = 0,
    KIVFS_MOD_AUTH   = 1,
    KIVFS_MOD_SYNC   = 2,
    KIVFS_MOD_VFS    = 3,
    KIVFS_MOD_DB     = 4,
    KIVFS_MOD_FS     = 5,
    KIVFS_MOD_COUNT
};

/* ------------------------------------------------------------------ */
/* Kódy požadavků – musí být < KIVFS_RESPONSE_FLAG                     */
/* U každého kódu popiš pořadí záznamů v požadavku i v odpovědi.       */
/* ------------------------------------------------------------------ */

enum kivfs_request_code {
    /*
     * HELLO – první paket na každém novém spojení (handshake).
     * Strana, která se připojila, se představí; druhá strana odpoví tím
     * samým za sebe. Teprve potom smí po spojení jít cokoli jiného.
     *   požadavek: [u32 node_id][u32 module][u32 proto_version]
     *              [u64 incarnation][u32 cluster_hash]
     *   odpověď:   [u64 req_ts] + stejných 5 polí za odpovídající stranu
     * CLI klient: node_id = KIVFS_NODE_ID_UNKNOWN, module = KIVFS_MOD_CLIENT,
     * cluster_hash = 0. Kdo se smí s kým spojit a co se kontroluje: hello.h.
     * Chybové stavy: MALFORMED, REJECTED (odpovídající pak spojení zavře).
     */
    KIVFS_REQ_HELLO = 1,

    /*
     * PING – diagnostika spojení a měření RTT. Server obsah nevyhodnocuje.
     *   požadavek: 0..N libovolných záznamů
     *   odpověď:   [u64 req_ts] + kopie všech záznamů požadavku ve stejném pořadí
     * Chybové stavy: MALFORMED (záznam přetéká payload, nebo by odpověď
     * přesáhla KIVFS_MAX_PAYLOAD – kopie je o 12 B větší než požadavek).
     * CLI klient posílá jeden záznam [u32 seq] a kontroluje, že se vrátil.
     */
    KIVFS_REQ_PING = 2,

    /*
     * STATUS – diagnostika propojení (kivfs-check). Kaskáda: modul připojí
     * svůj blok a zeptá se modulů, které volá (Auth -> Sync -> VFS -> DB, FS);
     * jejich bloky připojí za svůj. Synce ostatních uzlů se neptá.
     *   požadavek: prázdný
     *   odpověď:   [u64 req_ts] + 1..N bloků modulů, formát bloku: status.h
     * Modul, který neodpověděl (linka dole, timeout), v odpovědi chybí.
     */
    KIVFS_REQ_STATUS = 3,

    /* ---------------------------------------------------------------
     * Operace klienta (10–99): klient -> Auth -> Sync -> VFS.
     * Klient posílá záznamy níže; Auth před ně vloží [ctx] (fsproto.h)
     * a se STEJNÝM kódem pošle dál, Sync přepošle beze změny, VFS ho
     * zpracuje. Odpověď putuje zpět (každý hop přebalí kvůli ref).
     * Pevné struktury (attr, ctx, entry) = jeden binární záznam, viz
     * fsproto.h. path = absolutní cesta (pravidla: kivfs_path_check).
     * --------------------------------------------------------------- */

    /* STAT  req [path]            resp [ref][attr] */
    KIVFS_REQ_STAT  = 10,
    /* MKDIR req [path][u32 mode]  resp [ref]        mode = jen práva (07777) */
    KIVFS_REQ_MKDIR = 11,
    /* LIST  req [path]            resp [ref][entry]...  (všechny položky,
     *                             seřazené podle jména; bez stránkování –
     *                             adresář nad KIVFS_MAX_PAYLOAD -> INTERNAL) */
    KIVFS_REQ_LIST  = 12,
    /* RMDIR req [path]            resp [ref]        adresář musí být prázdný */
    KIVFS_REQ_RMDIR = 13,

    /* ---------------------------------------------------------------
     * Interní operace DB (100–199): VFS -> DB. DB je úložiště, práva
     * nekontroluje (dělá to VFS); kontroluje jen konzistenci stromu.
     * --------------------------------------------------------------- */

    /* DB_GET    req [path]                          resp [ref][attr] */
    KIVFS_REQ_DB_GET    = 100,
    /* DB_MKNODE req [path][mknode]                  resp [ref][attr]
     *           mknode = u32 mode | u32 uid | u32 gid (jeden záznam, 12 B),
     *           mode včetně typu (KIVFS_MODE_DIR | práva) */
    KIVFS_REQ_DB_MKNODE = 101,
    /* DB_LIST   req [path]                          resp [ref][entry]... */
    KIVFS_REQ_DB_LIST   = 102,
    /* DB_RMNODE req [path]                          resp [ref]
     *           adresář musí být prázdný (NOT_EMPTY), "/" nejde (ACCESS_DENIED) */
    KIVFS_REQ_DB_RMNODE = 103,

    /* ... další kódy doplnit podle specifikace ... */
};

/* ------------------------------------------------------------------ */
/* Stavové kódy NA DRÁTĚ (return_value v odpovědi)                     */
/* Říkají protistraně, jak dopadl JEJÍ požadavek. Nemíchat s errors.h. */
/* ------------------------------------------------------------------ */

enum kivfs_status {
    KIVFS_STATUS_OK            =  0,
    KIVFS_STATUS_UNKNOWN_REQ   = -1,   /* neznámý request_code */
    KIVFS_STATUS_MALFORMED     = -2,   /* špatný počet/typ záznamů v payloadu */
    KIVFS_STATUS_NOT_FOUND     = -3,   /* soubor/adresář neexistuje */
    KIVFS_STATUS_EXISTS        = -4,   /* soubor/adresář už existuje */
    KIVFS_STATUS_ACCESS_DENIED = -5,   /* nedostatečná práva */
    KIVFS_STATUS_BUSY          = -6,   /* dočasně nelze (zámek apod.), zkus znovu */
    KIVFS_STATUS_INTERNAL      = -7,   /* chyba na straně odpovídajícího
                                        * (DB, disk...); detail jen v jeho logu */
    KIVFS_STATUS_REJECTED      = -8,   /* HELLO odmítnuto (nesedí verze, cluster,
                                        * nebo protistrana se nesmí připojit);
                                        * odpovídající pak spojení zavře */
    KIVFS_STATUS_UNAVAILABLE   = -9,   /* chybí závislost (např. VFS nemá DB),
                                        * zkus později */
    KIVFS_STATUS_NOT_DIR       = -10,  /* složka cesty (nebo cíl) není adresář */
    KIVFS_STATUS_NOT_EMPTY     = -11   /* rmdir neprázdného adresáře */
};

/* Vrátí statický název stavu pro výpisy a logy ("OK", "BUSY", ...).
 * Obdoba kivfs_strerror, ale pro stavy NA DRÁTĚ. Hodnota přichází od
 * protistrany, takže může být libovolná: pro neznámou vrací "UNKNOWN_STATUS". */
const char *kivfs_status_str(int16_t status);

/* Název modulu ("auth", "sync", ...; "client") = název jeho sekce
 * v konfiguraci. Hodnota může přijít od protistrany: neznámá -> "?". */
const char *kivfs_module_name(uint32_t module);

/* ------------------------------------------------------------------ */
/* Request / response kódy                                             */
/* Funkce jsou "static inline" přímo v hlavičce: jsou to jednořádkové   */
/* bitové operace, kompilátor je vloží na místo volání bez režie.      */
/* ------------------------------------------------------------------ */

/* 1, pokud kód označuje odpověď (má nastavený KIVFS_RESPONSE_FLAG). */
static inline int kivfs_proto_is_response(uint16_t code)
{
    return (code & KIVFS_RESPONSE_FLAG) != 0;
}

/* Z kódu požadavku udělá kód odpovědi: 27 (0x001B) -> 0x801B. */
static inline uint16_t kivfs_proto_response_code(uint16_t request_code)
{
    return (uint16_t)(request_code | KIVFS_RESPONSE_FLAG);
}

/* Odstraní příznak odpovědi: 0x801B -> 27, 27 -> 27.
 * Hodí se pro dispatch podle typu operace bez ohledu na směr. */
static inline uint16_t kivfs_proto_base_code(uint16_t code)
{
    return (uint16_t)(code & (uint16_t)~KIVFS_RESPONSE_FLAG);
}

/* ------------------------------------------------------------------ */
/* Big-endian kódování celých čísel                                    */
/* JEDINÉ místo v projektu, kde se řeší pořadí bajtů na drátě.         */
/* Používá ho kódování hlavičky (protocol.c) i záznamů (packet.c).     */
/*                                                                     */
/* Zápis po bajtech přes posuny funguje stejně na little- i big-endian */
/* CPU a nevyžaduje zarovnanou adresu. Před posunem se bajt vždy       */
/* přetypuje na cílový typ: (uint32_t)p[0] << 24 – bez přetypování by  */
/* se posouval int a 0xFF << 24 by přeteklo (nedefinované chování).    */
/* ------------------------------------------------------------------ */

static inline void kivfs_proto_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void kivfs_proto_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void kivfs_proto_put_u64(uint8_t *p, uint64_t v)
{
    kivfs_proto_put_u32(p,     (uint32_t)(v >> 32));
    kivfs_proto_put_u32(p + 4, (uint32_t)v);
}

static inline uint16_t kivfs_proto_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t kivfs_proto_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline uint64_t kivfs_proto_get_u64(const uint8_t *p)
{
    return ((uint64_t)kivfs_proto_get_u32(p) << 32) | kivfs_proto_get_u32(p + 4);
}

/* ------------------------------------------------------------------ */
/* Hlavička                                                            */
/* ------------------------------------------------------------------ */

/* Hlavička V PAMĚTI: běžná (nepacked) struktura, hodnoty v host byte order.
 * Pořadí polí odpovídá drátu jen kvůli čitelnosti – rozložení v paměti
 * (padding) na drát nemá žádný vliv. */
struct kivfs_header {
    uint32_t magic_number;
    uint64_t timestamp;
    uint16_t request_code;
    int16_t  return_value;
    uint32_t data_size;
};

/* Zapíše hlavičku do přesně 20 bajtů v big-endian pořadí.
 * Nic nevaliduje – zapíše, co dostane. Nemůže selhat. */
void kivfs_proto_header_encode(const struct kivfs_header *h,
                               uint8_t out[KIVFS_HEADER_WIRE_SIZE]);

/* Přečte hlavičku z 20 bajtů a zvaliduje ji:
 *   - magic_number musí být KIVFS_MAGIC         jinak KIVFS_EBADMAGIC
 *   - data_size nesmí přesáhnout MAX_PAYLOAD     jinak KIVFS_ETOOBIG
 * Payload nečte – po úspěchu volající ví, kolik bajtů má dočíst (data_size).
 * Vrací KIVFS_OK nebo chybu z errors.h; při chybě je obsah *h nedefinovaný. */
int  kivfs_proto_header_decode(const uint8_t in[KIVFS_HEADER_WIRE_SIZE],
                               struct kivfs_header *h);

#endif /* KIVFS_PROTOCOL_H */
