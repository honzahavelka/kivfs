/*
 * Testy pruhu Syncu (sync/lane.h) – bez sítě, přenositelné.
 *
 * Kromě jednotlivých pravidel (hodiny, pořadí, stabilita, duplicity,
 * limity) obsahuje SIMULACI celého hlasování (7.4.1 + Skeen): několik
 * uzlů, souběžná kola, zprávy doručované v náhodném pořadí (FIFO jen
 * v rámci jednoho spojení, jako TCP). Kontroluje, že všechny uzly
 * provedou stejné operace ve stejném pořadí se stejnými indexy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/errors.h"
#include "proto/syncproto.h"
#include "sync/lane.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

static const struct kivfs_lane_limits LIM = { 16, 1024 };
static const uint8_t PL[4] = { 1, 2, 3, 4 };

static struct kivfs_lane *mk(uint32_t lane, uint32_t node)
{
    struct kivfs_lane *l = NULL;
    CHECK(kivfs_lane_create(&l, lane, node, 100 + node, &LIM) == KIVFS_OK);
    return l;
}

static struct kivfs_op_id opid(uint32_t node, uint64_t seq)
{
    struct kivfs_op_id o = { node, 100 + node, seq };
    return o;
}

static void test_create(void)
{
    struct kivfs_lane *l;
    struct kivfs_lane_limits z = { 0, 10 }, zb = { 4, 0 };
    CHECK(kivfs_lane_create(&l, KIVFS_LANE_COUNT, 1, 1, &LIM) == KIVFS_EINVAL && l == NULL);
    CHECK(kivfs_lane_create(&l, KIVFS_LANE_DB, 1, 1, &z) == KIVFS_EINVAL);
    CHECK(kivfs_lane_create(&l, KIVFS_LANE_DB, 1, 1, &zb) == KIVFS_EINVAL);
    CHECK(kivfs_lane_create(&l, KIVFS_LANE_CLI, 1, 1, &zb) == KIVFS_OK);  /* CLI nemá bajty */
    CHECK(kivfs_lane_clock(l) == 0 && kivfs_lane_count(l) == 0 && kivfs_lane_next(l) == NULL);
    kivfs_lane_destroy(l);
    kivfs_lane_destroy(NULL);
}

/* Pravidla hodin: Ttmp = T + 1; příjemce max(t_in, T + 1); commit posune
 * hodiny i pro neznámou operaci. */
static void test_clock(void)
{
    struct kivfs_lane *l = mk(KIVFS_LANE_CLI, 1);
    struct kivfs_lane_entry *e;
    struct kivfs_op_id o2 = opid(2, 1), o3 = opid(3, 1);
    uint64_t t;

    CHECK(kivfs_lane_begin(l, NULL, 0, NULL, &e) == KIVFS_OK);
    CHECK(e->t == 1 && e->t_proposed == 1 && e->mine && e->op.node == 1
          && e->op.incarnation == 101 && e->op.seq == 1 && kivfs_lane_clock(l) == 1);

    CHECK(kivfs_lane_on_propose(l, &o2, 10, NULL, 0, &t) == KIVFS_OK && t == 10);  /* t_in > T */
    CHECK(kivfs_lane_on_propose(l, &o3, 4, NULL, 0, &t) == KIVFS_OK && t == 11);   /* t_in <= T */
    CHECK(kivfs_lane_clock(l) == 11);
    CHECK(kivfs_lane_count(l) == 1);           /* CLI: cizí se neřadí */

    /* COMMIT cizí operace, kterou CLI nedrží: ENOENT, ale hodiny ano */
    CHECK(kivfs_lane_commit(l, &o2, 20) == KIVFS_ENOENT && kivfs_lane_clock(l) == 20);
    CHECK(kivfs_lane_commit(l, &o2, 5) == KIVFS_ENOENT && kivfs_lane_clock(l) == 20);

    /* další vlastní návrh je nad vším, co uzel viděl */
    CHECK(kivfs_lane_begin(l, NULL, 0, NULL, &e) == KIVFS_OK && e->t == 21 && e->op.seq == 2);

    /* payload v CLI je chyba; vlastní op_id v on_propose také */
    CHECK(kivfs_lane_begin(l, PL, 4, NULL, &e) == KIVFS_EINVAL);
    struct kivfs_op_id me = opid(1, 9);
    CHECK(kivfs_lane_on_propose(l, &me, 1, NULL, 0, &t) == KIVFS_EINVAL);
    kivfs_lane_destroy(l);
}

/* Pořadí, přeřazení při COMMIT, stabilita hlavy, sériové provádění, indexy. */
static void test_order_and_stability(void)
{
    struct kivfs_lane *l = mk(KIVFS_LANE_DB, 1);
    struct kivfs_lane_entry *a, *e;
    struct kivfs_op_id b = opid(2, 1), c = opid(3, 1);
    uint64_t tb, tc;
    int ua = 7;

    CHECK(kivfs_lane_begin(l, NULL, 0, NULL, &a) == KIVFS_EINVAL);   /* DB bez payloadu */
    CHECK(kivfs_lane_begin(l, PL, 4, &ua, &a) == KIVFS_OK && a->t == 1);
    CHECK(kivfs_lane_on_propose(l, &b, 1, PL, 2, &tb) == KIVFS_OK && tb == 2);
    CHECK(kivfs_lane_on_propose(l, &c, 2, PL, 3, &tc) == KIVFS_OK && tc == 3);
    CHECK(kivfs_lane_count(l) == 3);
    CHECK(kivfs_lane_find(l, &b) != NULL && kivfs_lane_find(l, &b)->payload_len == 2
          && memcmp(kivfs_lane_find(l, &b)->payload, PL, 2) == 0);

    /* fronta: a(1,P) b(2,P) c(3,P); potvrdím c -> hlava a je pořád P */
    CHECK(kivfs_lane_commit(l, &c, 3) == KIVFS_OK);
    CHECK(kivfs_lane_next(l) == NULL);

    /* a dostane T_final 5 -> posune se za c: b(2,P) c(3,C) a(5,C) */
    CHECK(kivfs_lane_commit(l, &a->op, 5) == KIVFS_OK && a->t == 5 && kivfs_lane_clock(l) == 5);
    CHECK(kivfs_lane_next(l) == NULL);           /* hlava b čeká */

    /* b potvrdím s 3 – stejné t jako c, rozhoduje node (2 < 3) */
    CHECK(kivfs_lane_commit(l, &b, 3) == KIVFS_OK);
    e = kivfs_lane_next(l);
    CHECK(e != NULL && kivfs_op_id_eq(&e->op, &b) && e->index == 1 && e->state == KIVFS_LANE_EXECUTING);
    CHECK(kivfs_lane_next(l) == NULL);           /* sériově */
    CHECK(kivfs_lane_finish(l, e) == NULL && kivfs_lane_last_index(l) == 1);

    e = kivfs_lane_next(l);
    CHECK(e != NULL && kivfs_op_id_eq(&e->op, &c) && e->index == 2);
    CHECK(kivfs_lane_finish(l, e) == NULL);
    e = kivfs_lane_next(l);
    CHECK(e != NULL && e->mine && e->index == 3);
    CHECK(kivfs_lane_finish(l, e) == &ua);       /* u původce vrátí kontext */
    CHECK(kivfs_lane_next(l) == NULL && kivfs_lane_count(l) == 0 && kivfs_lane_last_index(l) == 3);
    CHECK(kivfs_lane_finish(l, NULL) == NULL);
    kivfs_lane_destroy(l);
}

/* Duplicitní PROPOSE, porušení protokolu, abort. */
static void test_dup_proto_abort(void)
{
    struct kivfs_lane *l = mk(KIVFS_LANE_DB, 1);
    struct kivfs_lane_entry *a, *e;
    struct kivfs_op_id b = opid(2, 1), x = opid(9, 9);
    uint64_t t;
    void *u;
    int ua;

    CHECK(kivfs_lane_on_propose(l, &b, 5, PL, 4, &t) == KIVFS_OK && t == 5);
    CHECK(kivfs_lane_on_propose(l, &b, 50, PL, 4, &t) == KIVFS_OK && t == 5);  /* stejná odpověď */
    CHECK(kivfs_lane_clock(l) == 5 && kivfs_lane_count(l) == 1);

    CHECK(kivfs_lane_commit(l, &b, 4) == KIVFS_EPROTO);          /* pod návrhem */
    CHECK(kivfs_lane_find(l, &b) != NULL && kivfs_lane_find(l, &b)->state == KIVFS_LANE_PROPOSED);
    CHECK(kivfs_lane_commit(l, &b, 6) == KIVFS_OK);
    CHECK(kivfs_lane_commit(l, &b, 6) == KIVFS_OK);              /* opakovaný COMMIT */
    CHECK(kivfs_lane_commit(l, &b, 7) == KIVFS_EPROTO);          /* jiné T_final */
    CHECK(kivfs_lane_abort(l, &b, &u) == KIVFS_EPROTO && u == NULL);
    CHECK(kivfs_lane_abort(l, &x, &u) == KIVFS_ENOENT);
    CHECK(kivfs_lane_commit(l, &x, 3) == KIVFS_ENOENT);

    CHECK(kivfs_lane_begin(l, PL, 4, &ua, &a) == KIVFS_OK && a->t == 8);
    CHECK(kivfs_lane_abort(l, &a->op, &u) == KIVFS_OK && u == &ua);
    CHECK(kivfs_lane_count(l) == 1);

    e = kivfs_lane_next(l);
    CHECK(e != NULL && kivfs_lane_commit(l, &b, 6) == KIVFS_OK);   /* i během EXECUTING */
    CHECK(kivfs_lane_abort(l, &b, NULL) == KIVFS_EPROTO);
    CHECK(kivfs_lane_set_last_index(l, 10) == KIVFS_EINVAL);
    kivfs_lane_finish(l, e);
    CHECK(kivfs_lane_set_last_index(l, 10) == KIVFS_OK);
    CHECK(kivfs_lane_on_propose(l, &x, 1, PL, 1, &t) == KIVFS_OK);
    CHECK(kivfs_lane_commit(l, &x, t) == KIVFS_OK);
    e = kivfs_lane_next(l);
    CHECK(e != NULL && e->index == 11);
    kivfs_lane_finish(l, e);
    kivfs_lane_destroy(l);
}

/* HOLD: COMMITTED hlava s hold > 0 se nevydá a blokuje i položky za ní;
 * set_last_index jde nastavit s neprázdnou frontou, ne během EXECUTING. */
static void test_hold(void)
{
    struct kivfs_lane *l = mk(KIVFS_LANE_DB, 1);
    struct kivfs_lane_entry *a, *e;
    struct kivfs_op_id x = opid(2, 1), nic = opid(3, 9);
    uint64_t t;

    /* po startu: cizí PROPOSE dorazí dřív než head z DB */
    CHECK(kivfs_lane_on_propose(l, &x, 5, PL, 1, &t) == KIVFS_OK && t == 5);
    CHECK(kivfs_lane_set_last_index(l, 7) == KIVFS_OK && kivfs_lane_last_index(l) == 7);

    CHECK(kivfs_lane_begin(l, PL, 4, NULL, &a) == KIVFS_OK && a->t == 6);
    CHECK(kivfs_lane_hold(l, &a->op, 2) == KIVFS_OK && a->hold == 2);   /* i PROPOSED */
    CHECK(kivfs_lane_hold(l, &nic, 1) == KIVFS_ENOENT);
    CHECK(kivfs_lane_release(l, &nic) == KIVFS_ENOENT);
    CHECK(kivfs_lane_commit(l, &a->op, 4 + 2) == KIVFS_OK);
    CHECK(kivfs_lane_commit(l, &x, 9) == KIVFS_OK);                     /* x za a */
    CHECK(kivfs_lane_next(l) == NULL);           /* a drží hold, x za ní čeká */
    CHECK(kivfs_lane_release(l, &a->op) == KIVFS_OK && a->hold == 1);
    CHECK(kivfs_lane_next(l) == NULL);
    CHECK(kivfs_lane_release(l, &a->op) == KIVFS_OK && a->hold == 0);
    CHECK(kivfs_lane_release(l, &a->op) == KIVFS_OK && a->hold == 0);   /* pod 0 ne */
    e = kivfs_lane_next(l);
    CHECK(e == a && e->index == 8);
    CHECK(kivfs_lane_hold(l, &a->op, 1) == KIVFS_EPROTO);               /* už EXECUTING */
    CHECK(kivfs_lane_set_last_index(l, 1) == KIVFS_EINVAL);
    kivfs_lane_finish(l, e);
    e = kivfs_lane_next(l);
    CHECK(e != NULL && kivfs_op_id_eq(&e->op, &x) && e->index == 9);
    kivfs_lane_finish(l, e);
    kivfs_lane_destroy(l);
}

/* Limity: plno -> EAGAIN a hodiny se neposunou. */
static void test_limits(void)
{
    struct kivfs_lane_limits lim = { 3, 10 };
    struct kivfs_lane *l;
    struct kivfs_lane_entry *e;
    uint8_t big[8] = { 0 };
    uint64_t t;

    CHECK(kivfs_lane_create(&l, KIVFS_LANE_DB, 1, 1, &lim) == KIVFS_OK);
    CHECK(kivfs_lane_begin(l, big, 8, NULL, &e) == KIVFS_OK);
    struct kivfs_op_id o = opid(2, 1);
    CHECK(kivfs_lane_on_propose(l, &o, 100, big, 3, &t) == KIVFS_EAGAIN);  /* 8 + 3 > 10 */
    CHECK(kivfs_lane_clock(l) == 1);
    CHECK(kivfs_lane_on_propose(l, &o, 100, big, 2, &t) == KIVFS_OK && t == 100);
    CHECK(kivfs_lane_begin(l, big, 0 + 1, NULL, &e) == KIVFS_EAGAIN);       /* bajty */
    kivfs_lane_destroy(l);

    lim.max_bytes = 1000;
    CHECK(kivfs_lane_create(&l, KIVFS_LANE_DB, 1, 1, &lim) == KIVFS_OK);
    for (int i = 0; i < 3; i++)
        CHECK(kivfs_lane_begin(l, big, 1, NULL, &e) == KIVFS_OK);
    CHECK(kivfs_lane_begin(l, big, 1, NULL, &e) == KIVFS_EAGAIN && e == NULL);
    CHECK(kivfs_lane_clock(l) == 3);
    /* uvolnění místa abortem */
    struct kivfs_op_id first = opid(1, 1);
    first.incarnation = 1;
    CHECK(kivfs_lane_abort(l, &first, NULL) == KIVFS_OK);
    CHECK(kivfs_lane_begin(l, big, 1, NULL, &e) == KIVFS_OK && e->t == 4);
    kivfs_lane_destroy(l);
}

static int count_cb(struct kivfs_lane_entry *e, void *arg)
{
    (void)e;
    ++*(int *)arg;
    return 0;
}

/* drop_origin: jen PROPOSED od staré incarnation uzlu. */
static void test_drop_origin(void)
{
    struct kivfs_lane *l = mk(KIVFS_LANE_DB, 1);
    struct kivfs_op_id old1 = { 2, 5, 1 }, old2 = { 2, 5, 2 }, cur = { 2, 6, 1 }, other = { 3, 5, 1 };
    uint64_t t;
    int n = 0;

    CHECK(kivfs_lane_on_propose(l, &old1, 1, PL, 1, &t) == KIVFS_OK);
    CHECK(kivfs_lane_on_propose(l, &old2, 1, PL, 1, &t) == KIVFS_OK);
    CHECK(kivfs_lane_on_propose(l, &cur, 1, PL, 1, &t) == KIVFS_OK);
    CHECK(kivfs_lane_on_propose(l, &other, 1, PL, 1, &t) == KIVFS_OK);
    CHECK(kivfs_lane_commit(l, &old2, 10) == KIVFS_OK);
    CHECK(kivfs_lane_drop_origin(l, 2, 6) == 1);              /* old1; old2 je COMMITTED */
    CHECK(kivfs_lane_find(l, &old1) == NULL && kivfs_lane_find(l, &old2) != NULL);
    CHECK(kivfs_lane_find(l, &cur) != NULL && kivfs_lane_find(l, &other) != NULL);
    kivfs_lane_foreach(l, count_cb, &n);
    CHECK(n == 3 && kivfs_lane_count(l) == 3);
    kivfs_lane_destroy(l);
}

/* ------------------------------------------------------------------ */
/* Simulace hlasování                                                  */
/* ------------------------------------------------------------------ */

#define SIM_NODES  3
#define SIM_OPS    40
#define SIM_MSGS   4096

enum { M_PROPOSE, M_REPLY, M_COMMIT, M_ACK };

struct msg { int type; struct kivfs_op_id op; uint64_t t; };

/* FIFO fronta zpráv pro každé spojení from -> to */
struct link { struct msg m[SIM_MSGS]; int head, tail; };

struct round { struct kivfs_op_id op; int origin; int replies; uint64_t tmax; int used; };

struct sim {
    struct kivfs_lane *lane[SIM_NODES];
    struct link link[SIM_NODES][SIM_NODES];
    struct round rounds[SIM_OPS];
    int nrounds;
    struct kivfs_op_id done[SIM_NODES][SIM_OPS];
    uint64_t done_idx[SIM_NODES][SIM_OPS];
    int ndone[SIM_NODES];
    unsigned rng;
};

static unsigned rnd(struct sim *s)
{
    s->rng = s->rng * 1103515245u + 12345u;
    return (s->rng >> 16) & 0x7FFF;
}

static void send_msg(struct sim *s, int from, int to, int type, const struct kivfs_op_id *op, uint64_t t)
{
    struct link *k = &s->link[from][to];
    struct msg *m = &k->m[k->tail++];
    m->type = type; m->op = *op; m->t = t;
}

static void drain(struct sim *s, int node)
{
    struct kivfs_lane_entry *e;
    while ((e = kivfs_lane_next(s->lane[node])) != NULL) {
        s->done[node][s->ndone[node]] = e->op;
        s->done_idx[node][s->ndone[node]] = e->index;
        s->ndone[node]++;
        kivfs_lane_finish(s->lane[node], e);
    }
}

static void deliver(struct sim *s, int from, int to, const struct msg *m)
{
    uint64_t t;
    switch (m->type) {
    case M_PROPOSE:
        CHECK(kivfs_lane_on_propose(s->lane[to], &m->op, m->t, PL, 1, &t) == KIVFS_OK);
        send_msg(s, to, from, M_REPLY, &m->op, t);
        break;
    case M_REPLY:
        for (int r = 0; r < s->nrounds; r++) {
            struct round *rd = &s->rounds[r];
            if (rd->used && kivfs_op_id_eq(&rd->op, &m->op)) {
                if (m->t > rd->tmax)
                    rd->tmax = m->t;
                if (++rd->replies == SIM_NODES - 1) {          /* všichni odpověděli */
                    /* bod 13 a: provést až po potvrzení COMMIT od všech */
                    CHECK(kivfs_lane_hold(s->lane[to], &rd->op, SIM_NODES - 1) == KIVFS_OK);
                    CHECK(kivfs_lane_commit(s->lane[to], &rd->op, rd->tmax) == KIVFS_OK);
                    for (int n = 0; n < SIM_NODES; n++)
                        if (n != to)
                            send_msg(s, to, n, M_COMMIT, &rd->op, rd->tmax);
                    rd->used = 0;
                }
            }
        }
        break;
    case M_COMMIT:
        CHECK(kivfs_lane_commit(s->lane[to], &m->op, m->t) == KIVFS_OK);
        send_msg(s, to, from, M_ACK, &m->op, 0);
        break;
    case M_ACK: {
        /* u původce: vlastní operace se nesmí provést dřív, než ji
         * potvrdí všichni (hold) */
        const struct kivfs_lane_entry *e = kivfs_lane_find(s->lane[to], &m->op);
        CHECK(e != NULL && e->hold > 0 && e->state == KIVFS_LANE_COMMITTED);
        CHECK(kivfs_lane_release(s->lane[to], &m->op) == KIVFS_OK);
        break;
    }
    }
    drain(s, to);
}

static void run_sim(unsigned seed)
{
    static struct sim s;
    struct kivfs_lane_limits lim = { SIM_OPS * 2, 1u << 20 };
    memset(&s, 0, sizeof s);
    s.rng = seed;
    for (int n = 0; n < SIM_NODES; n++)
        CHECK(kivfs_lane_create(&s.lane[n], KIVFS_LANE_DB, (uint32_t)n + 1, 1, &lim) == KIVFS_OK);

    int started = 0;
    for (;;) {
        /* buď začni novou operaci na náhodném uzlu, nebo doruč zprávu
         * z náhodného neprázdného spojení */
        int pending = 0;
        for (int a = 0; a < SIM_NODES; a++)
            for (int b = 0; b < SIM_NODES; b++)
                pending += s.link[a][b].tail - s.link[a][b].head;
        if (pending == 0 && started == SIM_OPS)
            break;

        if (started < SIM_OPS && (pending == 0 || rnd(&s) % 4 == 0)) {
            int n = (int)(rnd(&s) % SIM_NODES);
            struct kivfs_lane_entry *e;
            CHECK(kivfs_lane_begin(s.lane[n], PL, 1, NULL, &e) == KIVFS_OK);
            struct round *rd = &s.rounds[s.nrounds++];
            rd->op = e->op; rd->origin = n; rd->replies = 0; rd->tmax = e->t; rd->used = 1;
            for (int b = 0; b < SIM_NODES; b++)
                if (b != n)
                    send_msg(&s, n, b, M_PROPOSE, &e->op, e->t);
            started++;
            continue;
        }
        int pick = (int)(rnd(&s) % (unsigned)pending);
        for (int a = 0; a < SIM_NODES; a++)
            for (int b = 0; b < SIM_NODES; b++) {
                struct link *k = &s.link[a][b];
                int c = k->tail - k->head;
                if (pick >= 0 && pick < c) {
                    struct msg m = k->m[k->head++];
                    deliver(&s, a, b, &m);
                    pick = -1;
                } else if (pick >= 0) {
                    pick -= c;
                }
            }
    }

    /* všechny uzly provedly všechno, ve stejném pořadí a se stejnými indexy */
    int same = 1;
    for (int n = 0; n < SIM_NODES; n++) {
        CHECK(s.ndone[n] == SIM_OPS);
        CHECK(kivfs_lane_count(s.lane[n]) == 0 && kivfs_lane_last_index(s.lane[n]) == SIM_OPS);
        for (int i = 0; i < s.ndone[n] && i < s.ndone[0]; i++)
            if (!kivfs_op_id_eq(&s.done[n][i], &s.done[0][i]) || s.done_idx[n][i] != (uint64_t)i + 1)
                same = 0;
    }
    CHECK(same);
    if (!same)
        fprintf(stderr, "     seed %u: uzly provedly operace v různém pořadí\n", seed);
    for (int n = 0; n < SIM_NODES; n++)
        kivfs_lane_destroy(s.lane[n]);
}

/* Protipříklad k pravidlu „commit posune hodiny“: bez něj by uzel mohl
 * navrhnout operaci s t menším než už provedená. Tady jen ověříme, že
 * návrh po COMMIT(T_final) je > T_final, i když uzel kolo neviděl. */
static void test_commit_clock_guard(void)
{
    struct kivfs_lane *l = mk(KIVFS_LANE_DB, 3);
    struct kivfs_lane_entry *e;
    struct kivfs_op_id x = opid(1, 1);
    uint64_t t;

    CHECK(kivfs_lane_on_propose(l, &x, 2, PL, 1, &t) == KIVFS_OK && t == 2);
    CHECK(kivfs_lane_commit(l, &x, 10) == KIVFS_OK);          /* T_final od jiných uzlů */
    CHECK(kivfs_lane_begin(l, PL, 1, NULL, &e) == KIVFS_OK && e->t == 11);
    kivfs_lane_destroy(l);
}

int main(void)
{
    test_create();
    test_clock();
    test_order_and_stability();
    test_dup_proto_abort();
    test_hold();
    test_limits();
    test_drop_origin();
    test_commit_clock_guard();
    for (unsigned seed = 1; seed <= 300; seed++)
        run_sim(seed);
    printf("lane: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
