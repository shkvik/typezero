// Class syntax that tz does not have: getters and setters (reading or
// writing a field never runs code), and the forms that only tsc needs.
class Shape {
  // expect-error: getters are not supported: reading a field never runs code; write a method instead, e.g. 'area()'
  get area() {
    return 1
  }
  // expect-error: setters are not supported: writing a field never runs code; write a method instead, e.g. 'setWidth(value)'
  set width(v: int) {}
  // expect-error: static blocks are not supported; give each static field its value in its declaration
  static {
    console.log("init")
  }
  // expect-error: a member name in brackets is not supported: an object has a fixed set of fields; keep a dictionary in a field
  [key: string]: int
  // expect-error: 'declare' is not supported: a field gets its value in its declaration or in the constructor
  declare size: int
  // expect-error: expected '(' after the name of the generator method
  *items: int
  // expect-error: a constructor cannot be a generator: 'new' gives the object
  *constructor() {}
  // expect-error: a method cannot be 'readonly': a method cannot be replaced anyway
  readonly paint() {}
  // expect-error: method 'add' has no body; overload signatures over one body are not supported: give each variant a body of its own
  add(v: int): int
  // expect-error: a constructor cannot have a return type
  constructor(): Shape {}
  // expect-error: 'public' before a parameter makes it a field, which only a constructor's parameter can be
  move(public x: int) {}
  // expect-error: abstract method 'fill' cannot have a body: the classes that extend 'Shape' give it one; to give it one here, drop 'abstract'
  abstract fill() {}
  // expect-error: abstract fields are not supported: declare the field in the class that sets it, or give it a value here
  abstract color: string
  // expect-error: a static method cannot be 'abstract': it belongs to the class itself
  static abstract make(): Shape
}
class Other {
  // expect-error: a constructor cannot be 'override'
  override constructor() {}
}
