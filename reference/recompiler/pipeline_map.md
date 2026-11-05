# Pipeline map: two precedent pipelines → FM4 Recomp

Read-only survey of two precedents; nothing was run in either. Port A = a reference Xbox 360 port (another
title; recompiler patches 0001–0011). Port B = an earlier FM4 port attempt (patches 0001–0033; its M1 passed).
Patch numbers below are the precedent's own, not ours. Labels: OBSERVED (read in source), HYPOTHESIS (inferred).

## 1. Pipelines

### 1a. Port A: `scripts/survey.sh` (bash, multi-pass, config rewritten in place)

| # | Step | Inputs → outputs |
|---|---|---|
| 0 | Strip generated blocks from the title config (regex on markers); reset the inline switch-table file | toml → toml |
| 1 | `xexsurvey` (image, section table, helper patterns, imports) | default.xex → `image.bin`, `xexsurvey.txt` (markdown) |
| 2 | `XenonAnalyse` (upstream jump-table scan) | xex → `switch_tables.raw.toml` |
| 3 | `fix_switch_tables.py`: drop a DROP set + tables with a *borrowed* bounds check (cmplwi taken from the previous function), append hand ADD tables and the inline file | raw → `switch_tables.toml` |
| 4 | XenonRecomp pass 1 `--discovery` (only for `ppc_func_mapping.cpp`; FATAL summary tolerated, crash not) | → `ppc/` |
| 5 | `find_missing_tables.py`: uncovered `bctr`, decode `lis/addi/rlwinm/lwzx/mtctr` absolute tables with no bound; length = consecutive in-function code words | image + pass-1 map → inline switch-table file |
| 6 | `fix_switch_tables.py` again | → `switch_tables.toml` |
| 7 | `leaf_switch_bounds.py --write`: explicit `functions` bounds for leaf (no .pdata) table functions (start = latest bl target / data ref / lis-addi ref / after padding; end = branch-closure + next exit; overlaps merged) + `b +4` tail-call stubs + MANUAL | image, tables → `[main].functions` block |
| 8 | `find_entry_points.py --write`: code addresses in .rdata/.data or built by lis+addi/ori, entry-like (prev word blr/b/0), not a known start or label → `secondary_entries` | image, pass-1 map → `[main].secondary_entries` block |
| 9 | Assert `functions`/`secondary_entries` are `[main]` keys (tomllib) | |
| 10 | XenonRecomp (must be clean) | → `ppc/`, `recomp.log` |
| 11 | Fixed point: rerun step 8 against the new map, recompile, ≤3 passes or fail | |
| 12 | `validate_recomp.py recomp.log` (§3) | |

Helpers: `layout.py` parses `xexsurvey.txt` section rows; `classify_switches.py` is a diagnostic classifier of
"switch case outside function" lines and is `exec`'d as a prelude library by steps 5/7/8. Instruction tests are
separate (§4).

### 1b. Port B: `scripts/survey.py` (python, single pass, config hand-written)

| # | Step | Inputs → outputs |
|---|---|---|
| 0 | `Paths(module)`: title config or `config/<module>.toml`, survey dir, ppc dir must match | |
| 1 | `xex_report` | xex → `image.bin`, `report.json` (base, entry, export_table, sections, helpers, imports, import_slots) |
| 2 | Each helper pattern exactly one hit and equal to config | report + config |
| 3 | Import records consistent (thunk count = function_slot count = imports) | report |
| 4 | setjmp/longjmp/runtime_routines: context-shape (≥17 of r14–r31 + r1 via one base reg) and a direct-branch target | image + config |
| 5 | `survey/switch_tables.py`: classify every bctr SWITCH/UNPROVEN/INDIRECT; prove bounds (dominating cmplwi, masks, nested cases, `prove_region` function-level), fixed point ≤64 iterations | image, report → survey-dir `switch_tables.toml`, `bctr.json` |
| 6 | XenonRecomp (one pass; function analysis is inside the patched recompiler: 0016–0028) | config → `ppc/` incl. `ppc_functions.txt`, `recomp.log` |
| 7 | Census of `FATAL:`/`Unrecognized instruction` and `NOTE:` lines by category | recomp.log |
| 8 | Every context-shaped analysed function must equal the configured routines | ppc_functions.txt |
| 9 | `summary.json` (input/tool/image SHA-256, timings, counts); `--require-clean` fails on any problem | |
| 10 | `scripts/validate.py [--self-test] [--syntax]` (separate command, §3) | |
| — | `extract_module.py` (copy a disc-root file from the ISO, GDFX walk), `coverage.py` (M2 strict-vs-diagnostic report; needs opcode_census + runtime/) | |

### 1c. Which is more fail-closed

Port B. Port A tolerates FATAL in pass 1, its switch inference uses unbounded heuristics (find_missing_tables:
table length = "consecutive in-function words"; byte tables "until the first byte that isn't a valid offset"),
its validator has no self-test, and its instruction harness silently drops Xenia tests if the Xenia test
directory is absent (`|| true`). Port B proves every bound or emits no labels → FATAL (its patch 0025), checks
helpers/routines against the image before recompiling, self-tests every HARD check, and its harness has count +
mutation controls (§4). Port A is simpler and leaves less in the recompiler.

### 1d. FM4 specifics: who handles them

| FM4 specific | Port A | Port B |
|---|---|---|
| Fibers / context restore | — | `runtime_routines` key, body = `PpcRuntime_<name>` (patch 0024); shape-checked by the survey; validator `runtime_routines` |
| setjmp/longjmp | config keys only, upstream semantics | shape check, host jump buffer `PpcJumpBuffer`, 2nd setjmp in a function FATAL (0024, 0027) |
| frsqrte residual | — | FATAL, approved site list `strict_stops` → `PpcStrictStop` (0029), EXPERIMENTAL estimate (0031), checked by the validator |
| Unproven switches | heuristics + MANUAL | empty labels → FATAL; `strict_stops`; `experimental_labels` (0030) |
| Secondary entries | config list from heuristics (find_entry_points.py), patch 0003 | analysis in the recompiler (0021 branch targets, 0026 pointer-reached); emitted in `ppc_functions.txt` as `secondary@owner` |
| SEH metadata / `invalid_instructions` | one hand `invalid_instructions` word (title-specific) | patch 0017 skips the 2 words before each ExceptionFlag .pdata entry; coverage counts them |
| Export table at end of .text | — | patch 0018 + `export_table()`; `report.json.export_table` |
| Three code sections (.text, PAGELK, _TEXT) | `TEXT` = one span min..max | per-section loops |
| Title modules (XMediaFacade) | — | `--module`, modules.py, extract_module.py |

## 2. `config/fm4.toml`

**Port B's** title config is *entirely hand-written*; its header says generated data goes to the survey
directory. Structure, all in `[main]`:
- paths: `file_path`, `out_directory_path = "../ppc"`, `switch_table_file_path` pointing into the survey directory;
- eight `*_as_local`/`skip_*` = false;
- eight helper addresses. Evidence: comment + the survey verifies *exactly one* pattern hit equal to the value on
  every run;
- `setjmp_address`, `longjmp_address`, `runtime_routines = [{address, name, noreturn?}]`.
  Evidence: comment (found by behaviour, call counts) + shape and branch-target check every run;
- `strict_stops = [...]`: a dated decision in the comment; validator requires it to equal the FATAL list exactly;
- `invalid_instructions` padding only.
No `functions`, no `secondary_entries` (recompiler derives them). XMediaFacade has the same shape with its own
addresses.

**Port A's** config = hand-written header (paths, flags, helpers, setjmp/longjmp with a prose description of each
routine, `invalid_instructions`), then two **generated blocks inside `[main]`** between markers:
`secondary_entries` (`# --- generated by survey/find_entry_points.py --write ---` … `# --- end find_entry_points ---`;
each line carries a comment *"referenced from data|code; in .pdata fn X / leaf code"*) and `functions`
(`# --- generated by survey/leaf_switch_bounds.py --write ---` … `]`), then hand-written `[[midasm_hook]]` tables.
Switch tables are a committed generated file `config/switch_tables.toml` (XenonAnalyse output + `# ---- MANUAL` +
`# ---- INLINE` sections).

**Placement rule.** A known TOML pitfall: a generator appending after a hand-written `[[table]]` makes its list
that table's key. Port A enforces it three ways: insert at the end of `[main]` before the first table and its
leading comments, insert `secondary_entries` before the leaf block, and assert both keys are in `[main]` after
writing.

**Open layout decision (not made here).** M1's acceptance ("byte-identical `config/` generated blocks",
"config/fm4.toml (+ switch tables)") assumes Port A's layout. Under Port B's layout `config/` has no generated
block and that clause is vacuous; Port B proved reproducibility on `ppc/`, `switch_tables.toml` and `bctr.json` in
its survey directory instead. Committing addresses-only generated files in `config/` is allowed. Options: (A) Port
A layout: commit `config/switch_tables.toml` (generated by Port B's prover) and keep any generated `[main]` keys
behind markers; (B) Port B layout: everything generated stays in the survey directory, and the acceptance text is
amended. Recommendation: (A) for the switch tables (reviewable diffs of every proven/unproven table, matches the
acceptance text), with Port B's `switch_tables.py` as the generator; no generated `functions`/`secondary_entries`
blocks unless a patch set without 0021/0026 is chosen.

## 3. Validators: HARD checks

### Port A `survey/validate_recomp.py` (no self-test, no syntax check)
| Check | Fault caught |
|---|---|
| `sub_` mapping entries without a definition | function listed but never emitted |
| definitions not in the mapping | stray/duplicate-named body |
| functions defined more than once | duplicated body |
| `PPC_FUNC_IMPL` without matching `PPC_WEAK_FUNC` | broken weak/alias wrapper |
| .pdata starts in code not in the mapping | lost .pdata function |
| entry-like lis+addi/ori code pointers not a function start | indirect call aborts at runtime (seen in that title) |
| translation-failure markers = 0: `#error "XenonRecomp:`, `// ERROR X`, `// ERROR: 0xX`, blrl debugtrap, non-zero `.long`, comment-only (silently dropped) instruction, `attn` | any dropped/untranslated code |
| every `[[midasm_hook]]` emitted at its address | hook silently skipped by XenonRecomp |
| render-queue fences (title-specific, would FAIL on FM4 since it requires the state to exist) | unfenced publish / handshake |
| recomp.log: zero FATAL lines and zero "other" lines | any recompiler complaint |
Coverage is REPORT only, not HARD.

### Port B `scripts/validate.py` (`--self-test` injects one fault per check, `--syntax` clang `-fsyntax-only` both with and without `-DPPC_EXPERIMENTAL`)
| Check | Fault caught |
|---|---|
| `pdata_functions` | an analysed function's size ≠ .pdata FunctionLength×4 (VMX helpers excepted) |
| `no_overlap` | overlapping analysed functions (helpers' shared tails excepted) |
| `no_code_over_data` | a function spanning a thunk/SEH words/export table/switch table (own table allowed) |
| `secondary_entries` | entry not strictly inside its owner, not emitted, or an inline switch table made an entry |
| `runtime_routines` | setjmp/longjmp/runtime routine body is not only its runtime call; has a secondary entry |
| `address_taken` | pointer-reached code (data or lis+addi/ori after terminator) is not a start/entry |
| `no_tail_cycles` | a cycle of non-call transfers between functions (guest loop split → C recursion) |
| `coverage` | a non-zero code word outside every function/thunk/export table/SEH words/proven table |
| `experimental` | experimental case not code of its function, table words ≠ cases, dispatch not emitted as listed; frsqrte stop without its #ifdef estimate |
| `no_error_markers` | any `#error` in generated code (except ppc_context.h guards) |
| `defined_labels` | `goto loc_X` with no `loc_X:` in the function |
| `fatal_residuals` | FATAL not in RESIDUALS×`strict_stops`; listed stop not reported; stop missing/unlisted |
| `syntax` (opt.) | generated C++ that does not compile |
Self-test: 19 injections; hardcodes FM4 address 0x823C2398 and assumes a proven inline table exists — both need
re-derivation with evidence here.

## 4. Instruction-test harnesses

Both: tests are gas-syntax `.s` files in Xenia's format, assembled by **LLVM** (`llvm-mc
-triple=powerpc-unknown-unknown -mcpu=pwr7 -mattr=+altivec`), linked by `ld.lld -e 0 --image-base=0
-Ttext=0x10000`, disassembled with `llvm-objdump -d`. A perl pass adds llvm-mc's `%` register prefix. No
hand-encoded words, no binutils. Then XenonRecomp **test mode** (`XenonRecomp <dir>/bin <out>`; upstream
main.cpp:48 → test_recompiler.cpp:35) reads `<bin>/*.o`, `.dis` and `../<name>.s`, recompiles each function, and
generates `main.cpp` with one `test_*` function per label: `REGISTER_IN`/`MEMORY_IN` set up `PPCContext`,
`REGISTER_OUT`/`MEMORY_OUT` become `PPC_CHECK_VALUE_U/F`, which print `… EXPECTED … ACTUAL …` on mismatch
(test_recompiler.cpp:108-109, 209-253). Expected values are the `#_ REGISTER_OUT` lines in the `.s` (vectors as
`[w0, w1, w2, w3]`, floats need a `.`).

Upstream hazard: an untranslatable function is skipped with "Function X in Y has unimplemented instructions" and
exit 0 (test_recompiler.cpp:64); "Found no symbol"/"Unable to locate source file" also only print.

| | Port A `tests/run_instr_tests.sh` | Port B `tests/run_translator_tests.sh` |
|---|---|---|
| Build | rebuilds XenonRecomp+XenonAnalyse, then a CMake/Ninja `XenonTests` target | compiles generated `out/*.cpp` directly with clang++ `-include harness_doubles.h` |
| Untranslatable | grep "has unimplemented instructions" → exit 1; non-zero exit or `^FATAL:` → exit 1 | XenonRecomp non-zero (its 0003 makes test mode exit ≠0) → FAIL; **"harness runs N of M tests"** count |
| Pass criterion | results.txt empty | no `EXPECTED` line |
| Parser-quirk guards | none | rejects `REGISTER_OUT` on cr/ctr (silently ignored by upstream); label directly after an expectation; strips plain `#` comments |
| Mutation control | none | every `_OUT` value rewritten; *every* generated check must fail |
| Negative tests | none | `trap/*.s` must die by signal; `reject/*.s` must fail with the `#! FATAL <text>` message; `#! EMITS <text>` for fences etc. |
| Expected values | hand from ISA or `gen_instr_tests.py` independent Python model, seeded RNG | hand from ISA, sources stated in each file header; frsqrte vs 12 Xenia hardware vectors + ISA |
| Fail-open spots | Xenia tests copied with `|| true`: missing Xenia tests pass silently; "Found no symbol" not caught | depends on its 0003 (non-zero exit) and on `harness_doubles.h` macros from its 0012/0015 (`PPC_CALL_INDIRECT_FUNC`, `PpcQueryTimebase`) |

## 5. Recommended port plan for FM4 Recomp

Current state (OBSERVED): `tools/XenonRecomp` = the upstream pin + 0001 + 0002, build has
XenonRecomp/XenonAnalyse/XenonTests dirs; `survey/xexsurvey.cpp` (adapted from Port A, markdown output); no
`config/`; no Xenia instruction tests; mutation gate exists (tests/mutants.py, mutate.py).

**Incompatibility to fix first:** FM4's `xexsurvey` section rows have 6 columns
(`| name | start | end | size | code|data[, discardable] | note |`, xexsurvey.cpp:243-245) while Port A's
`layout.py` matches 5 columns ending `| (code|data) |` → it would find no sections. Port B's validator/prover read
`report.json`, whose `export_table` field needs its patch 0018's loader change. Choose one: add a JSON report to
`xexsurvey.cpp` (recommended: one parser, typed fields, Port B's scripts then port with small edits) and leave
`export_table` = 0 until 0018 is imported.

### Order (each step = its own commit, tests + mutants; patches one at a time)

| Step | Take | From | Depends on |
|---|---|---|---|
| S1 | `xexsurvey` JSON report (base, entry, sections{name,start,end,code}, helpers, imports{address}, import_slots) + exact-one-hit helper check | Port B xex_report, survey.py | 0001, 0002 (in) |
| S2 | `config/fm4.toml` hand header: paths, flags off, 8 helper addresses (from our own xexsurvey run, with evidence), padding `invalid_instructions` | Port B config (structure; addresses re-derived) | S1 |
| S3 | **First diagnostic survey** `scripts/survey.sh`: preflight → xexsurvey → XenonRecomp → census of `FATAL:`/`Unrecognized instruction` by category + summary with SHA-256s; non-zero XenonRecomp exit allowed only in this diagnostic mode, crash fails | Port B survey.py (as bash or a python step called from survey.sh) | S1, S2; works with upstream log lines (Port B's PROBLEM regex matches "Unrecognized instruction at") |
| S4 | Patch 0003 fail-closed (FATAL + `#error` + non-zero exit, also test mode) | Port B 0003 (reviewed; Port A 0011 is the same idea) | 0001, 0002 |
| S5 | Instruction harness `tests/run_instr_tests.sh`: Port B structure (count check, quirk guards, mutation control, `reject/`, `trap/`, `#! EMITS`), Port A's option of an independent Python generator; no Xenia test fetch unless pinned and made mandatory (not `|| true`) | Port B run_translator_tests.sh; Port A gen_instr_tests.py | S4 (reject/ tests and exit codes need it) |
| S6 | Translation patches 0004–0015, 0032, 0033 as the census demands, each with `.s` tests | Port B | S4, S5 |
| S7 | Analysis patches 0016 (`ppc_functions.txt`), 0017 (SEH words), 0018 (export table), 0019, 0020, 0021 (secondary entries), 0022, 0023 | Port B | 0016 first; see the dependency column of Port B's patch README |
| S8 | Validator `survey/validate.py`: start with checks that need only 0003+0016 (`pdata_functions`, `no_overlap`, `coverage`, `no_tail_cycles`, `no_error_markers`, `defined_labels`, `fatal_residuals` with empty RESIDUALS), add `secondary_entries`/`address_taken` after 0021/0026, `runtime_routines` after 0024, `experimental` only if 0030/0031 are approved; `--self-test` per check (re-derive 0x823C2398), `--syntax`; mutants for each check | Port B validate.py; Port A validate_recomp.py marker scan as an extra HARD check | S7 |
| S9 | 0024 + `setjmp_address`/`longjmp_address`/`runtime_routines` with shape check | Port B survey.py; 0024, 0027 | 0021, 0023 |
| S10 | Switch prover `survey/switch_tables.py` + `tests/survey/test_switch_tables.py` (synthetic hand-encoded images) → `config/switch_tables.toml` (option A) | Port B | **0025 must be in first**: before it, an UNPROVEN entry (empty `labels`) and the `table`/`table_size` keys are not fail-closed in upstream; then 0026, 0027, 0028 |
| S11 | Residuals: 0029 `strict_stops` (decision per site); 0030/0031 only with explicit approval (EXPERIMENTAL) | Port B | 0025, 0029 |
| S12 | Reproducibility: two from-scratch runs, compare `config/` generated files and `ppc/` hashes | Port B's evidence pattern | all |
| later | `--module`/modules.py/extract_module.py (XMediaFacade, M3), coverage.py (M2) | Port B | — |

**Do not take** (title-specific): Port A's `fix_switch_tables.py` ADD and DROP lists, `leaf_switch_bounds.py`
MANUAL list, its `invalid_instructions` word, its validator's render-queue checks, its `[[midasm_hook]]` entries;
Port B's addresses (helpers, routines, strict_stops) are facts to cross-check, re-derived from our image with
evidence. Port A's multi-pass heuristic chain (steps 3–11) is not needed if Port B's 0021/0025/0026 are imported;
keep Port A's marker+`[main]` assertion for any generated key we do write.

### Minimal set for a first diagnostic survey (now, patches 0001+0002 only)
`tools/survey/xexsurvey` (exists) + XenonRecomp (exists) + a hand `config/fm4.toml` (S2) + `scripts/survey.sh`
(S3) that records problems by category. No switch tables yet: `switch_table_file_path` must point at an empty
generated file (or XenonAnalyse's raw output, labelled diagnostic). Expect upstream's silent `// ERROR` drops (an
earlier attempt counted 3,885 sites): the census must also grep `ppc/` for `// ERROR` markers, since upstream
exits 0. Port B's PROBLEM regex matches only `FATAL:` and upstream's "Unrecognized instruction at"
(recompiler.cpp:2432); upstream also logs "Unable to decode instruction" (:2422) and "ERROR: Switch case at …
outside function" (:621), and emits `// ERROR` for calls to non-functions with no log line (:402). The diagnostic
census must count all four.

### Tool dependencies on the test Mac (checked with `ls`/`command -v` only)
| Tool | Needed by | Status |
|---|---|---|
| Homebrew LLVM clang/clang++ 23 | build, harness, `--syntax` | present `/opt/homebrew/opt/llvm/bin/clang`(-23) |
| `llvm-mc`, `llvm-objdump` | harness | present in `/opt/homebrew/opt/llvm/bin` |
| `ld.lld` | harness | present `/opt/homebrew/bin/ld.lld` (not in the llvm bin dir; both runners fall back to PATH) |
| LLVM PowerPC target | harness | HYPOTHESIS present: `libLLVM-23.dylib` contains "PowerPC 64"/"PowerPC 32 LE" strings, and this LLVM assembled big-endian PPC test objects before; not re-run |
| powerpc binutils (`powerpc-*-as`) | nobody | absent; not required by either precedent |
| ninja, cmake | XenonRecomp build | present `/opt/homebrew/bin` |
| perl | `%`-prefix rewrite, mutation control | `/usr/bin/perl` |
| Python ≥3.11 (`tomllib`), stdlib only | all scripts (no pip packages) | `/opt/homebrew/bin/python3` 3.15.0; `/usr/bin/python3` 3.9.6 too old |
| shasum | survey.sh fixed point / reproducibility | `/usr/bin/shasum` |
| Xenia instruction tests | Port A harness only (optional there) | absent |
