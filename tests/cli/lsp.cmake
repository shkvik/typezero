# The language server, tz lsp (docs/lsp.md, step 1): lsp.cjs talks to it as
# VS Code does. Errors appear where they are, columns in UTF-16 units, as the
# text in the editor changes, saved or not; a module is checked as part of the
# "main" of its tz.json, a file that main does not import is checked alone,
# notes go along with their error. A quick check comes first, then the whole
# program (step 5). What the check learns answers hover,
# definition, references, completion after a dot and rename (step 4). Needs
# node.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("node is not installed")
endif()

write(single/main.tz [[
const a = 1
const b: int = "hi"
console.log(a, b)
]])
write(wide/main.tz [[
const s = "привет😀"; const n: int = s
console.log(n)
]])
write(broken/main.tz [[
const = 1
]])
write(project/tz.json [[
{
  "name": "project",
  "main": "main.tz"
}
]])
write(project/main.tz [[
import { twice } from "./util.tz"
console.log(twice(2))
const bad: string = 1
]])
write(project/util.tz [[
export function twice(n: int): int {
  return n * 2 + undefinedName
}
]])
write(project/other.tz [[
const lonely: boolean = 1
]])
write(overflow/main.tz [[
function add(a: int8, b: int8): int8 {
  return a + b
}
console.log(add(100, 100))
]])
write(complete/main.tz [[
class Point {
  public x: double
  public y: double
  public constructor(x: double, y: double) {
    this.x = x
    this.y = y
  }
  public length(): double {
    return Math.sqrt(this.x * this.x + this.y * this.y)
  }
}
const p = new Point(3, 4)
console.log(p.length())
]])
write(notes/main.tz [[
function f() {
  const a = 1
  const a = 2
  console.log(a)
}
f()
]])

execute_process(COMMAND ${NODE} ${SOURCE}/lsp.cjs ${TZ} ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "lsp.cjs: exit code ${code}\n${out}${err}")
endif()
