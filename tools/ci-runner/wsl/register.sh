#!/bin/bash
# register.sh <n>: registers /opt/squadala-ci/runner-<n> as squadala-wsl-<n> (labels self-hosted, linux, x64,
# squadala-wsl) and starts its service. Run as root by install-wsl.ps1, which pipes the registration token on
# stdin (never on a command line or in a file). Then locks the folder: root owns it, squadala-ci reads and runs it
# and writes only _work and _diag, so a job cannot change the runner, its hooks or its .env.
set -euo pipefail
n=${1:?runner number}
[[ "$n" =~ ^[0-9]$ ]] || { echo "bad runner number"; exit 1; }
d=/opt/squadala-ci/runner-$n
U=squadala-ci
read -r token
[ -n "$token" ] || { echo "no token on stdin"; exit 1; }
[ -f "$d/.runner" ] && { echo "runner-$n is already registered"; exit 0; }
install -d -o "$U" -g "$U" "$d/_work" "$d/_diag"
chown -R "$U:$U" "$d"
# config.sh refuses root; the token goes to it as an argument only inside the distro, as GitHub documents.
runuser -u "$U" -- "$d/config.sh" --unattended --url https://github.com/daurosk/oot-remake-multiplayer --token "$token" \
    --name "squadala-wsl-$n" --labels squadala-wsl --work _work --replace --disableupdate >/dev/null
token=""
chown -R root:"$U" "$d"; chmod -R u=rwX,g=rX,o= "$d"
chown -R "$U:$U" "$d/_work" "$d/_diag"
systemctl enable --now "squadala-ci-runner@$n.service"
echo "runner-$n registered and started"
