/// @file   apps/goodnetd/subcommands/identity_import_hsm.cpp
/// @brief  `goodnetd identity import-hsm` — switch the daemon to an
///         HSM-backed identity by writing `identity-config.json`.
///
/// Phase 4 of the identity refactor (kernel-side Phase 2 landed
/// `gn_core_install_identity_from_provider`; Phase 3 added the
/// PKCS#11 plugin's `gn.identity.pkcs11` extension; this subcommand
/// is the operator-facing UX). It does NOT touch the on-token key
/// material — provisioning the on-token Ed25519 stays with the
/// vendor's `pkcs11-tool` / `softhsm2-util`. What this command does
/// own is the descriptor: a JSON file the daemon reads at boot to
/// decide whether to mint a file-backed identity or query an
/// extension-registered signer.
///
/// The descriptor lives next to (not inside) the existing file-based
/// `identity/default.bin`. Both coexist; `identity-config.json` carries
/// a `backend` field that is the source of truth. Existing
/// `identity gen` / `identity show` operate on the file-backed path
/// unchanged.
///
/// `extra.*` keys are plugin-opaque — the kernel never reads them.
/// `cmd_run` peels them out and `setenv`s each one before kernel init
/// so the PKCS#11 plugin (which reads its module path / PIN from env)
/// can pick them up via the same code path it uses for an embedded
/// host. This keeps the daemon's config surface small (one descriptor
/// file) without baking PKCS#11-specific knobs into the kernel ABI.

#include "../subcommands.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

namespace gn::apps::goodnet {

namespace {

[[nodiscard]] std::filesystem::path default_config_path() {
    /// Mirror the XDG layout `doctor` / `quickstart` walk so the
    /// descriptor lives next to `identity/default.bin`, `plugins/`,
    /// `manifests/baseline.json` rather than in an isolated home of
    /// its own. Falling back to `$HOME/.local/share` matches the XDG
    /// spec when the env override is unset.
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
        return std::filesystem::path{x} / "goodnet" / "identity-config.json";
    }
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path{h} / ".local" / "share" / "goodnet"
               / "identity-config.json";
    }
    return std::filesystem::path{"identity-config.json"};
}

struct Args {
    std::string extension_id;
    std::string key_label;
    std::string pin_env;
    std::string module_path;
    std::string config_path;
    bool        force = false;
};

[[nodiscard]] int parse_args(std::span<const std::string_view> args, Args& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        const auto need_val = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                (void)std::fprintf(stderr,
                    "goodnetd identity import-hsm: %s requires an argument\n",
                    flag);
                return false;
            }
            return true;
        };
        if (a == "--extension-id") {
            if (!need_val("--extension-id")) return 2;
            out.extension_id.assign(args[++i]);
        } else if (a == "--key-label") {
            if (!need_val("--key-label")) return 2;
            out.key_label.assign(args[++i]);
        } else if (a == "--pin-env") {
            if (!need_val("--pin-env")) return 2;
            out.pin_env.assign(args[++i]);
        } else if (a == "--module") {
            if (!need_val("--module")) return 2;
            out.module_path.assign(args[++i]);
        } else if (a == "--config") {
            if (!need_val("--config")) return 2;
            out.config_path.assign(args[++i]);
        } else if (a == "--force") {
            out.force = true;
        } else if (a == "--help" || a == "-h") {
            (void)std::fputs(
                "usage: goodnetd identity import-hsm \\\n"
                "    --extension-id <id> --key-label <label> \\\n"
                "    [--pin-env <ENV>] [--module <path>] \\\n"
                "    [--config <file>] [--force]\n"
                "\n"
                "Writes identity-config.json with backend=provider so the\n"
                "daemon installs the identity through the named extension\n"
                "(typically gn.identity.pkcs11). On-token key provisioning\n"
                "is out of scope; use the vendor's tool (pkcs11-tool /\n"
                "softhsm2-util / ykman) first.\n",
                stdout);
            return 0;
        } else {
            (void)std::fprintf(stderr,
                "goodnetd identity import-hsm: unknown argument '%.*s'\n",
                static_cast<int>(a.size()), a.data());
            return 2;
        }
    }
    return -1;  // sentinel for "no early-exit, continue with validation"
}

[[nodiscard]] int validate(const Args& a) {
    if (a.extension_id.empty()) {
        (void)std::fputs(
            "goodnetd identity import-hsm: --extension-id is required\n",
            stderr);
        return 2;
    }
    if (a.key_label.empty()) {
        (void)std::fputs(
            "goodnetd identity import-hsm: --key-label is required\n",
            stderr);
        return 2;
    }
    return 0;
}

/// Atomic write: dump to `<path>.tmp` first, then `rename(2)` into
/// place. The rename is the POSIX atomic step — readers either see
/// the previous file or the new file, never a half-written one. We
/// do not fsync the directory: the daemon re-reads on every start, a
/// crash window large enough to drop the rename also drops the
/// kernel and the operator notices.
[[nodiscard]] int write_atomic(const std::filesystem::path& path,
                                const std::string&            content) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            (void)std::fprintf(stderr,
                "goodnetd identity import-hsm: cannot mkdir %s — %s\n",
                parent.string().c_str(), ec.message().c_str());
            return 1;
        }
    }
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            (void)std::fprintf(stderr,
                "goodnetd identity import-hsm: cannot open %s for write\n",
                tmp.c_str());
            return 1;
        }
        f << content;
        if (!f.good()) {
            (void)std::fprintf(stderr,
                "goodnetd identity import-hsm: write to %s failed\n",
                tmp.c_str());
            return 1;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        (void)std::fprintf(stderr,
            "goodnetd identity import-hsm: rename %s -> %s failed — %s\n",
            tmp.c_str(), path.string().c_str(), ec.message().c_str());
        std::error_code ignore;
        std::filesystem::remove(tmp, ignore);
        return 1;
    }
    return 0;
}

}  // namespace

int cmd_identity_import_hsm(std::span<const std::string_view> args) {
    Args a;
    if (const int rc = parse_args(args, a); rc >= 0) return rc;
    if (const int rc = validate(a); rc != 0) return rc;

    const auto cfg_path = a.config_path.empty()
        ? default_config_path()
        : std::filesystem::path{a.config_path};

    /// Clobber gate. The descriptor is small and easy to regenerate,
    /// but a silent overwrite is exactly the operator-surprise we are
    /// supposed to prevent: an existing file-backed setup that the
    /// next `goodnetd run` would otherwise quietly bypass needs an
    /// explicit `--force` from the operator's hand.
    std::error_code ec;
    if (std::filesystem::exists(cfg_path, ec) && !a.force) {
        std::ifstream f(cfg_path, std::ios::binary);
        if (f) {
            try {
                nlohmann::json existing;
                f >> existing;
                if (existing.is_object() && existing.contains("backend")) {
                    const auto cur = existing["backend"].is_string()
                        ? existing["backend"].get<std::string>()
                        : std::string{};
                    /// Same backend re-declaration with the same fields
                    /// is the operator re-running the command — accept
                    /// it without --force only when nothing changed.
                    /// Anything else (different backend, same backend
                    /// with different extension/label) must go through
                    /// --force so a typo doesn't wedge the daemon.
                    const bool same_ext =
                        existing.contains("extension_id") &&
                        existing["extension_id"].is_string() &&
                        existing["extension_id"].get<std::string>() ==
                            a.extension_id;
                    const bool same_label =
                        existing.contains("key_label") &&
                        existing["key_label"].is_string() &&
                        existing["key_label"].get<std::string>() ==
                            a.key_label;
                    if (!(cur == "provider" && same_ext && same_label)) {
                        (void)std::fprintf(stderr,
                            "goodnetd identity import-hsm: %s already "
                            "declares backend=%s; refuse to clobber. Pass "
                            "--force to overwrite.\n",
                            cfg_path.string().c_str(), cur.c_str());
                        return 1;
                    }
                }
            } catch (const std::exception&) {
                /// Existing file is not valid JSON — treat as garbage
                /// the operator wants replaced, but still demand
                /// --force so a misplaced unrelated file isn't lost.
                (void)std::fprintf(stderr,
                    "goodnetd identity import-hsm: %s exists but is not "
                    "valid JSON; pass --force to overwrite.\n",
                    cfg_path.string().c_str());
                return 1;
            }
        }
    }

    /// Compose the descriptor. `extra.*` is plugin-opaque; only keys
    /// the operator actually supplied land in the file so a future
    /// reader (doctor / run) does not have to distinguish "unset"
    /// from "empty".
    nlohmann::json extra = nlohmann::json::object();
    if (!a.pin_env.empty())     extra["pin_env"]     = a.pin_env;
    if (!a.module_path.empty()) extra["module_path"] = a.module_path;

    nlohmann::json doc = {
        {"backend",      "provider"},
        {"extension_id", a.extension_id},
        {"key_label",    a.key_label},
    };
    if (!extra.empty()) {
        doc["extra"] = std::move(extra);
    }

    const std::string serialised = doc.dump(2) + "\n";
    if (const int rc = write_atomic(cfg_path, serialised); rc != 0) return rc;

    (void)std::fprintf(stdout,
        "[ok] identity backend set: provider "
        "(extension_id=%s, key_label=%s)\n"
        "      wrote %s\n"
        "      next: goodnetd doctor (verify), goodnetd run (start)\n",
        a.extension_id.c_str(),
        a.key_label.c_str(),
        cfg_path.string().c_str());
    return 0;
}

}  // namespace gn::apps::goodnet
