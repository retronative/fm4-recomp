# Security policy

## Reporting a vulnerability

Please report security problems privately through GitHub's private vulnerability reporting:
**[Report a vulnerability](https://github.com/retronative/fm4-recomp/security/advisories/new)**
(the repository's **Security** tab, then **Report a vulnerability**). Don't open a public issue,
pull request or discussion for a suspected vulnerability.

REtronative's temporary public contact address is
[retronativeorg@gmail.com](mailto:retronativeorg@gmail.com). Its suitability for security
reports is **pending confirmation** of mailbox access, monitoring and confidential report
handling. Until confirmed, do not send vulnerability details there; use GitHub's private
vulnerability reporting above.

Include what you can of:

- the affected component (see Scope) and the commit or build you tested;
- steps to reproduce, and what happens versus what you expected;
- the impact as you understand it, and any suggested fix.

**Never attach game files, disc images, executables, recompiled code or a built binary** to a
report, even privately. Describe them instead, or reduce the problem to a small file of your own.

FM4 Recomp is maintained by volunteers, so responses are best effort. We'll acknowledge the
report, keep you updated in the private advisory, and credit you when a fix is published unless
you'd rather stay anonymous. Please give us a reasonable chance to fix the problem before you
disclose it publicly.

## Supported versions

Only the latest release and the `main` branch are supported; fixes land on `main`.

## Scope

In scope: code in this repository and what it builds:

- the runtime (`runtime/`, `recomp/`), the recompiler patches (`patches/`) and the configuration
  (`config/`);
- the setup, survey, build and packaging scripts (`scripts/`, `survey/`, `app/`), for example
  unsafe handling of disc images, packages or archive contents;
- the website (`.website/`) and the GitHub Actions workflow (`.github/`).

Out of scope:

- problems in *Forza Motorsport 4* itself or in Xbox network services; report those to their
  owners;
- problems that exist only in the upstream projects FM4 Recomp uses (XenonRecomp, SDL3,
  MoltenVK, FFmpeg and others); report those upstream, and tell us if FM4 Recomp is affected;
- requests to bypass security checks or reach online services; FM4 Recomp runs offline.
