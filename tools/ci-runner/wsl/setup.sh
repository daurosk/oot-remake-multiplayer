#!/bin/bash
# Sets up the WSL runners (docs/ci-runner.md, "Linux runners in WSL"). Run as root in the distro by install-wsl.ps1,
# from /opt/squadala-ci/src (the tools/ci-runner/wsl folder, copied in over stdin). Safe to run again: it installs
# what is missing and rewrites the config files; registered runners are kept (register.sh registers new ones).
# It never sees the registration token.
set -euo pipefail
cd "$(dirname "$0")"
VERSION=2.338.2
SHA256=baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40
RUNNERS=${RUNNERS:-3}
CI=/opt/squadala-ci
U=squadala-ci
HOSTNAME_CI=squadala-ci
# Everything the Build workflow's Linux jobs install with sudo or call, plus the runner's own needs (libicu).
PACKAGES="build-essential cmake ninja-build ccache gcc-12 g++-12 gcc-13 g++-13 clang llvm lld zip unzip xz-utils zstd
 file patch curl ca-certificates git jq python3 python3-venv python3-pip pkg-config squashfs-tools libicu74 rsync"

export DEBIAN_FRONTEND=noninteractive
missing=$(for p in $PACKAGES; do dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' || echo "$p"; done)
if [ -n "$missing" ]; then
    apt-get update -q
    # shellcheck disable=SC2086
    apt-get install -y -q $PACKAGES
fi

# The job user: no password, no sudo (root in WSL can reach the owner's files through a C: mount).
id "$U" >/dev/null 2>&1 || useradd --create-home --shell /bin/bash "$U"
passwd -l "$U" >/dev/null
for g in sudo adm admin; do gpasswd -d "$U" "$g" >/dev/null 2>&1 || true; done

# No Windows interop (a job could start Windows programs as the owner) and no C: mounts. Keeps the distro's
# default user line, if any.
default_user=$(sed -n 's/^[[:space:]]*default[[:space:]]*=[[:space:]]*//p' /etc/wsl.conf 2>/dev/null | head -n1)
{
    echo "# squadala-ci (tools/ci-runner/wsl/setup.sh): runners only; no Windows interop, no Windows drives."
    echo "[boot]"; echo "systemd=true"
    echo "[automount]"; echo "enabled=false"; echo "mountFsTab=false"
    echo "[interop]"; echo "enabled=false"; echo "appendWindowsPath=false"
    # Not the Windows PC name: it is the owner's name, and public job logs print the runner's machine name.
    echo "[network]"; echo "hostname=$HOSTNAME_CI"; echo "generateHosts=true"
    if [ -n "$default_user" ]; then echo "[user]"; echo "default=$default_user"; fi
} > /etc/wsl.conf

# TSan needs fewer mmap random bits (the workflow's 'sudo sysctl -w vm.mmap_rnd_bits=28' is checked by the shim).
echo "vm.mmap_rnd_bits=28" > /etc/sysctl.d/90-squadala-ci.conf
sysctl -q -p /etc/sysctl.d/90-squadala-ci.conf || true

install -d -o root -g root -m 755 "$CI" "$CI/bin" "$CI/hooks"
install -o root -g root -m 755 hooks/job-started.sh hooks/job-completed.sh "$CI/hooks/"
install -o root -g root -m 755 sudo-shim.sh "$CI/bin/sudo"
install -o root -g root -m 755 status-sink.sh "$CI/status-sink.sh"
install -o root -g root -m 755 register.sh "$CI/register.sh"
install -d -o "$U" -g "$U" -m 755 /var/cache/squadala-ccache "$CI/toolcache"
echo "max_size = 20G" > /var/cache/squadala-ccache/ccache.conf; chown "$U:$U" /var/cache/squadala-ccache/ccache.conf
echo "d /run/squadala-ci 0755 root root -" > /etc/tmpfiles.d/squadala-ci.conf
systemd-tmpfiles --create /etc/tmpfiles.d/squadala-ci.conf 2>/dev/null || install -d -m 755 /run/squadala-ci

tarball=/var/cache/actions-runner-linux-x64-$VERSION.tar.gz
if ! echo "$SHA256  $tarball" | sha256sum -c --status 2>/dev/null; then
    curl -fsSL -o "$tarball.part" "https://github.com/actions/runner/releases/download/v$VERSION/actions-runner-linux-x64-$VERSION.tar.gz"
    echo "$SHA256  $tarball.part" | sha256sum -c --status || { rm -f "$tarball.part"; echo "SHA-256 mismatch"; exit 1; }
    mv "$tarball.part" "$tarball"
fi

PATHLINE="$CI/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
for i in $(seq 1 "$RUNNERS"); do
    d=$CI/runner-$i
    if [ ! -f "$d/.runner" ]; then
        rm -rf "$d"; install -d -m 755 "$d"
        tar -C "$d" -xzf "$tarball"
        "$d/bin/installdependencies.sh" >/dev/null
    fi
    printf '%s\n' "LANG=C.UTF-8" "ACTIONS_RUNNER_HOOK_JOB_STARTED=$CI/hooks/job-started.sh" \
        "ACTIONS_RUNNER_HOOK_JOB_COMPLETED=$CI/hooks/job-completed.sh" "RUNNER_TOOL_CACHE=$CI/toolcache" \
        "AGENT_TOOLSDIRECTORY=$CI/toolcache" "CCACHE_DIR=/var/cache/squadala-ccache" > "$d/.env"
    echo "$PATHLINE" > "$d/.path"
done

cat > /etc/systemd/system/squadala-ci-runner@.service <<'UNIT'
[Unit]
Description=GitHub Actions runner %i (squadala-wsl, tools/ci-runner/wsl)
After=network-online.target
Wants=network-online.target
ConditionPathExists=/opt/squadala-ci/runner-%i/.runner

[Service]
User=squadala-ci
WorkingDirectory=/opt/squadala-ci/runner-%i
# Runner.Listener itself, as the runner's own service script starts it: run.sh would copy run-helper.sh into the
# root-owned runner folder, which squadala-ci cannot write.
ExecStart=/opt/squadala-ci/runner-%i/bin/Runner.Listener run --startuptype service
Restart=always
RestartSec=15
KillMode=process
TimeoutStopSec=5min
Nice=10
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=full

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload 2>/dev/null || true
echo "setup.sh done: $RUNNERS runner folders in $CI (version $VERSION); register with register.sh <n>"
