// same-as-node --experimental-transform-types
// A tree whose nodes point at their parents: every node is in a cycle with
// its parent, and the whole tree goes when nothing outside holds it.
class TreeNode {
  children: TreeNode[] = []
  parent?: TreeNode
  constructor(public name: string) {}
  add(child: TreeNode) {
    child.parent = this
    this.children.push(child)
    return child
  }
  remove(child: TreeNode) {
    const at = this.children.indexOf(child)
    if (at >= 0) this.children.splice(at, 1)
    child.parent = undefined
  }
  path(): string {
    return this.parent ? `${this.parent.path()}/${this.name}` : this.name
  }
  size(): int {
    let n = 1
    for (const c of this.children) n += c.size()
    return n
  }
}

function build(depth: int, name: string): TreeNode {
  const node = new TreeNode(name)
  if (depth > 0) for (let i = 0; i < 3; i++) node.add(build(depth - 1, `${name}${i}`))
  return node
}

function deepest(node: TreeNode): TreeNode {
  let at = node
  for (let first = at.children[0]; first != undefined; first = at.children[0]) at = first
  return at
}

// Trees made and dropped, many times.
let total = 0
for (let round = 0; round < 50; round++) {
  const root = build(4, "r")
  total += root.size()
}
console.log(total)

// A node kept alone keeps the whole tree, as in JavaScript.
let kept: TreeNode | undefined = deepest(build(3, "t"))
console.log(kept.path())
const up = kept.parent
console.log(up?.name, up?.children.length)
kept = undefined

// A subtree moved to another tree, and one cut off.
const a = build(2, "a")
const b = build(2, "b")
const moved = a.children[1]
if (moved != undefined) {
  a.remove(moved)
  b.add(moved)
  console.log(moved.path(), a.size(), b.size())
}
const cut = b.children[0]
if (cut != undefined) {
  b.remove(cut)
  console.log(cut.path(), cut.size(), b.size())
}

// A node that is its own child, and a parent of itself.
const odd = new TreeNode("odd")
odd.add(odd)
console.log(odd.children.length, odd.parent == odd)
