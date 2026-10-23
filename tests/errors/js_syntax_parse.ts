// JavaScript and TypeScript syntax the parser rejects and goes on.
const nums = [1, 2]
// expect-error: 'satisfies' is not supported, just as 'as' is not; declare the type of the variable instead: 'const x: int[] = ...'
const config = [1, 2] satisfies int[]
// expect-error: declare the type of the variable instead: 'const x: int = ...'
const size = nums.length + 1 satisfies int
// expect-error: the left side of '**' cannot be a unary 'void' expression; add parentheses, e.g. '(void x) ** 2'
const v = void 0 ** 2
// expect-error: the left side of '**' cannot be a unary 'delete' expression; add parentheses, e.g. '(delete x) ** 2'
const d = delete nums[0] ** 2
const tag = (texts: string[]) => texts.length
const maybe: ((texts: string[]) => int) | undefined = tag
// expect-error: a tagged template cannot be part of an optional chain; check the tag before the template
const tagged = maybe?.call`x`
// expect-error: a point right after a number belongs to the number, so a name cannot follow it; write '1..toString' or '(1).toString'
const one = 1.toString()
// `in` in a loop header belongs to `for...in`, which is not supported.
// expect-error: 'for...in' loops are not supported; use 'for...of'
for (const k in nums) console.log(k)
