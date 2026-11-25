// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

@main enum ProfileSetupTests {
  static func main() throws {
    let fm = FileManager.default
    let temp = fm.temporaryDirectory.appendingPathComponent("fm4-profile-" + UUID().uuidString)
    defer { try? fm.removeItem(at: temp) }
    for external in [false, true] {
      let user = temp.appendingPathComponent(external ? "external" : "internal")
      let support = user.appendingPathComponent("Library/Application Support")
      let previous = support.appendingPathComponent("FM4 Recomp ShiftGlue Playtest")
      let current = support.appendingPathComponent("FM4 Recomp")
      let paths = Paths(kit: temp, home: current)
      let disc = (external ? user.appendingPathComponent("separate") : previous).appendingPathComponent("data")
      try fm.createDirectory(at: disc, withIntermediateDirectories: true)
      try fm.createDirectory(at: previous, withIntermediateDirectories: true)
      try Data("verified".utf8).write(to: disc.appendingPathComponent("setup.ok"))
      try Data("version=1\ndata_root=\(disc.path)\n".utf8).write(to: previous.appendingPathComponent("locations.conf"))
      try adoptPreviousSetup(Paths(kit: temp, home: user.appendingPathComponent("override")), userHome: user)
      precondition(!fm.fileExists(atPath: current.path))
      try adoptPreviousSetup(paths, userHome: user)
      let mapped = Locations.read(paths)
      precondition(mapped.dataRoot.path == (external ? disc : current.appendingPathComponent("data")).path)
      precondition(mapped.userDataRoot.path == current.appendingPathComponent("data/user-data").path)
      let data = try Data(contentsOf: mapped.dataRoot.appendingPathComponent("setup.ok"))
      precondition(data == Data("verified".utf8))
      precondition(fm.fileExists(atPath: disc.appendingPathComponent("setup.ok").path))
      try Data("version=1\n# existing config\n".utf8).write(to: paths.locationsFile)
      try adoptPreviousSetup(paths, userHome: user)
      let preserved = try String(contentsOf: paths.locationsFile, encoding: .utf8)
      precondition(preserved.contains("existing config"))
    }
    print("PASS: canonical home, explicit override, retained internal/external disc and existing configuration")
  }
}
