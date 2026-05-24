/// @file   subcommands/quickstart.cpp
/// @brief  `goodnetd quickstart` — first-time setup wizard.
///
/// Walks an operator from a fresh checkout to a usable runtime in
/// four idempotent steps:
///   1. identity — generate `~/.local/share/goodnet/identity/default.bin`
///      (skipped if the file is already present);
///   2. plugins  — scan the Nix profile and any local plugin dirs for
///      installed .so files; print install instructions if none found;
///   3. manifest — write `~/.local/share/goodnet/manifests/baseline.json`
///      from the .so files found in step 2 (skipped if already present
///      or if no plugins were detected);
///   4. config   — write `~/.local/share/goodnet/config.json` with
///      sane defaults if the file is missing.
///
/// `--non-interactive` skips every prompt (uses defaults) so the
/// wizard can be scripted into a Docker / NixOS image build. The
/// non-interactive run is the same code path as the interactive
/// one minus the `getline` reads, so the two surfaces stay in sync
/// by construction.
///
/// Identity generation reuses `cmd_identity` (defined in
/// `subcommands/identity.cpp`) rather than relinking
/// `gn::core::identity::NodeIdentity` here — the existing subcommand
/// already handles the `--out`/`--expiry` parse + the mode-0600 write,
/// and going through it keeps quickstart's behaviour identical to
/// what the operator would get by typing the command by hand.

#include "../subcommands.hpp"
#include "sha256_file.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace gn::apps::goodnet {

namespace {

[[nodiscard]] std::filesystem::path xdg_data_home_qs() {
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
        return std::filesystem::path{x};
    }
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path{h} / ".local" / "share";
    }
    return {};
}

[[nodiscard]] std::filesystem::path goodnet_data_dir_qs() {
    auto p = xdg_data_home_qs();
    if (p.empty()) return {};
    return p / "goodnet";
}

/// Prompt the operator for a single line, falling back to @p def on
/// either EOF or an empty answer. The wizard's only interactive
/// surface — every other step is a print-then-act.
[[nodiscard]] std::string prompt(const std::string& message,
                                  const std::string& def) {
    (void)std::fprintf(stdout, "%s [%s]: ", message.c_str(), def.c_str());
    (void)std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) {
        return def;
    }
    if (line.empty()) return def;
    return line;
}

/// Interactive HSM branch of the identity step. Reached when the
/// operator answers "2" to the backend question. Returns 0 on
/// success; otherwise propagates the import-hsm exit code so a
/// failed PKCS#11 setup aborts quickstart instead of falling
/// through to a half-configured state.
[[nodiscard]] int step_identity_hsm(bool interactive) {
    if (!interactive) {
        (void)std::fputs(
            "[1/4] identity: HSM backend not supported in --non-interactive "
            "mode (no PIN env / module path defaults). Run `goodnetd "
            "identity import-hsm` after quickstart finishes.\n",
            stderr);
        return 1;
    }
    const std::string module_path = prompt(
        "PKCS#11 module path",
        "/usr/lib/softhsm/libsofthsm2.so");
    const std::string key_label = prompt(
        "PKCS#11 key label (CKA_LABEL on the token)",
        "goodnet");
    const std::string pin_env = prompt(
        "Env var name carrying the PIN at run time",
        "GOODNET_PKCS11_PIN");

    const std::array<std::string_view, 8> argv = {
        std::string_view{"--extension-id"},
        std::string_view{"gn.identity.pkcs11"},
        std::string_view{"--key-label"},
        std::string_view{key_label},
        std::string_view{"--module"},
        std::string_view{module_path},
        std::string_view{"--pin-env"},
        std::string_view{pin_env},
    };
    const int rc = cmd_identity_import_hsm(
        std::span<const std::string_view>{argv.data(), argv.size()});
    if (rc != 0) {
        (void)std::fprintf(stderr,
            "[1/4] identity: cmd_identity_import_hsm returned %d\n", rc);
        return rc;
    }
    (void)std::fputs(
        "[1/4] identity: wrote identity-config.json (backend=provider)\n",
        stdout);
    return 0;
}

[[nodiscard]] int step_identity(const std::filesystem::path& data_dir,
                                 bool interactive) {
    if (interactive) {
        (void)std::fputs(
            "Identity backend:\n"
            "  1) File (default — ~/.local/share/goodnet/identity/default.bin)\n"
            "  2) PKCS#11 HSM (YubiKey / SoftHSM / enterprise HSM)\n",
            stdout);
        const std::string choice = prompt("Choose", "1");
        if (choice == "2") {
            return step_identity_hsm(interactive);
        }
    }

    const auto default_path = data_dir / "identity" / "default.bin";
    std::string out_path;
    if (interactive) {
        out_path = prompt("Identity file path", default_path.string());
    } else {
        out_path = default_path.string();
    }

    std::error_code ec;
    if (std::filesystem::exists(out_path, ec)) {
        (void)std::fprintf(stdout,
            "[1/4] identity: already configured at %s — skipping\n",
            out_path.c_str());
        return 0;
    }

    const auto parent = std::filesystem::path{out_path}.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            (void)std::fprintf(stderr,
                "[1/4] identity: failed to create %s — %s\n",
                parent.string().c_str(), ec.message().c_str());
            return 1;
        }
    }

    const std::string flag_out{"--out"};
    const std::array<std::string_view, 3> argv = {
        std::string_view{"gen"},
        std::string_view{flag_out},
        std::string_view{out_path},
    };
    const int rc = cmd_identity(
        std::span<const std::string_view>{argv.data(), argv.size()});
    if (rc != 0) {
        (void)std::fprintf(stderr,
            "[1/4] identity: cmd_identity returned %d\n", rc);
        return rc;
    }
    (void)std::fprintf(stdout, "[1/4] identity: wrote %s\n", out_path.c_str());
    return 0;
}

/// Scan candidate directories for installed GoodNet plugin .so files.
/// Priority order:
///   1. ~/.nix-profile/lib/goodnet/plugins/  (nix profile add .#full)
///   2. ~/.local/share/goodnet/plugins/      (manual / bootstrap-env installs)
/// Returns sorted paths; empty if no plugins found anywhere.
[[nodiscard]] std::vector<std::string> find_profile_plugins() {
    std::vector<std::filesystem::path> candidates;

    auto scan_dir = [&](const std::filesystem::path& dir) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return;
        for (const auto& entry :
             std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            const auto& p = entry.path();
            if (p.extension() == ".so") {
                candidates.push_back(p);
            }
        }
    };

    if (const char* h = std::getenv("HOME"); h && *h) {
        scan_dir(std::filesystem::path{h} / ".nix-profile"
                 / "lib" / "goodnet" / "plugins");
        scan_dir(std::filesystem::path{h} / ".local" / "share"
                 / "goodnet" / "plugins");
    }
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
        scan_dir(std::filesystem::path{x} / "goodnet" / "plugins");
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    std::vector<std::string> result;
    result.reserve(candidates.size());
    for (auto& p : candidates) {
        result.push_back(p.string());
    }
    return result;
}

/// Detect installed plugins and populate @p out_paths.
/// Prints a summary of what was found; if nothing is found, prints
/// install instructions so the operator knows what to do next.
void step_plugins(std::vector<std::string>& out_paths) {
    out_paths = find_profile_plugins();
    if (!out_paths.empty()) {
        (void)std::fprintf(stdout,
            "[2/4] plugins: found %zu plugin(s):\n",
            out_paths.size());
        for (const auto& p : out_paths) {
            (void)std::fprintf(stdout, "        %s\n", p.c_str());
        }
        return;
    }
    (void)std::fputs(
        "[2/4] plugins: no plugins found in ~/.nix-profile or "
        "~/.local/share/goodnet/plugins/\n"
        "      Install with one of:\n"
        "        nix profile add github:GoodNet-io/goodnetd#full\n"
        "        # or build from source:\n"
        "        nix profile add path:/path/to/goodnetd#full\n"
        "      Then re-run quickstart to generate the manifest.\n",
        stdout);
}

/// Write the plugin manifest to @p manifest_path from @p plugin_paths.
/// Skips if the file already exists (idempotent).
/// Returns 0 on success, 1 on failure.
[[nodiscard]] int step_manifest(const std::filesystem::path& data_dir,
                                 const std::vector<std::string>& plugin_paths) {
    const auto manifest_path = data_dir / "manifests" / "baseline.json";

    std::error_code ec;
    if (std::filesystem::exists(manifest_path, ec)) {
        (void)std::fprintf(stdout,
            "[3/4] manifest: already present at %s — skipping\n",
            manifest_path.string().c_str());
        return 0;
    }

    if (plugin_paths.empty()) {
        (void)std::fputs(
            "[3/4] manifest: skipped — no plugins detected "
            "(install plugins then re-run quickstart)\n",
            stdout);
        return 0;
    }

    const auto manifest_dir = manifest_path.parent_path();
    std::filesystem::create_directories(manifest_dir, ec);
    if (ec) {
        (void)std::fprintf(stderr,
            "[3/4] manifest: failed to create %s — %s\n",
            manifest_dir.string().c_str(), ec.message().c_str());
        return 1;
    }

    std::string out;
    out.reserve(plugin_paths.size() * 128);
    out.append("{\n  \"plugins\": [\n");
    bool first = true;
    for (const auto& path : plugin_paths) {
        std::uint8_t digest[32]{};
        if (!sha256_file(path, digest)) {
            (void)std::fprintf(stderr,
                "[3/4] manifest: %s — failed to hash file\n", path.c_str());
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

    std::ofstream f(manifest_path, std::ios::binary | std::ios::trunc);
    if (!f) {
        (void)std::fprintf(stderr,
            "[3/4] manifest: cannot open %s for write\n",
            manifest_path.string().c_str());
        return 1;
    }
    f << out;
    if (!f.good()) {
        (void)std::fprintf(stderr,
            "[3/4] manifest: write to %s failed\n",
            manifest_path.string().c_str());
        return 1;
    }
    (void)std::fprintf(stdout,
        "[3/4] manifest: wrote %s (%zu plugin(s))\n",
        manifest_path.string().c_str(), plugin_paths.size());
    return 0;
}

[[nodiscard]] int step_config(const std::filesystem::path& data_dir) {
    const auto cfg_path = data_dir / "config.json";

    std::error_code ec;
    if (std::filesystem::exists(cfg_path, ec)) {
        (void)std::fprintf(stdout,
            "[4/4] config: already present at %s — skipping\n",
            cfg_path.string().c_str());
        return 0;
    }
    if (!std::filesystem::exists(data_dir, ec)) {
        std::filesystem::create_directories(data_dir, ec);
        if (ec) {
            (void)std::fprintf(stderr,
                "[4/4] config: failed to create %s — %s\n",
                data_dir.string().c_str(), ec.message().c_str());
            return 1;
        }
    }

    nlohmann::json doc = {
        {"identity", {
            {"path", (data_dir / "identity" / "default.bin").string()},
        }},
        {"manifest_path",
         (data_dir / "manifests" / "baseline.json").string()},
        {"listeners", nlohmann::json::array({
            { {"uri", "ws://0.0.0.0:9100"} },
            { {"uri", "tcp://0.0.0.0:9101"} },
        })},
    };
    std::ofstream f(cfg_path, std::ios::binary | std::ios::trunc);
    if (!f) {
        (void)std::fprintf(stderr,
            "[4/4] config: cannot open %s for write\n",
            cfg_path.string().c_str());
        return 1;
    }
    f << doc.dump(2) << '\n';
    if (!f.good()) {
        (void)std::fprintf(stderr,
            "[4/4] config: write to %s failed\n",
            cfg_path.string().c_str());
        return 1;
    }
    (void)std::fprintf(stdout, "[4/4] config: wrote %s\n",
                        cfg_path.string().c_str());
    return 0;
}

void print_summary(bool has_manifest) {
    (void)std::fputs("\nSetup complete. Next:\n", stdout);
    if (!has_manifest) {
        (void)std::fputs(
            "    # Install plugins first:\n"
            "    nix profile add github:GoodNet-io/goodnetd#full\n"
            "    # Then re-run quickstart to generate the manifest:\n"
            "    goodnetd quickstart\n",
            stdout);
        return;
    }
    (void)std::fputs(
        "    goodnetd run \\\n"
        "      --config   ~/.local/share/goodnet/config.json \\\n"
        "      --manifest ~/.local/share/goodnet/manifests/baseline.json \\\n"
        "      --identity ~/.local/share/goodnet/identity/default.bin\n"
        "    goodnetd doctor    # verify the runtime env\n",
        stdout);
}

}  // namespace

int cmd_quickstart(std::span<const std::string_view> args) {
    bool interactive = true;
    for (const auto& a : args) {
        if (a == "--non-interactive") {
            interactive = false;
        } else if (a == "--help" || a == "-h") {
            (void)std::fputs(
                "usage: goodnetd quickstart [--non-interactive]\n"
                "\n"
                "First-time wizard. Each step is idempotent — re-running\n"
                "quickstart skips work that is already done. Pass\n"
                "--non-interactive to use defaults without prompts (for\n"
                "scripted image builds).\n",
                stdout);
            return 0;
        } else {
            (void)std::fprintf(stderr,
                "goodnetd quickstart: unknown argument '%.*s'\n",
                static_cast<int>(a.size()), a.data());
            return 2;
        }
    }

    const auto data_dir = goodnet_data_dir_qs();
    if (data_dir.empty()) {
        (void)std::fputs(
            "goodnetd quickstart: $HOME / $XDG_DATA_HOME not set — cannot "
            "locate goodnet data dir\n",
            stderr);
        return 1;
    }

    (void)std::fprintf(stdout,
        "goodnetd quickstart: target data dir %s%s\n",
        data_dir.string().c_str(),
        interactive ? "" : " (non-interactive)");

    if (const int rc = step_identity(data_dir, interactive); rc != 0) {
        return rc;
    }

    std::vector<std::string> plugin_paths;
    step_plugins(plugin_paths);

    if (const int rc = step_manifest(data_dir, plugin_paths); rc != 0) {
        return rc;
    }

    if (const int rc = step_config(data_dir); rc != 0) {
        return rc;
    }

    const auto manifest_path = data_dir / "manifests" / "baseline.json";
    std::error_code ec;
    const bool has_manifest = std::filesystem::exists(manifest_path, ec);
    print_summary(has_manifest);
    return 0;
}

}  // namespace gn::apps::goodnet
