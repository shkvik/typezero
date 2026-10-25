// Reading the variable of a file before it loads: main.ts loads a.ts, which
// loads b.ts first, which imports a.ts back (docs/modules.md).
import { aName } from "./a"
import { x } from "./c"
import { e1 } from "./e"
console.log(aName, x, e1)
