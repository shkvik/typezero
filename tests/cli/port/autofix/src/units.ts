export function plural(unit: string, n: number) {
  return n === 1 ? unit : unit + "s"
}

export function short(unit: string) {
  return unit.slice(0, 1)
}
