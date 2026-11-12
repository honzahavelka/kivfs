/*
 * Testy fronty: jednovláknové chování, ukončení (shutdown) a stresový
 * test s více producenty a konzumenty současně. Pouštět i pod
 * ThreadSanitizerem (-fsanitize=thread), který odhalí datové souběhy.
 */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "errors.h"
#include "queue.h"

static atomic_int g_checks;
static atomic_int g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

#define CHECK_RC(expr, expected)                                           \
    do {                                                                   \
        int rc_ = (expr);                                                  \
        g_checks++;                                                        \
        if (rc_ != (expected)) {                                           \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s -> %d (%s), expected %s\n",   \
                    __FILE__, __LINE__, #expr, rc_, kivfs_strerror(rc_),   \
                    #expected);                                            \
        }                                                                  \
    } while (0)

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

/* ------------------------------------------------------------------ */

static void test_invalid_args(void)
{
    struct kivfs_queue *q = (struct kivfs_queue *)1;
    CHECK_RC(kivfs_queue_create(&q, 0, 4, NULL), KIVFS_EINVAL);
    CHECK(q == NULL);
    CHECK_RC(kivfs_queue_create(&q, 4, 0, NULL), KIVFS_EINVAL);
    CHECK_RC(kivfs_queue_create(&q, SIZE_MAX / 2, 4, NULL), KIVFS_EINVAL);
    kivfs_queue_destroy(NULL);              /* nesmí spadnout */
}

static void test_fifo_and_full(void)
{
    struct kivfs_queue *q;
    int v;
    CHECK_RC(kivfs_queue_create(&q, 3, sizeof(int), NULL), KIVFS_OK);

    for (v = 1; v <= 3; v++)
        CHECK_RC(kivfs_queue_try_push(q, &v), KIVFS_OK);
    v = 4;
    CHECK_RC(kivfs_queue_try_push(q, &v), KIVFS_EAGAIN);   /* plná, nečeká */

    CHECK_RC(kivfs_queue_pop(q, &v), KIVFS_OK);
    CHECK(v == 1);
    v = 4;
    CHECK_RC(kivfs_queue_push(q, &v), KIVFS_OK);           /* místo je -> nečeká */

    /* Přes konec bufferu (index se "otočí" na začátek) – pořadí musí sedět. */
    for (int expected = 2; expected <= 4; expected++) {
        CHECK_RC(kivfs_queue_pop(q, &v), KIVFS_OK);
        CHECK(v == expected);
    }
    kivfs_queue_destroy(q);
}

static void test_shutdown_drains(void)
{
    struct kivfs_queue *q;
    int v;
    CHECK_RC(kivfs_queue_create(&q, 4, sizeof(int), NULL), KIVFS_OK);
    v = 10; CHECK_RC(kivfs_queue_push(q, &v), KIVFS_OK);
    v = 11; CHECK_RC(kivfs_queue_push(q, &v), KIVFS_OK);

    kivfs_queue_shutdown(q);
    kivfs_queue_shutdown(q);                               /* idempotentní */
    CHECK_RC(kivfs_queue_push(q, &v), KIVFS_ESHUTDOWN);
    CHECK_RC(kivfs_queue_try_push(q, &v), KIVFS_ESHUTDOWN);

    /* Zbylé položky se ještě vydají, pak teprve ESHUTDOWN. */
    CHECK_RC(kivfs_queue_pop(q, &v), KIVFS_OK);
    CHECK(v == 10);
    CHECK_RC(kivfs_queue_pop(q, &v), KIVFS_OK);
    CHECK(v == 11);
    CHECK_RC(kivfs_queue_pop(q, &v), KIVFS_ESHUTDOWN);
    kivfs_queue_destroy(q);
}

/* ---- shutdown musí probudit vlákna, která čekají ---- */

struct waiter_arg {
    struct kivfs_queue *q;
    int rc;
};

static void *blocked_pop(void *p)
{
    struct waiter_arg *a = p;
    int v;
    a->rc = kivfs_queue_pop(a->q, &v);
    return NULL;
}

static void *blocked_push(void *p)
{
    struct waiter_arg *a = p;
    int v = 1;
    a->rc = kivfs_queue_push(a->q, &v);
    return NULL;
}

static void test_shutdown_wakes_waiters(void)
{
    struct kivfs_queue *q;
    enum { N = 3 };
    pthread_t th[N];
    struct waiter_arg args[N];

    /* N konzumentů čeká na prázdné frontě. */
    CHECK_RC(kivfs_queue_create(&q, 1, sizeof(int), NULL), KIVFS_OK);
    for (int i = 0; i < N; i++) {
        args[i].q = q;
        args[i].rc = 1;
        pthread_create(&th[i], NULL, blocked_pop, &args[i]);
    }
    sleep_ms(50);                       /* ať opravdu usnou v cond_wait */
    kivfs_queue_shutdown(q);
    for (int i = 0; i < N; i++) {
        pthread_join(th[i], NULL);      /* bez broadcastu by tu test visel */
        CHECK_RC(args[i].rc, KIVFS_ESHUTDOWN);
    }
    kivfs_queue_destroy(q);

    /* Producent čeká na plné frontě. */
    int v = 0;
    CHECK_RC(kivfs_queue_create(&q, 1, sizeof(int), NULL), KIVFS_OK);
    CHECK_RC(kivfs_queue_push(q, &v), KIVFS_OK);
    args[0].q = q;
    args[0].rc = 1;
    pthread_create(&th[0], NULL, blocked_push, &args[0]);
    sleep_ms(50);
    kivfs_queue_shutdown(q);
    pthread_join(th[0], NULL);
    CHECK_RC(args[0].rc, KIVFS_ESHUTDOWN);
    kivfs_queue_destroy(q);
}

/* ---- stres: více producentů a konzumentů současně ---- */

enum { PRODUCERS = 4, CONSUMERS = 4, PER_PRODUCER = 50000, STRESS_CAP = 16 };

struct item {
    uint32_t producer;
    uint32_t seq;           /* pořadí u daného producenta: 0, 1, 2, ... */
};

static struct kivfs_queue *g_stress_q;
static atomic_uchar g_seen[PRODUCERS][PER_PRODUCER];   /* kolikrát položka prošla */

static void *stress_producer(void *p)
{
    uint32_t id = (uint32_t)(uintptr_t)p;
    for (uint32_t s = 0; s < PER_PRODUCER; s++) {
        struct item it = { id, s };
        /* Malá kapacita (16) -> producenti často čekají na místo. */
        CHECK_RC(kivfs_queue_push(g_stress_q, &it), KIVFS_OK);
    }
    return NULL;
}

static void *stress_consumer(void *p)
{
    (void)p;
    /* FIFO: jeden konzument musí od každého producenta vidět rostoucí seq
     * (mezi konzumenty se položky rozdělí, ale nesmí se v nich prohodit). */
    int64_t last_seq[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++)
        last_seq[i] = -1;

    struct item it;
    while (kivfs_queue_pop(g_stress_q, &it) == KIVFS_OK) {
        if (it.producer >= PRODUCERS || it.seq >= PER_PRODUCER) {
            CHECK(!"položka mimo rozsah");
            continue;
        }
        CHECK((int64_t)it.seq > last_seq[it.producer]);
        last_seq[it.producer] = it.seq;
        g_seen[it.producer][it.seq]++;
    }
    return NULL;
}

static void test_mpmc_stress(void)
{
    pthread_t prod[PRODUCERS], cons[CONSUMERS];
    CHECK_RC(kivfs_queue_create(&g_stress_q, STRESS_CAP, sizeof(struct item), NULL), KIVFS_OK);

    for (int i = 0; i < CONSUMERS; i++)
        pthread_create(&cons[i], NULL, stress_consumer, NULL);
    for (int i = 0; i < PRODUCERS; i++)
        pthread_create(&prod[i], NULL, stress_producer, (void *)(uintptr_t)i);

    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(prod[i], NULL);
    kivfs_queue_shutdown(g_stress_q);   /* konzumenti dočerpají zbytek a skončí */
    for (int i = 0; i < CONSUMERS; i++)
        pthread_join(cons[i], NULL);

    /* Každá položka právě jednou: nic se neztratilo ani nezdvojilo. */
    long missing = 0, duplicated = 0;
    for (int pi = 0; pi < PRODUCERS; pi++) {
        for (int s = 0; s < PER_PRODUCER; s++) {
            unsigned char n = g_seen[pi][s];
            if (n == 0) missing++;
            if (n > 1)  duplicated++;
        }
    }
    CHECK(missing == 0);
    CHECK(duplicated == 0);
    if (missing || duplicated)
        fprintf(stderr, "missing=%ld duplicated=%ld\n", missing, duplicated);

    kivfs_queue_destroy(g_stress_q);
}

/* ---- destroy uvolní obsah zbylých položek ---- */

struct owned { char *buf; };
static int g_freed;

static void free_owned(void *elem)
{
    struct owned *o = elem;
    free(o->buf);
    g_freed++;
}

static void test_destroy_frees_leftovers(void)
{
    struct kivfs_queue *q;
    CHECK_RC(kivfs_queue_create(&q, 4, sizeof(struct owned), free_owned), KIVFS_OK);
    for (int i = 0; i < 3; i++) {
        struct owned o = { malloc(32) };
        CHECK_RC(kivfs_queue_push(q, &o), KIVFS_OK);
    }
    struct owned taken;
    CHECK_RC(kivfs_queue_pop(q, &taken), KIVFS_OK);   /* tahle je teď naše */
    free(taken.buf);

    g_freed = 0;
    kivfs_queue_destroy(q);             /* zbylé 2 uvolní free_owned */
    CHECK(g_freed == 2);                /* LeakSanitizer ověří, že nic neuniklo */
}

int main(void)
{
    test_invalid_args();
    test_fifo_and_full();
    test_shutdown_drains();
    test_shutdown_wakes_waiters();
    test_mpmc_stress();
    test_destroy_frees_leftovers();

    printf("%d checks, %d failed\n", (int)g_checks, (int)g_failed);
    return g_failed == 0 ? 0 : 1;
}
