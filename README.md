# droofix

Splits a CNF formula into cubes by reading a prefix of the solver's own proof,
choosing a different variable at every node of the cube tree.

Cube-and-conquer partitions a formula into subproblems $F \wedge c_i$ and solves
them independently. The question is which variables to split on. `droofix`
answers it from a *proof prefix*: run a SAT solver on the formula for a while,
keep the clauses it derives, and split on the variables that prefix leans on
most. The difference from a static split is that the choice is made again at
every node, over the part of the prefix that is still live — so two sibling
subtrees generally split on different variables, and a branch whose unit
propagation already conflicts is dropped instead of being handed to the solver.
The tree is therefore not full: depth `d` yields *at most* `2^d` cubes.

Only one solver run is needed. The prefix is derived once at the root and only
ever shrinks as the tree is descended.

## Build

```sh
make
```

Needs a C11 compiler and nothing else. `make debug` builds with ASan and UBSan.

## Use

Three steps: derive a proof prefix, cut it into cubes, solve the cubes.

```sh
# 1. a proof prefix -- any DRAT-emitting solver will do
cadical formula.cnf -q --binary=false proof.drat

# 2. cubes
./droofix -d 10 -p prod -k 100 -f formula.cnf -i proof.drat > cubes.txt

# 3. conquer: one incremental CNF, solved cube by cube
python3 make_icnf.py formula.cnf cubes.txt > formula.icnf
cadical formula.icnf
```

The solver does not have to finish step 1 — a prefix is all that is read, so
cap it however you like:

```sh
# stop after a million added clauses, without a temp file
cadical formula.cnf -q --binary=false - | ./droofix -a 1000000 -d 10 \
    -f formula.cnf -i - > cubes.txt
```

`-a` counts **additions** only; deletion lines are free. The banner reports
`capped=0` if the proof ended before the budget, meaning you cubed the whole
proof rather than a prefix of it.

## Options

| | |
|---|---|
| `-d, --depth <n>` | maximum cube depth (required) |
| `-f, --formula <f>` | the CNF (required) |
| `-i, --input <f>` | proof prefix, or `-` for stdin (required) |
| `-p, --pick <mode>` | split score over a variable's two polarity counts: `sum` (default), `min`, `prod` |
| `-k, --pick-k <p>` | percent of prefix clauses sampled for the counts (default 2) |
| `-w, --weight <m>` | `none` (default) or `inv-len`, where a clause carries weight 1 shared over its literals |
| `-a, --max-adds <n>` | stop after n proof additions (0 = whole proof) |
| `-L, --lrat` | the input is LRAT, not DRAT |
| `-s, --stats <f>` | one TSV line per cube: depth, reason, free vars, live clauses, occ sum, occ max |
| `-r, --seed <n>` | seed for the prefix sample (default 42) |

`-p prod` scores a variable by `occ+ * occ-`, so it counts only when both
polarities carry the proof; `-k 100` reads the whole prefix instead of sampling
it. That pair is a reasonable starting point.

## Output

One cube per line, DIMACS literals, `0`-terminated:

```
4 221 129 155 28 108 0
4 221 129 155 28 -108 0
```

Every assignment satisfies at least one cube, so the formula is unsatisfiable
exactly when every `F ∧ c_i` is. Cubes refuted during cubing are not emitted,
which is why you will usually see fewer than `2^d` lines.

## Notes

- **Arena width.** `ARENA_BITS=16` halves memory but caps the variable index at
  32767, and exceeding that wraps *silently* — the cubes come out well-formed
  and over the wrong literals. The default is 32. Changing it needs a
  `make clean`, since make cannot see the flag move.
- **LRAT.** Pass `-L` for LRAT input. The formats differ in more than a marker:
  a deletion is `d ...` in DRAT but `<id> d ...` in LRAT.
- A run stopped by an external `timeout` leaves a partial cube set on stdout,
  which is not a cover. Check the exit status before using it.
