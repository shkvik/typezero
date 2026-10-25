// The forms of import and export that tz does not have, and TypeScript's
// older ways to split code.
// expect-error: '/// <reference>' is not supported: files are joined by imports; import what the file needs by name
/// <reference path="./types.d.ts" />
// expect-error: default imports are not supported: there is no 'export default'; import by name, e.g. 'import { square } from "./math"'
import square from "./math"
// expect-error: 'import * as' is not supported; list the names you need, e.g. 'import { a, b } from "./math"'
import * as geo from "./math"
// expect-error: an import must name what it takes: importing a file only to run it is not supported; import the names you need, e.g. 'import { a } from "./math"'
import "./math"
// expect-error: an import must name what it takes
import {} from "./math"
// expect-error: 'import type' is not needed: types are imported like everything else; drop 'type'
import type { T } from "./math"
// expect-error: 'type' is not needed in an import: types are imported like everything else; drop it
import { type U, add } from "./math"
// expect-error: 'import add2 = ...' is not supported; import by name
import add2 = require("./math")
// expect-error: 'export default' is not supported: everything is exported by name
export default function main() {}
// expect-error: 'export * as' is not supported; re-export the names, e.g. 'export { a, b } from "./math"', or all of them with 'export * from "./math"'
export * as all from "./math"
// expect-error: 'export type' is not needed: types are exported like everything else; drop 'type'
export type { T }
// expect-error: 'export =' is not supported; export by name
export = add
// expect-error: 'namespace' is not supported: it is TypeScript's old way to group code; put the code in a file of its own and import it by name
namespace Shapes {
  export const sides = 4
}
// expect-error: 'declare module' is not supported: every file is a module of the program; import what it needs by name
declare module "lib" {
  export const y: int
}
// expect-error: 'declare global' is not supported
declare global {
  const z: int
}
// expect-error: 'declare' is not supported: the compiler needs the code of every declaration
declare const env: string
// expect-error: dynamic 'import()' is not supported: every file the program uses is compiled into it; import by name at the top of the file
const lazy = import("./math")
// expect-error: 'import.meta' is not supported
const url = import.meta.url
{
  // expect-error: 'import' is allowed only at the top level of a file
  import { add as inner } from "./math"
  // expect-error: 'export' is allowed only at the top level
  export const local = 1
}
