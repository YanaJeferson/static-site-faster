// A faster drop-in for makefun-site 1.0.0: run(input) returns the same output tree,
// path for path and byte for byte. The reference's rules live in src/ (see NOTICE).

import { build } from './src/build.js';

/** makefun-site 1.0.0's answer for one case's input. */
export function run(input) {
  return build(input);
}
