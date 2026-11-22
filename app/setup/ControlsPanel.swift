// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp --controls-panel [--frame <x,y,w,h>]: Keyboard Controls in the game (ControlsView.swift), as a panel over the game
// window. fm4recomp starts it on Command-comma (runtime/controls_panel_sdl.cpp, kernel/controls_screen.h): the game is paused
// for it (XN_SYS_UI, as for the Guide) and its pad is idle; every change is written to $FM4_RECOMP_HOME/keybindings.conf at once;
// Done (or the window's close) ends the panel with 0 and hands the keyboard back to the game, which re-reads the file and resumes.
//   --frame: the game window in points, top-left origin on the main display (SDL's); the panel opens centred over it.
// Unlike the Disc 2 panel (Panel.swift, non-activating), this panel takes the keyboard: capturing a key needs it (the model's
// local key monitor, SetupView.swift beginCapture). It ends with the game (the parent's exit).

import AppKit
import SwiftUI

enum ControlsPanel {
  static func run(_ args: [String]) -> Never {
    var frame: CGRect?
    switch (args.count, args.first) {
    case (0, _): break
    case (2, "--frame"):
      let n = args[1].split(separator: ",").compactMap { Double($0) }
      if n.count == 4 { frame = CGRect(x: n[0], y: n[1], width: n[2], height: n[3]) }
    default: print("usage: FM4 Recomp --controls-panel [--frame <x,y,w,h>]"); exit(2)
    }
    MainActor.assumeIsolated { open(frame: frame) }
    NSApplication.shared.run()
    exit(0)
  }

  @MainActor private static var window: NSPanel?
  @MainActor private static var parentWatch: DispatchSourceProcess?
  @MainActor private static var closeObserver: NSObjectProtocol?

  @MainActor private static func open(frame: CGRect?) {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)  // no Dock icon or menu bar of its own; it still takes the keyboard while in front
    let model = SetupModel(paths: Paths.standard())
    model.panel = true
    model.settingsTab = .graphics  // the keys are also in the game's own KEYBOARD CONTROLS (Pause menu); Graphics is only here
    model.graphicsAtStart = ProcessInfo.processInfo.environment["FM4_INTERNAL_RES"].flatMap(InternalResolution.init(rawValue:))
    if let started = ProcessInfo.processInfo.environment["FM4_POST_AA"] {
      model.strengthAtStart = SmoothingStrength(rawValue: started == "1" ? "on" : started)
      model.antiAliasingAtStart = model.strengthAtStart != nil
    }
    let startedHdr = ProcessInfo.processInfo.environment["FM4_HDR"]
    model.hdrAtStart = startedHdr == "on" || startedHdr == "1"
    model.hdrLive = ProcessInfo.processInfo.environment["FM4_GRAPHICS_LIVE"] == "1"
    model.extendedOffered = ProcessInfo.processInfo.environment["FM4_EXTENDED_VIEW_OFFERED"] == "1"
    model.hdrRunning = (try? String(contentsOf: Paths.standard().hdrStatusFile, encoding: .utf8)).flatMap(HdrStatus.parse)
    // The screen's most headroom above standard white (1: a standard-range display), as the game reads it (runtime/video/window_sdl.cpp).
    model.displayHdr = NSScreen.main.map { $0.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.01 }
    let size = NSSize(width: 720, height: 790)
    let panel = NSPanel(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled, .closable, .fullSizeContentView],
                        backing: .buffered, defer: false)
    panel.title = "Settings"
    panel.titleVisibility = .hidden
    panel.titlebarAppearsTransparent = true
    panel.isMovableByWindowBackground = true
    panel.hidesOnDeactivate = false
    panel.level = .floating
    panel.contentView = NSHostingView(rootView: ControlsRoot(model: model))
    panel.setFrameOrigin(DiscPanel.origin(for: size, over: frame))
    // The red close button is Done too.
    closeObserver = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: panel, queue: .main) { _ in
      MainActor.assumeIsolated { model.endCapture(); model.leaveControls() }
    }
    app.activate(ignoringOtherApps: true)
    panel.makeKeyAndOrderFront(nil)
    window = panel
    // The game's end ends the panel (nothing to hand back; the file is already written).
    let watch = DispatchSource.makeProcessSource(identifier: getppid(), eventMask: .exit, queue: .main)
    watch.setEventHandler { exit(0) }
    watch.resume()
    parentWatch = watch
    if getppid() == 1 { exit(0) }  // the game already ended
  }
}

// The pages on their own (no logo header or setup steps), in the launcher's colours: Keyboard and Graphics (GraphicsView.swift).
struct ControlsRoot: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      SettingsTabs(model: model)
      if model.settingsTab == .keyboard { ControlsPage(model: model) } else { GraphicsPage(model: model) }
    }
      .padding(.horizontal, 36).padding(.top, 30).padding(.bottom, 20)
      .frame(width: 720, height: 790, alignment: .top)
      .background(Theme.background)
      .foregroundStyle(.white)
      .preferredColorScheme(.dark)
  }
}
