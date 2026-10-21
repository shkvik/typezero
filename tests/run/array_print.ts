// same-as-node
// console.log shows arrays exactly as Node.js does (util.inspect).
const big: int[] = []
for (let i = 0; i < 150; i++) big.push(i * 37 % 1000)
console.log(big)
const many: string[] = []
for (let i = 0; i < 26; i++) many.push("item" + i)
console.log(many)
console.log(["a\nb", "tab\there", "back\slash", "q'uote", "both'\"", "all'\"`", "\x01\x7f\x9f", "привет", "😀", "a😀b"])
const long = "This is a fairly long string that goes on and on.\nIt has a second line too.\nAnd a third one."
console.log([long], [long, "x"])
const e4: int[][][][] = [[[[]]]]
console.log([[[[1]]]], e4, [[["deep", "er"], ["x"]]], [[[[1], [2]]]])
console.log([-1, 22, -333, 4444, 5, 66, 777, -8])
console.log([1.5, 22.25, 333, 0.125, 5, 66, 777, 8, 9, 10])
const words = ["the", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog", "again", "and", "again"]
console.log(words)
const longWords = ["internationalization", "localization", "accessibility", "configuration", "documentation", "implementation", "optimization"]
console.log(longWords)
const opts: (double | undefined)[] = [1, undefined, 2, 3, 4, 5, 6, 7]
console.log(opts)
const cjk = ["日本", "中文", "한국어", "abc", "de", "f", "gh"]
console.log(cjk)
const mixed = [[1, 2, 3], [4, 5, 6], [7, 8, 9], [10, 11, 12], [13, 14, 15], [16, 17, 18], [19, 20, 21]]
console.log(mixed)
const bools = [true, false, true, false, true, false, true, false, true]
console.log(bools)
const wide: string[] = []
for (let i = 0; i < 12; i++) wide.push("x".repeat(i))
console.log(wide)
const huge = "y".repeat(10005)
console.log([huge].length, [huge.slice(0, 20)])
console.log([""], ["", ""], [" "])
