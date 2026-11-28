// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app: the game's keyboard bindings (Keyboard Controls, in the game: ControlsPanel.swift), without SwiftUI so
// tests/app/test_keybindings.swift runs it headless. The file is $FM4_RECOMP_HOME/keybindings.conf, read by the game
// (runtime/kernel/key_bindings.h owns the format: one `action = key` line per key, SDL scancode names, every action at least one
// key, a key at most one action); play.sh passes it when it exists, so no file is the defaults, and the game re-reads it when
// Keyboard Controls closes. The app is its only writer.
//
// Keys: a Mac keyboard's virtual key codes (Carbon HIToolbox Events.h kVK_*) as SDL 3 names them (SDL 3.4
// src/events/scancodes_darwin.h maps each code to its scancode, SDL_keymap.c names it); the same set as key_bindings.cpp's
// kKeyNames (tests/test_key_tables.py compares the two). Left out: ` and the ISO section key (SDL swaps them on ISO keyboards),
// Caps Lock (a toggle), Command (system shortcuts), fn, the clear key and F13 and up.

import Carbon.HIToolbox
import Foundation

struct KeyAction {
  let id: String       // key_bindings.cpp kActions, in its order
  let label: String
  let gamepad: String  // the pad's control for the same action (fixed)
}

let keyActions: [KeyAction] = [
  KeyAction(id: "steer_left", label: "Steer left", gamepad: "Left stick"),
  KeyAction(id: "steer_right", label: "Steer right", gamepad: "Left stick"),
  KeyAction(id: "throttle", label: "Throttle", gamepad: "Right trigger"),
  KeyAction(id: "brake", label: "Brake", gamepad: "Left trigger"),
  KeyAction(id: "dpad_up", label: "D-pad up", gamepad: "D-pad up"),
  KeyAction(id: "dpad_down", label: "D-pad down", gamepad: "D-pad down"),
  KeyAction(id: "dpad_left", label: "D-pad left", gamepad: "D-pad left"),
  KeyAction(id: "dpad_right", label: "D-pad right", gamepad: "D-pad right"),
  KeyAction(id: "a", label: "A (confirm)", gamepad: "A"),
  KeyAction(id: "b", label: "B (back)", gamepad: "B"),
  KeyAction(id: "x", label: "X", gamepad: "X"),
  KeyAction(id: "y", label: "Y", gamepad: "Y"),
  KeyAction(id: "start", label: "Start", gamepad: "Start (Menu)"),
  KeyAction(id: "back", label: "Back", gamepad: "Back (View)"),
  KeyAction(id: "left_shoulder", label: "Left shoulder", gamepad: "LB"),
  KeyAction(id: "right_shoulder", label: "Right shoulder", gamepad: "RB"),
  KeyAction(id: "camera", label: "Camera", gamepad: "Right stick click"),
]

func actionLabel(_ id: String) -> String { keyActions.first { $0.id == id }?.label ?? id }

// kVK_* -> SDL scancode name.
let macKeyNames: [Int: String] = [
  kVK_ANSI_A: "A", kVK_ANSI_B: "B", kVK_ANSI_C: "C", kVK_ANSI_D: "D", kVK_ANSI_E: "E", kVK_ANSI_F: "F", kVK_ANSI_G: "G",
  kVK_ANSI_H: "H", kVK_ANSI_I: "I", kVK_ANSI_J: "J", kVK_ANSI_K: "K", kVK_ANSI_L: "L", kVK_ANSI_M: "M", kVK_ANSI_N: "N",
  kVK_ANSI_O: "O", kVK_ANSI_P: "P", kVK_ANSI_Q: "Q", kVK_ANSI_R: "R", kVK_ANSI_S: "S", kVK_ANSI_T: "T", kVK_ANSI_U: "U",
  kVK_ANSI_V: "V", kVK_ANSI_W: "W", kVK_ANSI_X: "X", kVK_ANSI_Y: "Y", kVK_ANSI_Z: "Z",
  kVK_ANSI_1: "1", kVK_ANSI_2: "2", kVK_ANSI_3: "3", kVK_ANSI_4: "4", kVK_ANSI_5: "5", kVK_ANSI_6: "6", kVK_ANSI_7: "7",
  kVK_ANSI_8: "8", kVK_ANSI_9: "9", kVK_ANSI_0: "0",
  kVK_Return: "Return", kVK_Escape: "Escape", kVK_Delete: "Backspace", kVK_Tab: "Tab", kVK_Space: "Space",
  kVK_ANSI_Minus: "-", kVK_ANSI_Equal: "=", kVK_ANSI_LeftBracket: "[", kVK_ANSI_RightBracket: "]", kVK_ANSI_Backslash: "\\",
  kVK_ANSI_Semicolon: ";", kVK_ANSI_Quote: "'", kVK_ANSI_Comma: ",", kVK_ANSI_Period: ".", kVK_ANSI_Slash: "/",
  kVK_F1: "F1", kVK_F2: "F2", kVK_F3: "F3", kVK_F4: "F4", kVK_F5: "F5", kVK_F6: "F6", kVK_F7: "F7", kVK_F8: "F8",
  kVK_F9: "F9", kVK_F10: "F10", kVK_F11: "F11", kVK_F12: "F12",
  kVK_Home: "Home", kVK_PageUp: "PageUp", kVK_ForwardDelete: "Delete", kVK_End: "End", kVK_PageDown: "PageDown",
  kVK_RightArrow: "Right", kVK_LeftArrow: "Left", kVK_DownArrow: "Down", kVK_UpArrow: "Up",
  kVK_ANSI_KeypadDivide: "Keypad /", kVK_ANSI_KeypadMultiply: "Keypad *", kVK_ANSI_KeypadMinus: "Keypad -",
  kVK_ANSI_KeypadPlus: "Keypad +", kVK_ANSI_KeypadEnter: "Keypad Enter",
  kVK_ANSI_Keypad1: "Keypad 1", kVK_ANSI_Keypad2: "Keypad 2", kVK_ANSI_Keypad3: "Keypad 3", kVK_ANSI_Keypad4: "Keypad 4",
  kVK_ANSI_Keypad5: "Keypad 5", kVK_ANSI_Keypad6: "Keypad 6", kVK_ANSI_Keypad7: "Keypad 7", kVK_ANSI_Keypad8: "Keypad 8",
  kVK_ANSI_Keypad9: "Keypad 9", kVK_ANSI_Keypad0: "Keypad 0", kVK_ANSI_KeypadDecimal: "Keypad .", kVK_ANSI_KeypadEquals: "Keypad =",
  kVK_Control: "Left Ctrl", kVK_Shift: "Left Shift", kVK_Option: "Left Option",
  kVK_RightControl: "Right Ctrl", kVK_RightShift: "Right Shift", kVK_RightOption: "Right Option",
]

private let bindableKeys = Set(macKeyNames.values)

func keyName(forKeyCode code: UInt16) -> String? { macKeyNames[Int(code)] }

// How a Mac keyboard labels the key (the file keeps SDL's name).
func keyLabel(_ name: String) -> String {
  switch name {
  case "Backspace": return "Backspace ⌫"
  case "Delete": return "Forward Delete ⌦"
  case "Escape": return "Esc"
  case "Up": return "↑"
  case "Down": return "↓"
  case "Left": return "←"
  case "Right": return "→"
  case "Left Ctrl": return "Left Control"
  case "Right Ctrl": return "Right Control"
  default: return name
  }
}

struct KeyBindings: Equatable {
  var keys: [String: [String]]  // action id -> SDL key names

  // The game's defaults (key_bindings.cpp DefaultKeyBindings).
  static let defaults = KeyBindings(keys: [
    "steer_left": ["A"], "steer_right": ["D"], "throttle": ["W"], "brake": ["S"],
    "dpad_up": ["Up"], "dpad_down": ["Down"], "dpad_left": ["Left"], "dpad_right": ["Right"],
    "a": ["Space"], "b": ["Backspace"], "x": ["Left Shift"], "y": ["Left Ctrl"], "start": ["Return"], "back": ["Tab"],
    "left_shoulder": ["Q"], "right_shoulder": ["E"], "camera": ["C"],
  ])

  func keys(for action: String) -> [String] { keys[action] ?? [] }

  func owner(of key: String) -> String? { keyActions.first { keys(for: $0.id).contains(key) }?.id }

  // Assigning a key to an action replaces that action's keys with it. The key leaves the action that had it (`from`); if that
  // leaves `from` with no key, `from` takes the selected action's previous keys instead (a swap), so every action keeps a key.
  struct Change: Equatable {
    let action: String
    let key: String
    let from: String?        // the other action that has the key now, nil if none
    let fromGets: [String]   // its keys afterwards
  }

  func change(assigning key: String, to action: String) -> Change {
    let previous = keys(for: action).filter { $0 != key }
    guard let from = owner(of: key), from != action else { return Change(action: action, key: key, from: nil, fromGets: []) }
    let rest = keys(for: from).filter { $0 != key }
    return Change(action: action, key: key, from: from, fromGets: rest.isEmpty ? previous : rest)
  }

  func applying(_ change: Change) -> KeyBindings {
    var result = self
    result.keys[change.action] = [change.key]
    if let from = change.from { result.keys[from] = change.fromGets }
    return result
  }

  // The file: a comment, then one `action = key` line per key, in action order.
  func serialized() -> String {
    var text = "# FM4 Recomp keyboard bindings, written by FM4 Recomp's Keyboard Controls. Delete this file for the defaults.\n"
    for action in keyActions {
      if keys(for: action.id).isEmpty { text += "\(action.id) = none\n" }
      for key in keys(for: action.id) { text += "\(action.id) = \(key)\n" }
    }
    return text
  }

  // The game's rules (key_bindings.cpp ParseKeyBindings), with its messages.
  static func parse(_ text: String) -> Result<KeyBindings, BindingError> {
    var keys: [String: [String]] = [:]
    var owners: [String: (action: String, line: Int)] = [:]
    let ids = Set(keyActions.map(\.id))
    func trim<S: StringProtocol>(_ s: S) -> String { String(s).trimmingCharacters(in: CharacterSet(charactersIn: " \t\r")) }
    // components(separatedBy:) splits on UTF-16 units, so "\r\n" (one Swift Character) splits too, as in the game's parser.
    for (index, raw) in text.components(separatedBy: "\n").enumerated() {
      let number = index + 1
      let line = trim(raw)
      if line.isEmpty || line.hasPrefix("#") { continue }
      guard let eq = line.firstIndex(of: "=") else { return .failure(BindingError("line \(number): expected '<action> = <key>', not '\(line)'")) }
      let action = trim(line[line.startIndex..<eq]), key = trim(line[line.index(after: eq)...])
      guard ids.contains(action) else { return .failure(BindingError("line \(number): unknown action '\(action)'")) }
      if key == "none" { keys[action, default: []] += []; continue }  // named, no key (cleared in the game's KEYBOARD CONTROLS)
      guard bindableKeys.contains(key) else { return .failure(BindingError("line \(number): unknown key '\(key)'")) }
      if let owner = owners[key] {
        return .failure(BindingError("line \(number): key '\(key)' is already bound to '\(owner.action)' (line \(owner.line))"))
      }
      owners[key] = (action, number)
      keys[action, default: []].append(key)
    }
    if let missing = keyActions.first(where: { keys[$0.id] == nil }) { return .failure(BindingError("no key for '\(missing.id)'")) }
    return .success(KeyBindings(keys: keys))
  }

  // No file: the defaults. A file that cannot be read or is invalid: the defaults and why (the game would refuse that file).
  static func load(_ url: URL) -> (KeyBindings, String?) {
    guard FileManager.default.fileExists(atPath: url.path) else { return (defaults, nil) }
    guard let text = try? String(contentsOf: url, encoding: .utf8) else { return (defaults, "\(url.lastPathComponent) cannot be read") }
    switch parse(text) {
    case .success(let bindings): return (bindings, nil)
    case .failure(let error): return (defaults, "\(url.lastPathComponent) \(error.message)")
    }
  }

  func write(to url: URL) throws {
    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try Data(serialized().utf8).write(to: url, options: .atomic)
  }

  // The launcher's Controls card.
  func summary() -> String {
    let k = { (action: String) in keys(for: action).map(keyLabel).joined(separator: "/") }
    let dpad = ["dpad_up", "dpad_down", "dpad_left", "dpad_right"]
    let arrows = dpad.allSatisfy { keys(for: $0) == KeyBindings.defaults.keys(for: $0) } ? "arrows" : dpad.map(k).joined(separator: " ")
    return "\(k("throttle")) throttle, \(k("brake")) brake, \(k("steer_left"))/\(k("steer_right")) steer, \(k("a")) = A, \(k("b")) = B, \(arrows) = D-pad"
  }
}

struct BindingError: Error, Equatable {
  let message: String
  init(_ message: String) { self.message = message }
}
