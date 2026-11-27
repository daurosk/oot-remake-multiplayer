#!/bin/bash
# Job-completed hook of the WSL runners (docs/ci-runner.md): empties the job's workspace and temp folder so build trees
# never pile up (disk guard), then logs the disk use. The shared ccache (/var/cache/squadala-ccache, size-capped by
# its ccache.conf) and the tool cache stay. Never fails the job.
set -u
say() { echo "squadala-ci cleanup: $*"; }
for d in "${GITHUB_WORKSPACE:-}" "${RUNNER_TEMP:-}"; do
    case "$d" in
        /opt/squadala-ci/runner-[0-9]/_work/?*) find "$d" -mindepth 1 -delete 2>/dev/null || say "could not empty all of $d" ;;
        "") ;;
        *) say "skipped $d (not under a runner's _work)" ;;
    esac
done
say "disk: $(df -h --output=used,avail / | tail -n1 | tr -s ' ') (distro used/avail); ccache $(du -sh /var/cache/squadala-ccache 2>/dev/null | cut -f1)"
exit 0
