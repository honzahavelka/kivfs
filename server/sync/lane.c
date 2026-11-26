/*
 * KIVFS – pruh Syncu: Lamportovy hodiny + seřazená fronta (implementace),
 * viz lane.h
 *
 * Fronta = obousměrný seznam seřazený podle klíče (t, op_id) nad pevným
 * poolem položek (alokovaný při create → ukazatele na položky jsou
 * stabilní a za běhu se alokují jen kopie payloadů). Vkládá se od konce:
 * nové návrhy mají největší t (T roste), takže je vložení obvykle O(1);
 * hledání podle op_id je lineární – při max_entries v řádu stovek to
 * nevadí a kód zůstává jednoduchý.
 */
#include "sync/lane.h"
#include "proto/errors.h"

#include <stdlib.h>
#include <string.h>

struct lnode {
    struct kivfs_lane_entry e;      /* musí být první: entry <-> lnode */
    struct lnode *prev, *next;
};

struct kivfs_lane {
    uint32_t lane;
    uint32_t self_node;
    uint64_t self_incarnation;
    struct kivfs_lane_limits lim;

    uint64_t T;                     /* Lamportovy hodiny pruhu */
    uint64_t seq;                   /* poslední přidělené seq vlastní operace */
    uint64_t last_index;            /* pruh DB: poslední provedená operace */

    struct lnode *pool;             /* lim.max_entries položek */
    struct lnode *free_list;        /* volné položky (přes next) */
    struct lnode *head, *tail;      /* fronta v pořadí klíče */
    size_t   count;
    uint64_t bytes;                 /* součet payload_len */
    int      executing;             /* 1 = hlava je EXECUTING */
};

static struct lnode *as_node(struct kivfs_lane_entry *e)
{
    return (struct lnode *)(void *)e;
}

/* ------------------------------------------------------------------ */
/* Seznam                                                              */
/* ------------------------------------------------------------------ */

static int key_cmp(const struct lnode *a, const struct lnode *b)
{
    return kivfs_sync_key_cmp(a->e.t, &a->e.op, b->e.t, &b->e.op);
}

static void list_unlink(struct kivfs_lane *l, struct lnode *n)
{
    if (n->prev) n->prev->next = n->next; else l->head = n->next;
    if (n->next) n->next->prev = n->prev; else l->tail = n->prev;
    n->prev = n->next = NULL;
}

/* Vloží n na místo podle klíče; hledá od konce (viz komentář nahoře). */
static void list_insert(struct kivfs_lane *l, struct lnode *n)
{
    struct lnode *after = l->tail;
    while (after != NULL && key_cmp(after, n) > 0)
        after = after->prev;
    n->prev = after;
    n->next = after ? after->next : l->head;
    if (n->next) n->next->prev = n; else l->tail = n;
    if (after)   after->next = n;   else l->head = n;
}

/* ------------------------------------------------------------------ */
/* Položky                                                             */
/* ------------------------------------------------------------------ */

/* Kontrola payloadu podle pruhu: DB ho musí mít, CLI nesmí. */
static int payload_ok(const struct kivfs_lane *l, const uint8_t *payload, uint32_t len)
{
    if (l->lane == KIVFS_LANE_DB)
        return payload != NULL && len > 0;
    return payload == NULL && len == 0;
}

/* Rezervuje položku a zkopíruje payload; do fronty ji NEvkládá.
 * KIVFS_EAGAIN (limity), KIVFS_ENOMEM. Hodiny nemění. */
static int node_alloc(struct kivfs_lane *l, const uint8_t *payload, uint32_t len,
                      struct lnode **out)
{
    if (l->free_list == NULL)
        return KIVFS_EAGAIN;
    if (len > 0 && l->bytes + len > l->lim.max_bytes)
        return KIVFS_EAGAIN;

    uint8_t *copy = NULL;
    if (len > 0) {
        copy = malloc(len);
        if (copy == NULL)
            return KIVFS_ENOMEM;
        memcpy(copy, payload, len);
    }
    struct lnode *n = l->free_list;
    l->free_list = n->next;
    memset(n, 0, sizeof *n);
    n->e.payload     = copy;
    n->e.payload_len = len;
    l->count++;
    l->bytes += len;
    *out = n;
    return KIVFS_OK;
}

/* Vyjme položku z fronty a vrátí ji do poolu. */
static void node_free(struct kivfs_lane *l, struct lnode *n)
{
    list_unlink(l, n);
    free((void *)n->e.payload);
    l->bytes -= n->e.payload_len;
    l->count--;
    memset(n, 0, sizeof *n);
    n->next = l->free_list;
    l->free_list = n;
}

static struct lnode *find_node(struct kivfs_lane *l, const struct kivfs_op_id *op)
{
    for (struct lnode *n = l->head; n != NULL; n = n->next)
        if (kivfs_op_id_eq(&n->e.op, op))
            return n;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Život                                                               */
/* ------------------------------------------------------------------ */

int kivfs_lane_create(struct kivfs_lane **out, uint32_t lane, uint32_t self_node,
                      uint64_t self_incarnation, const struct kivfs_lane_limits *lim)
{
    *out = NULL;
    if (lane >= KIVFS_LANE_COUNT || lim == NULL || lim->max_entries == 0
        || (lane == KIVFS_LANE_DB && lim->max_bytes == 0))
        return KIVFS_EINVAL;

    struct kivfs_lane *l = calloc(1, sizeof *l);
    if (l == NULL)
        return KIVFS_ENOMEM;
    l->pool = calloc(lim->max_entries, sizeof *l->pool);
    if (l->pool == NULL) {
        free(l);
        return KIVFS_ENOMEM;
    }
    l->lane             = lane;
    l->self_node        = self_node;
    l->self_incarnation = self_incarnation;
    l->lim              = *lim;
    for (uint32_t i = lim->max_entries; i-- > 0; ) {
        l->pool[i].next = l->free_list;
        l->free_list = &l->pool[i];
    }
    *out = l;
    return KIVFS_OK;
}

void kivfs_lane_destroy(struct kivfs_lane *l)
{
    if (l == NULL)
        return;
    for (struct lnode *n = l->head; n != NULL; n = n->next)
        free((void *)n->e.payload);
    free(l->pool);
    free(l);
}

/* ------------------------------------------------------------------ */
/* Hodiny a obnova                                                     */
/* ------------------------------------------------------------------ */

uint64_t kivfs_lane_clock(const struct kivfs_lane *l)      { return l->T; }
uint64_t kivfs_lane_last_index(const struct kivfs_lane *l) { return l->last_index; }

int kivfs_lane_set_last_index(struct kivfs_lane *l, uint64_t last_index)
{
    if (l->executing)
        return KIVFS_EINVAL;
    l->last_index = last_index;
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Kolo hlasování                                                      */
/* ------------------------------------------------------------------ */

int kivfs_lane_begin(struct kivfs_lane *l, const uint8_t *payload, uint32_t payload_len,
                     void *user, struct kivfs_lane_entry **out)
{
    struct lnode *n;
    int rc;

    *out = NULL;
    if (!payload_ok(l, payload, payload_len))
        return KIVFS_EINVAL;
    if ((rc = node_alloc(l, payload, payload_len, &n)) != KIVFS_OK)
        return rc;                      /* hodiny se neposunuly */

    l->T += 1;                          /* Ttmp = T + d, d = 1 */
    n->e.op.node        = l->self_node;
    n->e.op.incarnation = l->self_incarnation;
    n->e.op.seq         = ++l->seq;
    n->e.t              = l->T;
    n->e.t_proposed     = l->T;
    n->e.state          = KIVFS_LANE_PROPOSED;
    n->e.mine           = 1;
    n->e.user           = user;
    list_insert(l, n);
    *out = &n->e;
    return KIVFS_OK;
}

int kivfs_lane_on_propose(struct kivfs_lane *l, const struct kivfs_op_id *op, uint64_t t_in,
                          const uint8_t *payload, uint32_t payload_len, uint64_t *t_out)
{
    if (op->node == l->self_node || !payload_ok(l, payload, payload_len))
        return KIVFS_EINVAL;

    if (l->lane == KIVFS_LANE_DB) {
        struct lnode *dup = find_node(l, op);
        if (dup != NULL) {              /* opakování: stejná odpověď */
            *t_out = dup->e.t_proposed;
            return KIVFS_OK;
        }
    }

    struct lnode *n = NULL;
    if (l->lane == KIVFS_LANE_DB) {
        int rc = node_alloc(l, payload, payload_len, &n);
        if (rc != KIVFS_OK)
            return rc;                  /* hodiny se neposunuly */
    }

    /* 7.4.1: TmsgIn > T => TmsgIn, jinak T + d */
    uint64_t t = t_in > l->T ? t_in : l->T + 1;
    l->T = t;
    *t_out = t;

    if (n != NULL) {
        n->e.op         = *op;
        n->e.t          = t;
        n->e.t_proposed = t;
        n->e.state      = KIVFS_LANE_PROPOSED;
        list_insert(l, n);
    }
    return KIVFS_OK;
}

int kivfs_lane_commit(struct kivfs_lane *l, const struct kivfs_op_id *op, uint64_t t_final)
{
    /* vždy, i pro neznámou operaci – jinak by další návrh tohoto uzlu
     * mohl být menší než už provedené T_final */
    if (t_final > l->T)
        l->T = t_final;

    struct lnode *n = find_node(l, op);
    if (n == NULL)
        return KIVFS_ENOENT;
    if (n->e.state != KIVFS_LANE_PROPOSED)
        return n->e.t == t_final ? KIVFS_OK : KIVFS_EPROTO;
    if (t_final < n->e.t_proposed)
        return KIVFS_EPROTO;

    list_unlink(l, n);
    n->e.t     = t_final;
    n->e.state = KIVFS_LANE_COMMITTED;
    list_insert(l, n);
    return KIVFS_OK;
}

int kivfs_lane_abort(struct kivfs_lane *l, const struct kivfs_op_id *op, void **user_out)
{
    if (user_out != NULL)
        *user_out = NULL;
    struct lnode *n = find_node(l, op);
    if (n == NULL)
        return KIVFS_ENOENT;
    if (n->e.state != KIVFS_LANE_PROPOSED)
        return KIVFS_EPROTO;
    if (user_out != NULL)
        *user_out = n->e.user;
    node_free(l, n);
    return KIVFS_OK;
}

/* ------------------------------------------------------------------ */
/* Provádění                                                           */
/* ------------------------------------------------------------------ */

int kivfs_lane_hold(struct kivfs_lane *l, const struct kivfs_op_id *op, uint32_t n)
{
    struct lnode *x = find_node(l, op);
    if (x == NULL)
        return KIVFS_ENOENT;
    if (x->e.state == KIVFS_LANE_EXECUTING)
        return KIVFS_EPROTO;
    x->e.hold = n;
    return KIVFS_OK;
}

int kivfs_lane_release(struct kivfs_lane *l, const struct kivfs_op_id *op)
{
    struct lnode *x = find_node(l, op);
    if (x == NULL)
        return KIVFS_ENOENT;
    if (x->e.hold > 0)
        x->e.hold--;
    return KIVFS_OK;
}

struct kivfs_lane_entry *kivfs_lane_next(struct kivfs_lane *l)
{
    struct lnode *h = l->head;
    if (l->executing || h == NULL || h->e.state != KIVFS_LANE_COMMITTED || h->e.hold > 0)
        return NULL;
    h->e.state = KIVFS_LANE_EXECUTING;
    if (l->lane == KIVFS_LANE_DB)
        h->e.index = l->last_index + 1;
    l->executing = 1;
    return &h->e;
}

void *kivfs_lane_finish(struct kivfs_lane *l, struct kivfs_lane_entry *e)
{
    if (e == NULL || e->state != KIVFS_LANE_EXECUTING)
        return NULL;                    /* chyba volajícího */
    void *user = e->user;
    if (l->lane == KIVFS_LANE_DB)
        l->last_index = e->index;
    l->executing = 0;
    node_free(l, as_node(e));
    return user;
}

/* ------------------------------------------------------------------ */
/* Dotazy a úklid                                                      */
/* ------------------------------------------------------------------ */

struct kivfs_lane_entry *kivfs_lane_find(struct kivfs_lane *l, const struct kivfs_op_id *op)
{
    struct lnode *n = find_node(l, op);
    return n ? &n->e : NULL;
}

size_t kivfs_lane_count(const struct kivfs_lane *l) { return l->count; }

void kivfs_lane_foreach(struct kivfs_lane *l,
                        int (*cb)(struct kivfs_lane_entry *e, void *arg), void *arg)
{
    for (struct lnode *n = l->head; n != NULL; n = n->next)
        if (cb(&n->e, arg))
            return;
}

size_t kivfs_lane_drop_origin(struct kivfs_lane *l, uint32_t node, uint64_t current_incarnation)
{
    size_t dropped = 0;
    struct lnode *n = l->head;
    while (n != NULL) {
        struct lnode *next = n->next;
        if (n->e.state == KIVFS_LANE_PROPOSED && n->e.op.node == node
            && n->e.op.incarnation != current_incarnation) {
            node_free(l, n);
            dropped++;
        }
        n = next;
    }
    return dropped;
}
