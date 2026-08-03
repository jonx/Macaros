# Bridge Lab — turning a legacy-program run into reusable compatibility evidence

Bridge Lab records what a 68k program asked the runtime for, and classifies each
demand as a **runtime contract** that is satisfied, missing, or ambiguous.

**Its consumer is an agent, not a reader.** That single fact decides most of the
design below. An agent cannot skim a terminal, cannot infer meaning from silence,
pays for every token it reads, and needs to know *what to do next* rather than
*what happened*. Everything here follows from that.

## What it answers

- Which runtime contracts did the program exercise?
- Which guest context owned a port, signal, object or callback?
- Is this a generic bridge capability, or application-private behaviour?
- What is the smallest next action, and what evidence supports it?

It must **not** add application-specific routing or silently widen compatibility.
A trace may propose a contract; only a reviewed registry entry can change one's
status. A trace can never enable a crossing — that property is what makes the
importer trustworthy and it is not negotiable here.

## Why the consumer being an agent changes the design

| Human-oriented | Agent-oriented (what we build) |
|---|---|
| Prose report, JSON behind a flag | **Structured output is the product**; prose is a rendering of it |
| "See the trace for details" | Decisive events **embedded inline**, bounded, with `seq` ranges to slice more |
| Skeleton test for a human to finish | **Machine-readable test plan** (contract, invariants, fixtures to build); the agent writes the fixture, which it does better with repo context than a generator does |
| "NEEDS REVIEW" | A structured `next_action` naming the hypothesis, what would confirm it, and the command that produces that evidence |
| Full trace is fine | **Aggregate first.** 20,059 events must reduce to a few hundred tokens; detail is opt-in and addressable |
| Timestamps and addresses are useful | **Byte-stable output** for the same run, so a diff of two reports is signal, never churn |

### Concretely

1. **`report` emits JSON as its primary output.** A `--text` rendering exists for
   people. The JSON is the contract; the prose is not.

2. **Findings are self-describing.** An agent must not need this document to act
   on one. Every finding carries the contract id, its status, the invariant text
   in full, the evidence, and the next action as data:

   ```json
   {
     "contract": "scheduler.yield.frame_wait",
     "status": "needs-review",
     "invariant": "a context that never calls a blocking primitive must not starve its siblings",
     "evidence": {
       "seq_range": [1204, 20698],
       "aggregate": [
         {"context": 1, "event": "port.get", "count": 20049},
         {"context": 0, "event": "port.get", "count": 1}
       ],
       "sample_seq": [1204, 1205, 1206]
     },
     "next_action": {
       "kind": "propose-contract",
       "hypothesis": "WaitTOF blocks on real hardware and is therefore a yield point",
       "confirms_if": "ctx 0 resumes and its GetMsg count rises above 1",
       "refutes_if": "ctx 1 stops making progress it needs",
       "command": "graft/bridge-lab compare <trace> baselines/photodemo-startup.json"
     }
   }
   ```

3. **Aggregation is the default, detail is addressable.** The report leads with
   the smallest thing that changes a decision. `bridge-lab events <trace> --seq
   A..B` returns the raw window. An agent never has to read a 20k-line file to
   find the three lines that matter.

4. **Identities are assigned by first-seen order within the trace**, not by
   address. Our guest heap is a bump allocator: addresses are stable within a
   build and shift on any unrelated edit. Normalizing by address would make every
   baseline churn on every commit. `port:1`, `task:2`, `window:3`.

5. **Silence is never ambiguous.** The recorder emits `run.start` unconditionally
   when enabled, and `run.end` with a result. An empty trace file therefore means
   "the flag never reached the process" and a trace with only those two events
   means "the events did not happen" — two different bugs that must not look the
   same. This rule exists because they did look the same, and it cost two
   rebuild cycles to tell them apart.

6. **The tool's own failure is a finding**, not a crash: an unreadable trace, a
   schema version it does not know, or a truncated file is reported as a
   structured error with the same shape as any other finding.

## Scope: the execution substrate

Not every generated library vector. The substrate is where the contracts live:

- guest processes and cooperative scheduling;
- `MsgPort` lifecycle, queueing, signals, `Wait`, `WaitPort`, `GetMsg`, `ReplyMsg`;
- native-to-guest port bindings, including IDCMP;
- guest object handles, facades, adopted structures;
- callbacks and nested guest re-entry;
- blocking and yielding calls, including observed frame/timer waits;
- capability gaps and bridge refusals.

Library vectors stay visible in traces; their argument contents are not decoded
beyond known handles, tags and structures.

## Levels

`EMU68K_BRIDGE_TRACE=<file>` with `EMU68K_BRIDGE_TRACE_LEVEL`:

`off` (default) · `summary` · `runtime` · `calls` · `debug`

Disabled by default, no behavioural change and negligible cost when off. The
recorder uses a bounded buffer and never allocates from guest memory.

## The first contract, already evidenced

`EMU68K_TRACE_TASKS` (the precursor to this) produced the run that motivates the
whole thing:

```
IDCMP bind window=003115a0 port=00306658 mp_SigTask=00210000 mp_SigBit=31 (bound by ctx=0)
ctx=1 CREATED task=00317a20 entry=002dd11c

20049  ctx=1 GetMsg      <- a child that never blocks
    1  ctx=0 GetMsg      <- the context that OWNS the IDCMP port, once
```

Three windows bind correctly to one shared port owned by ctx 0; ctx 1 then spins
in `GetMsg` and never calls a blocking primitive, so the cooperative scheduler —
which only switches at `Wait` — never runs ctx 0 again. The owner of the input
port is starved by a sibling that never yields.

That is `scheduler.yield.frame_wait`: **a context that never calls a blocking
primitive must not starve its siblings.** Stated generically, with no program
named, which is the test every contract has to pass.

## Rollout

1. Recorder + schema, no behaviour change. **DONE** — `hosted/emu68k/bridge_lab.[ch]`.
2. Normalized report (JSON first) + contract registry. **DONE** —
   `graft/bridge-lab`, `graft/emu68k-runtime-contracts.json`.
3. `T3EVENT` gate over the already-supported ports/IDCMP/process behaviour.
   **DONE** — `make hosted-emu68k-t3event`, asserted from the run's own trace.
4. Trace PhotoDemo and one unrelated GUI program. **PhotoDemo done**; the second
   program is the next step, and is what stops one program's shape being
   mistaken for a general contract.
5. Only then propose a new contract from shared evidence.
6. Land the reviewed capability, its regression test, and a baseline trace.

Test-plan emission is step 7, deliberately last: it is the least proven part, and
until the registry and reports exist there is nothing for it to emit against.

## Gates

`T3EVENT`, alongside the existing mandatory ones (importer generation, LhA,
guard/hardware routing, byte-identical legacy corpus):

1. a guest-created port owns the creating task and an allocated signal bit;
2. deleting a port releases its bit; create/delete cycles do not exhaust bits;
3. one native IDCMP source pumps to one explicitly bound guest port;
4. three windows sharing one guest port preserve order and reply pairing;
5. an unbound guest port and every worker `pr_MsgPort` receive no native input;
6. `Wait` wakes after a pump, `GetMsg` returns the same rebuilt message,
   `ReplyMsg` replies natively exactly once;
7. a declared yield lets eligible child contexts run;
8. a non-yielding call cannot accidentally schedule another context.

## Acceptance

Deterministic reports for LhA, native samples and PhotoDemo startup; PhotoDemo's
shared IDCMP binding and worker mailboxes identified with no program-specific
rule; the frame-wait question reported as one generic contract; a reviewable
regression candidate produced from that trace; trace-disabled runs unchanged; all
gates green.
