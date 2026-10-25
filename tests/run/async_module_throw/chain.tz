let n = 0

export function chain() {
  console.log("turn", n++)
  if (n < 10) queueMicrotask(chain)
}
