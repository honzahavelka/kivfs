/* pthread funkce jsou POSIX; čisté C11 je bez tohoto makra nezpřístupní. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include "util/queue.h"
#include "proto/errors.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Kruhový buffer:
 *
 *   index:   0    1    2    3    4    5
 *          [ _ | J3 | J4 | J5 | _  | _  ]
 *                ^head          ^tail = (head + count) % capacity
 *
 * head  = index nejstarší položky (odtud pop)
 * count = počet položek; tail se z něj dopočítá, takže stav "plná"
 *         (count == capacity) a "prázdná" (count == 0) se nedají splést,
 *         i když v obou případech platí head == tail.
 */
struct kivfs_queue {
    pthread_mutex_t lock;       /* chrání VŠECHNA pole níže */
    pthread_cond_t  not_empty;  /* signál pro konzumenty: přibyla položka */
    pthread_cond_t  not_full;   /* signál pro producenty: uvolnilo se místo */

    uint8_t *buf;               /* capacity * elem_size bajtů */
    size_t   capacity;
    size_t   elem_size;
    size_t   head;
    size_t   count;
    int      shutdown;

    void   (*free_elem)(void *elem);
};

static void *slot(const struct kivfs_queue *q, size_t index)
{
    return q->buf + index * q->elem_size;
}

/* Vložení položky; volat se zamčeným q->lock a jen když count < capacity. */
static void put_locked(struct kivfs_queue *q, const void *elem)
{
    size_t tail = (q->head + q->count) % q->capacity;
    memcpy(slot(q, tail), elem, q->elem_size);
    q->count++;
    /* signal stačí: přibyla JEDNA položka, takže má smysl vzbudit
     * jednoho konzumenta. Broadcast by zbytečně vzbudil všechny. */
    pthread_cond_signal(&q->not_empty);
}

int kivfs_queue_create(struct kivfs_queue **out, size_t capacity, size_t elem_size,
                       void (*free_elem)(void *elem))
{
    *out = NULL;
    if (capacity == 0 || elem_size == 0 || capacity > SIZE_MAX / elem_size)
        return KIVFS_EINVAL;

    struct kivfs_queue *q = calloc(1, sizeof *q);
    if (q == NULL)
        return KIVFS_ENOMEM;

    q->buf = malloc(capacity * elem_size);
    if (q->buf == NULL) {
        free(q);
        return KIVFS_ENOMEM;
    }

    /* Inicializace synchronizačních primitiv může selhat (nedostatek
     * prostředků); při chybě uklidit jen to, co se už podařilo. */
    if (pthread_mutex_init(&q->lock, NULL) != 0)
        goto fail_buf;
    if (pthread_cond_init(&q->not_empty, NULL) != 0)
        goto fail_mutex;
    if (pthread_cond_init(&q->not_full, NULL) != 0)
        goto fail_not_empty;

    q->capacity  = capacity;
    q->elem_size = elem_size;
    q->free_elem = free_elem;
    *out = q;
    return KIVFS_OK;

fail_not_empty:
    pthread_cond_destroy(&q->not_empty);
fail_mutex:
    pthread_mutex_destroy(&q->lock);
fail_buf:
    free(q->buf);
    free(q);
    return KIVFS_ENOMEM;
}

void kivfs_queue_destroy(struct kivfs_queue *q)
{
    if (q == NULL)
        return;

    /* Zámek už není potřeba – podle kontraktu frontu nikdo jiný nepoužívá. */
    if (q->free_elem != NULL) {
        for (size_t i = 0; i < q->count; i++)
            q->free_elem(slot(q, (q->head + i) % q->capacity));
    }

    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->buf);
    free(q);
}

int kivfs_queue_push(struct kivfs_queue *q, const void *elem)
{
    pthread_mutex_lock(&q->lock);

    /* WHILE, ne if: cond_wait se může probudit i bez signálu (spurious
     * wakeup) a uvolněné místo mezitím mohl zabrat jiný producent.
     * Podmínku je proto po každém probuzení nutné ověřit znovu. */
    while (q->count == q->capacity && !q->shutdown)
        pthread_cond_wait(&q->not_full, &q->lock);   /* atomicky: pustí zámek a uspí */

    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return KIVFS_ESHUTDOWN;
    }

    put_locked(q, elem);
    pthread_mutex_unlock(&q->lock);
    return KIVFS_OK;
}

int kivfs_queue_try_push(struct kivfs_queue *q, const void *elem)
{
    pthread_mutex_lock(&q->lock);

    int rc;
    if (q->shutdown) {
        rc = KIVFS_ESHUTDOWN;
    } else if (q->count == q->capacity) {
        rc = KIVFS_EAGAIN;              /* plná – nečekat, rozhodne volající */
    } else {
        put_locked(q, elem);
        rc = KIVFS_OK;
    }

    pthread_mutex_unlock(&q->lock);
    return rc;
}

int kivfs_queue_pop(struct kivfs_queue *q, void *out)
{
    pthread_mutex_lock(&q->lock);

    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->lock);

    /* Po shutdown se pokračuje, dokud ve frontě něco je (drain). Konec
     * hlásíme až ve chvíli, kdy je fronta prázdná A ukončená. */
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return KIVFS_ESHUTDOWN;
    }

    memcpy(out, slot(q, q->head), q->elem_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);  /* uvolnilo se jedno místo */

    pthread_mutex_unlock(&q->lock);
    return KIVFS_OK;
}

void kivfs_queue_shutdown(struct kivfs_queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    /* BROADCAST: vzbudit VŠECHNA čekající vlákna na obou stranách.
     * Se signal by se vzbudilo jen jedno a ostatní by spala navždy. */
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}
