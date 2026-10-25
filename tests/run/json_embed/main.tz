// embedJson and embedText (docs/modules.md, section 5): the files are read
// when the program is compiled, relative to the file of the call, and the
// exe needs them no more. The same file gives the same value every time, as
// require() of a JSON file does in node.
import { page, settings } from "./data/settings.ts"

const config = embedJson("./config.json")
console.log(config)
console.log(config.port, config.users[0].name, config.users[1].roles)
console.log(config == settings(), settings() == settings())
console.log(page.length)
console.log(page)

type Config = { port: int; host: string; debug: boolean; users: { name: string; roles: string[] }[] }
const typed = fromJson<Config>(config)
// expect-warning: 'typed.port + 1' may overflow int: 'typed.port' comes from JSON
console.log(typed.port + 1, typed.users.length, typed.debug)
