/// @file   subcommands/sha256_file.hpp
/// @brief  Shared SHA-256-of-file helper for `plugin hash`,
///         `manifest gen`, and `run`.
///
/// Streams the file at @p path through libsodium's
/// `crypto_hash_sha256_state` and writes the 32-byte digest into
/// @p out. The byte-exact mirror of what the kernel computes during
/// `gn_core_load_plugin` integrity check (per `plugin-manifest.en.md`
/// §2) so a manifest emitted here verifies cleanly at load time.
///
/// The function lives in a header (rather than the C ABI) on purpose:
/// SHA-256 is a wire-format primitive, and the public SDK does not
/// expose it through `sdk/core.h`. Pulling libsodium directly is the
/// path examples/two_node/main.cpp takes and the path the kernel's
/// own audit doc points downstream consumers at.

#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <sodium.h>

namespace gn::apps::goodnet {

[[nodiscard]] inline bool sha256_file(const std::string& path,
                                       std::uint8_t out[32]) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    crypto_hash_sha256_state st;
    if (crypto_hash_sha256_init(&st) != 0) return false;
    std::vector<char> buf(64 * 1024);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = f.gcount();
        if (got > 0) {
            if (crypto_hash_sha256_update(
                    &st,
                    reinterpret_cast<const std::uint8_t*>(buf.data()),
                    static_cast<std::size_t>(got)) != 0) {
                return false;
            }
        }
        if (f.bad()) return false;
    }
    return crypto_hash_sha256_final(&st, out) == 0;
}

/// Lowercase-hex encode 32 bytes into a fixed 64-char string. Matches
/// the manifest wire form (`plugin-manifest.en.md` §2 `sha256` field
/// is "64-character lowercase hex").
[[nodiscard]] inline std::string sha256_hex(const std::uint8_t digest[32]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (std::size_t i = 0; i < 32; ++i) {
        out[i * 2 + 0] = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return out;
}

/// Decode a 64-char lowercase-hex string into 32 raw bytes. Returns
/// false on any non-hex character or wrong length — used by `run`
/// when reading the manifest sha256 field for the C ABI's
/// `gn_core_load_plugin(expected_sha256[32])` parameter.
[[nodiscard]] inline bool hex_decode_32(std::string_view hex,
                                         std::uint8_t out[32]) {
    if (hex.size() != 64) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = nibble(hex[i * 2 + 0]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace gn::apps::goodnet
