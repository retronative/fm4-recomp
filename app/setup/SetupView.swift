// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app's window: welcome -> your Mac -> your Disc 1 -> setting up -> the launcher (Play, with the GPU safeguards of
// play.sh). Colours from the logo (assets/fm4-recomp-logo.svg): red #ff4429, asphalt #20242a, white.

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
  enum Page: String, CaseIterable { case welcome, prerequisites, disc, installing, failed, launcher }

  let paths: Paths
  @Published var page: Page
  @Published var checks: [Check] = []
  @Published var disc: URL?
  @Published var discCheck: DiscCheck?
  @Published var progress: Double = 0
  @Published var stage = ""
  @Published var log: [String] = []
  @Published var failure = ""
  @Published var playing = false
  @Published var lastSession = ""
  @Published var gpuStop: String?
  @Published var minutes = 120
  @Published var debugValidation = false  // play.sh --debug: Vulkan and Metal API validation (slower)
  private var process: Process?

  init(paths: Paths, preview: Bool = false) {
    self.paths = paths
    page = state(paths) == .ready && !preview ? .launcher : .welcome
    checks = prerequisites(paths)
    refreshGpuStop()
  }

  var allChecksOK: Bool { checks.allSatisfy(\.ok) }
  var free: UInt64 { freeSpace(at: paths.home) }

  func recheck() { checks = prerequisites(paths) }

  func choose(_ url: URL) {
    disc = url
    discCheck = verifyDisc(url)
  }

  func startSetup() {
    guard let disc else { return }
    page = .installing
    progress = 0
    stage = "Starting"
    log = []
    process = runKitScript(paths, "setup.sh", [disc.path], line: { text in
      Task { @MainActor in self.consumeSetupLine(text) }
    }, done: { status in
      Task { @MainActor in self.setupFinished(status) }
    })
  }

  private func consumeSetupLine(_ text: String) {
    if let (value, what) = parseProgress(text) {
      progress = value
      stage = what
    } else {
      log.append(text)
    }
  }

  private func setupFinished(_ status: Int32) {
    process = nil
    if status == 0 && state(paths) == .ready {
      page = .launcher
    } else {
      failure = log.last(where: { $0.hasPrefix("setup:") }) ?? (status == 15 ? "Setup was cancelled." : "Setup stopped (status \(status)).")
      page = .failed
    }
  }

  func cancelSetup() { process?.terminate() }

  func play() {
    refreshGpuStop()
    guard gpuStop == nil, !playing else { return }
    playing = true
    lastSession = "Starting the game…"
    process = runKitScript(paths, "play.sh", ["\(minutes)"] + (debugValidation ? ["--debug"] : []), line: { text in
      Task { @MainActor in if text.hasPrefix("play:") { self.lastSession = String(text.dropFirst(6)) } }
    }, done: { _ in
      Task { @MainActor in
        self.playing = false
        self.process = nil
        self.refreshGpuStop()
      }
    })
  }

  func refreshGpuStop() {
    gpuStop = (try? String(contentsOf: paths.gpuStop, encoding: .utf8))?.split(separator: "\n").first.map(String.init)
  }

  func openLogs() {
    try? FileManager.default.createDirectory(at: paths.logs, withIntermediateDirectories: true)
    NSWorkspace.shared.open(paths.logs)
  }

  func showForPreview(_ p: Page) {
    page = p
    switch p {
    case .disc:
      disc = URL(fileURLWithPath: "/Volumes/Games/Forza Motorsport 4 (USA) (Disc 1) (Play Disc).iso")
      discCheck = DiscCheck(ok: true, message: "Xbox 360 game disc image (XGD3), 8.7 GB. Setup checks that it is Forza Motorsport 4 (USA) Disc 1.")
    case .installing:
      progress = 0.42
      stage = "Copying the game files from your disc"
    case .failed:
      failure = "setup: default.xex is not the file this build needs: another disc version?"
    case .launcher:
      lastSession = "session over, no GPU hang evidence."
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
        case .disc: DiscPage(model: model)
        case .installing: InstallingPage(model: model)
        case .failed: FailedPage(model: model)
        case .launcher: LauncherPage(model: model)
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
      if model.page != .launcher { StepIndicator(page: model.page) }
    }
    .padding(.top, 34)
    .padding(.bottom, 18)
  }
}

struct StepIndicator: View {
  let page: SetupModel.Page
  private let steps = ["Welcome", "Your Mac", "Your Disc 1", "Set up"]
  private var current: Int {
    switch page {
    case .welcome: return 0
    case .prerequisites: return 1
    case .disc: return 2
    default: return 3
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
        Bullet(n: 3, text: "Setup copies the game files into your Library (about 8 GB) and checks every one against this build. Nothing leaves your Mac.")
      }
      HStack {
        Text("Alpha build: expect a low frame rate and known issues (Read Me).").font(.system(size: 12)).foregroundStyle(Theme.secondary)
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

struct DiscPage: View {
  @ObservedObject var model: SetupModel
  @State private var targeted = false
  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      Text("Your Disc 1").font(.system(size: 22, weight: .bold))
      VStack(spacing: 10) {
        Image(systemName: "opticaldisc").font(.system(size: 34)).foregroundStyle(Theme.red)
        if let disc = model.disc {
          Text(disc.lastPathComponent).font(.system(size: 13, weight: .semibold)).multilineTextAlignment(.center).lineLimit(2)
          if let check = model.discCheck {
            Label(check.message, systemImage: check.ok ? "checkmark.seal.fill" : "exclamationmark.triangle.fill")
              .font(.system(size: 12)).foregroundStyle(check.ok ? Theme.good : Theme.red).multilineTextAlignment(.center)
          }
        } else {
          Text("Drop your Forza Motorsport 4 (USA) Disc 1 ISO here").font(.system(size: 13.5, weight: .medium))
          Text("The Play Disc, as an .iso image of your own disc").font(.system(size: 12)).foregroundStyle(Theme.secondary)
        }
        SecondaryButton(title: model.disc == nil ? "Choose ISO…" : "Choose Another…") { pick() }
      }
      .padding(22).frame(maxWidth: .infinity, minHeight: 190)
      .background(RoundedRectangle(cornerRadius: 12).fill(targeted ? Theme.red.opacity(0.10) : Theme.card))
      .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6, 5]))
        .foregroundStyle(targeted ? Theme.red : Color.white.opacity(0.22)))
      .onDrop(of: [.fileURL], isTargeted: $targeted) { providers in
        _ = providers.first?.loadObject(ofClass: URL.self) { url, _ in
          if let url { Task { @MainActor in model.choose(url) } }
        }
        return true
      }
      let free = model.free
      Text("Setup needs about 9 GB free in your Library: \(gigabytes(free)) free.")
        .font(.system(size: 12)).foregroundStyle(free >= setupBytesNeeded ? Theme.secondary : Theme.red)
      HStack {
        SecondaryButton(title: "Back") { model.page = .prerequisites }
        Spacer()
        PrimaryButton(title: "Set Up", enabled: model.discCheck?.ok == true && free >= setupBytesNeeded) { model.startSetup() }
      }
    }
  }

  private func pick() {
    let panel = NSOpenPanel()
    panel.title = "Choose your Forza Motorsport 4 Disc 1 ISO"
    panel.allowedContentTypes = [UTType(filenameExtension: "iso") ?? .diskImage]
    panel.allowsMultipleSelection = false
    if panel.runModal() == .OK, let url = panel.url { model.choose(url) }
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
        Text("Copying about 8 GB takes a few minutes. Every file is checked against this build.")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary)
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
        PrimaryButton(title: "Try Again") { model.page = .disc }
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
            Text("Gamepad: Xbox layout. Keyboard: W throttle, S brake, A/D steer, Enter = A, Esc = B, arrows = D-pad. A skips the welcome video.")
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
        Spacer()
        Text("Alpha · your saves are kept in your Library").font(.system(size: 11)).foregroundStyle(Theme.secondary)
      }
    }
  }
}
