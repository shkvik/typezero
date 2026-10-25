// same-as-node
// A program of several files: named imports, renaming with `as`,
// re-exports, the index file of a directory, and the order files load in:
// what a file imports first, then the file itself, each file once.
import { add, PI, scale } from "./math.ts"
import { add as plus } from "./math.ts"
import { area, perimeter, unit } from "./geometry/index.ts"
import { counter, increment, history } from "./state.ts"
import { describe } from "./describe.ts"
import { label as oneLabel } from "./one.ts"
import { label as twoLabel } from "./two.ts"

console.log("main runs last")
console.log(add(1, 2), plus(2, 3), PI)
console.log(area(2).toFixed(2), perimeter(3, 4), unit)
console.log(describe(5))
// An import is read-only here, but it shows what its file changes.
console.log(counter)
increment()
increment()
console.log(counter, history.join(","))
// Files may have names of their own that are the same.
console.log(oneLabel(), twoLabel())
console.log(scale([1, 2, 3], 2))
