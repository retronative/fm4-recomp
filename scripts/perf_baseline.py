# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# A compact performance baseline of one bounded run, read from its logs only (nothing is run):
#
#   /opt/homebrew/bin/python3 -B -I scripts/perf_baseline.py [--index <out.md>] <run dir> [<run dir> ...]
#
# --index also writes one table per phase (intro, menu, race) over every log given, one row per log.
#
# For every runN.err in a run dir (the gpu_run.sh evidence layout) it writes runN.perf_baseline.md beside the log, with
# one section per phase. Phases (the logs carry no screen marker): "intro" = the legal screen and intro video, from the
# first swap up to the swap before the first one with more than INTRO_MAX_DRAWS draws (VERIFIED on FM4:
# 5-6 draws per swap through the video, 97+ from the first menu); "menu" = from there to the run's end; "race" = not
# reached by any run yet, reported as unavailable. The phase rule is printed in every summary.
#
# Timing: the only wall clock in the log is the "[audio] periodic: ... wall W s" line (~every 2 s); each is paired with
# the last swap logged before it, so presented FPS and frame time are per-window averages, not per-frame values; a window
# whose two stamps fall in different phases is left out. A swap line closes its frame, so anything logged after swap N
# (a render target's creation) belongs to frame N + 1. GPU busy
# time and fence-wait time are not instrumented: reported "unavailable". Per-swap counters come from the
# "[gpu] swap N presented ..." lines (draws, resolves, texture upload bytes).
import os
import re
import statistics
import struct
import sys

INTRO_MAX_DRAWS = 20
SWAP = re.compile(r"\[gpu\] swap (\d+) presented .*?: (\d+) draw\(s\) rendered.*?(\d+) resolve\(s\).*?textures: (\d+) upload\(s\), (\d+) bytes")
AUDIO = re.compile(r"\[audio\] periodic: .*?wall ([0-9.]+) s")
TARGET = re.compile(r"\[edram\] host target for key ([0-9A-F]+): (\d+)x(\d+), (\d+) sample\(s\), format (\d+)")
STOP = re.compile(r"\[stop (\d+)\] (.*)")
MODE = re.compile(r"^\[gpu\] renderer (\w+):")
DIAG = re.compile(r"^(\[diag\] (FM4_[A-Z0-9_]+)|\[gpu\] diagnostic (FM4_[A-Z0-9_]+))")


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", head[16:24])


def parse(log_lines):
    swaps, audio, targets, stop, mode, diags = [], [], [], None, None, []
    last_swap = 0
    for line in log_lines:
        m = SWAP.search(line)
        if m:
            n, d, r, u, b = (int(x) for x in m.groups())
            swaps.append((n, d, r, u, b))
            last_swap = n
            continue
        m = AUDIO.search(line)
        if m:
            audio.append((last_swap, float(m.group(1))))
            continue
        m = TARGET.search(line)
        if m:
            targets.append((last_swap + 1, m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))))
            continue
        m = STOP.search(line)
        if m and stop is None:
            stop = (int(m.group(1)), m.group(2).strip())
            continue
        m = MODE.search(line)
        if m and mode is None:
            mode = m.group(1)
            continue
        m = DIAG.search(line)
        if m:
            name = m.group(2) or m.group(3)
            if name not in diags:
                diags.append(name)
    return swaps, audio, targets, stop, mode, diags


def phases(swaps):
    """[(name, first swap, last swap)]: intro up to the swap before the first above INTRO_MAX_DRAWS, then menu."""
    if not swaps:
        return []
    first_big = next((n for n, d, *_ in swaps if d > INTRO_MAX_DRAWS), None)
    if first_big is None:
        return [("intro", swaps[0][0], swaps[-1][0])]
    out = []
    if first_big > swaps[0][0]:
        out.append(("intro", swaps[0][0], first_big - 1))
    out.append(("menu", first_big, swaps[-1][0]))
    return out


def window_rates(audio, lo, hi):
    """Per-window presented FPS between consecutive audio stamps whose swaps both fall in [lo, hi]."""
    rates = []
    for (s0, w0), (s1, w1) in zip(audio, audio[1:]):
        if lo <= s0 and s1 <= hi and w1 > w0 and s1 > s0:
            rates.append((s1 - s0) / (w1 - w0))
    return rates


def fmt(x, digits=1):
    return "unavailable" if x is None else f"{x:.{digits}f}"


def summarize(run_dir, log_name):
    log_path = os.path.join(run_dir, log_name)
    with open(log_path, errors="replace") as f:
        swaps, audio, targets, stop, mode, diags = parse(f)
    stem = log_name[: -len(".err")]
    evidence = os.path.dirname(run_dir.rstrip("/"))
    commit = binary = launcher = None
    for path, name in ((os.path.join(evidence, "commit.txt"), "commit"), (os.path.join(run_dir, "binary.sha256"), "binary")):
        if os.path.exists(path):
            text = open(path).read().split()
            if name == "commit":
                commit = text[0] if text else None
            else:
                binary = text[0] if text else None
    launcher_path = os.path.join(evidence, "launcher.txt")
    launcher = open(launcher_path, errors="replace").read() if os.path.exists(launcher_path) else ""
    validation = "inserted, 0 messages" if re.search(r"layer inserted, no validation message", launcher) else (
        "inserted" if "layer inserted" in launcher else "unknown (no launcher.txt)")
    captures_dir = os.path.join(run_dir, stem + ".captures")
    resolution = None
    if os.path.isdir(captures_dir):
        pngs = sorted(p for p in os.listdir(captures_dir) if p.endswith(".png"))
        if pngs:
            resolution = png_size(os.path.join(captures_dir, pngs[-1]))
    captured = sum(1 for _ in open(log_path, errors="replace") if "front buffer captured" in _)

    out = [f"# Performance baseline: {os.path.relpath(log_path)}", "",
           "Generated by scripts/perf_baseline.py from the run's own logs (nothing was re-run). Targets: M7 stable 30 FPS racing; "
           "M8 60 FPS and <= 14 ms GPU time per race frame.", "",
           "| Setting | Value |", "|---|---|",
           f"| Build | commit {commit or 'unavailable'}, fm4recomp sha256 {binary or 'unavailable'} |",
           f"| Run end | {('stop ' + str(stop[0]) + ': ' + stop[1][:140]) if stop else 'unavailable'} |",
           f"| Presented resolution | {resolution[0]}x{resolution[1]} (front-buffer capture) |" if resolution else "| Presented resolution | unavailable |",
           f"| Validation | Vulkan validation layer {validation}; MTL_DEBUG_LAYER=1 (scripts/gpu_run.sh sets it for every run) |",
           f"| Captures | PNG of every presented swap ({captured} captured) |",
           f"| Renderer mode | {mode or 'unavailable (no renderer line: CPU-only or pre-renderer run)'} |",
           f"| Diagnostics on (timing not representative) | {', '.join(diags + (['FM4_CP_TRACE (cp.txt)'] if os.path.exists(os.path.join(evidence, 'cp.txt')) else [])) or 'none logged'} |",
           f"| Swaps logged | {len(swaps)} |", "",
           f"Phase rule: intro = swaps before the first with more than {INTRO_MAX_DRAWS} draws; menu = from there on; race = "
           "not reached by this run.", ""]
    out.append("| Metric | " + " | ".join(name for name, *_ in phases(swaps)) + " | race |")
    out.append("|---|" + "---|" * (len(phases(swaps)) + 1))
    rows = {k: [] for k in ("Screen", "Swaps", "Presented FPS (mean of ~2 s windows)", "FPS min / max window",
                            "Frame time ms (mean)", "Frame time ms (worst window)", "Per-frame time p99", "GPU busy ms per frame",
                            "Fence wait ms per frame", "Draws per frame (mean / max)", "Resolves per frame (mean)",
                            "Texture upload MB per frame (mean / max)", "Render targets created (key: WxH, samples)")}
    for name, lo, hi in phases(swaps):
        sel = [s for s in swaps if lo <= s[0] <= hi]
        rates = window_rates(audio, lo, hi)
        fps = statistics.mean(rates) if rates else None
        rows["Screen"].append("legal screen + intro video" if name == "intro" else "front end (welcome / menus)")
        rows["Swaps"].append(f"{lo}-{hi}")
        rows["Presented FPS (mean of ~2 s windows)"].append(fmt(fps, 2))
        rows["FPS min / max window"].append(f"{fmt(min(rates), 2)} / {fmt(max(rates), 2)}" if rates else "unavailable")
        rows["Frame time ms (mean)"].append(fmt(1000.0 / fps if fps else None))
        rows["Frame time ms (worst window)"].append(fmt(1000.0 / min(rates) if rates else None))
        rows["Per-frame time p99"].append("unavailable (no per-frame clock)")
        rows["GPU busy ms per frame"].append("unavailable (not instrumented)")
        rows["Fence wait ms per frame"].append("unavailable (not instrumented)")
        draws = [s[1] for s in sel]
        rows["Draws per frame (mean / max)"].append(f"{statistics.mean(draws):.1f} / {max(draws)}" if draws else "unavailable")
        rows["Resolves per frame (mean)"].append(f"{statistics.mean(s[2] for s in sel):.1f}" if sel else "unavailable")
        mb = [s[4] / 1e6 for s in sel]
        rows["Texture upload MB per frame (mean / max)"].append(f"{statistics.mean(mb):.2f} / {max(mb):.2f}" if mb else "unavailable")
        made = [t for t in targets if lo <= t[0] <= hi]
        rows["Render targets created (key: WxH, samples)"].append(
            "; ".join(f"{k}: {w}x{h}, {s}x" for _, k, w, h, s, _f in made) or "none")
    for key, values in rows.items():
        out.append(f"| {key} | " + " | ".join(values) + " | " + ("not reached" if key == "Screen" else "unavailable") + " |")
    out.append("")
    dest = os.path.join(run_dir, stem + ".perf_baseline.md")
    with open(dest, "w") as f:
        f.write("\n".join(out))
    index = {}
    for i, (name, *_r) in enumerate(phases(swaps)):
        index[name] = [os.path.relpath(log_path), commit or "unavailable", mode or "unavailable",
                       ", ".join(diags + (["FM4_CP_TRACE"] if os.path.exists(os.path.join(evidence, "cp.txt")) else [])) or "none"] + \
            [rows[k][i] for k in ("Swaps", "Presented FPS (mean of ~2 s windows)", "Frame time ms (mean)", "Frame time ms (worst window)",
                                  "Draws per frame (mean / max)", "Resolves per frame (mean)", "Texture upload MB per frame (mean / max)")]
    return dest, index


def main(argv):
    args = argv[1:]
    index_path = None
    if args[:1] == ["--index"]:
        if len(args) < 2:
            print("usage: perf_baseline.py [--index <out.md>] <run dir> [<run dir> ...]", file=sys.stderr)
            return 2
        index_path, args = args[1], args[2:]
    if not args:
        print("usage: perf_baseline.py [--index <out.md>] <run dir> [<run dir> ...]", file=sys.stderr)
        return 2
    tables = {"intro": [], "menu": [], "race": []}
    for run_dir in args:
        logs = sorted(n for n in os.listdir(run_dir) if re.fullmatch(r"run\d+\.err", n))
        if not logs:
            print(f"{run_dir}: no runN.err", file=sys.stderr)
            return 1
        for name in logs:
            dest, index = summarize(run_dir, name)
            print(dest)
            for phase, row in index.items():
                tables[phase].append(row)
    if index_path:
        head = ["Log", "Commit", "Mode", "Diagnostics", "Swaps", "FPS", "Frame ms (mean)", "Frame ms (worst window)", "Draws/frame (mean / max)",
                "Resolves/frame", "Upload MB/frame (mean / max)"]
        out = ["# Performance baselines by phase (scripts/perf_baseline.py; per-log details in each runN.perf_baseline.md)", "",
               "Targets: M7 stable 30 FPS racing (33.3 ms frames); M8 60 FPS (16.7 ms) and <= 14 ms GPU time per race frame. GPU busy, "
               "fence wait and per-frame times are not instrumented in any run so far: unavailable. Rows with diagnostics on are not "
               "representative of timing.", ""]
        for phase in ("intro", "menu", "race"):
            out += [f"## {phase}", ""]
            if not tables[phase]:
                out += ["No run has reached this phase: unavailable.", ""]
                continue
            out += ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
            out += ["| " + " | ".join(r) + " |" for r in tables[phase]] + [""]
        with open(index_path, "w") as f:
            f.write("\n".join(out))
        print(index_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
