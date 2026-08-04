/// @file   apps/goodnetd/tests/test_external_key.cpp
/// @brief  Unit tests for `cmd_external_key`.
///
/// Verifies:
///   1. Known-URI determinism — same URI produces identical 64-hex output.
///   2. Format — exactly 64 lowercase hex chars + newline.
///   3. Collision resistance — two distinct URIs produce distinct keys.
///   4. Algorithm match — output equals the FNV-1a derivation used by
///      raw_inject / ws_inject (reimplemented here for cross-check).

#include "../subcommands.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Reference implementation mirroring raw_inject::derive_source_pk.
void ref_derive_pk(std::string_view uri, std::uint8_t out[32]) noexcept {
    constexpr std::uint64_t kBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (int round = 0; round < 4; ++round) {
        std::uint64_t h = kBasis ^ static_cast<std::uint64_t>(round);
        for (unsigned char c : uri) { h ^= c; h *= kPrime; }
        std::memcpy(out + round * 8, &h, 8);
    }
}

std::string ref_hex(std::string_view uri) {
    std::uint8_t pk[32];
    ref_derive_pk(uri, pk);
    char buf[65];
    for (int i = 0; i < 32; ++i) {
        std::snprintf(buf + i * 2, 3, "%02x", pk[i]);
    }
    buf[64] = '\0';
    return buf;
}

// Capture stdout while calling cmd_external_key.
std::string capture(std::string_view uri_arg) {
    std::vector<std::string_view> args{uri_arg};
    // Redirect stdout to a pipe, call cmd_external_key, read result.
    int fds[2];
    assert(::pipe(fds) == 0);
    int saved = ::dup(STDOUT_FILENO);
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[1]);

    gn::apps::goodnet::cmd_external_key(
        std::span<const std::string_view>{args.data(), args.size()});

    ::fflush(stdout);
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);

    char buf[128] = {};
    ssize_t n = ::read(fds[0], buf, sizeof(buf) - 1);
    ::close(fds[0]);
    if (n > 0) buf[n] = '\0';
    return buf;
}

}  // namespace

#include <unistd.h>

int main() {
    // Test 1: format — 64 hex chars + newline.
    {
        std::string out = capture("tcp://127.0.0.1:9000");
        assert(out.size() == 65);   // 64 hex + '\n'
        assert(out[64] == '\n');
        for (int i = 0; i < 64; ++i) {
            char c = out[i];
            assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
    }

    // Test 2: determinism — two calls with same URI give identical output.
    {
        std::string a = capture("tcp://192.168.1.100:9000");
        std::string b = capture("tcp://192.168.1.100:9000");
        assert(a == b);
    }

    // Test 3: collision resistance — different URIs give different keys.
    {
        std::string a = capture("tcp://192.168.1.100:9000");
        std::string b = capture("tcp://192.168.1.101:9000");
        assert(a != b);
    }

    // Test 4: algorithm match — output equals ref_derive_pk.
    {
        const char* uris[] = {
            "tcp://192.168.1.100:9000",
            "tcp://0.0.0.0:9101",
            "ws://10.0.0.1:8080/path",
        };
        for (auto uri : uris) {
            std::string got = capture(uri);
            got.pop_back();  // strip '\n'
            std::string want = ref_hex(uri);
            assert(got == want);
        }
    }

    // Test 5: help flag — exits 0, no crash.
    {
        std::vector<std::string_view> h{"--help"};
        int rc = gn::apps::goodnet::cmd_external_key(
            std::span<const std::string_view>{h.data(), h.size()});
        assert(rc == 0);
    }

    // Test 6: no args — exits 2 (usage error).
    {
        int rc = gn::apps::goodnet::cmd_external_key(
            std::span<const std::string_view>{});
        assert(rc == 2);
    }

    std::puts("test_external_key: all tests passed");
    return 0;
}
