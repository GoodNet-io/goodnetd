// SPDX-License-Identifier: Apache-2.0
/// @file   apps/goodnetd/subcommands/external_key.cpp
/// @brief  `goodnetd external-key <uri>` — derive the 32-byte public key
///         the raw_inject / ws_inject plugins assign to an external peer.
///
/// Both raw_inject and ws_inject use four rounds of FNV-1a 64-bit with
/// salt = round index to map a peer URI to a deterministic 32-byte key.
/// This subcommand exposes that derivation so operators can resolve which
/// key a bridged peer will appear as in manifests and logs.

#include "../subcommands.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

namespace gn::apps::goodnet {
namespace {

constexpr int kPkBytes = 32;

void derive_pk(std::string_view uri, std::uint8_t out[kPkBytes]) noexcept {
    constexpr std::uint64_t kBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (int round = 0; round < 4; ++round) {
        std::uint64_t h = kBasis ^ static_cast<std::uint64_t>(round);
        for (unsigned char c : uri) { h ^= c; h *= kPrime; }
        std::memcpy(out + round * 8, &h, 8);
    }
}

}  // namespace

int cmd_external_key(std::span<const std::string_view> args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        (void)std::fputs(
            "usage: goodnetd external-key <uri>\n"
            "\n"
            "Derive the 32-byte public key that raw_inject / ws_inject assign\n"
            "to an external peer at <uri>. Prints 64 lowercase hex digits.\n"
            "\n"
            "example:\n"
            "  goodnetd external-key tcp://192.168.1.100:9000\n",
            args.empty() ? stderr : stdout);
        return args.empty() ? 2 : 0;
    }

    std::uint8_t pk[kPkBytes];
    derive_pk(args[0], pk);

    for (int i = 0; i < kPkBytes; ++i) {
        (void)std::printf("%02x", pk[i]);
    }
    (void)std::putchar('\n');
    return 0;
}

}  // namespace gn::apps::goodnet
