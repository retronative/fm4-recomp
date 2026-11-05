# Unproven switch dispatches: first FM4 diagnostic survey

Run: `scripts/survey.sh --diagnostic`. bctr classes: 384 SWITCH, **10 UNPROVEN**, 1,140 INDIRECT; XenonRecomp exit 1
with 10 × "switch table at X has no proven bound" (12 `#error` markers: 824FB8FC and 82ADF294 are in bodies
re-emitted for secondary entries).

**FALSIFIED** (an earlier draft README): "the blind rule should not change 390/4/1140". The 6 function-level proofs
lie in functions whose dispatches depend on each other; our blind rule refuses them. Expected after the fixes below:
393 SWITCH / 1 UNPROVEN / 1,140 INDIRECT (HYPOTHESIS).

Details: per-dispatch analyses of 824FB8FC, 82ADA478, 82ADF294, 82AE79A8 and of functions 82B4DA48, 82B90B98, and a
reference survey (an earlier FM4 port attempt, two other FM4 ports) (not
distributed); 82ADA478's index: [unproven_switch_c.md](unproven_switch_c.md), [unproven_switch_d.md](unproven_switch_d.md).
Key instructions re-checked in the listings (VERIFIED). No bound below comes from a table scan: in-range scans
overshoot (824FB8FC: 22 vs 16; 82ADF294: 30 vs 29).

| bctr | pattern (index) | table | candidate targets (plausible run) | ways in and guard | missing evidence | remedy |
|---|---|---|---|---|---|---|
| 824FB8FC | BYTE<<2 (r28 = arg r4, one write) | .rdata 0x82030418, labels 0x824FB900 + byte<<2 | 16 entries 0..15 (16..21 still in function, 22 outside) | fall-through: `cmplwi r28,15; bgt` (8D0); branches from 868/87C into 8D8: only after `cmpwi r28,15; bne` at 820/828 → r28 == 15 (crosses 2 bl + 2 bctrl: ABI r14-r31) | none in the image; the prover lacks the equality rule and function-level proof for .rdata tables | (d) prover |
| 82ADA478 | ABSOLUTE (r7 = field +24 of arg r3) | inline 0x82ADA47C | 12 (word 13 is code) | fall-through only; **no compare or mask** on either path | an invariant +24 ≤ 11 from every writer and the initialiser; the in-function writer can store 12 or 13 (r29 ≤ 11 only if h[12] < 16384); 82AD8DD8 and 13 callers unexamined | **UNRESOLVED: needs a decision** |
| 82ADF294 | HALF (r19 = state field) | .rdata 0x821A6D78, labels 0x82ADF298 + half | 0..28 (29 entries; entry 29 = 0, entry 30 = text) | fall-through: `cmplwi r19,28; bgt` 66 straight-line instructions back; back edge: `cmplwi r19,28; ble` at 82AE05AC/B0 | none in the image; `prove` walks back only 16 instructions | (b) prover |
| 82AE79A8 | ABSOLUTE (r8, reload of 112(r1)) | inline 0x82AE79AC | 16 (two distinct targets) | fall-through only: a load of 112(r1), `cmplwi 15; bgt`, then a reload of 112(r1) into the index (no store/call between) | none in the image; the prover treats the reload as a new value | (c) prover |
| 82B4DC74 (A3) | ABSOLUTE (r28, one write DA90) | inline DC78 | 12 | fall-through from DC5C; traces to cases of A1/A2 (bounded by `cmplwi r28,11` at DB48) | none; refused by our blind rule (A4-A6 unproven) | (e) group proof |
| 82B4DCF0 (A4) | ABSOLUTE (r28) | inline DCF4 | 12 | only A3's cases | A3 proven first | (e) |
| 82B4DDC4 (A5) | ABSOLUTE (r28) | inline DDC8 | 12 | fall-through after a compare on r29; traces to cases of A3/A4, crosses bctrl DD98 (ABI) | A3, A4 proven first | (e) |
| 82B4DE44 (A6) | ABSOLUTE (r28) | inline DE48 | 12 | only A5's cases | A5 proven first | (e) |
| 82B90D5C (B4) | ABSOLUTE (r10, writes BA8, E40) | inline D60 | 12 | only B3's cases (B3 local `cmplwi r10,11`) | `prove` reads B3's last table word as a fall-through | (a) prover |
| 82B90E0C (B6) | ABSOLUTE (r10) | inline E10 | 12 | only B5's cases (B5 local `cmplwi r10,11`) | same as B4 | (a) prover |

References: an earlier FM4 port attempt proved the 6 function-level ones only together with an EXPERIMENTAL
recompiler patch (its own docstring) and ran the other 4 as strict stops; its run reached the stop at **82B4DC74**
(audio engine). The other FM4 ports have no entries for these addresses (one manual table each,
another site). HYPOTHESES on purpose: 82ADF294 = zlib inflate mode switch; 82AE79A8 = PNG bit depth; 82ADA478 = a
range-coder adaptive model (so a stop there would likely fire during asset loading).

## Remedies (in order; each with synthetic tests, mutants, a diagnostic rerun and a bctr.json diff)
- (a) `prove()` does not count a proven table's word as a fall-through (prove_region's existing rule).
- (b) straight-line back-walk to the function start (stops at any branch target, case, entry, call or
  write of the index) instead of 16 instructions.
- (c) a reload `lwz rX,d(r1)` equals an earlier `lwz rY,d(r1)` when no store, call, branch target or
  write of r1 lies between.
- (d) `cmpwi/cmplwi idx,K; bne away` bounds idx to K on the fall-through; prove_region accepts a
  compare on the index as a root and runs for non-inline tables (the inline-extent check is a
  consistency check only).
- (e) joint proof of all unproven dispatches of a function (inline ABSOLUTE, same index), rooted in a
  real compare outside the group; all or none.

## 82ADA478: decision needed (if no invariant is found)
1. Prove +24 ≤ 11 from every writer and the initialiser (new evidence, program-wide).
2. Accept as a documented residual that stops: a new patch (listed addresses compile to a self-naming
   stop), a config list, a validator check that the list equals the FATAL set; own tests and mutants.
   Fail-closed default.
3. Heuristic extent (as in the earlier attempt's experimental patch): rejected by current rules; only with explicit approval.
M1 is not complete while this dispatch is unresolved.

## Context routines (same run)
Shaped analysed functions: 82762A90 (load, 6 calls), 82762EBC (store), 82C7A450 (store+switch),
82C8F3AC (load, 1 call). VERIFIED: 82762EBC is a zero padding word after `blr`; setjmp starts at
82762EC0 (a secondary entry of the sweep function). 82C7A450 is CreateFiber's 16-byte fiber-start
trampoline (calls the start routine with r31 as its argument, then the exit call); the context switch starts at
82C7A460 (SwitchToFiber's target). Configure 82762EC0 (setjmp), 82762A90 (longjmp), 82C7A460 (switch_fiber),
82C8F3AC (restore_context, noreturn): equal to the earlier FM4 attempt and another FM4 port. Notes for M3: the
trampoline is a fiber's initial PC; setjmp first reads a hook at 0x82E9D918 that patch 0024's body replacement ignores.
