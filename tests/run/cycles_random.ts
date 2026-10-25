// same-as-node --experimental-transform-types
// Random links between objects of several cyclic types, through classes,
// interfaces, unions, closures with shared variables, sets and records.
interface Thing {
  id: int
  peer?: Thing
}
class Base implements Thing {
  peer?: Thing
  kids: Base[] = []
  constructor(public id: int) {}
  score(): int {
    return this.id
  }
}
class Leaf extends Base {
  tags = new Set<Base>()
  score(): int {
    return this.id * 2 + this.tags.size
  }
}
class Box {
  item?: Base | Leaf
  rec: Record<string, Thing> = {}
  get?: () => int
}
type Any = Base | Box

let seed = 4242
function rnd(n: int): int {
  seed = (seed * 75 + 74) % 65537
  return seed % n
}

function total(t: Thing | undefined, depth: int): int {
  if (t == undefined || depth == 0) return 0
  return t.id + total(t.peer, depth - 1)
}

function run(steps: int): double {
  const bases: Base[] = []
  const boxes: Box[] = []
  for (let i = 0; i < 20; i++) {
    bases.push(i % 3 == 0 ? new Leaf(i) : new Base(i))
    boxes.push(new Box())
  }
  let next = 20
  let check: double = 0 // a checksum: kept below 1000003, as a double
  let shared: Base | undefined
  for (let step = 0; step < steps; step++) {
    const i = rnd(20), j = rnd(20)
    const x = bases[i], y = bases[j], bx = boxes[i]
    if (x == undefined || y == undefined || bx == undefined) continue
    switch (rnd(12)) {
      case 0:
        x.peer = y
        break
      case 1:
        x.kids.push(y)
        break
      case 2:
        x.kids = x.kids.filter(k => k.id % 2 == 0)
        break
      case 3:
        if (x instanceof Leaf) x.tags.add(y)
        break
      case 4:
        bx.item = y
        break
      case 5:
        bx.rec[`r${rnd(4)}`] = x
        break
      case 6: {
        shared = y
        const holder = x
        bx.get = () => (shared ? shared.score() : -1) + holder.id
        break
      }
      case 7: {
        const fresh = rnd(2) == 0 ? new Leaf(next++) : new Base(next++)
        fresh.peer = x
        x.kids.unshift(fresh)
        if (i >= 0 && i < bases.length) bases[i] = fresh
        break
      }
      case 8:
        x.kids.reverse()
        x.kids.sort((a, b) => a.id - b.id)
        break
      case 9: {
        const all: Any[] = [x, bx, y]
        for (const a of all) if (a instanceof Box) a.item = x
        break
      }
      case 10:
        x.kids = [...x.kids, y].slice(-5)
        break
      case 11:
        const fresh = new Box()
        if (j >= 0 && j < boxes.length) boxes[j] = fresh
        break
    }
    if (step % 500 == 0) {
      let w = 0
      for (const b of bases) w += total(b, 5) + b.kids.length + b.score()
      for (const b of boxes) w += (b.get ? b.get() : 0) + Object.keys(b.rec).length
      check = (check * 31 + w) % 1000003
    }
  }
  return check + next
}
console.log(run(10000), run(3000))
