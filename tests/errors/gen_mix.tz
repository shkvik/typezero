// Generics with decorators: a decorator that may replace a method has one
// descriptor, and each instance of a generic class has a method of its own.
function Log<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    console.log(name)
    return original.apply(this, args)
  }
}
function Mark(cls: object, name: string) {
  Reflect.defineMetadata("marked", true, cls, name)
}
class Stack<T> {
  items: T[] = []
  // expect-error: a decorator that takes the descriptor of a method of a generic class is not supported yet: each instance of 'Stack' has a method of its own
  @Log
  push(x: T) {
    this.items.push(x)
  }
  // Fine: it takes only the class and the name.
  @Mark
  pop(): T | undefined {
    return this.items.pop()
  }
}
new Stack<int>().push(1)
// A list of parameters has 'length' and 'join' in generic code, and goes on spread.
function wrapper<A extends unknown[]>(...a: A) {
  // expect-error: a list of parameters has no method 'reverse' here
  return a.reverse()
}
