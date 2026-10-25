// Classes as values with generic classes, those of the program and of the
// library: each list of type arguments makes a class of its own.
import { EventEmitter } from "node:events"

class Box<T> {
  constructor(public value: T) {}
}
// expect-error: type 'typeof Box' is not assignable to type 'Constructor'; each list of type arguments of 'Box' makes a class of its own
const box: Constructor = Box
// expect-error: each list of type arguments of 'EventEmitter' makes a class of its own
const kinds: Constructor[] = [EventEmitter]
const u: unknown = new Box<int>(1)
// expect-error: 'instanceof Box' cannot tell which 'Box' a 'unknown' would be
console.log(u instanceof Box)
const o: object = new EventEmitter()
// expect-error: 'instanceof EventEmitter' cannot tell which 'EventEmitter' a 'object' would be
console.log(o instanceof EventEmitter)
// Fine: a class that extends an instance of it.
class Ints extends Box<int> {}
const ints: Constructor<Box<int>> = Ints
console.log(box, kinds, new ints(1).value)
