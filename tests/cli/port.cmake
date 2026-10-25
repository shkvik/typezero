# tz port: a copy of a TypeScript library with the mechanical changes made,
# and a list of what is left for a person. port/autofix needs only the
# mechanical ones: its copy is a tz package a program uses. port/manual needs
# a person for each thing it does.
include(${SOURCE}/../cli.cmake)

tz(port ${SOURCE}/port/autofix -o lib STDOUT [[
ported autofix 1.0.0 into lib (4 files)
changed:
    5  '+' with a string became a template literal
    5  '===' and '!==' became '==' and '!='
    3  'export default' became a named export
    1  'import * as' became named imports
    2  'import type' and 'export type' lost 'type'
    2  'null' and 'void 0' became 'undefined'
    1  'var' became 'let'
    1  '| null' next to Json was dropped: a Json holds null itself
    1  default imports became named imports
then 'tz build lib/src/index.ts' reports what else tz does not accept
]])
expect_file(lib/tz.json [[
{
  "name": "autofix",
  "version": "1.0.0",
  "main": "src/index.ts",
  "dependencies": {}
}
]])
expect_file(lib/src/index.ts [[
// A library written for tsc and node; tz port makes a tz package of it.
import { whisper, voice as shout } from "./voice"
import { plural, short } from "./units.js"
import { Options } from "./options"
import { Level, levels } from "./options"
export { voice as loud } from "./voice"
export { levels }

export function greet(name: string, options?: Options): string {
  let level: Level = options == undefined ? "normal" : options.level
  if (level == "loud") return shout(name)
  if (level != "normal") return whisper(name)
  return `hello, ${name}!`
}

export function count(n: number, unit: string): string {
  return `${n} ${plural(unit, n)} (${short(unit)})`
}

export function find(names: string[], name: string): string | undefined {
  const i = names.indexOf(name)
  return i == -1 ? undefined : names[i]
}
]])
expect_file(lib/src/voice.ts [[
export function voice(name: string) {
  return `HELLO, ${name.toUpperCase()}!`
}

export function whisper(name: string) {
  return `(hello, ${name})`
}
]])
expect_file(lib/src/units.ts [[
export function plural(unit: string, n: number) {
  return n == 1 ? unit : `${unit}s`
}

export function short(unit: string) {
  return unit.slice(0, 1)
}
]])
expect_match(lib/src/options.ts "
export type Raw = Json
export const nothing: Json = null
")
expect_file(lib/LICENSE "MIT License\n\nA library for the tests of tz port.\n")
if(EXISTS ${WORK}/lib/src/index.test.ts OR EXISTS ${WORK}/lib/package.json)
  message(FATAL_ERROR "tz port copied files that no export reaches")
endif()

# The copy is a package.
tz(init app)
tz(add ../lib IN app STDOUT "added autofix: file:../lib\n")
write(app/main.ts [[
import { greet, count, find, loud, levels } from "autofix"
console.log(greet("tz"))
console.log(greet("tz", { level: "loud" }))
console.log(greet("tz", { level: "quiet" }))
console.log(loud("x"), count(2, "file"), count(1, "dir"))
console.log(find(["a", "b"], "b"), find([], "c"), levels.length)
]])
tz(run IN app STDOUT [[
hello, tz!
HELLO, TZ!
(hello, tz)
HELLO, X! 2 files (f) 1 dir (d)
b undefined 3
]])

tz(port ${SOURCE}/port/manual STDOUT [[
ported @x/manual 0.1.0 into manual (4 files)
changed:
    1  '===' and '!==' became '==' and '!='
    1  'any' became 'unknown'
    1  'export default' became a named export
    1  'import * as' became named imports
    1  'jsr:' imports became package names in tz.json
to change by hand (13):
  manual/mod.ts:3:23: npm packages are not supported (docs/packages.md): take the package from JSR or git, adapted for tz, or do without it
  manual/mod.ts:4:19: 'zod' is another package: add it to tz.json once it is adapted for tz (docs/packages.md), or do without it
  manual/mod.ts:5:1: an import for side effects is not supported: import what the file uses by name
  manual/mod.ts:15:3: getters and setters are not supported (docs/objects.md): reading a field never runs code; make 'get size()' a method, or keep the value in a field
  manual/mod.ts:16:3: getters and setters are not supported (docs/objects.md): reading a field never runs code; make 'set size()' a method, or keep the value in a field
  manual/mod.ts:19:25: 'any' became 'unknown': give the exact type, or narrow the value before using it (typeof, instanceof, 'in')
  manual/mod.ts:20:19: 'as' is not supported: remove it; if the value's type is wider than written, narrow it with a check (typeof, instanceof, 'in', '!= undefined')
  manual/mod.ts:24:33: 'as const' is not supported: declare the type of the value instead, e.g. a union of literal types
  manual/mod.ts:25:32: 'satisfies' is not supported: declare the type of the variable instead, or remove it
  manual/mod.ts:28:8: several signatures with one body ('pick') are not supported for plain functions (docs/overloads.md): keep one signature with unions, or give each its own function
  manual/mod.ts:34:10: 'arguments' is not supported: declare a rest parameter, e.g. '...args: string[]'
  manual/mod.ts:34:39: prototypes are not supported (docs/objects.md): declare the methods in a class
  manual/mod.ts:38:10: 'require' is not supported: import by name, e.g. 'import { x } from "./x"'
then 'tz build manual/mod.ts' reports what else tz does not accept
]])
# jsr: imports become dependencies; the anonymous class gets the file's name.
expect_file(manual/tz.json [[
{
  "name": "@x/manual",
  "version": "0.1.0",
  "exports": {
    ".": "./mod.ts",
    "./extra": "./extra.ts"
  },
  "dependencies": {
    "@std/path": "jsr:@std/path@^1.0.0"
  }
}
]])
expect_match(manual/mod.ts "\nimport { join } from \"@std/path/join\"\n")
expect_match(manual/mod.ts "\nimport { count } from \"./helpers.ts\"\n")
expect_match(manual/mod.ts "\n  return arguments.length \\+ count\\(Box.prototype\\)\n")
expect_file(manual/extra.ts "export class Extra {\n  name = \"extra\"\n}\n")
tz(port ${SOURCE}/port/manual EXIT 1 STDERR "tz: 'manual' is not empty; choose another directory with -o")

# From JSR and from git, through the cache.
jsr_publish(@x/manual 0.1.0 port/manual)
tz(port jsr:@x/manual -o fromjsr)
string(FIND "${TZ_STDOUT}" "ported @x/manual 0.1.0 into fromjsr (4 files)\n" found)
if(NOT found EQUAL 0)
  message(FATAL_ERROR "tz port jsr: printed\n${TZ_STDOUT}")
endif()
if(GIT)
  file(COPY ${SOURCE}/port/autofix/ DESTINATION ${WORK}/src/autofix)
  git(init -q IN src/autofix)
  git(add . IN src/autofix)
  git(commit -qm one IN src/autofix)
  git(tag v1.0.0 IN src/autofix)
  tz(port git:${WORK}/src/autofix@v1.0.0 -o fromgit)
  string(FIND "${TZ_STDOUT}" "ported autofix 1.0.0 into fromgit (4 files)\n" found)
  if(NOT found EQUAL 0)
    message(FATAL_ERROR "tz port git: printed\n${TZ_STDOUT}")
  endif()
  file(READ ${WORK}/lib/src/index.ts expected)
  expect_file(fromgit/src/index.ts "${expected}")
endif()

# In place, and with the entry named.
file(COPY ${SOURCE}/port/autofix/src/units.ts DESTINATION ${WORK}/own)
tz(port own --in-place EXIT 1 STDERR
   "tz: cannot tell which files own exports: it has no tz.json, jsr.json or package.json that names .ts files, and no mod.ts, index.ts or src/index.ts; name them with --entry <file>")
tz(port own --in-place --entry units.ts STDOUT [[
ported own into own (1 file)
changed:
    1  '+' with a string became a template literal
    1  '===' and '!==' became '==' and '!='
then 'tz build own/units.ts' reports what else tz does not accept
]])
file(READ ${WORK}/lib/src/units.ts expected)
expect_file(own/units.ts "${expected}")
expect_file(own/tz.json "{\n  \"name\": \"own\",\n  \"main\": \"units.ts\",\n  \"dependencies\": {}\n}\n")
tz(port jsr:@x/manual --in-place EXIT 1 STDERR
   "tz: --in-place changes a directory of this machine; 'jsr:@x/manual' is not one")
tz(port EXIT 2 STDERR "tz: 'tz port' needs a package")
