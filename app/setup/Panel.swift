// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp --disc-panel 2 [--frame <x,y,w,h>] [--message <text>] [--problem <text>]: the launcher's
// own Disc 2 setup (the Disc 2 page: drop or choose the ISO, where its files go, the free space; then the setup's progress with
// Cancel; a failure with Try Again) as a floating panel over the game's "Insert Disc 2" screen. fm4recomp starts it
// (runtime/disc_setup_sdl.cpp) when the game asks for a Disc 2 that is not set up, reads its lines (setup.sh's, passed on) and
// its exit status: 0 once Disc 2 is set up (the game then mounts it, completes its swap and shows "Install Additional Content",
// confirmed with A as on the console), 6 when the player goes back to the game without it.
//   --frame: the game window in points, top-left origin on the main display (SDL's); the panel opens centred over it.
//   --message: the game's own text; --problem: open on that failure (the game could not use a disc that was set up).
// The panel never takes the game's input: a non-activating panel (clicks, drops and buttons work while the game stays the
// active app, so its keyboard and gamepad keep working, and its Back still cancels), focus handed back after a file dialog
// (SetupModel.returnFocus). It ends with the game: a game that stops closes it (its setup tool stops too: fm4recomp ends the
// panel's process group on a cancel; here, the parent's exit).

import AppKit
import SwiftUI

enum DiscPanel {
  static func run(_ args: [String]) -> Never {
    var disc = "", frame: CGRect?, message = "", problem = ""
    var i = 0
    while i < args.count {
      let value = i + 1 < args.count ? args[i + 1] : nil
      switch (i, args[i], value) {
      case (0, let d, _): disc = d; i += 1; continue
      case (_, "--frame", let v?):
        let n = v.split(separator: ",").compactMap { Double($0) }
        if n.count == 4 { frame = CGRect(x: n[0], y: n[1], width: n[2], height: n[3]) }
      case (_, "--message", let v?): message = v
      case (_, "--problem", let v?): problem = v
      default: print("usage: FM4 Recomp --disc-panel 2 [--frame <x,y,w,h>] [--message <text>] [--problem <text>]"); exit(2)
      }
      i += 2
    }
    guard disc == "2" else { print("FM4 Recomp --disc-panel: only Disc 2 has a setup"); exit(2) }
    MainActor.assumeIsolated { open(frame: frame, message: message, problem: problem) }
    NSApplication.shared.run()
    exit(6)
  }

  @MainActor private static var window: NSPanel?
  @MainActor private static var parentWatch: DispatchSourceProcess?

  @MainActor private static func open(frame: CGRect?, message: String, problem: String) {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)  // no Dock icon, no menu bar of its own: the game stays the app in front
    let model = SetupModel(paths: Paths.standard())
    model.panel = true
    model.gameMessage = message
    if problem.isEmpty {
      model.page = .disc2
    } else {
      model.installingDisc2 = true
      model.failure = problem
      model.page = .failed
    }
    let size = NSSize(width: 720, height: 560)  // RootView's size
    let panel = NSPanel(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled, .fullSizeContentView, .nonactivatingPanel],
                        backing: .buffered, defer: false)
    panel.titleVisibility = .hidden
    panel.titlebarAppearsTransparent = true
    panel.isMovableByWindowBackground = true
    panel.hidesOnDeactivate = false  // stays up while the game is the active app
    panel.becomesKeyOnlyIfNeeded = true
    panel.level = .floating
    panel.contentView = NSHostingView(rootView: RootView(model: model))
    panel.setFrameOrigin(origin(for: size, over: frame))
    panel.orderFrontRegardless()
    window = panel
    // The game's end ends the panel (and its setup, which setup.sh then reports as cancelled to nobody).
    let watch = DispatchSource.makeProcessSource(identifier: getppid(), eventMask: .exit, queue: .main)
    watch.setEventHandler { model.leavePanel(6) }
    watch.resume()
    parentWatch = watch
    if getppid() == 1 { model.leavePanel(6) }  // the game already ended
  }

  // Centred over the game window (SDL's top-left origin on the main display -> AppKit's bottom-left), else on the main screen.
  @MainActor static func origin(for size: NSSize, over frame: CGRect?) -> NSPoint {
    let main = NSScreen.screens.first?.frame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
    guard let f = frame, f.width > 0, f.height > 0 else {
      return NSPoint(x: main.midX - size.width / 2, y: main.midY - size.height / 2)
    }
    let gameBottom = main.maxY - (f.minY + f.height)
    return NSPoint(x: f.minX + (f.width - size.width) / 2, y: gameBottom + (f.height - size.height) / 2)
  }
}
