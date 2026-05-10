// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax

function collectStatsFor(fn) {
  %CompositeStats();
  fn();
  return %CompositeStats();
}

{
  const stats = collectStatsFor(() => {
    new Composite({ a: 1, b: 2, c: 3 });
    new Composite({ c: 3, b: 2, a: 1 });
  });

  assertEquals(2, stats.totalConstructions);
  assertEquals(2, stats.fastPathConstructions);
  assertEquals(0, stats.slowPathConstructions);
  assertEquals(6, stats.totalProperties);
  assertTrue(stats.collectPropertiesMicros >= 0);
  assertTrue(stats.sortPropertiesMicros >= 0);
  assertTrue(stats.internalizeKeysMicros >= 0);
  assertTrue(stats.mapTransitionMicros >= 0);
  assertTrue(stats.hashValuesMicros >= 0);
  assertTrue(stats.writePropertiesMicros >= 0);
  assertTrue(stats.preventExtensionsMicros >= 0);
  assertTrue(stats.constructTotalMicros >= 0);
}

{
  const stats = %CompositeStats();

  assertEquals(0, stats.totalConstructions);
  assertEquals(0, stats.totalProperties);
  assertEquals(0, stats.constructTotalMicros);
}

{
  const stats = collectStatsFor(() => {
    const first = new Composite({ nested: new Composite({ value: 1 }) });
    const second = new Composite({ nested: new Composite({ value: 1 }) });
    const set = new Set([first]);
    assertTrue(set.has(second));
  });

  assertTrue(stats.compareCalls > 0);
  assertTrue(stats.comparePropertyChecks > 0);
  assertTrue(stats.compareRecursiveCalls > 0);
  assertTrue(stats.compareTotalMicros >= 0);
}
