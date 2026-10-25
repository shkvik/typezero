// Records of metadata, and the keys they are kept by.
function Table(name: string) {
  return (cls: object) => Reflect.defineMetadata(Table, name, cls)
}
@Table("users")
class User {}

// expect-error: type 'int' is not assignable to type 'string' of the records of 'Table'
Reflect.defineMetadata(Table, 5, User)
// expect-error: the records 'design:*' are what the compiler writes for decorated members; write a key of your own
Reflect.defineMetadata("design:type", User, User)
// expect-error: the target of metadata is an object, such as a class, found 'int'
Reflect.getMetadata(Table, 5)
// expect-error: a key of metadata is a decorator, a class or a constant, whose records have the type of what is written, or a string, whose records are 'unknown'; found 'int'
Reflect.getMetadata(5, User)
function Never() {}
// expect-error: the records of 'Never' have the type of the first one written, 'Reflect.defineMetadata(Never, value, target)', and none is written before this
Reflect.getMetadata(Never, User)
// expect-error: 'Reflect.metadata(key, value)' makes a decorator; write it as one, before a class or a member: '@Reflect.metadata(key, value)'
const decorator = Reflect.metadata(Table, "x")
// expect-error: 'Reflect.getMetadataKeys' is not supported
Reflect.getMetadataKeys(User)
// A string key gives unknown, which needs a check.
const role = Reflect.getMetadata("role", User)
// expect-error: an unknown value has no property 'length' until a check shows what it holds
console.log(role.length)
// expect-error: an expression of type 'void' has no value to record
Reflect.defineMetadata("x", console.log(1), User)
