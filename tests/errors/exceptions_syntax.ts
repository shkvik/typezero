// The forms of throw and try that are not JavaScript, or not supported.
function f() {
  // expect-error: 'throw' needs a value on the same line, e.g. 'throw new Error("...")'
  throw
  new Error("x")
}

// expect-error: 'try' needs 'catch' or 'finally' after its block
try {
  f()
}
console.log("next")

try {
  f()
  // expect-error: destructuring the caught error is not supported; write 'catch (e)'
} catch ({ message }) {
  console.log(message)
}

// expect-error: 'catch' without 'try': it goes right after the block of a 'try'
catch (e) {
}
