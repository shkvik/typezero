// same-as-node
// env: TZ_DICT_A=one
// `env` of node:process is process.env itself: the same Record, read,
// written, deleted from and walked under either name.
import { env } from "node:process"
console.log(env.TZ_DICT_A, env["TZ_DICT_A"], process.env.TZ_DICT_A)
env.TZ_DICT_B = "two"
console.log(process.env.TZ_DICT_B, env.TZ_DICT_B, "TZ_DICT_B" in env, "TZ_DICT_B" in process.env)
process.env["TZ_DICT_C"] = "three"
console.log(env.TZ_DICT_C)
delete env.TZ_DICT_B
console.log(process.env.TZ_DICT_B, Object.keys(env).includes("TZ_DICT_A"))
const all: Record<string, string> = env
console.log(all.TZ_DICT_A, Object.entries(process.env).some(([k, v]) => k == "TZ_DICT_C" && v == "three"))
let n = 0
for (const k in env) if (k.startsWith("TZ_DICT_")) n++
console.log(n, process.env.TZ_DICT_C)
