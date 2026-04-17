#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MODULE_ID="crest"
COMPONENT_TYPE="sound_generators"
DIST_DIR="$REPO_ROOT/dist/$MODULE_ID"
REMOTE_USER="ableton"
REMOTE_HOST="move.local"
REMOTE_PATH="/data/UserData/schwung/modules/${COMPONENT_TYPE}/${MODULE_ID}"

if [ ! -d "$DIST_DIR" ]; then
    echo "ERROR: dist/$MODULE_ID not found. Run scripts/build.sh first."
    exit 1
fi

echo "Deploying to ${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_PATH} ..."

ssh "${REMOTE_USER}@${REMOTE_HOST}" "mkdir -p '${REMOTE_PATH}'"
scp -r "${DIST_DIR}/"* "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_PATH}/"
ssh "${REMOTE_USER}@${REMOTE_HOST}" "chmod -R a+rw '${REMOTE_PATH}'"

echo "Installed to ${REMOTE_PATH}"
