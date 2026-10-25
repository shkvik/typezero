class Node1 {
  constructor(public name: string) {}
}
// expect-error: a WeakRef refers to an object, not to a value of type 'int'
let a: WeakRef<int> | undefined
// expect-error: 'WeakRef' needs the type of its object
let b: WeakRef | undefined
// expect-error: a WeakRef refers to an object, not to a value of type 'string'
const c = new WeakRef("text")
const w = new WeakRef(new Node1("x"))
// expect-error: property 'target' does not exist on type 'WeakRef<Node1>'; it has 'deref()'
w.target()
// expect-error: property 'deref' of a WeakRef can only be called
const d = w.deref
// expect-error: a WeakRef of 'Date' is not supported yet
const e = new WeakRef(new Date())
// expect-error: 'new WeakRef' takes the object it refers to
const f = new WeakRef()
