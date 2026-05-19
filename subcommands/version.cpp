/// @file   subcommands/version.cpp
/// @brief  `goodnetd version` — print goodnetd + kernel versions.
///
/// goodnetd is a downstream consumer of the GoodNet kernel; two
/// version axes matter to operators:
///   - The goodnetd binary's own version (`GOODNETD_VERSION_STRING`,
///     baked in at CMake configure time from the goodnetd flake's
///     project version).
///   - The kernel ABI version reachable through `gn_version()` from
///     `sdk/core.h` — what the goodnetd binary was linked against.
///
/// Both are printed so a deployment audit can correlate goodnetd
/// against kernel at a glance.

#include "../subcommands.hpp"

#include <cstdio>

#include <sdk/core.h>

#ifndef GOODNETD_VERSION_STRING
#define GOODNETD_VERSION_STRING "0.0.0-unknown"
#endif

namespace gn::apps::goodnet {

int cmd_version(std::span<const std::string_view> args) {
    if (!args.empty()) {
        (void)std::fputs("goodnetd version: takes no arguments\n", stderr);
        return 2;
    }
    const char* kernel_v = gn_version();
    (void)std::fprintf(stdout,
                       "goodnetd %s (kernel %s)\n",
                       GOODNETD_VERSION_STRING,
                       kernel_v ? kernel_v : "unknown");
    return 0;
}

}  // namespace gn::apps::goodnet
