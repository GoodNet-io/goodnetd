# nix/setup.nix — `nix run .#setup` umbrella.
#
# One-shot bootstrap for a fresh goodnetd checkout:
# clones kernel + protocol-gnet into .goodnet/, then wires .githooks/.
# Idempotent — re-running on a set-up tree is a no-op.

{ pkgs, install-deps, install-hooks }:

pkgs.writeShellApplication {
  name = "goodnetd-setup";
  runtimeInputs = [ ];
  text = ''
    set -euo pipefail

    if [ ! -f flake.nix ]; then
      echo "setup: run from goodnetd root" >&2
      exit 1
    fi

    echo ">>> setup: install-deps"
    ${install-deps}/bin/goodnetd-install-deps

    echo ""
    echo ">>> setup: install-hooks"
    ${install-hooks}/bin/goodnetd-install-hooks

    echo ""
    echo "setup: done."
    echo ""
    echo "Enter dev shell:"
    echo "  nix develop \\"
    echo "    --override-input goodnet       path:.goodnet/goodnet \\"
    echo "    --override-input protocol-gnet path:.goodnet/protocol-gnet"
  '';
}
