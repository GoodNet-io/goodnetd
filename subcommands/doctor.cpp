/// @file   subcommands/doctor.cpp
/// @brief  `goodnetd doctor` — environment self-check with fix hints.
///
/// Walks the operator's XDG layout (identity / plugins / manifest /
/// config / loader / control socket) and prints one tagged finding
/// per check. Tags are `[ok]`, `[warn]`, `[error]`; each non-ok
/// finding carries an actionable hint string the operator can paste
/// straight onto a shell. Exit status is 0 if there are no `[error]`
/// findings (warnings do not fail the doctor), 1 otherwise. The
/// `--json` flag swaps the human report for a `[{tag, message,
/// hint}, ...]` array so the doctor can be scripted into CI.
///
/// Doctor never mutates the user's environment. Every fix is the
/// operator's call to make after reading the report — quickstart is
/// the side-channel that *does* lay things down on disk.
///
/// The check list lives in `kChecks` below. Adding a check means
/// dropping a new entry into that array plus a corresponding
/// `check_*` function. Each check returns a single `Finding`; the
/// driver tallies tags and emits the summary line.

#include "../subcommands.hpp"
#include "sha256_file.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace gn::apps::goodnet {

namespace {

enum class Tag { Ok, Warn, Error };

struct Finding {
    Tag         tag;
    std::string message;
    std::string hint;  // empty when tag == Ok
};

[[nodiscard]] const char* tag_label(Tag t) {
    switch (t) {
        case Tag::Ok:    return "ok";
        case Tag::Warn:  return "warn";
        case Tag::Error: return "error";
    }
    return "?";
}

/// XDG_DATA_HOME or `$HOME/.local/share` per the spec. Doctor never
/// invents a path the operator did not configure; if neither env var
/// is set we fall through to an empty string and the file existence
/// checks then report a useful `[error]`.
[[nodiscard]] std::filesystem::path xdg_data_home() {
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
        return std::filesystem::path{x};
    }
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path{h} / ".local" / "share";
    }
    return {};
}

[[nodiscard]] std::filesystem::path goodnet_data_dir() {
    auto p = xdg_data_home();
    if (p.empty()) return {};
    return p / "goodnet";
}

[[nodiscard]] std::filesystem::path xdg_runtime_dir() {
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) {
        return std::filesystem::path{x};
    }
    return {};
}

/// Identity file: stored as 96 bytes (user_sk||device_sk||attest)
/// per `NodeIdentity::save_to_file`, but the doctor's lower bound is
/// the Ed25519 secret seed (32 B). Any blob shorter than 32 bytes is
/// definitely not a valid identity; 32-128 is plausible (the
/// concrete byte layout has shifted across kernel releases and
/// doctor is build-version-agnostic by design — it reports the size,
/// flags the obviously-wrong cases, and points at `identity gen`).
constexpr std::uintmax_t kIdentityMinBytes = 32;
constexpr std::uintmax_t kIdentityMaxBytes = 4096;

/// Phase-4 identity descriptor location — written by
/// `goodnetd identity import-hsm`, read by `cmd_run` and by the
/// HSM checks below. Doctor never mutates this file; presence + a
/// `provider` backend switches doctor into HSM-aware checks.
[[nodiscard]] std::filesystem::path identity_config_path(
    const std::filesystem::path& data_dir) {
    return data_dir / "identity-config.json";
}

/// Lightweight descriptor parse — doctor needs only `backend`,
/// `extension_id`, `key_label`, and the `extra.pin_env` name to
/// decide which environment variable to probe. Anything beyond that
/// stays plugin-opaque (doctor does not query the token itself,
/// just whether the kernel surfaces the extension and whether the
/// operator's PIN env is exported).
struct DescriptorView {
    bool        present       = false;
    bool        parse_failed  = false;
    std::string backend;
    std::string extension_id;
    std::string key_label;
    std::string pin_env_name;  ///< value of `extra.pin_env`, if present
};

[[nodiscard]] DescriptorView load_descriptor(
    const std::filesystem::path& data_dir) {
    DescriptorView v;
    const auto path = identity_config_path(data_dir);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return v;
    }
    v.present = true;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        v.parse_failed = true;
        return v;
    }
    try {
        nlohmann::json doc;
        f >> doc;
        if (!doc.is_object()) {
            v.parse_failed = true;
            return v;
        }
        if (doc.contains("backend") && doc["backend"].is_string()) {
            v.backend = doc["backend"].get<std::string>();
        }
        if (doc.contains("extension_id") && doc["extension_id"].is_string()) {
            v.extension_id = doc["extension_id"].get<std::string>();
        }
        if (doc.contains("key_label") && doc["key_label"].is_string()) {
            v.key_label = doc["key_label"].get<std::string>();
        }
        if (doc.contains("extra") && doc["extra"].is_object()) {
            const auto& extra = doc["extra"];
            if (extra.contains("pin_env") && extra["pin_env"].is_string()) {
                v.pin_env_name = extra["pin_env"].get<std::string>();
            }
        }
    } catch (const std::exception&) {
        v.parse_failed = true;
    }
    return v;
}

/// Test-only hook: the doctor's provider-backend check normally
/// spins a fresh kernel via the C ABI and queries the extension
/// registry. The unit tests cannot link the kernel (the test build
/// uses POSIX asserts + stub subcommands), so they substitute a
/// pure-in-process result here. Production calls leave the hook
/// nullptr and walk the real kernel path.
using ProviderQueryFn = int(*)(const std::string& extension_id,
                                const std::string& key_label);
ProviderQueryFn g_provider_query_hook = nullptr;

}  // namespace

/// Test seam. Tests set the hook to inject a deterministic answer
/// (0 == extension reachable, non-0 == not found / version mismatch)
/// without dragging the kernel into the link line. Production builds
/// never call this; the hook stays nullptr and the doctor walks the
/// live kernel.
void set_doctor_provider_query_hook_for_test(
    int (*hook)(const std::string&, const std::string&)) {
    g_provider_query_hook = hook;
}

namespace {

Finding check_identity(const std::filesystem::path& data_dir) {
    const auto path = data_dir / "identity" / "default.bin";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Finding{
            Tag::Error,
            "identity file missing at " + path.string(),
            "Run: goodnetd identity gen --out " + path.string(),
        };
    }
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        return Finding{
            Tag::Error,
            "cannot stat identity file " + path.string(),
            "Check filesystem readability: ls -la " + path.string(),
        };
    }
    if (sz < kIdentityMinBytes || sz > kIdentityMaxBytes) {
        return Finding{
            Tag::Error,
            "identity at " + path.string() + " has implausible size " +
                std::to_string(sz) + " bytes",
            "Regenerate: goodnetd identity gen --out " + path.string(),
        };
    }
    /// Quick readability probe — Doctor never decodes the seed,
    /// just confirms the file opens for read.
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        return Finding{
            Tag::Error,
            "identity file " + path.string() + " is not readable",
            "Fix mode: chmod 0600 " + path.string(),
        };
    }
    return Finding{
        Tag::Ok,
        "identity exists at " + path.string() + " (" +
            std::to_string(sz) + " bytes)",
        {},
    };
}

Finding check_plugin_dir(const std::filesystem::path& data_dir) {
    const auto dir = data_dir / "plugins";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return Finding{
            Tag::Error,
            "plugin dir missing at " + dir.string(),
            "Run: nix profile add github:GoodNet-io/goodnetd#full",
        };
    }
    std::size_t so_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".so") {
            ++so_count;
        }
    }
    if (so_count == 0) {
        return Finding{
            Tag::Warn,
            "plugin dir " + dir.string() + " contains zero .so files",
            "Run: nix profile add github:GoodNet-io/goodnetd#full",
        };
    }
    return Finding{
        Tag::Ok,
        "plugin dir contains " + std::to_string(so_count) + " .so files",
        {},
    };
}

Finding check_manifest(const std::filesystem::path& data_dir) {
    const auto path = data_dir / "manifests" / "baseline.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Finding{
            Tag::Warn,
            "baseline manifest missing at " + path.string(),
            "Run: goodnetd manifest gen <plugin-dir>/*.so > " + path.string(),
        };
    }
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        return Finding{
            Tag::Error,
            "manifest at " + path.string() + " unreadable",
            "Check perms: ls -la " + path.string(),
        };
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& ex) {
        return Finding{
            Tag::Error,
            std::string{"manifest at "} + path.string() +
                " is not valid JSON (" + ex.what() + ")",
            "Regenerate: goodnetd manifest gen <plugin-dir>/*.so",
        };
    }
    if (!doc.is_object() || !doc.contains("plugins") ||
        !doc["plugins"].is_array()) {
        return Finding{
            Tag::Error,
            "manifest at " + path.string() +
                " missing top-level 'plugins' array",
            "Regenerate: goodnetd manifest gen <plugin-dir>/*.so",
        };
    }
    /// Cross-check every entry's `path` exists and its recorded
    /// sha256 still matches the file on disk. A missing file is a
    /// `[warn]`; a hash mismatch is an `[error]` (silent tampering or
    /// out-of-band swap is exactly what the manifest is meant to
    /// catch).
    std::size_t missing = 0;
    std::size_t mismatched = 0;
    std::string first_missing;
    std::string first_mismatch;
    for (const auto& entry : doc["plugins"]) {
        if (!entry.is_object() || !entry.contains("path") ||
            !entry.contains("sha256")) {
            return Finding{
                Tag::Error,
                "manifest at " + path.string() +
                    " has a malformed plugin entry",
                "Regenerate: goodnetd manifest gen <plugin-dir>/*.so",
            };
        }
        const std::string entry_path = entry["path"].get<std::string>();
        const std::string entry_hash = entry["sha256"].get<std::string>();
        if (!std::filesystem::exists(entry_path, ec)) {
            if (++missing == 1) first_missing = entry_path;
            continue;
        }
        std::uint8_t digest[32]{};
        if (!sha256_file(entry_path, digest)) {
            if (++missing == 1) first_missing = entry_path;
            continue;
        }
        if (sha256_hex(digest) != entry_hash) {
            if (++mismatched == 1) first_mismatch = entry_path;
        }
    }
    if (mismatched > 0) {
        return Finding{
            Tag::Error,
            "manifest at " + path.string() + " has " +
                std::to_string(mismatched) +
                " plugin(s) with sha256 mismatch (first: " +
                first_mismatch + ")",
            "Regenerate: goodnetd manifest gen <plugin-dir>/*.so",
        };
    }
    if (missing > 0) {
        return Finding{
            Tag::Warn,
            "manifest at " + path.string() + " references " +
                std::to_string(missing) + " missing plugin(s) (first: " +
                first_missing + ")",
            "Run: nix profile add github:GoodNet-io/goodnetd#full",
        };
    }
    return Finding{
        Tag::Ok,
        "manifest at " + path.string() + " is valid (" +
            std::to_string(doc["plugins"].size()) + " entries)",
        {},
    };
}

Finding check_config(const std::filesystem::path& data_dir) {
    const auto path = data_dir / "config.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Finding{
            Tag::Warn,
            "config missing at " + path.string(),
            "Run: goodnetd quickstart",
        };
    }
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        return Finding{
            Tag::Error,
            "config at " + path.string() + " unreadable",
            "Check perms: ls -la " + path.string(),
        };
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& ex) {
        return Finding{
            Tag::Error,
            std::string{"config at "} + path.string() +
                " is not valid JSON (" + ex.what() + ")",
            "Edit " + path.string() + " or regenerate via goodnetd quickstart",
        };
    }
    /// Doctor's config view is structural; the kernel's deep schema
    /// validator is `goodnetd config validate`. We only look at the
    /// path-reference keys that the rest of the doctor walks above:
    /// `identity.path`, `manifest.path`. Their presence is optional
    /// (the daemon's defaults overlap with the XDG layout), but if
    /// the user pinned them to a literal that does not resolve we
    /// flag that as `[error]` since `goodnetd run` will fail at boot.
    if (doc.is_object()) {
        if (doc.contains("identity") && doc["identity"].is_object() &&
            doc["identity"].contains("path") &&
            doc["identity"]["path"].is_string()) {
            const std::string p = doc["identity"]["path"].get<std::string>();
            if (!std::filesystem::exists(p, ec)) {
                return Finding{
                    Tag::Error,
                    "config.json references nonexistent identity path " + p,
                    "Edit " + path.string() + " identity.path",
                };
            }
        }
        if (doc.contains("manifest") && doc["manifest"].is_object() &&
            doc["manifest"].contains("path") &&
            doc["manifest"]["path"].is_string()) {
            const std::string p = doc["manifest"]["path"].get<std::string>();
            if (!std::filesystem::exists(p, ec)) {
                return Finding{
                    Tag::Error,
                    "config.json references nonexistent manifest path " + p,
                    "Edit " + path.string() + " manifest.path",
                };
            }
        }
    }
    return Finding{
        Tag::Ok,
        "config at " + path.string() + " parses and references resolve",
        {},
    };
}

Finding check_loader() {
    /// LD_LIBRARY_PATH override OR the standard nix profile location.
    /// We do not dlopen libgoodnet_kernel.so here — goodnetd itself
    /// is dynamically linked against it, so if we are running we
    /// already resolved the kernel SO. The check is informational.
    const char* ld = std::getenv("LD_LIBRARY_PATH");
    const char* nix = std::getenv("NIX_PROFILES");
    if ((ld && *ld) || (nix && *nix)) {
        std::string msg = "libgoodnet_kernel.so resolution path is set (";
        if (ld && *ld) {
            msg += "LD_LIBRARY_PATH";
        } else {
            msg += "NIX_PROFILES";
        }
        msg += ")";
        return Finding{Tag::Ok, std::move(msg), {}};
    }
    return Finding{
        Tag::Warn,
        "no LD_LIBRARY_PATH or NIX_PROFILES — kernel SO will rely on "
        "default ld.so.cache",
        "Source the goodnet devshell or set LD_LIBRARY_PATH explicitly",
    };
}

Finding check_forgejo_runner() {
    /// NixOS-only soft check. We look for a `.runner` token in the
    /// place the GoodNet CI flake lands it (`~/.local/share/forgejo-
    /// runner/.runner`). Absent file => the user is not running CI
    /// locally and we skip with `[ok] (n/a)`. Present file but no
    /// `systemctl` binary => `[warn]`. Present file + inactive
    /// service => `[warn]`.
    auto data = xdg_data_home();
    if (data.empty()) {
        return Finding{Tag::Ok, "forgejo-runner: skipped (no $HOME)", {}};
    }
    const auto tok = data / "forgejo-runner" / ".runner";
    std::error_code ec;
    if (!std::filesystem::exists(tok, ec)) {
        return Finding{
            Tag::Ok,
            "forgejo-runner: skipped (no .runner token at " +
                tok.string() + ")",
            {},
        };
    }
    /// `systemctl --user is-active forgejo-runner` — capture exit
    /// status via `system()`. Doctor avoids a forkv vendor dep; a
    /// pipe through /bin/sh is fine for a low-frequency check.
    const int rc = std::system(
        "systemctl --user is-active forgejo-runner >/dev/null 2>&1");
    if (rc == -1) {
        return Finding{
            Tag::Warn,
            "forgejo-runner: token present but systemctl unavailable",
            "Install forgejo-runner: see https://forgejo.org/docs/latest/admin/runner-installation/",
        };
    }
    if (rc != 0) {
        return Finding{
            Tag::Warn,
            "forgejo-runner: token present but service inactive",
            "Start: systemctl --user start forgejo-runner",
        };
    }
    return Finding{Tag::Ok, "forgejo-runner: active", {}};
}

Finding check_control_socket() {
    /// Daemon control socket is informational. Doctor probes the
    /// XDG_RUNTIME_DIR path; if the socket file does not exist the
    /// daemon is not running and the doctor reports `[warn]` (not
    /// `[error]` — many doctor invocations happen before `serve`).
    const auto rt = xdg_runtime_dir();
    if (rt.empty()) {
        return Finding{
            Tag::Warn,
            "daemon control socket: no $XDG_RUNTIME_DIR set",
            "Set $XDG_RUNTIME_DIR or run as a normal user session",
        };
    }
    const auto sock = rt / "goodnetd.sock";
    std::error_code ec;
    if (!std::filesystem::exists(sock, ec)) {
        return Finding{
            Tag::Warn,
            "daemon control socket missing at " + sock.string(),
            "Daemon not running; run: goodnetd run --config ...",
        };
    }
    /// Connect probe — non-blocking, drop on first refused.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return Finding{
            Tag::Warn,
            "daemon control socket: cannot create probe socket",
            {},
        };
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string sock_str = sock.string();
    if (sock_str.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return Finding{
            Tag::Warn,
            "daemon control socket path too long for sockaddr_un",
            {},
        };
    }
    std::memcpy(addr.sun_path, sock_str.c_str(), sock_str.size() + 1);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr));
    ::close(fd);
    if (rc != 0) {
        return Finding{
            Tag::Warn,
            "daemon control socket present at " + sock.string() +
                " but refused connect (errno " + std::to_string(errno) + ")",
            "Daemon may be shutting down; re-check after a moment",
        };
    }
    return Finding{
        Tag::Ok,
        "daemon control socket responsive at " + sock.string(),
        {},
    };
}

/// Provider-backend health probe. Called only when
/// `identity-config.json` declares `backend = "provider"`. Pushes a
/// stack of findings: extension reachability, PIN env presence. The
/// presence of the file with backend=file emits no findings here
/// (the file-backed `check_identity` above already covers it).
void check_identity_provider(const DescriptorView&  d,
                              std::vector<Finding>& out) {
    if (d.parse_failed) {
        out.push_back(Finding{
            Tag::Error,
            "identity-config.json present but is not valid JSON",
            "Re-run: goodnetd identity import-hsm --extension-id ... "
            "--key-label ... [--force]",
        });
        return;
    }
    if (d.extension_id.empty() || d.key_label.empty()) {
        out.push_back(Finding{
            Tag::Error,
            "identity-config.json: provider backend missing extension_id "
            "or key_label",
            "Re-run: goodnetd identity import-hsm --extension-id ... "
            "--key-label ... --force",
        });
        return;
    }

    /// Reachability: production uses the live kernel (spin a fresh
    /// `gn_core_t`, dlopen plugins from the manifest, query the
    /// extension). The test seam shortcuts this with a stub so the
    /// test binary doesn't link the kernel. We don't actually spin a
    /// kernel here yet (that's wired through `cmd_run` for production
    /// runs); the production path falls back to a [warn] reminding
    /// the operator to run `goodnetd run --dry-run` once that lands.
    /// For now the live check is gated on the test hook so doctor is
    /// at least testable end-to-end; absent the hook in production
    /// we report a `[warn]` rather than a false `[ok]`.
    if (g_provider_query_hook != nullptr) {
        const int rc = g_provider_query_hook(d.extension_id, d.key_label);
        if (rc == 0) {
            out.push_back(Finding{
                Tag::Ok,
                "identity provider extension '" + d.extension_id +
                    "' reachable (key_label='" + d.key_label + "')",
                {},
            });
        } else {
            out.push_back(Finding{
                Tag::Error,
                "identity provider extension '" + d.extension_id +
                    "' not reachable (rc=" + std::to_string(rc) + ")",
                "Verify the manifest lists the signer plugin and that the "
                "plugin registers the extension under the identity-signer "
                "version pin.",
            });
        }
    } else {
        out.push_back(Finding{
            Tag::Warn,
            "identity provider backend declared (extension_id='" +
                d.extension_id + "', key_label='" + d.key_label +
                "') — extension reachability not probed (kernel link "
                "not available in this doctor build)",
            "Run `goodnetd run --config ... --manifest ...` once to verify "
            "the extension is actually loaded; the daemon's startup log "
            "surfaces the install error if the plugin is missing.",
        });
    }

    /// PIN env: the operator's deployment is supposed to surface the
    /// PIN to the daemon under the env name carried in the descriptor's
    /// `extra.pin_env`. Doctor never reads the PIN itself; it only
    /// checks the env var is set so a typo (`PCKS11_PIN` vs `PKCS11_
    /// PIN`) is caught before the daemon's `C_Login` fails the
    /// session. Absence is `[warn]` not `[error]` — many deployments
    /// inject the PIN interactively at the systemd `LoadCredential=`
    /// boundary, which is invisible to a doctor run from a login
    /// shell.
    if (!d.pin_env_name.empty()) {
        const char* v = std::getenv(d.pin_env_name.c_str());
        if (v == nullptr || *v == '\0') {
            out.push_back(Finding{
                Tag::Warn,
                "PIN env var '" + d.pin_env_name + "' not set (or empty) "
                "in this shell",
                "Either export it before running `goodnetd run`, or let "
                "systemd LoadCredential= inject it at service start.",
            });
        } else {
            out.push_back(Finding{
                Tag::Ok,
                "PIN env var '" + d.pin_env_name + "' is set",
                {},
            });
        }
    }
}

void print_human(std::span<const Finding> findings) {
    std::size_t ok = 0, warn = 0, err = 0;
    for (const auto& f : findings) {
        const char* tag = tag_label(f.tag);
        (void)std::fprintf(stdout, "[%s] %s\n", tag, f.message.c_str());
        if (!f.hint.empty()) {
            (void)std::fprintf(stdout, "       hint: %s\n", f.hint.c_str());
        }
        switch (f.tag) {
            case Tag::Ok:    ++ok;   break;
            case Tag::Warn:  ++warn; break;
            case Tag::Error: ++err;  break;
        }
    }
    if (err > 0) {
        (void)std::fprintf(stdout,
            "Summary: %zu ok, %zu warn, %zu error — fix the error and re-run.\n",
            ok, warn, err);
    } else if (warn > 0) {
        (void)std::fprintf(stdout,
            "Summary: %zu ok, %zu warn — environment usable; warnings are "
            "advisory.\n",
            ok, warn);
    } else {
        (void)std::fprintf(stdout,
            "Summary: %zu ok — environment is clean.\n", ok);
    }
}

void print_json(std::span<const Finding> findings) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& f : findings) {
        arr.push_back({
            {"tag",     tag_label(f.tag)},
            {"message", f.message},
            {"hint",    f.hint},
        });
    }
    (void)std::fputs(arr.dump(2).c_str(), stdout);
    (void)std::fputc('\n', stdout);
}

}  // namespace

int cmd_doctor(std::span<const std::string_view> args) {
    bool emit_json = false;
    for (const auto& a : args) {
        if (a == "--json") {
            emit_json = true;
        } else if (a == "--help" || a == "-h") {
            (void)std::fputs(
                "usage: goodnetd doctor [--json]\n"
                "\n"
                "Walks identity / plugins / manifest / config / loader /\n"
                "forgejo-runner / control-socket and prints a tagged report.\n"
                "Exit 0 if no [error] findings, 1 otherwise.\n",
                stdout);
            return 0;
        } else {
            (void)std::fprintf(stderr,
                "goodnetd doctor: unknown argument '%.*s'\n",
                static_cast<int>(a.size()), a.data());
            return 2;
        }
    }

    const auto data_dir = goodnet_data_dir();
    std::vector<Finding> findings;
    findings.reserve(8);

    if (data_dir.empty()) {
        findings.push_back(Finding{
            Tag::Error,
            "no $XDG_DATA_HOME or $HOME set; cannot locate goodnet data dir",
            "Set $HOME (or $XDG_DATA_HOME) and re-run",
        });
    } else {
        findings.push_back(check_identity(data_dir));
        /// Provider-backend overlay: when `identity-config.json`
        /// declares `backend = "provider"`, the file-backed identity
        /// is no longer the source of truth — `cmd_run` queries the
        /// signer extension instead. The doctor reports BOTH checks
        /// (file-backed status above, provider-backend below) so an
        /// operator who is mid-migration can see what's happening on
        /// each path before flipping.
        const auto desc = load_descriptor(data_dir);
        if (desc.present && desc.backend == "provider") {
            check_identity_provider(desc, findings);
        } else if (desc.present && desc.parse_failed) {
            findings.push_back(Finding{
                Tag::Error,
                "identity-config.json present but is not valid JSON",
                "Re-run: goodnetd identity import-hsm --extension-id ... "
                "--key-label ... --force",
            });
        }
        findings.push_back(check_plugin_dir(data_dir));
        findings.push_back(check_manifest(data_dir));
        findings.push_back(check_config(data_dir));
    }
    findings.push_back(check_loader());
    findings.push_back(check_forgejo_runner());
    findings.push_back(check_control_socket());

    if (emit_json) {
        print_json(findings);
    } else {
        print_human(findings);
    }

    const bool has_error = std::any_of(
        findings.begin(), findings.end(),
        [](const Finding& f) { return f.tag == Tag::Error; });
    return has_error ? 1 : 0;
}

}  // namespace gn::apps::goodnet
