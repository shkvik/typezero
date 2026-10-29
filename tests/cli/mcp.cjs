// The tools for AI agents (tz mcp), for tests/cli/mcp.cmake: talks to the
// server as an agent does (MCP over stdin and stdout, a JSON message a line)
// and checks what each tool answers.
//
//   node mcp.cjs <tz> <dir>
//
// <dir> is a project of mcp.cmake and the server's working directory. Prints
// what went wrong and exits with 1 if something did.
const { spawn } = require("child_process")

const [tz, dir] = process.argv.slice(2)

const child = spawn(tz, ["mcp"], { cwd: dir, stdio: ["pipe", "pipe", "inherit"] })
let buffer = ""
const answers = new Map()
const waiting = []
let next = 1

child.stdout.on("data", data => {
  buffer += data.toString("utf8")
  for (let end = buffer.indexOf("\n"); end >= 0; end = buffer.indexOf("\n")) {
    const line = buffer.slice(0, end)
    buffer = buffer.slice(end + 1)
    if (line.trim() == "") continue
    const message = JSON.parse(line)
    answers.set(message.id, message)
    for (const wait of waiting.slice()) wait()
  }
})

function request(method, params) {
  const id = next++
  child.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n")
  return new Promise((resolve, reject) => {
    const check = () => {
      if (!answers.has(id)) return
      clearTimeout(timer)
      waiting.splice(waiting.indexOf(check), 1)
      resolve(answers.get(id))
    }
    const timer = setTimeout(() => reject(new Error(`no answer to ${method} in 60 s`)), 60000)
    waiting.push(check)
  })
}

async function call(name, args) {
  const answer = await request("tools/call", { name, arguments: args })
  const text = answer.result.content.map(part => part.text).join("")
  return { text, failed: answer.result.isError }
}

function expectText(what, text, ...parts) {
  for (const part of parts) {
    if (!text.includes(part)) throw new Error(`${what} has no '${part}':\n${text}`)
  }
}

async function main() {
  const init = await request("initialize", {
    protocolVersion: "2025-06-18",
    capabilities: {},
    clientInfo: { name: "test", version: "1" },
  })
  if (init.result.serverInfo.name != "tz") throw new Error(`initialize: ${JSON.stringify(init)}`)
  child.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n")

  const list = await request("tools/list", {})
  const names = list.result.tools.map(tool => tool.name).sort().join(" ")
  if (names != "check_edit context impact language_card map query") throw new Error(`tools: ${names}`)

  let answer = await call("map", {})
  expectText("map", answer.text, "program main.tz: 2 modules", "export function centroid(shape: Shape): Vec")

  answer = await call("context", { target: "Vec.add" })
  expectText("context", answer.text, "## method Vec.add - vec.tz:10:10", "used by (1):", "in function centroid")

  answer = await call("query", { kind: "callers", target: "centroid" })
  expectText("query callers", answer.text, "function centroid - called from 1 place", "const c - main.tz:4:11")

  answer = await call("query", { kind: "type", target: "vec.tz:22:7" })
  expectText("query type", answer.text, "sum: Vec")

  answer = await call("impact", { target: "Vec.add" })
  expectText("impact", answer.text, "level 1 - use it directly (1):", "function centroid")

  answer = await call("check_edit", { file: "vec.tz", old: "return sum.scale(1 / n.toDouble())", new: "return sum.scale(n)" })
  if (answer.failed) throw new Error(`check_edit failed: ${answer.text}`)
  expectText("check_edit with an error", answer.text, "1 problem with this edit", "vec.tz:31:20: error:")

  answer = await call("check_edit", { file: "vec.tz", old: "let sum = new Vec(0, 0)", new: "let sum = new Vec(1, 1)" })
  expectText("check_edit that checks", answer.text, "no problems")

  answer = await call("check_edit", { file: "vec.tz", old: "this", new: "that" })
  if (!answer.failed) throw new Error(`check_edit of an ambiguous 'old' did not fail: ${answer.text}`)

  answer = await call("context", { target: "nothingLikeThis" })
  if (!answer.failed) throw new Error(`context of an unknown name did not fail: ${answer.text}`)
  expectText("context of an unknown name", answer.text, "nothing in the program is named 'nothingLikeThis'")

  // The answers come from the records of the last check while the files stay
  // the same; a change on the disk makes a new check.
  require("fs").appendFileSync(require("path").join(dir, "vec.tz"), "export function extra(): int {\n  return 1\n}\n")
  answer = await call("query", { kind: "definition", target: "extra" })
  expectText("a function added on the disk", answer.text, "vec.tz:33:17  function extra(): int")

  answer = await call("language_card", {})
  expectText("language card", answer.text, "# tz для ИИ")

  child.stdin.end()
}

main().then(
  () => process.exit(0),
  error => {
    console.error(error.message)
    child.kill()
    process.exit(1)
  },
)
