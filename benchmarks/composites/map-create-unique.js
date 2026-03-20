"use strict";

load("benchmarks/composites/harness.js");

const {
  DEFAULT_ITERATIONS,
  DEFAULT_WARMUP_ITERATIONS,
  now,
  printResult,
  makeCompositeKey,
} = CompositeBench;

function warmup() {
  const map = new Map();

  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    map.set(makeCompositeKey(i), i);
  }
}

function runBenchmark() {
  const map = new Map();

  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    map.set(makeCompositeKey(i), i);
  }

  const end = now();

  printResult(
    "map-create-unique",
    DEFAULT_ITERATIONS,
    [["Map size", map.size]],
    end - start,
  );
}

warmup();
runBenchmark();
