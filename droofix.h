#ifndef DROOFIX_H
#define DROOFIX_H

/* clock_gettime is POSIX and -std=c11 hides it; must precede any system
 * header. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

/* Arena element width. 16 bits halves the largest allocation but caps the
 * variable index at 32767; build with `make ARENA_BITS=32` for larger
 * instances. solver_parse refuses an out-of-range variable either way. */
#ifndef DROOFIX_ARENA_BITS
#define DROOFIX_ARENA_BITS 16
#endif

#if DROOFIX_ARENA_BITS == 16
typedef uint16_t arena_t;
#define ARENA_MAX_VAR 32767
#elif DROOFIX_ARENA_BITS == 32
typedef uint32_t arena_t;
/* nlits = 2*(nvars+1) is an int, so that caps the range, not the element. */
#define ARENA_MAX_VAR (INT_MAX / 2 - 1)
#else
#error "DROOFIX_ARENA_BITS must be 16 or 32"
#endif

/* Literals: variable v >= 1, positive literal 2*v, negative 2*v + 1. */
#define VAR(l)    ((l) >> 1)
#define SIGN(l)   ((l) & 1)
#define NEG(l)    ((l) ^ 1)
#define POSLIT(v) ((v) << 1)
#define NEGLIT(v) (((v) << 1) | 1)
#define FROMDI(d) ((d) > 0 ? ((d) << 1) : ((-(d)) << 1 | 1))
#define TODI(l)   (SIGN(l) ? -(int)VAR(l) : (int)VAR(l))

/* Split-variable score. occ+ and occ- are a candidate's two polarity counts
 * over the still-active prefix sample. */
typedef enum {
    WEIGHT_NONE = 0,  /* every literal occurrence counts 1                  */
    WEIGHT_INVLEN     /* a clause carries weight 1, shared over its literals */
} WeightMode;

typedef enum {
    PICK_SUM = 0,   /* occ+ + occ-     */
    PICK_MIN,       /* min(occ+, occ-) */
    PICK_PROD       /* occ+ * occ-     */
} PickMode;

/* val[var] */
#define UNDEF  0
#define LTRUE  1
#define LFALSE 2

/* realloc that exits with a message rather than returning NULL.
 * ptr == NULL behaves like malloc. Defined in droofix.c. */
void *droofix_alloc(void *ptr, size_t n, const char *what);

typedef struct { int *d; int sz, cap; } Vec;

static inline void vec_init(Vec *v) { v->d = NULL; v->sz = v->cap = 0; }

static inline void vec_push(Vec *v, int x)
{
    if (v->sz == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->d = (int *)droofix_alloc(v->d, (size_t)v->cap * sizeof(int),
                                  "occurrence/watch list");
    }
    v->d[v->sz++] = x;
}

static inline void vec_free(Vec *v) { free(v->d); vec_init(v); }

typedef struct {
    /* Clause storage: literals packed contiguously in `arena`, indexed by
     * cl_off/cl_len. Clauses [0, nformula) are the CNF; [nformula, nclauses)
     * are the sampled prefix, so sample slot i is clause nformula + i. */
    arena_t  *arena;
    int64_t  *cl_off;
    uint16_t *cl_len;         /* max 65535 literals per clause */
    int   nclauses, nformula;
    /* 64-bit: a full proof of a hard instance reaches 10^8 clauses at ~15
     * literals each, i.e. past 2^31 arena entries, and a 32-bit offset then
     * wraps into undefined behaviour rather than failing. */
    int64_t arena_sz, arena_cap;
    int   cl_cap;

    int   nvars;              /* max variable index (1-based) */
    int   nlits;              /* 2 * (nvars + 1)              */

    Vec  *watches;            /* two-watched literals; formula clauses only */

    /* Occurrence counts over the sampled prefix, kept per literal so that
     * pick_var can see the two polarities separately. */
    Vec  *occ_list;           /* occ_list[lit] = sample slots containing lit */
    int  *occ;                /* occ[lit] over slots still active           */
    /* The same counts with each clause contributing 1/len to every literal
     * it holds, i.e. a clause carries weight 1 and shares it out -- which is
     * what cubical and lratdag score with.  `occ` stays an exact integer
     * count because CANDIDACY is "appears at all", and that must not become
     * a float comparison; only the SCORE uses occw. */
    double *occw;             /* 1/len-weighted, when weight_mode != WEIGHT_NONE */
    bool *sample_active;      /* slot not yet satisfied                     */
    int  *sat_stk;            /* satisfied slots, for undo on backtrack     */
    int   sat_sz;
    int   pick_k;             /* number of sampled prefix clauses           */

    int  *val;
    int  *trail;
    int   trail_sz, prop_head;
    int   occ_head;           /* trail prefix already reflected in occ[]    */

    int  *decs;               /* current cube */
    int   ndecs, max_depth, ncubes;

    /* Cubes are BUFFERED and written only once the DFS has finished.
     *
     * They used to be fprintf'd as the DFS found them, which made a killed
     * run (`timeout 1000 ./droofix ...`) leave a partial cube set on stdout.
     * A partial set does not cover the search space, so solving its cubes
     * and seeing every one come back UNSAT proves nothing at all -- and the
     * file looks perfectly well-formed.  Buffering means an interrupted run
     * writes NOTHING, which is self-enforcing.
     * Flat array, each cube terminated by a 0. */
    int  *cube_buf;
    long  cube_buf_sz, cube_buf_cap;

    PickMode   pick_mode;
    WeightMode weight_mode;
    double   cube_time;       /* seconds spent in the DFS */
    FILE    *stats_out;       /* per-cube TSV, NULL = disabled */

    /* Addition lines consumed by the last proof parse, and whether the cap
     * actually bit.  `adds_capped == 0` at a budget of N means the proof
     * held fewer than N additions -- that row is the FULL proof wearing a
     * budget's name.  lratdag carries the same `capped` canary for exactly
     * this reason: without it, the instances whose 10M arm is secretly the
     * full proof are the large ones, which is a size-correlated confound. */
    int64_t  n_adds;
    int      adds_capped;
} Solver;

double  droofix_now(void);      /* monotonic seconds; only differences matter */
Solver *solver_new(void);
void    solver_free(Solver *s);
/* keep_pct >= 100 keeps every clause; otherwise each clause is kept with
 * probability keep_pct/100. Call srand() before a sampled parse. */
/* lrat != 0: lines are `id lits 0 antecedents 0` and deletions are
 * `id d ids 0`.  Only the literals are read -- the parse already stops at the
 * first 0, so the antecedent chain costs nothing and is simply never seen. */
/* max_adds > 0 stops the parse after that many ADDITION lines, so a prefix
 * budget needs no temp file: a clausal proof is append-only, so its first N
 * additions are byte-for-byte what a solve capped at N would have written.
 * Deletions and comments do not count against the budget, matching both the
 * awk cut this replaces and lratdag's --max-adds. */
int     solver_parse(Solver *s, FILE *in, double keep_pct, int lrat,
                     int64_t max_adds);
int     solver_init(Solver *s);   /* -1 on level-0 conflict */
void    solver_cube(Solver *s, int max_depth, FILE *out);

#endif
