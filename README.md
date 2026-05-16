# goodnetd

BusyBox-style multicall CLI for the GoodNet kernel — single binary
covers every operator-facing subcommand. The daemon's name follows
the Linux convention (`systemd`, `dockerd`, `sshd`): `goodnetd run`
is the long-running process; the other subcommands are
one-shot operator utilities.

## Subcommands

- `goodnetd run` — long-running daemon process (load config + manifest
  → start kernel → load plugins → serve until SIGTERM)
- `goodnetd version` — prints kernel + plugin versions
- `goodnetd config validate` — checks a config file against the kernel's
  config schema
- `goodnetd plugin hash <path>` — SHA-256 of a plugin's `.so` for
  pinning in manifests
- `goodnetd manifest gen` — emits a manifest skeleton for a plugin
  directory
- `goodnetd identity gen [--expiry N]` — generates a fresh node
  identity
- `goodnetd identity show` — prints the loaded node identity

## Build

```
nix build .#
./result/bin/goodnetd version
```

## License

MIT — see `LICENSE`. Links against the GoodNet kernel through the
stable C ABI declared in the kernel's SDK headers; not a derivative
work per the kernel's Linking Exception.
