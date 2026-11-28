// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app: what opening the app does (after setup, launching the app starts the game
// directly; setup or repair shows if the game data is missing). Foundation only, so tests/app/test_launch_decision.swift runs
// it headless. The checks are play.sh's own refusals, in its order, so the app never starts a session play.sh would refuse for
// missing data: setup.sh's completion mark (setup.ok, written last, setup.sh:172; removed when a setup starts, :147), then
// locations.sh loc_game_args (:136-155): Disc 1's three files, the user-data folder (only the internal default may be created),
// Disc 2's files once it was set up; the saved keyboard keys, which the game refuses at start (stop 9) when invalid -- with
// Settings gone from the launcher, Repair is where they can be reset; then play.sh's GPU_STOP refusal (:34-37).

import Foundation

enum LaunchDecision: Equatable {
  case setup             // never set up here: the guided setup from the welcome page
  case repair(String)    // set up, but the recorded data is not all there (moved, deleted, a drive not connected): why
  case blocked(String)   // GPU_STOP from an earlier session: the launcher shows it, nothing starts
  case play              // start the game
}

func launchDecision(_ paths: Paths, exists: (URL) -> Bool = { FileManager.default.fileExists(atPath: $0.path) }) -> LaunchDecision {
  let recorded = exists(paths.locationsFile)
  let loc = Locations.read(paths)
  let data = loc.dataRoot
  if !exists(paths.setupDone) {
    // No record and no completed setup in the default folder: a first run. A record without its mark: the data folder is
    // gone (a drive not connected) or a setup did not finish.
    guard recorded else { return .setup }
    return .repair(exists(data)
      ? "Setup did not finish in \(data.path). Set up again to complete it."
      : "The game data folder \(data.path) is missing. If it is on an external drive, connect it.")
  }
  for file in ["survey/image.bin", "files/default.xex", "survey/disc1_gdfx.tsv"] where !exists(data.appendingPathComponent(file)) {
    return .repair("\(data.appendingPathComponent(file).path) is missing. Set up again with your Disc 1 image.")
  }
  let defaultUserData = paths.home.appendingPathComponent("data/user-data")
  if !exists(loc.userDataRoot) && loc.userDataRoot.standardizedFileURL.path != defaultUserData.standardizedFileURL.path {
    return .repair("Your save folder \(loc.userDataRoot.path) is missing. If it is on an external drive, connect it.")
  }
  if let disc2 = loc.disc2Root, !exists(disc2.appendingPathComponent("default.xex")) {
    return .repair("Disc 2's files in \(disc2.path) are missing. Connect its drive, or set up Disc 2 again.")
  }
  if let problem = KeyBindings.load(paths.keyBindingsFile).1 {
    return .repair("Your saved keyboard keys cannot be used: \(problem). Reset them to the defaults.")
  }
  if exists(paths.gpuStop) {
    return .blocked("An earlier session showed evidence of a GPU hang (\(paths.gpuStop.path)).")
  }
  return .play
}

// After a session the app started on its own (its window hidden: the launcher does not show once the game is
// set up), the app quits when the game ended the normal way: the player closed its window or quit it (the game's stop 14,
// runtime/stops.h WindowClosed) or the session limit ended it (124, play.sh), and play.sh found nothing wrong (its exit 0).
// Anything else (a game stop, a GPU safeguard, play.sh refusing) shows the launcher with the message. `gameEnded`: play.sh's
// "play: the game ended (exit N): ..." line, nil if it never printed one.
func quitsAfterAutoSession(scriptStatus: Int32, gameEnded: String?) -> Bool {
  guard scriptStatus == 0, let line = gameEnded else { return false }
  return gameEndedNormally(line)
}

// play.sh's game-ended line for a normal end (closed, quit, or the session limit): the app leaves the screen at once (no Dock icon,
// no menu bar) while play.sh finishes its GPU checks, then quits (or comes back with the launcher if they found something).
func gameEndedNormally(_ line: String) -> Bool {
  line.hasPrefix("play: the game ended (exit 14)") || line.hasPrefix("play: the game ended (exit 124)")
}
