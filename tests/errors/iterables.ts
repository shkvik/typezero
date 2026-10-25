// Collections of the program: `[Symbol.iterator]()` must give an iterator,
// and `Iterable<T>` is the type of parameters only.
class NotIterator {
  [Symbol.iterator]() { return 5 }
}
// expect-error: '[Symbol.iterator]()' of 'NotIterator' gives 'int', which is not an iterator: it needs a method 'next()' that gives '{ done, value }'
for (const x of new NotIterator()) console.log(x)
class Plain { n = 1 }
// expect-error: cannot iterate over a value of type 'Plain'
for (const x of new Plain()) console.log(x)

function sum(xs: Iterable<int>) {
  let s = 0
  for (const x of xs) s += x
  return s
}
// expect-error: 'Set<string>' gives values of type 'string', and the parameter 'Iterable<int>' of 'sum' takes values of type 'int'
sum(new Set(["a"]))
// expect-error: 'Plain' cannot be iterated, and the parameter 'Iterable<int>' of 'sum' takes something that can
sum(new Plain())
// expect-error: 'Iterable<T>' can only be the type of a parameter of a function declared at the top level or of a method
const xs: Iterable<int> = [1, 2]
// expect-error: 'Iterable<T>' can only be the type of a parameter
const f = (ys: Iterable<int>) => 1
function outer() {
  // expect-error: a parameter 'Iterable<int>' makes the function generic, and a generic function can be declared only at the top level for now
  function inner(zs: Iterable<int>) { return 1 }
  return inner([1])
}
class Bag {
  // expect-error: a constructor cannot take an 'Iterable<int>' yet
  constructor(items: Iterable<int>) {}
}
