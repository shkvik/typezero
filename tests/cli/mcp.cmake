# The tools for AI agents, tz mcp (docs/lsp.md, step 2): mcp.cjs talks to it
# as an agent does and calls each tool - map, context, query, impact,
# check_edit (an edit checked in memory, nothing written) and language_card;
# answers come from the records of the last check until a file changes.
# Needs node.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("node is not installed")
endif()

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

execute_process(COMMAND ${NODE} ${SOURCE}/mcp.cjs ${TZ} ${WORK}/shapes
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "mcp.cjs: exit code ${code}\n${out}${err}")
endif()
file(READ ${WORK}/shapes/vec.tz text)
string(FIND "${text}" "return sum.scale(1 / n.toDouble())" found)
if(found EQUAL -1)
  message(FATAL_ERROR "check_edit wrote vec.tz:\n${text}")
endif()
