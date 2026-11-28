// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Brings the saved games a player kept with FM4 Recomp 0.3.x (the ShiftGlue engine) back into this engine's layout, before the
// game mounts the user-data folder. play.sh runs it (migrate-from-0.3.sh) under its per-profile lock. Adapted from 0.3.1's
// scripts/alpha/migrate-career.swift (f7d26cd, the opposite direction): the same lstat checks, staging and exclusive renames.
//
// 0.3.x layout (ShiftGlue ContentManager: <user-data>/<xuid>/<title>/<type>/<file name>/ and
// <user-data>/<xuid>/<title>/Headers/<type>/<file name>.header, a 0x148-byte XCONTENT_AGGREGATE_DATA: XCONTENT_DATA 0x134,
// xuid +0x138, title id +0x140). This engine (runtime/kernel/content.h): <user-data>/content/1/<type>/<file name>/ and
// <file name>.xcontent, the 0x134-byte XCONTENT_DATA. The record is the header's first 0x134 bytes.
//
// Rules: 0.3.x was played after 0.2.0, so its copy of a package replaces this layout's copy
// of the same name; the replaced copy moves (never deleted) into <user-data>/saves-replaced-<date>/. Identical copies are left
// alone. A package only in this layout stays. Everything is checked before anything changes; any surprise refuses the whole
// import, changes nothing, and play.sh does not start the game. A finished import writes its done marker and never runs again.
import Darwin
import Foundation

private let fm = FileManager.default
private let account = "B13EBABEBABEBABE"  // the offline user both 0.3.x and its importer used (migrate-career.swift)
private let titleID = "4D530910"
private let saveType = "00000001"
private let headerSize = 0x148
private let recordSize = 0x134
private let prefix: [UInt8] = [0, 0, 0, 1, 0, 0, 0, 1]  // device 1 (HDD), content type 1 (saved game)

private struct MigrationError: Error, CustomStringConvertible {
  let description: String
  init(_ message: String) { description = message }
}

// Notes and the refusal share unbuffered stderr, so a refusal is always the last line (play.sh shows the last line).
private func note(_ message: String) { fputs("saves: \(message)\n", stderr) }

private func info(_ path: URL) throws -> stat? {
  var result = stat()
  if lstat(path.path, &result) == 0 { return result }
  if errno == ENOENT { return nil }
  throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
}

private func kind(_ s: stat) -> mode_t { s.st_mode & S_IFMT }

private func directory(_ path: URL, create: Bool = false) throws -> Bool {
  if let s = try info(path) {
    guard kind(s) == S_IFDIR else { throw MigrationError("\(path.path) is not a folder (a symbolic link is refused)") }
    return true
  }
  if create { try fm.createDirectory(at: path, withIntermediateDirectories: false); return true }
  return false
}

private func regular(_ path: URL) throws -> Bool {
  guard let s = try info(path) else { return false }
  guard kind(s) == S_IFREG else { throw MigrationError("\(path.path) is not a regular file (a symbolic link is refused)") }
  return true
}

// runtime/kernel/file_system.cpp HostOnlyName: names the guest never sees.
private func hostOnly(_ name: String) -> Bool { name == ".DS_Store" || name.hasPrefix("._") }

private func entries(_ path: URL) throws -> [String] {
  try fm.contentsOfDirectory(atPath: path.path).filter { !hostOnly($0) }.sorted()
}

// runtime/kernel/content.cpp SafeComponent, and ContentFileName's 42-byte field.
private func safeName(_ name: String) -> Bool {
  let bytes = Array(name.utf8)
  return !bytes.isEmpty && bytes.count <= 42 && name != "." && name != ".." &&
    bytes.allSatisfy { $0 > 31 && $0 < 127 && $0 != 0x2f && $0 != 0x5c && $0 != 0x3a }
}

// The record this engine lists for a 0.3.x header (content.h kContentDataSize; ShiftGlue content_manager.h
// XCONTENT_AGGREGATE_DATA). The xuid (+0x138) and the padding (+0x134, +0x144) have no place in the record.
private func record(fromHeader url: URL, package: String) throws -> Data {
  guard try regular(url) else { throw MigrationError("\(package): its 0.3 header is missing") }
  let header = try Data(contentsOf: url)
  guard header.count == headerSize else { throw MigrationError("\(package): its 0.3 header is \(header.count) bytes, not \(headerSize)") }
  guard header.starts(with: prefix) else { throw MigrationError("\(package): its 0.3 header is not a saved game on the hard disk") }
  guard header[0x140..<0x144].elementsEqual([0x4d, 0x53, 0x09, 0x10]) else {
    throw MigrationError("\(package): its 0.3 header belongs to another title")
  }
  let name = header[0x108..<0x132].prefix(while: { $0 != 0 })
  guard name.elementsEqual(package.utf8), safeName(package) else {
    throw MigrationError("\(package): its 0.3 header names another package")
  }
  return header.prefix(recordSize)
}

// The fields this engine and the game identify an item by: device, type, display name (to its terminator), file name.
private func identity(ofRecord data: Data) -> [UInt8] {
  var end = 8
  while end < 0x108 && (data[end] != 0 || data[end + 1] != 0) { end += 2 }
  return Array(data[0..<end]) + [0, 0] + Array(data[0x108..<0x132].prefix(while: { $0 != 0 }))
}

// A package folder: folders and regular files only. A name with '%' or ':' is refused: this engine stores ':' as "%3A" and
// '%' as "%25" (file_system.cpp HostNameOf), 0.3.x stored names verbatim, and which spelling the game wrote cannot be told.
// Returns the newest modification time of its files. This engine's own copy (`ours`) may hold its encoded names.
private func check(tree url: URL, ours: Bool = false) throws -> timespec {
  var newest = timespec(tv_sec: 0, tv_nsec: 0)
  guard let s = try info(url) else { throw MigrationError("\(url.path) is missing") }
  switch kind(s) {
  case S_IFDIR:
    for name in try entries(url) {
      guard ours || (!name.contains("%") && !name.contains(":")) else {
        throw MigrationError("\(url.appendingPathComponent(name).path): a name with '%' or ':' is not imported")
      }
      let t = try check(tree: url.appendingPathComponent(name), ours: ours)
      if t.tv_sec > newest.tv_sec || (t.tv_sec == newest.tv_sec && t.tv_nsec > newest.tv_nsec) { newest = t }
    }
  case S_IFREG: newest = s.st_mtimespec
  default: throw MigrationError("\(url.path) is not a file or folder (a symbolic link is refused)")
  }
  return newest
}

private func sameTree(_ a: URL, _ b: URL) throws -> Bool {
  guard let sa = try info(a), let sb = try info(b), kind(sa) == kind(sb) else { return false }
  if kind(sa) == S_IFREG {
    guard sa.st_size == sb.st_size else { return false }
    let left = try Data(contentsOf: a), right = try Data(contentsOf: b)
    return left == right
  }
  let names = try entries(a), others = try entries(b)
  guard names == others else { return false }
  for name in names {
    if try !sameTree(a.appendingPathComponent(name), b.appendingPathComponent(name)) { return false }
  }
  return true
}

private func checkedCopy(_ source: URL, _ destination: URL) throws {
  guard let s = try info(source) else { throw MigrationError("\(source.path) is missing") }
  switch kind(s) {
  case S_IFDIR:
    try fm.createDirectory(at: destination, withIntermediateDirectories: false)
    for name in try entries(source) {
      try checkedCopy(source.appendingPathComponent(name), destination.appendingPathComponent(name))
    }
  case S_IFREG: try fm.copyItem(at: source, to: destination)
  default: throw MigrationError("\(source.path) is not a file or folder")
  }
}

private func inode(_ path: URL) throws -> String {
  guard let s = try info(path), kind(s) == S_IFDIR else { throw MigrationError("\(path.path): the staged folder is missing") }
  return "\(s.st_ino)"
}

// An exclusive rename never replaces a destination that appeared meanwhile (same volume: everything is under the user-data root).
private func publish(_ source: URL, at target: URL) throws {
  if renamex_np(source.path, target.path, UInt32(RENAME_EXCL)) != 0 {
    let code = errno
    throw MigrationError("cannot move \(source.path) to \(target.path): \(String(cString: strerror(code)))")
  }
}

private func text(_ url: URL) throws -> String {
  guard try regular(url) else { throw MigrationError("\(url.path): the import receipt is incomplete") }
  return try String(contentsOf: url, encoding: .utf8)
}

private enum Action: String { case add, replace }

private struct Layout {
  let root: URL
  var sourceSaves: URL { root.appendingPathComponent("\(account)/\(titleID)/\(saveType)") }
  var sourceHeaders: URL { root.appendingPathComponent("\(account)/\(titleID)/Headers/\(saveType)") }
  var saves: URL { root.appendingPathComponent("content/1/\(saveType)") }
  var state: URL { root.appendingPathComponent(".fm4-from-0.3-saves-v1") }
  func header(_ name: String) -> URL { sourceHeaders.appendingPathComponent(name + ".header") }
  func target(_ name: String) -> URL { saves.appendingPathComponent(name) }
  func targetRecord(_ name: String) -> URL { saves.appendingPathComponent(name + ".xcontent") }
}

private struct Planned { let name: String; let action: Action }

// Read-only: what the import would do, or the first reason to refuse.
private func plan(_ l: Layout) throws -> [Planned] {
  var names: [String] = []
  if try directory(l.sourceSaves) {
    for name in try entries(l.sourceSaves) {
      guard safeName(name), try directory(l.sourceSaves.appendingPathComponent(name)) else {
        throw MigrationError("\(l.sourceSaves.appendingPathComponent(name).path) is not a saved-game folder")
      }
      names.append(name)
    }
  }
  if try directory(l.sourceHeaders) {
    for file in try entries(l.sourceHeaders) {
      guard file.hasSuffix(".header"), try regular(l.sourceHeaders.appendingPathComponent(file)) else {
        throw MigrationError("\(l.sourceHeaders.appendingPathComponent(file).path) is not a saved-game header")
      }
      let name = String(file.dropLast(7))
      // 0.3.x lists folders only (ShiftGlue ListContent), so a header without its folder was invisible there.
      if !names.contains(name) { note("\(name): a 0.3 header without its folder, not imported") }
    }
  }
  var result: [Planned] = []
  for name in names {
    let source = l.sourceSaves.appendingPathComponent(name)
    // ShiftGlue would show a folder without its header under the folder's name; inventing a record here is refused instead.
    guard try regular(l.header(name)) else {
      throw MigrationError("\(source.path) has no header (\(l.header(name).path))")
    }
    let wanted = try record(fromHeader: l.header(name), package: name)
    let sourceNewest = try check(tree: source)
    let hasTarget = try directory(l.target(name)), hasRecord = try regular(l.targetRecord(name))
    if !hasTarget && !hasRecord { result.append(Planned(name: name, action: .add)); continue }
    guard hasTarget && hasRecord else {
      throw MigrationError("\(l.target(name).path) is incomplete (a folder without its .xcontent record, or a record without its folder)")
    }
    let current = try Data(contentsOf: l.targetRecord(name))
    guard current.count == recordSize, current.starts(with: prefix),
          current[0x108..<0x132].prefix(while: { $0 != 0 }).elementsEqual(name.utf8) else {
      throw MigrationError("\(l.targetRecord(name).path) is not this package's \(recordSize)-byte record")
    }
    let targetNewest = try check(tree: l.target(name), ours: true)
    if identity(ofRecord: current) == identity(ofRecord: wanted), try sameTree(source, l.target(name)) { continue }
    // 0.3.x is the newer copy by assumption; a file here written after every file of the 0.3 copy contradicts that.
    if targetNewest.tv_sec > sourceNewest.tv_sec || (targetNewest.tv_sec == sourceNewest.tv_sec && targetNewest.tv_nsec > sourceNewest.tv_nsec) {
      throw MigrationError("\(l.target(name).path) was saved after the 0.3 copy (\(source.path)); keep one: move the other out of \(l.root.path)")
    }
    result.append(Planned(name: name, action: .replace))
  }
  return result
}

private func backupName() -> String {
  let f = DateFormatter()
  f.locale = Locale(identifier: "en_US_POSIX")
  f.dateFormat = "yyyy-MM-dd'_'HHmmss"
  return "saves-replaced-" + f.string(from: Date())
}

private func stage(_ p: Planned, _ l: Layout, backup: String) throws {
  let wanted = try record(fromHeader: l.header(p.name), package: p.name)
  let staging = l.state.appendingPathComponent(".stage-" + UUID().uuidString)
  try fm.createDirectory(at: staging, withIntermediateDirectories: false)
  defer { try? fm.removeItem(at: staging) }
  let payload = staging.appendingPathComponent("payload")
  try checkedCopy(l.sourceSaves.appendingPathComponent(p.name), payload)
  try wanted.write(to: staging.appendingPathComponent("record"))
  try inode(payload).write(to: staging.appendingPathComponent("inode"), atomically: true, encoding: .utf8)
  try p.action.rawValue.write(to: staging.appendingPathComponent("action"), atomically: true, encoding: .utf8)
  if p.action == .replace {
    _ = try directory(l.root.appendingPathComponent(backup), create: true)
    try backup.write(to: staging.appendingPathComponent("backup"), atomically: true, encoding: .utf8)
  }
  try publish(staging, at: l.state.appendingPathComponent("pending-" + p.name))
}

// Completes a staged package; also the crash recovery (each step is skipped once done, decided by what is on disk).
// Order: the old record out, the old folder out, the new folder in, the new record in. Until the new record is in, the game
// does not list the package, and play.sh does not start it after an error.
private func finish(_ name: String, _ l: Layout) throws {
  let pending = l.state.appendingPathComponent("pending-" + name)
  let payload = pending.appendingPathComponent("payload"), staged = pending.appendingPathComponent("record")
  guard let action = Action(rawValue: try text(pending.appendingPathComponent("action"))) else {
    throw MigrationError("\(pending.path): unknown action")
  }
  let expected = try text(pending.appendingPathComponent("inode"))
  let wanted = try record(fromHeader: l.header(name), package: name)
  let target = l.target(name), targetRecord = l.targetRecord(name)
  if try directory(payload) {
    if action == .replace {
      let backup = l.root.appendingPathComponent(try text(pending.appendingPathComponent("backup")))
      guard backup.lastPathComponent.hasPrefix("saves-replaced-"), try directory(backup) else {
        throw MigrationError("\(pending.path): its backup folder is missing")
      }
      if try regular(targetRecord) { try publish(targetRecord, at: backup.appendingPathComponent(name + ".xcontent")) }
      if try directory(target) { try publish(target, at: backup.appendingPathComponent(name)) }
    }
    try publish(payload, at: target)
  }
  guard try inode(target) == expected else { throw MigrationError("\(target.path) changed during the import") }
  if try regular(staged) {
    guard try Data(contentsOf: staged) == wanted else { throw MigrationError("\(l.header(name).path) changed during the import") }
    try publish(staged, at: targetRecord)
  } else {
    guard try regular(targetRecord), try Data(contentsOf: targetRecord) == wanted else {
      throw MigrationError("\(targetRecord.path) changed during the import")
    }
  }
  // Completion becomes visible before the receipt goes: an interrupted clean-up never looks unfinished.
  let finished = l.state.appendingPathComponent(".finished-" + UUID().uuidString)
  try publish(pending, at: finished)
  try fm.removeItem(at: finished)
  note("\(name): " + (action == .add ? "imported from 0.3" : "the 0.3 copy is in use; the previous copy is in the saves-replaced folder"))
}

private func migrate(_ root: URL) throws {
  guard try directory(root) else { throw MigrationError("the user-data folder \(root.path) is missing") }
  let l = Layout(root: root)
  if try directory(l.state), try regular(l.state.appendingPathComponent("done")) { return }
  guard try directory(root.appendingPathComponent(account)) else { return }  // never ran 0.3.x on this folder
  _ = try directory(l.state, create: true)
  for entry in try entries(l.state) {
    let path = l.state.appendingPathComponent(entry)
    if entry.hasPrefix("pending-") { try finish(String(entry.dropFirst(8)), l) }
    else if entry.hasPrefix(".stage-") || entry.hasPrefix(".finished-") { try fm.removeItem(at: path) }
  }
  let work = try plan(l)
  if !work.isEmpty {
    var part = root
    for name in ["content", "1", saveType] { part.appendPathComponent(name); _ = try directory(part, create: true) }
  }
  let backup = backupName()
  for p in work {
    try stage(p, l, backup: backup)
    try finish(p.name, l)
  }
  // The 0.3 copies stay where they are; a later deletion in this engine stays deleted.
  try Data("1\n".utf8).write(to: l.state.appendingPathComponent("done"), options: .atomic)
}

do {
  guard CommandLine.arguments.count == 2 else { throw MigrationError("usage: migrate-from-0.3.sh <user-data folder>") }
  try migrate(URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true))
} catch {
  fputs("saves: \(error)\n", stderr)
  exit(1)
}
