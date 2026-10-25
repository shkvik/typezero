// Generic variants of methods (docs/overloads.md, step 3): a variant with
// type parameters takes the types its arguments give them, and fits as well
// as those types do; at equal fit, a variant without type parameters wins,
// as in C#. Not compared with node, which runs only the last body.
class Fmt {
  show(x: int) { return `int ${x}` }
  show<T>(x: T[]) { return `array of ${x.length}` }
  show<T>(x: T, label: string) { return `${label}: ${typeof x}` }
  pick(x: double) { return "double" }
  pick<T>(x: T) { return "generic" }
  exact(x: int) { return "int" }
  exact<T>(x: T) { return "generic" }
  static wrap(x: string) { return [x] }
  static wrap<T>(x: T, n: int) {
    const out: T[] = []
    for (let i = 0; i < n; i++) out.push(x)
    return out
  }
}
const f = new Fmt()
console.log(f.show(1), f.show([1, 2]), f.show(["a"]), f.show(true, "flag"))
// An int fits T = int exactly, and double only by widening.
console.log(f.pick(1.5), f.pick(1), f.pick("s"))
// Both fit an int exactly: the variant without type parameters.
console.log(f.exact(1), f.exact("s"), f.exact([1]))
console.log(Fmt.wrap("a"), Fmt.wrap(7, 3), Fmt.wrap("b", 2))

// In a generic class, with its own type parameter too.
class Store<T> {
  items: T[] = []
  add(x: T) { this.items.push(x); return this }
  add(xs: T[]) { for (const x of xs) this.items.push(x); return this }
  find(i: int): T | undefined { return this.items[i] }
  find<K>(key: K, by: (x: T) => K): T | undefined {
    for (const x of this.items) if (by(x) == key) return x
    return undefined
  }
}
const st = new Store<string>().add("ab").add(["cde", "f"])
console.log(st.items, st.find(1), st.find(1, s => s.length), st.find("f", s => s))
