# The editor's view of a project (docs/modules.md, section 6): tz init writes
# tsconfig.json, which extends .tz/tsconfig.json; tz writes that one, with
# where the packages are, whenever it builds the project or changes its
# packages. editor_tsc.cmake checks with TypeScript that it works.
include(${SOURCE}/../cli.cmake)

# The declarations next to tz.
get_filename_component(types ${TZ} DIRECTORY)
set(types ${types}/types)

# expect_config(<file> <text>): as expect_file, with <types> for the
# directory of the declarations.
function(expect_config file text)
  if(NOT EXISTS ${WORK}/${file})
    message(FATAL_ERROR "${file} does not exist")
  endif()
  file(READ ${WORK}/${file} actual)
  normalize(actual "${actual}")
  string(REPLACE "${types}" "<types>" actual "${actual}")
  if(NOT actual STREQUAL text)
    message(FATAL_ERROR "${file} differs\n--- expected:\n${text}\n--- actual:\n${actual}")
  endif()
endfunction()

# .tz/tsconfig.json is the header, the paths of the packages, and the footer.
set(header [=[
// Written by tz for the editor (tsc in VS Code), and again whenever tz builds
// the project or changes its packages: what tz has built in, and where the
// packages are on this computer. tsconfig.json extends it; git does not keep it.
{
  "extends": "<types>/tsconfig.json",
  "compilerOptions": {
    "paths": {
      "neverthrow": ["<types>/node_modules/neverthrow/index.d.ts"]]=])
set(footer [[
    }
  },
  "include": ["../**/*.ts"]
}
]])

file(MAKE_DIRECTORY ${WORK}/app)
tz(init IN app STDOUT
   "created tz.json, main.ts, .gitignore and tsconfig.json; run the program with 'tz run'\n")
expect_file(app/tsconfig.json [[
// The editor's view of the project (tsc in VS Code): what tz has built in and
// where the packages are come from .tz/tsconfig.json, which tz writes.
{
  "extends": "./.tz/tsconfig.json"
}
]])
expect_file(app/.gitignore "*.exe\n*.ll\n/app\n/.tz/\n")
expect_config(app/.tz/tsconfig.json "${header}\n${footer}")
if(NOT EXISTS ${types}/tsconfig.json OR NOT EXISTS ${types}/node_modules/@types/tz/index.d.ts
   OR NOT EXISTS ${types}/node_modules/@types/node/index.d.ts)
  message(FATAL_ERROR "the declarations for the editor are not next to tz: ${types}")
endif()

# Packages: what their exports name, or their entry and the paths inside
# them; tz add and tz remove change the paths.
write(libs/shapes/tz.json [[
{
  "name": "shapes",
  "exports": { ".": "./mod.ts", "./units": "./units.ts" }
}
]])
write(libs/shapes/mod.ts [[
export function area(w: double, h: double) { return w * h }
]])
write(libs/shapes/units.ts [[
export const cm = "cm"
]])
write(libs/fmt/src/index.ts [[
export function fixed(n: double) { return n.toFixed(1) }
]])
tz(add ../libs/shapes ../libs/fmt IN app)
expect_config(app/.tz/tsconfig.json "${header},
      \"fmt\": [\"<work>/libs/fmt/src/index.ts\"],
      \"fmt/*\": [\"<work>/libs/fmt/*\"],
      \"shapes\": [\"<work>/libs/shapes/mod.ts\"],
      \"shapes/units\": [\"<work>/libs/shapes/units.ts\"]
${footer}")
tz(remove fmt IN app)
expect_config(app/.tz/tsconfig.json "${header},
      \"shapes\": [\"<work>/libs/shapes/mod.ts\"],
      \"shapes/units\": [\"<work>/libs/shapes/units.ts\"]
${footer}")

# tz build writes it again, as after a clone (git does not keep it).
write(app/main.ts [[
import { area } from "shapes"
console.log(area(2, 3))
]])
file(REMOVE_RECURSE ${WORK}/app/.tz)
tz(run IN app STDOUT "6\n")
expect_config(app/.tz/tsconfig.json "${header},
      \"shapes\": [\"<work>/libs/shapes/mod.ts\"],
      \"shapes/units\": [\"<work>/libs/shapes/units.ts\"]
${footer}")

# A project made before tz set the editor up gets it from tz init; .tz/ goes
# into its .gitignore.
write(old/tz.json [[
{
  "name": "old",
  "main": "main.ts",
  "dependencies": { "shapes": "../libs/shapes" }
}
]])
write(old/main.ts "console.log(1)\n")
write(old/.gitignore "*.exe")
tz(init IN old STDOUT "created tsconfig.json for the editor\n")
expect_file(old/.gitignore "*.exe\n/.tz/\n")
expect_match(old/tsconfig.json [[extends": "\./\.tz/tsconfig\.json]])
expect_config(old/.tz/tsconfig.json "${header},
      \"shapes\": [\"<work>/libs/shapes/mod.ts\"],
      \"shapes/units\": [\"<work>/libs/shapes/units.ts\"]
${footer}")
tz(init IN old EXIT 1 STDERR "tz: there is a tz.json already in <work>/old")

# A project with a tsconfig.json of its own is left alone.
write(own/tz.json [[
{
  "name": "own",
  "main": "main.ts",
  "dependencies": {}
}
]])
write(own/main.ts "console.log(2)\n")
write(own/tsconfig.json "{}\n")
tz(run IN own STDOUT "2\n")
if(EXISTS ${WORK}/own/.tz)
  message(FATAL_ERROR "tz wrote .tz into a project whose tsconfig.json does not extend it")
endif()
