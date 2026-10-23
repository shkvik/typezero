// JavaScript that a compiled program without holes in arrays cannot have:
// `in` and `delete` on arrays, code from strings, Proxy; and tagged
// templates used wrongly.
const arr = [1, 2, 3]
const i = 1
const s = "abc"
// expect-error: operator 'in' is not supported for arrays, which have no holes; compare the index with the length instead: '1 < arr.length'
const a1 = 1 in arr
// expect-error: compare the index with the length instead: 'i >= 0 && i < arr.length'
const a2 = i in arr
// expect-error: operator 'in' cannot be applied to types 'string' and 'string'
const a3 = "x" in s
// expect-error: 'delete' would leave a hole in the array, and arrays have none; remove the element with 'arr.splice(0, 1)', or replace it: 'arr[0] = ...'
delete arr[0]
// expect-error: remove the element with 'a.splice(i, 1)', or replace it: 'a[i] = ...'
delete [1][i]
// expect-error: strings are immutable; a character cannot be deleted
delete s[0]
// expect-error: 'delete' removes fields of objects, and this is not a field
delete arr

// expect-error: 'eval' is not supported: a compiled program cannot run code from a string
eval("1 + 1")
// expect-error: 'new Function' is not supported: a compiled program cannot run code from a string; write the function itself, e.g. '(a: int) => a'
const f = new Function("a", "return a")
// expect-error: 'Function' is not supported: a compiled program cannot run code from a string
const g = Function("a", "return a")
// expect-error: 'new Proxy' is not supported: the fields of an object are read and written straight in memory, so nothing can intercept that
const p = new Proxy(arr, arr)

function tag(texts: TemplateStringsArray, x: int): int {
  // expect-error: 'raw' is not supported: a tag gets the texts of its template as a plain 'string[]', with the escapes already applied
  return texts.raw.length + x
}
// expect-error: expected 2 arguments, but the tagged template passes 3: the array of its texts and 2 values
tag`a${1}b${2}`
// expect-error: expected 2 arguments, but the tagged template passes 1: the array of its texts
tag`a`
// expect-error: type 'string' is not assignable to type 'int'
tag`a${"x"}`
// expect-error: String.raw can only tag a template, e.g. String.raw`C:\dir`
const raw = String.raw("x")

// `void x` is undefined, which a function returning int cannot give.
function count(): int {
  // expect-error: type 'undefined' is not assignable to type 'int'
  return void 0
}
// expect-error: cannot infer a type from 'undefined'; add one, e.g. 'let nothing: int | undefined'
let nothing = void count()
// expect-error: operator '-' cannot be applied to type 'int[]'
const negated = -arr
