// Decorators of a library: they come in by name, as anything else.
export function Log<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    const result = original.apply(this, args)
    console.log(`${name}(${args.join(", ")}) → ${String(result)}`)
    return result
  }
}

export function Entity(table: string) {
  return (cls: object) => Reflect.defineMetadata(Entity, table, cls)
}

export function tableOf(cls: object) {
  return Reflect.getMetadata(Entity, cls) ?? "?"
}
