// A small DI container and a router, as libraries write them on decorators,
// metadata and reflection (docs/decorators.md, sections 6 and 7): nothing of
// it is built into the language.

// The container makes each class once, with what its constructor takes:
// `design:paramtypes`, which the compiler writes for a decorated class, lists
// the classes of its parameters.
const injectable: Constructor[] = []
function Injectable(cls: Constructor) {
  injectable.push(cls)
}

class Container {
  private instances = new Map<Constructor, object>()
  resolve<T extends object>(cls: Constructor<T>): T {
    const found = this.instances.get(cls)
    if (found instanceof cls)
      return found
    if (!injectable.includes(cls))
      throw new Error(`${cls.name} is not injectable`)
    const deps = Reflect.getMetadata("design:paramtypes", cls) ?? []
    const made = Reflect.construct(cls, deps.map(dep => this.resolve(dep)))
    this.instances.set(cls, made)
    return made
  }
}

@Injectable
class Logger {
  lines: string[] = []
  log(text: string) {
    this.lines.push(text)
  }
}

@Injectable
class UserRepo {
  constructor(private logger: Logger) {}
  find(id: int) {
    this.logger.log(`find ${id}`)
    return { id, name: `user${id}` }
  }
}

@Injectable
class UserService {
  constructor(private repo: UserRepo, private logger: Logger) {}
  greet(id: int) {
    this.logger.log(`greet ${id}`)
    return `hello ${this.repo.find(id).name}`
  }
}

class Unknown {}

const container = new Container()
const service = container.resolve(UserService)
console.log(service.greet(7))
console.log(container.resolve(UserService) == service, container.resolve(UserRepo) instanceof UserRepo)
console.log(container.resolve(Logger).lines)
try {
  container.resolve(Unknown)
} catch (e) {
  console.log(String(e))
}

// The router of section 6: a method decorator records its class and name;
// a request makes a controller of a class known only when the program runs,
// takes the method by its name and calls it.
type Route = { path: string; cls: Constructor; method: string }
const routes: Route[] = []
function Get(path: string) {
  return (cls: Constructor, method: string) => {
    routes.push({ path, cls, method })
  }
}

function handle(path: string): unknown {
  const route = routes.find(r => r.path == path)
  if (route == undefined) return "404"
  const controller = new route.cls()
  const method = Reflect.get(controller, route.method)
  if (typeof method != "function") return "500"
  return Reflect.apply(method, controller, [])
}

class UsersController {
  users = ["ann", "bob"]
  @Get("/users")
  list() {
    return this.users.join(", ")
  }
  @Get("/count")
  count() {
    return this.users.length
  }
}

console.log(handle("/users"), handle("/count"), handle("/nope"))
console.log(routes.map(r => `${r.path} -> ${r.cls.name}.${r.method}`))
