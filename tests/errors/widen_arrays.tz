// Writes through a wider type, of arrays (docs/objects.md, section 2): an
// array of Dog may go where an array of Animal is expected only if no code
// puts another Animal into it, through any path. The error is at the
// conversion and names the line of the write.
class Animal { constructor(public name: string) {} }
class Dog extends Animal {}
class Cat extends Animal {}
const dogs: Dog[] = [new Dog("Rex")]

function adopt(pets: Animal[]) { pets.push(new Cat("Tom")) }
// expect-error: 'Dog[]' cannot go where 'Animal[]' is expected here: line 10 puts a 'Cat' into it, and an array of 'Dog' cannot hold one; pass a copy, '[...dogs]'
adopt(dogs)

// Through a variable, an element written, and a function that passes it on.
// expect-error: line 17 puts a 'Cat' into it
const animals: Animal[] = dogs
animals[0] = new Cat("Tim")
function relay(xs: Animal[]) { replace(xs) }
function replace(ys: Animal[]) { ys.splice(0, 1, new Cat("Sam")) }
// expect-error: line 19 puts a 'Cat' into it
relay(dogs)

// Kept in a field, and filled by a method later.
class Shelter {
  animals: Animal[] = []
  take(xs: Animal[]) { this.animals = xs }
  add(x: Animal) { this.animals.unshift(x) }
}
const shelter = new Shelter()
// expect-error: line 27 puts an 'Animal' into it, and an array of 'Dog' cannot hold one
shelter.take(dogs)

// Inside another array, in a dictionary, returned, in a closure.
const nested: Animal[][] = []
// expect-error: line 37 puts a 'Cat' into it
nested.push(dogs)
nested[0]?.push(new Cat("Nested"))
const byKind = new Map<string, Animal[]>()
// expect-error: line 41 puts a 'Cat' into it
byKind.set("dogs", dogs)
byKind.get("dogs")?.fill(new Cat("Filled"))
function same(xs: Animal[]) { return xs }
// expect-error: line 44 puts a 'Cat' into it
same(dogs).push(new Cat("Back"))
function later(xs: Animal[]) { return () => { xs.push(new Cat("Later")) } }
// expect-error: line 45 puts a 'Cat' into it
later(dogs)

// Called through a function value, or as a callback.
const handlers: ((xs: Animal[]) => void)[] = [adopt]
// expect-error: puts a 'Cat' into it, and an array of 'Dog' cannot hold one
handlers[0]?.(dogs)
function each(xs: Animal[], f: (xs: Animal[]) => void) { f(xs) }
// expect-error: line 55 puts a 'Cat' into it
each(dogs, xs => { xs.push(new Cat("Callback")) })

// An array of arrays: the inner arrays may not get another Animal either.
const kennels: Dog[][] = [[new Dog("A")]]
function fillAll(all: Animal[][]) { for (const xs of all) xs.push(new Cat("C")) }
// expect-error: 'Dog[][]' cannot go where 'Animal[][]' is expected here: line 59 puts a 'Cat' into an array in it, and an array of 'Dog' cannot hold one
fillAll(kennels)
