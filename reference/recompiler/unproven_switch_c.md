# 0x82ADA478: bounded investigation of the index (field +24)

Bounded read-only analysis of the decrypted image. OBSERVED = seen in disassembly; HYPOTHESIS = assumed or not
verified. **Status: HAND PROOF WITH OPEN ASSUMPTIONS (A1, A2). Not a machine-checked bound; the dispatch stays a
documented named stop (rule: no bound from table size, no clamp).** Follow-up: [unproven_switch_d.md](unproven_switch_d.md);
overview: [unproven_switches.md](unproven_switches.md).

## Claim
On every path to 0x82ADA45C, +24 is 0 (constructor memset) or r29 from the rebuild block; at each rebuild
r29 is derived from h12 = h10 <= h0 <= 15161 < 16384, so r29 <= 11. The bound comes from the frequency total,
not from the alphabet size (two constructor sites take the alphabet cap from 16 bits of stream data, up to 65535)
and not from the table size.

## Object layout (byte offsets; hN = halfword at +N; OBSERVED)
h0 total frequency; h2 rebuild threshold (0x82ADA35C `cmplw h0,h2; blt` skips the rebuild); h4 max step; h6
current step; h8 rescale limit (0x82ADA368: h2 >= h8 → prune 0x82ADA0C8); h10 live symbol count; h12 snapshot of
h10 at rebuild (store at 0x82ADA330 in 0x82ADA270); h14 alphabet cap; +16 symbol map ptr; +20 freq array
ptr (u16); +24 step count = dispatch index; +28..+76 step sizes; +80.. cumulative table.
r29: v = h12+1; while v > 4: v = (v+1)>>1, n++; r29 = n-1. r29 <= 11 iff h12 <= 16383 (h12 = 16384 → 12,
65535 → 13). Last write of +24: store of r29 at 0x82ADA438 (the loop's stwu and the memmove at 0x82ADA430 write
+24..+76 earlier). The +28..+76 storage would hold 12 or 13 steps: storage size does not imply the bound.
0x82AD8DD8 takes only the coder (r28), not the model.

## Initialiser 0x82AD9F88 (OBSERVED)
memset 0 (0x82760500), then h0 = 4, freq[0] = 4, h2 = 8, h6 = 4, h14 = r6 (16-bit), +28 = r4 (0 at all sites),
h8 = clamp(r5*32, 256, 15160) (0x82ADA048..080), h4 = 128 if r5*2 < 128 else min(r5*2, (h8>>1)-32) ≤ 7548.
Call sites: 0x82AD88D8, 8928, 8968, 89A4, 89D0, 89FC, 0x82ADBF70, 0x82ADC448, C46C, C484 (alphabets ≤ 511, ≤ 255,
≤ 255, ≤ 256, ≤ 4, ≤ 8192, stream+1, stream+1, 64, 256).

## Writers (OBSERVED, scan of 0x82AD8000..0x82ADE000)
0x82ADA340 (freq/h0 increments, escape paths, h10 += 1 only at 0x82ADA7B4, rebuild block, +24 at 0x82ADA438);
0x82ADA0C8 prune (halves, removes freq <= 1, recomputes h0 exactly, never increases h10); 0x82ADA270 rebuild
(h2 = h0 + step, step = min(2*h6, h4); h12 = h10). Other stores to +24 / halfwords 0..14 in that range belong to
other object types not passed to 0x82ADA340 (0x82AD9158, 9364, 94F0..98F0, 9A24..9A74; 0x82ADCD7C, D828, D864:
HYPOTHESIS, base registers not traced).

## Bound chain
1. Live symbols 1..h10 keep freq >= 1 (new +2; decode increments; prune halves values > 1 and removes <= 1).
2. Every change to h0 pairs with the same change to freq; prune recomputes h0 → h0 >= Σ live freq >= h10.
3. One call raises h0 by at most 3, and the rebuild check runs every call → at rebuild h0 <= h2 + 2.
4. h2 written only by the constructor (8) and 0x82ADA270 (h0 + step); step <= h4 <= 7548; h8 <= 15160.
5. Induction after each rebuild: no prune (h2 < h8): h0 <= h8 + 1 <= 15161; prune: before <= 22711, after
   <= 11356. No 16-bit field wraps (all <= 22711).
6. h12 = h10 <= h0 <= 15161 < 16384 → r29 <= 11; on the no-rebuild path +24 is the previous r29 or 0.

## Open assumptions
- A1 (HYPOTHESIS): no code outside the constructor, prune, rebuild and 0x82ADA340 writes h0..h14, +24 or the freq
  array. Only 0x82AD8000..0x82ADE000 was scanned; a whole-image alias proof is not available.
- A2 (partly verified): every pointer passed to 0x82ADA340 came from 0x82AD9F88. Verified at 0x82AD8A34 and
  0x82AD8BD8; NOT verified at 0x82AD8B10 ((r29+71)*4 against the +1324 array) and 0x82ADC6BC (index 0..32 via
  cntlzw against the r15 array).
- A3: r14-r31 preserved across calls; no model shared between threads.

## References
An earlier FM4 port attempt: only a `strict_stops` entry and a milestone note; no initialiser or runtime evidence.
Another FM4 port (a different build): no entry.

## Consequence for the stop
The named stop fires on **every** execution of this dispatch (its cases are not emitted), not only for an index
>= 12. If the analysis above is right, indices are always 0..11, so the stop fires whenever this range decoder
runs (HYPOTHESIS: asset/data decompression). That is the M2 risk to carry.
