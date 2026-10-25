// Сколько простых чисел меньше миллиона. Делители перебираются до корня.
function isPrime(n: int): boolean {
  if (n < 2) return false
  if (n % 2 == 0) return n == 2
  for (let d = 3; d * d <= n; d += 2) {
    if (n % d == 0) return false
  }
  return true
}

let count = 0
for (let n = 0; n < 1000000; n++) {
  if (isPrime(n)) count++
}
console.log("простых чисел меньше миллиона:", count)
