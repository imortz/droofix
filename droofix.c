#include "droofix.h"
#include <time.h>

/* Reports what was being allocated and how large it had grown: on a big
 * proof the cause is almost always the sample size rather than a bug. */
void *droofix_alloc(void *ptr, size_t n, const char *what)
{
    void *q = realloc(ptr, n);
    if (!q && n) {
        fprintf(stderr,
            "\nError: out of memory growing %s to %.2f GB.\n"
            "       Lower --pick-k, or use a shorter prefix.\n",
            what, (double)n / (1 << 30));
        exit(2);
    }
    return q;
}

double droofix_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* ---------------------------------------------------------------- */
/*  Assignment helpers                                               */
/* ---------------------------------------------------------------- */

static inline bool lit_true(Solver *s, int l)
{
    return SIGN(l) ? s->val[VAR(l)] == LFALSE : s->val[VAR(l)] == LTRUE;
}

static inline bool lit_false(Solver *s, int l)
{
    return SIGN(l) ? s->val[VAR(l)] == LTRUE : s->val[VAR(l)] == LFALSE;
}

static inline bool lit_undef(Solver *s, int l)
{
    return s->val[VAR(l)] == UNDEF;
}

static void assign(Solver *s, int lit)
{
    s->val[VAR(lit)] = SIGN(lit) ? LFALSE : LTRUE;
    s->trail[s->trail_sz++] = lit;
}

/* Bring occ[] up to date with the trail by retiring sampled clauses that
 * the trail has satisfied. Only pick_var() reads occ[], so this is deferred
 * until a branching decision is about to be made: branches that end in a
 * conflict, and leaves at max_depth, never pay for it. */
static void occ_sync(Solver *s)
{
    for (; s->occ_head < s->trail_sz; s->occ_head++) {
        Vec *ol = &s->occ_list[s->trail[s->occ_head]];
        for (int i = 0; i < ol->sz; i++) {
            int pos = ol->d[i];
            if (!s->sample_active[pos]) continue;
            s->sample_active[pos] = false;
            s->sat_stk[s->sat_sz++] = pos;

            int cid = s->nformula + pos;
            arena_t *c = s->arena + s->cl_off[cid];
            int len = s->cl_len[cid];
            double w = 1.0 / len;
            for (int k = 0; k < len; k++) {
                s->occ[c[k]]--;
                s->occw[c[k]] -= w;
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/*  Unit propagation (two watched literals). 0 = ok, 1 = conflict.   */
/* ---------------------------------------------------------------- */

static int propagate(Solver *s)
{
    while (s->prop_head < s->trail_sz) {
        int  p    = s->trail[s->prop_head++];   /* p is true         */
        int  flit = NEG(p);                     /* flit is now false */
        Vec *ws   = &s->watches[flit];
        int  i, j;

        for (i = j = 0; i < ws->sz; i++) {
            int cid = ws->d[i];
            arena_t *c = s->arena + s->cl_off[cid];
            int len = s->cl_len[cid];

            /* Keep the false literal at c[1]. */
            if (c[0] == (arena_t)flit) {
                arena_t t = c[0]; c[0] = c[1]; c[1] = t;
            }

            if (lit_true(s, c[0])) {            /* already satisfied */
                ws->d[j++] = cid;
                continue;
            }

            int found = 0;
            for (int k = 2; k < len; k++) {     /* new watch for c[1] */
                if (!lit_false(s, c[k])) {
                    arena_t t = c[1]; c[1] = c[k]; c[k] = t;
                    vec_push(&s->watches[c[1]], cid);
                    found = 1;
                    break;
                }
            }
            if (found) continue;                /* left this watch list */

            ws->d[j++] = cid;

            if (lit_false(s, c[0])) {           /* conflict */
                for (++i; i < ws->sz; i++) ws->d[j++] = ws->d[i];
                ws->sz = j;
                return 1;
            }
            assign(s, c[0]);                    /* unit */
        }
        ws->sz = j;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/*  Variable selection                                               */
/* ---------------------------------------------------------------- */

/* Candidacy and scoring are separate on purpose. A variable is a candidate
 * iff occ+ + occ- > 0; the score only orders the candidates. Folding the
 * two together would let PICK_MIN and PICK_PROD, which are zero for a
 * one-sided variable, abandon a branch while usable splits remained. */
static int pick_var(Solver *s)
{
    int    best = 0, best_sum = 0;
    double best_score = -1.0;

    for (int v = 1; v <= s->nvars; v++) {
        if (s->val[v] != UNDEF) continue;

        /* Candidacy from the exact integer counts; the score may be weighted.
         * Keeping them apart preserves the existing invariant that a variable
         * appearing at all stays splittable even when its score is 0. */
        int ip = s->occ[POSLIT(v)], iq = s->occ[NEGLIT(v)];
        int sum = ip + iq;
        if (sum == 0) continue;
        double p = (double)ip, q = (double)iq;
        if (s->weight_mode == WEIGHT_INVLEN) {
            p = s->occw[POSLIT(v)];
            q = s->occw[NEGLIT(v)];
        }

        double score;
        switch (s->pick_mode) {
        case PICK_MIN:  score = (p < q ? p : q);          break;
        /* double, not int: occ reaches ~1e7 on large proofs and the product
         * then needs 47 bits. */
        case PICK_PROD: score = p * q;                    break;
        default:        score = p + q;                    break;
        }

        /* Ties break towards the larger total. This matters for PICK_MIN,
         * where min() collapses many variables onto the same value. */
        if (score > best_score || (score == best_score && sum > best_sum)) {
            best_score = score;
            best_sum   = sum;
            best       = v;
        }
    }
    return best;
}

/* ---------------------------------------------------------------- */
/*  Cube emission                                                    */
/* ---------------------------------------------------------------- */

/* Per-cube features as they stand when the cube is emitted, i.e. the only
 * signals available before the cube is solved. `reason` is 0 for a leaf at
 * max_depth and 1 for one with no candidate variables left. Costs an
 * O(nvars) scan, hence opt-in. */
static void emit_stats(Solver *s, int reason)
{
    int free_vars = 0, occ_sum = 0, occ_max = 0;
    for (int v = 1; v <= s->nvars; v++) {
        if (s->val[v] != UNDEF) continue;
        int tot = s->occ[POSLIT(v)] + s->occ[NEGLIT(v)];
        free_vars++;
        occ_sum += tot;
        if (tot > occ_max) occ_max = tot;
    }
    fprintf(s->stats_out, "%d\t%d\t%d\t%d\t%d\t%d\n",
            s->ndecs, reason, free_vars, s->pick_k - s->sat_sz,
            occ_sum, occ_max);
}

static void emit_cube(Solver *s, FILE *out, int reason)
{
    (void)out;                       /* written by solver_cube, at the end */
    long need = s->cube_buf_sz + s->ndecs + 1;
    if (need > s->cube_buf_cap) {
        long cap = s->cube_buf_cap ? s->cube_buf_cap : 4096;
        while (cap < need) cap *= 2;
        s->cube_buf = (int *)droofix_alloc(s->cube_buf,
                                         (size_t)cap * sizeof(int),
                                         "cube buffer");
        s->cube_buf_cap = cap;
    }
    for (int i = 0; i < s->ndecs; i++)
        s->cube_buf[s->cube_buf_sz++] = TODI(s->decs[i]);
    s->cube_buf[s->cube_buf_sz++] = 0;
    s->ncubes++;
    if (s->stats_out) emit_stats(s, reason);
}

/* ---------------------------------------------------------------- */
/*  DFS                                                              */
/* ---------------------------------------------------------------- */

static void dfs(Solver *s, int depth, FILE *out)
{
    if (propagate(s))
        return;                     /* conflict: branch pruned, no cube */

    if (depth >= s->max_depth) {
        if (s->stats_out) occ_sync(s);   /* only needed for occ_sum/occ_max */
        emit_cube(s, out, 0);
        return;
    }

    occ_sync(s);
    int var = pick_var(s);
    if (var == 0) {
        emit_cube(s, out, 1);
        return;
    }

    for (int sign = 0; sign <= 1; sign++) {
        int lit      = sign ? NEGLIT(var) : POSLIT(var);
        int sv_trail = s->trail_sz;
        int sv_sat   = s->sat_sz;

        s->decs[s->ndecs++] = lit;
        assign(s, lit);
        dfs(s, depth + 1, out);
        s->ndecs--;

        /* occ_sync ran before the decision was pushed, so occ_head was
         * exactly sv_trail then and can only have advanced since. */
        while (s->trail_sz > sv_trail)
            s->val[VAR(s->trail[--s->trail_sz])] = UNDEF;
        s->prop_head = sv_trail;
        s->occ_head  = sv_trail;

        while (s->sat_sz > sv_sat) {     /* reactivate satisfied slots */
            int pos = s->sat_stk[--s->sat_sz];
            s->sample_active[pos] = true;
            int cid = s->nformula + pos;
            arena_t *c = s->arena + s->cl_off[cid];
            int len = s->cl_len[cid];
            double w = 1.0 / len;
            for (int k = 0; k < len; k++) {
                s->occ[c[k]]++;
                s->occw[c[k]] += w;
            }
        }
    }
}

/* ================================================================ */
/*  Public API                                                       */
/* ================================================================ */

Solver *solver_new(void)
{
    Solver *s = (Solver *)droofix_alloc(NULL, sizeof(Solver), "solver state");
    memset(s, 0, sizeof(Solver));
    s->arena_cap = 1 << 16;
    s->cl_cap    = 1 << 12;
    s->arena  = (arena_t *)droofix_alloc(NULL, (size_t)s->arena_cap * sizeof(arena_t), "clause arena");
    s->cl_off = (int64_t *)droofix_alloc(NULL, (size_t)s->cl_cap * sizeof(int64_t), "clause offsets");
    s->cl_len = (uint16_t *)droofix_alloc(NULL, (size_t)s->cl_cap * sizeof(uint16_t), "clause lengths");
    return s;
}

void solver_free(Solver *s)
{
    free(s->cube_buf);
    free(s->arena);
    free(s->cl_off);
    free(s->cl_len);
    if (s->watches) {
        for (int i = 0; i < s->nlits; i++) vec_free(&s->watches[i]);
        free(s->watches);
    }
    if (s->occ_list) {
        for (int i = 0; i < s->nlits; i++) vec_free(&s->occ_list[i]);
        free(s->occ_list);
    }
    free(s->occ);
    free(s->occw);
    free(s->sample_active);
    free(s->sat_stk);
    free(s->val);
    free(s->trail);
    free(s->decs);
    free(s);
}

int solver_parse(Solver *s, FILE *in, double keep_pct, int lrat,
                 int64_t max_adds)
{
    char buf[1 << 16];
    int64_t nadds = 0;

    /* keep_all short-circuits the RNG, so parsing the formula consumes no
     * randomness and cannot perturb the prefix sample. */
    bool keep_all = keep_pct >= 100.0;
    long keep_cut = (long)(keep_pct / 100.0 * ((double)RAND_MAX + 1.0));

    while (fgets(buf, (int)sizeof(buf), in)) {
        char c = buf[0];
        /* comments, deletions, headers, status lines, blanks */
        if (c == 'c' || c == 'd' || c == 'p' || c == 's' || c == 'S' ||
            c == 'U' || c == '%' || c == '\n' || c == '\r' || c == '\0')
            continue;

        char *p = buf;

        /* LRAT: strip the clause id, and drop deletion lines.  An LRAT
         * deletion is `<id> d <ids> 0`, which STARTS WITH A DIGIT -- the
         * 'd' test at the top of this loop only catches DRAT's `d ...`, so
         * without this every deleted id would be read as a literal.  Done
         * before the sample so deletions neither consume randomness nor
         * count against the sample rate. */
        if (lrat) {
            char *e;
            strtol(p, &e, 10);
            if (p == e) continue;            /* not a numbered line */
            p = e;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == 'd') continue;         /* deletion */
        }

        /* Counted HERE: after the deletion tests and before the sample, so
         * the budget means "additions in the proof", not "lines read" and
         * not "clauses sampled".  Sampling at -k 2 with the counter after it
         * would make `--max-adds 100000` cut the proof at 5 million. */
        if (max_adds > 0 && nadds == max_adds) { s->adds_capped = 1; break; }
        nadds++;

        /* Sample before tokenizing: a rejected clause costs one fgets and
         * one rand(), never a strtol per literal. */
        if (!keep_all && (long)rand() >= keep_cut)
            continue;

        int64_t off = s->arena_sz;

        for (;;) {
            char *end;
            long  v = strtol(p, &end, 10);
            if (p == end) break;            /* no more integers  */
            p = end;
            if (v == 0) break;              /* clause terminator */

            if (s->arena_sz >= s->arena_cap) {
                s->arena_cap *= 2;
                s->arena = (arena_t *)droofix_alloc(s->arena,
                        (size_t)s->arena_cap * sizeof(arena_t), "clause arena");
            }
            int av = (int)(v > 0 ? v : -v);
            if (av > ARENA_MAX_VAR) {
                fprintf(stderr, "Error: variable %d exceeds the %d-bit arena "
                        "limit of %d variables.\n", av, DROOFIX_ARENA_BITS,
                        ARENA_MAX_VAR);
#if DROOFIX_ARENA_BITS == 16
                fprintf(stderr, "       Rebuild with: make ARENA_BITS=32\n");
#endif
                return 1;
            }
            if (av > s->nvars) s->nvars = av;
            s->arena[s->arena_sz++] = (arena_t)FROMDI((int)v);
        }

        int len = (int)(s->arena_sz - off);
        if (len == 0) continue;             /* empty or malformed line */
        if (len > 65535) {
            fprintf(stderr, "Error: clause with %d literals exceeds the "
                            "65535 limit\n", len);
            return 1;
        }

        if (s->nclauses >= s->cl_cap) {
            s->cl_cap *= 2;
            s->cl_off = (int64_t *)droofix_alloc(s->cl_off,
                    (size_t)s->cl_cap * sizeof(int64_t), "clause offsets");
            s->cl_len = (uint16_t *)droofix_alloc(s->cl_len,
                    (size_t)s->cl_cap * sizeof(uint16_t), "clause lengths");
        }
        s->cl_off[s->nclauses] = off;
        s->cl_len[s->nclauses] = (uint16_t)len;
        s->nclauses++;
    }
    s->n_adds = nadds;
    return 0;
}

static void *zalloc(size_t n, const char *what)
{
    return memset(droofix_alloc(NULL, n, what), 0, n);
}

int solver_init(Solver *s)
{
    if (s->nclauses == 0) return 0;

    s->nlits    = 2 * (s->nvars + 1);
    s->val      = (int *)zalloc((size_t)(s->nvars + 1) * sizeof(int), "assignment");
    s->trail    = (int *)droofix_alloc(NULL, (size_t)(s->nvars + 1) * sizeof(int), "trail");
    s->occ      = (int *)zalloc((size_t)s->nlits * sizeof(int), "occurrence counts");
    s->occw     = (double *)zalloc((size_t)s->nlits * sizeof(double), "weighted counts");
    s->watches  = (Vec *)zalloc((size_t)s->nlits * sizeof(Vec), "watch lists");
    s->occ_list = (Vec *)zalloc((size_t)s->nlits * sizeof(Vec), "occurrence lists");

    /* The sample was chosen during parsing, so it already sits contiguously
     * at [nformula, nclauses) and slot i is clause nformula + i. Only the
     * prefix is sampled; formula clauses are not, since the prefix is what
     * carries information about where the formula is hard. */
    s->pick_k        = s->nclauses - s->nformula;
    s->sample_active = (bool *)droofix_alloc(NULL, (size_t)s->pick_k * sizeof(bool), "sample flags");
    s->sat_stk       = (int *)droofix_alloc(NULL, (size_t)s->pick_k * sizeof(int), "satisfied stack");

    for (int i = 0; i < s->pick_k; i++) {
        int cid = s->nformula + i;
        arena_t *c = s->arena + s->cl_off[cid];
        int len = s->cl_len[cid];
        double w = 1.0 / len;
        s->sample_active[i] = true;
        for (int k = 0; k < len; k++) {
            vec_push(&s->occ_list[c[k]], i);
            s->occ[c[k]]++;
            s->occw[c[k]] += w;
        }
    }

    {   /* --pick-k is the only knob that moves these, so report them. */
        long long occ_cap = 0;
        for (int i = 0; i < s->nlits; i++) occ_cap += s->occ_list[i].cap;
        const double gb = 1073741824.0;
        double a = (double)s->arena_cap * sizeof(arena_t) / gb;
        double o = (double)occ_cap * sizeof(int) / gb;
        double x = (double)s->nclauses * (sizeof(int) + sizeof(uint16_t)) / gb;
        double k = (double)s->pick_k * (sizeof(bool) + sizeof(int)) / gb;
        fprintf(stderr, "c memory: arena %.2f + occ lists %.2f + index %.2f"
                " + sample %.2f = %.2f GB (%d-bit arena)\n",
                a, o, x, k, a + o + x + k, DROOFIX_ARENA_BITS);
    }

    /* Propagation runs over formula clauses only. A prefix clause is
     * guaranteed equisatisfiable with F, not implied by it, so propagating
     * one could force a literal F does not imply. */
    for (int i = 0; i < s->nformula; i++) {
        int len = s->cl_len[i];
        arena_t *c = s->arena + s->cl_off[i];

        if (len == 0) return -1;
        if (len == 1) {                     /* unit: assign, nothing to watch */
            if (lit_undef(s, c[0]))       assign(s, c[0]);
            else if (lit_false(s, c[0]))  return -1;
            continue;
        }
        vec_push(&s->watches[c[0]], i);
        vec_push(&s->watches[c[1]], i);
    }

    if (propagate(s)) return -1;
    if (s->trail_sz)
        fprintf(stderr, "c %d literals forced at level 0\n", s->trail_sz);
    return 0;
}

void solver_cube(Solver *s, int max_depth, FILE *out)
{
    s->max_depth = max_depth;
    s->ndecs     = 0;
    s->ncubes    = 0;
    s->decs      = (int *)droofix_alloc(NULL, (size_t)(max_depth + 1) * sizeof(int),
                                      "decision stack");

    if (s->stats_out)
        fprintf(s->stats_out, "depth\treason\tfree_vars\tactive_slots\tocc_sum\tocc_max\n");

    double t0 = droofix_now();
    dfs(s, 0, out);
    s->cube_time = droofix_now() - t0;

    /* The DFS completed, so the cube set is a complete cover: write it now.
     * Anything that kills us before this point leaves stdout empty. */
    for (long i = 0; i < s->cube_buf_sz; i++)
        fprintf(out, s->cube_buf[i] ? "%d " : "0\n", s->cube_buf[i]);
    fflush(out);
    fprintf(stderr, "c COMPLETE ncubes=%d depth=%d\n", s->ncubes, max_depth);

    /* Cubes go to `out` (block-buffered when redirected) and diagnostics to
     * stderr (unbuffered). Flush first so that under 2>&1 the summary lands
     * after the cubes instead of in the middle of them. */
    fflush(out);
    fprintf(stderr, "c generated %d cubes in %.3f sec (%.0f cubes/sec)\n",
            s->ncubes, s->cube_time,
            s->cube_time > 0 ? s->ncubes / s->cube_time : 0.0);

    free(s->decs);
    s->decs = NULL;
}
