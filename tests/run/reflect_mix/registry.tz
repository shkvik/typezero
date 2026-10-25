// What main.ts builds on: a registry of classes that a class decorator fills,
// a method decorator for async methods and async generators, a mixin over an
// emitter class, and a generic store whose items fromJson makes.
import { EventEmitter } from "node:events"

export const registered: Constructor[] = []
export const calls: string[] = []

export function Entity(cls: Constructor) {
  registered.push(cls)
}

export function Logged<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    calls.push(`${name}(${args.join(", ")})`)
    return original.apply(this, args)
  }
}

// An emitter, whose events are those its code and the program emit.
export class Bus extends EventEmitter {
  send(n: int) {
    this.emit("data", n)
  }
}

// A mixin of buses: the class it gives counts the notes it emits.
export function Counting<B extends Constructor<Bus>>(Base: B) {
  return class extends Base {
    emitted = 0
    note(what: string) {
      this.emitted++
      this.emit("note", what, this.emitted)
    }
  }
}

// Items of a class known only when the program runs, made from Json.
export class Store<T extends object> {
  items: T[] = []
  constructor(readonly kind: Constructor<T>) {}

  load(data: Json[]) {
    for (const j of data) this.items.push(fromJson(this.kind, j))
    return this
  }

  async *each() {
    for (const item of this.items) {
      await undefined
      yield item
    }
  }
}

// Decorated async methods and an async generator.
export class Names {
  constructor(readonly names: string[]) {}

  @Logged
  async first(): Promise<string | undefined> {
    await undefined
    return this.names[0]
  }

  @Logged
  async *from(start: int) {
    for (let i = start; i < this.names.length; i++) {
      await undefined
      yield this.names[i] ?? ""
    }
  }
}
