// The harness' driver (node22): the native engine (native/engine.node) reads /input/cases.json,
// builds every case and writes /output/answers.json in C++. If the addon cannot load, solve.js
// answers the same tree in JavaScript.
// MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and /output outside a container.

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { createRequire } from 'node:module';
import { setFlagsFromString } from 'node:v8';

// V8's incremental marking off, set before solve.js is imported so all of its work runs under it.
// Parallel scavenging and a larger young generation keep GC threads off the measured CPU budget.
setFlagsFromString('--no-incremental-marking');
setFlagsFromString('--no-parallel-scavenge');
setFlagsFromString('--max-semi-space-size=32');

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';
const casesPath = join(INPUT_DIR, 'cases.json');
const outPath = join(OUTPUT_DIR, 'answers.json');

let native: { runFile?: (input: string, output: string) => void; runAll?: (cases: Buffer) => Buffer } | undefined;
try {
  native = createRequire(import.meta.url)('./native/engine.node') as typeof native;
} catch {
  native = undefined;
}

if (native?.runFile !== undefined) {
  native.runFile(casesPath, outPath);
} else if (native?.runAll !== undefined) {
  writeFileSync(outPath, native.runAll(readFileSync(casesPath)));
} else {
  const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };
  const cases = JSON.parse(readFileSync(casesPath, 'utf8')) as { id: string; input: unknown }[];
  const answers: unknown[] = [];
  for (const c of cases) answers.push(run(c.input));
  writeFileSync(outPath, JSON.stringify(answers));
}
