#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "ed25519.h"
#include "sha256.h"

// US-040: checks that an update is the one the release pipeline signed. The
// release publishes LeanLauncher.exe.sha256, one line "<sha256>  LeanLauncher.exe
// <tag>", and LeanLauncher.exe.minisig, its minisign signature in the legacy
// "Ed" format (Ed25519 over the text itself). Everything here is pure (no
// files, no network) so it can be unit tested; updates.h supplies the public
// key and does the I/O.
namespace takeoff {

struct MinisignPublicKey {
    uint8_t keyId[8]{};
    uint8_t key[32]{};
};

enum class UpdateVerifyResult {
    Ok,
    Malformed,     // a file is missing, truncated, oversized, or not in the expected format
    WrongKey,      // signed by a different key than the one built into the launcher
    BadSignature,  // the signature does not match the signed text
    WrongTag,      // validly signed, but for another release (a replayed older release)
    HashMismatch,  // the downloaded exe is not the signed one
};

namespace minisign_detail {

inline constexpr size_t kMaxSignedTextBytes = 512;
inline constexpr size_t kMaxSignatureFileBytes = 1024;

inline int Base64Value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

// Standard alphabet, padding required, no whitespace.
inline bool Base64Decode(std::string_view in, std::vector<uint8_t>& out) {
    out.clear();
    if (in.empty() || in.size() % 4 != 0) return false;
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char ch = in[i + k];
            if (ch == '=') {
                // Padding is only valid in the last two positions of the last group.
                if (i + 4 != in.size() || k < 2) return false;
                v[k] = 0;
                ++pad;
            } else {
                if (pad > 0) return false;
                v[k] = Base64Value(ch);
                if (v[k] < 0) return false;
            }
        }
        const uint32_t group = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                               (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<uint8_t>(group >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>(group >> 8));
        if (pad < 1) out.push_back(static_cast<uint8_t>(group));
    }
    return true;
}

// One line of `text` starting at `pos`, without its line ending; advances `pos`.
inline bool NextLine(std::string_view text, size_t& pos, std::string_view& line) {
    if (pos >= text.size()) return false;
    size_t end = text.find('\n', pos);
    const size_t next = end == std::string_view::npos ? text.size() : end + 1;
    if (end == std::string_view::npos) end = text.size();
    line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    pos = next;
    return true;
}

struct ParsedSignature {
    uint8_t keyId[8]{};
    uint8_t signature[64]{};
    uint8_t globalSignature[64]{};
    std::string trustedComment;
};

// The four-line minisign signature file. Only the legacy "Ed" algorithm is
// accepted: a prehashed "ED" signature (BLAKE2b) is not supported.
inline bool ParseSignatureFile(std::string_view text, ParsedSignature& out) {
    if (text.empty() || text.size() > kMaxSignatureFileBytes) return false;
    size_t pos = 0;
    std::string_view untrusted, sigLine, trusted, globalLine;
    if (!NextLine(text, pos, untrusted) || !NextLine(text, pos, sigLine) || !NextLine(text, pos, trusted) ||
        !NextLine(text, pos, globalLine)) {
        return false;
    }
    while (pos < text.size()) {  // only blank lines may follow
        std::string_view extra;
        if (!NextLine(text, pos, extra) || !extra.empty()) return false;
    }
    constexpr std::string_view kUntrustedPrefix = "untrusted comment:";
    constexpr std::string_view kTrustedPrefix = "trusted comment: ";
    if (untrusted.substr(0, kUntrustedPrefix.size()) != kUntrustedPrefix) return false;
    if (trusted.substr(0, kTrustedPrefix.size()) != kTrustedPrefix) return false;

    std::vector<uint8_t> sig, global;
    if (!Base64Decode(sigLine, sig) || sig.size() != 2 + 8 + 64) return false;
    if (sig[0] != 'E' || sig[1] != 'd') return false;
    if (!Base64Decode(globalLine, global) || global.size() != 64) return false;

    std::memcpy(out.keyId, sig.data() + 2, 8);
    std::memcpy(out.signature, sig.data() + 10, 64);
    std::memcpy(out.globalSignature, global.data(), 64);
    out.trustedComment.assign(trusted.substr(kTrustedPrefix.size()));
    return true;
}

struct SignedText {
    std::string sha256;
    std::string tag;
};

// "<64 hex>  LeanLauncher.exe  <tag>" and nothing else (one trailing line
// ending is fine).
inline bool ParseSignedText(std::string_view text, SignedText& out) {
    if (text.empty() || text.size() > kMaxSignedTextBytes) return false;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    std::string_view tokens[3];
    int count = 0;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') ++i;
        if (i >= text.size()) break;
        const size_t start = i;
        while (i < text.size() && text[i] != ' ') {
            const unsigned char ch = static_cast<unsigned char>(text[i]);
            if (ch < 0x21 || ch > 0x7e) return false;  // control characters, tabs, newlines, non-ASCII
            ++i;
        }
        if (count == 3) return false;
        tokens[count++] = text.substr(start, i - start);
    }
    if (count != 3 || !IsSha256Hex(tokens[0]) || tokens[1] != "LeanLauncher.exe") return false;
    out.sha256.assign(tokens[0]);
    out.tag.assign(tokens[2]);
    return true;
}

} // namespace minisign_detail

// The public key as printed by minisign: base64 of "Ed" + key id (8) + key (32).
inline bool ParseMinisignPublicKey(std::string_view base64, MinisignPublicKey& out) {
    std::vector<uint8_t> bytes;
    if (!minisign_detail::Base64Decode(base64, bytes) || bytes.size() != 2 + 8 + 32) return false;
    if (bytes[0] != 'E' || bytes[1] != 'd') return false;
    std::memcpy(out.keyId, bytes.data() + 2, 8);
    std::memcpy(out.key, bytes.data() + 10, 32);
    return true;
}

// Decides whether the downloaded exe may be installed. `signedText` and
// `signatureFile` are the contents of LeanLauncher.exe.sha256 and
// LeanLauncher.exe.minisig, `exeSha256Hex` the hash of the exe's bytes, and
// `releaseTag` the tag of the release that was offered.
inline UpdateVerifyResult VerifyUpdate(const MinisignPublicKey& publicKey, std::string_view signedText,
                                       std::string_view signatureFile, std::string_view exeSha256Hex,
                                       std::string_view releaseTag) {
    using namespace minisign_detail;
    ParsedSignature signature;
    SignedText parsed;
    if (!ParseSignatureFile(signatureFile, signature) || !IsSha256Hex(exeSha256Hex) || releaseTag.empty() ||
        signedText.size() > kMaxSignedTextBytes) {
        return UpdateVerifyResult::Malformed;
    }
    if (std::memcmp(signature.keyId, publicKey.keyId, sizeof(publicKey.keyId)) != 0) {
        return UpdateVerifyResult::WrongKey;
    }
    if (!Ed25519Verify(signature.signature, reinterpret_cast<const uint8_t*>(signedText.data()), signedText.size(),
                       publicKey.key)) {
        return UpdateVerifyResult::BadSignature;
    }
    // The trusted comment is covered by a second signature over signature || comment.
    std::vector<uint8_t> globalMessage(signature.signature, signature.signature + 64);
    globalMessage.insert(globalMessage.end(), signature.trustedComment.begin(), signature.trustedComment.end());
    if (!Ed25519Verify(signature.globalSignature, globalMessage.data(), globalMessage.size(), publicKey.key)) {
        return UpdateVerifyResult::BadSignature;
    }
    if (!ParseSignedText(signedText, parsed)) return UpdateVerifyResult::Malformed;
    if (parsed.tag != releaseTag) return UpdateVerifyResult::WrongTag;
    if (!Sha256HexMatches(parsed.sha256, exeSha256Hex)) return UpdateVerifyResult::HashMismatch;
    return UpdateVerifyResult::Ok;
}

} // namespace takeoff
