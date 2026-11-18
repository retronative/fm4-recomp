# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The keyboard bindings' two tables agree: every key the app can capture (app/setup/KeyBindings.swift macKeyNames) is a key the
# game accepts (runtime/kernel/key_bindings.cpp kKeyNames) and the other way round, and both list the same 17 action ids in the
# same order. Read from the sources; nothing is built.
#
#   /opt/homebrew/bin/python3 -B -I tests/test_key_tables.py      (<1 s)
import os
import re
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
STRING = r'"((?:[^"\\]|\\.)*)"'


def block(text, start):
    """The text from `start` to the end of its table (C++ `};`, Swift `]` at a line start)."""
    at = text.index(start)
    end = min(i for i in (text.find("\n};", at), text.find("\n]", at)) if i >= 0)
    return text[at:end]


def unescape(s):
    return re.sub(r"\\(.)", r"\1", s)


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


class KeyTables(unittest.TestCase):
    def setUp(self):
        self.cpp = read("runtime/kernel/key_bindings.cpp")
        self.swift = read("app/setup/KeyBindings.swift")

    def test_keys(self):
        game = [unescape(n) for n, _ in re.findall(r"\{" + STRING + r", (\d+)\}", block(self.cpp, "const KeyName kKeyNames[]"))]
        app = [unescape(n) for n in re.findall(r"kVK_\w+: " + STRING, block(self.swift, "let macKeyNames"))]
        self.assertEqual(len(game), 95)                       # hand count (tests/app/test_keybindings.swift)
        self.assertEqual(len(set(game)), len(game))
        self.assertEqual(len(set(app)), len(app))
        self.assertEqual(set(app), set(game))
        self.assertIn("\\", game)                              # CONTROL: the escaped backslash key is read as one character
        self.assertIn(",", app)

    def test_actions(self):
        game = re.findall(r'\{"(\w+)", &Keys::', block(self.cpp, "const Action kActions"))
        app = re.findall(r'KeyAction\(id: "(\w+)"', block(self.swift, "let keyActions"))
        self.assertEqual(len(game), 17)
        self.assertEqual(app, game)


if __name__ == "__main__":
    unittest.main(verbosity=2)
