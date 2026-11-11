#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p bin
for service in backend gateway client auth websocket-bridge; do
  CGO_ENABLED=0 go build -trimpath -o "bin/$service" "./cmd/$service"
done
cmake -S scene -B build/scene -DCMAKE_BUILD_TYPE=Release
cmake --build build/scene --parallel 2
cp build/scene/scene-server bin/scene-server
echo 'Build completed in bin/'
