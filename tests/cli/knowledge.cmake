# The answers of the compiler about a program (docs/lsp.md, step 2): tz map,
# tz context, tz query and tz impact read the records of the modules that the
# check makes (KnowledgePass): declarations with their types (inferred ones
# too), references, calls, imports; docs sections and tests are found by text.
include(${SOURCE}/../cli.cmake)

write(shapes/tz.json [[
{
  "name": "shapes",
  "main": "main.tz"
}
]])
write(shapes/vec.tz [[
export class Vec {
  public x: double
  public y: double

  public constructor(x: double, y: double) {
    this.x = x
    this.y = y
  }

  public add(other: Vec): Vec {
    return new Vec(this.x + other.x, this.y + other.y)
  }

  public scale(k: double): Vec {
    return new Vec(this.x * k, this.y * k)
  }
}

export type Shape = { name: string; points: Vec[] }

export function centroid(shape: Shape): Vec {
  let sum = new Vec(0, 0)
  for (const p of shape.points) {
    sum = sum.add(p)
  }
  const n = shape.points.length
  if (n == 0) {
    return sum
  }

  return sum.scale(1 / n.toDouble())
}
]])
write(shapes/main.tz [[
import { Vec, centroid } from "./vec.tz"

const square = { name: "square", points: [new Vec(0, 0), new Vec(1, 0), new Vec(1, 1), new Vec(0, 1)] }
const c = centroid(square)
console.log(c.x, c.y)
]])
write(shapes/docs/geometry.md [[
# Geometry

## Centers

`centroid(shape)` is the average of the points of a shape.
]])
write(shapes/tests/centroid_test.tz [[
import { centroid } from "../vec.tz"
console.log(centroid({ name: "none", points: [] }).x)
]])

# The entry first, then the modules that others use most.
tz(map IN shapes STDOUT [[
program main.tz: 2 modules, 37 lines

main.tz - 5 lines; imports ./vec.tz
  const square: Shape  - 1 use, line 3
  const c: Vec  - 2 uses, line 4

vec.tz - 32 lines; imported by main.tz
  export class Vec  - 13 uses, line 1
    x: double  - 5 uses, line 2
    y: double  - 5 uses, line 3
    constructor(x: double, y: double)  - 7 uses, line 5
    add(other: Vec): Vec  - 1 use, line 10
    scale(k: double): Vec  - 1 use, line 14
  export type Shape = { name: string; points: Vec[] }  - 1 use, line 19
  export function centroid(shape: Shape): Vec  - 2 uses, line 21
]])

# Within the budget the exported and most used come first.
tz(map --budget 60 IN shapes STDOUT [[
program main.tz: 2 modules, 37 lines

main.tz - 5 lines; imports ./vec.tz
  ... 2 more (raise --budget)

vec.tz - 32 lines; imported by main.tz
  export class Vec  - 13 uses, line 1
  export function centroid(shape: Shape): Vec  - 2 uses, line 21
  ... 6 more (raise --budget)
]])

tz(context centroid IN shapes STDOUT [[

## function centroid - vec.tz:21:17 (exported)
function centroid(shape: Shape): Vec
source:
  21 | export function centroid(shape: Shape): Vec {
  22 |   let sum = new Vec(0, 0)
  23 |   for (const p of shape.points) {
  24 |     sum = sum.add(p)
  25 |   }
  26 |   const n = shape.points.length
  27 |   if (n == 0) {
  28 |     return sum
  29 |   }
  30 |
  31 |   return sum.scale(1 / n.toDouble())
  32 | }
uses (5):
  type Shape - vec.tz:19:13 - export type Shape = { name: string; points: Vec[] }
  class Vec - vec.tz:1:14
  constructor Vec.constructor - vec.tz:5:10 - constructor(x: double, y: double)
  method Vec.add - vec.tz:10:10 - add(other: Vec): Vec
  method Vec.scale - vec.tz:14:10 - scale(k: double): Vec
used by (2):
  main.tz:1:15 in top-level code of main.tz - import { Vec, centroid } from "./vec.tz"
  main.tz:4:11 in const c - const c = centroid(square)
docs:
  docs/geometry.md:5 (Centers) - `centroid(shape)` is the average of the points of a shape.
tests that mention 'centroid' (by text): tests/centroid_test.tz
]])

# A place: the innermost declaration around the line.
tz(context main.tz:4 IN shapes STDOUT [[

## const c - main.tz:4:7
const c: Vec
uses (2):
  function centroid - vec.tz:21:17 - function centroid(shape: Shape): Vec
  const square - main.tz:3:7 - const square: Shape
used by (2):
  main.tz:5:13 in top-level code of main.tz - console.log(c.x, c.y)
  main.tz:5:18 in top-level code of main.tz - console.log(c.x, c.y)
]])

tz(query definition Vec IN shapes STDOUT "vec.tz:1:14  class Vec\n")
tz(query references Vec.x IN shapes STDOUT [[
field Vec.x - 5 references
  vec.tz:6:10 in constructor Vec.constructor - this.x = x
  vec.tz:11:25 in method Vec.add - return new Vec(this.x + other.x, this.y + other.y)
  vec.tz:11:35 in method Vec.add - return new Vec(this.x + other.x, this.y + other.y)
  vec.tz:15:25 in method Vec.scale - return new Vec(this.x * k, this.y * k)
  main.tz:5:15 in top-level code of main.tz - console.log(c.x, c.y)
]])
tz(query callers centroid IN shapes STDOUT [[
function centroid - called from 1 place
  const c - main.tz:4:11
]])
# The type of a local, which the compiler inferred, and where it is used.
tz(query type vec.tz:22:7 IN shapes STDOUT "sum: Vec\n")
tz(query references vec.tz:24:5 IN shapes STDOUT [[
local sum: Vec - vec.tz:22:7 in function centroid
4 uses:
  vec.tz:24:5
  vec.tz:24:11
  vec.tz:28:12
  vec.tz:31:10
]])

tz(impact Vec.add IN shapes STDOUT [[
changing method Vec.add (vec.tz:10:10) can affect 2 symbols in 2 modules
level 1 - use it directly (1):
  function centroid - vec.tz:21:17
level 2 - use those above (1):
  const c - main.tz:4:7
modules: vec.tz, main.tz
exported among them: function centroid
]])

# --json, and the records themselves.
tz(query definition centroid --json IN shapes)
string(FIND "${TZ_STDOUT}" "\"key\": \"vec.tz#centroid\"" found)
if(found EQUAL -1)
  message(FATAL_ERROR "query --json has no key of centroid:\n${TZ_STDOUT}")
endif()
tz(query records IN shapes)
foreach(part "\"to\": \"vec.tz#Vec.add\"" "\"from\": \"vec.tz#centroid\"" "\"hash\": " "\"type\": \"Vec\"")
  string(FIND "${TZ_STDOUT}" "${part}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "the records have no ${part}:\n${TZ_STDOUT}")
  endif()
endforeach()

# A file that main.tz does not import is its own program; a line of top-level
# code is about what it uses.
tz(context tests/centroid_test.tz:2 IN shapes)
foreach(part "## function centroid - vec.tz:21:17"
             "tests/centroid_test.tz:2:13 in top-level code of tests/centroid_test.tz")
  string(FIND "${TZ_STDOUT}" "${part}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "the test file is not looked at as its own program:\n${TZ_STDOUT}")
  endif()
endforeach()

tz(context nothingLikeThis IN shapes EXIT 1
   STDERR "tz: nothing in the program is named 'nothingLikeThis'")
tz(context vec.tz:99 IN shapes EXIT 1 STDERR "tz: nothing is declared or used at vec.tz:99")
tz(query everything centroid IN shapes EXIT 2 STDERR "'tz query' takes what to find")
tz(map EXIT 2 STDERR "which program?")
