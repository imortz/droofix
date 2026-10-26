#!/usr/bin/env python3
"""
Merge a CNF formula and a cubes file into iCNF format.

Usage:
  make_icnf.py <formula.cnf> <cubes.txt> [> out.icnf]

The cubes file contains one cube per line: "l1 l2 ... 0"
Output iCNF has:
  p inccnf
  <formula clauses>
  a l1 l2 ... 0   (one per cube)
"""
import sys

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <formula.cnf> <cubes.txt>", file=sys.stderr)
    sys.exit(1)

cnf_path   = sys.argv[1]
cubes_path = sys.argv[2]

print("p inccnf")

with open(cnf_path) as f:
    for line in f:
        line = line.strip()
        if not line or line[0] in ('c', 'p', '%'):
            continue
        print(line)

with open(cubes_path) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        print("a " + line)
