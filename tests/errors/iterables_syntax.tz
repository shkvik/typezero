// Of the symbols of JavaScript, only Symbol.iterator and Symbol.asyncIterator name
// members.
class Odd {
  // expect-error: 'Symbol.species' cannot name a member: of the symbols of JavaScript, only 'Symbol.iterator' and 'Symbol.asyncIterator' can
  [Symbol.species]() { return 1 }
}
