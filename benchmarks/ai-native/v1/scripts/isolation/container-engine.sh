#!/usr/bin/env bash
# AI-NATIVE BENCHMARK - ISOLATION M1: shared container-engine detection.
#
# Mirrors scripts/build-release-linux-x86_64.sh's own Podman-first/
# Docker-fallback detection (see that script) so this project has exactly
# one place that knows how to pick a container engine for the release
# build, and exactly one (this file) for the benchmark sandbox - never a
# third independently-drifting copy of the same ~10 lines.
#
# Meant to be `source`d by sandbox-exec.sh and test-isolation.sh, not
# executed directly. Defines detect_container_engine(), which sets:
#
#   ENGINE            - "podman" or "docker"
#   ENGINE_USER_ARGS  - array of extra engine-specific flags so the
#                        container process's UID/GID lines up with the
#                        host user that owns the bind-mounted trial
#                        workspace directory - this is what makes the
#                        workspace writable inside the sandbox WITHOUT
#                        ever resorting to chmod 777 or running as root.
set -euo pipefail

detect_container_engine() {
    if command -v podman >/dev/null 2>&1; then
        ENGINE=podman
        # Rootless Podman maps container UIDs through a subordinate UID
        # range by default; --userns=keep-id makes the numeric UID/GID we
        # pass via --user resolve to the SAME identity inside the
        # container as it already is on the host, so the bind-mounted
        # workspace (owned by the invoking host user) stays writable
        # without relaxing its permissions.
        ENGINE_USER_ARGS=(--userns=keep-id --user "$(id -u):$(id -g)")
    elif command -v docker >/dev/null 2>&1; then
        ENGINE=docker
        # Docker (without user-namespace remapping, the default on most
        # hosts including GitHub-hosted runners) already shares the host
        # UID namespace directly, so --user alone is sufficient - Podman's
        # --userns=keep-id flag does not exist for Docker and is not
        # needed here.
        ENGINE_USER_ARGS=(--user "$(id -u):$(id -g)")
    else
        echo "error: neither podman nor docker was found on PATH." >&2
        echo "       The AI-native benchmark's isolated sandbox requires one of" >&2
        echo "       them to build/run the trial container - this is a hard" >&2
        echo "       requirement, not a convenience." >&2
        return 1
    fi
}
