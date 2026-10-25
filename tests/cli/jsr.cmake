# Packages from JSR: versions and ranges, tz add / remove / update, tz.lock,
# conflicts of versions, hashes, builds without the registry. The registry is
# a mirror on the disk (see jsr_publish in cli.cmake) of the packages in jsr/.
include(${SOURCE}/../cli.cmake)

jsr_publish(@std/fmt 1.0.0 jsr/fmt-1.0.0)
jsr_publish(@std/fmt 1.2.0 jsr/fmt-1.2.0)
jsr_publish(@std/fmt 1.3.0 jsr/fmt-1.2.0 YANKED)
jsr_publish(@std/fmt 2.0.0 jsr/fmt-2.0.0)
jsr_publish(@tz/greet 1.0.0 jsr/greet-1.0.0)
jsr_publish(@tz/greet 2.0.0 jsr/greet-2.0.0)
jsr_publish(@tz/greet 2.1.0-beta.1 jsr/greet-2.1.0)
jsr_publish(@tz/old 1.0.0 jsr/old-1.0.0)

# Ranges as npm writes them; 1.3.0 is yanked, a prerelease only where the
# range names one.
tz(init ranges)
foreach(case "~1.0#1.0.0" ">=1.0.0 <1.2.0#1.0.0" "1.x || >=2#2.0.0" "1.0 - 1.3#1.2.0" "~1#1.2.0"
             "*#2.0.0" "=1.0.0#1.0.0" "<2#1.2.0" "1.2.0#1.2.0" "^1.2#1.2.0")
  string(REPLACE "#" ";" case "${case}")
  list(GET case 0 range)
  list(GET case 1 version)
  tz(add "@std/fmt@${range}" IN ranges STDOUT "added @std/fmt: jsr:@std/fmt@${range} (${version})\n")
endforeach()
tz(add @std/fmt@^0.1 IN ranges EXIT 1 STDERR "tz.json needs jsr:@std/fmt@^0.1, and the registry has no such version")
tz(add greet@^2.1.0-beta.1 IN ranges STDOUT "added greet: jsr:@tz/greet@^2.1.0-beta.1 (2.1.0-beta.1)\n")
tz(add @std/fmt@^1.2.3. IN ranges EXIT 1 STDERR "tz: in 'jsr:@std/fmt@^1.2.3.': '1.2.3.' is not a version or a range of versions")

tz(init app STDOUT
   "created tz.json, main.ts, .gitignore and tsconfig.json in app; run the program with 'tz run'\n")
write(app/main.ts [[
import { greet } from "greet"
console.log(greet("jsr"))
]])

# A name alone is the package adapted for tz, @tz/greet: its newest version
# that is not a prerelease, and what it imports. The cache is shared:
# @std/fmt 1.2.0 came for the project above and is not downloaded again.
tz(add greet IN app STDOUT "added greet: jsr:@tz/greet@^2.0.0 (2.0.0)\n"
   STDERR "tz: downloading jsr:@tz/greet@2.0.0")
if(TZ_STDERR MATCHES "downloading jsr:@std/fmt")
  message(FATAL_ERROR "@std/fmt was downloaded again:\n${TZ_STDERR}")
endif()
# The editor finds it in the cache (tests/cli/editor.cmake).
expect_match(app/.tz/tsconfig.json [=["greet": \["<work>/cache/jsr/@tz/greet/2.0.0/mod.ts"\]]=])
expect_file(app/tz.json [[
{
  "name": "app",
  "main": "main.ts",
  "dependencies": {
    "greet": "jsr:@tz/greet@^2.0.0"
  }
}
]])
# @std/fmt 1.3.0 is yanked; the test file of @std/fmt imports @std/assert,
# which no export reaches.
expect_match(app/tz.lock [[^{
  "lockVersion": 1,
  "packages": {
    "jsr:@std/fmt": {
      "version": "1.2.0",
      "hash": "sha256-[0-9a-f]+"
    },
    "jsr:@tz/greet": {
      "version": "2.0.0",
      "hash": "sha256-[0-9a-f]+",
      "dependencies": \["jsr:@std/fmt@\^1.0.0"\]
    }
  }
}
$]])
tz(run IN app STDOUT "<red>hello, jsr</red>\n")

# A range of tz.json and a range of a package meet at one version.
tz(add @std/fmt@~1.0 IN app STDOUT "added @std/fmt: jsr:@std/fmt@~1.0 (1.0.0)\n")
expect_match(app/tz.lock [["jsr:@std/fmt": {
      "version": "1.0.0",]])
write(app/main.ts [[
import { greet } from "greet"
import { shout, fmtVersion } from "@std/fmt"
import { red } from "@std/fmt/colors"
console.log(greet("jsr"), shout("fmt"), fmtVersion, red("r"))
]])
tz(run IN app STDOUT "<red>hello, jsr</red> !FMT! 1.0.0 <red>r</red>\n")

# No version fits all: tz.json stays as it was.
file(READ ${WORK}/app/tz.json before)
tz(add old IN app EXIT 1 STDERR [[
tz: no one version of jsr:@std/fmt fits every package that needs it:
  tz.json wants ~1.0 (newest: 1.0.0)
  jsr:@tz/greet 2.0.0 wants ^1.0.0 (newest: 1.2.0)
  jsr:@tz/old 1.0.0 wants ^2.0.0 (newest: 2.0.0)
a program has each package in one version (docs/packages.md); change the range in tz.json, or update the package that wants the older one
]])
file(READ ${WORK}/app/tz.json after)
if(NOT before STREQUAL after)
  message(FATAL_ERROR "a failed 'tz add' changed tz.json:\n${after}")
endif()

tz(add nosuch IN app EXIT 1 STDERR
   "tz: 'nosuch' has not been adapted for tz yet: there is no jsr:@tz/nosuch in the registry")
tz(add @std/fmt@^5 IN app EXIT 1 STDERR
   "tz: tz.json needs jsr:@std/fmt@^5, and the registry has no such version; it has 1.0.0, 1.2.0, 2.0.0")
tz(add npm:zod IN app EXIT 1 STDERR "tz: npm packages are not supported: npm has compiled JavaScript")

# Without @std/fmt in tz.json, greet's range lets it go up, but tz.lock keeps
# what it has until an update.
tz(remove @std/fmt IN app STDOUT "removed @std/fmt\n")
write(app/main.ts [[
import { greet } from "greet"
console.log(greet("jsr"))
]])
tz(run IN app STDOUT "<red>hello, jsr</red>\n")
expect_match(app/tz.lock [["version": "1.0.0"]])
tz(update IN app STDOUT "updated jsr:@std/fmt 1.0.0 -> 1.2.0\n")
tz(update IN app STDOUT "the packages are up to date\n")

# A new version within the range.
jsr_publish(@tz/greet 2.1.0 jsr/greet-2.1.0)
tz(update greet IN app STDOUT "updated jsr:@tz/greet 2.0.0 -> 2.1.0\n")
tz(run IN app STDOUT "<red>hi, jsr</red> (fmt 1.2.0)\n")
tz(remove nosuch IN app EXIT 1 STDERR "tz: 'nosuch' is not a dependency in <work>/app/tz.json")

# Only what a package exports can be imported.
write(app/bad.ts [[
import { wrap } from "greet/_wrap"
console.log(wrap("x", "-"))
]])
tz(build bad.ts IN app EXIT 1 STDERR "error: 'greet/_wrap' names nothing greet exports; it exports 'greet'")

# Files that are not what the registry lists, and sources that are not what
# tz.lock names.
jsr_publish(@tz/bad 1.0.0 jsr/greet-1.0.0)
file(APPEND ${WORK}/jsr/@tz/bad/1.0.0/mod.ts "// changed\n")
tz(add bad IN app EXIT 1 STDERR "/jsr/@tz/bad/1.0.0/mod.ts is not the file the registry lists: its SHA-256 differs")
file(READ ${WORK}/app/tz.lock lock)
string(REGEX REPLACE "(\"jsr:@tz/greet\": {[^}]*\"hash\": \")sha256-[0-9a-f]+" "\\1sha256-0123"
       changed "${lock}")
file(WRITE ${WORK}/app/tz.lock "${changed}")
tz(run IN app EXIT 1 STDERR "tz: the files of jsr:@tz/greet 2.1.0 are not those tz.lock names")
file(WRITE ${WORK}/app/tz.lock "${lock}")

# What tz.lock names is in the cache: the registry is not needed any more.
file(REMOVE_RECURSE ${WORK}/jsr)
tz(run IN app STDOUT "<red>hi, jsr</red> (fmt 1.2.0)\n")
# Named after the project.
tz(build IN app)
if(NOT EXISTS ${WORK}/app/app.exe AND NOT EXISTS ${WORK}/app/app)
  message(FATAL_ERROR "tz build did not make app.exe")
endif()
