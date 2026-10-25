// Decorators of overloaded members (docs/overloads.md, section 6): the
// records of two variants under one name would be one.
function Inject(token: string) {
  return (_cls: object, _name: string | undefined, _index: int) => {
    console.log(token)
  }
}
function Tag(cls: object, name: string) {
  Reflect.defineMetadata(Tag, name, cls, name)
}

class Twice {
  x: int
  constructor(@Inject("a") a: int) {
    this.x = a
  }
  // expect-error: two constructors of 'Twice' have decorated parameters, and the record 'design:paramtypes' the compiler writes for the class would be one
  constructor(@Inject("b") b: string) {
    this.x = b.length
  }
  @Tag
  size(n: int) {
    return n
  }
  @Tag
  // expect-error: two variants of 'size' of 'Twice' are decorated, and the records of '@Tag' for both would be one
  size(s: string) {
    return s.length
  }
}
console.log(new Twice(1).size(2))
