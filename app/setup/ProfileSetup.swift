// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

// A first-time 0.3.0 installation can have its verified disc inside the old app home.
// Adopt only the recorded storage locations. Saves are converted separately under the play lock.
func adoptPreviousSetup(_ paths: Paths, userHome: URL) throws {
  let fm = FileManager.default
  let support = userHome.appendingPathComponent("Library/Application Support")
  let home = support.appendingPathComponent("FM4 Recomp")
  let previous = support.appendingPathComponent("FM4 Recomp ShiftGlue Playtest")
  guard paths.home.standardizedFileURL.path == home.standardizedFileURL.path,
        !fm.fileExists(atPath: paths.locationsFile.path),
        !fm.fileExists(atPath: paths.data.appendingPathComponent("setup.ok").path) else { return }
  let old = Paths(kit: paths.kit, home: previous)
  let hasConfig = fm.fileExists(atPath: old.locationsFile.path)
  guard hasConfig || fm.fileExists(atPath: old.data.appendingPathComponent("setup.ok").path) else { return }

  let raw = hasConfig ? try String(contentsOf: old.locationsFile, encoding: .utf8) : ""
  var recorded: [String: URL] = ["data_root": old.data, "user_data_root": old.data.appendingPathComponent("user-data")]
  for line in raw.split(separator: "\n") where !line.hasPrefix("#") {
    let fields = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
    if fields.count == 2, ["data_root", "user_data_root", "disc2_root", "disc2_map"].contains(String(fields[0])) {
      let path = String(fields[1])
      guard path.hasPrefix("/") else { throw CocoaError(.fileReadCorruptFile) }
      recorded[String(fields[0])] = URL(fileURLWithPath: path).standardizedFileURL
    }
  }
  try fm.createDirectory(at: home, withIntermediateDirectories: true)
  let staging = home.appendingPathComponent(".import-" + UUID().uuidString)
  try fm.createDirectory(at: staging, withIntermediateDirectories: false)
  defer { try? fm.removeItem(at: staging) }

  var installed: [String: URL] = [:]
  for key in ["data_root", "user_data_root", "disc2_root", "disc2_map"] {
    guard let src = recorded[key] else { continue }
    guard src.path.hasPrefix(previous.path + "/") else { installed[key] = src; continue }
    let relative = String(src.path.dropFirst(previous.path.count + 1))
    let dest: URL
    if key == "user_data_root", relative.hasPrefix("shiftglue/") {
      dest = home.appendingPathComponent("data/user-data")
      installed[key] = dest // The playtest importer reads the old account and its headers together.
      continue
    }
    dest = home.appendingPathComponent(relative)
    installed[key] = dest
    if !fm.fileExists(atPath: src.path) {
      // Disc 1 and external storage must pass the launcher's normal setup checks.
      continue
    }
    if fm.fileExists(atPath: dest.path) {
      // Disc 1 and its user-data/map can be nested: the outer copy installs those too.
      if key == "data_root", !fm.fileExists(atPath: dest.appendingPathComponent("setup.ok").path) {
        throw CocoaError(.fileWriteFileExists, userInfo: [NSFilePathErrorKey: dest.path])
      }
      continue
    }
    let temp = staging.appendingPathComponent(key)
    try fm.copyItem(at: src, to: temp)
    try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
    try fm.moveItem(at: temp, to: dest)
  }
  if fm.fileExists(atPath: old.gpuStop.path), !fm.fileExists(atPath: paths.gpuStop.path) {
    try fm.copyItem(at: old.gpuStop, to: paths.gpuStop)
  }
  let text = "version=1\n" + installed.keys.sorted().map { "\($0)=\(installed[$0]!.path)\n" }.joined()
  let config = staging.appendingPathComponent("locations.conf")
  try Data(text.utf8).write(to: config)
  try fm.moveItem(at: config, to: paths.locationsFile)
}
