/// @file   subcommands/plugin_hash.cpp
/// @brief  `goodnetd plugin hash <so>` — print SHA-256 of a plugin .so.
///
/// Pure local IO + libsodium SHA-256, mirroring the digest the kernel
/// computes during `gn_core_load_plugin`'s manifest integrity check
/// (`plugin-manifest.en.md` §2). No kernel handle is allocated for
/// this subcommand — it is a pure operator-side bookkeeping helper.
///
/// Output is the hex form, unprefixed, followed by two spaces and
/// the path — same shape as `sha256sum(1)` so a manifest can be
/// assembled with shell tooling if desired.

#include "../subcommands.hpp"
#include "sha256_file.hpp"

#include <cstdio>
#include <string>

namespace gn::apps::goodnet {

int cmd_plugin_hash(std::span<const std::string_view> args) {
    if (args.size() != 1) {
        (void)std::fputs("goodnetd plugin hash: needs exactly one <so> argument\n",
                          stderr);
        return 2;
    }
    const std::string path{args[0]};

    std::uint8_t digest[32]{};
    if (!sha256_file(path, digest)) {
        (void)std::fprintf(stderr,
                           "goodnetd plugin hash: %s — failed to read file\n",
                           path.c_str());
        return 1;
    }
    const auto hex = sha256_hex(digest);
    (void)std::fprintf(stdout, "%s  %s\n", hex.c_str(), path.c_str());
    return 0;
}

}  // namespace gn::apps::goodnet
