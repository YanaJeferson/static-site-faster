// The harness' driver (node22): the native engine reads /input/cases.json, builds every case and
// writes /output/answers.json in C++. If the addon cannot load, solve.js answers the same tree.
// MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and /output outside a container.

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';
const casesPath = `${INPUT_DIR}/cases.json`;
const outPath = `${OUTPUT_DIR}/answers.json`;

interface Engine { runFile?: (input: string, output: string, exitWhenDone?: boolean) => void; runAll?: (cases: Buffer) => Buffer }
let native: Engine | undefined;
try {
  const file = process.arch === 'arm64' ? './native/engine.arm64.node' : './native/engine.node';
  native = require(file) as Engine;
} catch {
  native = undefined;
}

async function javascript(): Promise<void> {
  const { readFileSync, writeFileSync } = await import('node:fs');
  if (native?.runAll !== undefined) {
    writeFileSync(outPath, native.runAll(readFileSync(casesPath)));
    return;
  }
  const { setFlagsFromString } = await import('node:v8');
  setFlagsFromString('--no-incremental-marking');
  setFlagsFromString('--no-parallel-scavenge');
  setFlagsFromString('--max-semi-space-size=32');
  const { run } = (await import('./solve.js')) as { run: (input: unknown) => unknown };
  const cases = JSON.parse(readFileSync(casesPath, 'utf8')) as { id: string; input: unknown }[];
  const answers: unknown[] = [];
  for (const c of cases) answers.push(run(c.input));
  writeFileSync(outPath, JSON.stringify(answers));
}

if (native?.runFile !== undefined) {
  try {
    native.runFile(casesPath, outPath, true);
  } catch {
    void javascript();
  }
} else {
  void javascript();
}
