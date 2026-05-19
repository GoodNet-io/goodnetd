/// @file   subcommands/manifest_gen.cpp
/// @brief  `goodnetd manifest gen <so>...` — emit `plugins.json`.
///
/// Streams a fresh manifest document to stdout in the wire format
/// the kernel parses at `gn_core_load_plugin` time (per
/// `plugin-manifest.en.md` §2): a single `"plugins"` array of
/// `{ "path", "sha256" }` records. Hashes come from libsodium's
/// `crypto_hash_sha256` — same primitive the kernel uses during the
/// integrity check, so a manifest emitted here verifies cleanly when
/// `gn_core_load_plugin` runs against the same bytes.
///
/// Output is plain JSON without trailing whitespace surprises so it
/// can be redirected straight into a file (`> plugins.json`) or
/// piped through `jq`. Failure on any path prints to stderr and the
/// process exits non-zero with no manifest written, leaving the
/// operator's previous file intact.
///
/// The wire format is stable per the manifest contract; this file
/// writes it directly rather than reaching into kernel-private
/// `core/plugin/plugin_manifest.hpp` (which is no longer reachable
/// from a downstream SDK consumer).

#include "../subcommands.hpp"
#include "sha256_file.hpp"

#include <cstdio>
#include <string>

namespace gn::apps::goodnet {

int cmd_manifest_gen(std::span<const std::string_view> args) {
    if (args.empty()) {
        (void)std::fputs("goodnetd manifest gen: needs one or more <so> arguments\n",
                          stderr);
        return 2;
    }

    /// Hash every input first; only emit JSON once every digest is in
    /// hand so a partial failure does not leave half a manifest on
    /// stdout that the operator's redirect captures as a valid file.
    std::string out;
    out.reserve(args.size() * 128);
    out.append("{\n  \"plugins\": [\n");
    bool first = true;
    for (const auto path_sv : args) {
        const std::string path{path_sv};
        std::uint8_t digest[32]{};
        if (!sha256_file(path, digest)) {
            (void)std::fprintf(stderr,
                               "goodnetd manifest gen: %s — failed to read file\n",
                               path.c_str());
            return 1;
        }
        const auto hex = sha256_hex(digest);
        if (!first) out.append(",\n");
        first = false;
        out.append("    { \"path\": \"");
        out.append(path);
        out.append("\", \"sha256\": \"");
        out.append(hex);
        out.append("\" }");
    }
    out.append("\n  ]\n}\n");
    (void)std::fputs(out.c_str(), stdout);
    return 0;
}

}  // namespace gn::apps::goodnet
