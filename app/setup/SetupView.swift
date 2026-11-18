// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app's window: welcome -> your Mac -> your Disc 1 -> setting up -> your Disc 2 (optional: Skip, or set up; also
// "Add Disc 2" in the launcher) -> the launcher (Play, with the GPU safeguards of play.sh). Once set up, opening the app starts
// the game at once (LaunchDecision.swift); missing or moved game data opens Repair instead. The keyboard bindings are changed in
// the game (Keyboard Controls, ControlsView.swift / ControlsPanel.swift). Colours from the logo (assets/fm4-recomp-logo.svg): red #ff4429, asphalt #20242a, white.

import AppKit
import SwiftUI
import UniformTypeIdentifiers

enum Theme {
  static let red = Color(red: 1.0, green: 0.267, blue: 0.161)
  static let background = Color(red: 0.051, green: 0.059, blue: 0.071)
  static let card = Color(red: 0.090, green: 0.102, blue: 0.122)
  static let line = Color.white.opacity(0.08)
  static let secondary = Color(red: 0.70, green: 0.73, blue: 0.77)
  static let good = Color(red: 0.30, green: 0.80, blue: 0.45)
}

@MainActor
final class SetupModel: ObservableObject {
  enum Page: String, CaseIterable { case welcome, prerequisites, disc, disc2, installing, failed, launcher, repair }

  let paths: Paths
  @Published var page: Page
  @Published var checks: [Check] = []
  @Published var disc: URL?
  @Published var discCheck: DiscCheck?
  @Published var disc2: URL?  // the optional Disc 2 (Content Install Disc), set up after Disc 1
  @Published var disc2Check: DiscCheck?
  @Published var installingDisc2 = false
  @Published var hasDisc2 = false
  // Where setup puts things (default: the recorded locations, else the internal ones); any of them may be on an external drive.
  @Published var dataDir: URL
  @Published var userDataDir: URL
  @Published var disc2Dir: URL
  @Published var progress: Double = 0
  @Published var stage = ""
  @Published var log: [String] = []
  @Published var failure = ""
  @Published var playing = false
  @Published var lastSession = ""
  @Published var gpuStop: String?
  @Published var minutes = 120
  @Published var debugValidation = false  // play.sh --debug: Vulkan and Metal API validation (slower)
  // Keyboard Controls (KeyBindings.swift; in the game only, ControlsPanel.swift): the saved keyboard bindings, why the saved file was refused (the defaults are shown
  // then), the action waiting for its new key, a key taken from another action waiting for Replace, the reset question.
  @Published var bindings: KeyBindings
  @Published var bindingsProblem: String?
  @Published var capturing: String?
  @Published var pendingChange: KeyBindings.Change?
  @Published var confirmingReset = false
  @Published var controlsNotice = ""
  private var keyMonitor: Any?
  // Why the game cannot start as set up (LaunchDecision.swift .repair), shown on the Repair page.
  @Published var repairReason = ""
  private var autoPlayed = false
  // The in-game Disc 2 panel (Panel.swift): set up, then end; "Back to the Game" ends it without the disc.
  var panel = false
  var gameMessage = ""
  private var process: Process?

  static let shared = SetupModel(paths: Paths.standard())  // the app's model (Main.swift: the scene and the automatic start)

  init(paths: Paths, preview: Bool = false) {
    self.paths = paths
    let recorded = Locations.read(paths)
    dataDir = recorded.dataRoot
    userDataDir = recorded.userDataRoot
    disc2Dir = recorded.disc2Root ?? recorded.dataRoot.appendingPathComponent("disc2")
    let saved = KeyBindings.load(paths.keyBindingsFile)
    bindings = saved.0
    bindingsProblem = saved.1
    page = .welcome
    checks = prerequisites(paths)
    if !preview { applyLaunchDecision() }
    hasDisc2 = disc2Ready(paths)
    refreshGpuStop()
  }

  var allChecksOK: Bool { checks.allSatisfy(\.ok) }

  // nil when every destination volume has room (the same rule as locations.sh loc_need, which setup.sh applies again).
  func shortfall(_ which: Disc) -> String? {
    which == .one ? spaceShortfall([(dataDir, FileManager.default.fileExists(atPath: dataDir.appendingPathComponent("files").path) ? 0 : disc1DataBytes),
                                     (userDataDir, 0)])
      : spaceShortfall([(disc2Dir, disc2FilesBytes), (userDataDir, disc2ContentBytes)])
  }

  // A folder the player picked: setup's own folder inside it ("FM4 Recomp/<name>"), so a drive's top folder works too.
  func place(_ picked: URL, _ name: String) -> URL {
    picked.appendingPathComponent("FM4 Recomp", isDirectory: true).appendingPathComponent(name, isDirectory: true)
  }

  func recheck() { checks = prerequisites(paths) }

  func choose(_ url: URL, _ which: Disc = .one) {
    if which == .one {
      disc = url
      discCheck = verifyDisc(url)
    } else {
      disc2 = url
      disc2Check = verifyDisc(url, .two)
    }
  }

  func startSetup(_ which: Disc = .one) {
    guard let iso = which == .one ? disc : disc2 else { return }
    installingDisc2 = which == .two
    page = .installing
    progress = 0
    stage = "Starting"
    log = []
    let options = which == .one ? ["--data", dataDir.path, "--user-data", userDataDir.path] : ["--disc2", "--dest", disc2Dir.path]
    process = runKitScript(paths, "setup.sh", options + [iso.path], line: { text in
      Task { @MainActor in self.consumeSetupLine(text) }
    }, done: { status in
      Task { @MainActor in self.setupFinished(status) }
    })
  }

  private func consumeSetupLine(_ text: String) {
    if panel { print(text); fflush(stdout) }  // the game logs the setup's lines and reads its result (disc_setup_sdl.cpp)
    if let (value, what) = parseProgress(text) {
      progress = value
      stage = what
    } else if text.hasPrefix("result ") {  // setup.sh's machine-readable last line; its exit status says the same
    } else {
      log.append(text)
    }
  }

  private func setupFinished(_ status: Int32) {
    process = nil
    hasDisc2 = disc2Ready(paths)
    if panel && status == 0 && hasDisc2 { leavePanel(0) }  // set up: the game mounts it and shows its next screen
    if status == 0 && installingDisc2 && hasDisc2 {
      page = .launcher
    } else if status == 0 && !installingDisc2 && state(paths) == .ready {
      page = hasDisc2 ? .launcher : .disc2  // Disc 2 is optional: its page offers Skip
    } else {
      failure = log.last(where: { $0.hasPrefix("setup:") }) ?? (status == 6 || status == 15 ? "Setup was cancelled." : "Setup stopped (status \(status)).")
      page = .failed
    }
  }

  func cancelSetup() { process?.terminate() }

  // The panel never keeps the focus: after a file or folder dialog the game is active again (its keyboard and gamepad input).
  func returnFocus() {
    if panel { NSRunningApplication(processIdentifier: getppid())?.activate() }
  }

  // Ends the panel: 0 once Disc 2 is set up ("result ok", setup.sh's own line, came first), else "cancelled" (6): the game keeps
  // waiting on its Disc 2 screen, whose Back returns to its menu.
  func leavePanel(_ status: Int32) {
    process?.terminate()
    if status != 0 { print("result cancelled") }
    fflush(stdout)
    exit(status)
  }

  func play(automatic: Bool = false) {
    refreshGpuStop()
    guard gpuStop == nil, !playing else { return }
    playing = true
    lastSession = "Starting the game…"
    var gameEnded: String?
    process = runKitScript(paths, "play.sh", ["\(minutes)"] + (debugValidation ? ["--debug"] : []), line: { text in
      Task { @MainActor in
        if text.hasPrefix("play: the game ended") {
          gameEnded = text
          if automatic && gameEndedNormally(text) { NSApplication.shared.setActivationPolicy(.prohibited) }
        }
        if text.hasPrefix("play:") { self.lastSession = String(text.dropFirst(6)) }
      }
    }, done: { status in
      Task { @MainActor in
        self.playing = false
        self.process = nil
        self.refreshGpuStop()
        (self.bindings, self.bindingsProblem) = KeyBindings.load(self.paths.keyBindingsFile)  // changed in the game, perhaps
        guard automatic else { return }
        if quitsAfterAutoSession(scriptStatus: status, gameEnded: gameEnded) { NSApplication.shared.terminate(nil) }
        // Something went wrong: the launcher appears with the session's message (and GPU_STOP's card, if any).
        NSApplication.shared.setActivationPolicy(.regular)
        self.applyLaunchDecision()
        Launcher.show()
      }
    })
  }

  // The page for the data as it is now: the welcome (never set up), Repair (set up, but the data is missing or moved), or the
  // launcher (ready; GPU_STOP shows its card there).
  func applyLaunchDecision() {
    switch launchDecision(paths) {
    case .setup: page = .welcome
    case .repair(let reason): repairReason = reason; page = .repair
    case .blocked, .play: page = .launcher
    }
  }

  // The app was opened and the game is ready: start it at once, once per app launch (not after a session ends, not when
  // GPU_STOP blocks it, never in the previews or the panels, which do not call this).
  func launchIfReady() {
    guard !autoPlayed else { return }
    autoPlayed = true
    guard case .play = launchDecision(paths) else { return }
    // The game alone: the launcher window stays hidden while it runs (it reappears only if the session goes wrong).
    runningWithoutWindow = true
    play(automatic: true)
  }

  func refreshGpuStop() {
    gpuStop = (try? String(contentsOf: paths.gpuStop, encoding: .utf8))?.split(separator: "\n").first.map(String.init)
  }

  func openLogs() {
    try? FileManager.default.createDirectory(at: paths.logs, withIntermediateDirectories: true)
    NSWorkspace.shared.open(paths.logs)
  }

  // MARK: Settings -> Controls

  func beginCapture(_ action: String) {
    endCapture()
    confirmingReset = false
    controlsNotice = ""
    capturing = action
    // While capturing, every key goes to the capture, none to the window (Return would press the default button).
    keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged]) { [weak self] event in
      guard let self else { return event }
      let consumed = MainActor.assumeIsolated { self.capture(event) }  // local monitors run on the main thread
      return consumed ? nil : event
    }
  }

  func endCapture() {
    if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
    keyMonitor = nil
    capturing = nil
    pendingChange = nil
  }

  private func capture(_ event: NSEvent) -> Bool {
    guard let action = capturing else { return false }
    guard pendingChange == nil else { return true }  // Replace or Cancel first
    if event.type == .keyDown && event.isARepeat { return true }
    if event.type == .flagsChanged && !isModifierPress(event) { return true }  // a modifier's release
    guard let key = keyName(forKeyCode: event.keyCode) else {
      controlsNotice = "That key cannot be used in the game (Caps Lock, Command, fn, ` and § are not available). Press another key."
      return true
    }
    let change = bindings.change(assigning: key, to: action)
    if change.from != nil { pendingChange = change } else { apply(change) }
    return true
  }

  func apply(_ change: KeyBindings.Change) {
    save(bindings.applying(change))
    endCapture()
  }

  private func save(_ new: KeyBindings) {
    do {
      try new.write(to: paths.keyBindingsFile)
      bindings = new
      bindingsProblem = nil
      controlsNotice = "Saved. The new keys apply when you return to the game."
    } catch {
      controlsNotice = "Could not save \(paths.keyBindingsFile.path): \(error.localizedDescription)"
    }
  }

  // The defaults are no file at all (play.sh then passes no --keybindings).
  func resetBindings() {
    endCapture()
    confirmingReset = false
    do {
      if FileManager.default.fileExists(atPath: paths.keyBindingsFile.path) { try FileManager.default.removeItem(at: paths.keyBindingsFile) }
      bindings = .defaults
      bindingsProblem = nil
      controlsNotice = "Reset to the defaults."
    } catch {
      controlsNotice = "Could not remove \(paths.keyBindingsFile.path): \(error.localizedDescription)"
    }
  }

  // Keyboard Controls in the game (ControlsPanel.swift): Done. The game gets the keyboard back, reloads the file and resumes.
  // A key still held (Return, which pressed Done) would reach the game as soon as it has the focus again (the system's
  // auto-repeat), so the panel first waits, at most 2 s, until no game key is down.
  func leaveControls() {
    let held = { macKeyNames.keys.contains { CGEventSource.keyState(.combinedSessionState, key: CGKeyCode($0)) } }
    let finish: () -> Never = {
      NSRunningApplication(processIdentifier: getppid())?.activate()
      fflush(stdout)
      exit(0)
    }
    guard held() else { finish() }
    let deadline = Date().addingTimeInterval(2)
    Timer.scheduledTimer(withTimeInterval: 0.02, repeats: true) { _ in if !held() || Date() > deadline { finish() } }
  }

  func showForPreview(_ p: Page) {
    page = p
    switch p {
    case .disc:
      disc = URL(fileURLWithPath: "/Volumes/Games/Forza Motorsport 4 (USA) (Disc 1) (Play Disc).iso")
      discCheck = DiscCheck(ok: true, message: "Xbox 360 game disc image (XGD3), 8.7 GB. Setup checks that it is Forza Motorsport 4 (USA) Disc 1.")
    case .disc2:
      disc2 = URL(fileURLWithPath: "/Volumes/Games/Forza Motorsport 4 (USA) (Disc 2) (Content Install Disc).iso")
      disc2Check = DiscCheck(ok: true, message: "Forza Motorsport 4 Content Install Disc image (XGD3), 8.7 GB. Setup checks every file against this build.")
    case .installing:
      progress = 0.42
      stage = "Copying the game files from your disc"
    case .failed:
      failure = "setup: default.xex is not the file this build needs: another disc version?"
    case .launcher:
      lastSession = "session over, no GPU hang evidence."
    case .repair:
      repairReason = "The game data folder /Volumes/Games/FM4 Recomp/data is missing. If it is on an external drive, connect it."
    default: break
    }
  }
}

// MARK: Views

struct RootView: View {
  @ObservedObject var model: SetupModel

  var body: some View {
    VStack(spacing: 0) {
      Header(model: model)
      Group {
        switch model.page {
        case .welcome: WelcomePage(model: model)
        case .prerequisites: PrerequisitesPage(model: model)
        case .disc: DiscPage(model: model, which: .one)
        case .disc2: DiscPage(model: model, which: .two)
        case .installing: InstallingPage(model: model)
        case .failed: FailedPage(model: model)
        case .launcher: LauncherPage(model: model)
        case .repair: RepairPage(model: model)
        }
      }
      .padding(.horizontal, 36)
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
    .frame(width: 720, height: 560)
    .background(Theme.background)
    .foregroundStyle(.white)
    .preferredColorScheme(.dark)
  }
}

struct Header: View {
  @ObservedObject var model: SetupModel

  var body: some View {
    VStack(spacing: 10) {
      if let image = NSImage(contentsOf: model.paths.logo) {
        Image(nsImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
          .frame(height: model.page == .launcher ? 150 : 96)
      }
      if model.page != .launcher && model.page != .repair && !model.panel { StepIndicator(page: model.page, disc2: model.installingDisc2) }
    }
    .padding(.top, 34)
    .padding(.bottom, 18)
  }
}

struct StepIndicator: View {
  let page: SetupModel.Page
  let disc2: Bool  // installing or failing on Disc 2
  private let steps = ["Welcome", "Your Mac", "Your Disc 1", "Set up", "Disc 2 (optional)"]
  private var current: Int {
    switch page {
    case .welcome: return 0
    case .prerequisites: return 1
    case .disc: return 2
    case .disc2: return 4
    default: return disc2 ? 4 : 3
    }
  }

  var body: some View {
    HStack(spacing: 8) {
      ForEach(Array(steps.enumerated()), id: \.offset) { i, name in
        HStack(spacing: 6) {
          Circle().fill(i <= current ? Theme.red : Color.white.opacity(0.18)).frame(width: 8, height: 8)
          Text(name).font(.system(size: 12, weight: i == current ? .semibold : .regular))
            .foregroundStyle(i == current ? Color.white : Theme.secondary)
        }
        if i < steps.count - 1 { Rectangle().fill(Theme.line).frame(width: 26, height: 1) }
      }
    }
  }
}

struct Card<Content: View>: View {
  @ViewBuilder var content: Content
  var body: some View {
    VStack(alignment: .leading, spacing: 12) { content }
      .padding(20)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(RoundedRectangle(cornerRadius: 12).fill(Theme.card))
      .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.line))
  }
}

struct PrimaryButton: View {
  let title: String
  var enabled = true
  let action: () -> Void
  var body: some View {
    Button(action: action) {
      Text(title).font(.system(size: 14, weight: .semibold)).padding(.horizontal, 22).padding(.vertical, 9)
        .background(RoundedRectangle(cornerRadius: 8).fill(enabled ? Theme.red : Color.white.opacity(0.12)))
        .foregroundStyle(enabled ? Color.white : Theme.secondary)
    }
    .buttonStyle(.plain)
    .disabled(!enabled)
    .keyboardShortcut(.defaultAction)
  }
}

struct SecondaryButton: View {
  let title: String
  let action: () -> Void
  var body: some View {
    Button(action: action) {
      Text(title).font(.system(size: 13)).padding(.horizontal, 16).padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 8).stroke(Color.white.opacity(0.25)))
    }
    .buttonStyle(.plain)
  }
}

struct WelcomePage: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      Text("Welcome to FM4 Recomp").font(.system(size: 24, weight: .bold))
      Text("FM4 Recomp runs Forza Motorsport 4 natively on Apple silicon. It is built from your own copy of the game: you need your Forza Motorsport 4 (USA) Disc 1 (Play Disc) as an ISO image.")
        .font(.system(size: 14)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
      Card {
        Bullet(n: 1, text: "Check your Mac: Apple silicon and macOS 26 or later. Everything else is included.")
        Bullet(n: 2, text: "Choose your Disc 1 ISO. It is only read.")
        Bullet(n: 3, text: "Setup copies the game files (about 8 GB) into your Library or a folder you choose, an external drive too, and checks every one against this build. Nothing leaves your Mac.")
      }
      HStack {
        Text("Beta build: expect a low frame rate and known issues (Read Me).").font(.system(size: 12)).foregroundStyle(Theme.secondary)
        Spacer()
        PrimaryButton(title: "Get Started") { model.recheck(); model.page = .prerequisites }
      }
    }
  }
}

struct Bullet: View {
  let n: Int
  let text: String
  var body: some View {
    HStack(alignment: .firstTextBaseline, spacing: 12) {
      Text("\(n)").font(.system(size: 12, weight: .bold)).frame(width: 22, height: 22)
        .background(Circle().fill(Theme.red.opacity(0.9)))
      Text(text).font(.system(size: 13.5)).fixedSize(horizontal: false, vertical: true)
    }
  }
}

struct PrerequisitesPage: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      Text("Your Mac").font(.system(size: 22, weight: .bold))
      Card {
        ForEach(model.checks) { c in
          HStack(spacing: 10) {
            Image(systemName: c.ok ? "checkmark.circle.fill" : "xmark.circle.fill").foregroundStyle(c.ok ? Theme.good : Theme.red)
            Text(c.name).font(.system(size: 13))
            Spacer()
            Text(c.detail).font(.system(size: 12)).foregroundStyle(Theme.secondary)
          }
        }
      }
      if !model.allChecksOK {
        Text("FM4 Recomp needs an Apple silicon Mac with macOS 26 or later. Everything else it runs is included in the app; if a part is missing, download FM4 Recomp again.")
          .font(.system(size: 12.5)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
      } else {
        Text("Everything FM4 Recomp runs is included in the app: nothing else to install.")
          .font(.system(size: 12.5)).foregroundStyle(Theme.secondary)
      }
      HStack {
        SecondaryButton(title: "Back") { model.page = .welcome }
        Spacer()
        SecondaryButton(title: "Check Again") { model.recheck() }
        PrimaryButton(title: "Continue", enabled: model.allChecksOK) { model.page = .disc }
      }
    }
  }
}

// Disc 1 (required), or the optional Disc 2 (Content Install Disc) after Disc 1 is set up.
struct DiscPage: View {
  @ObservedObject var model: SetupModel
  let which: Disc
  @State private var targeted = false
  private var chosen: URL? { which == .one ? model.disc : model.disc2 }
  private var check: DiscCheck? { which == .one ? model.discCheck : model.disc2Check }
  private var label: String { which == .one ? "Disc 1" : "Disc 2" }
  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      Text(which == .one ? "Your Disc 1" : model.panel ? "Your Disc 2" : "Your Disc 2 (optional)").font(.system(size: 22, weight: .bold))
      if which == .two && model.panel {
        Text("The game asks for Disc 2\(model.gameMessage.isEmpty ? "" : " (\u{201C}\(model.gameMessage)\u{201D})"). Drop your Disc 2 image here or choose it, and where its files go: FM4 Recomp checks and copies them, then the game shows its own Install screen (confirm with A). The game keeps running; its Back (B) returns to the menu.")
          .font(.system(size: 12.5)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
      } else if which == .two {
        Text("Disc 1 is set up and plays on its own. The Content Install Disc holds content the game installs when you choose to (about 3 GB copied now, 3 GB more when installed). Add it now, later from the launcher, or when the game asks.")
          .font(.system(size: 12.5)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
      }
      VStack(spacing: 10) {
        Image(systemName: "opticaldisc").font(.system(size: 34)).foregroundStyle(Theme.red)
        if let disc = chosen {
          Text(disc.lastPathComponent).font(.system(size: 13, weight: .semibold)).multilineTextAlignment(.center).lineLimit(2)
          if let check {
            Label(check.message, systemImage: check.ok ? "checkmark.seal.fill" : "exclamationmark.triangle.fill")
              .font(.system(size: 12)).foregroundStyle(check.ok ? Theme.good : Theme.red).multilineTextAlignment(.center)
          }
        } else {
          Text("Drop your Forza Motorsport 4 (USA) \(label) ISO here").font(.system(size: 13.5, weight: .medium))
          Text(which == .one ? "The Play Disc, as an .iso image of your own disc" : "The Content Install Disc, as an .iso image of your own disc")
            .font(.system(size: 12)).foregroundStyle(Theme.secondary)
        }
        SecondaryButton(title: chosen == nil ? "Choose ISO…" : "Choose Another…") { pick() }
      }
      .padding(16).frame(maxWidth: .infinity, minHeight: which == .one ? 140 : 120)
      .background(RoundedRectangle(cornerRadius: 12).fill(targeted ? Theme.red.opacity(0.10) : Theme.card))
      .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6, 5]))
        .foregroundStyle(targeted ? Theme.red : Color.white.opacity(0.22)))
      .onDrop(of: [.fileURL], isTargeted: $targeted) { providers in
        let target = which
        _ = providers.first?.loadObject(ofClass: URL.self) { url, _ in
          if let url { Task { @MainActor in model.choose(url, target) } }
        }
        return true
      }
      if which == .one {
        LocationRow(title: "Game data (about 8 GB)", url: model.dataDir) { if let p = pickFolder() { model.dataDir = model.place(p, "data") } }
        LocationRow(title: "Saves and installed Disc 2 content (about 3 GB)", url: model.userDataDir) {
          if let p = pickFolder() { model.userDataDir = model.place(p, "user-data") }
        }
      } else {
        LocationRow(title: "Disc 2 files (about 3.2 GB)", url: model.disc2Dir) { if let p = pickFolder() { model.disc2Dir = model.place(p, "disc2") } }
        LocationRow(title: "Installed content, with your saves (about 3 GB)", url: model.userDataDir, change: nil)
      }
      let short = model.shortfall(which)
      Text(short ?? "Enough free space at each location. Any folder may be on an external drive.")
        .font(.system(size: 12)).foregroundStyle(short == nil ? Theme.secondary : Theme.red)
      HStack {
        if which == .one {
          SecondaryButton(title: "Back") { model.page = .prerequisites }
        } else {
          SecondaryButton(title: model.panel ? "Back to the Game" : "Skip") { if model.panel { model.leavePanel(6) } else { model.page = .launcher } }
        }
        Spacer()
        PrimaryButton(title: which == .one ? "Set Up" : "Set Up Disc 2", enabled: check?.ok == true && short == nil) { model.startSetup(which) }
      }
    }
  }

  private func pickFolder() -> URL? {
    let panel = NSOpenPanel()
    panel.title = "Choose where FM4 Recomp keeps this (setup makes an \"FM4 Recomp\" folder inside it)"
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    panel.canCreateDirectories = true
    panel.allowsMultipleSelection = false
    defer { model.returnFocus() }
    return panel.runModal() == .OK ? panel.url : nil
  }

  private func pick() {
    let panel = NSOpenPanel()
    panel.title = "Choose your Forza Motorsport 4 \(label) ISO"
    panel.allowedContentTypes = [UTType(filenameExtension: "iso") ?? .diskImage]
    panel.allowsMultipleSelection = false
    defer { model.returnFocus() }
    if panel.runModal() == .OK, let url = panel.url { model.choose(url, which) }
  }
}

// One storage location: what goes there, the folder (middle-truncated), and Change… (absent when it cannot be changed here).
struct LocationRow: View {
  let title: String
  let url: URL
  var change: (() -> Void)?
  var body: some View {
    HStack(spacing: 10) {
      Image(systemName: "externaldrive").foregroundStyle(Theme.secondary)
      VStack(alignment: .leading, spacing: 2) {
        Text(title).font(.system(size: 12, weight: .medium))
        Text(url.path).font(.system(size: 11)).foregroundStyle(Theme.secondary).lineLimit(1).truncationMode(.middle)
      }
      Spacer()
      if let change { SecondaryButton(title: "Change…", action: change) }
    }
  }
}

struct InstallingPage: View {
  @ObservedObject var model: SetupModel
  @State private var details = false
  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      Text("Setting up").font(.system(size: 22, weight: .bold))
      Card {
        HStack {
          Text(model.stage).font(.system(size: 13.5, weight: .medium))
          Spacer()
          Text("\(Int(model.progress * 100)) %").font(.system(size: 13, weight: .semibold, design: .rounded)).monospacedDigit()
        }
        ProgressBar(value: model.progress)
        Text(model.installingDisc2 ? "Copying about 3 GB takes a few minutes. Every content package is checked against this build."
             : "Copying about 8 GB takes a few minutes. Every file is checked against this build.")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
      }
      DisclosureGroup(isExpanded: $details) { LogView(lines: model.log) } label: {
        Text("Details").font(.system(size: 12.5)).foregroundStyle(Theme.secondary)
      }
      HStack { Spacer(); SecondaryButton(title: "Cancel") { model.cancelSetup() } }
    }
  }
}

struct ProgressBar: View {
  let value: Double
  var body: some View {
    GeometryReader { g in
      ZStack(alignment: .leading) {
        Capsule().fill(Color.white.opacity(0.10))
        Capsule().fill(LinearGradient(colors: [Theme.red.opacity(0.75), Theme.red], startPoint: .leading, endPoint: .trailing))
          .frame(width: max(10, g.size.width * value))
      }
    }
    .frame(height: 10)
  }
}

struct LogView: View {
  let lines: [String]
  var body: some View {
    ScrollView {
      Text(lines.suffix(200).joined(separator: "\n")).font(.system(size: 11, design: .monospaced)).foregroundStyle(Theme.secondary)
        .frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
    }
    .frame(height: 110).padding(8).background(RoundedRectangle(cornerRadius: 8).fill(Color.black.opacity(0.35)))
  }
}

struct FailedPage: View {
  @ObservedObject var model: SetupModel
  @State private var details = true
  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      Label("Setup did not finish", systemImage: "exclamationmark.triangle.fill").font(.system(size: 22, weight: .bold))
        .foregroundStyle(Theme.red)
      Card {
        Text(model.failure).font(.system(size: 13)).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
        Text("Nothing was changed on your disc image. Your files so far are kept; Try Again continues where setup stopped.")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary)
      }
      DisclosureGroup(isExpanded: $details) { LogView(lines: model.log) } label: {
        Text("Details").font(.system(size: 12.5)).foregroundStyle(Theme.secondary)
      }
      HStack {
        SecondaryButton(title: "Show Files") { NSWorkspace.shared.open(model.paths.home) }
        Spacer()
        if model.panel { SecondaryButton(title: "Back to the Game") { model.leavePanel(6) } }
        else if model.installingDisc2 { SecondaryButton(title: "Play Without Disc 2") { model.page = .launcher } }
        PrimaryButton(title: "Try Again") { model.page = model.installingDisc2 ? .disc2 : .disc }
      }
    }
  }
}

struct LauncherPage: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(spacing: 16) {
      if let stop = model.gpuStop {
        Card {
          Label("Play is blocked by the GPU safeguard", systemImage: "hand.raised.fill").font(.system(size: 14, weight: .semibold))
            .foregroundStyle(Theme.red)
          Text(stop).font(.system(size: 12)).fixedSize(horizontal: false, vertical: true)
          Text("An earlier session showed evidence of a GPU hang. Read GPU_STOP, send it and the session's logs to the developers, then delete it yourself to play again.")
            .font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
          HStack {
            SecondaryButton(title: "Show GPU_STOP") { NSWorkspace.shared.activateFileViewerSelecting([model.paths.gpuStop]) }
            SecondaryButton(title: "Check Again") { model.refreshGpuStop() }
          }
        }
      }
      HStack(spacing: 14) {
        PrimaryButton(title: model.playing ? "Playing…" : "Play", enabled: !model.playing && model.gpuStop == nil) { model.play() }
          .scaleEffect(1.15)
        Picker("", selection: $model.minutes) {
          ForEach([30, 60, 120, 240], id: \.self) { Text("\($0) minute session").tag($0) }
        }
        .labelsHidden().frame(width: 170).disabled(model.playing)
      }
      Toggle("Debug mode: full GPU validation (much slower)", isOn: $model.debugValidation)
        .toggleStyle(.checkbox).font(.system(size: 12)).foregroundStyle(Theme.secondary).disabled(model.playing)
      if !model.lastSession.isEmpty {
        Text(model.lastSession).font(.system(size: 12)).foregroundStyle(Theme.secondary).lineLimit(2).multilineTextAlignment(.center)
      }
      Card {
        HStack(alignment: .top, spacing: 24) {
          VStack(alignment: .leading, spacing: 5) {
            Text("Controls").font(.system(size: 12.5, weight: .semibold))
            Text("Gamepad: Xbox layout. Keyboard: \(model.bindings.summary()). To change the keys, pause a race and choose Keyboard Controls (or press ⌘,). A skips the welcome video.")
              .font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
          }
          VStack(alignment: .leading, spacing: 5) {
            Text("GPU safeguards").font(.system(size: 12.5, weight: .semibold))
            Text("A hard session limit, the game's GPU timeouts, and a hang check after every session. Close the game window to end early.")
              .font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
          }
        }
      }
      HStack {
        SecondaryButton(title: "Logs") { model.openLogs() }
        SecondaryButton(title: "Read Me & Known Issues") { NSWorkspace.shared.open(model.paths.readme) }
        if !model.hasDisc2 && !model.playing { SecondaryButton(title: "Add Disc 2…") { model.page = .disc2 } }
        Spacer()
        Text("Beta · your saves are kept in your Library").font(.system(size: 11)).foregroundStyle(Theme.secondary)
      }
    }
  }
}

// Set up, but the game cannot start from what was recorded (LaunchDecision.swift): say what is missing; connect the drive and
// check again, or set up again (setup.sh keeps what is already there and checks it).
struct RepairPage: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      Label("FM4 Recomp needs a repair", systemImage: "wrench.and.screwdriver.fill").font(.system(size: 22, weight: .bold))
        .foregroundStyle(Theme.red)
      Card {
        Text(model.repairReason).font(.system(size: 13)).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
        Text("Your saves are not touched. If the data is on an external drive, connect it and choose Check Again. Otherwise set up again with your Disc 1 image.")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
      }
      HStack {
        SecondaryButton(title: "Show Files") { NSWorkspace.shared.open(model.paths.home) }
        Spacer()
        if model.bindingsProblem != nil {
          SecondaryButton(title: "Reset Keys to Defaults") { model.resetBindings(); model.applyLaunchDecision() }
        }
        SecondaryButton(title: "Check Again") { model.applyLaunchDecision(); model.refreshGpuStop() }
        PrimaryButton(title: "Set Up Again") { model.recheck(); model.page = .prerequisites }
      }
    }
  }
}
