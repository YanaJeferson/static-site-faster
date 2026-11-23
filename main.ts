// The harness' driver (node22): the native engine (native/engine.node) reads /input/cases.json,
// builds every case and writes /output/answers.json in C++. If the addon cannot load, solve.js
// answers the same tree in JavaScript.
// MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and /output outside a container.

import { createRequire } from 'node:module';

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';
const casesPath = `${INPUT_DIR}/cases.json`;
const outPath = `${OUTPUT_DIR}/answers.json`;

let native: { runFile?: (input: string, output: string) => void; runAll?: (cases: Buffer) => Buffer } | undefined;
try {
  native = createRequire(import.meta.url)('./native/engine.node') as typeof native;
} catch {
  native = undefined;
}

if (native?.runFile !== undefined) {
  native.runFile(casesPath, outPath);
} else {
  const { readFileSync, writeFileSync } = await import('node:fs');
  if (native?.runAll !== undefined) {
    writeFileSync(outPath, native.runAll(readFileSync(casesPath)));
  } else {
    const { setFlagsFromString } = await import('node:v8');
    setFlagsFromString('--no-incremental-marking');
    setFlagsFromString('--no-parallel-scavenge');
    setFlagsFromString('--max-semi-space-size=32');
    const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };
    const cases = JSON.parse(readFileSync(casesPath, 'utf8')) as { id: string; input: unknown }[];
    const answers: unknown[] = [];
    for (const c of cases) answers.push(run(c.input));
    writeFileSync(outPath, JSON.stringify(answers));
  }
}
