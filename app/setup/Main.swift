// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app: the guided first-run setup; once set up, opening the app starts the game (LaunchDecision.swift).
//
//   FM4 Recomp                          the setup window; once set up, the launcher, which starts the game at once (Repair
//                                       instead when the recorded game data is missing)
//   FM4 Recomp --setup <command>        the same steps without a window (tests, scripts):
//     status                            needs-setup | ready
//     launch-check                      what opening the app does: play | setup | repair <why> | blocked <why> (exit 0 for play)
//     prerequisites                     one line per check; exit 1 if any fails
//     verify <iso>                      what the image is, or why not (exit 1)
//     install [--data <dir>] [--user-data <dir>] <iso>
//                                       runs the kit's setup.sh: its lines (progress, result), its exit status (setup.sh's codes)
//     verify-disc2 <iso>                the same for the optional Disc 2 (Content Install Disc)
//     install-disc2 [--dest <dir>] <iso>  runs setup.sh --disc2 (after Disc 1; not-set-up 7 otherwise)
//     play [minutes]                    runs the kit's play.sh (the GPU safeguards): its lines, its exit status
//     render <dir>                      the window's pages as PNGs (previews)
//   FM4 Recomp --disc-panel 2 [--frame <x,y,w,h>] [--message <text>] [--problem <text>]
//                                     the Disc 2 setup panel the game opens over its "Insert Disc 2" screen (Panel.swift)
//   FM4 Recomp --controls-panel [--frame <x,y,w,h>]
//                                     Keyboard Controls, which the game opens on Command-comma (ControlsPanel.swift)
// Paths: FM4_RECOMP_HOME (default ~/Library/Application Support/FM4 Recomp) and FM4_RECOMP_KIT (default the app's Resources/kit).

import AppKit
import SwiftUI

@main
enum Entry {
  static func main() {
    let args = Array(CommandLine.arguments.dropFirst())
    if args.first == "--setup" { exit(SetupCLI.run(Array(args.dropFirst()), paths: Paths.standard())) }
    if args.first == "--disc-panel" { DiscPanel.run(Array(args.dropFirst())) }
    if args.first == "--controls-panel" { ControlsPanel.run(Array(args.dropFirst())) }
    // No window restoration: an app quit while its window was hidden for a session came back with no window, so nothing started
    // (2026-11-15; the restorationBehavior modifier does not discard the state already saved).
    UserDefaults.standard.register(defaults: ["ApplePersistenceIgnoreState": true, "NSQuitAlwaysKeepsWindows": false])
    // Decided before any window exists (ReCarbon's lifecycle, runtime/main.cpp at 66401ea: the game when it is installed, the
    // installer only when it is not): set up and nothing blocks it -> the game starts and the launcher window is never created,
    // so it cannot flash; it opens only for setup, repair, GPU_STOP or a session that went wrong.
    MainActor.assumeIsolated { Launcher.startsGame = launchDecision(Paths.standard()) == .play }
    SetupApp.main()
  }
}

// While a session the app started on its own runs, the launcher window is hidden: the app must not quit for having no visible
// window (SwiftUI's single-window app would, and play.sh would end with it).
@MainActor var runningWithoutWindow = false

@MainActor enum Launcher {
  static var startsGame = false
  static var open: (() -> Void)?  // the launcher scene's openWindow (LauncherOpener)
  // The launcher window, created now if this launch suppressed it.
  static func show() {
    runningWithoutWindow = false
    if let window = NSApplication.shared.windows.first(where: { $0.identifier?.rawValue.hasPrefix("setup") == true }) {
      window.makeKeyAndOrderFront(nil)
    } else {
      open?()
    }
    NSApplication.shared.activate(ignoringOtherApps: true)
  }
  // The game this app's kit runs (play.sh's grandchild, its own app process with the window).
  static func focusGame() {
    let kit = Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/kit/bin/fm4recomp").standardizedFileURL.path
    NSWorkspace.shared.runningApplications.first { $0.executableURL?.standardizedFileURL.path == kit }?.activate()
  }
}

// Captures the scene's openWindow action (Commands get the environment at launch, before any window).
struct LauncherOpener: Commands {
  @Environment(\.openWindow) private var openWindow
  var body: some Commands {
    let _ = { Launcher.open = { [openWindow] in openWindow(id: "setup") } }()
    CommandGroup(after: .appInfo) {}
  }
}

final class SetupAppDelegate: NSObject, NSApplicationDelegate {
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { !runningWithoutWindow }
  func applicationDidFinishLaunching(_ notification: Notification) {
    guard Launcher.startsGame else { return }
    SetupModel.shared.launchIfReady()
    if !SetupModel.shared.playing { Launcher.show() }  // the data changed since the decision: the launcher says why
  }
  // The Dock icon clicked (or the app switched to) while the game runs: the game comes to the front, no launcher.
  func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
    guard runningWithoutWindow else { return true }
    Launcher.focusGame()
    return false
  }
  func applicationDidBecomeActive(_ notification: Notification) {
    if runningWithoutWindow { Launcher.focusGame() }
  }
}

struct SetupApp: App {
  @NSApplicationDelegateAdaptor(SetupAppDelegate.self) private var delegate
  @StateObject private var model = SetupModel.shared

  init() { NSApplication.shared.setActivationPolicy(.regular) }

  var body: some Scene {
    Window("FM4 Recomp", id: "setup") {
      RootView(model: model)
        .onAppear { if !model.playing { NSApplication.shared.activate(ignoringOtherApps: true) } }
    }
    .windowResizability(.contentSize)
    .windowStyle(.hiddenTitleBar)
    // No restoration (an app quit while its window was hidden came back with no window, 2026-11-15); presented only when the
    // launcher is needed at launch.
    .restorationBehavior(.disabled)
    .defaultLaunchBehavior(Launcher.startsGame ? .suppressed : .presented)
    .commands { LauncherOpener() }
  }
}

enum SetupCLI {
  static func run(_ args: [String], paths: Paths) -> Int32 {
    guard let command = args.first else { return usage() }
    let rest = Array(args.dropFirst())
    switch (command, rest.count) {
    case ("status", 0):
      print(state(paths).rawValue)
      return 0
    case ("launch-check", 0):
      switch launchDecision(paths) {
      case .play: print("play"); return 0
      case .setup: print("setup"); return 1
      case .repair(let why): print("repair \(why)"); return 1
      case .blocked(let why): print("blocked \(why)"); return 1
      }
    case ("prerequisites", 0):
      let list = prerequisites(paths)
      for c in list { print("\(c.ok ? "ok  " : "FAIL") \(c.name): \(c.detail)") }
      return list.allSatisfy(\.ok) ? 0 : 1
    case ("verify", 1):
      let check = verifyDisc(URL(fileURLWithPath: rest[0]))
      print(check.message)
      return check.ok ? 0 : 1
    case ("install", 1), ("install", 3), ("install", 5):  // [--data <dir>] [--user-data <dir>] <iso>: setup.sh checks the options
      let check = verifyDisc(URL(fileURLWithPath: rest[rest.count - 1]))
      guard check.ok else { print(check.message); print("result wrong-disc"); return 3 }
      return wait { done in runKitScript(paths, "setup.sh", rest, line: { print($0); fflush(stdout) }, done: done) }
    case ("verify-disc2", 1):
      let check = verifyDisc(URL(fileURLWithPath: rest[0]), .two)
      print(check.message)
      return check.ok ? 0 : 1
    case ("install-disc2", 1), ("install-disc2", 3):  // [--dest <dir>] <iso>
      let check = verifyDisc(URL(fileURLWithPath: rest[rest.count - 1]), .two)
      guard check.ok else { print(check.message); print("result wrong-disc"); return 3 }
      return wait { done in runKitScript(paths, "setup.sh", ["--disc2"] + rest, line: { print($0); fflush(stdout) }, done: done) }
    case ("play", 0), ("play", 1):
      guard state(paths) == .ready else { print("not set up yet: run --setup install <iso> first"); return 1 }
      return wait { done in runKitScript(paths, "play.sh", rest, line: { print($0); fflush(stdout) }, done: done) }
    case ("render", 1):
      return MainActor.assumeIsolated { renderPages(paths: paths, to: URL(fileURLWithPath: rest[0])) }
    default:
      return usage()
    }
  }

  static func wait(_ start: (@escaping (Int32) -> Void) -> Process?) -> Int32 {
    let finished = DispatchSemaphore(value: 0)
    var status: Int32 = 1
    _ = start { s in status = s; finished.signal() }
    finished.wait()
    return status
  }

  static func usage() -> Int32 {
    print("usage: FM4 Recomp --setup status | launch-check | prerequisites | verify <iso> | install [--data <dir>] [--user-data <dir>] <iso> | verify-disc2 <iso> | install-disc2 [--dest <dir>] <iso> | play [minutes] | render <dir>")
    return 2
  }
}

// The window's pages as PNGs, for review (no window is opened).
@MainActor
func renderPages(paths: Paths, to dir: URL) -> Int32 {
  try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  let model = SetupModel(paths: paths, preview: true)
  var failed = false
  for page in SetupModel.Page.allCases {
    model.showForPreview(page)
    let renderer = ImageRenderer(content: RootView(model: model).environment(\.colorScheme, .dark))
    renderer.scale = 2
    guard let image = renderer.cgImage else { failed = true; continue }
    let rep = NSBitmapImageRep(cgImage: image)
    let url = dir.appendingPathComponent("\(page.rawValue).png")
    do { try rep.representation(using: .png, properties: [:])!.write(to: url); print("wrote \(url.path)") } catch { failed = true }
  }
  // The in-game Disc 2 panel (Panel.swift): its first page and a failure.
  model.panel = true
  model.gameMessage = "Content Install Disc"
  for page in [SetupModel.Page.disc2, .failed] {
    model.showForPreview(page)
    model.installingDisc2 = page == .failed
    let renderer = ImageRenderer(content: RootView(model: model).environment(\.colorScheme, .dark))
    renderer.scale = 2
    guard let image = renderer.cgImage else { failed = true; continue }
    let url = dir.appendingPathComponent("panel-\(page.rawValue).png")
    do { try NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])!.write(to: url); print("wrote \(url.path)") } catch { failed = true }
  }
  // Keyboard Controls (ControlsPanel.swift): idle, and asking about a key another action has.
  model.panel = false
  let controlsShots: [(String, KeyBindings.Change?)] = [("controls", nil), ("controls-conflict", model.bindings.change(assigning: "S", to: "throttle"))]
  for (name, change) in controlsShots {
    model.capturing = change == nil ? nil : "throttle"
    model.pendingChange = change
    let renderer = ImageRenderer(content: ControlsRoot(model: model).environment(\.colorScheme, .dark))
    renderer.scale = 2
    guard let image = renderer.cgImage else { failed = true; continue }
    let url = dir.appendingPathComponent("panel-\(name).png")
    do { try NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])!.write(to: url); print("wrote \(url.path)") } catch { failed = true }
  }
  model.capturing = nil
  model.pendingChange = nil
  // Graphics (GraphicsView.swift): the default, and QHD chosen in a game that started at 720p.
  model.settingsTab = .graphics
  let graphicsShots: [(String, InternalResolution, SmoothingStrength?)] =
    [("graphics", .hd, nil), ("graphics-qhd", .qhd, nil), ("graphics-smoothing", .hd, .standard), ("graphics-smoothing-strong", .qhd, .strong)]
  for (name, resolution, smoothing) in graphicsShots {
    model.graphics.resolution = resolution
    model.graphics.antiAliasing = smoothing != nil
    model.graphics.strength = smoothing ?? .standard
    model.graphicsAtStart = .hd
    model.antiAliasingAtStart = false
    let renderer = ImageRenderer(content: ControlsRoot(model: model).environment(\.colorScheme, .dark))
    renderer.scale = 2
    guard let image = renderer.cgImage else { failed = true; continue }
    let url = dir.appendingPathComponent("panel-\(name).png")
    do { try NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])!.write(to: url); print("wrote \(url.path)") } catch { failed = true }
  }
  // HDR output Auto: active on a display that supports it, standard range on one that does not; and Off.
  let hdrShots: [(String, Bool, Bool, HdrStatus?)] = [("graphics-hdr-active", true, true, HdrStatus(active: true, reason: "")),
                                                     ("graphics-hdr-unsupported", true, false, HdrStatus(active: false, reason: "this display does not support HDR")),
                                                     ("graphics-hdr-off", false, true, HdrStatus(active: false, reason: "HDR is off"))]
  for (name, auto, display, running) in hdrShots {
    model.graphics = GraphicsSettings(resolution: .fhd, antiAliasing: true, strength: .standard, hdr: auto, extendedView: auto)
    model.graphicsAtStart = .fhd
    model.antiAliasingAtStart = true
    model.strengthAtStart = .standard
    model.hdrAtStart = false
    model.hdrLive = true
    model.extendedOffered = auto  // the Extended View row in the Auto shots only (not offered in 0.2.0)
    model.displayHdr = display
    model.hdrRunning = running
    let renderer = ImageRenderer(content: ControlsRoot(model: model).environment(\.colorScheme, .dark))
    renderer.scale = 2
    guard let image = renderer.cgImage else { failed = true; continue }
    let url = dir.appendingPathComponent("panel-\(name).png")
    do { try NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])!.write(to: url); print("wrote \(url.path)") } catch { failed = true }
  }
  model.settingsTab = .keyboard
  model.graphics = GraphicsSettings.load(paths.graphicsFile).0
  return failed ? 1 : 0
}
