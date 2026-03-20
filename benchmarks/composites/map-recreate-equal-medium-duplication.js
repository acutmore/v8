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

const DUPLICATION = 0.5;
const UNIQUE_COUNT = Math.max(
  1,
  Math.round(DEFAULT_KEY_COUNT * (1 - DUPLICATION)),
);

function warmup(map) {
  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    map.get(makeCompositeKey(i % UNIQUE_COUNT));
  }
}

function runBenchmark() {
  const map = new Map();

  for (let i = 0; i < UNIQUE_COUNT; i++) {
    map.set(makeCompositeKey(i), i);
  }

  warmup(map);

  let hits = 0;
  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    const id = i % UNIQUE_COUNT;
    if (map.get(makeCompositeKey(id)) === id) {
      hits++;
    }
  }

  const end = now();

  printResult(
    "map-recreate-equal-medium-duplication",
    DEFAULT_ITERATIONS,
    [
      ["Hits", hits],
      ["Duplication", DUPLICATION],
      ["Unique key count", UNIQUE_COUNT],
    ],
    end - start,
  );
}

runBenchmark();
