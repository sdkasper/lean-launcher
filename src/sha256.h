#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace takeoff {

// Small streaming SHA-256 (FIPS 180-4). Used to pin the bytes of a downloaded
// update so the elevated update helper installs exactly what was downloaded.
class Sha256 {
public:
    Sha256() { Reset(); }

    void Update(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        totalBytes_ += size;
        while (size > 0) {
            const size_t take = (std::min)(size, sizeof(block_) - blockUsed_);
            std::memcpy(block_ + blockUsed_, bytes, take);
            blockUsed_ += take;
            bytes += take;
            size -= take;
            if (blockUsed_ == sizeof(block_)) {
                Compress(block_);
                blockUsed_ = 0;
            }
        }
    }

    // Lowercase hex digest. The object is reset afterwards.
    std::string FinalHex() {
        const uint64_t bitLength = totalBytes_ * 8;
        const uint8_t pad = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while (blockUsed_ != 56) Update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bitLength >> (56 - 8 * i));
        Update(length, 8);

        static constexpr char kDigits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(64);
        for (uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) hex.push_back(kDigits[(word >> shift) & 0xF]);
        }
        Reset();
        return hex;
    }

private:
    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Reset() {
        static constexpr uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(state_, kInit, sizeof(state_));
        blockUsed_ = 0;
        totalBytes_ = 0;
    }

    void Compress(const uint8_t* chunk) {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(chunk[i * 4]) << 24) | (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) | static_cast<uint32_t>(chunk[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    uint32_t state_[8]{};
    uint8_t block_[64]{};
    size_t blockUsed_ = 0;
    uint64_t totalBytes_ = 0;
};

inline std::string Sha256Hex(const void* data, size_t size) {
    Sha256 hasher;
    hasher.Update(data, size);
    return hasher.FinalHex();
}

inline bool IsSha256Hex(std::string_view text) {
    if (text.size() != 64) return false;
    for (char ch : text) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        if (!hex) return false;
    }
    return true;
}

// Both sides must be well-formed 64-character hex; the compare ignores case.
inline bool Sha256HexMatches(std::string_view expected, std::string_view actual) {
    if (!IsSha256Hex(expected) || !IsSha256Hex(actual)) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < 64; ++i) {
        const auto lower = [](char ch) { return static_cast<unsigned char>(ch >= 'A' && ch <= 'F' ? ch + 32 : ch); };
        diff |= static_cast<unsigned char>(lower(expected[i]) ^ lower(actual[i]));
    }
    return diff == 0;
}

} // namespace takeoff
