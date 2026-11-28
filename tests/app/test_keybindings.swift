// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app's keyboard bindings (app/setup/KeyBindings.swift), headless: the Mac key table, the defaults' file, the game's
// parse rules and messages (runtime/kernel/key_bindings.cpp, the same strings as tests/runtime/test_runtime.cpp), assigning a
// key with and without a conflict, the atomic save and the load. Expected values by hand: the virtual key codes from Carbon
// HIToolbox Events.h (kVK_ANSI_W 0x0D, kVK_Return 0x24, kVK_Delete 0x33, kVK_ForwardDelete 0x75, kVK_Option 0x3A,
// kVK_RightControl 0x3E, kVK_ANSI_KeypadEnter 0x4C, kVK_ANSI_Comma 0x2B; not bindable: kVK_ANSI_Grave 0x32, kVK_ISO_Section 0x0A,
// kVK_CapsLock 0x39, kVK_Command 0x37, kVK_Function 0x3F, kVK_ANSI_KeypadClear 0x47), the names SDL 3.4 gives their scancodes.
// "CONTROL" marks a negative control.
//
//   tests/run_app_tests.sh

import Foundation

@main
enum KeyBindingsTest {
  static var failures = 0

  static func expect(_ ok: Bool, _ name: String) {
    print("\(ok ? "pass" : "FAIL") \(name)")
    if !ok { failures += 1 }
  }

  static func error(_ text: String) -> String? {
    if case .failure(let e) = KeyBindings.parse(text) { return e.message }
    return nil
  }

  static let defaultsFile = "steer_left = A\nsteer_right = D\nthrottle = W\nbrake = S\ndpad_up = Up\ndpad_down = Down\ndpad_left = Left\n"
    + "dpad_right = Right\na = Space\nb = Backspace\nx = Left Shift\ny = Left Ctrl\nstart = Return\nback = Tab\n"
    + "left_shoulder = Q\nright_shoulder = E\ncamera = C\n"

  // Every action has a key and no key is bound twice: the game accepts the file.
  static func valid(_ b: KeyBindings) -> Bool {
    if case .success(let again) = KeyBindings.parse(b.serialized()) { return again == b }
    return false
  }

  static func main() {
    // The Mac key table: 95 keys (26 letters, 10 digits, Return Escape Backspace Tab Space, 10 punctuation keys, F1-F12, 9 navigation
    // keys, keypad / * - + Enter, keypad 0-9, keypad . =, 6 modifiers).
    let codes: [(UInt16, String?)] = [(0x0D, "W"), (0x24, "Return"), (0x33, "Backspace"), (0x75, "Delete"), (0x3A, "Left Option"),
                                      (0x3E, "Right Ctrl"), (0x4C, "Keypad Enter"), (0x2B, ","), (0x32, nil), (0x0A, nil), (0x39, nil),
                                      (0x37, nil), (0x3F, nil), (0x47, nil)]
    expect(macKeyNames.count == 95 && Set(macKeyNames.values).count == 95 && codes.allSatisfy { keyName(forKeyCode: $0.0) == $0.1 },
           "key table: 95 keys, each name once; W Return Delete(Backspace) ForwardDelete(Delete) Option Right-Control Keypad-Enter comma; ` § Caps Lock Command fn Clear not bindable")
    expect(keyActions.map(\.id) == ["steer_left", "steer_right", "throttle", "brake", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "a", "b",
                                    "x", "y", "start", "back", "left_shoulder", "right_shoulder", "camera"],
           "actions: the game's 17 ids in its order")

    // The defaults' file and the game's rules.
    let d = KeyBindings.defaults
    let header = "# FM4 Recomp keyboard bindings, written by FM4 Recomp's Keyboard Controls. Delete this file for the defaults.\n"
    expect(d.serialized() == header + defaultsFile, "defaults: the file is the comment and the game's 18 default lines")
    expect(valid(d) && KeyBindings.parse(defaultsFile.replacingOccurrences(of: "\n", with: "\r\n")) == .success(d),
           "defaults: the file parses back (also with CRLF line ends)")
    expect(error("throttle W\n") == "line 1: expected '<action> = <key>', not 'throttle W'" &&
             error("\n# c\nnitro = N\n") == "line 3: unknown action 'nitro'" && error("throttle = w\n") == "line 1: unknown key 'w'" &&
             error("throttle = CapsLock\n") == "line 1: unknown key 'CapsLock'" && error("throttle =\n") == "line 1: unknown key ''" &&
             error("throttle = W\nbrake = W\n") == "line 2: key 'W' is already bound to 'throttle' (line 1)" &&
             error("b = X\n\nb = X\n") == "line 3: key 'X' is already bound to 'b' (line 1)" &&
             error(String(defaultsFile[..<defaultsFile.range(of: "camera")!.lowerBound])) == "no key for 'camera'" &&
             error("") == "no key for 'steer_left'",
           "parse: the game's errors, the same messages")
    expect(error(defaultsFile) == nil && error(defaultsFile + "camera = V\n") == nil && error(defaultsFile + "camera = C\n") != nil,
           "CONTROL: parse: a second key for an action is valid, the same key twice is not")

    // Assigning: a free key replaces the action's keys; a taken key moves (the other action keeps its other keys, or swaps).
    let free = d.change(assigning: "J", to: "a")
    let freed = d.applying(free)
    expect(free.from == nil && freed.keys(for: "a") == ["J"] && freed.owner(of: "Space") == nil && valid(freed),
           "assign: J to A: A is J, Space is free")
    let swap = d.change(assigning: "S", to: "throttle")
    let swapped = d.applying(swap)
    expect(swap.from == "brake" && swap.fromGets == ["W"] && swapped.keys(for: "throttle") == ["S"] && swapped.keys(for: "brake") == ["W"] && valid(swapped),
           "assign: S (Brake's only key) to Throttle: asks first; Brake gets W (a swap)")
    let move = d.change(assigning: "Backspace", to: "start")
    let moved = d.applying(move)
    expect(move.from == "b" && move.fromGets == ["Return"] && moved.keys(for: "start") == ["Backspace"] && moved.keys(for: "b") == ["Return"] &&
             valid(moved),
           "assign: Backspace (B's only key) to Start: asks first; B takes Start's Return (a swap)")
    let narrow = KeyBindings(keys: d.keys.merging(["b": ["Backspace", "Escape"]]) { $1 })  // Escape is free in the defaults
    let one = narrow.change(assigning: "Escape", to: "b")
    expect(one.from == nil && narrow.applying(one).keys(for: "b") == ["Escape"] && valid(narrow.applying(one)),
           "assign: Escape to B holding Backspace and Escape (its own key): no question; B is Escape only")
    // An action with no key (Y in the game's KEYBOARD CONTROLS) is saved as `none` and read back; an action not named at all is
    // still invalid (the game's key_bindings.cpp ParseKeyBindings: "no key for '<action>'").
    var cleared = d
    cleared.keys["brake"] = []
    expect(valid(cleared) && cleared.serialized().contains("\nbrake = none\n") && d != swapped,
           "an action with no key is saved as none and read back; a swap changes the bindings")
    let missing = d.serialized().replacingOccurrences(of: "brake = S\n", with: "")
    if case .failure(let error) = KeyBindings.parse(missing) { expect(error.message == "no key for 'brake'", "CONTROL: an action not named at all is invalid") }
    else { expect(false, "CONTROL: an action not named at all is invalid") }
    expect(d.summary() == "W throttle, S brake, A/D steer, Space = A, Backspace ⌫ = B, arrows = D-pad" &&
             swapped.summary().hasPrefix("S throttle, W brake,") && freed.summary().contains("J = A"),
           "summary: the launcher's Controls text follows the bindings")

    // The file: an atomic write (no temporary left), the load, and a refused file.
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("fm4 keybindings test \(getpid())")
    let file = dir.appendingPathComponent("keybindings.conf")
    defer { try? FileManager.default.removeItem(at: dir) }
    let none = KeyBindings.load(file)
    expect(none.0 == d && none.1 == nil, "load: no file is the defaults, no problem")
    do {
      try swapped.write(to: file)
      let back = KeyBindings.load(file)
      let names = try FileManager.default.contentsOfDirectory(atPath: dir.path)
      expect(back.0 == swapped && back.1 == nil && names == ["keybindings.conf"], "save: written atomically and loaded back")
      try Data("nitro = N\n".utf8).write(to: file)
      let refused = KeyBindings.load(file)
      expect(refused.0 == d && refused.1 == "keybindings.conf line 1: unknown action 'nitro'", "load: an invalid file is the defaults and the game's message")
    } catch {
      expect(false, "save: \(error)")
    }
    print(failures == 0 ? "all passed" : "\(failures) failed")
    exit(failures == 0 ? 0 : 1)
  }
}
