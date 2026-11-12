#ifndef KIVFS_CHANNEL_H
#define KIVFS_CHANNEL_H

/*
 * KIVFS – blokující kanál pro ODCHOZÍ spojení (varianta B)
 * ========================================================
 *
 * Kanál = jedno TCP spojení, po kterém posíláme požadavky a čekáme na
 * odpovědi. Spojuje socket.h (přenos bajtů) a packet.h (formát paketu).
 *
 * Kdo ho používá:
 *   - CLI klient: klient -> uzel
 *   - workery uzlu: uzel -> jiný uzel
 * Příchozí spojení obsluhuje epoll reactor, NE tento modul.
 *
 * Pravidla:
 *   - Jeden kanál = jeden vlastník (vlákno). Kanál se mezi vlákny nesdílí:
 *     dva současné zápisy by se v TCP proudu proložily a rozbily pakety.
 *   - Na kanálu je vždy nejvýše jeden rozpracovaný požadavek. Díky tomu
 *     je další přijatý paket vždy odpověď na ten náš.
 *   - Modul NEMĚNÍ obsah paketů. Timestamp (Lamportovy hodiny) nastavuje
 *     před odesláním a zpracuje po přijetí modul Sync nad touto vrstvou.
 *   - Modul nedělá handshake HELLO (ten nese Lamportův čas, tedy patří
 *     Sync). peer_node_id vyplní volající po úspěšném HELLO.
 *   - Po JAKÉKOLI chybě send/recv/call je kanál nepoužitelný a musí se
 *     zavřít: nevíme, kolik bajtů z paketu prošlo, takže už nevíme, kde
 *     v TCP proudu začíná další paket. Případně se otevře nový kanál.
 *
 * Dotaz na více uzlů najednou (send-all / recv-all) – nejdřív kivfs_chan_send
 * na všechny kanály, potom kivfs_chan_recv ze všech – skládá modul nad touto
 * vrstvou. Latence je pak ~ nejpomalejší uzel, ne součet všech.
 *
 * Typické použití:
 *     struct kivfs_chan ch;
 *     kivfs_chan_init(&ch);
 *     if (kivfs_chan_open(&ch, "node2", 9000, KIVFS_CHAN_DEFAULT_TIMEOUT_MS) != KIVFS_OK)
 *         ...;
 *     ... HELLO přes kivfs_chan_call, pak ch.peer_node_id = ID z odpovědi ...
 *     rc = kivfs_chan_call(&ch, &req, &resp);
 *     if (rc != KIVFS_OK) { kivfs_chan_close(&ch); ... }
 *     ... zpracovat resp, kivfs_pkt_free(&resp) ...
 *     kivfs_chan_close(&ch);
 *
 * Návratové kódy: errors.h.
 */

#include <stdint.h>
#include "packet.h"
#include "socket.h"

#define KIVFS_CHAN_DEFAULT_TIMEOUT_MS 5000

struct kivfs_chan {
    int      fd;                                  /* -1 = zavřeno */
    uint32_t peer_node_id;                        /* KIVFS_NODE_ID_UNKNOWN do HELLO */
    char     peer_addr[KIVFS_SOCK_ADDR_STRLEN];   /* "ip:port" pro logy */
};

/* Výchozí (zavřený) stav: fd = -1, peer_node_id = KIVFS_NODE_ID_UNKNOWN,
 * prázdná adresa. Volat před open, aby close byl bezpečný i bez open. */
void kivfs_chan_init(struct kivfs_chan *c);

/* Připojí se na host:port. timeout_ms se použije pro connect a pak i pro
 * každé čtení/zápis na kanálu (SO_RCVTIMEO/SO_SNDTIMEO).
 * Při chybě zůstane kanál zavřený. */
int  kivfs_chan_open(struct kivfs_chan *c, const char *host, uint16_t port, int timeout_ms);

/* Zavře spojení a vrátí kanál do stavu po kivfs_chan_init. Idempotentní. */
void kivfs_chan_close(struct kivfs_chan *c);

/* Odešle celý paket: zakóduje hlavičku a pošle ji spolu s payloadem
 * jedním writev. Paket nemění ani neuvolňuje. */
int  kivfs_chan_send(struct kivfs_chan *c, const struct kivfs_packet *p);

/* Přijme celý paket:
 *   1. přečte 20 B hlavičky
 *   2. zvaliduje ji (magic, data_size) – při chybě se payload ani nealokuje
 *   3. alokuje data_size bajtů a dočte payload
 * Úspěch: out->data alokováno, uvolní volající přes kivfs_pkt_free.
 * Chyba:  out je prázdný paket, nic k uvolnění. */
int  kivfs_chan_recv(struct kivfs_chan *c, struct kivfs_packet *out);

/* Požadavek-odpověď v jednom: send + recv + kontrola, že odpověď patří
 * k požadavku:
 *   - má KIVFS_RESPONSE_FLAG,
 *   - kivfs_proto_base_code(odpověď) == kód požadavku,
 *   - ref (první záznam odpovědi) == req->header.timestamp.
 * Pokud nesedí, vrací KIVFS_EUNEXPECTED a resp je uvolněný.
 * Pozor: KIVFS_OK znamená jen "přišla platná odpověď"; jak operace
 * dopadla u protistrany, říká resp->header.return_value (kivfs_status). */
int  kivfs_chan_call(struct kivfs_chan *c, const struct kivfs_packet *req,
                     struct kivfs_packet *resp);

#endif /* KIVFS_CHANNEL_H */
