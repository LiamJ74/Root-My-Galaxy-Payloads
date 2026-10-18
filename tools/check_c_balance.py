#!/usr/bin/env python3
"""Refuse to hand a source file to CI with unbalanced braces or parens.

Written after an edit of mine dropped a function's closing brace and the build spent five minutes
telling me so ("function definition is not allowed here", pointing at the function that followed).
A C compiler is not available where this repository is edited, so this is the cheapest check that
would have caught it before the push: strip comments and string literals, then count.

It is deliberately not a parser - it cannot see a mis-nested but balanced pair - and it is called
from the wiring step of ksu-susfs.yml for the one file this repository adds to the merged tree.
"""

import sys


def strip(text):
    """Drop block comments, line comments and double-quoted literals, in one pass."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            i = n if end < 0 else end + 1
            continue
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check(path):
    text = strip(open(path, encoding="utf-8").read())
    counts = {c: text.count(c) for c in "{}()"}
    balanced = counts["{"] == counts["}"] and counts["("] == counts[")"]
    print(f"{path}: {counts} -> {'balanced' if balanced else 'UNBALANCED'}")
    return balanced


def main(argv):
    if len(argv) < 2:
        sys.exit("usage: check_c_balance.py <file.c> [more.c ...]")
    sys.exit(0 if all(check(p) for p in argv[1:]) else 1)


if __name__ == "__main__":
    main(sys.argv)
