"use strict";

load("benchmarks/composites/harness.js");

const {
  DEFAULT_ITERATIONS,
  DEFAULT_WARMUP_ITERATIONS,
  DEFAULT_KEY_COUNT,
  now,
  printResult,
  makeCompositeKey,
} = CompositeBench;

function warmup(map) {
  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    map.get(makeCompositeKey(i % DEFAULT_KEY_COUNT));
  }
}

function runBenchmark() {
  const map = new Map();

  for (let i = 0; i < DEFAULT_KEY_COUNT; i++) {
    map.set(makeCompositeKey(i), i);
  }

  warmup(map);

  let hits = 0;
  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    const id = i % DEFAULT_KEY_COUNT;
    if (map.get(makeCompositeKey(id)) === id) {
      hits++;
    }
  }

  const end = now();

  printResult(
    "map-recreate-equal",
    DEFAULT_ITERATIONS,
    [["Hits", hits]],
    end - start,
  );
}

runBenchmark();
