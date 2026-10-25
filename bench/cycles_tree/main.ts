// Trees whose nodes point at their parents and hold their children in
// arrays: every node is in cycles with its parent. Ten trees of 349 525
// nodes (fan-out 4, depth 9) are built, walked from each leaf up to the root
// and dropped; in tz each tree is one group (in Go, the collector).
class TreeNode {
  children: TreeNode[] = []
  parent?: TreeNode
  constructor(public id: int) {}
}

let nextId = 0
function build(depth: int, parent: TreeNode | undefined): TreeNode {
  const node = new TreeNode(nextId++)
  if (parent) {
    node.parent = parent
    parent.children.push(node)
  }
  if (depth > 0) for (let i = 0; i < 4; i++) build(depth - 1, node)
  return node
}

// The depths of the leaves, each found by walking up.
function walk(node: TreeNode): int {
  if (node.children.length == 0) {
    let depth = 0
    for (let up = node.parent; up; up = up.parent) depth++
    return depth
  }
  let sum = 0
  for (const c of node.children) sum += walk(c)
  return sum
}

let check = 0
for (let round = 0; round < 10; round++) {
  nextId = 0
  const root = build(9, undefined)
  check += walk(root) + nextId
}
console.log(check)
