# Contributing to FM4 Recomp

Thanks for your interest. FM4 Recomp is an unofficial fan project in early development; read the
[README](README.md) and the [legal notice](docs/LEGAL.md) first.

## Before you start

- **Licence.** FM4 Recomp is GPL-3.0-or-later (see [COPYING](COPYING) and [NOTICE](NOTICE)). By
  contributing, you agree that your contribution is licensed under the same terms. For
  significant work, please open an issue first so we can agree on the change.
- **Security problems** go through private reporting, never a public issue: see
  [SECURITY.md](SECURITY.md).
- **No game files.** Never attach, commit or link game files, disc images, executables (original
  or decrypted), extracted packages, the recompiled C++ (`ppc/`), captures or traces of the game,
  or a `fm4recomp` binary or DMG you built yourself, in issues, pull requests or commits. You need your own
  legally obtained copy of *Forza Motorsport 4* to build and test FM4 Recomp.
- **No online services.** Contributions that target Xbox network services or bypass security
  checks will not be accepted.

## Reporting bugs

Open an issue with your macOS version, Mac model, the commit you built, what you did, what
happened and what you expected. FM4 Recomp stops with a named reason when it meets something it
does not implement; include that stop line from the log. Attach logs and screenshots only if they
contain no game files.

## Development

```sh
scripts/setup_tools.sh                                    # pinned tools into tools/
scripts/extract_game.sh <dir with your ISOs> <out dir>    # unpack your discs outside the repo
mkdir -p game && ln -sfn <out dir>/disc1 game/files
scripts/survey.sh                                         # recompile the executable into ppc/
scripts/build.sh <evidence dir>                           # native build
```

- Recompiler changes are patches in `patches/xenonrecomp/`, applied in order to a pinned
  XenonRecomp; see [patches/xenonrecomp/README.md](patches/xenonrecomp/README.md). A problem that
  would drop code must fail the run, never be skipped.
- Unimplemented kernel or XAM imports stop with their name and arguments. Never report success
  for something that is not implemented, and cite each import's semantics (Xenia source with its
  revision, or Xbox documentation).
- Fix problems for real (configuration, function bounds, documented patches); never suppress
  errors or sanitizer findings.
- Game addresses come from the supported disc's own executable, with evidence; never from another
  title or build. Runtime code names no title address.
- GPU work runs only through `scripts/gpu_run.sh` (deadline, lock-up detection, stop flag). Never
  run deliberately invalid GPU work on real hardware; test such cases with CPU-only models.

## Tests

Every behaviour change comes with a focused test whose expected values come from an independent
source (the PowerPC ISA manual, Xenia's behaviour cited by revision, Xbox documentation, published
vectors or hand computation), never from FM4 Recomp's own output. Run the tests your change
touches; the commands are in the test runners under `tests/`.

Critical logic (decoders, address and endianness arithmetic, bounds and safety checks) also gets
mutants in `tests/mutants.py`. The mutation gate (`tests/mutate.py`) plants each one in a scratch
copy, and every mutant must be killed by the tests or documented as equivalent. Never weaken an
assertion or change production behaviour only to make a test pass.

## Commits and pull requests

- One logical change per commit, with a single-line
  [Conventional Commits](https://www.conventionalcommits.org/) message, e.g. `fix: ...` or
  `docs: ...`. Every commit builds.
- Keep pull requests small and focused, describe what you verified and how, and include the
  test output.
- Code taken from another project keeps its attribution and licence, with our changes marked.
