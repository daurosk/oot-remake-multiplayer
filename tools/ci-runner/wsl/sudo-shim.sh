#!/bin/bash
# /opt/squadala-ci/bin/sudo, first on the WSL runners' PATH. Jobs never get root: root in WSL can mount C: and read
# the owner's files. The workflow's sudo lines are answered here instead:
#   sudo apt-get update ...            -> nothing to do (packages come from setup.sh)
#   sudo apt-get install ... <pkgs>    -> ok only when every package is already installed
#   sudo sysctl -w vm.mmap_rnd_bits=N  -> ok only when the value is already N (setup.sh sets 28 at boot)
# Anything else is refused. Add a missing package to setup.sh's PACKAGES and run install-wsl.ps1 again.
say() { echo "sudo shim (squadala-wsl runner): $*" >&2; }
case "${1:-}" in
    apt-get)
        shift; sub=""; pkgs=()
        for a in "$@"; do case "$a" in -*) ;; update|install) [ -z "$sub" ] && sub=$a || pkgs+=("$a") ;; *) pkgs+=("$a") ;; esac; done
        case "$sub" in
            update) say "apt-get update skipped (packages are preinstalled)"; exit 0 ;;
            install)
                for p in "${pkgs[@]}"; do
                    dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' \
                        || { say "$p is not preinstalled; add it to tools/ci-runner/wsl/setup.sh PACKAGES"; exit 1; }
                done
                say "already installed: ${pkgs[*]}"; exit 0 ;;
        esac ;;
    sysctl)
        if [ "${2:-}" = "-w" ] && [[ "${3:-}" =~ ^([a-z_.]+)=([0-9]+)$ ]]; then
            key=${BASH_REMATCH[1]}; want=${BASH_REMATCH[2]}
            have=$(cat "/proc/sys/${key//.//}" 2>/dev/null)
            [ "$have" = "$want" ] && { say "$key is already $want"; exit 0; }
            say "$key is $have, not $want; set it in setup.sh's sysctl file"; exit 1
        fi ;;
esac
say "refused: sudo $*"
exit 1
