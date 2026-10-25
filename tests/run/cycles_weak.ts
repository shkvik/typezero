// WeakRef (docs/objects.md, section 11): a parent that its children refer
// to weakly forms no cycle with them, so the parts of a tree go on their
// own, and a weak reference becomes undefined exactly when its object dies
// (in node: some time after, when the garbage collector runs).
class TreeNode {
  parent?: WeakRef<TreeNode>
  constructor(
    public name: string,
    public children: TreeNode[] = [],
  ) {
    for (const c of children) c.parent = new WeakRef(this)
  }
  path(): string {
    const up = this.parent?.deref()
    return up ? `${up.path()}/${this.name}` : this.name
  }
}

const leaf = new TreeNode("b")
{
  const root = new TreeNode("root", [new TreeNode("a", [leaf])])
  console.log(leaf.path(), root.children.length)
}
// Nothing holds root and a any more: they died, and b sees no parent.
console.log(leaf.path(), leaf.parent?.deref() == undefined)

// Many weak references to one object, and the object going.
class Session {
  constructor(public user: string) {}
}
let session: Session | undefined = new Session("ann")
const refs: WeakRef<Session>[] = []
for (let i = 0; i < 3; i++) refs.push(new WeakRef(session))
console.log(refs.map(r => r.deref()?.user ?? "-").join(","))
session = undefined
console.log(refs.map(r => r.deref()?.user ?? "-").join(","))
console.log(refs[0])

// An object in a group: its weak references go when the group does.
class Peer {
  other?: Peer
  constructor(public name: string) {}
}
function pair(): WeakRef<Peer> {
  const a = new Peer("a")
  const b = new Peer("b")
  a.other = b
  b.other = a
  const weak = new WeakRef(b)
  console.log(weak.deref()?.other?.name)
  return weak
}
console.log(pair().deref() == undefined)

// Players that refer to their opponents weakly do not join one group: each
// goes when nothing else holds it.
class Player {
  opponent?: WeakRef<Player>
  constructor(public name: string) {}
}
const lobby: Player[] = []
for (let i = 0; i < 4; i++) lobby.push(new Player(`p${i}`))
for (let i = 0; i < 4; i++) {
  const p = lobby[i], q = lobby[(i + 1) % 4]
  if (p != undefined && q != undefined) p.opponent = new WeakRef(q)
}
const first = lobby[0]
lobby.splice(1, 1)
console.log(first?.opponent?.deref()?.name ?? "gone", lobby.map(p => p.opponent?.deref()?.name ?? "gone").join(" "))
