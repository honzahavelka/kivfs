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

/* ------------------------------------------------------------------ */
/* Kódy požadavků – musí být < KIVFS_RESPONSE_FLAG                     */
/* U každého kódu popiš pořadí záznamů v požadavku i v odpovědi.       */
/* ------------------------------------------------------------------ */

enum kivfs_request_code {
    /*
     * HELLO – první paket na každém novém spojení (handshake).
     * Strana, která se připojila, se představí; druhá strana odpoví svým ID.
     * Teprve potom smí po spojení jít cokoli jiného.
     *   požadavek: [u32 node_id]            (CLI klient posílá KIVFS_NODE_ID_UNKNOWN)
     *   odpověď:   [u64 req_ts][u32 node_id]
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
    KIVFS_STATUS_INTERNAL      = -7    /* chyba na straně odpovídajícího
                                        * (DB, disk...); detail jen v jeho logu */
};

/* Vrátí statický název stavu pro výpisy a logy ("OK", "BUSY", ...).
 * Obdoba kivfs_strerror, ale pro stavy NA DRÁTĚ. Hodnota přichází od
 * protistrany, takže může být libovolná: pro neznámou vrací "UNKNOWN_STATUS". */
const char *kivfs_status_str(int16_t status);

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
