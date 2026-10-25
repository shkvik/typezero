// same-as-node --experimental-transform-types
// Enums: numbers counting up, explicit values, strings.
enum Color {
  Red,
  Green,
  Blue = 10,
  Violet,
}
enum Flags {
  None = 0,
  Read = 1 << 0,
  Write = 1 << 1,
  Both = Read | Write,
  Negative = -Write,
}
const enum Direction {
  Up = "UP",
  Down = "DOWN",
}
console.log(Color.Red, Color.Green, Color.Blue, Color.Violet)
console.log(Flags.Read, Flags.Write, Flags.Both, Flags.Negative, Flags.Both & Flags.Write)
console.log(Direction.Up, Direction.Down, Direction.Up + "!")

// The enum is a type: numbers (or strings) of it.
function paint(c: Color): string {
  switch (c) {
    case Color.Red:
      return "red"
    case Color.Blue:
      return "blue"
    default:
      return `color ${c}`
  }
}
console.log(paint(Color.Red), paint(Color.Green), paint(Color.Blue))
let d: Direction = Direction.Up
if (d == Direction.Up) d = Direction.Down
console.log(d)

// A numeric enum maps values back to names.
console.log(Color[0], Color[11], Color[Color.Blue])
for (let i = 0; i < 3; i++) console.log(i, Color[i] ?? "no such color")
const masks: Flags[] = [Flags.None, Flags.Both, Flags.Negative]
for (const m of masks) console.log(m, Flags[m])

// Members can use earlier members and other enums.
enum Sizes {
  Small = 4,
  Medium = Small * 2,
  Large = Sizes.Medium * 2,
  Huge = Color.Blue * 100,
}
console.log(Sizes.Small, Sizes.Medium, Sizes.Large, Sizes.Huge, typeof Sizes.Huge)
const grid: int[] = new Array<int>(Sizes.Small).fill(0)
console.log(grid.length, grid[Sizes.Small - 1])
