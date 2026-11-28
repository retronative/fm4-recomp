// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (keycap icons, keycap.h): a key label's ink by Core Text, in Helvetica Neue Medium (the game's help bar uses Swiss 721
// Medium, a Helvetica design), rendered into an 8-bit alpha bitmap and trimmed to its inked pixels.
#include "keycap.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <algorithm>
#include <cmath>

namespace runtime::video::keycap {

Ink CoreTextInk(std::string_view text, float pixelsPerEm)
{
    if (text.empty() || pixelsPerEm <= 0.0f)
        return {};
    CFStringRef string = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(text.data()), CFIndex(text.size()), kCFStringEncodingUTF8, false);
    CTFontRef font = CTFontCreateWithName(CFSTR("HelveticaNeue-Medium"), pixelsPerEm, nullptr);
    if (string == nullptr || font == nullptr)
    {
        if (string)
            CFRelease(string);
        if (font)
            CFRelease(font);
        return {};
    }
    const void* keys[] = {kCTFontAttributeName};
    const void* values[] = {font};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attributed = CFAttributedStringCreate(nullptr, string, attributes);
    CTLineRef line = CTLineCreateWithAttributedString(attributed);
    // A canvas with room around the line's typographic box; the ink is trimmed afterwards.
    const double width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    const int pad = int(std::ceil(pixelsPerEm * 0.5f));
    const int w = std::max(1, int(std::ceil(width)) + 2 * pad), h = std::max(1, int(std::ceil(pixelsPerEm * 1.5f)) + 2 * pad);
    std::vector<uint8_t> pixels(size_t(w) * h, 0);
    CGContextRef context = CGBitmapContextCreate(pixels.data(), size_t(w), size_t(h), 8, size_t(w), nullptr, kCGImageAlphaOnly);
    Ink ink;
    if (context != nullptr)
    {
        CGContextSetShouldAntialias(context, true);
        CGContextSetTextPosition(context, pad, pad + pixelsPerEm * 0.4f);  // the baseline, y up from the bottom row
        CTLineDraw(line, context);
        CGContextRelease(context);
        // Rows are top down in memory (Core Graphics' origin is the bottom-left of the image it draws, the first row the top).
        int left = w, right = -1, top = h, bottom = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (pixels[size_t(y) * w + x] != 0)
                    left = std::min(left, x), right = std::max(right, x), top = std::min(top, y), bottom = std::max(bottom, y);
        if (right >= left)
        {
            ink.width = right - left + 1, ink.height = bottom - top + 1;
            ink.alpha.resize(size_t(ink.width) * ink.height);
            for (int y = 0; y < ink.height; y++)
                for (int x = 0; x < ink.width; x++)
                    ink.at(x, y) = pixels[size_t(top + y) * w + left + x] / 255.0f;
        }
    }
    CFRelease(line);
    CFRelease(attributed);
    CFRelease(attributes);
    CFRelease(font);
    CFRelease(string);
    return ink;
}

}  // namespace runtime::video::keycap
