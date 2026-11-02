# A project: tz init, tz build and tz run without a file, scripts of
# package.json, the directory of tzconfig.json, local packages added with
# tz add, and the errors of package.json and tzconfig.json.
include(${SOURCE}/../cli.cmake)

file(MAKE_DIRECTORY ${WORK}/hello)
tz(init IN hello STDOUT "created package.json, tzconfig.json, main.tz and .gitignore; run the program with 'tz run'\n")
expect_file(hello/package.json [[
{
  "name": "hello",
  "main": "main.tz",
  "scripts": {
    "start": "tz run",
    "build": "tz build"
  },
  "dependencies": {}
}
]])
expect_file(hello/tzconfig.json [[
{
  "compilerOptions": {
    "outDir": "dist"
  }
}
]])
expect_file(hello/.gitignore "/dist/\n*.exe\n*.pdb\n*.ll\n")
tz(run IN hello STDOUT "Hello, world!\n")
tz(init IN hello EXIT 1 STDERR "tz: there is a package.json already in <work>/hello")

# Without a file, "main" runs; arguments that are not a .tz file are the
# program's, and so is everything after '--'.
write(hello/main.tz [[
import { argv } from "node:process"
console.log(argv.slice(2).join(" "))
]])
tz(run a b IN hello STDOUT "a b\n")
tz(run -- x.tz -o IN hello STDOUT "x.tz -o\n")
tz(run main.tz c IN hello STDOUT "c\n")
# A name in "scripts" runs that command line in the directory of package.json,
# with tz on its PATH and the other arguments after it; its exit code is tz's.
# After '--' the name is the program's again.
tz(run start x "y z" IN hello STDOUT "x y z\n" STDERR "$ tz run x \"y z\"")
tz(run -- start IN hello STDOUT "start\n")
file(READ ${WORK}/hello/package.json manifest)
string(REPLACE "\"build\": \"tz build\"" "\"build\": \"tz build\",\n    \"fail\": \"exit 3\"" manifest "${manifest}")
write(hello/package.json "${manifest}")
tz(run fail IN hello EXIT 3)
# From a directory inside the project too; the executable is named after it
# and goes to "compilerOptions.outDir" of tzconfig.json, made if missing, from
# anywhere inside the project; a file named to tz build goes there too, under
# its own name; -o writes where it says, making the directories.
file(MAKE_DIRECTORY ${WORK}/hello/sub)
tz(run d IN hello/sub STDOUT "d\n")
tz(build IN hello/sub)
if(NOT EXISTS ${WORK}/hello/dist/hello.exe AND NOT EXISTS ${WORK}/hello/dist/hello)
  message(FATAL_ERROR "tz build did not make dist/hello.exe")
endif()
write(hello/sub/tool.tz [[
console.log("tool")
]])
tz(build sub/tool.tz IN hello)
if(NOT EXISTS ${WORK}/hello/dist/tool.exe AND NOT EXISTS ${WORK}/hello/dist/tool)
  message(FATAL_ERROR "tz build sub/tool.tz did not make dist/tool.exe")
endif()
tz(build -o out/deep/hi.exe IN hello)
if(NOT EXISTS ${WORK}/hello/out/deep/hi.exe)
  message(FATAL_ERROR "tz build -o out/deep/hi.exe did not make it")
endif()
write(hello/tzconfig.json [[
{ "compilerOptions": { "outdir": "x" } }
]])
tz(build IN hello EXIT 1 STDERR
   "tzconfig.json:1:24: error: unknown compiler option 'outdir' in tzconfig.json; the options are outDir")
write(hello/tzconfig.json [[
{ "include": ["src"] }
]])
tz(build IN hello EXIT 1 STDERR
   "tzconfig.json:1:3: error: unknown field 'include' in tzconfig.json; the fields are compilerOptions")
write(hello/tzconfig.json [[
{ "compilerOptions": { "outDir": 5 } }
]])
tz(build IN hello EXIT 1 STDERR
   "tzconfig.json:1:34: error: 'outDir' is the directory tz build writes to, e.g. \"dist\"")

# Local packages: a directory with or without a package.json, which can depend on
# others; imports of a package's names and paths.
write(libs/shapes/package.json [[
{
  "name": "shapes",
  "exports": { ".": "./mod.tz", "./units": "./units.tz" },
  "dependencies": { "fmt": "file:../fmt" }
}
]])
write(libs/shapes/mod.tz [[
import { fixed } from "fmt"
import { cm } from "./units.tz"
export function area(w: double, h: double) { return `${fixed(w * h)} ${cm}²` }
]])
write(libs/shapes/units.tz [[
export const cm = "cm"
]])
write(libs/fmt/index.tz [[
export function fixed(n: double) { return n.toFixed(1) }
]])
write(libs/fmt/extra/more.tz [[
export const more = "more"
]])
tz(init app)
tz(add ../libs/shapes ../libs/fmt IN app STDOUT "added shapes: file:../libs/shapes\nadded fmt: file:../libs/fmt\n")
expect_file(app/package.json [[
{
  "name": "app",
  "main": "main.tz",
  "scripts": {
    "start": "tz run",
    "build": "tz build"
  },
  "dependencies": {
    "fmt": "file:../libs/fmt",
    "shapes": "file:../libs/shapes"
  }
}
]])
write(app/main.tz [[
import { area } from "shapes"
import { cm } from "shapes/units"
import { more } from "fmt/extra/more"
console.log(area(2, 1.5), cm, more)
]])
tz(run IN app STDOUT "3.0 cm² cm more\n")
if(EXISTS ${WORK}/app/package-lock.json)
  message(FATAL_ERROR "local packages need no package-lock.json")
endif()

# Errors.
write(app/bad.tz [[
import { z } from "zod"
console.log(z)
]])
tz(build bad.tz IN app EXIT 1 STDERR
   "error: 'zod' is not a dependency in <work>/app/package.json; add it with 'tz add zod'")
write(app/bad.tz [[
import { x } from "shapes/mod"
console.log(x)
]])
tz(build bad.tz IN app EXIT 1 STDERR
   "error: 'shapes/mod' names nothing shapes exports; it exports 'shapes', 'shapes/units'")
write(app/bad.tz [[
import { x } from "jsr:@std/path@^1"
console.log(x)
]])
tz(build bad.tz IN app EXIT 1 STDERR
   "error: import a package by its name in package.json: add it with 'tz add jsr:@std/path' and write 'from \"@std/path\"'")
write(app/bad.tz [[
import { x } from "npm:zod"
console.log(x)
]])
tz(build bad.tz IN app EXIT 1 STDERR "error: npm packages are not supported")

# The fields of npm that tz does not use are left alone.
write(broken/package.json [[
{
  "name": "broken",
  "description": "npm writes these",
  "type": "module",
  "devDependencies": { "typescript": "^5" },
  "main": "x.tz"
}
]])
write(broken/x.tz [[
console.log("npm fields are fine")
]])
tz(run IN broken STDOUT "npm fields are fine\n")
write(broken/package.json [[
{ "scripts": { "start": 5 } }
]])
tz(run start IN broken EXIT 1 STDERR "package.json:1:25: error: a script is a command line, e.g. \"tz build\"")
write(broken/package.json [[
{
  "dependencies": {
    "fs": "file:../libs/fmt",
  }
}
]])
tz(build x.tz IN broken EXIT 1 STDERR
   "package.json:3:5: error: 'fs' is the name of a module built into tz; give the package another name")
write(broken/package.json [[
{ "dependencies": { "zod": "^4" } }
]])
tz(build x.tz IN broken EXIT 1 STDERR
   "package.json:1:28: error: '^4' is a version, but not where the package comes from; tz takes packages from JSR and git: write e.g. 'jsr:@scope/name@^4'")
write(broken/package.json [[
{ "dependencies": { "zod": "npm:zod@^4" } }
]])
tz(build x.tz IN broken EXIT 1 STDERR "package.json:1:28: error: npm packages are not supported")
write(broken/package.json [[
{ "dependencies": { "lib": "file:../nowhere" } }
]])
tz(build x.tz IN broken EXIT 1 STDERR "package.json:1:28: error: there is no directory '../nowhere'")
write(broken/package.json [[
{ "name": "broken", }
]])
tz(build IN broken EXIT 1 STDERR "package.json has no \"main\": add \"main\": \"main.tz\", or name the file to build")
write(broken/package.json "{ \"name\": ")
tz(build IN broken EXIT 1 STDERR "package.json:1:11: error: unexpected end of the JSON text")
tz(build EXIT 2 STDERR "tz: no source file given, and there is no package.json here or above")
tz(add zod EXIT 1 STDERR "tz: there is no package.json in this directory or above it; make one with 'tz init'")
