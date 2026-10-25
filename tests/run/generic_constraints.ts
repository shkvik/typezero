// same-as-node --experimental-transform-types
// Constraints with fields and classes (docs/generics.md, section 4): `T
// extends { name: string }` takes any object with at least these fields,
// and each copy of the function reads them where its type has them;
// `T extends Animal` keeps the exact type of the argument.
interface User { id: int; name: string; email: string }
interface Post { title: string; name: string }
const ann: User = { id: 1, name: "ann", email: "ann@x" }
const post: Post = { title: "hi", name: "ann's post" }

function greet<T extends { name: string }>(x: T) { return `hello, ${x.name}` }
console.log(greet(ann), greet(post), greet({ name: "literal", extra: true }))
function names<T extends { name: string }>(xs: T[]) { return xs.map(x => x.name) }
console.log(names([ann, { id: 2, name: "bob", email: "" }]), names([post]))
function longest<T extends { name: string }>(xs: T[]): T | undefined {
  let best: T | undefined = undefined
  for (const x of xs) if (best == undefined || x.name.length > best.name.length) best = x
  return best
}
console.log(longest([post, { title: "t", name: "x" }]), longest<User>([]))

// A method in the constraint: a class that has it fits.
interface Shape { area(): double }
class Circle {
  constructor(public r: double) {}
  area() { return Math.PI * this.r * this.r }
}
class Rect {
  constructor(public w: double, public h: double) {}
  area() { return this.w * this.h }
}
function totalArea<T extends { area(): double }>(xs: T[]) {
  let total = 0.0
  for (const x of xs) total += x.area()
  return total
}
console.log(totalArea([new Circle(1)]).toFixed(3), totalArea([new Rect(2, 3), new Rect(1, 1)]))

// A class constraint: the result is of the exact class of the argument.
class Animal {
  constructor(public name: string) {}
  speak() { return `${this.name} makes a sound` }
}
class Dog extends Animal {
  speak() { return `${this.name} barks` }
  fetch() { return `${this.name} fetches` }
}
function loudest<T extends Animal>(xs: T[]): T | undefined {
  let best: T | undefined = undefined
  for (const x of xs) if (best == undefined || x.speak().length > best.speak().length) best = x
  return best
}
const rex = loudest([new Dog("rex"), new Dog("fido")])
console.log(rex?.fetch(), loudest([new Animal("cat"), new Dog("pug")])?.speak())

// Generic classes with constraints.
class Ranked<T extends { name: string }> {
  items: T[] = []
  add(x: T) { this.items.push(x); return this }
  names() { return this.items.map(x => x.name).join(", ") }
}
console.log(new Ranked<User>().add(ann).names(), new Ranked<Post>().add(post).add(post).names())
class Total<T extends number> {
  sum = 0.0
  add(x: T) { this.sum += x; return this }
}
console.log(new Total<int>().add(1).add(2).sum, new Total<double>().add(0.5).sum)
