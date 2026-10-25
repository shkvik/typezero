let sum = 0

export async function load(name: string) {
  await undefined
  sum += name.length
  return name.toUpperCase()
}

export function total() {
  return sum
}
