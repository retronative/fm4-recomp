// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// What opening FM4 Recomp.app does (app/setup/LaunchDecision.swift), headless, on real folders in a temporary home with a space
// in its path. Expected values from the kit's own rules, not from the function: setup.sh writes setup.ok last (setup.sh:172) and
// records locations.conf (locations.sh loc_set); play.sh refuses a missing Disc 1 file, a missing user-data folder on a chosen
// drive (the internal default is created) and a recorded Disc 2 without its files (locations.sh:136-155), and refuses to start
// while GPU_STOP exists (play.sh:34-37); the game stops (9) on an invalid keybindings.conf (runtime/kernel/key_bindings.cpp
// LoadKeyBindings), with the message KeyBindings.parse reproduces. "CONTROL" marks a negative control.
//
//   tests/run_app_tests.sh

import Foundation

@main
enum LaunchDecisionTest {
  static var failures = 0

  static func expect(_ ok: Bool, _ name: String) {
    print("\(ok ? "pass" : "FAIL") \(name)")
    if !ok { failures += 1 }
  }

  static func touch(_ url: URL) {
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    FileManager.default.createFile(atPath: url.path, contents: Data())
  }

  static func isRepair(_ d: LaunchDecision, mentioning text: String) -> Bool {
    if case .repair(let why) = d { return why.contains(text) }
    return false
  }

  static func main() {
    let fm = FileManager.default
    let root = fm.temporaryDirectory.appendingPathComponent("fm4 launch test \(getpid())")
    defer { try? fm.removeItem(at: root) }
    let home = root.appendingPathComponent("home")
    let kit = root.appendingPathComponent("kit")
    let paths = Paths(kit: kit, home: home)
    let data = home.appendingPathComponent("data")

    // A first run: nothing recorded.
    expect(launchDecision(paths) == .setup, "first run: no locations.conf, no setup.ok: the guided setup")

    // A completed default setup (what setup.sh leaves): locations.conf, the three Disc 1 files, setup.ok, the user-data folder.
    touch(home.appendingPathComponent("locations.conf"))
    try? "version=1\ndata_root=\(data.path)\nuser_data_root=\(data.path)/user-data\n".write(to: paths.locationsFile, atomically: true, encoding: .utf8)
    let disc1 = ["survey/image.bin", "files/default.xex", "survey/disc1_gdfx.tsv"].map { data.appendingPathComponent($0) }
    disc1.forEach(touch)
    try? fm.createDirectory(at: data.appendingPathComponent("user-data"), withIntermediateDirectories: true)
    expect(launchDecision(paths) == .repair("Setup did not finish in \(data.path). Set up again to complete it."),
           "recorded but no setup.ok (an interrupted setup): repair, naming the folder")
    touch(data.appendingPathComponent("setup.ok"))
    expect(launchDecision(paths) == .play, "set up: the game starts at once")

    // Each Disc 1 file missing in turn: repair naming it.
    for file in disc1 {
      try? fm.removeItem(at: file)
      expect(isRepair(launchDecision(paths), mentioning: file.path), "missing \(file.lastPathComponent): repair naming it")
      touch(file)
    }
    expect(launchDecision(paths) == .play, "CONTROL: all files back: play again")

    // The internal default user-data folder may be missing (play.sh creates it); a chosen one may not.
    try? fm.removeItem(at: data.appendingPathComponent("user-data"))
    expect(launchDecision(paths) == .play, "the internal user-data folder missing: still play (play.sh creates it)")
    let ssd = root.appendingPathComponent("Volumes/Extreme SSD/FM4 Recomp")
    try? "version=1\ndata_root=\(data.path)\nuser_data_root=\(ssd.path)/user-data\n".write(to: paths.locationsFile, atomically: true, encoding: .utf8)
    expect(isRepair(launchDecision(paths), mentioning: "\(ssd.path)/user-data"), "a chosen user-data folder missing (drive not connected): repair")
    try? fm.createDirectory(at: ssd.appendingPathComponent("user-data"), withIntermediateDirectories: true)
    expect(launchDecision(paths) == .play, "CONTROL: the drive connected: play")

    // The data folder on a drive that is not connected: no setup.ok can be seen, but locations.conf names it.
    let away = root.appendingPathComponent("Volumes/Gone/FM4 Recomp/data")
    try? "version=1\ndata_root=\(away.path)\nuser_data_root=\(ssd.path)/user-data\n".write(to: paths.locationsFile, atomically: true, encoding: .utf8)
    expect(launchDecision(paths) == .repair("The game data folder \(away.path) is missing. If it is on an external drive, connect it."),
           "the data folder's drive not connected: repair, not the first-run setup")
    try? "version=1\ndata_root=\(data.path)\nuser_data_root=\(ssd.path)/user-data\n".write(to: paths.locationsFile, atomically: true, encoding: .utf8)

    // Disc 2 recorded but gone.
    let disc2 = root.appendingPathComponent("Volumes/Extreme SSD/FM4 Recomp/disc2")
    try? "version=1\ndata_root=\(data.path)\nuser_data_root=\(ssd.path)/user-data\ndisc2_root=\(disc2.path)\n".write(to: paths.locationsFile, atomically: true, encoding: .utf8)
    expect(isRepair(launchDecision(paths), mentioning: disc2.path), "Disc 2 recorded, its files missing: repair")
    touch(disc2.appendingPathComponent("default.xex"))
    expect(launchDecision(paths) == .play, "CONTROL: Disc 2's files there: play")

    // Saved keys the game would refuse (stop 9 at start): repair (Reset Keys there); none or valid: play.
    try? "nitro = N\n".write(to: paths.keyBindingsFile, atomically: true, encoding: .utf8)
    expect(isRepair(launchDecision(paths), mentioning: "keybindings.conf line 1: unknown action 'nitro'"), "invalid keybindings.conf: repair naming the game's error")
    try? KeyBindings.defaults.write(to: paths.keyBindingsFile)
    expect(launchDecision(paths) == .play, "CONTROL: a valid keybindings.conf: play")
    try? fm.removeItem(at: paths.keyBindingsFile)

    // GPU_STOP blocks the automatic start; the launcher shows it.
    touch(paths.gpuStop)
    if case .blocked(let why) = launchDecision(paths) { expect(why.contains(paths.gpuStop.path), "GPU_STOP: blocked, naming the file") }
    else { expect(false, "GPU_STOP: blocked, naming the file") }
    try? fm.removeItem(at: paths.gpuStop)
    expect(launchDecision(paths) == .play, "CONTROL: GPU_STOP removed by the player: play")

    // After an automatic session (play.sh's lines, play.sh:122): the window closed (stop 14) or the session limit (124) quits the
    // app; a game stop (e.g. 9), a play.sh failure (3: GPU hang evidence) or no "game ended" line shows the launcher.
    let closed = "play: the game ended (exit 14): [stop 14] the window was closed by the user (SDL_EVENT_WINDOW_CLOSE_REQUESTED)"
    expect(quitsAfterAutoSession(scriptStatus: 0, gameEnded: closed), "auto session: window closed: the app quits")
    expect(quitsAfterAutoSession(scriptStatus: 0, gameEnded: "play: the game ended (exit 124): no stop line"), "auto session: session limit: the app quits")
    expect(!quitsAfterAutoSession(scriptStatus: 0, gameEnded: "play: the game ended (exit 9): [stop 9] user data: ..."), "CONTROL: auto session: a game stop shows the launcher")
    expect(!quitsAfterAutoSession(scriptStatus: 3, gameEnded: closed), "CONTROL: auto session: GPU hang evidence shows the launcher")
    expect(!quitsAfterAutoSession(scriptStatus: 0, gameEnded: nil), "CONTROL: auto session: no game-ended line shows the launcher")
    expect(!quitsAfterAutoSession(scriptStatus: 0, gameEnded: "play: the game ended (exit 140): ..."), "CONTROL: auto session: exit 140 is not 14")
    // The app leaves the screen (no Dock icon) on the game-ended line of a normal end, before play.sh's checks finish.
    expect(gameEndedNormally("play: the game ended (exit 14): [stop 14] the window was closed by the user (Command-Q)") &&
           gameEndedNormally("play: the game ended (exit 124): no stop line"), "auto session: a normal end hides the app at once")
    expect(!gameEndedNormally("play: the game ended (exit 9): [stop 9] user data: ...") && !gameEndedNormally("play: the game ended (exit 140): ..."),
           "CONTROL: auto session: a stop keeps the app (its launcher shows the error)")

    print(failures == 0 ? "all passed" : "\(failures) failed")
    exit(failures == 0 ? 0 : 1)
  }
}
