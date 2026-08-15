---
name: search-evidence
description: >-
  Evidence discipline for searching and reviewing this AROS tree: preflight your
  shell tools, never trust a negative result from an aliased `grep`, and count
  occurrences rather than matching lines. Use before any code review, audit,
  completeness claim, exhaustive producer/consumer sweep, or any statement of the
  form "X does not exist in the tree" / "there are N call sites".
---

# Search evidence discipline

A search result is a claim about the codebase. In this tree the default tools
produce **false negatives silently**, so an unverified negative is not evidence.

This is not hypothetical. It has already caused a wrong finding that survived two
attempts and was caught by the user, not the tooling: the `ACTION_*64` DOS packet
constants were twice reported as "not defined in AROS headers" when they had been
public in `compiler/include/dos/dosextens.h` since the initial import.

## 1. Preflight the shell tools

Before relying on any shell search, know what you are actually running:

```sh
type grep          # is it a function/alias, or /usr/bin/grep?
```

In this environment `grep` is a **shell function** wrapping
`ugrep -G --ignore-files --hidden -I ...`. Two of those flags change results
without saying so:

- **`-I`** treats any file containing a non-UTF-8 byte as binary and **skips it**.
  No warning. Clean exit status. Indistinguishable from "no matches".
- **`--ignore-files`** applies ignore-file rules, so ignored paths vanish too.

**`../aros-upstream` is 21% non-UTF-8: 4,404 of 21,123 files.** Much of AROS is
ISO-8859. Every one of those files is invisible to the wrapped `grep`.

Other tools worth preflighting rather than assuming: this is BSD userland, so
`grep -P` does not exist, `cat -A` does not exist, and `timeout` is not installed.

## 2. Never trust a negative from an aliased grep

A wrapped-grep "no matches" is **unproven**, not false. Never write "X does not
exist in the tree", "nothing references Y", or "this is the only definition" on
the strength of one.

To establish a negative, use a tool that cannot silently skip:

```sh
command grep -rn "PATTERN" .          # bypasses the wrapper entirely
```

or, for anything exhaustive, a Python scan that decodes explicitly:

```python
raw = open(path, "rb").read()
text = raw.decode("utf-8", errors="replace")   # never skips the file
```

Prefer the Python form for producer/consumer sweeps and completeness audits: it
also lets you report *which* hits came from non-UTF-8 files, which is exactly the
set a reviewer would otherwise never see.

Cross-check any surprising negative against a second, unrelated tool before
reporting it. `sed -n 'A,Bp'` and Read are both reliable here and disagreed with
`grep` in the case above.

## 3. Count occurrences, not matching lines

`grep -c` counts **lines that match**, not matches. Two occurrences on one line
count once. This silently undercounts, and the error grows with denser code.

It undercounted `e.entry.` accesses in the FAT handler as **130** when the true
figure is **146**, a figure that was feeding an architecture decision.

When a count is load-bearing, count occurrences:

```sh
command grep -o "PATTERN" file | wc -l
```

or in Python, `len(re.findall(pat, text))`. State which you counted.

## Before reporting

- [ ] Did I preflight the tool, or assume it?
- [ ] Is any negative claim backed by `command grep` or a Python scan?
- [ ] Did I cross-check surprising negatives with a second tool?
- [ ] Are counts occurrences, and is that stated?
- [ ] For exhaustive sweeps: did I report the scanned-file total, so the reader
      can judge coverage rather than take "complete" on trust?

Related: [CLEANROOM.md](../../../docs/features/CLEANROOM.md) for provenance
discipline on what a source may be used for, as distinct from whether a search
found it.
