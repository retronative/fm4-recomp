# 0x82ADA478: second bounded attempt on assumptions A1/A2

Follows [unproven_switch_c.md](unproven_switch_c.md). Status: **NOT a complete proof. Counterexamples exist for
out-of-spec stream data. Named stop retained.** OBSERVED = seen in disassembly at an address; HYPOTHESIS = reasoning.

## Whole-image callers (OBSERVED, bl scan of .text)
- Constructor 0x82AD9F88: only from 0x82AD88D8/8928/8968/89A4/89D0/89FC (fn 0x82AD8830), 0x82ADBF70 (0x82ADBDF0),
  0x82ADC448/C46C/C484 (0x82ADC210). Prune 0x82ADA0C8 and rebuild 0x82ADA270: only from 0x82ADA370/0x82ADA378.
- 0x82ADA340 is called from 12 sites: 0x82AD8A38, 8AAC, 8ADC, 8B14, 8BDC; 0x82ADC000, C0AC, C15C; 0x82ADC530, C5C0,
  C6C0, C7E8 (the first analysis listed 4).
## A1
- First family: buffer sized by 0x82AD87A0(255, 256, 0x3FFFF), allocated via fn ptr at 0x82DA23A0 (0x82AC8034), freed via
  0x82DA23A4 (0x82AC80E8); codec ptr goes only to 0x82AD8830, 0x82AD8A10 and the free (no global store seen). Models packed
  from codec+1336; object = cum table +80 (A+6 halfwords), freq at +92+2A, symbol map at +92+4A, end +92+6A, A = align4(h14+5).
- New writer: on escape 0x82ADA340 returns +16 + 2*h10 (0x82ADA800..804); callers `sth` a decoded value through it
  (0x82AD8A58, 8AC8, 8AFC, 8B44, 8BF8, 0x82ADC6DC, ...): the symbol map, while h10 < A.
- **Counterexample 1 (out-of-spec data):** at 0x82ADA7D8..7FC (h10 == h14) freq[0] = 0 and h0 reduced, but the cumulative
  table is not rebuilt until h0 >= h2; the stale cum[1] (0x82ADA2A8: >= 1) still decodes the escape; 0x82ADA714 increments
  freq[0]; path 0x82ADA728 → 0x82ADA7A0 makes h10 = h14+1; the equality test never matches again and prune re-enables the
  escape whenever h10 != h14 (0x82ADA230..264): h10 unbounded. At h10 >= A the caller's sth writes past the object into the
  next packed model's header (its +24 at symbol index A+12/A+13), read at 0x82ADA45C before any rebuild: +24 >= 12 possible.
  At h10 >= 2A the freq write at 0x82ADA7C0 leaves the object too.
- **Counterexample 2 (out-of-spec data):** 0x82AD8830 reads codec[1308] = word0 & 0x1FF (<= 511) and codec[1312] =
  word0 >> 9; no check against the sizing maxima between the header memcpy (0x82AC7FD0) and 0x82AC8090. word0 >= 2^27 →
  count codec[1324] > 256 → the pointer loop 0x82AD8990..89BC overruns codec+1308.. and the models from +1336.
## A2
- 0x82AD8B10: proven given word0 < 2^27 (escape value via 0x82AD8D58 clamped to n-1, 0x82AD8D90..8D98; slots
  0..codec[1324]-1 all constructed by 0x82AD8990..89BC). 0x82AD8A34: codec[1332] <= 64 (n = 65 at 0x82AD8A4C) → slot 68 =
  +272, constructed at 0x82AD896C.
- 0x82ADC6BC (and C7E8): proven up to the symbol-map caveat: r29, r20 16-bit; M = r29*r20 <= 0xFFFE0001 (no overflow);
  count = 33 - clz(M) in 1..33, all r15 slots constructed (0x82ADC434..C458); index = bitlen(sum >> 2) <= bitlen(M) =
  count - 1. Gaps: the caller's r15 buffer size (0x82ADC9E4..CAF4); full loop register flow not traced.
## Remaining gaps
Pointer escape and header checks for the 0x82ADBDF0/0x82ADC210 families; call sites 0x82ADC000, C0AC, C15C, C530, C5C0
unchecked; the header-maxima assumption; an h10 <= h14 invariant or an explicit "valid stream" premise; A3 unchanged.
## Conclusion
For well-formed (encoder-produced) streams the argument still looks sound (HYPOTHESIS). As a proof for every input it
fails: the bound is a property of valid data, not of the code. The named stop stays.
