# A project: tz init, tz build and tz run without a file, local packages
# added with tz add, and the errors of tz.json.
include(${SOURCE}/../cli.cmake)

file(MAKE_DIRECTORY ${WORK}/hello)
tz(init IN hello STDOUT "created tz.json, main.ts and .gitignore; run the program with 'tz run'\n")
expect_file(hello/tz.json [[
{
  "name": "hello",
  "main": "main.ts",
  "dependencies": {}
}
]])
expect_file(hello/.gitignore "*.exe\n*.ll\n/hello\n")
tz(run IN hello STDOUT "Hello, world!\n")
tz(init IN hello EXIT 1 STDERR "tz: there is a tz.json already in <work>/hello")

# Without a file, "main" runs; arguments that are not a .ts file are the
# program's, and so is everything after '--'.
write(hello/main.ts [[
import { argv } from "node:process"
console.log(argv.slice(2).join(" "))
]])
tz(run a b IN hello STDOUT "a b\n")
tz(run -- x.ts -o IN hello STDOUT "x.ts -o\n")
tz(run main.ts c IN hello STDOUT "c\n")
# From a directory inside the project too; the executable is named after it.
file(MAKE_DIRECTORY ${WORK}/hello/sub)
tz(run d IN hello/sub STDOUT "d\n")
tz(build IN hello)
if(NOT EXISTS ${WORK}/hello/hello.exe AND NOT EXISTS ${WORK}/hello/hello)
  message(FATAL_ERROR "tz build did not make hello.exe")
endif()

# Local packages: a directory with or without a tz.json, which can depend on
# others; imports of a package's names and paths.
write(libs/shapes/tz.json [[
{
  "name": "shapes",
  "exports": { ".": "./mod.ts", "./units": "./units.ts" },
  "dependencies": { "fmt": "file:../fmt" }
}
]])
write(libs/shapes/mod.ts [[
import { fixed } from "fmt"
import { cm } from "./units.ts"
export function area(w: double, h: double) { return `${fixed(w * h)} ${cm}²` }
]])
write(libs/shapes/units.ts [[
export const cm = "cm"
]])
write(libs/fmt/index.ts [[
export function fixed(n: double) { return n.toFixed(1) }
]])
write(libs/fmt/extra/more.ts [[
export const more = "more"
]])
tz(init app)
tz(add ../libs/shapes ../libs/fmt IN app STDOUT "added shapes: file:../libs/shapes\nadded fmt: file:../libs/fmt\n")
expect_file(app/tz.json [[
{
  "name": "app",
  "main": "main.ts",
  "dependencies": {
    "fmt": "file:../libs/fmt",
    "shapes": "file:../libs/shapes"
  }
}
]])
write(app/main.ts [[
import { area } from "shapes"
import { cm } from "shapes/units"
import { more } from "fmt/extra/more"
console.log(area(2, 1.5), cm, more)
]])
tz(run IN app STDOUT "3.0 cm² cm more\n")
if(EXISTS ${WORK}/app/tz.lock)
  message(FATAL_ERROR "local packages need no tz.lock")
endif()

# Errors.
write(app/bad.ts [[
import { z } from "zod"
console.log(z)
]])
tz(build bad.ts IN app EXIT 1 STDERR
   "error: 'zod' is not a dependency in <work>/app/tz.json; add it with 'tz add zod'")
write(app/bad.ts [[
import { x } from "shapes/mod"
console.log(x)
]])
tz(build bad.ts IN app EXIT 1 STDERR
   "error: 'shapes/mod' names nothing shapes exports; it exports 'shapes', 'shapes/units'")
write(app/bad.ts [[
import { x } from "jsr:@std/path@^1"
console.log(x)
]])
tz(build bad.ts IN app EXIT 1 STDERR
   "error: import a package by its name in tz.json: add it with 'tz add jsr:@std/path' and write 'from \"@std/path\"'")
write(app/bad.ts [[
import { x } from "npm:zod"
console.log(x)
]])
tz(build bad.ts IN app EXIT 1 STDERR "error: npm packages are not supported")

write(broken/tz.json [[
{
  "name": "broken",
  "dependecies": {}
}
]])
tz(build x.ts IN broken EXIT 1 STDERR
   "tz.json:3:3: error: unknown field 'dependecies' in tz.json; the fields are name, version, main, exports and dependencies")
write(broken/tz.json [[
{
  "dependencies": {
    "fs": "file:../libs/fmt",
  }
}
]])
tz(build x.ts IN broken EXIT 1 STDERR
   "tz.json:3:5: error: 'fs' is the name of a module built into tz; give the package another name")
write(broken/tz.json [[
{ "dependencies": { "zod": "^4" } }
]])
tz(build x.ts IN broken EXIT 1 STDERR
   "tz.json:1:28: error: '^4' is a version, but not where the package comes from; tz takes packages from JSR and git: write e.g. 'jsr:@scope/name@^4'")
write(broken/tz.json [[
{ "dependencies": { "zod": "npm:zod@^4" } }
]])
tz(build x.ts IN broken EXIT 1 STDERR "tz.json:1:28: error: npm packages are not supported")
write(broken/tz.json [[
{ "dependencies": { "lib": "file:../nowhere" } }
]])
tz(build x.ts IN broken EXIT 1 STDERR "tz.json:1:28: error: there is no directory '../nowhere'")
write(broken/tz.json [[
{ "name": "broken", }
]])
tz(build IN broken EXIT 1 STDERR "tz.json has no \"main\": add \"main\": \"main.ts\", or name the file to build")
write(broken/tz.json "{ \"name\": ")
tz(build IN broken EXIT 1 STDERR "tz.json:1:11: error: unexpected end of the JSON text")
tz(build EXIT 2 STDERR "tz: no source file given, and there is no tz.json here or above")
tz(add zod EXIT 1 STDERR "tz: there is no tz.json in this directory or above it; make one with 'tz init'")
