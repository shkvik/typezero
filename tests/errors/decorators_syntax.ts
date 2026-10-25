// Decorators where the syntax has none.
function Sealed(cls: object) {
  console.log(cls)
}

class Wrong {
  // expect-error: a decorator cannot decorate a constructor; decorate the class, or a parameter of the constructor
  @Sealed constructor() {}
}

// expect-error: a decorator can decorate only a class, a member of a class or a parameter of a method or a constructor
@Sealed
function notAClass() {}

// expect-error: a decorator can decorate only a class, a member of a class or a parameter of a method or a constructor
const literal = { @Sealed x: 1 }

// expect-error: a decorator can decorate only a class, a member of a class or a parameter of a method or a constructor
const f = @Sealed function () {}

// expect-error: a decorator can decorate only a class, a member of a class or a parameter of a method or a constructor
@Sealed
const value = 1

class Member {
  // expect-error: 'accessor' goes before a field, not a method
  accessor run() {}
}

// expect-error: expected the name of a decorator after '@', found integer literal
@1
class Numbered {}
