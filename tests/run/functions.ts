// same-as-node
function fib(n: int): int {
  if (n < 2) return n
  return fib(n - 1) + fib(n - 2)
}

function factorial(n: long): long {
  let result: long = 1
  for (let i: long = 2; i <= n; i++) result *= i
  return result
}

// Functions can be called before their declaration.
console.log(square(7))
function square(x: double): double {
  return x * x
}

console.log(fib(20))
console.log(factorial(20))
console.log(gcd(1071, 462))

function gcd(a: int, b: int): int {
  while (b != 0) {
    let t = b
    b = a % b
    a = t
  }
  return a
}

function sign(x: double): int {
  if (x > 0) {
    return 1
  } else if (x < 0) {
    return -1
  } else {
    return 0
  }
}
console.log(sign(-2.5), sign(0), sign(3))

function untilThree(): int {
  let i: int = 0
  while (true) {
    i++
    if (i == 3) return i
  }
}
console.log(untilThree())

function greet(): void {
  console.log("hi from a void function")
  return
}
greet()

// Top-level variables are globals shared by all functions.
let counter: int = 0
function bump(): void {
  counter += 1
}
bump()
bump()
bump()
console.log(counter)
