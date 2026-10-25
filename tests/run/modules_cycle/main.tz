// same-as-node
// Files that import each other. main.ts loads a.ts, which loads b.ts, which
// imports a.ts back: b.ts runs first, before a.ts has run, so it uses what
// a.ts declares only inside functions, which run later.
import { greet, isEven } from "./a.ts"
console.log(greet("main"))
console.log(isEven(10), isEven(7))
