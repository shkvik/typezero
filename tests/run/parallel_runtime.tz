// same-as-node
// The parallel runtime (docs/async.md, section 8): pieces after an await run
// on the threads of the runtime when they touch only their own data, and
// keep node's order where they touch shared data. The output is node's:
// what is printed, the counter that every task bumps, the strings and the
// objects that go from task to task.
const words = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"]
const lookup = new Map<string, int>()
for (let i = 0; i < words.length; i++) lookup.set(words[i], i)

interface Node {
  name: string
  children: Node[]
  parent?: Node
}

function tree(depth: int, name: string, parent?: Node): Node {
  const node: Node = { name, children: [], parent }
  if (depth > 0)
    for (let i = 0; i < 2; i++) node.children.push(tree(depth - 1, `${name}.${i}`, node))
  return node
}

function size(node: Node): int {
  let n = 1
  for (const c of node.children) n += size(c)
  return n
}

let done = 0

async function work(i: int) {
  await Promise.resolve()
  // Its own data: in parallel.
  let text = ""
  for (let k = 0; k < 2000; k++) text += (words[(i + k) % words.length] ?? "").slice(0, 1)
  const t = tree(8, `t${i}`)
  const n = size(t) + (lookup.get(words[i % words.length] ?? "") ?? 0)
  // Shared: in node's order.
  done++
  console.log("task", i, "made", text.length, n)
  return { i, text: text.slice(0, 12), root: t }
}

const results = await Promise.all([0, 1, 2, 3, 4, 5, 6, 7].map((i) => work(i)))
console.log(done, results.map((r) => `${r.i}:${r.text}:${size(r.root)}`).join(" "))

// A result one task makes, which another goes on with.
async function next(prev: Promise<{ i: int; text: string; root: Node }>) {
  const r = await prev
  await Promise.resolve()
  let total = 0
  for (const c of r.root.children) total += size(c)
  return `${r.text}+${total}`
}
console.log(await Promise.all(results.map((r) => next(Promise.resolve(r)))))
