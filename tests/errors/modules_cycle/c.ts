import { g } from "./d"
// expect-error: 'g' uses 'x' before it is declared: JavaScript would throw a ReferenceError when it runs here; move the declaration of 'x' up
export const x = g()
