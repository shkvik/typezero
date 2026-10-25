// `yield` where JavaScript does not allow it.
function plain() {
  // expect-error: 'yield' can only be used in a generator function, declared with 'function*'
  yield 1
}
function* inArrow() {
  const f = () => {
    // expect-error: 'yield' can only be used in a generator function, declared with 'function*'
    yield 2
  }
}
function* operand() {
  // expect-error: 'yield' must be in parentheses here, e.g. '1 + (yield x)'
  const x = 1 + yield 3
}
// expect-error: 'yield' can only be used in a generator function, declared with 'function*'
yield 4
