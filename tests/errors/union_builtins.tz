// instanceof Date, Map and Set narrow a union; where the type already says
// the answer, it is an error, as for classes.
function test(v: Date | string, w: Map<string, int> | Map<int, int>, r: Record<string, int> | int[]) {
  // expect-error: a 'Date | string' is never a 'Map': none of its members is one, so this is always false
  console.log(v instanceof Map)
  // expect-error: every member of 'Map<string, int> | Map<int, int>' is a 'Map', so 'instanceof Map' is always true
  console.log(w instanceof Map)
  // expect-error: a 'Record<string, int> | int[]' is never a 'Set': none of its members is one
  console.log(r instanceof Set)
}
// expect-error: the value is a 'Set<int>', so 'instanceof Set' is always true
console.log(new Set<int>() instanceof Set)
// expect-error: the keys of a Record are strings, found '1 | 2'
const byNumber: Record<1 | 2, string> = {}
console.log(byNumber, test)
// A key of literals reads any field, and assigns only fields of a plain type,
// which Sema knows nothing of from assignments by name.
function setOpt(o: Record<"a" | "b", int | undefined>, k: "a" | "b") {
  console.log(o[k])
  // expect-error: fields of type 'int | undefined' are assigned by name
  o[k] = 1
}
console.log(setOpt)
