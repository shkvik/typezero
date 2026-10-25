// same-as-node
// A union whose members refer back to it: the nodes of a tree.
type Expr = { kind: "num"; value: int } | { kind: "add"; left: Expr; right: Expr } | { kind: "neg"; arg: Expr }
function evaluate(e: Expr): int {
  switch (e.kind) {
    case "num": return e.value
    case "add": return evaluate(e.left) + evaluate(e.right)
    case "neg": return -evaluate(e.arg)
  }
}
const tree: Expr = { kind: "add", left: { kind: "num", value: 2 }, right: { kind: "neg", arg: { kind: "num", value: 5 } } }
console.log(evaluate(tree), tree)
interface Leaf { kind: "leaf"; value: string }
interface Branch { kind: "branch"; children: Node[] }
type Node = Leaf | Branch
function count(n: Node): int {
  if (n.kind == "leaf") return 1
  let total = 0
  for (const c of n.children) total += count(c)
  return total
}
const root: Node = { kind: "branch", children: [{ kind: "leaf", value: "a" }, { kind: "branch", children: [{ kind: "leaf", value: "b" }] }] }
console.log(count(root), JSON.stringify(root))
