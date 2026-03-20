"use strict";

load("benchmarks/composites/harness.js");

const {
  DEFAULT_ITERATIONS,
  DEFAULT_WARMUP_ITERATIONS,
  DEFAULT_KEY_COUNT,
  now,
  printResult,
  buildKeyPool,
} = CompositeBench;

function warmup(keys, map) {
  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    const id = i % DEFAULT_KEY_COUNT;
    map.get(keys[id]);
  }
}

function runBenchmark() {
  const map = new Map();
  const keys = buildKeyPool(DEFAULT_KEY_COUNT);

  for (let i = 0; i < DEFAULT_KEY_COUNT; i++) {
    map.set(keys[i], i);
  }

  warmup(keys, map);

  let hits = 0;
  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    const id = i % DEFAULT_KEY_COUNT;
    if (map.get(keys[id]) === id) {
      hits++;
    }
  }

  const end = now();

  printResult(
    "map-reuse-same-reference",
    DEFAULT_ITERATIONS,
    [["Hits", hits]],
    end - start,
  );
}

runBenchmark();
