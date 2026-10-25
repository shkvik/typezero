// Dictionaries: the traps of JavaScript and what a dictionary cannot do.
const m = new Map<string, int>()
const s = new Set<int>()
const r: Record<string, int> = {}

// Types.
// expect-error: the keys of a Record are strings: JavaScript would turn a number key into a string, so 'r[1]' and 'r["1"]' would be one; for numbers use a Map, e.g. 'Map<int, string>'
const byNumber: Record<int, string> = {}
// expect-error: 'Map' needs the types of its keys and values, e.g. 'Map<string, int>'
let untyped: Map<string>
// expect-error: 'new Map()' needs the types of its keys and values, e.g. 'new Map<string, int>()'
const noTypes = new Map()
// expect-error: 'new Set()' needs the type of its elements, e.g. 'new Set<int>()'
const noElement = new Set()
// expect-error: a Result cannot be a key: Results are not compared
const results = new Set<Result<int, string>>()
// expect-error: 'WeakMap' is not supported yet; a 'Map' holds its keys, and frees them with itself
const weak = new WeakMap<object, int>()

// A Map and a Set have entries, not properties.
// expect-error: 'm[key]' reads a property of the Map object in JavaScript, not one of its entries; read an entry with 'm.get(key)'
console.log(m["a"])
// expect-error: set an entry with 'm.set(key, value)'
m["a"] = 1
// expect-error: 'm.x = ...' sets a property of the Map object in JavaScript
m.x = 2
// expect-error: 'delete' removes a property of the Map object in JavaScript, not an entry; remove one with 'm.delete(key)'
delete m["a"]
// expect-error: 'in' asks for a property of the Map object in JavaScript, not for an entry; ask with 'm.has(key)'
console.log("a" in m)
// expect-error: a Set has no elements by position; check one with 's.has(value)', or copy them into an array, '[...s]'
console.log(s[0])
// expect-error: 'Object.keys' of a Map is always empty in JavaScript: its entries are no properties; use '[...m.keys()]'
console.log(Object.keys(m))
// expect-error: 'for...in' walks the properties of an object, and a Map has none in JavaScript; walk its keys with 'for (const x of m.keys())'
for (const k in m) console.log(k)
// expect-error: the number of entries is 'm.size'
console.log(m.length)
// expect-error: a Set adds a value with 's.add(value)' and checks one with 's.has(value)'
console.log(s.get(1))
// expect-error: a Map sets an entry with 'm.set(key, value)'
m.add(1)
// expect-error: call it on an iterator, e.g. 'm.entries().map(...)', or on an array, '[...m].map(...)'
m.map(1)
// expect-error: 'm.get' can only be called, e.g. 'm.get(...)'
const getter = m.get
// expect-error: a Map cannot be converted to a string: JavaScript would give '[object Map]'
console.log(`${m}`)
// expect-error: a Set cannot be converted to a string: JavaScript would give '[object Set]'
console.log(String(s))
// expect-error: 'union' needs a 'Set<int>' or a 'Map<int, ...>', found 'int[]'; make a Set first, e.g. 'new Set(xs)'
console.log(s.union([1]))
// expect-error: type 'int' is not assignable to type 'string'
m.set(1, 2)

// A key of a Record may be missing.
// expect-error: a Record is not iterable, as an object is not in JavaScript; walk its keys with 'for...in'
for (const x of r) console.log(x)
// expect-error: 'r.a' may be undefined, as a key may be missing: give it a default with '??', e.g. 'r.a = (r.a ?? 0) + 1'
r.a += 1
// expect-error: 'r[k]' may be undefined, as a key may be missing
r["b"]++
// expect-error: the keys of a Record are strings, and JavaScript would turn this 'int' into one, so 1 and "1" would be one key; convert it, 'String(i)', or use a Map, whose keys may be numbers
console.log(r[0])
// expect-error: the value may be undefined; check it before calling 'toFixed'
console.log(r.a.toFixed(2))
// expect-error: a Record inherits nothing from Object.prototype, and 'r.hasOwnProperty' is a key like any other; ask for a key with 'key in r'
r.hasOwnProperty("a")
const nested: Record<string, Record<string, int>> = {}
// expect-error: the value may be undefined; check it before assigning to 'x'
nested.a.x = 1
// expect-error: only objects can be spread into an object literal, found 'Map<string, int>'
const spread = { ...m }
// expect-error: only objects and Records can be spread into a Record, found 'Set<int>'; in JavaScript a Map or a Set has no own properties, so this would add nothing
const fromSet: Record<string, int> = { ...s }

// Building them.
// expect-error: the values of a Map have one type; found 'int' and 'string'
const mixed = new Map([["a", 1], ["b", "x"]])
// expect-error: type 'string' is not assignable to type 'int' of element 1
const wrong = new Map<string, int>([["a", "b"]])
// expect-error: 'new Map' takes [key, value] pairs, found 'int'
const notPairs = new Map([1, 2])
// expect-error: the keys of an object are strings, and JavaScript would turn 'int' keys into them; return a string, or group into a Map: 'Map.groupBy(...)'
const grouped = Object.groupBy([1, 2], x => x)
// expect-error: the keys of an object are strings, and JavaScript would turn 'int' keys into them; convert them first, or make a Map: 'new Map(...)'
const entries = Object.fromEntries([[1, "a"]])
// expect-error: JSON has objects, not a Map; read a 'Record<string, T>' and make the Map of it, 'new Map(Object.entries(r))'
console.log(JSON.parse<Map<string, int>>("{}"))
