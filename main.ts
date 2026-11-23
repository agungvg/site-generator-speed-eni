// makefun-site 1.0.0 driver. The harness runs `node /bot/main.ts` (node22).
// It reads every case from /input/cases.json (a JSON array of {id, input}),
// answers each one alone, and writes /output/answers.json. The fast path is
// the native engine (engine.node runAll): it reads the file, runs the whole
// build in C, and writes the answers. If the native path is unavailable or
// reports failure, fall back to the JS run() from solve.js (byte-exact).

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';
import { createRequire } from 'node:module';

// V8's incremental marking off, set before solve.js is imported so all of its work runs under it (the rules: the house
// baseline runs with --no-incremental-marking, and an entry may tune V8's flags too).
setFlagsFromString('--no-incremental-marking');

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

let done = false;
try {
  const eng = createRequire(import.meta.url)('./engine.node');
  if (eng && typeof eng.runAll === 'function') {
    done = eng.runAll(join(INPUT_DIR, 'cases.json'), join(OUTPUT_DIR, 'answers.json')) === true;
  }
} catch {
  done = false;
}

if (!done) {
  /** solve.js, imported by its URL (a .js module carries no types) and typed by its contract. */
  const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };
  const cases = JSON.parse(readFileSync(join(INPUT_DIR, 'cases.json'), 'utf8')) as { id: string; input: unknown }[];
  const answers: unknown[] = [];
  for (const c of cases) answers.push(run(c.input));
  writeFileSync(join(OUTPUT_DIR, 'answers.json'), JSON.stringify(answers));
}
