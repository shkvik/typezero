// same-as-node
// args: one "two words"
// env: TZ_TEST_GREETING=привет
// expect-exit: 3
// The names of node:process are those of the global process.
import { argv, env, cwd, platform, stdout, exit, execPath } from "node:process"
import { argv as args } from "process"

console.log(argv.slice(2), args.length == argv.length)
console.log(env.TZ_TEST_GREETING, env["TZ_TEST_" + "GREETING"], env.TZ_TEST_NOT_SET)
console.log(cwd() == process.cwd(), platform == process.platform, execPath == process.execPath)
stdout.write("no newline")
stdout.write("\n")
exit(3)
