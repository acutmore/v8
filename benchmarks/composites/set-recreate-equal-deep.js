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

const OPTIONS = {
  shape: "deep",
};

function warmup(set) {
  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    set.has(makeCompositeKey(i % DEFAULT_KEY_COUNT, OPTIONS));
  }
}

function runBenchmark() {
  const set = new Set();

  for (let i = 0; i < DEFAULT_KEY_COUNT; i++) {
    set.add(makeCompositeKey(i, OPTIONS));
  }

  warmup(set);

  let hits = 0;
  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    const id = i % DEFAULT_KEY_COUNT;
    if (set.has(makeCompositeKey(id, OPTIONS))) {
      hits++;
    }
  }

  const end = now();

  printResult(
    "set-recreate-equal-deep",
    DEFAULT_ITERATIONS,
    [
      ["Hits", hits],
      ["Shape", OPTIONS.shape],
    ],
    end - start,
  );
}

runBenchmark();
