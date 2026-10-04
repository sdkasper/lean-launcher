#pragma once

#include <cstddef>
#include <cstdint>

#include "sha512.h"

// Verify-only Ed25519 (RFC 8032), used to check the minisign signature on an
// update (US-040). The curve arithmetic follows TweetNaCl (public domain, by
// Bernstein, Janssen, Lange, Schwabe et al.): small and simple, not fast, which
// is fine for checking one 64-byte signature per update. It never handles a
// secret, so constant-time behaviour is not a requirement here. Checked
// against the RFC 8032 test vectors in tests/core_tests.cpp.
namespace takeoff {
namespace ed25519_detail {

using Gf = int64_t[16];

inline constexpr Gf kGf0 = {0};
inline constexpr Gf kGf1 = {1};
inline constexpr Gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                          0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
inline constexpr Gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                           0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
inline constexpr Gf kX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                          0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
inline constexpr Gf kY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                          0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
inline constexpr Gf kI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                          0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
// The group order L, little endian.
inline constexpr int64_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                   0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                                   0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

inline void Set(Gf r, const Gf a) {
    for (int i = 0; i < 16; ++i) r[i] = a[i];
}

inline void Carry(Gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        const int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

inline void Select(Gf p, Gf q, int b) {
    const int64_t c = ~(static_cast<int64_t>(b) - 1);
    for (int i = 0; i < 16; ++i) {
        const int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

inline void Pack(uint8_t* o, const Gf n) {
    Gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    Carry(t);
    Carry(t);
    Carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        Select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<uint8_t>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
    }
}

inline void Unpack(Gf o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (static_cast<int64_t>(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

inline void Add(Gf o, const Gf a, const Gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

inline void Sub(Gf o, const Gf a, const Gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

inline void Mul(Gf o, const Gf a, const Gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    Carry(o);
    Carry(o);
}

inline void Square(Gf o, const Gf a) { Mul(o, a, a); }

inline void Invert(Gf o, const Gf in) {
    Gf c;
    for (int a = 0; a < 16; ++a) c[a] = in[a];
    for (int a = 253; a >= 0; --a) {
        Square(c, c);
        if (a != 2 && a != 4) Mul(c, c, in);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

inline void Pow2523(Gf o, const Gf in) {
    Gf c;
    for (int a = 0; a < 16; ++a) c[a] = in[a];
    for (int a = 250; a >= 0; --a) {
        Square(c, c);
        if (a != 1) Mul(c, c, in);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

inline bool Differs(const Gf a, const Gf b) {
    uint8_t c[32], d[32];
    Pack(c, a);
    Pack(d, b);
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= static_cast<uint8_t>(c[i] ^ d[i]);
    return diff != 0;
}

inline int Parity(const Gf a) {
    uint8_t d[32];
    Pack(d, a);
    return d[0] & 1;
}

// Extended-coordinate point addition: p = p + q.
inline void PointAdd(Gf p[4], Gf q[4]) {
    Gf a, b, c, d, t, e, f, g, h;
    Sub(a, p[1], p[0]);
    Sub(t, q[1], q[0]);
    Mul(a, a, t);
    Add(b, p[0], p[1]);
    Add(t, q[0], q[1]);
    Mul(b, b, t);
    Mul(c, p[3], q[3]);
    Mul(c, c, kD2);
    Mul(d, p[2], q[2]);
    Add(d, d, d);
    Sub(e, b, a);
    Sub(f, d, c);
    Add(g, d, c);
    Add(h, b, a);
    Mul(p[0], e, f);
    Mul(p[1], h, g);
    Mul(p[2], g, f);
    Mul(p[3], e, h);
}

inline void Swap(Gf p[4], Gf q[4], uint8_t b) {
    for (int i = 0; i < 4; ++i) Select(p[i], q[i], b);
}

inline void PackPoint(uint8_t* r, Gf p[4]) {
    Gf tx, ty, zi;
    Invert(zi, p[2]);
    Mul(tx, p[0], zi);
    Mul(ty, p[1], zi);
    Pack(r, ty);
    r[31] ^= static_cast<uint8_t>(Parity(tx) << 7);
}

inline void ScalarMult(Gf p[4], Gf q[4], const uint8_t* s) {
    Set(p[0], kGf0);
    Set(p[1], kGf1);
    Set(p[2], kGf1);
    Set(p[3], kGf0);
    for (int i = 255; i >= 0; --i) {
        const uint8_t b = static_cast<uint8_t>((s[i / 8] >> (i & 7)) & 1);
        Swap(p, q, b);
        PointAdd(q, p);
        PointAdd(p, p);
        Swap(p, q, b);
    }
}

inline void ScalarBase(Gf p[4], const uint8_t* s) {
    Gf q[4];
    Set(q[0], kX);
    Set(q[1], kY);
    Set(q[2], kGf1);
    Mul(q[3], kX, kY);
    ScalarMult(p, q, s);
}

// Decodes a public key point and negates it. False if it is not on the curve.
inline bool UnpackNegated(Gf r[4], const uint8_t* p) {
    Gf t, chk, num, den, den2, den4, den6;
    Set(r[2], kGf1);
    Unpack(r[1], p);
    Square(num, r[1]);
    Mul(den, num, kD);
    Sub(num, num, r[2]);
    Add(den, r[2], den);
    Square(den2, den);
    Square(den4, den2);
    Mul(den6, den4, den2);
    Mul(t, den6, num);
    Mul(t, t, den);
    Pow2523(t, t);
    Mul(t, t, num);
    Mul(t, t, den);
    Mul(t, t, den);
    Mul(r[0], t, den);
    Square(chk, r[0]);
    Mul(chk, chk, den);
    if (Differs(chk, num)) Mul(r[0], r[0], kI);
    Square(chk, r[0]);
    Mul(chk, chk, den);
    if (Differs(chk, num)) return false;
    if (Parity(r[0]) == (p[31] >> 7)) Sub(r[0], kGf0, r[0]);
    Mul(r[3], r[0], r[1]);
    return true;
}

// Reduces a 64-byte little-endian number modulo L, in place (result in r[0..32)).
inline void ReduceModL(uint8_t* r) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = static_cast<int64_t>(r[i]);
    for (int i = 0; i < 64; ++i) r[i] = 0;
    int64_t carry = 0;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        int j = i - 32;
        for (; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * kL[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * kL[j];
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<uint8_t>(x[i] & 255);
    }
}

// True when the 32-byte little-endian scalar is smaller than L.
inline bool ScalarBelowL(const uint8_t* s) {
    for (int i = 31; i >= 0; --i) {
        if (s[i] < kL[i]) return true;
        if (s[i] > kL[i]) return false;
    }
    return false;  // equal to L
}

} // namespace ed25519_detail

// Ed25519 signature check: `signature` is R || S (64 bytes), `publicKey` is 32
// bytes. A non-canonical S (not below the group order) is rejected.
inline bool Ed25519Verify(const uint8_t signature[64], const uint8_t* message, size_t messageSize,
                          const uint8_t publicKey[32]) {
    using namespace ed25519_detail;
    if (!ScalarBelowL(signature + 32)) return false;

    Gf p[4], q[4];
    if (!UnpackNegated(q, publicKey)) return false;

    uint8_t h[64];
    Sha512 hasher;
    hasher.Update(signature, 32);
    hasher.Update(publicKey, 32);
    hasher.Update(message, messageSize);
    hasher.Final(h);
    ReduceModL(h);

    ScalarMult(p, q, h);
    ScalarBase(q, signature + 32);
    PointAdd(p, q);
    uint8_t t[32];
    PackPoint(t, p);

    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= static_cast<uint8_t>(signature[i] ^ t[i]);
    return diff == 0;
}

} // namespace takeoff
