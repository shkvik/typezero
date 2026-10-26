# Reflection of classes known when compiling (docs/decorators.md, plan step 5):
# `container.get(UserService)` of a generic `get<T>(cls: Constructor<T>)` that
# does `new cls()`, Reflect.construct and Reflect.get / Reflect.apply of a
# method by a literal name compiles to the direct `new` and calls, and the
# exe holds no description of the class (tz.classinfo.*) and no table of the
# runtime for reflection. The same program with the class known only when it
# runs keeps the description.
include(${SOURCE}/../cli.cmake)

set(classes [[
class UserService {
  name = "users"
  greet(who: string) {
    return `${this.name}: hello ${who}`
  }
}
class Container {
  get<T extends object>(cls: Constructor<T>): T {
    return new cls()
  }
  make<T extends object>(cls: Constructor<T>) {
    return Reflect.construct(cls, [])
  }
}
function call<T extends object>(obj: T, who: string): unknown {
  const greet = Reflect.get(obj, "greet")
  if (typeof greet != "function") return undefined
  return Reflect.apply(greet, obj, [who]).unwrapOr(undefined)
}
const container = new Container()
]])
write(known.tz "${classes}
const a = container.get(UserService)
const b = container.make(UserService)
if (b.isOk()) console.log(call(a, \"ann\"), call(b.value, \"bob\"), Reflect.get(b.value, \"name\"))
")
write(runtime.tz "${classes}
const kinds: Constructor[] = [UserService]
for (const kind of kinds) {
  const a = container.get(kind)
  const b = container.make(kind)
  if (b.isOk()) console.log(call(a, \"ann\"), call(b.value, \"bob\"), Reflect.get(b.value, \"name\"))
}
")
set(expected "users: hello ann users: hello bob users\n")

tz(run known.tz STDOUT "${expected}")
tz(build --emit-llvm -o known.ll known.tz)
file(READ ${WORK}/known.ll ir)
foreach(name tz.classinfo.UserService tz_reflect_init tz_reflect_get tz_reflect_apply
             tz_reflect_new tz_reflect_construct)
  string(FIND "${ir}" "${name}" found)
  if(NOT found EQUAL -1)
    message(FATAL_ERROR "known.ll has '${name}': the class is known when compiling")
  endif()
endforeach()

tz(run runtime.tz STDOUT "${expected}")
tz(build --emit-llvm -o runtime.ll runtime.tz)
expect_match(runtime.ll "@tz\\.classinfo\\.UserService = private constant")
expect_match(runtime.ll "tz_reflect_init")
