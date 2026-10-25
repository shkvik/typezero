// Imports from neverthrow name what it exports, and do not clash with the
// file's own declarations.
// expect-error: 'neverthrow' has no export named 'Okay'
import { ok, Okay } from "neverthrow"
// expect-error: import 'err' conflicts with the declaration of 'err' in this file
import { err } from "neverthrow"
// expect-error: neverthrow need not be re-exported: its names are built in, and every file has them
export { Result } from "neverthrow"

function err(message: string) {
  return message.length
}
console.log(ok(err("x")))
