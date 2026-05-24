{ pkgs }:

pkgs.writeShellApplication {
  name = "goodnetd-install-hooks";
  runtimeInputs = [ pkgs.git ];
  text = ''
    set -euo pipefail

    if [ ! -d .git ]; then
      echo "install-hooks: run from goodnetd root" >&2
      exit 1
    fi

    git config core.hooksPath .githooks
    echo ">>> install-hooks: .githooks wired"
  '';
}
