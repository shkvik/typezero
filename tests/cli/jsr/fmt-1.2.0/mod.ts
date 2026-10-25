// A JSR package as published: the import has .ts, as Deno writes it.
import { wrap } from "./_wrap.ts"

export const fmtVersion = "1.2.0"
export function shout(s: string) { return wrap(s.toUpperCase(), "!") }
