// same-as-node
// Optional parameters and default values.
function greet(name: string, greeting: string = "Hello", mark?: string): string {
  return `${greeting}, ${name}${mark ?? "."}`
}
console.log(greet("Ann"))
console.log(greet("Bob", "Hi"))
console.log(greet("Cy", "Hey", "!"))
// undefined takes the default too.
console.log(greet("Di", undefined, "?"))

// A default value sees the parameters before it, and is computed at each call.
let made = 0
function stamp(): int {
  made++
  return made
}
function box(width: int, height: int = width, id: int = stamp()): string {
  return `${width}x${height} #${id}`
}
console.log(box(3), box(2, 5), box(4, 4, 99), box(1))
console.log("stamps", made)

// Without an annotation, a literal default gives the type.
function repeat(text: string, times = 2, separator = ", "): string {
  const parts: string[] = []
  for (let i = 0; i < times; i++) parts.push(text)
  return parts.join(separator)
}
console.log(repeat("la"), repeat("ho", 3), repeat("x", 4, ""))

// An optional parameter is undefined when left out.
function describe(n?: int): string {
  if (n === undefined) return "nothing"
  return "number " + (n + 1)
}
console.log(describe(), describe(41))

// Arrays and strings as defaults are new on every call.
function append(value: int, list: int[] = []): int[] {
  list.push(value)
  return list
}
console.log(append(1), append(2), append(3, [1, 2]))

// A default for a parameter that may itself be undefined.
function pick(a: int | undefined = 7): int {
  return a ?? -1
}
console.log(pick(), pick(undefined), pick(3))
