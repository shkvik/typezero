// Точки на плоскости: псевдонимы типов, параметры по умолчанию, Math и метки.
type Coord = double

const xs: Coord[] = [0, 3, -2, 4.5, 1, -3]
const ys: Coord[] = [0, 4, 2, 1, 0.5, -1]

// До начала координат, если вторую точку не дали.
function distance(x1: Coord, y1: Coord, x2: Coord = 0, y2: Coord = 0): double {
  return Math.hypot(x2 - x1, y2 - y1)
}

// Угол направления на точку, в радианах или в градусах.
function angle(x: Coord, y: Coord, inDegrees?: boolean): double {
  const radians = Math.atan2(y, x)
  return inDegrees ?? false ? (radians * 180) / Math.PI : radians
}

for (let i = 0; i < xs.length && i < ys.length; i++) {
  const x = xs[i], y = ys[i]
  const r = distance(x, y)
  console.log(
    `(${x}, ${y})`.padEnd(12),
    `r = ${r.toFixed(3)}`,
    `угол = ${angle(x, y, true).toFixed(1)}°`,
    `площадь круга = ${(Math.PI * r ** 2).toFixed(2)}`,
  )
}

// Первая пара точек ближе 2.5: метка выходит сразу из обоих циклов.
let pair = "нет"
search: for (let i = 0; i < xs.length && i < ys.length; i++) {
  for (let j = i + 1; j < xs.length && j < ys.length; j++) {
    if (Math.hypot(xs[j] - xs[i], ys[j] - ys[i]) < 2.5) {
      pair = `${i} и ${j}`
      break search
    }
  }
}
console.log("первая близкая пара:", pair)

// Самая дальняя от первой точки — расстояние считается от неё, а не от нуля.
let farthest = 0
let best: double = 0
for (let i = 1; i < xs.length && i < ys.length; i++) {
  const d = distance(xs[0] ?? 0, ys[0] ?? 0, xs[i], ys[i])
  if (d > best) {
    best = d
    farthest = i
  }
}
console.log(`дальше всех от первой: точка ${farthest}, ${best.toFixed(2)}`)
console.log("по Манхэттену:", Math.abs(xs[farthest] ?? 0) + Math.abs(ys[farthest] ?? 0))
