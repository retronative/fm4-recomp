// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Keycap icons, CPU only (runtime/video/keycap.h): the icon table, the cap labels from key labels (arrows, legends, an unbound
// action keeps the pad icon), the mip chain's alpha-weighted average (hand values), and the keycap's shape (clear corners, the
// face's grey, dark text inside the face, the BACK cap's clear right part). KEYCAP_PNG_DIR=<dir> also writes each icon's cap with
// the default keys as a PNG for review.
#include "keycap.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace runtime::video::keycap;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}

static void WritePng(const Rgba& image, const std::string& path)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    // Premultiplied for Core Graphics.
    std::vector<uint8_t> pre(image.pixels);
    for (size_t i = 0; i < pre.size(); i += 4)
        for (size_t c = 0; c < 3; c++)
            pre[i + c] = uint8_t(pre[i + c] * pre[i + 3] / 255);
    CGContextRef context = CGBitmapContextCreate(pre.data(), image.width, image.height, 8, image.width * 4, space, kCGImageAlphaPremultipliedLast);
    CGImageRef cg = CGBitmapContextCreateImage(context);
    CFStringRef s = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, s, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(dest, cg, nullptr);
    CGImageDestinationFinalize(dest);
    CFRelease(dest), CFRelease(url), CFRelease(s), CGImageRelease(cg), CGContextRelease(context), CGColorSpaceRelease(space);
}

static const uint8_t* Px(const Rgba& r, uint32_t x, uint32_t y) { return &r.pixels[(size_t(y) * r.width + x) * 4]; }

int main()
{
    // The table: every hash distinct, found back; an unknown hash is no icon.
    for (size_t i = 0; i < kIcons.size(); i++)
        for (size_t j = i + 1; j < kIcons.size(); j++)
            if (kIcons[i].hash == kIcons[j].hash)
                Expect(false, "two icons share a hash");
    Expect(FindIcon(0x190cf394c4dd14d9ull) == 0 && FindIcon(0x01f30dddf859a467ull) == 10 && FindIcon(0) == -1 && FindIcon(0x190cf394c4dd14d8ull) == -1,
           "FindIcon: A_Button_F3 0, RBumper_F3 10, unknown -1 (one bit off too)");

    // Labels.
    Expect(CapLabel("LEFT ARROW") == std::string(kArrowLeft) && CapLabel("UP ARROW") == std::string(kArrowUp), "arrows become triangles");
    Expect(CapLabel("BACKSPACE") == std::string(kBackspace), "BACKSPACE becomes the backspace arrow, no text");
    Expect(CapLabel("RETURN") == "ENTER" && CapLabel("KEYPAD 7") == "NUM 7" && CapLabel("FORWARD DELETE") == "DEL" && CapLabel("SPACE") == "SPACE" &&
               CapLabel("W") == "W",
           "legends: RETURN -> ENTER, KEYPAD 7 -> NUM 7, FORWARD DELETE -> DEL; others as they are");
    const std::vector<std::string> dpad = {"UP ARROW", "LEFT ARROW", "DOWN ARROW", "RIGHT ARROW"};
    Expect(CapLabels(dpad).size() == 4, "the D-pad: four arrows");
    Expect(CapLabels(std::vector<std::string>{"UP ARROW", "", "DOWN ARROW", "RIGHT ARROW"}).empty(), "one unbound action: no cap (the pad icon stays)");
    Expect(CapLabels(std::vector<std::string>{"A", "A"}).size() == 1, "the same key twice: shown once");

    // Levels: a 2x2 of (255 white, alpha 255), (0 black, alpha 255), (anything, alpha 0) x 2 -> colour 128 (alpha-weighted: the
    // transparent texels add nothing), alpha 255 * 2/4 = 128 (rounded 127.5 -> 128).
    Rgba two{2, 2, {255, 255, 255, 255, 0, 0, 0, 255, 77, 77, 77, 0, 200, 200, 200, 0}};
    const auto chain = Levels(two);
    Expect(chain.size() == 2 && chain[1].width == 1 && chain[1].height == 1, "2x2 -> two levels");
    Expect(chain.size() == 2 && Px(chain[1], 0, 0)[0] == 128 && Px(chain[1], 0, 0)[3] == 128, "the average: colour 128, alpha 128 (hand)");
    Expect(Levels(Rgba{64, 64, std::vector<uint8_t>(64 * 64 * 4)}).size() == 7, "64x64: seven levels to 1x1");

    // Shape.
    const auto labels = CapLabels(std::vector<std::string>{"SPACE"});
    const Rgba cap = RenderKeycap(labels, 64, CoreTextInk);
    Expect(cap.width == 64 && Px(cap, 0, 0)[3] == 0 && Px(cap, 63, 0)[3] == 0, "corners clear (rounded)");
    Expect(Px(cap, 32, 2)[3] == 255 && Px(cap, 32, 2)[0] > 0xD0, "the face's top: opaque light grey");
    Expect(Px(cap, 32, 63)[3] > 200 && Px(cap, 32, 63)[0] < 0xA0, "the bottom edge: darker");
    int dark = 0;
    for (uint32_t y = 8; y < 52; y++)
        for (uint32_t x = 8; x < 56; x++)
            dark += Px(cap, x, y)[0] < 0x60;
    Expect(dark > 20, "text: dark pixels inside the face");
    Expect(CoreTextInk("H", 100).height > 60 && CoreTextInk("H", 100).height < 80, "Core Text: a capital H at 100 px/em is ~70 px tall");
    // The backspace cap: an arrow, wider than tall, pointing left (more ink in the head's column than at the shaft's tip).
    const Rgba bs = RenderKeycap(CapLabels(std::vector<std::string>{"BACKSPACE"}), 64, CoreTextInk);
    int headInk = 0, tailInk = 0;
    for (uint32_t y = 8; y < 52; y++)
        headInk += Px(bs, 20, y)[0] < 0x60, tailInk += Px(bs, 46, y)[0] < 0x60;
    Expect(Px(bs, 32, 26)[0] < 0x60 && headInk > tailInk && tailInk > 0, "backspace: a shaft through the middle, the head on the left");
    const Rgba back = RenderKeycap(CapLabels(std::vector<std::string>{"TAB"}), 64, CoreTextInk, 54);
    Expect(Px(back, 60, 30)[3] == 0 && Px(back, 30, 30)[3] == 255, "BACK: clear beyond its 54 of 64");

    if (const char* dir = std::getenv("KEYCAP_PNG_DIR"))
    {
        // The default keys (key_bindings.cpp kActions) as controls_menu.h KeyLabel words them.
        const std::vector<std::vector<std::string>> keys = {{"RETURN"}, {"BACKSPACE"}, {"X"}, {"Y"}, {"TAB"}, {"SPACE"}, dpad,
                                                            {"LEFT ARROW", "RIGHT ARROW"}, {"S"}, {"W"}, {"E"}};
        for (size_t i = 0; i < kIcons.size(); i++)
            WritePng(IconLevels(i, CapLabels(keys[i]), 2, CoreTextInk)[0], std::string(dir) + "/" + kIcons[i].name + ".png");
    }
    std::printf(failures ? "%d keycap check(s) FAIL\n" : "all keycap checks pass\n", failures);
    return failures ? 1 : 0;
}
