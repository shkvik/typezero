// same-as-node --experimental-transform-types
// Mixins (docs/decorators.md, section 5): a generic function whose class
// expression extends the class it gets. Each copy makes a class of its own,
// for the class it extends, which is known when compiling: its objects have
// the fields of both at fixed places. The class it gives is a class like
// any other: `new` of it, its members in its type, methods that override.

class User {
  constructor(public name: string) {}
  greet() {
    return `hi, ${this.name}`
  }
}
class Post {
  title = "untitled"
}

let clock = 100
function Timestamped<B extends Constructor>(Base: B) {
  return class extends Base {
    stamp = clock++
    age() {
      return clock - this.stamp
    }
  }
}

const TimedUser = Timestamped(User)
const ann = new TimedUser("ann")
console.log(ann, ann.greet(), ann.stamp, ann.age(), ann.name)
const TimedPost = Timestamped(Post)
const post = new TimedPost()
post.title = "news"
console.log(post, post.stamp, TimedUser, TimedPost)

// A constraint gives what the base class has: a mixin of Users overrides
// greet, and calls the one it replaces.
function Tagged<B extends Constructor<User>>(Base: B) {
  return class Tagged extends Base {
    tags: string[] = []
    tag(t: string) {
      this.tags.push(t)
      return this
    }
    greet() {
      return `${super.greet()} [${this.tags.join(", ")}]`
    }
  }
}
const Both = Tagged(TimedUser)
const bob = new Both("bob").tag("admin").tag("new")
console.log(bob.greet(), bob.stamp, Both.name, Both)
// Through the type of the class they extend, the override runs.
const users: User[] = [new User("cid"), bob, ann]
for (const u of users) console.log(u.greet())

// The type of its objects, and a class that extends a mixin.
const timed: InstanceType<typeof TimedUser>[] = [ann, new TimedUser("dan")]
console.log(timed.map(u => u.stamp))
const Admin = class extends Tagged(User) {
  level = 1
}
const root = new Admin("root")
root.tag("all")
console.log(root, root.greet(), root.level)
// A declaration whose base is a mixin is made where it runs.
class Moderator extends Timestamped(User) {
  role = "mod"
}
const mod = new Moderator("mo")
console.log(mod, mod.age(), mod.greet(), Moderator)
