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

  var data: URL { home.appendingPathComponent("data") }
  var logs: URL { home.appendingPathComponent("logs") }
  var gpuStop: URL { home.appendingPathComponent("GPU_STOP") }
  var setupDone: URL { data.appendingPathComponent("setup.ok") }
  var readme: URL { kit.appendingPathComponent("README.md") }
  var logo: URL { kit.appendingPathComponent("resources/logo@2x.png") }
}

enum SetupState: String { case needsSetup = "needs-setup", ready }

func state(_ paths: Paths) -> SetupState {
  FileManager.default.fileExists(atPath: paths.setupDone.path) ? .ready : .needsSetup
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
  let kit: [(String, [String])] = [
    ("Game and setup tools", ["bin/fm4recomp", "bin/extract-xiso", "bin/xexsurvey", "bin/gdfx", "setup.sh", "play.sh"]),
    ("Graphics (Vulkan on Metal: MoltenVK)", ["lib/libvulkan.1.dylib", "lib/libMoltenVK.dylib", "vulkan/icd.d/MoltenVK_icd.json"]),
    ("Shader compiler (glslang, SPIRV-Tools)", ["lib/libglslang.16.dylib", "lib/libSPIRV-Tools.dylib", "lib/libSPIRV-Tools-opt.dylib"]),
    ("Window, sound and input (SDL 3)", ["lib/libSDL3.0.dylib"]),
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

// An Xbox 360 game disc image: "MICROSOFT*XBOX*MEDIA" at sector 32 of a known game partition (the offsets of tools/gdfx.py).
// Which game and version it is, is decided by setup.sh's SHA-256 checks of the files it extracts.
func verifyDisc(_ url: URL) -> DiscCheck {
  guard let handle = try? FileHandle(forReadingFrom: url) else {
    return DiscCheck(ok: false, message: "This file cannot be read.")
  }
  defer { try? handle.close() }
  let size = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? UInt64) ?? 0
  guard size > 6_000_000_000 else {
    return DiscCheck(ok: false, message: "This file is too small to be a full Disc 1 image (\(gigabytes(size))).")
  }
  let magic = Data("MICROSOFT*XBOX*MEDIA".utf8)
  for (offset, kind) in [(UInt64(0x0000_0000), "XDVDFS"), (0x0000_FB20, "XGD"), (0x0002_0600, "XGD"), (0x0208_0000, "XGD3"), (0x0FD9_0000, "XGD2")] {
    let at = offset + 32 * 2048
    guard at + 20 <= size else { continue }
    try? handle.seek(toOffset: at)
    guard (try? handle.read(upToCount: 20)) == magic, let header = try? handle.read(upToCount: 8), header.count == 8 else { continue }
    // The root directory (sector, size: little-endian after the magic) must name Disc 1's executables.
    let rootSector = header.prefix(4).enumerated().reduce(UInt64(0)) { $0 | UInt64($1.element) << (8 * UInt64($1.offset)) }
    let rootSize = header.suffix(4).enumerated().reduce(UInt64(0)) { $0 | UInt64($1.element) << (8 * UInt64($1.offset)) }
    let rootAt = offset + rootSector * 2048
    guard rootSize > 0, rootSize <= 32 << 20, rootAt + rootSize <= size else { continue }
    try? handle.seek(toOffset: rootAt)
    let root = (try? handle.read(upToCount: Int(rootSize))) ?? Data()
    let names = ["default.xex", "SpeechFacade_default.xex", "XMediaFacade_default.xex"]
    guard names.allSatisfy({ root.range(of: Data($0.utf8)) != nil }) else {
      return DiscCheck(ok: false, message: "This Xbox 360 disc image is not Forza Motorsport 4 Disc 1 (the Play Disc): its game files are missing.")
    }
    return DiscCheck(ok: true, message: "Forza Motorsport 4 Play Disc image (\(kind)), \(gigabytes(size)). Setup checks every file against this build.")
  }
  return DiscCheck(ok: false, message: "This is not an Xbox 360 game disc image. Choose the ISO of your Forza Motorsport 4 Disc 1 (Play Disc).")
}

func gigabytes(_ bytes: UInt64) -> String { String(format: "%.1f GB", Double(bytes) / 1e9) }

func freeSpace(at url: URL) -> UInt64 {
  var probe = url
  while !FileManager.default.fileExists(atPath: probe.path) && probe.path != "/" { probe.deleteLastPathComponent() }
  let values = try? probe.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
  return UInt64(max(0, values?.volumeAvailableCapacityForImportantUsage ?? 0))
}

let setupBytesNeeded: UInt64 = 9_000_000_000

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
