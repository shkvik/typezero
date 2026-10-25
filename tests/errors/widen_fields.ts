// Writes through a wider type, of fields (docs/objects.md, section 2): an
// interface or a class may narrow a field of one up its chain (`pet: Dog`
// for `pet: Animal`), and its objects go where that one is expected only if
// no code writes another Animal to the field through it.
class Animal { constructor(public name: string) {} }
class Dog extends Animal {}
class Cat extends Animal {}

interface Owner { pet: Animal }
interface DogOwner extends Owner { pet: Dog }
function giveCat(o: Owner) { o.pet = new Cat("Мурка") }
const ivan: DogOwner = { pet: new Dog("Рекс") }
// expect-error: 'DogOwner' cannot go where 'Owner' is expected here: line 11 writes a 'Cat' to its field 'pet', which is a 'Dog' in 'DogOwner'
giveCat(ivan)
// expect-error: line 17 writes a 'Cat' to its field 'pet'
const owner: Owner = ivan
owner.pet = new Cat("Tom")

// A class: through a variable of the class up the chain, through a method
// that class has, and through the elements of an array of it.
class Kennel {
  constructor(public pet: Animal) {}
  empty() { this.pet = new Animal("nobody") }
}
class DogKennel extends Kennel {
  constructor(public pet: Dog) { super(pet) }
}
const kennel = new DogKennel(new Dog("Lassie"))
// expect-error: 'DogKennel' cannot go where 'Kennel' is expected here, as 'this' of 'Kennel.empty': line 23 writes an 'Animal' to its field 'pet', which is a 'Dog' in 'DogKennel'
kennel.empty()
function catsIn(ks: Kennel[]) { ks.forEach(k => { k.pet = new Cat("C") }) }
// expect-error: 'DogKennel' cannot go where 'Kennel' is expected here: line 31 writes a 'Cat' to its field 'pet', which is a 'Dog' in 'DogKennel'
catsIn([kennel])

// A class that implements an interface with a narrower field.
interface Holder { held: Animal }
class DogHolder implements Holder {
  held: Dog = new Dog("Held")
}
function swap(h: Holder) { h.held = new Cat("Swapped") }
// expect-error: 'DogHolder' cannot go where 'Holder' is expected here: line 40 writes a 'Cat' to its field 'held', which is a 'Dog' in 'DogHolder'
swap(new DogHolder())
