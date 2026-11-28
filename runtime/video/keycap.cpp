// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Adapted from fm4-engine 0.3.2 host/src/input/keycap.cpp and prompt_swap.cpp (Copyright (c) 2026, i2kiselev; BSD 3-Clause: the
// full notice is in keycap.h). FM4 Recomp changes: keycap.h.
#include "keycap.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace runtime::video::keycap {
namespace {

constexpr float kFaceTop = 0xE6, kFaceBottom = 0xBD, kEdge = 0x8D, kText = 0x1A;

bool IsArrow(std::string_view label)
{
    return label == kArrowUp || label == kArrowDown || label == kArrowLeft || label == kArrowRight;
}

Ink Blank(int width, int height)
{
    return {width, height, std::vector<float>(size_t(width) * height, 0.0f)};
}

// A filled triangle in a side x side box, pointing along the arrow.
Ink Arrow(std::string_view arrow, int side)
{
    Ink ink = Blank(side, side);
    constexpr int kSamples = 4;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
        {
            int inside = 0;
            for (int sy = 0; sy < kSamples; ++sy)
                for (int sx = 0; sx < kSamples; ++sx)
                {
                    // u along the arrow (0 base, 1 tip), v across it (-1..1).
                    const float px = (x + (sx + 0.5f) / kSamples) / side;
                    const float py = (y + (sy + 0.5f) / kSamples) / side;
                    float u = 0.0f, v = 0.0f;
                    if (arrow == kArrowUp)
                        u = 1.0f - py, v = px * 2.0f - 1.0f;
                    if (arrow == kArrowDown)
                        u = py, v = px * 2.0f - 1.0f;
                    if (arrow == kArrowLeft)
                        u = 1.0f - px, v = py * 2.0f - 1.0f;
                    if (arrow == kArrowRight)
                        u = px, v = py * 2.0f - 1.0f;
                    inside += std::abs(v) <= 1.0f - u;
                }
            ink.at(x, y) = float(inside) / (kSamples * kSamples);
        }
    return ink;
}

void Blit(Ink& target, const Ink& source, int x0, int y0)
{
    for (int y = 0; y < source.height; ++y)
        for (int x = 0; x < source.width; ++x)
        {
            const int tx = x0 + x, ty = y0 + y;
            if (tx >= 0 && ty >= 0 && tx < target.width && ty < target.height)
                target.at(tx, ty) = std::max(target.at(tx, ty), source.at(x, y));
        }
}

// The arrows of the D-pad: one, a row, or up above left, down and right.
Ink Arrows(std::span<const std::string> labels, float width, float height)
{
    const auto has = [&](std::string_view arrow) { return std::find(labels.begin(), labels.end(), arrow) != labels.end(); };
    const bool upperRow = has(kArrowUp) && labels.size() > 1;
    const int columns = int(labels.size()) - (upperRow ? 1 : 0);
    const int rows = upperRow ? 2 : 1;
    const int side = int(std::min(width / (columns + 0.25f * (columns - 1)), height / (rows + 0.25f * (rows - 1))));
    const int gap = side / 4;
    Ink ink = Blank(columns * side + (columns - 1) * gap, rows * side + (rows - 1) * gap);
    int column = 0;
    for (std::string_view arrow : {kArrowLeft, kArrowUp, kArrowDown, kArrowRight})
    {
        if (!has(arrow))
            continue;
        if (upperRow && arrow == kArrowUp)
        {
            Blit(ink, Arrow(arrow, side), (ink.width - side) / 2, 0);
            continue;
        }
        Blit(ink, Arrow(arrow, side), column++ * (side + gap), (rows - 1) * (side + gap));
    }
    return ink;
}

// The backspace arrow in a box letterHeight tall: a triangle head pointing left and a shaft to its right.
Ink BackspaceArrow(float width, float letterHeight)
{
    const int h = std::max(1, int(letterHeight)), w = std::max(1, int(std::min(width, letterHeight * 1.8f)));
    Ink ink = Blank(w, h);
    constexpr int kSamples = 4;
    const float head = h * 0.9f, shaft = h * 0.17f;  // the head's length, the shaft's half thickness
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            int inside = 0;
            for (int sy = 0; sy < kSamples; ++sy)
                for (int sx = 0; sx < kSamples; ++sx)
                {
                    const float px = x + (sx + 0.5f) / kSamples, dy = std::abs(y + (sy + 0.5f) / kSamples - h / 2.0f);
                    inside += (px <= head && dy <= px / head * h / 2.0f) || (px > head * 0.9f && dy <= shaft);
                }
            ink.at(x, y) = float(inside) / (kSamples * kSamples);
        }
    return ink;
}

// Coverage of a rounded rectangle at pixel (x, y).
float RoundedRect(int x, int y, float width, float height, float radius)
{
    constexpr int kSamples = 4;
    int inside = 0;
    for (int sy = 0; sy < kSamples; ++sy)
        for (int sx = 0; sx < kSamples; ++sx)
        {
            const float px = x + (sx + 0.5f) / kSamples, py = y + (sy + 0.5f) / kSamples;
            if (px > width || py > height)
                continue;
            const float dx = std::max({radius - px, px - (width - radius), 0.0f});
            const float dy = std::max({radius - py, py - (height - radius), 0.0f});
            inside += dx * dx + dy * dy <= radius * radius;
        }
    return float(inside) / (kSamples * kSamples);
}

std::string Joined(std::span<const std::string> labels)
{
    const bool letters = std::all_of(labels.begin(), labels.end(), [](const std::string& label) { return label.size() == 1; });
    std::string text;
    for (const std::string& label : labels)
    {
        if (!text.empty() && !(letters && labels.size() > 2))
            text += ' ';
        text += label;
    }
    return text;
}

// The labels' ink fitted to width x height, letters no taller than letterHeight.
Ink Fit(std::span<const std::string> labels, const TextInk& text, float width, float height, float letterHeight)
{
    if (std::all_of(labels.begin(), labels.end(), [](const std::string& label) { return IsArrow(label); }))
        return labels.size() == 1 ? Arrow(labels[0], int(std::min(width, letterHeight))) : Arrows(labels, width, height);
    if (labels.size() == 1 && labels[0] == kBackspace)
        return BackspaceArrow(width, letterHeight);
    const std::string joined = Joined(labels);
    constexpr float kProbe = 100.0f;
    const Ink probe = text(joined, kProbe);
    if (!probe.width)
        return {};
    // A capital's height, so a lone "W" and "ESC" stand as tall.
    const Ink capital = text("H", kProbe);
    float letterScale = letterHeight / std::max(capital.height, 1);
    // Punctuation at the letters' size is a speck: it grows, up to a capital's height.
    if (std::all_of(joined.begin(), joined.end(), [](char c) { return c == ' ' || std::ispunct(uint8_t(c)); }))
        letterScale = std::min(letterScale * 2.5f, letterHeight / std::max(probe.height, 1));
    const float scale = std::min({width / probe.width, height / probe.height, letterScale}) * 0.97f;
    return text(joined, kProbe * scale);
}

}  // namespace

int FindIcon(uint64_t baseLevelHash)
{
    for (size_t i = 0; i < kIcons.size(); i++)
        if (kIcons[i].hash == baseLevelHash)
            return int(i);
    return -1;
}

std::string CapLabel(std::string_view keyLabel)
{
    static constexpr std::pair<std::string_view, std::string_view> kLegends[] = {
        {"UP ARROW", kArrowUp},        {"DOWN ARROW", kArrowDown},  {"LEFT ARROW", kArrowLeft},   {"RIGHT ARROW", kArrowRight},
        {"RETURN", "ENTER"},           {"KEYPAD ENTER", "ENTER"},   {"FORWARD DELETE", "DEL"},    {"BACKSPACE", kBackspace},
        {"LEFT CONTROL", "CTRL"},      {"RIGHT CONTROL", "CTRL"},   {"LEFT SHIFT", "SHIFT"},      {"RIGHT SHIFT", "SHIFT"},
        {"LEFT OPTION", "OPTION"},     {"RIGHT OPTION", "OPTION"},  {"PAGEUP", "PG UP"},          {"PAGEDOWN", "PG DN"},
    };
    for (const auto& [label, legend] : kLegends)
        if (keyLabel == label)
            return std::string(legend);
    if (keyLabel.starts_with("KEYPAD "))
        return "NUM " + std::string(keyLabel.substr(7));
    return std::string(keyLabel);
}

std::vector<std::string> CapLabels(std::span<const std::string> keyLabels)
{
    std::vector<std::string> labels;
    for (const std::string& key : keyLabels)
    {
        if (key.empty())
            return {};
        std::string label = CapLabel(key);
        if (labels.empty() || labels.back() != label)
            labels.push_back(std::move(label));
    }
    return labels;
}

Rgba RenderKeycap(std::span<const std::string> labels, uint32_t size, const TextInk& text, uint32_t capWidth)
{
    const float cap = float(capWidth ? std::min(capWidth, size) : size);
    const float edge = float(size * 3 / 32);
    const float face = float(size) - edge;
    const float radius = size * 0.2f;
    const float margin = float(size / 8);
    const float width = cap - 2 * margin, height = face - 2 * margin;
    Ink ink = Fit(labels, text, width, height, face * 0.42f);
    // WASD's capitals come to about a sixth of the cap; smaller does not read.
    if (labels.size() > 1 && ink.height < float(size) / 6)
        ink = Fit(labels.first(1), text, width, height, face * 0.42f);
    const int inkX = int(std::lround((cap - ink.width) / 2.0f));
    const int inkY = int(std::lround((face - ink.height) / 2.0f));

    Rgba out{size, size, std::vector<uint8_t>(size_t(size) * size * 4)};
    for (uint32_t y = 0; y < size; ++y)
    {
        const float t = std::clamp((y + 0.5f) / face, 0.0f, 1.0f);
        const float faceColor = kFaceTop + (kFaceBottom - kFaceTop) * t;
        for (uint32_t x = 0; x < size; ++x)
        {
            const float edgeCover = RoundedRect(int(x), int(y), cap, float(size), radius);
            const float faceCover = RoundedRect(int(x), int(y), cap, face, radius);
            float alpha = faceCover + edgeCover * (1.0f - faceCover);
            float color = faceColor * faceCover + kEdge * edgeCover * (1.0f - faceCover);
            const int ix = int(x) - inkX, iy = int(y) - inkY;
            if (ix >= 0 && iy >= 0 && ix < ink.width && iy < ink.height)
            {
                const float t2 = ink.at(ix, iy) * faceCover;
                alpha = t2 + alpha * (1.0f - t2);
                color = kText * t2 + color * (1.0f - t2);
            }
            const uint8_t value = alpha > 0.0f ? uint8_t(std::lround(color / alpha)) : 0;
            uint8_t* const pixel = &out.pixels[(size_t(y) * size + x) * 4];
            pixel[0] = pixel[1] = pixel[2] = value;
            pixel[3] = uint8_t(std::lround(alpha * 255.0f));
        }
    }
    return out;
}

std::vector<Rgba> Levels(Rgba top)
{
    std::vector<Rgba> levels{std::move(top)};
    while (levels.back().width > 1 || levels.back().height > 1)
    {
        const Rgba& above = levels.back();
        Rgba level{std::max(above.width / 2, 1u), std::max(above.height / 2, 1u), {}};
        level.pixels.resize(size_t(level.width) * level.height * 4);
        for (uint32_t y = 0; y < level.height; ++y)
            for (uint32_t x = 0; x < level.width; ++x)
            {
                float color[3] = {}, alpha = 0.0f;
                int taps = 0;
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const uint32_t sx = std::min(x * 2 + dx, above.width - 1), sy = std::min(y * 2 + dy, above.height - 1);
                        const uint8_t* p = &above.pixels[(size_t(sy) * above.width + sx) * 4];
                        for (size_t c = 0; c < 3; ++c)
                            color[c] += p[c] * (p[3] / 255.0f);
                        alpha += p[3] / 255.0f;
                        taps++;
                    }
                uint8_t* const out = &level.pixels[(size_t(y) * level.width + x) * 4];
                for (size_t c = 0; c < 3; ++c)
                    out[c] = alpha > 0.0f ? uint8_t(std::lround(color[c] / alpha)) : 0;
                out[3] = uint8_t(std::lround(alpha / float(taps) * 255.0f));
            }
        levels.push_back(std::move(level));
    }
    return levels;
}

std::vector<Rgba> IconLevels(size_t icon, std::span<const std::string> labels, uint32_t scale, const TextInk& text)
{
    const uint32_t size = kIconWidth * scale;
    return Levels(RenderKeycap(labels, size, text, kIcons[icon].back ? size * 27 / 32 : 0));
}

}  // namespace runtime::video::keycap
