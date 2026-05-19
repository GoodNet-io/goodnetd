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
            "Run: nix run goodnet#bootstrap-env",
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
            "Run: nix run goodnet#bootstrap-env",
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
            "Run: nix run goodnet#install-plugins",
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
            "Install forgejo-runner via nix run goodnet#bootstrap-env",
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
