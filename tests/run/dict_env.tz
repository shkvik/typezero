// same-as-node
// env: TZ_DICT_ONE=1
// env: TZ_DICT_TWO=two
// process.env as a whole: a dictionary of the variables.
const env = process.env
const mine = Object.keys(env).filter(k => k.startsWith("TZ_DICT_")).sort()
console.log(mine, mine.map(k => env[k]))
console.log("TZ_DICT_ONE" in env, "TZ_DICT_NONE" in process.env, typeof env)
process.env.TZ_DICT_THREE = "3"
env["TZ_DICT_FOUR"] = "4"
console.log(process.env.TZ_DICT_THREE, env.TZ_DICT_FOUR, process.env["TZ_DICT_FOUR"])
delete process.env.TZ_DICT_ONE
console.log(process.env.TZ_DICT_ONE, env.TZ_DICT_ONE, "TZ_DICT_ONE" in env)
let count = 0
for (const k in process.env) if (k.startsWith("TZ_DICT_")) count++
console.log(count)
const copy = { ...process.env }
copy.TZ_DICT_TWO = "changed"
console.log(process.env.TZ_DICT_TWO, copy.TZ_DICT_TWO)
process.env.TZ_DICT_TWO ??= "unused"
process.env.TZ_DICT_FIVE ??= "5"
console.log(process.env.TZ_DICT_TWO, process.env.TZ_DICT_FIVE)
