/// @file   subcommands/config_validate.cpp
/// @brief  `goodnetd config validate <file>` — pre-deploy config check.
///
/// Reads the config file from disk and pipes its contents through
/// `gn_core_create_from_json` — the kernel's own JSON parse +
/// structural validator, reached entirely through the public C ABI.
/// On a successful parse the throw-away kernel handle is immediately
/// destroyed (the validator never reaches `gn_core_init`, so no
/// identity is minted, no FSM transition happens, no resources are
/// touched beyond a transient `gn_core_t`). On parse failure the
/// kernel returns a NULL handle and we report it. Operators wire
/// this into `ExecStartPre=` of their systemd unit so a malformed
/// config fails the unit start instead of crashing the kernel
/// mid-handshake.
///
/// Note: `gn_core_create_from_json` returns NULL on either parse
/// failure OR OOM. We assume the OOM path is unreachable for a
/// human-sized config and report parse failure on every NULL.

#include "../subcommands.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <sdk/core.h>

namespace gn::apps::goodnet {

namespace {

[[nodiscard]] bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

int cmd_config_validate(std::span<const std::string_view> args) {
    if (args.size() != 1) {
        (void)std::fputs(
            "goodnetd config validate: needs exactly one <file> argument\n",
            stderr);
        return 2;
    }
    const std::string path{args[0]};

    std::string bytes;
    if (!read_file(path, bytes)) {
        (void)std::fprintf(stderr,
                           "goodnetd config validate: %s — cannot open file\n",
                           path.c_str());
        return 1;
    }

    /// `gn_core_create_from_json` runs the kernel's own structural
    /// validation against @p bytes. A non-NULL return means the
    /// config parsed cleanly under the same rules the live kernel
    /// applies; we destroy the throw-away handle immediately.
    gn_core_t* core = gn_core_create_from_json(bytes.c_str());
    if (!core) {
        /// The public ABI does not surface a parse diagnostic
        /// pointer — `gn_core_create_from_json` returns NULL on
        /// either parse failure or OOM with no further detail.
        /// TODO(sdk-extension): add `gn_core_create_from_json_diag(
        /// const char* json, char* diag_out, size_t cap)` so the
        /// validator can route the kernel's structural error string
        /// (e.g. "limits.max_connections is negative") back up.
        (void)std::fprintf(stderr,
                           "goodnetd config validate: %s — parse failed "
                           "(structural error or OOM; no diagnostic exposed "
                           "through public ABI)\n",
                           path.c_str());
        return 1;
    }
    gn_core_destroy(core);
    (void)std::fprintf(stdout, "goodnetd config validate: %s OK\n", path.c_str());
    return 0;
}

}  // namespace gn::apps::goodnet
