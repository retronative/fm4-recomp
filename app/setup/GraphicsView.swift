// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp's Graphics settings: the rendering resolution, the extra edge smoothing and its strength, HDR (GraphicsSettings.swift), the
// second tab of the panel the game opens on Command-comma (ControlsPanel.swift). A choice is saved at once to
// $FM4_RECOMP_HOME/graphics.conf; the running game takes it when the panel closes (FM4_GRAPHICS_LIVE=1, runtime/video/window_sdl.cpp),
// and play.sh reads the file at every start.
// Portions copyright (c) 2026, i2kiselev (BSD 3-Clause; FM4 Static Recomp host/src/ui/graphics_page.cpp: the Effects
// tab's texts, adapted from its render scenario rows' descriptions). FM4 Recomp changes: shortened, the panel's style.

import SwiftUI

// Keyboard | Graphics. Changing the tab ends a key capture (its key monitor would swallow the next key).
struct SettingsTabs: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    HStack(spacing: 6) {
      tab("Keyboard", .keyboard)
      tab("Graphics", .graphics)
      if model.renderOffered { tab("Effects", .effects) }  // FM4 Recomp (render scenario settings)
      Spacer()
    }
  }
  private func tab(_ title: String, _ which: SetupModel.SettingsTab) -> some View {
    let selected = model.settingsTab == which
    return Button { model.endCapture(); model.confirmingReset = false; model.settingsTab = which } label: {
      Text(title).font(.system(size: 12.5, weight: .semibold)).padding(.horizontal, 14).padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 7).fill(selected ? Theme.red.opacity(0.9) : Theme.card))
        .foregroundStyle(selected ? Color.white : Theme.secondary)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}

struct GraphicsPage: View {
  @ObservedObject var model: SetupModel

  // What the page says under the choices: a refused file, a failed save, a change waiting for the next start, or the rule.
  private var status: (text: String, warning: Bool) {
    if let problem = model.graphicsProblem { return ("The saved setting was not used: \(problem). Choose a resolution to replace it.", true) }
    if !model.graphicsNotice.isEmpty { return (model.graphicsNotice, true) }
    // The running game takes every choice here when the panel closes (FM4_GRAPHICS_LIVE=1: the resolution, edge smoothing and HDR).
    if model.hdrLive { return ("A choice is saved at once and applies when you close Settings.", false) }
    if let running = model.graphicsAtStart, running != model.graphics.resolution {
      return ("Saved. Restart required: this session keeps \(running.label); \(model.graphics.resolution.label) applies the next time you start the game.", false)
    }
    if let running = model.antiAliasingAtStart, running != model.graphics.antiAliasing {
      return ("Saved. Restart required: edge smoothing turns \(model.graphics.antiAliasing ? "on" : "off") the next time you start the game.", false)
    }
    if model.graphics.antiAliasing, let running = model.strengthAtStart, running != model.graphics.strength {
      return ("Saved. Restart required: this session keeps \(running.label) smoothing; \(model.graphics.strength.label) applies the next time you start the game.", false)
    }
    if !model.hdrLive, let running = model.hdrAtStart, running != model.graphics.hdr {
      return ("Saved. Restart required: HDR output is \(model.graphics.hdr ? "Auto" : "Off") the next time you start the game.", false)
    }
    return ("A choice is saved at once. Restart required: it applies the next time you start the game.", false)
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      VStack(alignment: .leading, spacing: 3) {
        Text("Graphics").font(.system(size: 22, weight: .bold))
        Text("Rendering resolution. The game's anti-aliasing is kept at every resolution; the window scales the picture to its size.")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary)
      }
      VStack(spacing: 0) {
        ForEach(InternalResolution.allCases, id: \.rawValue) { resolution in ResolutionRow(model: model, resolution: resolution) }
      }
      .background(RoundedRectangle(cornerRadius: 12).fill(Theme.card))
      .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.line))
      SmoothingRow(model: model)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .background(RoundedRectangle(cornerRadius: 12).fill(Theme.card))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.line))
      if model.extendedOffered {
        ExtendedViewRow(model: model)
          .clipShape(RoundedRectangle(cornerRadius: 12))
          .background(RoundedRectangle(cornerRadius: 12).fill(Theme.card))
          .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.line))
      }
      HdrRow(model: model)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .background(RoundedRectangle(cornerRadius: 12).fill(Theme.card))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.line))
      HStack(spacing: 10) {
        if status.warning {
          Label(status.text, systemImage: "exclamationmark.triangle.fill").font(.system(size: 12)).foregroundStyle(Theme.red)
            .fixedSize(horizontal: false, vertical: true)
        } else {
          Text(status.text).font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
        }
        Spacer()
      }
      .frame(height: 44)
      Spacer(minLength: 0)
      HStack {
        Spacer()
        PrimaryButton(title: "Done") { model.endCapture(); model.leaveControls() }
      }
    }
  }
}

struct ResolutionRow: View {
  @ObservedObject var model: SetupModel
  let resolution: InternalResolution
  private var selected: Bool { model.graphics.resolution == resolution && model.graphicsProblem == nil }

  var body: some View {
    Button { model.choose(resolution) } label: {
      HStack(spacing: 12) {
        Image(systemName: selected ? "largecircle.fill.circle" : "circle").font(.system(size: 15))
          .foregroundStyle(selected ? Theme.red : Theme.secondary)
        VStack(alignment: .leading, spacing: 2) {
          HStack(spacing: 8) {
            Text(resolution.label).font(.system(size: 14, weight: .semibold))
            Text(resolution.pixels).font(.system(size: 12)).foregroundStyle(Theme.secondary)
            if resolution == GraphicsSettings.defaults.resolution { Text("Default").font(.system(size: 10.5, weight: .semibold)).foregroundStyle(Theme.secondary) }
          }
          Text(resolution.detail).font(.system(size: 12)).foregroundStyle(Theme.secondary)
        }
        Spacer()
      }
      .padding(.horizontal, 14).padding(.vertical, 12)
      .background(selected ? Theme.red.opacity(0.14) : Color.clear)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}

// Extra edge smoothing: a checkbox row (the post-process pass after the game's own anti-aliasing; on at Standard by default) and, while it is
// on, its strength (Soft / Standard / Strong).
struct SmoothingRow: View {
  @ObservedObject var model: SetupModel
  private var on: Bool { model.graphics.antiAliasing && model.graphicsProblem == nil }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      toggle
      if on {
        HStack(spacing: 6) {
          Text("Strength").font(.system(size: 12, weight: .semibold)).foregroundStyle(Theme.secondary)
          ForEach(SmoothingStrength.allCases, id: \.rawValue) { strength in
            let selected = model.graphics.strength == strength
            Button { model.choose(strength) } label: {
              Text(strength.label).font(.system(size: 12, weight: .semibold)).padding(.horizontal, 12).padding(.vertical, 5)
                .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Theme.red.opacity(0.9) : Theme.line))
                .foregroundStyle(selected ? Color.white : Theme.secondary)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
          }
          Text(model.graphics.strength.detail).font(.system(size: 12)).foregroundStyle(Theme.secondary).lineLimit(1).padding(.leading, 4)
          Spacer(minLength: 0)
        }
        .padding(.leading, 44).padding(.trailing, 14).padding(.bottom, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.red.opacity(0.14))
      }
    }
  }

  private var toggle: some View {
    Button { model.setAntiAliasing(!on) } label: {
      HStack(spacing: 12) {
        Image(systemName: on ? "checkmark.square.fill" : "square").font(.system(size: 15)).foregroundStyle(on ? Theme.red : Theme.secondary)
        VStack(alignment: .leading, spacing: 2) {
          HStack(spacing: 8) {
            Text("Extra edge smoothing").font(.system(size: 14, weight: .semibold))
            Text(on ? "On" : "Off").font(.system(size: 12)).foregroundStyle(Theme.secondary)
            if on && model.graphics.strength == GraphicsSettings.defaults.strength { Text("Default").font(.system(size: 10.5, weight: .semibold)).foregroundStyle(Theme.secondary) }
          }
          Text("Smooths the jagged edges left after the game's own anti-aliasing (thin lines, railings). Fine detail and small text get slightly softer.")
            .font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
        }
        Spacer()
      }
      .padding(.horizontal, 14).padding(.vertical, 12)
      .background(on ? Theme.red.opacity(0.14) : Color.clear)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}

// HDR output: Off (the default) or Auto, and what is active. Auto uses HDR when this display and the system can show it; otherwise
// the picture is standard range (SDR), and the line says why.
struct HdrRow: View {
  @ObservedObject var model: SetupModel
  private var auto: Bool { model.graphics.hdr && model.graphicsProblem == nil }
  private var status: String { HdrStatus.line(preference: auto, display: model.displayHdr, live: model.hdrLive, running: model.hdrRunning) }

  var body: some View {
    HStack(spacing: 12) {
      Image(systemName: "sun.max").font(.system(size: 15)).foregroundStyle(auto ? Theme.red : Theme.secondary)
      VStack(alignment: .leading, spacing: 4) {
        HStack(spacing: 6) {
          Text("HDR output").font(.system(size: 14, weight: .semibold))
          choice("Off", false)
          choice("Auto", true)
          Text(status).font(.system(size: 12, weight: .semibold)).foregroundStyle(Theme.secondary).padding(.leading, 6)
          Spacer(minLength: 0)
        }
        Text("Brighter highlights (the sun, lights, reflections) on an HDR display; the rest of the picture is unchanged. Auto uses HDR only where the display supports it.\(model.hdrLive ? " A change applies when you close Settings." : "")")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
      }
      Spacer(minLength: 0)
    }
    .padding(.horizontal, 14).padding(.vertical, 12)
    .background(auto ? Theme.red.opacity(0.14) : Color.clear)
  }

  private func choice(_ title: String, _ value: Bool) -> some View {
    let selected = auto == value
    return Button { model.setHdr(value) } label: {
      Text(title + (value ? "" : " (default)")).font(.system(size: 12, weight: .semibold)).padding(.horizontal, 12).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Theme.red.opacity(0.9) : Theme.line))
        .foregroundStyle(selected ? Color.white : Theme.secondary)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}

// Extended View: Off (the default) or On. The race fills a window or screen of another shape than 16:9: more of the track at the
// sides when it is wider (up to 32:9), more above and below when it is taller (16:10, down to 4:3); the HUD keeps its shape. The running game takes a change when the panel closes (as HDR).
struct ExtendedViewRow: View {
  @ObservedObject var model: SetupModel
  private var on: Bool { model.graphics.extendedView && model.graphicsProblem == nil }

  var body: some View {
    HStack(spacing: 12) {
      Image(systemName: "arrow.left.and.right").font(.system(size: 15)).foregroundStyle(on ? Theme.red : Theme.secondary)
      VStack(alignment: .leading, spacing: 4) {
        HStack(spacing: 6) {
          Text("Extended View").font(.system(size: 14, weight: .semibold))
          choice("Off", false)
          choice("On", true)
          Spacer(minLength: 0)
        }
        Text("Races fill a window or screen that is not 16:9: more view at the sides when it is wider (up to 32:9), more above and below when it is taller (16:10, down to 4:3). The HUD keeps its shape; menus stay 16:9.\(model.hdrLive ? " A change applies when you close Settings." : "")")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
      }
      Spacer(minLength: 0)
    }
    .padding(.horizontal, 14).padding(.vertical, 12)
    .background(on ? Theme.red.opacity(0.14) : Color.clear)
  }

  private func choice(_ title: String, _ value: Bool) -> some View {
    let selected = on == value
    return Button { model.setExtendedView(value) } label: {
      Text(title + (value ? "" : " (default)")).font(.system(size: 12, weight: .semibold)).padding(.horizontal, 12).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Theme.red.opacity(0.9) : Theme.line))
        .foregroundStyle(selected ? Color.white : Theme.secondary)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}

// FM4 Recomp (render scenario settings; runtime/game/render_scenario.h): the game's own effects, each a row of choices, the first
// the game's own (the default). The texts follow FM4 Static Recomp 0.3.2's Graphics page (host/src/ui/graphics_page.cpp kRows).
// A choice is saved at once; the running game takes it when the panel closes, from its next frame; several show from the next race.
struct EffectsPage: View {
  @ObservedObject var model: SetupModel

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      VStack(alignment: .leading, spacing: 3) {
        Text("Effects").font(.system(size: 22, weight: .bold))
        Text("The game's own effects, beyond what the console shows in races. Console is the game as on the console.")
          .font(.system(size: 12)).foregroundStyle(Theme.secondary)
      }
      VStack(spacing: 0) {
        EffectRow(title: "Scenery shadows", detail: "Shadows of cones, barriers and other scenery in races, which the game draws only in replays and photo mode. Experimental: a shadow may flicker near the edge of the screen.",
                  choices: [("Off", false), ("On", true)], value: model.graphics.sceneryShadows) { v in model.setRender { $0.sceneryShadows = v } }
        EffectRow(title: "Reflection updates", detail: "How often car reflections are redrawn. The console freezes them in 16-car races and split screen; Every Race redraws them there too; Double redraws twice as many faces each frame.",
                  choices: ReflectionUpdates.allCases.map { ($0.label, $0) }, value: model.graphics.reflections) { v in model.setRender { $0.reflections = v } }
        EffectRow(title: "Mirror", detail: "Full draws the mirror every frame with up to 16 cars and the cones and barriers, where the console halves its rate and draws 6 cars (12- and 16-car races).",
                  choices: [("Console", false), ("Full", true)], value: model.graphics.fullMirror) { v in model.setRender { $0.fullMirror = v } }
        EffectRow(title: "Soft particles", detail: "Smoke and dust fade where they meet the ground and cars instead of cutting into them, as in replays and photo mode.",
                  choices: [("Off", false), ("On", true)], value: model.graphics.softParticles) { v in model.setRender { $0.softParticles = v } }
        EffectRow(title: "Crowds", detail: "Every Race adds the crowds the console leaves out of 1- and 16-car races. Uses more game memory.",
                  choices: [("Console", false), ("Every Race", true)], value: model.graphics.crowdsEveryRace) { v in model.setRender { $0.crowdsEveryRace = v } }
        EffectRow(title: "Bloom", detail: "Full Rate updates the glow every frame, where the console skips every second one (16-car races).",
                  choices: [("Console", false), ("Full Rate", true)], value: model.graphics.fullRateBloom) { v in model.setRender { $0.fullRateBloom = v } }
        EffectRow(title: "Level of detail", detail: "How far cars keep their detailed models. Ultra also keeps the best model for your car, at more game memory per car.",
                  choices: LevelOfDetail.allCases.map { ($0.label, $0) }, value: model.graphics.detail) { v in model.setRender { $0.detail = v } }
        if model.dofOffered {
          EffectRow(title: "Depth of field", detail: "Blurs what is out of focus in the showroom, pre-race and replay cameras and photo mode.",
                    choices: [("On", true), ("Off", false)], value: model.graphics.depthOfField) { v in model.setRender { $0.depthOfField = v } }
        }
        if model.overridesOffered {
          EffectRow(title: "Motion blur", detail: "The speed streaks of the road surface. Off keeps the road's grain still.",
                    choices: [("On", true), ("Off", false)], value: model.graphics.motionBlur) { v in model.setRender { $0.motionBlur = v } }
          EffectRow(title: "Car dirt", detail: "The dirt that builds up on cars during a race. Off keeps them clean.",
                    choices: [("On", true), ("Off", false)], value: model.graphics.carDirt) { v in model.setRender { $0.carDirt = v } }
        }
      }
      .background(RoundedRectangle(cornerRadius: 12).fill(Theme.card))
      .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.line))
      Text(model.graphicsNotice.isEmpty ? "A choice is saved at once and applies when you close Settings; several show from the next race." : model.graphicsNotice)
        .font(.system(size: 12)).foregroundStyle(model.graphicsNotice.isEmpty ? Theme.secondary : Theme.red).fixedSize(horizontal: false, vertical: true)
      Spacer(minLength: 0)
      HStack {
        Spacer()
        PrimaryButton(title: "Done") { model.endCapture(); model.leaveControls() }
      }
    }
  }
}

// One effect: its name, its choices (the first marked default) and what it does.
struct EffectRow<Value: Equatable>: View {
  let title: String
  let detail: String
  let choices: [(String, Value)]
  let value: Value
  let choose: (Value) -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack(spacing: 6) {
        Text(title).font(.system(size: 14, weight: .semibold)).frame(width: 150, alignment: .leading)
        ForEach(Array(choices.enumerated()), id: \.offset) { index, choice in
          let selected = choice.1 == value
          Button { choose(choice.1) } label: {
            Text(choice.0 + (index == 0 ? " (default)" : "")).font(.system(size: 12, weight: .semibold)).padding(.horizontal, 10).padding(.vertical, 4)
              .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Theme.red.opacity(0.9) : Theme.line))
              .foregroundStyle(selected ? Color.white : Theme.secondary)
              .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
        }
        Spacer(minLength: 0)
      }
      Text(detail).font(.system(size: 11.5)).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
    }
    .padding(.horizontal, 14).padding(.vertical, 9)
  }
}
