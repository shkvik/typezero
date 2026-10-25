// Writes through a wider type, of read-only fields (docs/objects.md, section
// 2): an object with a read-only field may go where the field may change, and
// back, only if no code changes it through that type.
interface Settings { readonly port: int }
function resetPort(s: { port: int }) { s.port = 0 }
const settings: Settings = { port: 8080 }
// expect-error: 'Settings' cannot go where '{ port: int }' is expected here: line 5 writes to its field 'port', which is read-only in 'Settings'
resetPort(settings)
function bump(all: { port: int }[]) { for (const s of all) s.port++ }
const all: Settings[] = [settings]
// expect-error: 'Settings[]' cannot go where '{ port: int }[]' is expected here: line 9 writes to field 'port' of a 'Settings' in it, which is read-only in 'Settings'
bump(all)
// expect-error: 'Settings' cannot go where '{ port: int }' is expected here: line 9 writes to its field 'port', which is read-only in 'Settings'
bump([settings])

// A class that implements an interface with a read-only field.
interface Named { name: string }
class Fixed implements Named { readonly name = "fixed" }
function rename(n: Named) { n.name += "!" }
// expect-error: 'Fixed' cannot go where 'Named' is expected here: line 19 writes to its field 'name', which is read-only in 'Fixed'
rename(new Fixed())

// A read-only tuple.
const origin: readonly [int, int] = [0, 0]
function move(p: [int, int]) { p[0] = 1 }
// expect-error: 'readonly [int, int]' cannot go where '[int, int]' is expected here: line 25 writes to its element 0, which is read-only in 'readonly [int, int]'
move(origin)
