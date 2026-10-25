// same-as-node --experimental-transform-types
// Graphs: vertices that point at each other in any way, edges as objects
// that point at both ends, and a map of vertices by name.
class Vertex {
  out: Vertex[] = []
  edges: Edge[] = []
  constructor(public id: int) {}
}

class Edge {
  constructor(
    public from: Vertex,
    public to: Vertex,
    public weight: int,
  ) {}
}

class Graph {
  vertices = new Map<string, Vertex>()
  vertex(id: int): Vertex {
    const key = `v${id}`
    const found = this.vertices.get(key)
    if (found) return found
    const v = new Vertex(id)
    this.vertices.set(key, v)
    return v
  }
  connect(a: int, b: int, weight: int) {
    const from = this.vertex(a), to = this.vertex(b)
    from.out.push(to)
    const e = new Edge(from, to, weight)
    from.edges.push(e)
    to.edges.push(e)
  }
}

// Breadth-first: how many vertices are reachable from `start`.
function reachable(start: Vertex): int {
  const seen = new Set<Vertex>([start])
  const queue = [start]
  while (queue.length > 0) {
    const v = queue.shift()
    if (v == undefined) break
    for (const w of v.out)
      if (!seen.has(w)) {
        seen.add(w)
        queue.push(w)
      }
  }
  return seen.size
}

let seed = 7
function next(n: int): int {
  seed = (seed * 75 + 74) % 65537
  return seed % n
}

let total = 0
for (let round = 0; round < 20; round++) {
  const g = new Graph()
  for (let i = 0; i < 300; i++) g.connect(next(100), next(100), next(10))
  g.connect(5, 5, 1) // a loop on one vertex
  for (let s = 0; s < 10; s++) total += reachable(g.vertex(s))
}
console.log(total)

// A vertex kept from a graph that is gone keeps what it reaches.
function keepOne(): Vertex {
  const g = new Graph()
  g.connect(1, 2, 3)
  g.connect(2, 3, 4)
  g.connect(3, 1, 5)
  return g.vertex(2)
}
const v = keepOne()
const firstOut = v.out[0]
console.log(v.id, firstOut?.id, v.edges.length, reachable(v))
