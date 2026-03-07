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

function warmup(set) {
  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    set.has(makeCompositeKey(i % DEFAULT_KEY_COUNT));
  }
}

function runBenchmark() {
  const set = new Set();

  for (let i = 0; i < DEFAULT_KEY_COUNT; i++) {
    set.add(makeCompositeKey(i));
  }

  warmup(set);

  let hits = 0;
  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    const id = i % DEFAULT_KEY_COUNT;
    if (set.has(makeCompositeKey(id))) {
      hits++;
    }
  }

  const end = now();

  printResult(
    "set-recreate-equal",
    DEFAULT_ITERATIONS,
    [["Hits", hits]],
    end - start,
  );
}

runBenchmark();
