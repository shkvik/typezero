// `let` without a value needs a type; the error suggests one made from the
// values assigned below, ready to paste.
// expect-error: variable 'total' needs a type annotation or an initializer; write the type: 'let total: int'
let total
total = 0
for (const n of [1, 2]) total += n
// expect-error: cannot infer a type from 'undefined'; write the type: 'let empty: int | undefined'
let empty = undefined
empty = 5
// expect-error: write the type: 'let ratio: double'
let ratio
ratio = 1
ratio = 0.5
// expect-error: write the type: 'let names: string[]'
let names
names = ["a", "b"]
// expect-error: write the type: 'let late: string | undefined'
let late
function setLate(): void {
  late = undefined
  late = "set"
}
function inner(): void {
  // expect-error: write the type: 'let message: string'
  let message
  if (total > 1) message = "many"
  else message = "few"
}

// Nothing assigned, or values no one type holds: a general hint.
// expect-error: variable 'unused' needs a type annotation or an initializer; e.g. 'let unused: int' or 'let unused = 0'
let unused
// expect-error: cannot infer a type from 'undefined'; add one, e.g. 'let nothing: int | undefined'
let nothing = undefined
// expect-error: variable 'mixed' needs a type annotation or an initializer; e.g. 'let mixed: int'
let mixed
mixed = 1
mixed = "one"
