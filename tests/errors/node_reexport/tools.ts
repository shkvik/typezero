import { join } from "node:path"
// expect-error: 'join' comes from a built-in module, which is not re-exported: import it from there in the file that uses it
export { join }
export function tool() {
  return join("a", "b")
}
