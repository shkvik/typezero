// escape and unescape are legacy functions: the errors name what replaces
// them, and what the idioms around them mean.
const s = "a b"
// expect-error: 'escape' is a legacy function whose '%uXXXX' escapes nothing decodes but 'unescape'; encode with 'encodeURIComponent(s)'
const a = escape(s)
// expect-error: 'unescape' is a legacy function that decodes the '%uXXXX' escapes of 'escape'; decode with 'decodeURIComponent(s)'
const b = unescape("%u0041")
// expect-error: 'unescape(encodeURIComponent(s))' makes a string of the UTF-8 bytes of s, a legacy trick; get the bytes themselves with 'new TextEncoder().encode(s)'
const bytes = unescape(encodeURIComponent(s))
// expect-error: 'decodeURIComponent(escape(s))' reads the UTF-8 bytes that s holds as characters, a legacy trick; keep the bytes in a Uint8Array and decode them with 'new TextDecoder().decode(bytes)'
const text = decodeURIComponent(escape(bytes))
// expect-error: 'escape' is not supported: it is a legacy function
const names = ["x y"].map(escape)
// expect-error: 'encodeURIComponent' can be used as a value only where a function type is expected, e.g. 'const f: (s: string) => string = encodeURIComponent'
const f = encodeURIComponent
// expect-error: type 'int' is not assignable to type 'string'
const c = decodeURI(5)
// expect-error: type 'undefined' is not assignable to type 'string'
const d = encodeURIComponent(undefined)
// expect-error: expected 1 argument, but got 2
const e = encodeURI("a", "b")
