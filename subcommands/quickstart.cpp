/// @file   subcommands/quickstart.cpp
/// @brief  `goodnetd quickstart` — first-time setup wizard.
///
/// Walks an operator from a fresh checkout to a usable runtime in
/// three idempotent steps:
///   1. identity — generate `~/.local/share/goodnet/identity/default.bin`
///      (skipped if the file is already present);
///   2. plugins — print the `nix run goodnet#bootstrap-env` invocation
///      that actually installs the baseline plugin set. Quickstart
///      itself never shells out to `nix build` — the binary stays
///      hermetic, the operator runs the installer themselves;
///   3. config  — write `~/.local/share/goodnet/config.json` with
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

[[nodiscard]] int step_identity(const std::filesystem::path& data_dir,
                                 bool interactive) {
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
            "[1/3] identity: already configured at %s — skipping\n",
            out_path.c_str());
        return 0;
    }

    /// Ensure the parent dir exists. `create_directories` is the
    /// idempotent variant — no-op if the path is already a directory.
    const auto parent = std::filesystem::path{out_path}.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            (void)std::fprintf(stderr,
                "[1/3] identity: failed to create %s — %s\n",
                parent.string().c_str(), ec.message().c_str());
            return 1;
        }
    }

    /// Delegate the actual keypair + on-disk write to `cmd_identity`.
    /// This matches what the operator would type interactively and
    /// keeps the mode-0600 + secret-handling rules in one place.
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
            "[1/3] identity: cmd_identity returned %d\n", rc);
        return rc;
    }
    (void)std::fprintf(stdout, "[1/3] identity: wrote %s\n", out_path.c_str());
    return 0;
}

void step_plugins() {
    /// Quickstart deliberately does not shell out to `nix build` /
    /// `nix run` here — the binary should never surprise an operator
    /// with multi-gigabyte download IO. We print the exact one-liner
    /// the operator can paste back on the shell themselves.
    (void)std::fputs(
        "[2/3] plugins: baseline plugin install is delegated to\n"
        "      nix run goodnet#bootstrap-env\n"
        "      (link-tcp, link-udp, security-noise, handler-heartbeat).\n"
        "      Run that command once you finish quickstart; it lands\n"
        "      .so files under ~/.local/share/goodnet/plugins/.\n",
        stdout);
}

[[nodiscard]] int step_config(const std::filesystem::path& data_dir) {
    const auto cfg_path = data_dir / "config.json";

    std::error_code ec;
    if (std::filesystem::exists(cfg_path, ec)) {
        (void)std::fprintf(stdout,
            "[3/3] config: already present at %s — skipping\n",
            cfg_path.string().c_str());
        return 0;
    }
    if (!std::filesystem::exists(data_dir, ec)) {
        std::filesystem::create_directories(data_dir, ec);
        if (ec) {
            (void)std::fprintf(stderr,
                "[3/3] config: failed to create %s — %s\n",
                data_dir.string().c_str(), ec.message().c_str());
            return 1;
        }
    }

    /// Default config:
    ///   - identity.path  → XDG identity blob written in step 1;
    ///   - manifest_path  → baseline manifest the operator generates
    ///                       via `goodnetd manifest gen` once the
    ///                       bootstrap-env step lands the .so files;
    ///   - listeners[0]   → TCP on 0.0.0.0:9100, the documented
    ///                       default for the link-tcp baseline plugin.
    nlohmann::json doc = {
        {"identity", {
            {"path", (data_dir / "identity" / "default.bin").string()},
        }},
        {"manifest_path",
         (data_dir / "manifests" / "baseline.json").string()},
        {"listeners", nlohmann::json::array({
            { {"uri", "tcp://0.0.0.0:9100"} },
        })},
    };
    std::ofstream f(cfg_path, std::ios::binary | std::ios::trunc);
    if (!f) {
        (void)std::fprintf(stderr,
            "[3/3] config: cannot open %s for write\n",
            cfg_path.string().c_str());
        return 1;
    }
    f << doc.dump(2) << '\n';
    if (!f.good()) {
        (void)std::fprintf(stderr,
            "[3/3] config: write to %s failed\n",
            cfg_path.string().c_str());
        return 1;
    }
    (void)std::fprintf(stdout, "[3/3] config: wrote %s\n",
                        cfg_path.string().c_str());
    return 0;
}

void print_summary() {
    /// Trailing summary intentionally references `goodnetd run` (the
    /// production daemon subcommand wired in `main.cpp`). Doctor
    /// follows for verification — completes the loop the doctor's
    /// own missing-config hint points back at.
    (void)std::fputs(
        "\n"
        "Setup complete. Next:\n"
        "    nix run goodnet#bootstrap-env      # install baseline plugins\n"
        "    goodnetd run --config ~/.local/share/goodnet/config.json \\\n"
        "                 --manifest ~/.local/share/goodnet/manifests/baseline.json \\\n"
        "                 --identity ~/.local/share/goodnet/identity/default.bin\n"
        "    goodnetd doctor                    # verify the runtime env\n",
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
    step_plugins();
    if (const int rc = step_config(data_dir); rc != 0) {
        return rc;
    }
    print_summary();
    return 0;
}

}  // namespace gn::apps::goodnet
