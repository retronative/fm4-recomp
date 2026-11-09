#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4_PASS_MERGE (renderer.cpp) leaves a draw's render pass open for the next draw. Every command the EDRAM and texture units
# record outside a render pass must first end it (render_hooks.h OutsideRenderPass); a missed site records a barrier, copy or
# clear inside a render pass (invalid Vulkan; the validation messenger would stop the run). This check holds the units to it:
# each vkCmdPipelineBarrier / vkCmdBeginRenderPass directly follows an OutsideRenderPass call, and each copy or clear follows a
# barrier helper (Transition / Barrier, which call it) or OutsideRenderPass within its block. With a negative control.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_pass_merge_sites.py
import pathlib, re, unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
UNITS = [ROOT / 'runtime/video/edram_vk.cpp', ROOT / 'runtime/video/texture_vk.cpp']
DIRECT = re.compile(r'\bvkCmd(PipelineBarrier|BeginRenderPass)\(')
TRANSFER = re.compile(r'\bvkCmd(Copy\w+|Clear(Color|DepthStencil)Image)\(')
GUARD = re.compile(r'\b(OutsideRenderPass|Transition|Barrier|Rest)\(')
# Functions that only run with no draw recorded before them in the command buffer (start-up self-checks).
EXEMPT_FUNCTIONS = {'RecordDepthSelfCheck'}


def violations(text):
    found, lines, function = [], text.splitlines(), None
    for i, line in enumerate(lines):
        if m := re.match(r'^\w[\w:<>,\s\*&]*\s(\w+)\(.*\)\s*$', line):
            function = m[1]
        code = line.split('//', 1)[0]
        if function in EXEMPT_FUNCTIONS:
            continue
        if DIRECT.search(code):
            previous = next((l for l in reversed(lines[:i]) if l.strip()), '')
            if 'OutsideRenderPass(' not in previous:
                found.append(f'{i + 1}: {line.strip()}')
        elif TRANSFER.search(code):
            window = lines[max(0, i - 25):i]
            if not any(GUARD.search(l.split('//', 1)[0]) for l in window):
                found.append(f'{i + 1}: {line.strip()}')
    return found


class PassMergeSites(unittest.TestCase):
    def test_units_end_an_open_pass_first(self):
        for path in UNITS:
            with self.subTest(path=path.name):
                self.assertEqual(violations(path.read_text()), [])

    def test_control_missing_call_is_found(self):
        snippet = 'void F(VkCommandBuffer c)\n{\n    int x = 0;\n    vkCmdPipelineBarrier(c, a, b, 0, 0, nullptr, 0, nullptr, 1, &x);\n' \
                  '    vkCmdCopyImageToBuffer(c, i, l, s, 1, &copy);\n}\n'
        self.assertEqual(len(violations(snippet)), 2)

    def test_control_guarded_sites_pass(self):
        snippet = 'void F(VkCommandBuffer c)\n{\n    OutsideRenderPass(c);\n    vkCmdPipelineBarrier(c, a, b, 0, 0, nullptr, 0, nullptr, 1, &x);\n' \
                  '    Transition(c, t, l, s, a);\n    vkCmdCopyImageToBuffer(c, i, l, s, 1, &copy);\n}\n'
        self.assertEqual(violations(snippet), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
