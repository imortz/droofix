#include "droofix.h"
#include <getopt.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -d <depth> -f <formula.cnf> -i <prefix|-> [options]\n"
        "\n"
        "Reads a clausal proof prefix, splits the formula by DFS over the\n"
        "most frequently occurring variables in that prefix, and writes one\n"
        "cube per line to stdout.\n"
        "\n"
        "  -d, --depth <n>    maximum cube depth (required)\n"
        "  -f, --formula <f>  CNF file (required)\n"
        "  -i, --input <f>    proof prefix file, or - for stdin (required)\n"
        "  -k, --pick-k <p>   percentage of prefix clauses to sample for the\n"
        "                     occurrence counts (default 2)\n"
        "  -w, --weight <m>   how a clause contributes to the counts\n"
        "                     (default none):\n"
        "                       none     every literal occurrence counts 1\n"
        "                       inv-len  a clause carries weight 1, shared\n"
        "                                over its literals -- what cubical\n"
        "                                and lratdag score with\n"
        "  -p, --pick <mode>  split-variable score over a candidate's two\n"
        "                     polarity counts (default sum):\n"
        "                       sum   occ+ + occ-\n"
        "                       min   min(occ+, occ-)\n"
        "                       prod  occ+ * occ-\n"
        "  -L, --lrat         the input is LRAT, not DRAT: strip the clause\n"
        "                     id and skip `<id> d ...` deletion lines\n"
        "  -a, --max-adds <n> stop after n ADDITION lines of the proof, so a\n"
        "                     prefix budget needs no temp file (0 = whole\n"
        "                     proof).  Deletions do not count.  The banner\n"
        "                     reports `capped=0` when the proof was shorter\n"
        "                     than n, i.e. this arm is really the full proof\n"
        "  -r, --seed <n>     seed for the prefix sample (default 42)\n"
        "  -s, --stats <f>    write one TSV line per cube: depth, reason,\n"
        "                     free_vars, active_slots, occ_sum, occ_max\n"
        "  -h, --help         show this message\n"
        "\n"
        "Examples:\n"
        "  %s -d 10 -f formula.cnf -i prefix.drat > cubes.txt\n"
        "  %s -d 6 -a 1000000 -L -f formula.cnf -i proof.lrat > cubes.txt\n"
        "  cadical formula.cnf -q --binary=false - | %s -d 10 -f formula.cnf -i -\n",
        prog, prog, prog, prog);
    exit(1);
}

int main(int argc, char **argv)
{
    int         depth        = -1;
    double      pick_pct     = 2.0;
    unsigned    seed         = 42;
    PickMode    pick_mode    = PICK_SUM;
    WeightMode  weight_mode  = WEIGHT_NONE;
    const char *input_file   = NULL;
    const char *formula_file = NULL;
    const char *stats_file   = NULL;
    int         lrat         = 0;
    int64_t     max_adds     = 0;

    static struct option long_opts[] = {
        {"depth",   required_argument, 0, 'd'},
        {"formula", required_argument, 0, 'f'},
        {"input",   required_argument, 0, 'i'},
        {"pick-k",  required_argument, 0, 'k'},
        {"pick",    required_argument, 0, 'p'},
        {"weight",  required_argument, 0, 'w'},
        {"lrat",     no_argument,       0, 'L'},
        {"max-adds", required_argument, 0, 'a'},
        {"seed",    required_argument, 0, 'r'},
        {"stats",   required_argument, 0, 's'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "a:d:f:i:k:p:r:s:w:Lh", long_opts, NULL)) != -1) {
        switch (c) {
        case 'd': depth        = atoi(optarg);           break;
        case 'f': formula_file = optarg;                 break;
        case 'i': input_file   = optarg;                 break;
        case 'k': pick_pct     = atof(optarg);           break;
        case 'r': seed         = (unsigned)atoi(optarg); break;
        case 's': stats_file   = optarg;                 break;
        case 'L': lrat = 1;                              break;
        case 'a': max_adds = (int64_t)strtoll(optarg, NULL, 10); break;
        case 'w':
            if      (!strcmp(optarg, "none"))    weight_mode = WEIGHT_NONE;
            else if (!strcmp(optarg, "inv-len")) weight_mode = WEIGHT_INVLEN;
            else {
                fprintf(stderr, "Error: --weight must be none or inv-len "
                                "(got '%s')\n\n", optarg);
                usage(argv[0]);
            }
            break;
        case 'p':
            if      (!strcmp(optarg, "sum"))  pick_mode = PICK_SUM;
            else if (!strcmp(optarg, "min"))  pick_mode = PICK_MIN;
            else if (!strcmp(optarg, "prod")) pick_mode = PICK_PROD;
            else {
                fprintf(stderr, "Error: --pick must be sum, min or prod "
                                "(got '%s')\n\n", optarg);
                usage(argv[0]);
            }
            break;
        default: usage(argv[0]);
        }
    }

    if (depth < 0) {
        fprintf(stderr, "Error: --depth is required\n\n");       usage(argv[0]);
    }
    if (!formula_file) {
        fprintf(stderr, "Error: --formula is required\n\n");     usage(argv[0]);
    }
    if (!input_file) {
        fprintf(stderr, "Error: --input is required\n\n");       usage(argv[0]);
    }
    if (pick_pct <= 0.0) {
        fprintf(stderr, "Error: --pick-k must be positive\n\n"); usage(argv[0]);
    }

    Solver *s = solver_new();
    srand(seed);                 /* the prefix sample is drawn while parsing */

    double t_start = droofix_now();

    FILE *ff = fopen(formula_file, "r");
    if (!ff) { perror(formula_file); return 1; }
    if (solver_parse(s, ff, 100.0, 0, 0)) { /* formula: keep every clause */
        fprintf(stderr, "Error: failed to parse formula\n");
        solver_free(s);
        return 1;
    }
    fclose(ff);
    s->nformula = s->nclauses;

    int   from_stdin = strcmp(input_file, "-") == 0;
    FILE *in = from_stdin ? stdin : fopen(input_file, "r");
    if (!in) { perror(input_file); return 1; }
    if (solver_parse(s, in, pick_pct, lrat, max_adds)) {
        fprintf(stderr, "Error: failed to parse input\n");
        solver_free(s);
        return 1;
    }
    if (!from_stdin) fclose(in);

    double t_parsed = droofix_now();
    s->pick_mode   = pick_mode;
    s->weight_mode = weight_mode;

    fprintf(stderr, "c %d formula clauses, %d prefix clauses sampled at %g%%, "
                    "%d variables\n",
            s->nformula, s->nclauses - s->nformula, pick_pct, s->nvars);
    fprintf(stderr, "c input format: %s\n", lrat ? "LRAT" : "DRAT");
    /* Printed unconditionally.  A budget larger than the proof yields
     * capped=0, which says the arm is the full proof under a budget's name
     * -- and WHICH instances those are correlates with proof size, so a
     * column read without this canary is a size-selected subset. */
    fprintf(stderr, "c adds: %lld budget=%lld capped=%d\n",
            (long long)s->n_adds, (long long)max_adds, s->adds_capped);
    fprintf(stderr, "c clause weight: %s\n",
            weight_mode == WEIGHT_INVLEN ? "1/len (clause carries weight 1)"
                                         : "none (every occurrence counts 1)");
    fprintf(stderr, "c split score: %s\n",
            pick_mode == PICK_MIN  ? "min(occ+, occ-)" :
            pick_mode == PICK_PROD ? "occ+ * occ-" : "occ+ + occ-");

    if (solver_init(s)) {
        fprintf(stderr, "c level-0 conflict: formula is UNSAT\n");
        solver_free(s);
        return 0;
    }
    double t_inited = droofix_now();

    FILE *stats_out = NULL;
    if (stats_file) {
        stats_out = fopen(stats_file, "w");
        if (!stats_out) { perror(stats_file); solver_free(s); return 1; }
    }
    s->stats_out = stats_out;

    solver_cube(s, depth, stdout);

    /* Reported apart because only the cube phase responds to -d, -p and -k;
     * a slower end-to-end run may just be a larger prefix to read. */
    fflush(stdout);
    fprintf(stderr, "c time: parse %.3f  init %.3f  cube %.3f  total %.3f sec\n",
            t_parsed - t_start, t_inited - t_parsed, s->cube_time,
            droofix_now() - t_start);

    if (stats_out) fclose(stats_out);
    solver_free(s);
    return 0;
}
