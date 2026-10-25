// Top-level code that calls a function before a variable the function uses
// is declared: JavaScript would throw a ReferenceError there.
function total(): int {
  return base + 1
}
// expect-error: 'total' uses 'base' before it is declared: JavaScript would throw a ReferenceError when it runs here; move the declaration of 'base' up
console.log(total())
const base = 5
const items = [1, 2]
// expect-error: the function uses 'unit' before it is declared
items.forEach(i => show(i))
function show(i: int) {
  console.log(`${i}${unit}`)
}
const unit = "cm"
// expect-error: 'twice' uses 'seed' before it is declared
const seed = twice()
function twice(): int {
  return seed * 2
}
// Called after everything is set: fine.
const later = () => total() + base
console.log(later())
