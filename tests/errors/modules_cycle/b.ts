import { aName, getA } from "./a"
// expect-error: b.ts reads 'aName' of a.ts while it loads, but a.ts is not loaded yet: main.ts -> a.ts -> b.ts -> (a.ts already started); compute the value inside a function, or move 'aName' to a file outside the cycle
export const bSeesA = "b sees " + aName
// expect-error: b.ts calls 'getA' while it loads, and it reads 'aName' of a.ts, which is not loaded yet: main.ts -> a.ts -> b.ts -> (a.ts already started); make the call inside a function, or move 'aName' to a file outside the cycle
export const bCallsA = getA()
// Fine: by the time it is called, a.ts has loaded.
export function bLater() {
  return aName
}
