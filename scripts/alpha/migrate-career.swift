// SPDX-License-Identifier: GPL-3.0-or-later
// Import FM4 Recomp 0.2.0 saved-game packages before the game mounts this directory.
// The caller holds the per-profile launch lock for the duration of migration and play.
import Darwin
import Foundation

private let fm = FileManager.default
private let saveType = "00000001"
// Both engines sign in the same built-in offline user. The old folder "1" identifies
// a storage device, not a player. The legacy writer only handles this FM4 title.
private let userAccount = "B13EBABEBABEBABE"
private let titleID = "4D530910"

private struct MigrationError: Error, CustomStringConvertible {
    let description: String
    init(_ message: String) { description = message }
}

private func info(_ path: URL) throws -> stat? {
    var result = stat()
    if lstat(path.path, &result) == 0 { return result }
    if errno == ENOENT { return nil }
    throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
}

private func kind(_ info: stat) -> mode_t { info.st_mode & S_IFMT }

private func directory(_ path: URL, create: Bool = false) throws -> Bool {
    if let s = try info(path) {
        guard kind(s) == S_IFDIR else { throw MigrationError("expected a real directory at \(path.lastPathComponent)") }
        return true
    }
    if create { try fm.createDirectory(at: path, withIntermediateDirectories: false); return true }
    return false
}

private func regular(_ path: URL) throws -> Bool {
    guard let s = try info(path) else { return false }
    guard kind(s) == S_IFREG else { throw MigrationError("expected a regular save file at \(path.lastPathComponent)") }
    return true
}

private func files(_ path: URL) throws -> [URL] {
    try fm.contentsOfDirectory(at: path, includingPropertiesForKeys: nil).filter {
        $0.lastPathComponent != ".DS_Store" && !$0.lastPathComponent.hasPrefix("._")
    }
}

private func checkedCopy(_ source: URL, _ destination: URL) throws {
    guard let s = try info(source) else { throw MigrationError("missing save payload") }
    switch kind(s) {
    case S_IFDIR:
        try fm.createDirectory(at: destination, withIntermediateDirectories: false)
        // Match the old filesystem's explicit exclusions; other dot-prefixed files can be guest data.
        for next in try files(source) {
            try checkedCopy(next, destination.appendingPathComponent(next.lastPathComponent))
        }
    case S_IFREG: try fm.copyItem(at: source, to: destination)
    default: throw MigrationError("unsupported item inside a legacy save; original retained")
    }
}

// runtime/kernel/content.h (0.2.0) and ShiftGlue ContentManager::WriteContentHeaderFile.
// Read the actual XCONTENT_DATA strings. The legacy writer preserves guest-memory padding
// after their terminators; the SDK's aggregate constructor clears it.
private func nativeHeader(_ recordURL: URL, package: String) throws -> Data {
    guard try regular(recordURL) else { throw MigrationError("legacy content record missing") }
    let old = try Data(contentsOf: recordURL)
    let prefix: [UInt8] = [0, 0, 0, 1, 0, 0, 0, 1]
    guard old.count == 0x134, old.starts(with: prefix) else { throw MigrationError("invalid legacy saved-game record") }
    let name = old[0x108..<0x132].prefix(while: { $0 != 0 })
    guard name.elementsEqual(package.utf8), !name.isEmpty, package != ".", package != "..",
          name.allSatisfy({ $0 > 31 && $0 < 127 && $0 != 47 && $0 != 92 && $0 != 58 }) else {
        throw MigrationError("the saved-game record does not match its package")
    }
    var end = 8
    // The SDK reserves one UTF-16 unit for a trailing null.
    while end < 0x106 && (old[end] != 0 || old[end + 1] != 0) { end += 2 }
    var header = Data(repeating: 0, count: 0x148)
    header.replaceSubrange(0..<8, with: prefix)
    header.replaceSubrange(8..<end, with: old[8..<end])
    header.replaceSubrange(0x108..<(0x108 + name.count), with: name)
    header.replaceSubrange(0x140..<0x144, with: [0x4d, 0x53, 0x09, 0x10])
    return header
}

private func identity(_ path: URL) throws -> String {
    guard let s = try info(path), kind(s) == S_IFDIR else { throw MigrationError("missing staged save directory") }
    return "\(s.st_ino)"
}

// An exclusive rename never replaces a destination that appeared while staging.
private func publish(_ source: URL, at target: URL) throws {
    if renamex_np(source.path, target.path, UInt32(RENAME_EXCL)) != 0 {
        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }
}

private func install(_ name: String, record: URL, legacy: URL, saves: URL, headers: URL, state: URL) throws {
    let header = try nativeHeader(record, package: name)
    let source = legacy.appendingPathComponent(name)
    guard try directory(source) else { throw MigrationError("save payload missing for \(name)") }
    let target = saves.appendingPathComponent(name)
    let targetHeader = headers.appendingPathComponent(name + ".header")
    let pending = state.appendingPathComponent("pending-" + name)
    if try !directory(pending) {
        let hasTarget = try directory(target), hasHeader = try regular(targetHeader)
        if hasTarget && hasHeader {
            print("saves: keeping existing native package \(name); original 0.2.0 copy retained")
            return
        }
        guard !hasTarget && !hasHeader else { throw MigrationError("incomplete existing native package \(name); both copies retained") }
        let staging = state.appendingPathComponent(".stage-" + UUID().uuidString)
        try fm.createDirectory(at: staging, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: staging) }
        let payload = staging.appendingPathComponent("payload")
        try checkedCopy(source, payload)
        try header.write(to: staging.appendingPathComponent("header"))
        try identity(payload).write(to: staging.appendingPathComponent("inode"), atomically: true, encoding: .utf8)
        try publish(staging, at: pending)
    }
    // Crash recovery. The companion receipt identifies the exact directory renamed below.
    // Until both final paths exist this package has no game-visible header, and the caller
    // does not start the guest after a migration error.
    let payload = pending.appendingPathComponent("payload")
    let stagedHeader = pending.appendingPathComponent("header")
    let inode = pending.appendingPathComponent("inode")
    guard try regular(inode) else { throw MigrationError("migration receipt missing") }
    let expected = try String(contentsOf: inode, encoding: .utf8)
    if try directory(payload) { try publish(payload, at: target) }
    guard try identity(target) == expected else { throw MigrationError("native package changed during migration") }
    if try regular(stagedHeader) {
        guard try Data(contentsOf: stagedHeader) == header else { throw MigrationError("legacy record changed during migration") }
        try publish(stagedHeader, at: targetHeader)
    } else {
        guard try regular(targetHeader), try Data(contentsOf: targetHeader) == header else { throw MigrationError("native header changed during migration") }
    }
    // Make completion visible before removing the receipt: interruption during cleanup
    // must never make a completed package look unfinished on the next launch.
    let finished = state.appendingPathComponent(".finished-" + UUID().uuidString)
    try publish(pending, at: finished)
    try fm.removeItem(at: finished)
    print("saves: imported \(name) from 0.2.0")
}

private func migrate(_ root: URL) throws {
    guard try directory(root) else { throw MigrationError("configured user-data folder is missing") }
    var legacy = root
    for part in ["content", "1", saveType] {
        legacy.appendPathComponent(part)
        guard try directory(legacy) else { return }
    }
    let state = root.appendingPathComponent(".fm4-legacy-saves-v1")
    let done = state.appendingPathComponent("done")
    if try directory(state), try regular(done) { return }
    let records = try files(legacy).filter { $0.pathExtension == "xcontent" }
        .sorted { $0.lastPathComponent < $1.lastPathComponent }
    if records.isEmpty { return }
    _ = try directory(state, create: true)
    let user = root.appendingPathComponent(userAccount)
    let title = user.appendingPathComponent(titleID)
    let saves = title.appendingPathComponent(saveType)
    let headers = title.appendingPathComponent("Headers")
    let typeHeaders = headers.appendingPathComponent(saveType)
    for path in [user, title, saves, headers, typeHeaders] { _ = try directory(path, create: true) }
    for record in records {
        try install(record.deletingPathExtension().lastPathComponent, record: record, legacy: legacy, saves: saves, headers: typeHeaders, state: state)
    }
    // An intentional deletion from the new engine remains deleted after subsequent launches.
    try Data("1\n".utf8).write(to: done, options: .atomic)
}

do {
    guard CommandLine.arguments.count == 2 else { throw MigrationError("usage: migrate-career.sh <configured-user-root>") }
    try migrate(URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true))
} catch {
    fputs("saves: \(error)\n", stderr)
    exit(1)
}
