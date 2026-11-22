// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app: the game's graphics settings (Graphics, in the game: ControlsPanel.swift), without SwiftUI so
// tests/app/test_graphics_settings.swift runs it headless. The file is $FM4_RECOMP_HOME/graphics.conf: an
// `internal_resolution = 720p | fhd | qhd | 4k` line and an `anti_aliasing = off | soft | on | strong` line (extra edge smoothing and
// its strength, `on` being Standard: the post-process pass of runtime/video/post_aa.h). A setting the file does not name is its default.
// play.sh reads the file at the game's start and exports
// FM4_INTERNAL_RES (runtime/video/render_scale.h owns the spellings) and FM4_POST_AA; no file is Full HD with Standard smoothing, the
// defaults (owner decision 2026-11-21). The app writes it, and so does the game's own GRAPHICS submenu (the Pause menu; runtime/kernel/graphics_menu.h, the same
// text byte for byte). A choice applies the next time the game starts (the renderer reads both once).
// `hdr = auto | off` (HDR output, runtime/video/hdr_output.h; off by default): play.sh exports it as FM4_HDR. Auto uses HDR when the
// display and the system can show it and standard range (SDR) otherwise; it is the player's preference, not what is active (the
// running game writes that to hdr_status, HdrStatus below). Off is not written (a file that does not name it is off), so a file
// without HDR is the same text as before the setting existed.

import Foundation

enum InternalResolution: String, CaseIterable, Equatable {
  case hd = "720p"  // the game's own 1280x720
  case fhd = "fhd"  // 1920x1080: every render target 1.5 times the game's size (the default)
  case qhd = "qhd"  // 2560x1440: every render target twice the game's size
  case uhd = "4k"   // 3840x2160: every render target three times the game's size

  var label: String {
    switch self {
    case .hd: return "720p"
    case .fhd: return "Full HD"
    case .qhd: return "QHD"
    case .uhd: return "4K"
    }
  }
  var pixels: String {
    switch self {
    case .hd: return "1280 × 720"
    case .fhd: return "1920 × 1080"
    case .qhd: return "2560 × 1440"
    case .uhd: return "3840 × 2160"
    }
  }
  var detail: String {
    switch self {
    case .hd: return "The game's original resolution. Fastest, and the least memory."
    case .fhd: return "Sharper picture. Needs more graphics time and memory."
    case .qhd: return "Much sharper picture. Needs about twice the graphics time of 720p."
    case .uhd: return "Sharpest picture, for a 4K display. Very heavy: expect a low frame rate."
    }
  }
}

// How strongly the extra edge smoothing works (runtime/video/post_aa.h Tuning). The raw value is the file's and FM4_POST_AA's
// spelling: Standard is `on`, the only strength before there was a choice.
enum SmoothingStrength: String, CaseIterable, Equatable {
  case soft = "soft"
  case standard = "on"
  case strong = "strong"

  var label: String {
    switch self {
    case .soft: return "Soft"
    case .standard: return "Standard"
    case .strong: return "Strong"
    }
  }
  var detail: String {
    switch self {
    case .soft: return "Only the clearest edges; small text stays sharpest."
    case .standard: return "A balance of smooth edges and fine detail."
    case .strong: return "The smoothest edges; fine detail gets softer."
    }
  }
}

struct GraphicsSettings: Equatable {
  var resolution: InternalResolution = .fhd
  var antiAliasing = true  // extra edge smoothing after the game's own anti-aliasing (on at Standard by default)
  var strength = SmoothingStrength.standard  // only with antiAliasing: off is always saved, and loaded, as Standard
  var hdr = false  // Auto: HDR output when the display and system support it; the picture is standard range otherwise
  var extendedView = false  // races fill a window of another shape than 16:9 (runtime/kernel/extended_view.h); `extended_view = on`, off not written

  static let defaults = GraphicsSettings()

  // Both lines always: a setting a file does not name is its default, so off has to be written.
  func serialized() -> String {
    "# FM4 Recomp graphics settings, written by FM4 Recomp's Graphics panel. Delete this file for the defaults.\n"
      + "internal_resolution = \(resolution.rawValue)\n" + "anti_aliasing = \(antiAliasing ? strength.rawValue : "off")\n" + (hdr ? "hdr = auto\n" : "") + (extendedView ? "extended_view = on\n" : "")
  }

  // `key = value` lines, # comments and blank lines. Only the spellings this app writes are accepted (play.sh's rule), each
  // setting at most once.
  static func parse(_ text: String) -> Result<GraphicsSettings, GraphicsError> {
    var settings = GraphicsSettings()
    var seen = false, seenAa = false, seenHdr = false, seenExtended = false
    func trim<S: StringProtocol>(_ s: S) -> String { String(s).trimmingCharacters(in: CharacterSet(charactersIn: " \t\r")) }
    for (index, raw) in text.components(separatedBy: "\n").enumerated() {
      let number = index + 1
      let line = trim(raw)
      if line.isEmpty || line.hasPrefix("#") { continue }
      guard let eq = line.firstIndex(of: "=") else { return .failure(GraphicsError("line \(number): expected '<setting> = <value>', not '\(line)'")) }
      let key = trim(line[line.startIndex..<eq]), value = trim(line[line.index(after: eq)...])
      if key == "anti_aliasing" {
        guard !seenAa else { return .failure(GraphicsError("line \(number): anti_aliasing is set twice")) }
        let strength = SmoothingStrength(rawValue: value)
        guard strength != nil || value == "off" else { return .failure(GraphicsError("line \(number): unknown anti_aliasing '\(value)' (off, soft, on or strong)")) }
        settings.antiAliasing = strength != nil
        settings.strength = strength ?? .standard
        seenAa = true
        continue
      }
      if key == "extended_view" {
        guard !seenExtended else { return .failure(GraphicsError("line \(number): extended_view is set twice")) }
        guard value == "on" || value == "off" else { return .failure(GraphicsError("line \(number): unknown extended_view '\(value)' (off or on)")) }
        settings.extendedView = value == "on"
        seenExtended = true
        continue
      }
      if key == "hdr" {
        guard !seenHdr else { return .failure(GraphicsError("line \(number): hdr is set twice")) }
        guard value == "auto" || value == "off" else { return .failure(GraphicsError("line \(number): unknown hdr '\(value)' (off or auto)")) }
        settings.hdr = value == "auto"
        seenHdr = true
        continue
      }
      guard key == "internal_resolution" else { return .failure(GraphicsError("line \(number): unknown setting '\(key)'")) }
      guard !seen else { return .failure(GraphicsError("line \(number): internal_resolution is set twice")) }
      guard let resolution = InternalResolution(rawValue: value) else {
        return .failure(GraphicsError("line \(number): unknown resolution '\(value)' (720p, fhd, qhd or 4k)"))
      }
      settings.resolution = resolution
      seen = true
    }
    return .success(settings)
  }

  // No file: the defaults. A file that cannot be read or is invalid: the defaults and why (play.sh then starts with the defaults too).
  static func load(_ url: URL) -> (GraphicsSettings, String?) {
    guard FileManager.default.fileExists(atPath: url.path) else { return (defaults, nil) }
    guard let text = try? String(contentsOf: url, encoding: .utf8) else { return (defaults, "\(url.lastPathComponent) cannot be read") }
    switch parse(text) {
    case .success(let settings): return (settings, nil)
    case .failure(let error): return (defaults, "\(url.lastPathComponent) \(error.message)")
    }
  }

  func write(to url: URL) throws {
    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try Data(serialized().utf8).write(to: url, options: .atomic)
  }
}

struct GraphicsError: Error, Equatable {
  let message: String
  init(_ message: String) { self.message = message }
}

// What the running game last said about its output ($FM4_RECOMP_HOME/hdr_status, written by the game when it changes:
// `active = hdr | sdr` and `reason = <why standard range>`; runtime/kernel/graphics_menu.h WriteHdrStatus). No file, or one that
// cannot be read: nothing is known.
struct HdrStatus: Equatable {
  var active = false  // HDR is what the game shows now
  var reason = ""     // why it is standard range (empty when active)

  static func parse(_ text: String) -> HdrStatus? {
    var status = HdrStatus()
    var seen = false
    for raw in text.components(separatedBy: "\n") {
      let line = raw.trimmingCharacters(in: CharacterSet(charactersIn: " \t\r"))
      guard let eq = line.firstIndex(of: "=") else { continue }
      let key = line[line.startIndex..<eq].trimmingCharacters(in: .whitespaces), value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
      if key == "active" {
        guard value == "hdr" || value == "sdr" else { return nil }
        status.active = value == "hdr"
        seen = true
      } else if key == "reason" {
        status.reason = value
      }
    }
    return seen ? status : nil
  }

  // The line under the HDR choice. `preference`: Auto. `display`: whether this screen can show HDR (nil: not known). `live`: the
  // running game takes a change when the panel closes. `running`: the game's own last word (nil: none).
  static func line(preference: Bool, display: Bool?, live: Bool, running: HdrStatus?) -> String {
    if !preference { return "SDR active: HDR is off." }
    if display == false { return "SDR active: this display does not support HDR." }
    if let running = running {
      if running.active { return "HDR active." }
      // The game's reason stands unless it only says the setting was off (the choice here is newer).
      if !running.reason.isEmpty && running.reason != "HDR is off" { return "SDR active: \(running.reason)." }
    }
    return live ? "HDR becomes active when you close Settings." : "SDR active: restart required. HDR applies the next time you start the game."
  }
}
