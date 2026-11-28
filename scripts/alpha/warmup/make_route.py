#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause AND GPL-3.0-or-later
# Copyright (c) 2026, i2kiselev (the route shape, races and menu positions: fm4-static-recomp host/tools/warm-up.ps1, BSD-3-Clause,
# LICENSES/FM4-Static-Recomp-BSD-3-Clause.txt); Copyright (C) 2026 FM4 Recomp contributors (our changes, as the docstring says).
"""All warm-up races in one launch as an FM4_INPUT_SCRIPT with @title and @scene sections (input_script.h). Route shape, races and
menu positions after fm4-static-recomp's warm-up.ps1 (Copyright (c) 2026, i2kiselev, BSD-3-Clause): Free Play > Quick Race; per race:
manufacturer, car, showroom, assists, environment, track, GO RACE, START RACE, the accelerator held with the race cameras cycled,
pause > QUIT RACE, confirm, the post-race menu, CONTINUE (track select); then B, B back to car select (B again to the manufacturers
when the make changes). Our changes: open-loop vblank delays within a section (measured in our test runs), each race
section started by the race scene after the load (@scene race; the track select after CONTINUE comes within ~200 vblanks, and some
track selects draw like a race, so the next race's menus follow CONTINUE in the same section); the route's last event marks its end.
usage: make_route.py <races.tsv> [race number ...] > route.txt (scripts/release/bundle_kit.sh writes the kit's warmup/route.txt)"""
import sys

rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")]
pick = [int(n) for n in sys.argv[2:]] or [int(r[0]) for r in rows]
races = [(int(r[1]), int(r[2]), int(r[3]), r[4]) for n in pick for r in rows if int(r[0]) == n]

out = ["# Shader warm-up: %d races in one launch (%s)" % (len(races), ", ".join(r[3] for r in races)), "180 START", "188"]
for v in range(1800, 4501, 300):  # A taps until the title takes one
    out += [f"{v} A", f"{v + 8}"]

class List:
    def __init__(self, header):
        self.ev = []; self.header = header
    def press(self, v, b):
        self.ev += [(v, b), (v + 8, "")]
    def moves(self, t, n, b="DPAD_RIGHT"):
        for i in range(n):
            self.press(t + 20 * i, b)
        return t + 20 * max(n - 1, 0)
    def emit(self):
        out.append(self.header)
        out.extend(f"{v} {b}".rstrip() for v, b in self.ev)

def to_track(m, t, model_from, model, env):
    """From car select (cursor on model_from) to GO RACE; returns the time of the GO press."""
    t = m.moves(t, model - model_from) + 280; m.press(t, "A")      # car -> showroom
    t += 600; m.press(t, "A")                                      # showroom -> assists
    t += 600; m.press(t, "A")                                      # assists -> environment select (from 0)
    t = m.moves(t + 400, env) + 240; m.press(t, "A")               # environment -> track select
    t += 600; m.press(t, "A")                                      # track -> GO RACE
    return t

make = model = None
lists = []
for i, (mk, md, env, label) in enumerate(races):
    if i == 0:
        m = List("@title")
        for v in (600, 700, 800):
            m.press(v, "DPAD_RIGHT")
        m.press(900, "A"); m.press(1150, "DPAD_RIGHT"); m.press(1300, "DPAD_RIGHT"); m.press(1500, "A")  # Free Play, Quick Race
        t = m.moves(2000, mk - 1) + 340; m.press(t, "A")          # manufacturer (Abarth 1) -> car select, first car
        to_track(m, t + 400, 0, md, env)
    else:
        m = lists[-1]                                              # after CONTINUE (the previous race's list): track select
        t = m.ev[-1][0] + 600
        m.press(t, "B"); m.press(t + 600, "B")                     # environment select, car select (cursor on the last car)
        t += 1200
        if mk != make:
            m.press(t, "B")                                        # manufacturer select (cursor on the last make)
            t = m.moves(t + 600, mk - make) + 340; m.press(t, "A") # -> car select, first car
            model, t = 0, t + 400
        to_track(m, t, model, md, env)
    if i == 0:
        lists.append(m)
    make, model = mk, md
    r = List(f"@scene race 120  # race {i + 1}: {label}")          # the pre-race scene after the load
    for v in range(600, 4200, 600):                                # pre-race menu, START RACE: pairs of taps
        r.press(v, "A"); r.press(v + 120, "A")
    r.ev.append((4600, "RT"))
    for k in range(4):                                             # BACK cycles the race cameras, each with shaders of its own
        r.ev += [(5000 + 300 * k, "RT BACK"), (5008 + 300 * k, "RT")]
    r.ev.append((6200, ""))
    r.press(6400, "START")                                         # pause menu
    r.moves(7000, 6, "DPAD_DOWN")                                  # QUIT RACE
    r.press(7500, "A")                                             # confirm
    r.press(8100, "A")                                             # post-race menu over a replay
    r.press(9000, "A")                                             # CONTINUE -> track select
    lists.append(r)
last = lists[-1]
last.ev.append((last.ev[-1][0] + 1300, ""))  # the route's end, ~20 s of menu later: a pipeline-cache save (every 600 swaps) has passed
for l in lists:
    l.emit()
print("\n".join(out))
