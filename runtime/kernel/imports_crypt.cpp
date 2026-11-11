// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XeCrypt imports. SHA-1 is computed here from FIPS 180-4 (sections 5.1.1, 5.3.1, 6.1.2); its state lives in the
// guest's XECRYPT_SHA_STATE as Xenia lays it out (xenia-canary 67f16c4e315f, kernel/xboxkrnl/xboxkrnl_crypt.cc:90-95):
// +0 byte count (big-endian 32 bits), +4 the five hash words (big-endian), +0x18 the partial 64-byte block (its first
// count % 64 bytes are meaningful). The state is read and written at every call, so the guest may copy it.
#include "imports.h"
#include <algorithm>
#include <cstdlib>  // arc4random_buf (macOS)
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kShaStateSize = 0x58, kCount = 0x0, kWords = 0x4, kBlock = 0x18;

uint32_t Rotl(uint32_t v, int n) { return v << n | v >> (32 - n); }

// FIPS 180-4 6.1.2: one 64-byte block into the five hash words.
void Compress(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int t = 0; t < 16; t++)
        w[t] = uint32_t(block[4 * t]) << 24 | uint32_t(block[4 * t + 1]) << 16 | uint32_t(block[4 * t + 2]) << 8 | block[4 * t + 3];
    for (int t = 16; t < 80; t++)
        w[t] = Rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; t++)
    {
        const uint32_t f = t < 20 ? (b & c) | (~b & d) : t < 40 ? b ^ c ^ d : t < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
        const uint32_t k = t < 20 ? 0x5A827999 : t < 40 ? 0x6ED9EBA1 : t < 60 ? (1u << 31 | 0x0F1BBCDC) : 0xCA62C1D6;  // K = 5A827999, 6ED9EBA1, 8F1BBCDC, CA62C1D6 (4.2.1)
        const uint32_t temp = Rotl(a, 5) + f + e + k + w[t];
        e = d, d = c, c = Rotl(b, 30), b = a, a = temp;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

struct Sha1
{
    uint32_t count = 0;  // bytes processed (Xenia keeps 32 bits in the guest state)
    uint32_t h[5] = {};
    uint8_t block[64] = {};

    void Load(const uint8_t* base, uint32_t state)
    {
        count = runtime::Load32(base, state + kCount);
        for (int i = 0; i < 5; i++)
            h[i] = runtime::Load32(base, state + kWords + 4 * i);
        std::memcpy(block, base + state + kBlock, 64);
    }
    void Store(uint8_t* base, uint32_t state) const
    {
        runtime::Store32(base, state + kCount, count);
        for (int i = 0; i < 5; i++)
            runtime::Store32(base, state + kWords + 4 * i, h[i]);
        std::memcpy(base + state + kBlock, block, 64);
    }
    void Update(const uint8_t* data, uint32_t size)
    {
        for (uint32_t i = 0; i < size; i++)
        {
            block[count % 64] = data[i];
            count++;
            if (count % 64 == 0)
                Compress(h, block);
        }
    }
    // FIPS 180-4 5.1.1: 0x80, zeros to 56 mod 64, the message length in bits (64 bits, big-endian; the byte count here
    // has 32 bits, as Xenia's state).
    void Finish(uint8_t digest[20])
    {
        const uint64_t bits = uint64_t(count) * 8;
        const uint8_t one = 0x80, zero = 0;
        Update(&one, 1);
        while (count % 64 != 56)
            Update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; i++)
            length[i] = uint8_t(bits >> (56 - 8 * i));
        Update(length, 8);
        for (int i = 0; i < 20; i++)
            digest[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
    }
};

}  // namespace

GUEST_IMPORT(XeCryptShaInit, "REFERENCE_DERIVED: Xenia xboxkrnl_crypt.cc:111-120 (state zeroed, FIPS 180-4 5.3.1 initial hash)")
{
    const uint32_t state = ctx.r3.u32;
    std::memset(base + state, 0, kShaStateSize);
    Sha1 sha;
    const uint32_t initial[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::copy(initial, initial + 5, sha.h);
    sha.Store(base, state);
}

GUEST_IMPORT(XeCryptShaUpdate, "REFERENCE_DERIVED: Xenia xboxkrnl_crypt.cc:122-131 (state, input*, size)")
{
    const uint32_t state = ctx.r3.u32, input = ctx.r4.u32, size = ctx.r5.u32;
    Sha1 sha;
    sha.Load(base, state);
    sha.Update(base + input, size);
    sha.Store(base, state);
}

GUEST_IMPORT(XeCryptShaFinal, "REFERENCE_DERIVED: Xenia xboxkrnl_crypt.cc:133-146 (the digest's first min(20, size) bytes; the final words kept)")
{
    // (state, out*, out size): the digest, truncated to `size`; the state's hash words become the final ones (Xenia
    // stores only those, leaving the count and block as they were).
    const uint32_t state = ctx.r3.u32, out = ctx.r4.u32, size = ctx.r5.u32;
    Sha1 sha;
    sha.Load(base, state);
    uint8_t digest[20];
    sha.Finish(digest);
    std::memcpy(base + out, digest, std::min<uint32_t>(20, size));
    for (int i = 0; i < 5; i++)
        runtime::Store32(base, state + kWords + 4 * i, sha.h[i]);
}

// SHA-256 (FIPS 180-4 4.2.2, 5.3.3, 6.2.2) in Xenia's XECRYPT_SHA256_STATE (xboxkrnl_crypt.cc:172-177; Xenia: "Size of
// this struct hasn't been confirmed yet", UNVERIFIED): +0 byte count, +4 the eight hash words, +0x24 the 64-byte block.
namespace {

constexpr uint32_t kSha256StateSize = 0x64, kSha256Words = 0x4, kSha256Block = 0x24;

uint32_t Rotr(uint32_t v, int n) { return v >> n | v << (32 - n); }

// 4.2.2: the first 32 bits of the fractional parts of the cube roots of the first 64 primes.
// (Constants in the image range are written as bit 31 | the rest, for the title-address check.)
const uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, (1u << 31 | 0x00deb1fe), 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, (1u << 31 | 0x01c2c92e), 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, (1u << 31 | 0x04c87814), (1u << 31 | 0x0cc70208), 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void Compress256(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int t = 0; t < 16; t++)
        w[t] = uint32_t(block[4 * t]) << 24 | uint32_t(block[4 * t + 1]) << 16 | uint32_t(block[4 * t + 2]) << 8 | block[4 * t + 3];
    for (int t = 16; t < 64; t++)
    {
        const uint32_t s0 = Rotr(w[t - 15], 7) ^ Rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const uint32_t s1 = Rotr(w[t - 2], 17) ^ Rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 64; t++)
    {
        const uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[t] + w[t];
        const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

struct Sha256
{
    uint32_t count = 0;
    uint32_t h[8] = {};
    uint8_t block[64] = {};

    void Load(const uint8_t* base, uint32_t state)
    {
        count = runtime::Load32(base, state);
        for (int i = 0; i < 8; i++)
            h[i] = runtime::Load32(base, state + kSha256Words + 4 * i);
        std::memcpy(block, base + state + kSha256Block, 64);
    }
    void Store(uint8_t* base, uint32_t state) const
    {
        runtime::Store32(base, state, count);
        for (int i = 0; i < 8; i++)
            runtime::Store32(base, state + kSha256Words + 4 * i, h[i]);
        std::memcpy(base + state + kSha256Block, block, 64);
    }
    void Update(const uint8_t* data, uint32_t size)
    {
        for (uint32_t i = 0; i < size; i++)
        {
            block[count % 64] = data[i];
            count++;
            if (count % 64 == 0)
                Compress256(h, block);
        }
    }
    void Finish(uint8_t digest[32])  // 5.1.1 padding, as SHA-1
    {
        const uint64_t bits = uint64_t(count) * 8;
        const uint8_t one = 0x80, zero = 0;
        Update(&one, 1);
        while (count % 64 != 56)
            Update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; i++)
            length[i] = uint8_t(bits >> (56 - 8 * i));
        Update(length, 8);
        for (int i = 0; i < 32; i++)
            digest[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
    }
};

}  // namespace

GUEST_IMPORT(XeCryptSha256Init, "REFERENCE_DERIVED: Xenia xboxkrnl_crypt.cc:179-191 (state zeroed, FIPS 180-4 5.3.3 initial hash)")
{
    const uint32_t state = ctx.r3.u32;
    std::memset(base + state, 0, kSha256StateSize);
    Sha256 sha;
    // 5.3.3: the first 32 bits of the fractional parts of the square roots of the first 8 primes.
    const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::copy(initial, initial + 8, sha.h);
    sha.Store(base, state);
}

GUEST_IMPORT(XeCryptSha256Update, "REFERENCE_DERIVED: Xenia xboxkrnl_crypt.cc:190-206 (state, input*, size)")
{
    const uint32_t state = ctx.r3.u32, input = ctx.r4.u32, size = ctx.r5.u32;
    Sha256 sha;
    sha.Load(base, state);
    sha.Update(base + input, size);
    sha.Store(base, state);
}

GUEST_IMPORT(XeCryptSha256Final, "REFERENCE_DERIVED: Xenia xboxkrnl_crypt.cc:209-225 (the digest's first min(32, size) bytes; the digest copied to the state's block)")
{
    // (state, out*, out size): the digest truncated to `size`; as Xenia, the state's block area (+0x24) receives the
    // 32-byte digest and nothing else in the state changes.
    const uint32_t state = ctx.r3.u32, out = ctx.r4.u32, size = ctx.r5.u32;
    Sha256 sha;
    sha.Load(base, state);
    uint8_t digest[32];
    sha.Finish(digest);
    std::memcpy(base + out, digest, std::min<uint32_t>(32, size));
    std::memcpy(base + state + kSha256Block, digest, 32);
}

// AES-128 (FIPS-197) for the XeKeys imports. The S-box is derived at first use from its definition (5.1.1: the
// multiplicative inverse in GF(2^8) mod x^8+x^4+x^3+x+1, then the affine map with 0x63), not typed in.
namespace {

struct AesTables
{
    uint8_t sbox[256], inv[256];
    AesTables()
    {
        for (int x = 0; x < 256; x++)
        {
            uint8_t invx = 0;
            for (int y = 1; y < 256 && x; y++)
                if (Mul(uint8_t(x), uint8_t(y)) == 1)
                {
                    invx = uint8_t(y);
                    break;
                }
            const auto rotl8 = [](uint8_t v, int n) { return uint8_t(v << n | v >> (8 - n)); };
            const uint8_t s = invx ^ rotl8(invx, 1) ^ rotl8(invx, 2) ^ rotl8(invx, 3) ^ rotl8(invx, 4) ^ 0x63;
            sbox[x] = s;
            inv[s] = uint8_t(x);
        }
    }
    static uint8_t Mul(uint8_t a, uint8_t b)  // 4.2
    {
        uint8_t p = 0;
        for (; b; b >>= 1, a = uint8_t(a << 1 ^ (a & 0x80 ? 0x1B : 0)))
            if (b & 1)
                p ^= a;
        return p;
    }
};

const AesTables& Aes() { static const AesTables t; return t; }

// 5.2: the 11 round keys (176 bytes) of a 16-byte key.
void ExpandKey(const uint8_t key[16], uint8_t w[176])
{
    const AesTables& t = Aes();
    std::memcpy(w, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4)
    {
        uint8_t temp[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if (i % 16 == 0)
        {
            const uint8_t first = temp[0];
            temp[0] = uint8_t(t.sbox[temp[1]] ^ rcon), temp[1] = t.sbox[temp[2]], temp[2] = t.sbox[temp[3]], temp[3] = t.sbox[first];
            rcon = AesTables::Mul(rcon, 2);
        }
        for (int j = 0; j < 4; j++)
            w[i + j] = w[i - 16 + j] ^ temp[j];
    }
}

// 5.1 (Cipher) and 5.3 (InvCipher) on one 16-byte block; the state is column-major (byte r + 4c), as the input.
void AesBlock(const uint8_t w[176], const uint8_t in[16], uint8_t out[16], bool encrypt)
{
    const AesTables& t = Aes();
    uint8_t s[16];
    auto addRoundKey = [&](int round) { for (int i = 0; i < 16; i++) s[i] ^= w[16 * round + i]; };
    auto shiftRows = [&](bool inverse) {
        uint8_t r[16];
        for (int row = 0; row < 4; row++)
            for (int c = 0; c < 4; c++)
                r[row + 4 * c] = s[row + 4 * ((inverse ? c - row + 4 : c + row) % 4)];
        std::memcpy(s, r, 16);
    };
    auto mixColumns = [&](bool inverse) {
        const uint8_t m[4] = {uint8_t(inverse ? 0x0e : 2), uint8_t(inverse ? 0x0b : 3), uint8_t(inverse ? 0x0d : 1), uint8_t(inverse ? 0x09 : 1)};
        for (int c = 0; c < 4; c++)
        {
            uint8_t col[4];
            for (int row = 0; row < 4; row++)
                col[row] = AesTables::Mul(m[0], s[4 * c + row]) ^ AesTables::Mul(m[1], s[4 * c + (row + 1) % 4]) ^
                           AesTables::Mul(m[2], s[4 * c + (row + 2) % 4]) ^ AesTables::Mul(m[3], s[4 * c + (row + 3) % 4]);
            std::memcpy(s + 4 * c, col, 4);
        }
    };
    std::memcpy(s, in, 16);
    if (encrypt)
    {
        addRoundKey(0);
        for (int round = 1; round <= 10; round++)
        {
            for (uint8_t& b : s)
                b = t.sbox[b];
            shiftRows(false);
            if (round < 10)
                mixColumns(false);
            addRoundKey(round);
        }
    }
    else
    {
        addRoundKey(10);
        for (int round = 9; round >= 0; round--)
        {
            shiftRows(true);
            for (uint8_t& b : s)
                b = t.inv[b];
            addRoundKey(round);
            if (round > 0)
                mixColumns(true);
        }
    }
    std::memcpy(out, s, 16);
}

// The console's key-obfuscation key (hypervisor key 0x18) is per console and unknown. A compatibility
// policy: an invented, documented console identity, Xenia's all-zero key
// (xboxkrnl_crypt.cc:632-634). Saves made with it verify only under this runtime.
constexpr uint8_t kObfuscationKey[16] = {};

std::string Hex(const uint8_t* p, size_t n)
{
    std::string out;
    char b[4];
    for (size_t i = 0; i < n; i++)
        std::snprintf(b, sizeof b, "%02x", p[i]), out += b;
    return out;
}

// The key an obscured key stands for: AES-128 ECB decryption under the obfuscation key.
void Deobscure(const uint8_t* obscured, uint8_t key[16])
{
    uint8_t w[176];
    ExpandKey(kObfuscationKey, w);
    AesBlock(w, obscured, key, false);
}

}  // namespace

GUEST_IMPORT(XeKeysObscureKey, "REFERENCE_DERIVED: Xenia xboxkrnl_crypt.cc:655-665 (AES-128 ECB encryption of 16 bytes under the obfuscation key; compatibility policy: the all-zero key)")
{
    // (input*, output*): 16 bytes; may alias.
    const uint32_t input = ctx.r3.u32, output = ctx.r4.u32;
    if (!input || !output)
        Stop(StopCode::Unimplemented, "XeKeysObscureKey(input %08X, output %08X): a null pointer", input, output);
    uint8_t w[176];
    ExpandKey(kObfuscationKey, w);
    AesBlock(w, base + input, base + output, true);
    std::fprintf(stderr, "[keys] XeKeysObscureKey(caller lr %08X): obscured key %s\n", uint32_t(ctx.lr), Hex(base + output, 16).c_str());
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XeKeysAesCbcUsingKey, "REFERENCE_DERIVED: Xenia xboxkrnl_crypt.cc:636-653 and :512-545 (the obscured key deobscured, then AES-128 CBC; the feed block updated)")
{
    // (obscured key*, input*, size, output*, feed (IV) 16 bytes*, encrypt): whole 16-byte blocks only; input and output
    // may alias; the feed ends as the last ciphertext block (Xenia's XeCryptAesCbc).
    const uint32_t obscured = ctx.r3.u32, input = ctx.r4.u32, size = ctx.r5.u32, output = ctx.r6.u32, feed = ctx.r7.u32;
    const bool encrypt = ctx.r8.u32 != 0;
    if (!obscured || !feed || (size && (!input || !output)) || size % 16)
        Stop(StopCode::Unimplemented, "XeKeysAesCbcUsingKey(key %08X, in %08X, size %u, out %08X, feed %08X): not whole blocks or a null pointer",
             obscured, input, size, output, feed);
    std::fprintf(stderr, "[keys] XeKeysAesCbcUsingKey(caller lr %08X, %u bytes, %s)\n", uint32_t(ctx.lr), size, encrypt ? "encrypt" : "decrypt");
    uint8_t key[16], w[176];
    Deobscure(base + obscured, key);
    ExpandKey(key, w);
    uint8_t* iv = base + feed;
    for (uint32_t i = 0; i < size; i += 16)
    {
        uint8_t block[16];
        std::memcpy(block, base + input + i, 16);
        if (encrypt)
        {
            for (int j = 0; j < 16; j++)
                iv[j] ^= block[j];
            AesBlock(w, iv, iv, true);
            std::memcpy(base + output + i, iv, 16);
        }
        else
        {
            AesBlock(w, block, base + output + i, false);
            for (int j = 0; j < 16; j++)
                base[output + i + j] ^= iv[j];
            std::memcpy(iv, block, 16);
        }
    }
    ctx.r3.u64 = 0;
}

// FM4 Recomp: reached by SpeechFacade_default.xex (8 bytes).
GUEST_IMPORT(XeCryptRandom, "INFERRED: random bytes from the host (arc4random_buf); Xenia 67f16c4 xboxkrnl_crypt.cc:354-357 is a stub filling 0xFD")
{
    // (buffer*, size): fills the buffer with random bytes. Xenia's 0xFD fill is a declared stub (kStub), not the console's
    // behaviour; the console returns random data, which the host's generator provides. A null buffer with a size stops.
    const uint32_t buffer = ctx.r3.u32, size = ctx.r4.u32;
    if (size == 0)
        return;
    if (buffer == 0)
        Stop(StopCode::GuestFault, "XeCryptRandom(null, %u) (lr=%08X)", size, uint32_t(ctx.lr));
    arc4random_buf(base + buffer, size);
}
