// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp.app: paths, checks and the two kit scripts it drives (setup.sh, play.sh). The structure (a SwiftUI first-run setup
// with a headless --setup mode, data under Application Support, the tools in Resources/kit) follows reFrozen app/setup
// (GPL-3.0-or-later); the code is FM4 Recomp's own.

import Foundation

struct Paths {
  let kit: URL   // the app's Resources/kit: bin/, tools/, setup.sh, play.sh, README.md, resources/
  let home: URL  // the player's data, logs and GPU_STOP

  static func standard() -> Paths {
    let env = ProcessInfo.processInfo.environment
    let kit = env["FM4_RECOMP_KIT"].map { URL(fileURLWithPath: $0) }
      ?? Bundle.main.resourceURL!.appendingPathComponent("kit")
    let userHome = URL(fileURLWithPath: env["HOME"] ?? NSHomeDirectory())
    let home = env["FM4_RECOMP_HOME"].map { URL(fileURLWithPath: $0) }
      ?? userHome.appendingPathComponent("Library/Application Support/FM4 Recomp")
    return Paths(kit: kit, home: home)
  }

  var data: URL { home.appendingPathComponent("data") }  // the default (internal) data folder
  var logs: URL { home.appendingPathComponent("logs") }
  var gpuStop: URL { home.appendingPathComponent("GPU_STOP") }
  var locationsFile: URL { home.appendingPathComponent("locations.conf") }
  var keyBindingsFile: URL { home.appendingPathComponent("keybindings.conf") }  // Keyboard Controls in the game (KeyBindings.swift); play.sh passes it
  var hdrStatusFile: URL { home.appendingPathComponent("hdr_status") }  // written by the running game (GraphicsSettings.swift HdrStatus)
  var graphicsFile: URL { home.appendingPathComponent("graphics.conf") }  // Graphics in the game (GraphicsSettings.swift); play.sh reads it at the start
  var setupDone: URL { Locations.read(self).dataRoot.appendingPathComponent("setup.ok") }
  var readme: URL { kit.appendingPathComponent("README.md") }
  var logo: URL { kit.appendingPathComponent("resources/logo@2x.png") }
}

enum SetupState: String { case needsSetup = "needs-setup", ready }

func state(_ paths: Paths) -> SetupState {
  FileManager.default.fileExists(atPath: paths.setupDone.path) ? .ready : .needsSetup
}

func disc2Ready(_ paths: Paths) -> Bool { Locations.read(paths).disc2Root != nil }

// MARK: Storage locations (the kit's locations.sh owns the format and is the only writer: setup.sh records a choice once its
// step has passed; the app passes the player's choice to setup.sh and reads the file back)

struct Locations {
  var dataRoot: URL
  var userDataRoot: URL
  var disc2Root: URL?

  static func defaults(_ paths: Paths) -> Locations {
    Locations(dataRoot: paths.data, userDataRoot: paths.data.appendingPathComponent("user-data"), disc2Root: nil)
  }

  // key=value lines, the value verbatim after the first '='; comments and unknown keys skipped; a missing key is the default.
  // A file that cannot be read, or names a key twice, falls back to the defaults (setup.sh and play.sh then refuse it themselves).
  static func read(_ paths: Paths) -> Locations {
    var result = defaults(paths)
    guard let text = try? String(contentsOf: paths.locationsFile, encoding: .utf8) else { return result }
    var seen = Set<String>()
    for line in text.split(separator: "\n", omittingEmptySubsequences: true) where !line.hasPrefix("#") {
      guard let eq = line.firstIndex(of: "=") else { continue }
      let key = String(line[line.startIndex..<eq]), value = String(line[line.index(after: eq)...])
      guard value.hasPrefix("/") else { continue }
      if !seen.insert(key).inserted { return defaults(paths) }
      let url = URL(fileURLWithPath: value)
      switch key {
      case "data_root": result.dataRoot = url
      case "user_data_root": result.userDataRoot = url
      case "disc2_root": result.disc2Root = url
      default: break
      }
    }
    return result
  }
}

// MARK: Prerequisites (macOS 26 on Apple silicon, and the app's own kit: everything it runs is bundled)

struct Check: Identifiable {
  let name: String
  let ok: Bool
  let detail: String
  var id: String { name }
}

func prerequisites(_ paths: Paths) -> [Check] {
  var list: [Check] = []
  #if arch(arm64)
    list.append(Check(name: "Apple silicon", ok: true, detail: "arm64"))
  #else
    list.append(Check(name: "Apple silicon", ok: false, detail: "FM4 Recomp runs on Apple silicon Macs only"))
  #endif
  let v = ProcessInfo.processInfo.operatingSystemVersion
  list.append(Check(name: "macOS 26 or later", ok: v.majorVersion >= 26, detail: "macOS \(v.majorVersion).\(v.minorVersion)"))
  let engine = "engines/shiftglue/"
  let kit: [(String, [String])] = [
    ("Disc setup and launch", ["bin/extract-xiso", "bin/xexsurvey", "lib/libfm4_lzx.dylib", "bin/gdfx", "setup.sh", "play.sh", "locations.sh", "shiftglue-playtest.sh", "migrate-career.sh", "migrate-playtest-profile.sh", "bin/fm4_migrate_career"]),
    ("Game engine", ["fm4_static_recomp", "librexruntime.dylib", "librexgpu-fh1.dylib", "libfm4_static_recomp_XMediaFacade_default.dylib",
      "libfm4_static_recomp_SpeechFacade_default.dylib", "graphics-settings.json", "vulkan/lib/libvulkan.1.dylib"].map { engine + $0 }),
    ("Graphics (Vulkan on Metal: MoltenVK)", ["lib/libvulkan.1.dylib", "lib/libMoltenVK.dylib", "vulkan/icd.d/MoltenVK_icd.json"]),
    ("GPU safeguard (Vulkan validation layer)", ["lib/libVkLayer_khronos_validation.dylib",
      "vulkan/explicit_layer.d/VkLayer_khronos_validation.json", "bin/fm4_validation_control"]),
  ]
  for (name, files) in kit {
    let missing = files.filter { !FileManager.default.fileExists(atPath: paths.kit.appendingPathComponent($0).path) }
    list.append(Check(name: name, ok: missing.isEmpty, detail: missing.isEmpty ? "included" : "missing: download FM4 Recomp again"))
  }
  return list
}

// MARK: The disc image

struct DiscCheck {
  let ok: Bool
  let message: String
}

enum Disc { case one, two }

// An Xbox 360 game disc image: "MICROSOFT*XBOX*MEDIA" at sector 32 of a known game partition (the offsets of tools/gdfx.py),
// whose root directory names the files of FM4's Disc 1 (Play Disc) or Disc 2 (Content Install Disc). Which game and version it
// is, is decided by setup.sh's SHA-256 checks of the files it extracts.
func verifyDisc(_ url: URL, _ disc: Disc = .one) -> DiscCheck {
  let name = disc == .one ? "Disc 1 (the Play Disc)" : "Disc 2 (the Content Install Disc)"
  guard let handle = try? FileHandle(forReadingFrom: url) else {
    return DiscCheck(ok: false, message: "This file cannot be read.")
  }
  defer { try? handle.close() }
  let size = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? UInt64) ?? 0
  guard size > 6_000_000_000 else {
    return DiscCheck(ok: false, message: "This file is too small to be a full \(disc == .one ? "Disc 1" : "Disc 2") image (\(gigabytes(size))).")
  }
  let magic = Data("MICROSOFT*XBOX*MEDIA".utf8)
  for (offset, kind) in [(UInt64(0x0000_0000), "XDVDFS"), (0x0000_FB20, "XGD"), (0x0002_0600, "XGD"), (0x0208_0000, "XGD3"), (0x0FD9_0000, "XGD2")] {
    let at = offset + 32 * 2048
    guard at + 20 <= size else { continue }
    try? handle.seek(toOffset: at)
    guard (try? handle.read(upToCount: 20)) == magic, let header = try? handle.read(upToCount: 8), header.count == 8 else { continue }
    // The root directory (sector, size: little-endian after the magic) must name the disc's own files.
    let rootSector = header.prefix(4).enumerated().reduce(UInt64(0)) { $0 | UInt64($1.element) << (8 * UInt64($1.offset)) }
    let rootSize = header.suffix(4).enumerated().reduce(UInt64(0)) { $0 | UInt64($1.element) << (8 * UInt64($1.offset)) }
    let rootAt = offset + rootSector * 2048
    guard rootSize > 0, rootSize <= 32 << 20, rootAt + rootSize <= size else { continue }
    try? handle.seek(toOffset: rootAt)
    let root = (try? handle.read(upToCount: Int(rootSize))) ?? Data()
    // Disc 1: the game and its two facade executables. Disc 2: its content folder and dashboard art, and no facades.
    let names = disc == .one ? ["default.xex", "SpeechFacade_default.xex", "XMediaFacade_default.xex"]
      : ["default.xex", "content", "nxeart", "hash.manifest"]
    let has = { (n: String) in root.range(of: Data(n.utf8)) != nil }
    guard names.allSatisfy(has), disc == .one || !has("XMediaFacade_default.xex") else {
      return DiscCheck(ok: false, message: "This Xbox 360 disc image is not Forza Motorsport 4 \(name): its game files are missing.")
    }
    let title = disc == .one ? "Play Disc" : "Content Install Disc"
    return DiscCheck(ok: true, message: "Forza Motorsport 4 \(title) image (\(kind)), \(gigabytes(size)). Setup checks every file against this build.")
  }
  return DiscCheck(ok: false, message: "This is not an Xbox 360 game disc image. Choose the ISO of your Forza Motorsport 4 \(name).")
}

func gigabytes(_ bytes: UInt64) -> String { String(format: "%.1f GB", Double(bytes) / 1e9) }

private func existingAncestor(_ url: URL) -> URL {
  var probe = url
  while !FileManager.default.fileExists(atPath: probe.path) && probe.path != "/" { probe.deleteLastPathComponent() }
  return probe
}

func freeSpace(at url: URL) -> UInt64 {
  let values = try? existingAncestor(url).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeAvailableCapacityKey])
  // "Important usage" is not reported by every external drive's file system: the plain available capacity then.
  if let important = values?.volumeAvailableCapacityForImportantUsage, important > 0 { return UInt64(important) }
  return UInt64(max(0, values?.volumeAvailableCapacity ?? 0))
}

// What setup.sh writes (locations.sh loc_need checks the same on each volume, plus a 1 GiB margin).
let disc1DataBytes: UInt64 = 8_091_799_701   // Disc 1's files
let disc2FilesBytes: UInt64 = 3_169_575_934  // Disc 2's files (setup.sh --disc2)
let disc2ContentBytes: UInt64 = 3_004_952_576  // the content packages the game installs into the user-data folder
let spaceMargin: UInt64 = 1 << 30

// Free space per volume for these destinations (several on one volume add up): nil when each volume has room, otherwise
// a message naming the short one.
func spaceShortfall(_ needs: [(URL, UInt64)]) -> String? {
  var volumes: [(id: String, name: String, free: UInt64, need: UInt64)] = []
  for (url, bytes) in needs {
    let probe = existingAncestor(url)
    let values = try? probe.resourceValues(forKeys: [.volumeURLKey, .volumeNameKey])
    let id = values?.volume?.path ?? "/"
    if let i = volumes.firstIndex(where: { $0.id == id }) {
      volumes[i].need += bytes
    } else {
      volumes.append((id, values?.volumeName ?? id, freeSpace(at: probe), bytes))
    }
  }
  for v in volumes where v.free < v.need + spaceMargin {
    return "\(v.name) has \(gigabytes(v.free)) free; \(gigabytes(v.need + spaceMargin)) needed there."
  }
  return nil
}

// MARK: Running the kit's scripts

// Runs a kit script with FM4_RECOMP_HOME set, delivering each output line (stdout and stderr) as it arrives. The process is
// returned so a caller can stop it; `done` gets the exit status.
@discardableResult
func runKitScript(_ paths: Paths, _ script: String, _ arguments: [String], line: @escaping (String) -> Void,
                  done: @escaping (Int32) -> Void) -> Process? {
  let process = Process()
  process.executableURL = URL(fileURLWithPath: "/bin/bash")
  process.arguments = [paths.kit.appendingPathComponent(script).path] + arguments
  var env = ProcessInfo.processInfo.environment
  env["FM4_RECOMP_HOME"] = paths.home.path
  // The game started from here has no Dock icon of its own: this app's branded icon stands for it (runtime/video/window_sdl.cpp).
  if script == "play.sh" { env["SDL_MAC_BACKGROUND_APP"] = "1" }
  process.environment = env
  process.currentDirectoryURL = paths.kit
  let pipe = Pipe()
  process.standardOutput = pipe
  process.standardError = pipe
  var pending = Data()
  let lock = NSLock()
  pipe.fileHandleForReading.readabilityHandler = { handle in
    let chunk = handle.availableData
    lock.lock()
    defer { lock.unlock() }
    if chunk.isEmpty { return }
    pending.append(chunk)
    while let newline = pending.firstIndex(of: 0x0A) {
      let text = String(decoding: pending[pending.startIndex..<newline], as: UTF8.self)
      pending.removeSubrange(pending.startIndex...newline)
      line(text)
    }
  }
  process.terminationHandler = { p in
    pipe.fileHandleForReading.readabilityHandler = nil
    let rest = pipe.fileHandleForReading.readDataToEndOfFile()
    lock.lock()
    pending.append(rest)
    let tail = String(decoding: pending, as: UTF8.self)
    pending.removeAll()
    lock.unlock()
    for text in tail.split(separator: "\n", omittingEmptySubsequences: true) { line(String(text)) }
    done(p.terminationStatus)
  }
  do {
    try FileManager.default.createDirectory(at: paths.home, withIntermediateDirectories: true)
    try process.run()
  } catch {
    line("could not start \(script): \(error.localizedDescription)")
    done(127)
    return nil
  }
  return process
}

// "progress <percent> <stage>" from setup.sh.
func parseProgress(_ line: String) -> (Double, String)? {
  let parts = line.split(separator: " ", maxSplits: 2)
  guard parts.count == 3, parts[0] == "progress", let percent = Double(parts[1]) else { return nil }
  return (min(100, max(0, percent)) / 100, String(parts[2]))
}
