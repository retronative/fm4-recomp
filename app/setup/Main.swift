// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app: the guided first-run setup, then the launcher.
//
//   FM4 Recomp                          the setup window, or the launcher once setup is complete
//   FM4 Recomp --setup <command>        the same steps without a window (tests, scripts):
//     status                            needs-setup | ready
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
// Paths: FM4_RECOMP_HOME (default ~/Library/Application Support/FM4 Recomp) and FM4_RECOMP_KIT (default the app's Resources/kit).

import AppKit
import SwiftUI

@main
enum Entry {
  static func main() {
    let args = Array(CommandLine.arguments.dropFirst())
    if args.first == "--setup" { exit(SetupCLI.run(Array(args.dropFirst()), paths: Paths.standard())) }
    if args.first == "--disc-panel" { DiscPanel.run(Array(args.dropFirst())) }
    SetupApp.main()
  }
}

struct SetupApp: App {
  @StateObject private var model = SetupModel(paths: Paths.standard())

  init() { NSApplication.shared.setActivationPolicy(.regular) }

  var body: some Scene {
    Window("FM4 Recomp", id: "setup") {
      RootView(model: model)
        .onAppear { NSApplication.shared.activate(ignoringOtherApps: true) }
    }
    .windowResizability(.contentSize)
    .windowStyle(.hiddenTitleBar)
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
    print("usage: FM4 Recomp --setup status | prerequisites | verify <iso> | install [--data <dir>] [--user-data <dir>] <iso> | verify-disc2 <iso> | install-disc2 [--dest <dir>] <iso> | play [minutes] | render <dir>")
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
  return failed ? 1 : 0
}
