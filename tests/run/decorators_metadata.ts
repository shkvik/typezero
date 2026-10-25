// Metadata of decorators: Reflect.defineMetadata and Reflect.getMetadata,
// built in. A key that is an object (the decorator itself, usually) holds
// records of the type of what is written with it; a string key holds
// unknowns. The compiler writes design:type, design:paramtypes and
// design:returntype for every decorated member.

// The whole decorator: the record has the type string.
function Table(name: string) {
  return (cls: object) => Reflect.defineMetadata(Table, name, cls)
}

// The names of the columns, a string[] that each use adds to.
function Column(cls: object, name: string) {
  const names: string[] = Reflect.getOwnMetadata(Column, cls) ?? []
  names.push(name)
  Reflect.defineMetadata(Column, names, cls)
}

function Deprecated(why: string) {
  return (cls: object, name: string) => Reflect.defineMetadata(Deprecated, why, cls, name)
}

// A record of an object type, keyed by a constant.
type Route = { path: string; method: string }
const Routes = { name: "routes" }
function Get(path: string) {
  return (cls: object, method: string) => {
    const routes: Route[] = Reflect.getOwnMetadata(Routes, cls) ?? []
    routes.push({ path, method })
    Reflect.defineMetadata(Routes, routes, cls)
  }
}

@Table("users")
class User {
  @Column name: string = ""
  @Column age: int = 0
  @Column joined = new Date(0)

  @Deprecated("use add")
  plus(a: int, b: int) {
    return a + b
  }

  @Get("/users")
  list(): User[] {
    return []
  }

  @Reflect.metadata("role", "admin")
  static create(name: string) {
    const user = new User()
    user.name = name
    return user
  }
}

console.log(Reflect.getMetadata(Table, User))
console.log(Reflect.getMetadata(Column, User))
console.log(Reflect.getMetadata(Deprecated, User, "plus"), Reflect.getMetadata(Deprecated, User, "list"))
console.log(Reflect.getMetadata(Routes, User))
const table = Reflect.getMetadata(Table, User)
if (table != undefined) console.log(table.toUpperCase())

// A string key gives unknown, which a check tells about.
const role = Reflect.getMetadata("role", User, "create")
if (typeof role == "string") console.log(role.length, role)
console.log(Reflect.getMetadata("role", User), Reflect.getMetadata("missing", User))

// What the compiler records: classes, as TypeScript's emitDecoratorMetadata.
console.log(Reflect.getMetadata("design:type", User, "name"), Reflect.getMetadata("design:type", User, "age"))
console.log(Reflect.getMetadata("design:type", User, "joined"), Reflect.getMetadata("design:type", User, "plus"))
console.log(Reflect.getMetadata("design:paramtypes", User, "plus"))
console.log(Reflect.getMetadata("design:returntype", User, "plus"), Reflect.getMetadata("design:returntype", User, "list"))
console.log(Reflect.getMetadata("design:paramtypes", User, "create"), Reflect.getMetadata("design:returntype", User, "create"))
console.log(Reflect.getMetadata("design:type", User, "age") == Number, Reflect.getMetadata("design:returntype", User, "create") == User)

// has, own, delete; a class that extends another, and an instance, find the
// records up the chain.
console.log(Reflect.hasMetadata(Table, User), Reflect.hasOwnMetadata(Deprecated, User), Reflect.hasMetadata(Deprecated, User, "plus"))
class Admin extends User {
  @Column level: int = 1
}
console.log(Reflect.getMetadata(Table, Admin), Reflect.getOwnMetadata(Table, Admin))
console.log(Reflect.getMetadata(Column, Admin), Reflect.getMetadata(Column, User))
const ann = User.create("Ann")
console.log(Reflect.getMetadata(Table, ann), Reflect.getOwnMetadata(Table, ann))
Reflect.defineMetadata(Table, "one user", ann)
console.log(Reflect.getMetadata(Table, ann), Reflect.getMetadata(Table, new User()))
console.log(Reflect.deleteMetadata(Table, User), Reflect.getMetadata(Table, User), Reflect.deleteMetadata(Table, User))

// The parameters of a constructor are those of the class.
function Injectable(cls: object) {
  console.log("injectable", Reflect.getMetadata("design:paramtypes", cls))
}
@Injectable
class Service {
  constructor(public users: User[], public name: string, public admin: Admin) {}
}
console.log(new Service([], "s", new Admin()).name)
