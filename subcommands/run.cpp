/// @file   apps/goodnet/subcommands/run.cpp
/// @brief  `goodnet run --config X --manifest Y --identity Z` —
///         production node entry point.
///
/// Loads NodeIdentity from disk, parses the kernel config + plugin
/// manifest, constructs a Kernel with the gnet protocol layer, hands
/// it to a PluginManager to dlopen the link / security / handler
/// plugins listed in the manifest, then sits on SIGTERM / SIGINT.
/// On signal: PluginManager.shutdown() drains in-flight async work
/// before the process exits.
///
/// gnet protocol is registered via the public C ABI
/// `gn_gnet_register_protocol(gn_core_t*)` (sdk/gnet.h). An out-of-tree
/// protocol layer may be registered alongside it by the host program.
/// Operators with a custom protocol layer build their own runner.

#include "../subcommands.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <core/config/config.hpp>
#include <core/identity/identity_plugin_signer.hpp>
#include <core/identity/node_identity.hpp>
#include <core/kernel/host_api_builder.hpp>
#include <core/kernel/kernel.hpp>
#include <core/kernel/plugin_context.hpp>
#include <core/plugin/plugin_manager.hpp>
#include <core/plugin/plugin_manifest.hpp>
#include <core/registry/extension.hpp>
#include <core/util/log.hpp>
#include <core/util/log_config.hpp>

#include <sdk/extensions/identity.h>
#include <sdk/gnet.h>

namespace gn::apps::goodnet {

namespace {

/// Set on SIGTERM / SIGINT by the installed signal handler. The main
/// loop polls; on first set, the loop drops out and starts cleanup.
std::atomic<int> g_quit_signal{0};

extern "C" void run_signal_handler(int sig) {
    /// Set-once: a second signal during cleanup is a SIGKILL hint
    /// from systemd (TimeoutStopSec exceeded) — the kernel can't
    /// listen for it, but we don't overwrite the first sig number
    /// in case the operator wants to log which signal arrived first.
    int expected = 0;
    (void)g_quit_signal.compare_exchange_strong(expected, sig);
}

struct RunArgs {
    std::string config_path;
    std::string manifest_path;
    std::string identity_path;
};

[[nodiscard]] int parse_args(std::span<const std::string_view> args,
                              RunArgs& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        const auto need_val = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                (void)std::fprintf(stderr,
                    "goodnet run: %s requires an argument\n", flag);
                return false;
            }
            return true;
        };
        if (a == "--config") {
            if (!need_val("--config")) return 2;
            out.config_path.assign(args[++i]);
        } else if (a == "--manifest") {
            if (!need_val("--manifest")) return 2;
            out.manifest_path.assign(args[++i]);
        } else if (a == "--identity") {
            if (!need_val("--identity")) return 2;
            out.identity_path.assign(args[++i]);
        } else {
            (void)std::fprintf(stderr,
                "goodnet run: unknown argument '%.*s'\n",
                static_cast<int>(a.size()), a.data());
            return 2;
        }
    }
    if (out.config_path.empty() ||
        out.manifest_path.empty()) {
        (void)std::fputs(
            "goodnet run: requires --config FILE --manifest FILE "
            "[--identity FILE]\n",
            stderr);
        return 2;
    }
    /// `--identity` is required for the file-backed path and unused
    /// for the provider-backed path. We can't tell which path applies
    /// until after `load_identity_descriptor()` runs, so the
    /// file-vs-provider gate moves to the caller. Empty `identity_path`
    /// here means "operator omitted it"; the caller errors if the
    /// active backend turns out to be file-based.
    return 0;
}

[[nodiscard]] std::optional<std::string> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Phase-4 identity descriptor. Written by `goodnetd identity
/// import-hsm`; consumed here. Empty `extension_id` means the file is
/// missing or the backend is "file" — caller falls through to the
/// existing file-backed install path.
struct IdentityDescriptor {
    std::string backend;       ///< "file" | "provider"
    std::string extension_id;  ///< populated when backend == "provider"
    std::string key_label;     ///< populated when backend == "provider"
    /// Flat env-var map: each (k,v) is exported via `setenv(k,v,1)`
    /// before kernel init. The PKCS#11 plugin reads its module path /
    /// PIN through env vars; this keeps the kernel ABI plugin-agnostic.
    std::vector<std::pair<std::string, std::string>> extra_env;
};

[[nodiscard]] std::filesystem::path identity_descriptor_path() {
    /// Same XDG layout `doctor` / `quickstart` / `import-hsm` walk.
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
        return std::filesystem::path{x} / "goodnet" / "identity-config.json";
    }
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path{h} / ".local" / "share" / "goodnet"
               / "identity-config.json";
    }
    return {};
}

/// Read the descriptor if present. Absent file => returns a descriptor
/// with empty `backend` (caller treats as file-backed). Parse failure
/// is reported on stderr and surfaces as `nullopt` so the runner aborts
/// rather than silently dropping back to file-backed identity.
[[nodiscard]] std::optional<IdentityDescriptor> load_identity_descriptor() {
    IdentityDescriptor d;
    const auto path = identity_descriptor_path();
    if (path.empty()) {
        return d;  // no $HOME => no descriptor; legacy file path only
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return d;  // not provisioned; legacy file path
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        (void)std::fprintf(stderr,
            "goodnet run: identity descriptor %s — cannot open\n",
            path.string().c_str());
        return std::nullopt;
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& ex) {
        (void)std::fprintf(stderr,
            "goodnet run: identity descriptor %s — JSON parse: %s\n",
            path.string().c_str(), ex.what());
        return std::nullopt;
    }
    if (!doc.is_object()) {
        (void)std::fprintf(stderr,
            "goodnet run: identity descriptor %s — top-level must be object\n",
            path.string().c_str());
        return std::nullopt;
    }
    if (doc.contains("backend") && doc["backend"].is_string()) {
        d.backend = doc["backend"].get<std::string>();
    }
    if (d.backend == "provider") {
        if (doc.contains("extension_id") && doc["extension_id"].is_string()) {
            d.extension_id = doc["extension_id"].get<std::string>();
        }
        if (doc.contains("key_label") && doc["key_label"].is_string()) {
            d.key_label = doc["key_label"].get<std::string>();
        }
        if (d.extension_id.empty() || d.key_label.empty()) {
            (void)std::fprintf(stderr,
                "goodnet run: identity descriptor %s — provider backend "
                "needs non-empty extension_id and key_label\n",
                path.string().c_str());
            return std::nullopt;
        }
        if (doc.contains("extra") && doc["extra"].is_object()) {
            /// Mapping convention: each `extra.<key>` whose value is a
            /// string becomes a setenv(<KEY_UPPER>, <value>). The keys
            /// in the descriptor are documented (`pin_env`,
            /// `module_path`), but we don't whitelist — a plugin may
            /// grow more keys without a goodnetd rebuild.
            for (const auto& [k, v] : doc["extra"].items()) {
                if (!v.is_string()) continue;
                /// `pin_env` is special: its value is the NAME of the
                /// env var the operator wants the plugin to read.
                /// goodnetd does NOT inject the PIN itself (that would
                /// require the daemon to know the secret); the
                /// operator's systemd unit `LoadCredential=` or a
                /// shell wrapper exports the actual PIN under whatever
                /// env name `pin_env` declares. We carry the NAME
                /// through to the plugin under
                /// `GOODNET_PKCS11_PIN_ENV` so the plugin knows which
                /// env var to read.
                std::string env_key;
                env_key.reserve(k.size() + 8);
                env_key.append("GOODNET_");
                for (const char c : k) {
                    env_key.push_back(
                        (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c);
                }
                d.extra_env.emplace_back(std::move(env_key),
                                          v.get<std::string>());
            }
        }
    }
    return d;
}

/// Provider install path — replicates the work
/// `gn_core_install_identity_from_provider` does on the C ABI but
/// against the C++ `Kernel` the runner constructs directly. The C ABI
/// version takes a `gn_core_t*` which wraps `gn::core::Kernel`; we
/// reach the same primitives (extension registry + `NodeIdentity::
/// from_signer`) on the kernel we already own. Returns 0 on success;
/// non-zero exit code on failure with diagnostics on stderr.
[[nodiscard]] int install_provider_identity(
    gn::core::Kernel&        kernel,
    const std::string&       extension_id,
    const std::string&       key_label) {

    const void* vtable_raw = nullptr;
    const auto query_rc = kernel.extensions().query_extension_checked(
        extension_id, GN_EXT_IDENTITY_SIGNER_VERSION, &vtable_raw);
    if (query_rc != GN_OK || vtable_raw == nullptr) {
        (void)std::fprintf(stderr,
            "goodnet run: identity provider extension '%s' not found "
            "(kernel rc=%d). Verify the manifest lists the signer plugin "
            "and the plugin registers '%s' under the identity-signer "
            "version.\n",
            extension_id.c_str(), int(query_rc), extension_id.c_str());
        return 1;
    }
    const auto* vtable =
        static_cast<const gn_identity_signer_vtable_t*>(vtable_raw);
    /// Minimum api_size: producer must extend at least through the
    /// `sign` slot. Mirrors the gate `gn_core_install_identity_from_
    /// provider` applies on the C ABI.
    constexpr std::size_t required_api_size =
        offsetof(gn_identity_signer_vtable_t, sign) +
        sizeof(static_cast<gn_identity_signer_vtable_t*>(nullptr)->sign);
    if (vtable->api_size < required_api_size) {
        (void)std::fprintf(stderr,
            "goodnet run: identity provider '%s' vtable too small "
            "(api_size=%zu, required>=%zu)\n",
            extension_id.c_str(),
            static_cast<std::size_t>(vtable->api_size),
            required_api_size);
        return 1;
    }
    void* const ctx =
        const_cast<void*>(static_cast<const void*>(vtable));
    auto signer = std::make_unique<gn::core::identity::IdentityPluginSigner>(
        vtable, ctx, key_label);
    auto identity = gn::core::identity::NodeIdentity::from_signer(
        std::move(signer), /*expiry*/ 0);
    if (!identity) {
        (void)std::fprintf(stderr,
            "goodnet run: identity provider '%s' attestation install "
            "failed — %s\n",
            extension_id.c_str(),
            identity.error().what.empty()
                ? "unknown"
                : identity.error().what.c_str());
        return 1;
    }
    kernel.identities().add(identity->device().public_key());
    kernel.set_node_identity(std::move(*identity));
    return 0;
}

}  // namespace

int cmd_run(std::span<const std::string_view> args) {
    RunArgs ra;
    if (const int rc = parse_args(args, ra); rc != 0) return rc;

    /// Phase-4 identity descriptor. When `identity-config.json` lives
    /// at the XDG path and declares `backend = "provider"`, the
    /// runner defers identity install until AFTER plugins load so the
    /// signer extension is queryable. The file-backed default
    /// (descriptor absent / `backend = "file"`) preserves today's
    /// pre-plugin install order.
    auto descriptor = load_identity_descriptor();
    if (!descriptor) {
        return 1;  // diagnostics already printed
    }
    const bool provider_backend = (descriptor->backend == "provider");

    /// Export `extra.*` env vars BEFORE any plugin loads — the
    /// PKCS#11 plugin reads its module path / PIN from env on
    /// `gn_plugin_init`. We do this even when the operator passed
    /// `--identity` (the file-backed path also tolerates harmless
    /// extra env). The descriptor's `pin_env` value is the NAME of
    /// the env var that carries the actual PIN; goodnetd never
    /// touches the PIN itself.
    if (provider_backend) {
        for (const auto& [k, v] : descriptor->extra_env) {
            /// `setenv(name, value, /*overwrite=*/1)`. The descriptor
            /// is the source of truth at boot — a stale value from a
            /// previous run shouldn't shadow what the operator
            /// re-imported today.
            (void)::setenv(k.c_str(), v.c_str(), 1);
        }
    }

    /// File-backed identity: load now so a missing or malformed file
    /// fails fast before we touch the kernel. Provider-backed: skip
    /// the load — identity install happens after plugins register
    /// the signer extension.
    std::optional<gn::core::identity::NodeIdentity> identity;
    if (!provider_backend) {
        if (ra.identity_path.empty()) {
            (void)std::fputs(
                "goodnet run: file-backed identity requires --identity FILE "
                "(or run `goodnetd identity import-hsm` for provider-backed)\n",
                stderr);
            return 2;
        }
        auto loaded = gn::core::identity::NodeIdentity::load_from_file(
            ra.identity_path);
        if (!loaded) {
            (void)std::fprintf(stderr,
                "goodnet run: identity %s — %s\n",
                ra.identity_path.c_str(),
                loaded.error().what.empty()
                    ? "load failed"
                    : loaded.error().what.c_str());
            return 1;
        }
        identity.emplace(std::move(*loaded));
    }

    /// Config: parse + validate. Same path as
    /// `goodnet config validate`, but here a failure exits the
    /// runner rather than reporting OK.
    gn::core::Config cfg;
    {
        std::string reason;
        if (cfg.load_file(ra.config_path, &reason) != GN_OK) {
            (void)std::fprintf(stderr,
                "goodnet run: config %s — %s\n",
                ra.config_path.c_str(), reason.c_str());
            return 1;
        }
    }

    /// Manifest: trust root for plugin loads. Read the file bytes,
    /// hand them to `PluginManifest::parse`. PluginManager is
    /// configured with `set_manifest_required=true` so an absent
    /// manifest entry refuses dlopen with `GN_ERR_INTEGRITY_FAILED`.
    gn::core::PluginManifest manifest;
    {
        const auto bytes = read_file(ra.manifest_path);
        if (!bytes) {
            (void)std::fprintf(stderr,
                "goodnet run: manifest %s — cannot open\n",
                ra.manifest_path.c_str());
            return 1;
        }
        std::string diag;
        if (gn::core::PluginManifest::parse(*bytes, manifest, diag) != GN_OK) {
            (void)std::fprintf(stderr,
                "goodnet run: manifest %s — %s\n",
                ra.manifest_path.c_str(), diag.c_str());
            return 1;
        }
    }

    /// Kernel up. Order:
    /// 1. Apply config-derived limits (protocol layer reads them on
    ///    every frame, so they must be in place before the layer
    ///    sees traffic).
    /// 2. Set the gnet protocol layer.
    /// 3. Install identity (security pipeline reads it on every
    ///    `notify_connect`).
    /// 4. apply_log_config — reroutes spdlog with the config's
    ///    format/level.
    using gn::core::Kernel;
    using gn::core::PluginContext;
    using gn::core::PluginManager;
    using gn::core::build_host_api;

    /// Apply the operator's log shape from the loaded config keys
    /// before constructing the kernel — the kernel's ctor already
    /// emits its own INFO marker, and a Release default lands at the
    /// WARN console floor unless we lift it here. `goodnet run` is
    /// operator-facing; default `log.console_level = "info"` so
    /// kernel startup surfaces on `systemctl status` instead of
    /// being filtered by the build-aware default.
    auto lc = gn::core::util::load_log_config(cfg);
    if (lc.console_level.empty()) {
        lc.console_level = "info";
    }
    (void)gn::log::init_with(lc);

    Kernel kernel;
    kernel.set_limits(cfg.limits());
    if (const gn_result_t rc = gn_gnet_register_protocol(
            reinterpret_cast<gn_core_t*>(&kernel));
        rc != GN_OK) {
        (void)std::fprintf(stderr,
            "goodnet run: gnet protocol registration failed (rc=%d)\n",
            static_cast<int>(rc));
        return 1;
    }
    /// File-backed: install identity now (the security pipeline reads
    /// it on every `notify_connect`, so before-plugins is the right
    /// spot). Provider-backed: skip — plugins must load first so the
    /// signer extension is queryable, and we install after the
    /// `PluginManager::load` block below.
    if (!provider_backend) {
        kernel.identities().add(identity->device().public_key());
        kernel.set_node_identity(std::move(*identity));
    }

    /// Host context for the runner itself — no plugin anchor (the
    /// runner is not a loaded plugin), `kind = LINK` so the runner
    /// can hand `host_api` to embedded fixtures if one ever ships.
    /// Loaded plugins build their own contexts inside
    /// `PluginManager::load`.
    PluginContext host_ctx;
    host_ctx.plugin_name = "goodnet-runner";
    host_ctx.kernel      = &kernel;
    (void)build_host_api(host_ctx);  // initialises kernel-side state

    PluginManager plugins{kernel};
    plugins.set_manifest(std::move(manifest));
    plugins.set_manifest_required(true);

#ifdef GOODNET_STATIC_PLUGINS
    /// Static-linkage build: every plugin's entry symbols ship in the
    /// kernel binary itself (suffix-renamed per `sdk/plugin.h` macros)
    /// and the registry array `gn_plugin_static_registry[]` holds the
    /// addresses. Skip the manifest path entirely — there are no .so
    /// files to verify. `load_static()` mirrors `load()`'s two-phase
    /// init+register with rollback on any failure.
    std::size_t static_count = 0;
    {
        std::string diag;
        if (plugins.load_static(&diag) != GN_OK) {
            (void)std::fprintf(stderr,
                "goodnet run: static plugin load failed — %s\n",
                diag.c_str());
            return 1;
        }
        static_count = plugins.size();
    }
    const std::size_t loaded_plugin_count = static_count;
#else
    /// Pull the manifest's path list into a flat `std::vector<std::string>`
    /// for `PluginManager::load`. The manager re-hashes each path as
    /// part of the integrity gate, so we don't pre-verify here.
    std::vector<std::string> plugin_paths;
    for (const auto& entry : plugins.manifest().entries()) {
        plugin_paths.emplace_back(entry.path);
    }
    {
        std::string diag;
        if (plugins.load(std::span<const std::string>(plugin_paths),
                          &diag) != GN_OK) {
            (void)std::fprintf(stderr,
                "goodnet run: plugin load failed — %s\n", diag.c_str());
            return 1;
        }
    }
    const std::size_t loaded_plugin_count = plugin_paths.size();
#endif

    /// Provider-backed identity install runs HERE — after plugins
    /// register their extensions, before the kernel takes on session
    /// traffic. A descriptor that points at an extension no plugin
    /// registered surfaces as `GN_ERR_NOT_FOUND` from the inline
    /// helper, which is the precise diagnostic the operator wants
    /// (the manifest is missing the signer plugin).
    if (provider_backend) {
        if (const int rc = install_provider_identity(
                kernel, descriptor->extension_id, descriptor->key_label);
            rc != 0) {
            plugins.shutdown();
            return rc;
        }
    }

    /// Signal handlers go in AFTER plugins load — until then a
    /// SIGTERM should kill the process immediately rather than walk
    /// a half-loaded shutdown path.
    /// `std::signal` returns the previous handler; we don't need it
    /// (no chained handler to restore on graceful exit since we
    /// intentionally drain on SIGTERM/SIGINT).
    (void)std::signal(SIGINT,  &run_signal_handler);
    (void)std::signal(SIGTERM, &run_signal_handler);

    /// Operator-facing startup marker. Routed through the kernel
    /// logger so `log.level` / `log.console_level` / `log.file`
    /// shape it the same way as kernel and plugin lines. systemd
    /// users set `log.console_level = "info"` to keep this line
    /// visible on a Release deployment that otherwise pins WARN.
    GN_LOG_INFO("goodnet run: kernel up, {} plugins loaded, awaiting signal",
                loaded_plugin_count);

    /// Main loop: poll the quit flag every 100ms. Active work runs
    /// on plugin worker threads (asio io_context per transport,
    /// service executor per timer). The runner thread does nothing
    /// but wait — sleeping is correct; spinning would waste cycles.
    while (g_quit_signal.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    GN_LOG_INFO("goodnet run: signal {} received, draining plugins",
                g_quit_signal.load());

    /// Ordered teardown: PluginManager.shutdown() walks every loaded
    /// plugin's `gn_plugin_unregister` + `gn_plugin_shutdown` and
    /// waits on each anchor's weak_ptr to drop before `dlclose`.
    plugins.shutdown();

    GN_LOG_INFO("goodnet run: clean exit");
    return 0;
}

}  // namespace gn::apps::goodnet
