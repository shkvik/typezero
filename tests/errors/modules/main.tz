// Mistakes in imports and exports.
// expect-error: './math' has no export named 'sub'
// expect-error: './math' has no export named 'helper'; add 'export' to its declaration in math.ts
import { add, sub, helper, counter } from "./math"
// expect-error: import 'PI' conflicts with the declaration of 'PI' in this file; import it under another name, e.g. 'import { PI as PI2 } from "./math"'
import { add as plus, PI } from "./math"
// expect-error: duplicate import of 'add'
import { add } from "./math"
import { x } from "./stars"
const PI = 3
// expect-error: cannot assign to 'counter' because it is imported; change it in the file that declares it, e.g. with a function that file exports
counter = 5
// expect-error: cannot assign to 'counter' because it is imported
counter++
console.log(add(1, 2), plus(1, 2), PI, x, sub, helper)
// expect-error: cannot find 'nope' to export: this file neither declares nor imports it
export { nope, add as adder }
// expect-error: duplicate export of 'adder'
export { plus as adder }
// expect-error: './math' has no export named 'helper'
export { helper as h } from "./math"
// expect-error: 'require' is not supported: files are imported by name: 'import { a } from "./x"'
const r = require("./math")
