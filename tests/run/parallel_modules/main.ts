// same-as-node
// What runs in parallel across files: the explanation names the lines of
// another file with its name (visits.ts:8).
import { visit, visited } from "./visits.ts"

await Promise.all([visit("a"), visit("b")])
console.log(visited())
