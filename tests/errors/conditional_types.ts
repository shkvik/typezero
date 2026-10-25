// Conditional types, infer and template literal types: what they take, the
// types they give, and how deep they may go.
type IsString<T> = T extends string ? "yes" : "no"
// expect-error: type '"yes"' is not assignable to type '"no"'
const wrong: IsString<int> = "yes"
// expect-error: 'infer' declares a type only in the 'extends' clause of a conditional type, e.g. 'T extends Array<infer U> ? U : never'
type Outside<T> = infer U
// expect-error: a template literal type holds strings, numbers, booleans and undefined, not '{ x: int }'
type Holed = `a${{ x: int }}`
// expect-error: 'Uppercase' takes a string type, such as a string literal type, not 'int'
type Loud = Uppercase<int>
// expect-error: intersection types are not supported: a value has exactly the fields of its one type
type Both = { a: int } & { b: int }
interface User { id: int; name: string }
// expect-error: a mapped type renames its keys to names of fields, string literal types such as `get${Capitalize<K>}`, not '1'
type Numbered = { [K in keyof User as 1]: User[K] }
type Px = `${number}px`
// expect-error: type '"wide"' is not assignable to type '`${number}px`'
const width: Px = "wide"
function css(w: Px) { return w }
const text: string = "1px"
// expect-error: type 'string' is not assignable to type '`${number}px`'
css(text)

// Recursion: 50 nested instances, or 1000 tail calls, as in TypeScript.
type Tuple<N extends int, Acc extends unknown[] = []> = Acc["length"] extends N ? Acc : Tuple<N, [...Acc, Acc["length"]]>
// expect-error: type alias 'Reverse' nests more than 50 instances of itself, so it may never end; TypeScript stops there too
type Reverse<T> = T extends [infer H, ...infer R] ? [...Reverse<R>, H] : []
const deep: Reverse<Tuple<60>>[0] = 59
// expect-error: type alias 'Count' calls itself more than 1000 times, so it may never end; TypeScript stops there too
type Count<N extends int, Acc extends unknown[] = []> = Acc["length"] extends N ? Acc : Count<N, [...Acc, 1]>
const long: Count<2000>["length"] = 2000
// expect-error: type alias 'Same' refers to itself
type Same<T> = T extends string ? Same<T> : never
const same: Same<"a"> = "a"
