// Patterns on which backtracking blows up (node takes seconds at 25 letters
// and years at a million) finish at once on a million: past a budget of
// work the compiled matcher remembers where it failed (docs/regex.md,
// section 1). Not same-as-node: node does not finish.
const run = "a".repeat(1000000)
const evil = `${run}!`
console.log(/(a+)+$/.test(evil), /^(a|aa)+$/.test(evil), /(a*)*b/.test(evil), /(x+x+)+y/.test("x".repeat(100000)))
console.log(/(\w+\s?)+$/.test(`${"word ".repeat(200000)}!`), /(a+?)+$/.test(evil))
console.log(/(?=(a+)+$)a/.test(evil), /(?<=(a+)+)b/.test(evil))

// The match itself is found with its groups.
const m = /(a+)+!/.exec(evil)
console.log(m?.index, m?.[1]?.length, /((a)|b)+c/.exec(`${"ab".repeat(100000)}c`)?.[0].length)

// Quadratic, not exponential: each start reads the rest of the text.
console.log(/a.*x/.test(run), /a\w*?x/.test(run), /(?:a|b)*c/.test("ab".repeat(500000)),
  /(?:ab|ba)+c/.test("ab".repeat(500000)))

// Many matches, each after a blowup of its own.
const runs = `${"a".repeat(22)}b`.repeat(20000)
console.log(runs.match(/(a|a)*c|b/g)?.length, runs.replace(/(a|a)*c|b/g, "-").length,
  runs.split(/(?:a|a)*c|b/).length)
let found = 0
for (const m of runs.matchAll(/(a|a)*c|(b)/g)) if (m[2] != undefined) ++found
console.log(found)
