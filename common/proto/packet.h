#ifndef KIVFS_PACKET_H
#define KIVFS_PACKET_H

/*
 * KIVFS – práce s celým paketem: sestavení (builder) a čtení (reader)
 * =====================================================================
 *
 * Neví nic o socketech; formát bajtů je popsán v protocol.h.
 * Všechny funkce vracející int vrací kódy z errors.h.
 *
 * Payload se drží rovnou ve WIRE formátu (big-endian, s délkami záznamů).
 * Odeslání je pak jen "hlavička + memcpy payloadu" a přijatý payload se
 * nemusí nijak převádět – reader ho čte přímo.
 *
 * Životní cyklus – odeslání požadavku:
 *     struct kivfs_packet req;
 *     kivfs_pkt_init(&req, KIVFS_REQ_..., lamport_ts);   // nic nealokuje
 *     kivfs_pkt_add_string(&req, "/kivfs");              // alokuje dle potřeby
 *     kivfs_pkt_add_u64(&req, offset);
 *     ... odeslat ...
 *     kivfs_pkt_free(&req);                              // VŽDY uvolnit
 *
 * Životní cyklus – zpracování přijatého požadavku a odpověď:
 *     struct kivfs_pkt_reader r;
 *     char path[256];
 *     kivfs_pkt_reader_init(&r, &req);
 *     if (kivfs_pkt_read_string(&r, path, sizeof path) != KIVFS_OK
 *         || !kivfs_pkt_reader_at_end(&r))
 *         status = KIVFS_STATUS_MALFORMED;
 *     ...
 *     struct kivfs_packet resp;
 *     kivfs_pkt_init_response(&resp, &req, lamport_ts, status); // vloží ref
 *     kivfs_pkt_add_...(&resp, ...);                            // data odpovědi
 *
 * Vlastnictví paměti: paket vlastní svůj buffer data. Paket se nekopíruje
 * přiřazením (struct a = b sdílí buffer -> dvojí free). Reader paměť
 * nevlastní, jen ukazuje do paketu.
 */

#include <stddef.h>
#include <stdint.h>
#include "proto/protocol.h"

/* ------------------------------------------------------------------ */
/* Paket + builder                                                     */
/* ------------------------------------------------------------------ */

struct kivfs_packet {
    struct kivfs_header header;  /* header.data_size == počet platných bajtů v data;
                                  * builder ho udržuje automaticky */
    uint8_t *data;               /* payload ve wire formátu, vlastněn paketem;
                                  * NULL u paketu bez záznamů */
    size_t   capacity;           /* kolik je v data alokováno (>= data_size).
                                  * Builder zvětšuje buffer geometricky (×2),
                                  * aby N přidání nestálo N realloců. */
};

/* Připraví prázdný POŽADAVEK: magic, kód, timestamp, return_value = 0,
 * data_size = 0, data = NULL. Nic nealokuje, nemůže selhat.
 * request_code musí být < KIVFS_RESPONSE_FLAG. */
void kivfs_pkt_init(struct kivfs_packet *p, uint16_t request_code, uint64_t timestamp);

/* Připraví ODPOVĚĎ na req:
 *   - request_code = kód požadavku | KIVFS_RESPONSE_FLAG
 *   - return_value = status (enum kivfs_status)
 *   - první záznam = u64 req->header.timestamp (ref pro párování)
 * timestamp = aktuální Lamportův čas ODPOVÍDAJÍCÍHO uzlu (ne čas z req).
 * Ref se vkládá vždy, i do chybové odpovědi, aby šla spárovat.
 * Vrací KIVFS_OK nebo KIVFS_ENOMEM (pak je resp prázdný, nic k uvolnění). */
int  kivfs_pkt_init_response(struct kivfs_packet *resp, const struct kivfs_packet *req,
                             uint64_t timestamp, int16_t status);

/* Uvolní payload a vrátí paket do prázdného stavu (data = NULL, data_size = 0).
 * Idempotentní: volat dvakrát je bezpečné. */
void kivfs_pkt_free(struct kivfs_packet *p);

/* ---- přidání záznamu na konec payloadu ----
 * Každá funkce zapíše [u32 délka][data] a zvětší header.data_size.
 * Kontroluje, že nový data_size nepřeteče u32 a nepřesáhne
 * KIVFS_MAX_PAYLOAD (jinak KIVFS_ETOOBIG); při selhání alokace KIVFS_ENOMEM.
 * Při jakékoli chybě zůstane paket BEZE ZMĚNY (žádný napůl zapsaný záznam). */

/* Libovolná binární data (len může být 0 = prázdný záznam). */
int  kivfs_pkt_add_bytes (struct kivfs_packet *p, const void *buf, uint32_t len);
/* Řetězec jako záznam o délce strlen(s), BEZ koncového '\0'. */
int  kivfs_pkt_add_string(struct kivfs_packet *p, const char *s);
/* Číslo jako 4 B / 8 B záznam v big-endian. */
int  kivfs_pkt_add_u32   (struct kivfs_packet *p, uint32_t v);
int  kivfs_pkt_add_u64   (struct kivfs_packet *p, uint64_t v);

/* Z odpovědi vyčte ref = timestamp požadavku, na který odpovídá (první záznam).
 * Kontroluje, že paket má KIVFS_RESPONSE_FLAG a že první záznam je u64;
 * jinak KIVFS_EMALFORMED. Paket nemění (čte nezávisle na readeru). */
int  kivfs_pkt_response_ref(const struct kivfs_packet *resp, uint64_t *req_timestamp);

/* ------------------------------------------------------------------ */
/* Reader – sekvenční čtení záznamů s kontrolou mezí                   */
/* ------------------------------------------------------------------ */

/* Kurzor nad payloadem. Čte záznamy popořadě od začátku; zpět nejde.
 * U ODPOVĚDI je prvním záznamem vždy u64 ref – přečti ho (nebo přeskoč
 * přes kivfs_pkt_read_u64) dřív, než začneš číst data odpovědi.
 *
 * Každé čtení kontroluje, že hlavička záznamu i jeho data leží celá uvnitř
 * payloadu. Při JAKÉKOLI chybě se pozice NEPOSUNE, takže stav readeru
 * zůstane konzistentní. */
struct kivfs_pkt_reader {
    const uint8_t *data;   /* = packet->data, reader paměť nevlastní */
    uint32_t size;         /* = packet->header.data_size */
    uint32_t pos;          /* offset dalšího nepřečteného záznamu */
};

/* Nastaví reader na začátek payloadu paketu p. Paket musí žít déle než reader. */
void kivfs_pkt_reader_init(struct kivfs_pkt_reader *r, const struct kivfs_packet *p);

/* Další záznam bez kopírování: *out ukazuje přímo do paketu, *len je délka.
 * Ukazatel platí jen do kivfs_pkt_free paketu.
 * KIVFS_EEND = žádný další záznam, KIVFS_EMALFORMED = záznam přetéká payload. */
int  kivfs_pkt_read_bytes (struct kivfs_pkt_reader *r, const uint8_t **out, uint32_t *len);

/* Další záznam jako řetězec: zkopíruje ho do dst a doplní '\0'.
 * dst_size je velikost celého bufferu VČETNĚ místa pro '\0', tj. řetězec
 * může mít nejvýše dst_size - 1 znaků; jinak KIVFS_ETYPE.
 * POZOR: záznam může obsahovat i bajt 0 uprostřed – kdo potřebuje přesnou
 * délku nebo binární data, použije kivfs_pkt_read_bytes. */
int  kivfs_pkt_read_string(struct kivfs_pkt_reader *r, char *dst, size_t dst_size);

/* Další záznam jako číslo; záznam musí mít přesně 4 B / 8 B, jinak KIVFS_ETYPE. */
int  kivfs_pkt_read_u32   (struct kivfs_pkt_reader *r, uint32_t *out);
int  kivfs_pkt_read_u64   (struct kivfs_pkt_reader *r, uint64_t *out);

/* 1 = všechny bajty payloadu přečteny, 0 = ještě něco zbývá.
 * Volej po přečtení všech očekávaných záznamů: přebytečná data znamenají,
 * že si strany nerozumí ve formátu (-> KIVFS_STATUS_MALFORMED). */
int  kivfs_pkt_reader_at_end(const struct kivfs_pkt_reader *r);

#endif /* KIVFS_PACKET_H */
