// The binary-trees program of the Benchmarks Game: many short-lived trees
// allocated and freed, and one that lives to the end.
interface TreeNode {
  left?: TreeNode
  right?: TreeNode
}

function bottomUpTree(depth: int): TreeNode {
  if (depth == 0) return {}
  return { left: bottomUpTree(depth - 1), right: bottomUpTree(depth - 1) }
}

function itemCheck(node: TreeNode): int {
  if (node.left == undefined || node.right == undefined) return 1
  return 1 + itemCheck(node.left) + itemCheck(node.right)
}

const maxDepth = 18
const stretchDepth = maxDepth + 1
console.log(`stretch tree of depth ${stretchDepth}\t check: ${itemCheck(bottomUpTree(stretchDepth))}`)
const longLivedTree = bottomUpTree(maxDepth)
for (let depth = 4; depth <= maxDepth; depth += 2) {
  const iterations = 1 << (maxDepth - depth + 4)
  let check = 0
  for (let i = 0; i < iterations; i++) check += itemCheck(bottomUpTree(depth))
  console.log(`${iterations}\t trees of depth ${depth}\t check: ${check}`)
}
console.log(`long lived tree of depth ${maxDepth}\t check: ${itemCheck(longLivedTree)}`)
