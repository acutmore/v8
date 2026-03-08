"use strict";

(function (global) {
  const DEFAULT_ITERATIONS = 200_000;
  const DEFAULT_WARMUP_ITERATIONS = 20_000;
  const DEFAULT_KEY_COUNT = 1000;

  function now() {
    return performance.now();
  }

  function printResult(name, iterations, extra, elapsedMs) {
    print(`Benchmark: ${name}`);
    print(`Iterations: ${iterations}`);

    for (const [label, value] of extra) {
      print(`${label}: ${value}`);
    }

    print(`Elapsed ms: ${elapsedMs.toFixed(3)}`);
    print(`Ops/sec: ${Math.round((iterations / elapsedMs) * 1000)}`);
  }

  function makeShallowCompositeKey(id, options = {}) {
    const duplicateGroupMod = options.duplicateGroupMod ?? 10;

    return new Composite({
      kind: "node",
      id,
      meta: new Composite({
        active: true,
        group: id % duplicateGroupMod,
      }),
    });
  }

  function makeDeepCompositeKey(id, options = {}) {
    const duplicateGroupMod = options.duplicateGroupMod ?? 10;

    return new Composite({
      kind: "node",
      id,
      meta: new Composite({
        level: 1,
        child: new Composite({
          level: 2,
          child: new Composite({
            level: 3,
            child: new Composite({
              active: true,
              group: id % duplicateGroupMod,
            }),
          }),
        }),
      }),
    });
  }

  function makeWideCompositeKey(id, options = {}) {
    const duplicateGroupMod = options.duplicateGroupMod ?? 10;

    return new Composite({
      kind: "node",
      id,
      a: id % 2,
      b: id % 3,
      c: id % 5,
      d: id % 7,
      e: id % 11,
      f: id % 13,
      g: id % 17,
      meta: new Composite({
        active: true,
        group: id % duplicateGroupMod,
      }),
    });
  }

  function makeCompositeKey(id, options = {}) {
    const shape = options.shape ?? "shallow";

    switch (shape) {
      case "deep":
        return makeDeepCompositeKey(id, options);
      case "wide":
        return makeWideCompositeKey(id, options);
      case "shallow":
      default:
        return makeShallowCompositeKey(id, options);
    }
  }

  function buildKeyPool(count, options = {}) {
    const duplication = options.duplication ?? 0;
    const uniqueCount = Math.max(1, Math.round(count * (1 - duplication)));

    const keys = [];
    for (let i = 0; i < count; i++) {
      const logicalId = i % uniqueCount;
      keys.push(makeCompositeKey(logicalId, options));
    }
    return keys;
  }

  global.CompositeBench = {
    DEFAULT_ITERATIONS,
    DEFAULT_WARMUP_ITERATIONS,
    DEFAULT_KEY_COUNT,
    now,
    printResult,
    makeShallowCompositeKey,
    makeDeepCompositeKey,
    makeWideCompositeKey,
    makeCompositeKey,
    buildKeyPool,
  };
})(globalThis);
