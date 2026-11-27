#!/bin/bash
# Job-started hook of the WSL runners (docs/ci-runner.md, "Linux runners in WSL"). setup.sh installs it root-owned in
# /opt/squadala-ci/hooks; each runner's .env points ACTIONS_RUNNER_HOOK_JOB_STARTED at it. The runner runs it as
# squadala-ci before any step of every job; a non-zero exit fails the job, so nothing from the checkout runs.
# No switches: nothing in the job's environment changes what it checks (the guard limits below are constants).
#  1. Only a push by daurosk to daurosk/oot-remake-multiplayer on main, integrate-*, release-* or a tag, or a manual
#     run (workflow_dispatch: only people with write access can start one) by daurosk. pull_request never.
#  2. Disk guard: C: (which holds this distro's disk) must have >= 60 GB free, else the job is refused.
#  3. Memory guard: waits while a lane game or offline-queue job runs (lane locks / TEST-TURN LANE lines, read by
#     status-feed.ps1 on Windows) or Windows' free commit is < 75 GB (a stock game pair needs ~62 GB), at most
#     5 h, then refuses. The numbers come from /run/squadala-ci/status, which status-feed.ps1 keeps fresh; a
#     missing, old-format or stale (> 120 s) file counts as "not enough" (fail closed). The other half is the
#     offline queue's start gate, which counts a live WSL VM at its .wslconfig cap, so no game starts into a CI job.
set -u
STATUS=/run/squadala-ci/status
MIN_COMMIT_GB=75
MIN_DISK_GB=60
MAX_WAIT_S=18000  # 5 h: an rc test pass on the lanes takes hours; under the jobs' 6 h limit
STALE_S=120
say() { echo "squadala-ci hook: $*"; }

ev=${GITHUB_EVENT_NAME:-}; repo=${GITHUB_REPOSITORY:-}; actor=${GITHUB_ACTOR:-}; tactor=${GITHUB_TRIGGERING_ACTOR:-}; ref=${GITHUB_REF:-}
line="GITHUB_EVENT_NAME=$ev GITHUB_REPOSITORY=$repo GITHUB_ACTOR=$actor GITHUB_TRIGGERING_ACTOR=$tactor GITHUB_REF=$ref"
ok=0
if [ "$repo" = "daurosk/oot-remake-multiplayer" ] && [ "$actor" = "daurosk" ] && [ "$tactor" = "daurosk" ]; then
    case "$ev" in
        workflow_dispatch) case "$ref" in refs/heads/?*|refs/tags/?*) ok=1 ;; esac ;;
        push) case "$ref" in refs/heads/main|refs/heads/integrate-?*|refs/heads/release-?*|refs/tags/?*) ok=1 ;; esac ;;
    esac
fi
if [ "$ok" != 1 ]; then
    say "REFUSED, this runner only takes pushes by daurosk to main, integrate-*, release-* or tags, and daurosk's manual runs ($line)"
    exit 1
fi

waited=0; next_log=0
while :; do
    reason=""
    if ! read -r ts commit disk busy < "$STATUS" 2>/dev/null; then
        reason="no status file $STATUS (status-feed.ps1 not running on Windows?)"
    elif ! [[ "$ts" =~ ^[0-9]+$ && "$commit" =~ ^[0-9]+$ && "$disk" =~ ^[0-9]+$ && "${busy:-}" =~ ^[01]$ ]]; then
        reason="bad status line in $STATUS"
    elif [ $(( $(date +%s) - ts )) -gt "$STALE_S" ]; then
        reason="status is $(( $(date +%s) - ts )) s old (status-feed.ps1 stopped?)"
    elif [ "$disk" -lt "$MIN_DISK_GB" ]; then
        say "REFUSED, disk guard: C: has $disk GB free (< $MIN_DISK_GB GB). Free disk space, then re-run the job."
        exit 1
    elif [ "$busy" = 1 ]; then
        reason="a test-lane game or offline-queue job is running (free commit $commit GB)"
    elif [ "$commit" -lt "$MIN_COMMIT_GB" ]; then
        reason="free commit $commit GB < $MIN_COMMIT_GB GB (a game test is running)"
    else
        say "allowed ($line); free commit $commit GB, C: $disk GB free, waited ${waited} s"
        exit 0
    fi
    if [ "$waited" -ge "$MAX_WAIT_S" ]; then
        say "REFUSED, memory guard: waited $(( waited / 60 )) min: $reason. Re-run the job when the PC is free."
        exit 1
    fi
    if [ "$waited" -ge "$next_log" ]; then say "waiting: $reason (waited $(( waited / 60 )) of $(( MAX_WAIT_S / 60 )) min)"; next_log=$(( next_log + 300 )); fi
    sleep 30; waited=$(( waited + 30 ))
done
