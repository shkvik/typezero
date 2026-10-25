// Writes through a wider type that the check finds although it leaves out
// what types rule out (docs/objects.md, section 2): an array seen as an
// unknown and narrowed back, one kept in a field of an object in a tuple,
// and one in a dictionary.
class Animal {
  constructor(public name: string) {}
}
class Dog extends Animal {}
class Cat extends Animal {}
const dogs: Dog[] = [new Dog("Rex")]

function viaUnknown(a: Animal[]) {
  const u: unknown = a
  if (Array.isArray(u)) u.push(new Cat("Tom"))
}
// expect-error: line 14 puts a 'Cat' into it, and an array of 'Dog' cannot hold one
viaUnknown(dogs)

class Box {
  constructor(public items: Animal[]) {}
}
function viaBox(a: Animal[]) {
  const pairs: [string, Box][] = [["x", new Box(a)]]
  for (const [, box] of pairs) box.items.push(new Cat("Tim"))
}
// expect-error: line 24 puts a 'Cat' into it, and an array of 'Dog' cannot hold one
viaBox(dogs)

function viaMap(a: Animal[]) {
  const m = new Map<string, Animal[]>()
  m.set("k", a)
  m.get("k")?.push(new Cat("Max"))
}
// expect-error: line 32 puts a 'Cat' into it, and an array of 'Dog' cannot hold one
viaMap(dogs)
