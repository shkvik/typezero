export * from "./math"
// expect-error: 'x' is exported both by './math' and by './other' through 'export *'; re-export the one you mean by name, e.g. 'export { x } from "./math"'
export * from "./other"
