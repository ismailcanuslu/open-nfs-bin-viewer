#!/bin/bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Starting NFSU2 BIN Opener (Native Vulkan Engine)..."
"$DIR/build/nfsu2_vulkan" "$@"
