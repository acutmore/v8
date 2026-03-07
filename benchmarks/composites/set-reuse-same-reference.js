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

function warmup(set, keys) {
  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    set.has(keys[i % DEFAULT_KEY_COUNT]);
  }
}

function runBenchmark() {
  const set = new Set();
  const keys = buildKeyPool(DEFAULT_KEY_COUNT);

  for (let i = 0; i < DEFAULT_KEY_COUNT; i++) {
    set.add(keys[i]);
  }

  warmup(set, keys);

  let hits = 0;
  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    const id = i % DEFAULT_KEY_COUNT;
    if (set.has(keys[id])) {
      hits++;
    }
  }

  const end = now();

  printResult(
    "set-reuse-same-reference",
    DEFAULT_ITERATIONS,
    [["Hits", hits]],
    end - start,
  );
}

runBenchmark();
