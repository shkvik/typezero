// expect-error: type 'string' is not assignable to type 'int'
let n: int = "5"
let s = "abc"
// expect-error: operator '-' cannot be applied to types 'string' and 'int'
let d = s - 1
// expect-error: property 'size' does not exist on type 'string'
let k = s.size
// expect-error: property 'reverse' does not exist on type 'string'
s.reverse()
// expect-error: an index or position must be an integer, found 'double'
let c = s[1.5]
// expect-error: a declaration cannot be the whole body here; wrap it in '{ }'
if (n > 0) let t = 1
