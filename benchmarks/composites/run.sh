#!/usr/bin/env bash
set -euo pipefail

D8=out/x64.release/d8
DIR=benchmarks/composites

echo "Running Composite benchmarks..."
echo

"$D8" "$DIR/map-reuse-same-reference.js"
echo

"$D8" "$DIR/map-recreate-equal.js"
echo

"$D8" "$DIR/map-recreate-equal-medium-duplication.js"
echo

"$D8" "$DIR/map-recreate-equal-high-duplication.js"
echo

"$D8" "$DIR/map-create-unique.js"
echo

"$D8" "$DIR/set-reuse-same-reference.js"
echo

"$D8" "$DIR/set-recreate-equal.js"
echo