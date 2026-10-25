# Packages from git: tags, branches, what a git package depends on, tz.lock
# with commits and hashes, conflicts, builds without the repositories. The
# repositories are bare ones the test makes in its directory.
include(${SOURCE}/../cli.cmake)
if(NOT GIT)
  skip("git is not installed")
endif()

# numbers: v1.0.0 and v2.0.0.
write(src/numbers/tz.json [[
{ "name": "numbers", "main": "numbers.ts" }
]])
write(src/numbers/numbers.ts [[
export function scale(n: int) { return n * 10 }
]])
git(init -q IN src/numbers)
git(add . IN src/numbers)
git(commit -qm one IN src/numbers)
git(tag v1.0.0 IN src/numbers)
write(src/numbers/numbers.ts [[
export function scale(n: int) { return n * 100 }
]])
git(commit -qam two IN src/numbers)
git(tag v2.0.0 IN src/numbers)
git(clone -q --bare src/numbers numbers.git IN .)

# lib: v1.0.0 needs numbers v1.0.0, v1.1.0 needs numbers v2.0.0; its own files
# import each other and numbers by the name in its tz.json.
write(src/lib/tz.json "{
  \"name\": \"lib\",
  \"version\": \"1.0.0\",
  \"main\": \"src/index.ts\",
  \"dependencies\": { \"numbers\": \"git:${WORK}/numbers.git@v1.0.0\" }
}
")
write(src/lib/src/index.ts [[
import { scale } from "numbers"
import { label } from "./label"
export function describe(n: int) { return `${label} ${scale(n)}` }
]])
write(src/lib/src/label.ts [[
export const label = "lib 1.0:"
]])
git(init -q IN src/lib)
git(add . IN src/lib)
git(commit -qm one IN src/lib)
git(tag v1.0.0 IN src/lib)
file(READ ${WORK}/src/lib/tz.json manifest)
string(REPLACE "v1.0.0" "v2.0.0" manifest "${manifest}")
write(src/lib/tz.json "${manifest}")
write(src/lib/src/label.ts [[
export const label = "lib 1.1:"
]])
git(commit -qam two IN src/lib)
git(tag v1.1.0 IN src/lib)
git(clone -q --bare src/lib lib.git IN .)

tz(init app)
write(app/main.ts [[
import { describe } from "lib"
console.log(describe(4))
]])
tz(add git:${WORK}/lib.git@v1.0.0 IN app STDERR "tz: cloning <work>/lib.git" "tz: cloning <work>/numbers.git")
string(REGEX MATCH "^added lib: git:<work>/lib.git@v1.0.0 \\([0-9a-f]+\\)\n$" ok "${TZ_STDOUT}")
if(NOT ok)
  message(FATAL_ERROR "tz add printed:\n${TZ_STDOUT}")
endif()
expect_match(app/tz.lock [[^{
  "lockVersion": 1,
  "packages": {
    "git:<work>/lib.git": {
      "ref": "v1.0.0",
      "commit": "[0-9a-f]+",
      "hash": "sha256-[0-9a-f]+",
      "dependencies": \["git:<work>/numbers.git@v1.0.0"\]
    },
    "git:<work>/numbers.git": {
      "ref": "v1.0.0",
      "commit": "[0-9a-f]+",
      "hash": "sha256-[0-9a-f]+"
    }
  }
}
$]])
tz(run IN app STDOUT "lib 1.0: 40\n")

# Two tags of one repository in one program.
tz(add git:${WORK}/numbers.git@v2.0.0 IN app EXIT 1 STDERR
   "tz: the packages that need git:<work>/numbers.git want different versions of it:\n  tz.json wants 'v2.0.0'\n  git:<work>/lib.git v1.0.0 (")

# Another tag in tz.json: the next build takes it, and what it needs.
file(READ ${WORK}/app/tz.json manifest)
string(REPLACE "@v1.0.0" "@v1.1.0" manifest "${manifest}")
write(app/tz.json "${manifest}")
tz(run IN app STDOUT "lib 1.1: 400\n")
expect_match(app/tz.lock [["ref": "v1.1.0"]])
expect_match(app/tz.lock [["ref": "v2.0.0"]])
tz(add git:${WORK}/lib.git@v9 IN app EXIT 1 STDERR "tz: <work>/lib.git has no tag, branch or commit 'v9'")

# A branch: tz update moves to its newest commit.
file(READ ${WORK}/app/tz.json manifest)
string(REPLACE "lib.git@v1.1.0" "lib.git@main" manifest "${manifest}")
write(app/tz.json "${manifest}")
tz(run IN app STDOUT "lib 1.1: 400\n")
write(src/lib/src/label.ts [[
export const label = "lib main:"
]])
git(commit -qam three IN src/lib)
git(push -q ${WORK}/lib.git main IN src/lib)
tz(run IN app STDOUT "lib 1.1: 400\n")
tz(update IN app)
string(REGEX MATCH "^updated git:<work>/lib.git [0-9a-f]+ -> [0-9a-f]+\n$" ok "${TZ_STDOUT}")
if(NOT ok)
  message(FATAL_ERROR "tz update printed:\n${TZ_STDOUT}")
endif()
tz(run IN app STDOUT "lib main: 400\n")

# The sources in the cache are checked against tz.lock.
file(READ ${WORK}/app/tz.lock lock)
string(REGEX MATCH "\"ref\": \"main\",\n      \"commit\": \"([0-9a-f]+)\"" ignored "${lock}")
string(SUBSTRING "${CMAKE_MATCH_1}" 0 16 commit)
file(GLOB label ${WORK}/cache/git/lib-*/${commit}/src/label.ts)
file(READ ${label} saved)
file(APPEND ${label} "// changed\n")
tz(run IN app EXIT 1 STDERR "are not those tz.lock names")
file(WRITE ${label} "${saved}")

# What tz.lock names is in the cache: the repositories are not needed.
file(REMOVE_RECURSE ${WORK}/lib.git ${WORK}/numbers.git)
tz(run IN app STDOUT "lib main: 400\n")
tz(add github:nobody IN app EXIT 1 STDERR
   "tz: 'github:nobody' does not name a GitHub repository: write 'github:user/repo@v1.2.0'")
