// same-as-node
const LIMIT = 10
function clamp(x: int, max = LIMIT * 2) {
  return x > max ? max : x
}
console.log(clamp(5), clamp(50), clamp(50, 30))
function greet(name: string, greeting = "Hello".toUpperCase()) {
  return greeting + " " + name
}
console.log(greet("Ann"), greet("Bob", "Hi"))
function span(a: int, b = a * 2, c = [a, b]) {
  return c
}
console.log(span(3), span(3, 4))
function pick(xs = [1.5, 2.5]) {
  return xs[0] ?? 0
}
console.log(pick(), pick([7.5]))
