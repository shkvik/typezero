import { fz } from "./f"
// f.ts loads first, and its arrow needs the type of e1, so e.ts is checked
// up to e1 ahead of its turn; this arrow needs fz, which f.ts declares later.
// expect-error: 'fz' of f.ts has no type yet where e.ts creates this function: the files import each other; declare the function with 'function' at the top level of the file
const twice = () => fz * 2
export const e1 = 1
