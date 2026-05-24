{ pkgs }:

pkgs.writeShellApplication {
  name = "goodnetd-install-deps";
  runtimeInputs = [ pkgs.git ];
  text = ''
    set -euo pipefail

    if [ ! -f flake.nix ]; then
      echo "install-deps: run from goodnetd root" >&2
      exit 1
    fi

    mkdir -p .goodnet

    clone_or_skip() {
      local name="$1" url="$2"
      if [ -d ".goodnet/$name" ]; then
        echo ">>> install-deps: $name already present, skipping"
      else
        echo ">>> install-deps: cloning $name"
        git clone "$url" ".goodnet/$name"
      fi
    }

    clone_or_skip goodnet       git@github.com:GoodNet-io/goodnet.git
    clone_or_skip protocol-gnet git@github.com:GoodNet-io/protocol-gnet.git

    echo ">>> install-deps: done"
  '';
}
