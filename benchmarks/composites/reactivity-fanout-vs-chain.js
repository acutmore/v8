"use strict";

load("benchmarks/composites/harness.js");

const { DEFAULT_ITERATIONS, DEFAULT_WARMUP_ITERATIONS, now, printResult } =
  CompositeBench;

const VERSION_MOD = 64;

const CHAIN_LENGTH = 12;
const FANOUT_WIDTH = 12;

function makeChainNodeKey(step, version) {
  let input = new Composite({
    sourceVersion: version,
  });

  for (let i = 1; i <= step; i++) {
    input = new Composite({
      graph: "chain",
      step: i,
      input,
    });
  }

  return input;
}

function makeFanoutNodeKey(slot, version) {
  return new Composite({
    graph: "fanout",
    slot,
    input: new Composite({
      sourceVersion: version,
    }),
  });
}

function warmupChain(cache) {
  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    const version = i % VERSION_MOD;

    for (let step = 1; step <= CHAIN_LENGTH; step++) {
      const key = makeChainNodeKey(step, version);
      if (cache.get(key) === undefined) {
        cache.set(key, step + version);
      }
    }
  }
}

function warmupFanout(cache) {
  for (let i = 0; i < DEFAULT_WARMUP_ITERATIONS; i++) {
    const version = i % VERSION_MOD;

    for (let slot = 0; slot < FANOUT_WIDTH; slot++) {
      const key = makeFanoutNodeKey(slot, version);
      if (cache.get(key) === undefined) {
        cache.set(key, slot + version);
      }
    }
  }
}

function runChainScenario() {
  const cache = new Map();
  let hits = 0;
  let misses = 0;

  warmupChain(cache);

  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    const version = i % VERSION_MOD;

    for (let step = 1; step <= CHAIN_LENGTH; step++) {
      const key = makeChainNodeKey(step, version);
      const value = cache.get(key);

      if (value === undefined) {
        cache.set(key, step + version);
        misses++;
      } else {
        hits++;
      }
    }
  }

  const end = now();

  printResult(
    "reactivity-chain",
    DEFAULT_ITERATIONS * CHAIN_LENGTH,
    [
      ["Graph shape", "chain"],
      ["Chain length", CHAIN_LENGTH],
      ["Version count", VERSION_MOD],
      ["Hits", hits],
      ["Misses", misses],
      ["Cache size", cache.size],
    ],
    end - start,
  );
}

function runFanoutScenario() {
  const cache = new Map();
  let hits = 0;
  let misses = 0;

  warmupFanout(cache);

  const start = now();

  for (let i = 0; i < DEFAULT_ITERATIONS; i++) {
    const version = i % VERSION_MOD;

    for (let slot = 0; slot < FANOUT_WIDTH; slot++) {
      const key = makeFanoutNodeKey(slot, version);
      const value = cache.get(key);

      if (value === undefined) {
        cache.set(key, slot + version);
        misses++;
      } else {
        hits++;
      }
    }
  }

  const end = now();

  printResult(
    "reactivity-fanout",
    DEFAULT_ITERATIONS * FANOUT_WIDTH,
    [
      ["Graph shape", "fanout"],
      ["Fanout width", FANOUT_WIDTH],
      ["Version count", VERSION_MOD],
      ["Hits", hits],
      ["Misses", misses],
      ["Cache size", cache.size],
    ],
    end - start,
  );
}

runChainScenario();
print("");
runFanoutScenario();
