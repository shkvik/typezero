// Results used wrongly: compared, forgotten, read without a check, tested
// as conditions, turned into text, and given functions of the wrong kind.
function save(s: string): Result<void, string> {
  return s == "" ? err("empty") : ok()
}
function parse(s: string): Result<int, string> {
  return s == "" ? err("empty") : ok(s.length)
}

// A Result nobody looks at hides its error.
// expect-error: the Result is not used, so an error in it would go unchecked
save("x")
// expect-error: the Result is not used
parse("1").map(n => n + 1)
const names = ["a", "b"]
// expect-error: the function returns a Result that its caller drops
names.forEach(s => save(s))
// expect-error: the Result is not used
for (let i = 0; i < 2; i++, save("loop")) {}
// expect-error: the Results are not used, so errors in them would go unchecked
names.map(s => save(s))
const r = parse("y")

// In JavaScript two Results are two objects, never equal.
// expect-error: Results cannot be compared with '==': in JavaScript 'ok(1) == ok(1)' is false
if (r == parse("y")) console.log("same")
// expect-error: Results cannot be compared with '!='
console.log(ok(1) != ok(1))
// expect-error: 'includes' cannot compare Results
console.log([r].includes(r))

// The value is there only in an Ok, the error only in an Err.
// expect-error: 'value' is there only when the Result is an Ok: check it first, 'if (r.isOk())'
console.log(r.value)
// expect-error: 'error' is there only when the Result is an Err: check it first, 'if (r.isErr())'
console.log(r.error)
if (r.isOk()) {
  // expect-error: 'error' is there only when the Result is an Err
  console.log(r.error)
}
let changing = parse("z")
if (changing.isOk()) {
  changing = parse("")
  // expect-error: 'value' is there only when the Result is an Ok
  console.log(changing.value)
}
// expect-error: 'map' is a method of Result; call it: 'r.map(...)'
const m = r.map
// expect-error: property 'unwrap' does not exist on type 'Result<int, string>'
r.unwrap()

// A Result is always truthy, an Err too.
// expect-error: a Result is always truthy, an Err too, as both are objects in JavaScript; test it: 'r.isOk()'
if (r) console.log("ok?")
// expect-error: test it: 'r.isErr()'
if (!r) console.log("failed?")
// expect-error: a Result cannot be converted to a string: JavaScript would give '[object Object]'
console.log(`result: ${r}`)
// expect-error: Results cannot be converted to strings
console.log([r].join())

// The functions given to the methods.
// expect-error: the function given to 'andThen' must return a Result, 'ok(...)' or 'err(...)', found 'int'; to change the value, use 'map'
const a = r.andThen(n => n + 1)
// Errors (and values) of different types make a union, `string | int`, unless
// no union can hold them.
const lists: Result<int[], string[]> = ok([1])
const none = ["none"]
// expect-error: the Result has errors of 'string[]' and the function given to 'andThen' returns errors of 'int[]', which no union holds: 'string[]' and 'int[]' cannot be told apart when the program runs
const b = lists.andThen(n => err(n))
// expect-error: the functions given to 'match' return 'int[]' and 'string[]', and no type holds both
const c = lists.match(n => n, e => e)
// expect-error: 'unwrapOr' gives 'int[]' for an Ok and 'string[]' otherwise, and no type holds both
const d = lists.unwrapOr(none)
// expect-error: the Result has values of 'int[]' and the function given to 'orElse' returns values of 'string[]', which no union holds
const e = lists.orElse(e => ok(e))
// expect-error: type 'Result<string, string>' is not assignable to type 'Result<int, string>'
const f: Result<int, string> = ok("one")
// expect-error: an expression of type 'void' has no value to keep; write 'ok()'
const g = ok(console.log("x"))

// combine.
// expect-error: 'Result.combine' takes Results, found 'int'
const k = Result.combine([1, 2])
// expect-error: 'Result.combine' needs Results that hold values, found 'Result<void, string>'
const l = Result.combine([save("a")])

// Types.
// expect-error: 'Result' takes two type arguments, the value and the error, e.g. 'Result<int, string>'
const n: Result<int> = ok(1)
// expect-error: 'Result' needs the types of its value and its error, e.g. 'Result<int, string>'
let o: Result
// expect-error: 'ok' can be used as a value only where a function type is expected
const q = ok
// expect-error: 'Ok' can be used only as a type; make a Result with 'ok(value)'
const s = Ok
