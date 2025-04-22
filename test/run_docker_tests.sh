#!/bin/bash
#
# Copyright (C) 2025 The pgmoneta community
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this list
# of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice, this
# list of conditions and the following disclaimer in the documentation and/or other
# materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors may
# be used to endorse or promote products derived from this software without specific
# prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
# THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
# OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
# TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


set -euo pipefail

# Go to the root of the project (script is in test/, go one level up)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# Variables
IMAGE_NAME="pgmoneta-test"
CONTAINER_NAME="pgmoneta_test_container"
DOCKERFILE="./test/Dockerfile.testsuite"
LOG_DIR="./build/log"

# Function to cleanup container
echo " Cleaning up old container..."
if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}\$"; then
    docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
fi


echo " Building Docker image: $IMAGE_NAME"
if ! docker build -f "$DOCKERFILE" -t "$IMAGE_NAME" .; then
  echo "❌ Docker build failed."
  exit 1
fi

echo " Running tests in Docker container: $CONTAINER_NAME"
if ! docker run --name "$CONTAINER_NAME" "$IMAGE_NAME" bash -c "cd /pgmoneta/build && ./testsuite.sh"; then
  echo "⚠️ Test run in container exited with non-zero status (possibly expected for test failures)"
fi

echo " Copying logs from container to host: $LOG_DIR"
mkdir -p "$LOG_DIR"
if ! docker cp "$CONTAINER_NAME:/pgmoneta/build/log/." "$LOG_DIR"; then
  echo " Failed to copy logs from container."
  exit 1
fi

echo " Fixing log file ownership for host user"
if ! sudo chown -R "$(id -u):$(id -g)" "$LOG_DIR"; then
  echo " Could not change ownership of logs. You might need to clean them manually."
fi

echo " Done. Logs are available in $LOG_DIR"