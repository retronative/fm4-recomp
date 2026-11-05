# XenonRecomp patches

Applied in order with `git am` onto upstream XenonRecomp `ddd128bcca99fe8bfbb99bea583c972351fa6ace`
by `scripts/setup_tools.sh` (branch `fm4` in `tools/XenonRecomp`). After changing a patch, delete
the branch and re-run the script.

Origin: patches from ForzaRecomp (GPL-3.0-or-later, authored by the project owner) are imported
byte for byte, one at a time, each reviewed, re-tested here and given mutants
(`tests/mutants.py`). Our numbers differ from
ForzaRecomp's where the order had to change; the table gives both.

| FM4 Recomp | ForzaRecomp | What | Why here | Tests | Mutants |
|---|---|---|---|---|---|
| 0001 | 0001 | XEX BASIC compression: allocate the declared image size | FM4's blocks expand to 0xF48000 of a declared 0xF50000 bytes; upstream reads past its buffer | `tests/run_loader_tests.sh`: `xex_basic_*` (4 tests, one ours) | L1–L6 |
| 0002 | 0002 | XEX sections: bound names and extents by their fields and the image | `.XBMOVIE` (8 characters, no NUL); `.reloc` and the facades' sections past their images | `xex_section_*` (3 tests, one ours) | B1–B5 |
| 0003 | 0013 | ARM64: write FPCR from a 64-bit register | every translator-harness build fails with `-Werror=asm-operand-widths` without it, so it comes before the patches whose tests need the harness | `tests/run_translator_tests.sh`: `harness_smoke` (ours) | F1 |
| 0004 | 0003 | Fail closed on untranslatable code | upstream drops 3,885 FM4 sites and exits 0 (ForzaRecomp's count) | `counter_branch_tail_calls`, `traps_not_taken`, `trap/` (4), `reject/` (5), `xex_truncated_test` | C1–C11 |
| 0005 | 0004 | Translate update-form loads and stores | FM4: 2,229 sites (ForzaRecomp's survey count) | `update_forms` (18), `update_forms_extension` (3, ours: rT preloaded with ones), 2 rejects | U1–U6, Z1 |
| 0006 | 0005 | Translate bdzf, bdnzt, bso and bns | FM4: bdzf 661 sites (cr6.eq after VMX record compares), bso/bns 69 after fcmpu (ForzaRecomp's count) | `counter_and_so_branches` (15) | K1–K6 |
| 0007 | 0006 | Translate addc, addme, subfze and eqv | FM4 uses them (ForzaRecomp's survey); CA and CR0 from the low 32 bits = Xenia 67f16c4 (`ppc_emit_alu.cc:27` AddDidCarry, `ppc_hir_builder.cc:397` UpdateCR truncate to INT32) | `carry_and_eqv` (15) | E1–E6 |
| 0008 | 0007 | Translate VMX shifts, logic, select and memory-hint forms | FM4: 394 sites (ForzaRecomp's count); vsl assumes the ISA-required uniform count (a non-uniform-count mutant is equivalent on defined inputs) | `vmx_shift_logic_memory` (10) | V1–V7 |
| 0009 | 0008 | Translate VMX integer arithmetic, packs and vcfpuxws128 | FM4: 277 sites (ForzaRecomp's count); VSCR[SAT] not modelled | `vmx_arith_pack_convert` (14), `vcfpuxws128_guards` (1, ours: EMITS the UB guards), `reject/vcmpequh_record_form` (ForzaRecomp) | A1–A8 |
| 0010 | 0009 | Decode VMX128 before PowerPC 405 instructions for Cell | one FM4 word decoded as mullhwu. instead of vsldoi128 (ForzaRecomp); global decoder change: 403/405-only forms now fail as undecodable (fail-closed). Our own before/after opcode census is due once the survey exists | `decoder_vmx128_over_405` (1) | D1–D3 |
| 0011 | 0010 | Translate lhbrx and dcbst | FM4 uses both (ForzaRecomp); dcbst is a no-op on a coherent host, GPU-visible ordering relies on the sync/eieio fences (patch 0014, ForzaRecomp) | `byte_reverse_and_cache` (3), `lhbrx_ra_zero` (1, ours: rA = 0 means literal 0) | R1–R5 |
| 0012 | 0011 | Unpack D3D float16 types and pack float16_2 | FM4: 171 + 4 unpack sites, 1 pack site (ForzaRecomp's count). Pack overflow/NaN -> 0x7FFF is upstream's existing rule, APPROXIMATE: Xenia/ReCarbon reportedly give 0x7BFF with sign; no hardware oracle (UNRESOLVED) | `vmx_d3d_float16` (5), `vmx_d3d_float16_alias` (1, ours: vD = vB), `reject/vupkd3d128_immediate_low_bits` (ours) | H1–H8 |
| 0013 | 0012 | Translate blrl as an indirect call | FM4 uses blrl (ForzaRecomp); the skip_lr guard (FATAL) is untestable until fixtures can set config (planned own patch; its mutant is registered then) | `indirect_calls` (1) | I1–I2 |
