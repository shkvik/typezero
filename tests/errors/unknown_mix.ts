// unknown with unions, the values of the runtime and exceptions: what needs
// a check first.
const u: unknown = process.argv.length > 9 ? 1 : "one"
// expect-error: 'instanceof Map' cannot tell the types of the keys and values, which tz needs to use a Map; keep it in a variable of its type, e.g. 'Map<string, int>'
console.log(u instanceof Map)
const wide: unknown | int = u
// expect-error: operator '+' cannot be applied to types 'unknown' and 'int'
console.log(wide + 1)
try {
  throw new Error("x")
} catch (e: unknown) {
  // expect-error: an unknown value has no property 'message' until a check shows what it holds
  console.log(e.message)
}
