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

const NAME = "set-recreate-equal-wider";
const ITERATIONS = DEFAULT_ITERATIONS;

const set = new Set();

for (let i = 0; i < DEFAULT_KEY_COUNT; i++) {
  const key = makeCompositeKey(i, {
    shape: "wide",
    widthLevel: "wider",
  });

  set.add(key);
}

for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
  const key = makeCompositeKey(i % DEFAULT_KEY_COUNT, {
    shape: "wide",
    widthLevel: "wider",
  });

  set.has(key);
}

const start = now();

let hits = 0;

for (let i = 0; i < ITERATIONS; i++) {
  const key = makeCompositeKey(i % DEFAULT_KEY_COUNT, {
    shape: "wide",
    widthLevel: "wider",
  });

  if (set.has(key)) {
    hits++;
  }
}

const end = now();

printResult(
  NAME,
  ITERATIONS,
  [
    ["Hits", hits],
    ["Shape", "wider"],
  ],
  end - start,
);
