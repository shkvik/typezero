// An unknown can be used only once a check shows what it holds.
const u: unknown = 5

// expect-error: an unknown value cannot be a condition: it may hold 0, "" or false, which would count as false; check what it holds, e.g. 'u != undefined' or 'typeof u == "string"'
if (u) console.log("yes")
// expect-error: '||' cannot test an unknown value
const fallback = u || 1
// expect-error: an unknown value has no property 'length' until a check shows what it holds
console.log(u.length)
// expect-error: an unknown value has no method 'toFixed' until a check shows what it holds
console.log(u.toFixed(2))
// expect-error: an unknown value cannot be indexed until a check shows what it holds, e.g. 'if (Array.isArray(u)) ...'
console.log(u[0])
// expect-error: operator '+' cannot be applied to types 'unknown' and 'int'
const sum = u + 1
// expect-error: type 'unknown' is not assignable to type 'string'; an unknown value may hold anything: check what it holds first, e.g. 'if (typeof u == "string") ...'
const s: string = u
// expect-error: an unknown value cannot be converted to a string: it may hold an object or a function
console.log(`${u}`)
// expect-error: 'u' is not a function; it has type 'unknown'
u()

class User {
  name = "Ann"
}
const o: object = new User()
// expect-error: an object has no property 'name' until 'instanceof' shows its class, e.g. 'if (o instanceof C) ...'
console.log(o.name)
// expect-error: type 'object' is not assignable to type 'User'; an object may be of any class: check which first, e.g. 'if (o instanceof User) ...'
const user: User = o
// expect-error: type 'int[]' is not assignable to type 'object'; only objects of the language are 'object's
const list: object = [1, 2]
// expect-error: 'constructor' is not supported: an object has no prototype in tz; a decorator gets the class itself
console.log(o.constructor)

const mixed: unknown[] = [1, "a"]
// expect-error: unknown values cannot be converted to strings: one may hold an object
console.log(mixed.join(", "))
// expect-error: sorting unknown values needs a comparison function
mixed.sort()
// expect-error: an unknown cannot be a key: a key is found by its value
const byUnknown = new Map<unknown, int>()
