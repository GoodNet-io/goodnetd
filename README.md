# goodnetd

BusyBox-style multicall CLI for the GoodNet kernel — single binary
covers every operator-facing subcommand. The daemon's name follows
the Linux convention (`systemd`, `dockerd`, `sshd`): `goodnetd run`
is the long-running process; the other subcommands are
one-shot operator utilities.

## Quickstart

```sh
# 1. Install daemon + all plugins
nix profile add github:GoodNet-io/goodnetd#full

# 2. First-time wizard: generates identity, detects plugins,
#    writes manifest + config (ws://0.0.0.0:9100 by default)
goodnetd quickstart

# 3. Run
goodnetd run \
  --config   ~/.local/share/goodnet/config.json \
  --manifest ~/.local/share/goodnet/manifests/baseline.json \
  --identity ~/.local/share/goodnet/identity/default.bin

# 4. Verify
goodnetd doctor
```

Re-running `quickstart` after installing plugins is idempotent —
it skips steps already done and generates the manifest from whatever
`.so` files it finds under `~/.nix-profile/lib/goodnet/plugins/`.

## Subcommands

- `goodnetd run --config X --manifest Y [--identity Z]` — long-running
  daemon: loads config + manifest, starts kernel, loads plugins, serves
  until SIGTERM. `--identity` is required for file-backed identity;
  omit it when using an HSM/provider backend (see `identity import-hsm`).
- `goodnetd version` — prints goodnetd and kernel version strings.
- `goodnetd config validate <file>` — checks a config file for structural
  validity.
- `goodnetd plugin hash <so>` — SHA-256 of a plugin `.so` for pinning
  in manifests.
- `goodnetd manifest gen <so>...` — computes SHA-256 for each listed
  `.so` and emits a `plugins.json` manifest to stdout. Redirect into a
  file: `goodnetd manifest gen ~/.nix-profile/lib/goodnet/plugins/*.so > baseline.json`
- `goodnetd identity gen --out <file> [--expiry N]` — generates a fresh
  node identity and writes it to `<file>` (mode 0600). `--out` is
  required.
- `goodnetd identity show <file>` — prints the public surface (address,
  user_pk, device_pk) of a saved identity file.
- `goodnetd identity import-hsm` — binds a token-resident Ed25519
  keypair (PKCS#11) to the local node identity without materialising
  the private key on disk. Flags:
  - `--extension-id <id>` (required) — plugin extension ID for the signer
  - `--key-label <label>` (required) — PKCS#11 `CKA_LABEL` on the token
  - `--module <path>` — path to the PKCS#11 module `.so`
  - `--pin-env <VAR>` — name of the env var that carries the token PIN at runtime
  - `--config <file>` — write descriptor to a custom path
  - `--force` — overwrite an existing descriptor
- `goodnetd doctor [--json]` — pre-flight checks: identity, provider
  extension reachability, plugin manifest digests, config schema, loader,
  control socket. `--json` emits a machine-readable array for CI.
- `goodnetd quickstart [--non-interactive]` — first-time setup wizard:
  generates identity, detects installed plugins, writes manifest and
  `config.json`. Pass `--non-interactive` for scripted image builds.
  In interactive mode, offers an HSM backend option (PKCS#11).

## Config

`config.json` is a JSON object. Quickstart writes it to
`~/.local/share/goodnet/config.json`. Minimal example:

```json
{
  "listeners": [
    { "uri": "ws://0.0.0.0:9100" },
    { "uri": "tcp://0.0.0.0:9101" }
  ]
}
```

## Build

```sh
nix build .#          # daemon only
nix build .#full      # daemon + all plugins
nix build .#static    # statically linked (Linux only)
```

Dev shell with local kernel overrides:

```sh
nix run .#setup       # clone deps into .goodnet/, install git hooks
nix develop \
  --override-input goodnet       path:.goodnet/goodnet \
  --override-input protocol-gnet path:.goodnet/protocol-gnet
cmake -G Ninja -B build . && cmake --build build
```

## License

MIT — see `LICENSE`. Links against the GoodNet kernel through the
stable C ABI declared in the kernel's SDK headers; not a derivative
work per the kernel's Linking Exception.
