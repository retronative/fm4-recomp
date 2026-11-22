// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app's graphics settings (app/setup/GraphicsSettings.swift), headless: the four resolutions and their spellings
// (runtime/video/render_scale.h ParseInternalRes accepts each: 720p, fhd, qhd, 4k), Full HD with Standard smoothing as the defaults, the file, the parse rules,
// the atomic save and the load across "launches" (a fresh load of the saved file). Expected values by hand. "CONTROL" marks a
// negative control.
//
//   tests/run_app_tests.sh

import Foundation

@main
enum GraphicsSettingsTest {
  static var failures = 0

  static func expect(_ ok: Bool, _ name: String) {
    print("\(ok ? "pass" : "FAIL") \(name)")
    if !ok { failures += 1 }
  }

  static func error(_ text: String) -> String? {
    if case .failure(let e) = GraphicsSettings.parse(text) { return e.message }
    return nil
  }

  static func main() {
    expect(InternalResolution.allCases.map(\.rawValue) == ["720p", "fhd", "qhd", "4k"] &&
             InternalResolution.allCases.map(\.pixels) == ["1280 × 720", "1920 × 1080", "2560 × 1440", "3840 × 2160"],
           "resolutions: 720p, fhd, qhd, 4k in that order, with their sizes")
    let header = "# FM4 Recomp graphics settings, written by FM4 Recomp's Graphics panel. Delete this file for the defaults.\n"
    let d = GraphicsSettings.defaults
    expect(d.resolution == .fhd && d.serialized() == header + "internal_resolution = fhd\nanti_aliasing = on\n", "defaults: Full HD with Standard smoothing, and its file")
    expect(GraphicsSettings(resolution: .hd).serialized() == header + "internal_resolution = 720p\nanti_aliasing = on\n" &&
             GraphicsSettings(resolution: .qhd).serialized() == header + "internal_resolution = qhd\nanti_aliasing = on\n", "file: 720p and qhd by their spellings")
    expect(InternalResolution.allCases.allSatisfy { GraphicsSettings.parse(GraphicsSettings(resolution: $0).serialized()) == .success(GraphicsSettings(resolution: $0)) },
           "every resolution's file parses back")
    // Extra edge smoothing (anti_aliasing): on at Standard by default; off is written (a file that does not name it is the default).
    expect(d.antiAliasing && GraphicsSettings(resolution: .qhd, antiAliasing: false).serialized() == header + "internal_resolution = qhd\nanti_aliasing = off\n",
           "edge smoothing: on by default, and off is written")
    expect(GraphicsSettings(resolution: .qhd, antiAliasing: true).serialized() == header + "internal_resolution = qhd\nanti_aliasing = on\n",
           "edge smoothing: on is written after the resolution")
    expect(InternalResolution.allCases.allSatisfy { r in [false, true].allSatisfy { a in
             GraphicsSettings.parse(GraphicsSettings(resolution: r, antiAliasing: a).serialized()) == .success(GraphicsSettings(resolution: r, antiAliasing: a)) } },
           "edge smoothing: every resolution with it on or off parses back")
    expect(GraphicsSettings.parse("anti_aliasing = on\n") == .success(d) &&
             GraphicsSettings.parse("anti_aliasing=off\r\ninternal_resolution = fhd\n") == .success(GraphicsSettings(resolution: .fhd, antiAliasing: false)),
           "edge smoothing: alone it keeps Full HD; off is accepted; either order")
    expect(error("anti_aliasing = yes\n") == "line 1: unknown anti_aliasing 'yes' (off, soft, on or strong)" && error("anti_aliasing = ON\n") == "line 1: unknown anti_aliasing 'ON' (off, soft, on or strong)" &&
             error("anti_aliasing = 1\n") == "line 1: unknown anti_aliasing '1' (off, soft, on or strong)" &&
             error("anti_aliasing = on\nanti_aliasing = off\n") == "line 2: anti_aliasing is set twice",
           "CONTROL: another spelling of on, and the setting named twice, are refused")
    // Its strength: Standard is the file's `on` (the only spelling before there was a choice), soft and strong their own.
    expect(SmoothingStrength.allCases == [.soft, .standard, .strong] && SmoothingStrength.allCases.map(\.rawValue) == ["soft", "on", "strong"] &&
             SmoothingStrength.allCases.map(\.label) == ["Soft", "Standard", "Strong"], "strengths: soft, standard (spelled on), strong in that order")
    expect(d.strength == .standard && GraphicsSettings(resolution: .qhd, antiAliasing: true).strength == .standard, "strength: Standard by default")
    expect(GraphicsSettings(resolution: .qhd, antiAliasing: true, strength: .soft).serialized() == header + "internal_resolution = qhd\nanti_aliasing = soft\n" &&
             GraphicsSettings(resolution: .hd, antiAliasing: true, strength: .strong).serialized() == header + "internal_resolution = 720p\nanti_aliasing = strong\n",
           "strength: soft and strong are the anti_aliasing line's value")
    expect(InternalResolution.allCases.allSatisfy { r in SmoothingStrength.allCases.allSatisfy { s in
             let g = GraphicsSettings(resolution: r, antiAliasing: true, strength: s)
             return GraphicsSettings.parse(g.serialized()) == .success(g) } }, "strength: every resolution with every strength parses back")
    expect(GraphicsSettings.parse("anti_aliasing = strong\n") == .success(GraphicsSettings(antiAliasing: true, strength: .strong)) &&
             GraphicsSettings.parse("anti_aliasing = off\n") == .success(GraphicsSettings(antiAliasing: false)), "strength: alone it keeps the default resolution; off is Standard and off")
    expect(error("anti_aliasing = standard\n") == "line 1: unknown anti_aliasing 'standard' (off, soft, on or strong)" &&
             error("anti_aliasing = Soft\n") == "line 1: unknown anti_aliasing 'Soft' (off, soft, on or strong)" &&
             error("anti_aliasing = soft\nanti_aliasing = strong\n") == "line 2: anti_aliasing is set twice",
           "CONTROL: a strength the app does not write, another case, and two strengths are refused")
    // HDR output (hdr): Off by default and then not written, so the default file's text is what it was before the setting existed;
    // Auto is one more line. The preference is off or auto: never "on" (what is active is the game's to say).
    expect(!d.hdr && GraphicsSettings(hdr: true).serialized() == header + "internal_resolution = fhd\nanti_aliasing = on\nhdr = auto\n" &&
             GraphicsSettings(resolution: .qhd, antiAliasing: false, hdr: true).serialized() == header + "internal_resolution = qhd\nanti_aliasing = off\nhdr = auto\n",
           "hdr: Off by default and not written; Auto is the file's third line")
    expect(GraphicsSettings.parse("hdr = auto\n") == .success(GraphicsSettings(hdr: true)) && GraphicsSettings.parse("hdr=off\r\n") == .success(d) &&
             GraphicsSettings.parse(GraphicsSettings(resolution: .uhd, antiAliasing: true, strength: .soft, hdr: true).serialized())
               == .success(GraphicsSettings(resolution: .uhd, antiAliasing: true, strength: .soft, hdr: true)),
           "hdr: auto and off parse; alone it keeps the other defaults; a file with all three parses back")
    expect(error("hdr = on\n") == "line 1: unknown hdr 'on' (off or auto)" && error("hdr = 1\n") == "line 1: unknown hdr '1' (off or auto)" &&
             error("hdr = AUTO\n") == "line 1: unknown hdr 'AUTO' (off or auto)" && error("hdr = auto\nhdr = off\n") == "line 2: hdr is set twice",
           "CONTROL: on, 1, another case, and hdr named twice are refused")
    // Extended View (extended_view): off by default and not written; on is one more line, after hdr.
    expect(!d.extendedView && GraphicsSettings(extendedView: true).serialized() == header + "internal_resolution = fhd\nanti_aliasing = on\nextended_view = on\n" &&
             GraphicsSettings(hdr: true, extendedView: true).serialized() == header + "internal_resolution = fhd\nanti_aliasing = on\nhdr = auto\nextended_view = on\n" &&
             GraphicsSettings.parse("extended_view = on\n") == .success(GraphicsSettings(extendedView: true)) && GraphicsSettings.parse("extended_view=off\r\n") == .success(d) &&
             GraphicsSettings.parse(GraphicsSettings(resolution: .qhd, hdr: true, extendedView: true).serialized()) == .success(GraphicsSettings(resolution: .qhd, hdr: true, extendedView: true)),
           "extended view: off by default and not written; on is a line after hdr; it parses back with the others")
    expect(error("extended_view = yes\n") == "line 1: unknown extended_view 'yes' (off or on)" && error("extended_view = ON\n") == "line 1: unknown extended_view 'ON' (off or on)" &&
             error("extended_view = on\nextended_view = off\n") == "line 2: extended_view is set twice",
           "CONTROL: another value, another case, and extended_view named twice are refused")
    // The game's last word (hdr_status) and the line under the choice: the preference is separate from what is active.
    expect(HdrStatus.parse("active = hdr\n") == HdrStatus(active: true, reason: "") &&
             HdrStatus.parse("# c\nactive = sdr\nreason = this display does not support HDR\n") == HdrStatus(active: false, reason: "this display does not support HDR") &&
             HdrStatus.parse("") == nil && HdrStatus.parse("reason = x\n") == nil && HdrStatus.parse("active = yes\n") == nil,
           "hdr status: active hdr or sdr with a reason; no active line, or another value, is nothing known (CONTROL)")
    let sdrOff = HdrStatus(active: false, reason: "HDR is off"), noSystem = HdrStatus(active: false, reason: "HDR is not available on this system")
    expect(HdrStatus.line(preference: false, display: true, live: true, running: HdrStatus(active: true, reason: "")) == "SDR active: HDR is off." &&
             HdrStatus.line(preference: true, display: false, live: true, running: HdrStatus(active: true, reason: "")) == "SDR active: this display does not support HDR." &&
             HdrStatus.line(preference: true, display: true, live: true, running: HdrStatus(active: true, reason: "")) == "HDR active." &&
             HdrStatus.line(preference: true, display: true, live: true, running: noSystem) == "SDR active: HDR is not available on this system." &&
             HdrStatus.line(preference: true, display: true, live: true, running: sdrOff) == "HDR becomes active when you close Settings." &&
             HdrStatus.line(preference: true, display: nil, live: true, running: nil) == "HDR becomes active when you close Settings." &&
             HdrStatus.line(preference: true, display: true, live: false, running: sdrOff) == "SDR active: restart required. HDR applies the next time you start the game.",
           "hdr line: Off is SDR; Auto on a display without HDR is SDR with that reason; the game's HDR or its reason; a change waits for the panel to close, or for a restart")
    expect(GraphicsSettings.parse("") == .success(d) && GraphicsSettings.parse(header) == .success(d), "an empty file or only the comment is the default")
    expect(GraphicsSettings.parse("internal_resolution=qhd\r\n") == .success(GraphicsSettings(resolution: .qhd)), "CRLF line ends and no spaces")
    expect(error("internal_resolution = 8k\n") == "line 1: unknown resolution '8k' (720p, fhd, qhd or 4k)" &&
             error("internal_resolution = QHD\n") == "line 1: unknown resolution 'QHD' (720p, fhd, qhd or 4k)" &&
             error("internal_resolution = 1440p\n") == "line 1: unknown resolution '1440p' (720p, fhd, qhd or 4k)",
           "CONTROL: an unknown value, another case and a spelling the app does not write are refused")
    expect(error("\n# c\nmsaa = 4\n") == "line 3: unknown setting 'msaa'" && error("internal_resolution qhd\n") == "line 1: expected '<setting> = <value>', not 'internal_resolution qhd'" &&
             error("internal_resolution = fhd\ninternal_resolution = qhd\n") == "line 2: internal_resolution is set twice",
           "CONTROL: an unknown setting, a line without = and a setting named twice are refused")

    // The file across launches: nothing saved, a save, a fresh load, another save, a refused file.
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("fm4 graphics test \(getpid())")
    let file = dir.appendingPathComponent("graphics.conf")
    defer { try? FileManager.default.removeItem(at: dir) }
    let none = GraphicsSettings.load(file)
    expect(none.0 == d && none.1 == nil, "load: no file is the defaults, no problem")
    do {
      try GraphicsSettings(resolution: .qhd).write(to: file)
      let back = GraphicsSettings.load(file)
      let names = try FileManager.default.contentsOfDirectory(atPath: dir.path)
      expect(back.0.resolution == .qhd && back.1 == nil && names == ["graphics.conf"], "save: written atomically; the next launch loads QHD")
      try GraphicsSettings(resolution: .fhd).write(to: file)
      expect(GraphicsSettings.load(file).0.resolution == .fhd, "save: a second choice replaces the first")
      try GraphicsSettings(resolution: .hd).write(to: file)
      expect(GraphicsSettings.load(file).0 == GraphicsSettings(resolution: .hd) && FileManager.default.fileExists(atPath: file.path), "save: 720p is saved too")
      try GraphicsSettings(resolution: .qhd, antiAliasing: true).write(to: file)
      let smooth = GraphicsSettings.load(file)
      expect(smooth.0 == GraphicsSettings(resolution: .qhd, antiAliasing: true) && smooth.1 == nil, "save: edge smoothing on is loaded at the next launch")
      try GraphicsSettings(resolution: .qhd, antiAliasing: true, strength: .strong).write(to: file)
      expect(GraphicsSettings.load(file).0 == GraphicsSettings(resolution: .qhd, antiAliasing: true, strength: .strong), "save: the strength is loaded at the next launch")
      try GraphicsSettings(resolution: .qhd, antiAliasing: false).write(to: file)
      expect(GraphicsSettings.load(file).0 == GraphicsSettings(resolution: .qhd, antiAliasing: false), "save: turning it off is saved, the resolution kept")
      try Data("internal_resolution = 8k\n".utf8).write(to: file)
      let refused = GraphicsSettings.load(file)
      expect(refused.0 == d && refused.1 == "graphics.conf line 1: unknown resolution '8k' (720p, fhd, qhd or 4k)", "load: an invalid file is the defaults and why")
    } catch {
      expect(false, "save: \(error)")
    }
    print(failures == 0 ? "all passed" : "\(failures) failed")
    exit(failures == 0 ? 0 : 1)
  }
}
