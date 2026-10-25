# The formatter of the VS Code extension (editors/vscode/format.js, Prettier
# from third_party/prettier), laid out as install.ps1 lays it out: it formats
# tz as Prettier formats TypeScript, indents by the editor's settings, keeps
# the ends of lines, and parses every .tz file of the repository but those of
# tests/errors, so that the syntax of tz stays one Prettier reads. Needs node.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("node is not installed")
endif()

get_filename_component(root ${SOURCE}/../.. ABSOLUTE)
file(COPY ${root}/editors/vscode/format.js DESTINATION ${WORK}/ext)
file(COPY ${root}/third_party/prettier/ DESTINATION ${WORK}/ext/prettier)

# format(<arguments>...): node format.cjs with them; the output in FORMAT_OUT.
function(format)
  execute_process(COMMAND ${NODE} ${SOURCE}/format.cjs ${WORK}/ext/format.js ${ARGN}
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "format.cjs ${ARGN}: exit code ${code}\n${out}${err}")
  endif()
  set(FORMAT_OUT "${out}" PARENT_SCOPE)
endfunction()

# What tz has beyond TypeScript's types (int, double, decimal, overloads with
# bodies) is TypeScript's syntax, and Prettier formats it.
write(messy.tz [=[
import {readFileSync} from 'node:fs'
enum Op { Add, Sub }
class Vec {
	x: double
	y: double
	constructor(x: double, y: double) { this.x = x; this.y = y }
	add(k: int): Vec { return new Vec(this.x + k, this.y + k) }
	add(v: Vec): Vec { return new Vec(this.x + v.x, this.y + v.y) }
}
const price: decimal = 19.99
const words = readFileSync('words.txt', 'utf8').split('\n').filter(w => w.length > 3).map(w => w.toUpperCase())
console.log(new Vec(1, 2).add(3), price * 2, words, Op.Add)
]=])
set(formatted [=[
import { readFileSync } from "node:fs";
enum Op {
  Add,
  Sub,
}
class Vec {
  x: double;
  y: double;
  constructor(x: double, y: double) {
    this.x = x;
    this.y = y;
  }
  add(k: int): Vec {
    return new Vec(this.x + k, this.y + k);
  }
  add(v: Vec): Vec {
    return new Vec(this.x + v.x, this.y + v.y);
  }
}
const price: decimal = 19.99;
const words = readFileSync("words.txt", "utf8")
  .split("\n")
  .filter((w) => w.length > 3)
  .map((w) => w.toUpperCase());
console.log(new Vec(1, 2).add(3), price * 2, words, Op.Add);
]=])
format(print ${WORK}/messy.tz)
if(NOT FORMAT_OUT STREQUAL formatted)
  message(FATAL_ERROR "messy.tz formats differently\n--- expected:\n${formatted}\n--- actual:\n${FORMAT_OUT}")
endif()

# Indented by tabs when the editor indents by tabs.
format(print ${WORK}/messy.tz 4 tabs)
string(FIND "${FORMAT_OUT}" "class Vec {\n\tx: double;\n" found)
if(found EQUAL -1)
  message(FATAL_ERROR "messy.tz is not indented by tabs:\n${FORMAT_OUT}")
endif()

# Lines end as they end in the file, "\r\n" or "\n".
foreach(style CRLF UNIX)
  file(CONFIGURE OUTPUT ${WORK}/${style}.tz CONTENT "const a = 1\nconsole.log(a)\n"
       NEWLINE_STYLE ${style})
  format(ends ${WORK}/${style}.tz)
  set(expected "crlf 0, lf 2\n")
  if(style STREQUAL "CRLF")
    set(expected "crlf 2, lf 0\n")
  endif()
  if(NOT FORMAT_OUT STREQUAL expected)
    message(FATAL_ERROR "${style}.tz: the lines formatted end as '${FORMAT_OUT}', expected '${expected}'")
  endif()
endforeach()

format(check ${root}/tests ${root}/examples ${root}/src/lib ${root}/bench)
