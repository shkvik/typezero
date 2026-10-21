// Числа Фибоначчи: рекурсия и цикл.
function fib(n: int): int {
  if (n < 2) return n
  return fib(n - 1) + fib(n - 2)
}

for (let i = 0; i <= 10; i++) {
  console.log("fib", i, "=", fib(i))
}
console.log("fib 35 =", fib(35))
