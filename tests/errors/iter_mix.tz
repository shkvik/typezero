// Generator methods, and `yield*` of a Result outside the generator of
// safeTry.
class Box {
  // A field before `*name()` needs a ';', as in JavaScript: `[] * values()`
  // would go on.
  items: int[] = [];
  *values() {
    yield* this.items
    // expect-error: 'yield*' of a Result takes its value only in the generator given to safeTry
    yield* ok(1)
  }
  // expect-error: a generator returns an iterator: declare its type as 'Generator<int[]>'
  *wrong(): int[] {
    yield 1
  }
}
const box = new Box()
// expect-error: an iterator cannot be converted to a string
console.log(`${box.values()}`)
