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
//     install <iso>                     runs the kit's setup.sh: its lines, its exit status
//     play [minutes]                    runs the kit's play.sh (the GPU safeguards): its lines, its exit status
//     render <dir>                      the window's pages as PNGs (previews)
// Paths: FM4_RECOMP_HOME (default ~/Library/Application Support/FM4 Recomp) and FM4_RECOMP_KIT (default the app's Resources/kit).

import AppKit
import SwiftUI

@main
enum Entry {
  static func main() {
    let args = Array(CommandLine.arguments.dropFirst())
    if args.first == "--setup" { exit(SetupCLI.run(Array(args.dropFirst()), paths: Paths.standard())) }
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
    case ("install", 1):
      let iso = URL(fileURLWithPath: rest[0])
      let check = verifyDisc(iso)
      guard check.ok else { print(check.message); return 1 }
      return wait { done in runKitScript(paths, "setup.sh", [iso.path], line: { print($0); fflush(stdout) }, done: done) }
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
    print("usage: FM4 Recomp --setup status | prerequisites | verify <iso> | install <iso> | play [minutes] | render <dir>")
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
  return failed ? 1 : 0
}
