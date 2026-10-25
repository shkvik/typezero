// same-as-node
// Computations over types (docs/generics.md, step 4): `keyof`, `T[K]`, the
// utilities of TypeScript, mapped types of one's own and `typeof` in types.
// They exist only when compiling; each gives an exact type.
interface User { id: int; name: string; email?: string }
const ann: User = { id: 1, name: "ann" }
const bob: User = { id: 2, name: "bob", email: "b@x" }

type Key = keyof User
const key: Key = "name"
type Name = User["name"]
const name: Name = "cid"
type IdOrName = User["id" | "name"]
const either: IdOrName = 5
console.log(key, name, either)

// `K extends keyof T`: a key known at each call, read and written as a field.
function get<T, K extends keyof T>(o: T, k: K): T[K] { return o[k] }
function set<T, K extends keyof T>(o: T, k: K, value: T[K]) { o[k] = value }
console.log(get(ann, "id") + 1, get(ann, "name").toUpperCase(), get(bob, "email"))
set(ann, "name", "anna")
console.log(ann)
function pluck<T, K extends keyof T>(xs: T[], k: K): T[K][] { return xs.map(x => x[k]) }
console.log(pluck([ann, bob], "name"), pluck([ann, bob], "id"))
class Column<T, K extends keyof T> {
  private rows: T[]
  private k: K
  constructor(rows: T[], k: K) {
    this.rows = rows
    this.k = k
  }
  values(): T[K][] { return this.rows.map(r => r[this.k]) }
}
console.log(new Column([ann, bob], "email").values())

// The utilities: each a new object type.
type Preview = Pick<User, "id" | "name">
type Draft = Partial<User>
type Complete = Required<User>
type Frozen = Readonly<User>
type WithoutEmail = Omit<User, "email">
const preview: Preview = { id: 3, name: "pv" }
const drafts: Draft[] = [{}, { name: "x" }, { email: "e" }]
const complete: Complete = { id: 4, name: "c", email: "c@x" }
const frozen: Frozen = { id: 5, name: "f" }
const short: WithoutEmail = { id: 6, name: "s" }
console.log(preview, drafts, complete, frozen, short)
type Status = "draft" | "live" | "gone"
type Visible = Exclude<Status, "gone">
type Final = Extract<Status, "gone" | "lost">
const visible: Visible[] = ["draft", "live"]
const final: Final = "gone"
const present: NonNullable<string | undefined> = "here"
console.log(visible, final, present)
function area(w: double, h: double) { return w * h }
type AreaArgs = Parameters<typeof area>
type AreaResult = ReturnType<typeof area>
const args: AreaArgs = [2, 3.5]
const result: AreaResult = area(...args)
console.log(args, result)

// Mapped types of one's own, with `?`, `-?`, `readonly`, `-readonly`.
type Checks<T> = { [K in keyof T]?: (value: T[K]) => boolean }
function validate<T>(o: T, checks: Checks<T>) { return checks }
const checks = validate(ann, { id: v => v > 0, name: v => v.length > 2 })
console.log(checks.id?.(ann.id), checks.name?.("al"), checks.email)
type Getters<T> = { [K in keyof T]: () => T[K] }
function getters(u: User): Getters<User> {
  return { id: () => u.id, name: () => u.name, email: () => u.email }
}
console.log(getters(bob).email(), getters(ann).name())
type Flags = { [K in Status]: boolean }
const flags: Flags = { draft: true, live: false, gone: false }
type Mutable<T> = { -readonly [K in keyof T]: T[K] }
const thawed: Mutable<Frozen> = { id: 7, name: "t" }
thawed.name = "thawed"
console.log(flags, thawed)

// `typeof` in a type: the type of a value.
const defaults = { port: 8080, host: "localhost", debug: false }
type Config = typeof defaults
type ConfigKey = keyof typeof defaults
const config: Config = { port: 9090, host: "example.com", debug: true }
const which: ConfigKey = "host"
console.log(config, which, config[which])
