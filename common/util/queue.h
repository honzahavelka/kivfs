#ifndef KIVFS_QUEUE_H
#define KIVFS_QUEUE_H

/*
 * KIVFS – omezená blokující fronta (MPMC)
 * =======================================
 *
 * Fronta pro více producentů i více konzumentů (multi-producer,
 * multi-consumer). Odděluje I/O vlákno reactoru, které nesmí nikdy čekat,
 * od workerů, kteří dělají pomalou práci (MySQL, disk) a čekat smí.
 *
 * Uvnitř: kruhový buffer pevné kapacity + jeden mutex + dvě podmínkové
 * proměnné (not_empty – na ni čekají konzumenti, not_full – na ni čekají
 * producenti). Zámek se drží jen po dobu memcpy jedné položky.
 *
 * Položky mají pevnou velikost elem_size a do fronty se KOPÍRUJÍ (memcpy),
 * takže push/pop nic nealokuje. Typicky struktura { conn_id, packet }:
 * zkopírováním struktury přechází do fronty i vlastnictví payloadu
 * paketu (packet.data) a při pop zase ven.
 *
 * Vlastnictví při selhání push: položka zůstává VOLAJÍCÍMU. (Opak
 * kivfs_reactor_send, který paket převezme vždy.) Fronta je obecná a neví,
 * jak má být odmítnutá práce vyřízena – volající musí např. odpovědět
 * KIVFS_STATUS_BUSY a pak paket uvolnit.
 *
 * Použití: obecný stavební kámen. V modulech ji nepoužívej přímo – je
 * zabalená ve frontě událostí (server/core/events.h), která navíc hlídá, aby se
 * výsledky odchozích požadavků nikdy neztratily.
 *
 *     // producent, který čekat nesmí (I/O vlákno)
 *     if (kivfs_queue_try_push(q, &item) != KIVFS_OK) { ... odmítnout ... }
 *
 *     // konzument (worker)
 *     while (kivfs_queue_pop(q, &item) == KIVFS_OK) { ... }
 *     // KIVFS_ESHUTDOWN -> worker končí
 *
 * Návratové kódy: errors.h.
 */

#include <stddef.h>

/* Neprůhledná struktura: buffer, indexy, mutex a condvary zná jen queue.c. */
struct kivfs_queue;

/* Vytvoří frontu pro capacity položek o velikosti elem_size (obojí > 0).
 * free_elem (smí být NULL) uvolní obsah položky, která ve frontě zůstane
 * při destroy – fronta sama neví, co položka vlastní (např. packet.data).
 * Vrací KIVFS_OK, KIVFS_EINVAL (nulový parametr nebo capacity * elem_size
 * přeteče) nebo KIVFS_ENOMEM. */
int  kivfs_queue_create(struct kivfs_queue **out, size_t capacity, size_t elem_size,
                        void (*free_elem)(void *elem));

/* Uvolní frontu i zbylé položky (přes free_elem).
 * Volat až když frontu žádné vlákno nepoužívá: shutdown -> počkat na
 * všechny workery (pthread_join) -> destroy. */
void kivfs_queue_destroy(struct kivfs_queue *q);

/* Vloží kopii *elem na konec. Je-li fronta plná, ČEKÁ, dokud se neuvolní
 * místo. Pro vlákna, která čekat smí (workery).
 * KIVFS_OK nebo KIVFS_ESHUTDOWN (i když shutdown přijde během čekání). */
int  kivfs_queue_push(struct kivfs_queue *q, const void *elem);

/* Vloží kopii *elem na konec; NIKDY nečeká. Pro I/O vlákno reactoru.
 * KIVFS_OK, KIVFS_EAGAIN (fronta je plná) nebo KIVFS_ESHUTDOWN. */
int  kivfs_queue_try_push(struct kivfs_queue *q, const void *elem);

/* Vyzvedne nejstarší položku do *out (FIFO). Je-li fronta prázdná, ČEKÁ.
 * Po kivfs_queue_shutdown ještě vydá všechny zbylé položky (drain), teprve
 * pak vrací KIVFS_ESHUTDOWN – pro workera signál, že má skončit.
 * Požadavky, které už byly přijaty, tak dostanou odpověď. */
int  kivfs_queue_pop(struct kivfs_queue *q, void *out);

/* Ukončí frontu: další push/try_push vrátí KIVFS_ESHUTDOWN a VŠECHNA
 * čekající vlákna se probudí (broadcast na obě condvary). Idempotentní. */
void kivfs_queue_shutdown(struct kivfs_queue *q);

#endif /* KIVFS_QUEUE_H */
