# goodnetd

BusyBox-style multicall CLI for the GoodNet kernel — single binary
covers every operator-facing subcommand. The daemon's name follows
the Linux convention (`systemd`, `dockerd`, `sshd`): `goodnetd run`
is the long-running process; the other subcommands are
one-shot operator utilities.

## Subcommands

- `goodnetd run` — long-running daemon process (load config + manifest
  → start kernel → load plugins → serve until SIGTERM).
- `goodnetd version` — prints kernel + plugin versions.
- `goodnetd config validate` — checks a config file against the kernel's
  config schema.
- `goodnetd plugin hash <path>` — SHA-256 of a plugin's `.so` for
  pinning in manifests.
- `goodnetd manifest gen` — emits a manifest skeleton for a plugin
  directory.
- `goodnetd identity gen [--expiry N]` — generates a fresh node
  identity.
- `goodnetd identity show` — prints the loaded node identity.
- `goodnetd identity import-hsm` — Phase 4 of the identity refactor:
  binds a token-resident Ed25519 keypair (PKCS#11 label) to the local
  node identity without ever materialising the private half on disk.
- `goodnetd doctor` — pre-flight checks for a node install: identity
  file presence + readability, manifest digests vs. on-disk plugin
  hashes, identity-provider extension reachability, config schema
  validation. Designed to surface every common operator-side
  mis-configuration before `goodnetd run` is invoked.
- `goodnetd quickstart` — one-shot bootstrap for a fresh machine:
  generates `identity.bin`, emits a `plugins.json` manifest against
  the bundled `.so` set, writes a starter `node.json`, and points the
  operator at `goodnetd run` with the correct arguments. Supports the
  same `--hsm` option as `identity import-hsm` to skip generating a
  software identity when the operator is provisioning against a token.

## Build

```
nix build .#
./result/bin/goodnetd version
```

## License

MIT — see `LICENSE`. Links against the GoodNet kernel through the
stable C ABI declared in the kernel's SDK headers; not a derivative
work per the kernel's Linking Exception.
