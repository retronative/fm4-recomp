// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp's Keyboard Controls: the game's keyboard bindings (KeyBindings.swift; the model's capture in SetupView.swift), shown
// in the game (Command-comma: FM4 Recomp --controls-panel, ControlsPanel.swift; the runtime pauses the game and reloads the file
// when it closes, runtime/kernel/controls_screen.h). Select an action, press its new key (Cancel is a button: Escape is a key the
// game can use); a key another action has asks first; Reset to Defaults asks first. Every change is saved at once; the gamepad's
// layout is fixed. (Until 2026-11-14 this was the launcher's Settings page; it moved into the game.)

import AppKit
import Carbon.HIToolbox
import SwiftUI

// A modifier key's flagsChanged event is its press when its flag is now set (its release clears it).
func isModifierPress(_ event: NSEvent) -> Bool {
  let flags: [Int: NSEvent.ModifierFlags] = [
    kVK_Shift: .shift, kVK_RightShift: .shift, kVK_Control: .control, kVK_RightControl: .control,
    kVK_Option: .option, kVK_RightOption: .option, kVK_Command: .command, kVK_RightCommand: .command,
    kVK_CapsLock: .capsLock, kVK_Function: .function,
  ]
  guard let flag = flags[Int(event.keyCode)] else { return true }
  return event.modifierFlags.contains(flag)
}

struct ControlsPage: View {
  @ObservedObject var model: SetupModel
  // Reset removes the file (no file is the defaults), so it is offered whenever one exists, even one equal to the defaults.
  private var hasSavedFile: Bool { FileManager.default.fileExists(atPath: model.paths.keyBindingsFile.path) }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      VStack(alignment: .leading, spacing: 3) {
        Text("Keyboard Controls").font(.system(size: 22, weight: .bold))
        Text("Select an action, then press its new key. The gamepad uses the Xbox layout and cannot be changed here.")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary)
      }
      ScrollView {
        VStack(spacing: 0) {
          HStack {
            Text("Action").frame(width: 170, alignment: .leading)
            Text("Gamepad").frame(width: 150, alignment: .leading)
            Text("Keyboard")
            Spacer()
          }
          .font(.system(size: 11, weight: .semibold)).foregroundStyle(Theme.secondary).padding(.horizontal, 12).padding(.vertical, 6)
          ForEach(keyActions, id: \.id) { action in ActionRow(model: model, action: action) }
        }
      }
      .frame(height: 366)  // the panel is 790 high (ControlsPanel.swift: the Graphics page needs it)
      .background(RoundedRectangle(cornerRadius: 12).fill(Theme.card))
      .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.line))
      StatusBar(model: model).frame(height: 44)
      HStack {
        SecondaryButton(title: "Reset to Defaults") { model.endCapture(); model.controlsNotice = ""; model.confirmingReset = true }
          .disabled(!hasSavedFile)
          .opacity(hasSavedFile ? 1 : 0.4)
        Spacer()
        // Back to the game: the panel ends, the game reloads the file and resumes (ControlsPanel.swift).
        PrimaryButton(title: "Done", enabled: model.capturing == nil && !model.confirmingReset) { model.endCapture(); model.leaveControls() }
      }
    }
  }
}

struct ActionRow: View {
  @ObservedObject var model: SetupModel
  let action: KeyAction
  private var selected: Bool { model.capturing == action.id }

  var body: some View {
    Button { model.beginCapture(action.id) } label: {
      HStack {
        Text(action.label).font(.system(size: 12.5)).frame(width: 170, alignment: .leading)
        Text(action.gamepad).font(.system(size: 12)).foregroundStyle(Theme.secondary).frame(width: 150, alignment: .leading)
        Text(selected ? "Press a key…" : model.bindings.keys(for: action.id).map(keyLabel).joined(separator: "  /  "))
          .font(.system(size: 12.5, weight: .semibold)).foregroundStyle(selected ? Theme.red : Color.white)
        Spacer()
      }
      .padding(.horizontal, 12).padding(.vertical, 5)
      .background(selected ? Theme.red.opacity(0.14) : Color.clear)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}

// What the page is waiting for, or the last result.
struct StatusBar: View {
  @ObservedObject var model: SetupModel

  var body: some View {
    HStack(spacing: 10) {
      if let change = model.pendingChange, let from = change.from {
        Text("\(keyLabel(change.key)) is used by \(actionLabel(from)). Replace? \(actionLabel(from)) will then use \(change.fromGets.map(keyLabel).joined(separator: " / ")).")
          .font(.system(size: 12.5)).fixedSize(horizontal: false, vertical: true)
        Spacer()
        SecondaryButton(title: "Cancel") { model.endCapture() }
        PrimaryButton(title: "Replace") { model.apply(change) }
      } else if let action = model.capturing {
        Text("Press the new key for \(actionLabel(action)).\(model.controlsNotice.isEmpty ? "" : " " + model.controlsNotice)")
          .font(.system(size: 12.5)).fixedSize(horizontal: false, vertical: true)
        Spacer()
        SecondaryButton(title: "Cancel") { model.endCapture(); model.controlsNotice = "" }
      } else if model.confirmingReset {
        Text("Reset every key to the defaults (W throttle, S brake, A/D steer, Space = A, Backspace = B, arrows = D-pad)?")
          .font(.system(size: 12.5)).fixedSize(horizontal: false, vertical: true)
        Spacer()
        SecondaryButton(title: "Cancel") { model.confirmingReset = false }
        PrimaryButton(title: "Reset") { model.resetBindings() }
      } else if let problem = model.bindingsProblem {
        Label("The saved keys were not used: \(problem). The game would refuse this file: change a key or reset to the defaults.",
              systemImage: "exclamationmark.triangle.fill")
          .font(.system(size: 12)).foregroundStyle(Theme.red).fixedSize(horizontal: false, vertical: true)
        Spacer()
      } else {
        Text(model.controlsNotice.isEmpty ? "Changes are saved at once and apply when you return to the game." : model.controlsNotice)
          .font(.system(size: 12)).foregroundStyle(Theme.secondary)
        Spacer()
      }
    }
  }
}
