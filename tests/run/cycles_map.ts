// same-as-node --experimental-transform-types
// Dictionaries in cycles: items that hold the map they are in, a set of
// objects that hold the set, a Record of arrays of objects that point back.
class Item {
  constructor(
    public name: string,
    public owner: Map<string, Item>,
  ) {}
  siblings(): int {
    return this.owner.size - 1
  }
}

function fill(n: int): Map<string, Item> {
  const items = new Map<string, Item>()
  for (let i = 0; i < n; i++) {
    const item = new Item(`i${i}`, items)
    items.set(item.name, item)
  }
  return items
}

let count = 0
for (let round = 0; round < 100; round++) {
  const items = fill(20)
  items.delete("i3")
  count += items.size
}
console.log(count)

// An item taken out lives on with the map it points at.
const one = fill(5).get("i2")
console.log(one?.name, one?.siblings(), one?.owner.has("i4"))

// A set whose members know it, keys that are objects.
class Member {
  groups = new Set<Club>()
}
class Club {
  members = new Set<Member>()
  join(m: Member) {
    this.members.add(m)
    m.groups.add(this)
  }
}
const clubs: Club[] = []
for (let i = 0; i < 4; i++) clubs.push(new Club())
const people: Member[] = []
for (let i = 0; i < 10; i++) {
  const m = new Member()
  people.push(m)
  for (const c of clubs) if ((i + clubs.indexOf(c)) % 3 == 0) c.join(m)
}
console.log(clubs.map(c => c.members.size).join(","), people.map(p => p.groups.size).join(""))
clubs.length = 0
people.length = 0

// A Record of arrays whose objects point at the record.
interface Entry {
  index: Record<string, Entry[]>
  word: string
}
const index: Record<string, Entry[]> = {}
for (const word of ["apple", "avocado", "banana", "blueberry", "cherry"]) {
  const key = word[0] ?? ""
  const list = index[key] ?? []
  list.push({ index, word })
  index[key] = list
}
console.log(Object.keys(index).join(""), index["b"]?.length, index["a"]?.[1]?.index == index)
