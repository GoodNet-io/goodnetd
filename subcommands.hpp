/// @file   apps/goodnet/subcommands.hpp
/// @brief  Subcommand dispatch table for the `goodnet` multicall.
///
/// Each subcommand exposes one entry point taking the subcommand's
/// own argv slice (i.e. `argv[1..]` after the dispatcher peels the
/// subcommand name off `argv[0]`-style). Returns the process exit
/// code: 0 success, 1 generic failure, 2 usage error.

#pragma once

#include <span>
#include <string_view>

namespace gn::apps::goodnet {

[[nodiscard]] int cmd_version(std::span<const std::string_view> args);
[[nodiscard]] int cmd_config_validate(std::span<const std::string_view> args);
[[nodiscard]] int cmd_plugin_hash(std::span<const std::string_view> args);
[[nodiscard]] int cmd_manifest_gen(std::span<const std::string_view> args);
[[nodiscard]] int cmd_identity(std::span<const std::string_view> args);
[[nodiscard]] int cmd_identity_import_hsm(std::span<const std::string_view> args);
[[nodiscard]] int cmd_run(std::span<const std::string_view> args);
[[nodiscard]] int cmd_doctor(std::span<const std::string_view> args);
[[nodiscard]] int cmd_quickstart(std::span<const std::string_view> args);

/// Test-only seam. Doctor's provider-backend reachability check
/// normally spins a fresh `gn_core_t` and queries the extension
/// registry; tests inject a deterministic result here so the test
/// binary doesn't need to link the kernel. @p hook returns 0 when
/// the named extension is reachable, non-zero otherwise. Pass
/// nullptr to clear (the default production behaviour).
void set_doctor_provider_query_hook_for_test(
    int (*hook)(const std::string& extension_id,
                const std::string& key_label));

}  // namespace gn::apps::goodnet
