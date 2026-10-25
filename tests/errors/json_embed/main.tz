// embedJson and embedText read their files when the program is compiled:
// the path is a literal, relative to the file, and the JSON must be valid.

// expect-error: './bad.json' is not valid JSON: expected a value, found 'x' (line 3, column 8)
const bad = embedJson("./bad.json")
// expect-error: cannot read './missing.json':
const missing = embedJson("./missing.json")
const name = "./good.json"
// expect-error: the path of embedJson must be a string literal: the file is read when the program is compiled
const computed = embedJson(name)
// expect-error: the path of embedText is relative to the file it is written in, e.g. 'embedText("./page.html")'
const absolute = embedText("/etc/hosts")
// expect-error: write '/' in the path, not '\', as on every system
const backslash = embedJson(".\\good.json")
// expect-error: expected 1 argument, but got 2
const two = embedText("./good.json", "./bad.json")
const good = embedJson("./good.json")
// expect-error: a Json cannot be a condition
if (good.ok) console.log("ok")
