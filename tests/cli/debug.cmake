# What the .pdb next to an executable holds (docs/debug.md): `tz build` always
# writes the lines of the .tz files and the functions, for the stack of a
# panic; `tz build --debug` adds the variables with their types, named as the
# debugger views of tz.natvis expect, and puts tz.natvis into the .pdb.
include(${SOURCE}/../cli.cmake)

write(prog.tz [[
class User {
  public readonly name: string
  public readonly tags: string[]
  public constructor(name: string) {
    this.name = name
    this.tags = ["a"]
  }
  public greet(other: string): string {
    const text = `${this.name} greets ${other}`
    return text
  }
}

function look(count: int, mixed: string | int): string {
  const user = new User("Ann")
  const numbers = [1, 2, 3]
  const scores = new Map<string, int>()
  scores.set("x", 1)
  const parsed = parseInt("12")
  return `${user.greet("Bob")} ${count} ${mixed} ${numbers.length} ${scores.size} ${parsed.unwrapOr(0)}`
}

console.log(look(5, "m"))
]])
tz(build --debug -o debug.exe prog.tz)
tz(build -o plain.exe prog.tz)
foreach(name debug plain)
  if(NOT EXISTS ${WORK}/${name}.pdb)
    message(FATAL_ERROR "tz build wrote no ${name}.pdb next to ${name}.exe")
  endif()
  execute_process(COMMAND ${WORK}/${name}.exe OUTPUT_VARIABLE out RESULT_VARIABLE code)
  if(NOT code EQUAL 0 OR NOT out STREQUAL "Ann greets Bob 5 m 3 1 12\n")
    message(FATAL_ERROR "${name}.exe exited with ${code} and printed:\n${out}")
  endif()
endforeach()
if(NOT PDBUTIL)
  skip("llvm-pdbutil is not found")
endif()

# Symbols come from module 0, the object of the program: the C runtime of
# Windows has variables of its own.
function(dump name what var)
  execute_process(COMMAND ${PDBUTIL} dump ${what} ${ARGN} ${WORK}/${name}.pdb OUTPUT_VARIABLE out
                  RESULT_VARIABLE code)
  if(code)
    message(FATAL_ERROR "llvm-pdbutil dump ${what} ${name}.pdb failed")
  endif()
  set(${var} "${out}" PARENT_SCOPE)
endfunction()

dump(debug --symbols symbols --modi=0)
foreach(text "`look`" "`User.greet`" "`new User`" "`<top level>`" "S_LOCAL [size = 16] `count`"
        "`mixed`" "`user`" "`numbers`" "`scores`" "`parsed`" "`this`" "`text`")
  string(FIND "${symbols}" "${text}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "debug.pdb has no ${text} among its symbols")
  endif()
endforeach()
dump(debug --types types)
foreach(text "`User`" "`string`" "`Array<int>`" "`Array<string>`" "`Map<string, int>`"
        "`Union<string | int>`" "`Result<int, NumberError>`")
  string(FIND "${types}" "${text}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "debug.pdb has no type ${text}")
  endif()
endforeach()
dump(debug --named-streams streams)
if(NOT streams MATCHES "/src/files/[^\n]*tz\\.natvis")
  message(FATAL_ERROR "debug.pdb does not hold tz.natvis:\n${streams}")
endif()

dump(plain --symbols symbols --modi=0)
string(FIND "${symbols}" "`<top level>`" found)
if(found EQUAL -1)
  message(FATAL_ERROR "plain.pdb has no <top level> among its symbols")
endif()
dump(plain -l lines)
if(NOT lines MATCHES "prog\\.tz")
  message(FATAL_ERROR "plain.pdb has no lines of prog.tz")
endif()
string(FIND "${symbols}" "S_LOCAL" found)
if(NOT found EQUAL -1)
  message(FATAL_ERROR "plain.pdb has variables: only --debug writes them")
endif()
dump(plain --named-streams streams)
if(streams MATCHES "natvis")
  message(FATAL_ERROR "plain.pdb holds tz.natvis: only --debug puts it in")
endif()
