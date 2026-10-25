// same-as-node
// Writes through a wider type (docs/objects.md, section 2; step 5 of the
// plan): a value may go where its type is seen as one that allows more
// writes, as long as the program never makes such a write. Everything here
// only reads through the wider type, or writes what the value's own type
// allows; tests/errors/widen_*.ts are the writes that are not allowed.
class Animal {
  name: string
  constructor(name: string) { this.name = name }
}
class Dog extends Animal {
  bark() { return `${this.name}: woof` }
}
class Cat extends Animal {}

// An array of Dog as an array of Animal.
function names(pets: Animal[]) { return pets.map(p => p.name).join(", ") }
function longest(pets: Animal[]) {
  let best = pets[0]
  for (const p of pets) if (best == undefined || p.name.length > best.name.length) best = p
  return best?.name
}
function withCat(pets: Animal[]) {
  const all = [...pets]           // a new array: a Cat may go into it
  all.push(new Cat("Tom"))
  return all
}
function addDog(pets: Animal[], dog: Dog) { pets.push(dog) }
function sortByName(pets: Animal[]) { pets.sort((a, b) => a.name < b.name ? -1 : 1) }
function shout(pets: Animal[]) { for (const p of pets) p.name = p.name.toUpperCase() }
const dogs: Dog[] = [new Dog("Rex"), new Dog("Ace")]
const animals: Animal[] = dogs
console.log(names(dogs), longest(dogs), names(withCat(dogs)), animals.length)
addDog(dogs, new Dog("Bo"))
sortByName(dogs)
shout(dogs)
console.log(dogs.map(d => d.bark()))
const zoo: Animal[] = []
zoo.push(new Cat("Felix"))       // another array: nothing reaches Dog[]
console.log(names(zoo), names(dogs.filter(d => d.name != "BO")))

// A field narrowed by an interface that extends another.
interface Owner { pet: Animal }
interface DogOwner extends Owner { pet: Dog }
function petName(o: Owner) { return o.pet.name }
function giveDog(o: Owner, dog: Dog) { o.pet = dog }
function rename(o: Owner) { o.pet.name = "Rexy" }
const ivan: DogOwner = { pet: new Dog("Rex") }
console.log(petName(ivan))
giveDog(ivan, new Dog("Max"))
rename(ivan)
console.log(ivan.pet.bark())

// And by a class that extends another: the constructor of the class up the
// chain sets the field, and the class's own initializer after it.
class Kennel {
  pet: Animal
  open = true
  constructor(pet: Animal) { this.pet = pet }
  describe() { return `${this.pet.name} (${this.open ? "open" : "closed"})` }
}
class DogKennel extends Kennel {
  pet: Dog = new Dog("Lassie")
  readonly open = false
  walk() { return this.pet.bark() }
}
const kennel = new DogKennel(new Dog("Rex"))
const kennels: Kennel[] = [kennel, new Kennel(new Cat("Tom"))]
console.log(kennel.describe(), kennel.walk(), kennels.map(k => k.describe()))

// A read-only field, where the type would let code change it.
interface Settings { readonly host: string; readonly port: int }
function address(s: { host: string; port: int }) { return `${s.host}:${s.port}` }
function addresses(all: { host: string; port: int }[]) { return all.map(address) }
const settings: Settings = { host: "localhost", port: 8080 }
const backups: Settings[] = [settings, { host: "backup", port: 8081 }]
console.log(address(settings), addresses(backups))
const pair: readonly [int, int] = [3, 4]
function total(t: [int, int]) { return t[0] + t[1] }
console.log(total(pair))
