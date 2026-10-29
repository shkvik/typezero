// The language server (tz lsp), for tests/cli/lsp.cmake: talks to it as
// VS Code does and checks the errors it publishes.
//
//   node lsp.cjs <tz> <dir>
//
// <dir> holds the files of lsp.cmake. Prints what went wrong and exits with 1
// if something did; with TZ_LSP_LOG set, prints the server's log at the end.
const { spawn } = require("child_process")
const fs = require("fs")
const path = require("path")

const [tz, dir] = process.argv.slice(2)

// A file URI written the way VS Code writes it: "file:///d%3A/dir/a.tz".
function uriOf(file) {
  let text = path.resolve(file).split(path.sep).join("/")
  let prefix = "file://"
  const drive = /^([A-Za-z]):/.exec(text)
  if (drive) {
    prefix += "/" + drive[1].toLowerCase() + "%3A"
    text = text.slice(2)
  }
  return prefix + text.split("/").map(encodeURIComponent).join("/")
}

class Server {
  constructor() {
    this.child = spawn(tz, ["lsp"], { stdio: ["pipe", "pipe", "inherit"] })
    this.buffer = Buffer.alloc(0)
    this.waiting = []
    this.published = new Map()
    this.log = []
    this.next = 1
    this.answers = new Map()
    this.child.stdout.on("data", data => this.receive(data))
    this.exited = new Promise(resolve => this.child.on("exit", code => resolve(code)))
  }

  receive(data) {
    this.buffer = Buffer.concat([this.buffer, data])
    for (;;) {
      const end = this.buffer.indexOf("\r\n\r\n")
      if (end < 0) return
      const header = this.buffer.slice(0, end).toString()
      const length = Number(/Content-Length: (\d+)/i.exec(header)[1])
      if (this.buffer.length < end + 4 + length) return
      const body = this.buffer.slice(end + 4, end + 4 + length).toString("utf8")
      this.buffer = this.buffer.slice(end + 4 + length)
      this.handle(JSON.parse(body))
    }
  }

  handle(message) {
    if (message.method == "textDocument/publishDiagnostics") {
      const list = this.published.get(message.params.uri) ?? []
      list.push(message.params.diagnostics)
      this.published.set(message.params.uri, list)
    } else if (message.method == "window/logMessage") {
      this.log.push(message.params.message)
    } else if (message.id !== undefined) {
      this.answers.set(message.id, message)
    }
    for (const wait of this.waiting.slice()) wait()
  }

  send(message) {
    const body = Buffer.from(JSON.stringify({ jsonrpc: "2.0", ...message }), "utf8")
    this.child.stdin.write(`Content-Length: ${body.length}\r\n\r\n`)
    this.child.stdin.write(body)
  }

  request(method, params) {
    const id = this.next++
    this.send({ id, method, params })
    return this.until(`an answer to ${method}`, () => this.answers.get(id))
  }

  notify(method, params) {
    this.send({ method, params })
  }

  until(what, found) {
    return new Promise((resolve, reject) => {
      const check = () => {
        const value = found()
        if (value === undefined) return
        clearTimeout(timer)
        this.waiting.splice(this.waiting.indexOf(check), 1)
        resolve(value)
      }
      const timer = setTimeout(() => {
        this.waiting.splice(this.waiting.indexOf(check), 1)
        reject(new Error(`no ${what} in 30 s\nlog:\n${this.log.join("\n")}`))
      }, 30000)
      this.waiting.push(check)
      check()
    })
  }

  // The next errors published for the file after the call: skips the ones
  // already published, so a step waits for the check its change started.
  errors(file, accept = () => true) {
    const uri = uriOf(file)
    const seen = (this.published.get(uri) ?? []).length
    return this.until(`errors of ${file}`, () => {
      const list = this.published.get(uri) ?? []
      for (let i = seen; i < list.length; i++) if (accept(list[i])) return list[i]
      return undefined
    })
  }

  open(file, text) {
    this.versions = this.versions ?? new Map()
    this.versions.set(file, 1)
    this.notify("textDocument/didOpen", {
      textDocument: { uri: uriOf(file), languageId: "tz", version: 1, text: text ?? fs.readFileSync(file, "utf8") },
    })
  }

  change(file, contentChanges) {
    const version = this.versions.get(file) + 1
    this.versions.set(file, version)
    this.notify("textDocument/didChange", { textDocument: { uri: uriOf(file), version }, contentChanges })
  }

  close(file) {
    this.notify("textDocument/didClose", { textDocument: { uri: uriOf(file) } })
  }
}

// JSON with the keys of objects in order, so that their order does not matter.
function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical)
  if (value === null || typeof value != "object") return value
  return Object.fromEntries(Object.keys(value).sort().map(key => [key, canonical(value[key])]))
}

function expect(what, actual, expected) {
  const a = JSON.stringify(canonical(actual))
  const e = JSON.stringify(canonical(expected))
  if (a != e) throw new Error(`${what}:\n  expected ${e}\n  actual   ${a}`)
}

function range(line, character, endLine, endCharacter) {
  return { start: { line, character }, end: { line: endLine, character: endCharacter } }
}

const file = name => path.join(dir, name)
const some = list => list.length > 0
const none = list => list.length == 0

async function main() {
  const server = new Server()
  const init = await server.request("initialize", { processId: process.pid, rootUri: uriOf(dir), capabilities: {} })
  expect("the sync of documents", init.result.capabilities.textDocumentSync, { openClose: true, change: 2 })
  expect("the name of the server", init.result.serverInfo.name, "tz")
  server.notify("initialized", {})

  // An error appears where it is, and goes away when the text is fixed.
  const single = file("single/main.tz")
  let waited = server.errors(single, some)
  server.open(single)
  let errors = await waited
  expect("errors of single/main.tz", errors.length, 1)
  expect("where the error is", errors[0].range, range(1, 15, 1, 19))
  expect("the error is an error", [errors[0].severity, errors[0].source], [1, "tz"])
  waited = server.errors(single, none)
  server.change(single, [{ range: range(1, 15, 1, 19), text: "41" }])
  await waited

  // Text that is not saved: the file on the disk has no error, the editor's
  // text has one.
  waited = server.errors(single, some)
  server.change(single, [{ range: range(1, 15, 1, 17), text: "true" }])
  errors = await waited
  expect("the error of the text in the editor", errors[0].range, range(1, 15, 1, 19))

  // Columns count UTF-16 units as the editor does: "😀" is two, "п" one.
  const wide = file("wide/main.tz")
  waited = server.errors(wide, some)
  server.open(wide)
  errors = await waited
  expect("where the error after wide letters is", errors[0].range, range(0, 37, 0, 38))
  waited = server.errors(wide, none)
  server.change(wide, [{ range: range(0, 37, 0, 38), text: "1" }])
  await waited

  // A syntax error.
  const broken = file("broken/main.tz")
  waited = server.errors(broken, some)
  server.open(broken)
  errors = await waited
  expect("the syntax error is on line 1", errors[0].range.start.line, 0)

  // A project: the open module is checked as part of tz.json's main, and the
  // errors of main.tz, which is not open, are published too.
  const util = file("project/util.tz")
  const main = file("project/main.tz")
  const mainErrors = server.errors(main, some)
  waited = server.errors(util, some)
  server.open(util)
  errors = await waited
  expect("the error of util.tz", errors[0].range.start.line, 1)
  errors = await mainErrors
  expect("the error of main.tz", errors[0].range.start.line, 2)
  waited = server.errors(util, none)
  server.change(util, [{ range: range(1, 17, 1, 30), text: "1" }])
  await waited

  // A file of the project that main.tz does not import is checked alone.
  const other = file("project/other.tz")
  waited = server.errors(other, some)
  server.open(other)
  errors = await waited
  expect("the error of other.tz", errors[0].range.start.line, 0)

  // Notes go along with their error.
  const notes = file("notes/main.tz")
  waited = server.errors(notes, some)
  server.open(notes)
  errors = await waited
  const related = errors.find(e => e.relatedInformation.length > 0)
  if (!related) throw new Error(`no error of notes/main.tz has a note: ${JSON.stringify(errors)}`)
  expect("the note's file", related.relatedInformation[0].location.uri, uriOf(notes))

  // Closing a file that is checked alone takes its errors away.
  waited = server.errors(broken, none)
  server.close(broken)
  await waited

  // Two levels: a quick check without the analysis of the whole program, then
  // the whole program. The error only the second finds stays while the next
  // quick check runs.
  const overflow = file("overflow/main.tz")
  waited = server.errors(overflow, some)
  server.open(overflow)
  errors = await waited
  if (!errors[0].message.includes("may overflow int8")) throw new Error(`overflow: ${JSON.stringify(errors)}`)
  waited = server.errors(overflow, () => true)
  server.change(overflow, [{ range: range(4, 0, 4, 0), text: "\n" }])
  errors = await waited
  expect("the overflow error after a quick check", errors.length, 1)

  // What the check learned answers hover, definition and references; util.tz
  // is fixed above, so main.tz is checked with it.
  const at = (file, line, character) => ({ textDocument: { uri: uriOf(file) }, position: { line, character } })
  const main2 = file("project/main.tz")
  server.open(main2)
  let answer = await server.request("textDocument/hover", at(main2, 1, 12))
  if (!answer.result || !answer.result.contents.value.includes("function twice(n: int): int"))
    throw new Error(`hover of twice: ${JSON.stringify(answer)}`)
  answer = await server.request("textDocument/hover", at(util, 0, 22))
  if (!answer.result || !answer.result.contents.value.includes("n: int"))
    throw new Error(`hover of the parameter n: ${JSON.stringify(answer)}`)
  answer = await server.request("textDocument/definition", at(main2, 1, 12))
  expect("the definition of twice", answer.result, { uri: uriOf(util), range: range(0, 16, 0, 21) })
  answer = await server.request("textDocument/references", { ...at(util, 0, 16), context: { includeDeclaration: true } })
  const places = answer.result.map(place => `${place.uri == uriOf(util) ? "util" : "main"}:${place.range.start.line}:${place.range.start.character}`)
  expect("the references of twice", places.sort(), ["main:0:9", "main:1:12", "util:0:16"])

  // Completion after a dot lists the members of the type, from the last check
  // (the text with "p." does not parse); rename changes every place.
  const complete = file("complete/main.tz")
  waited = server.errors(complete, none)
  server.open(complete)
  await waited
  server.change(complete, [{ range: range(13, 0, 13, 0), text: "p." }])
  answer = await server.request("textDocument/completion", at(complete, 13, 2))
  const labels = answer.result.items.map(item => item.label).sort()
  expect("the members of Point", labels, ["length", "x", "y"])
  answer = await server.request("textDocument/prepareRename", at(complete, 11, 6))
  expect("what rename changes", answer.result, { range: range(11, 6, 11, 7), placeholder: "p" })
  answer = await server.request("textDocument/rename", { ...at(complete, 12, 12), newName: "point" })
  const edits = answer.result.changes[uriOf(complete)].map(edit => `${edit.range.start.line}:${edit.range.start.character}`)
  expect("the places rename changes", edits.sort(), ["11:6", "12:12"])
  answer = await server.request("textDocument/rename", { ...at(complete, 12, 12), newName: "1x" })
  if (!answer.error) throw new Error(`rename to '1x' did not fail: ${JSON.stringify(answer)}`)

  const shutdown = await server.request("shutdown", null)
  expect("the answer to shutdown", shutdown.result, null)
  server.notify("exit")
  expect("the exit code", await server.exited, 0)
  if (process.env.TZ_LSP_LOG) console.log(server.log.join("\n"))
}

main().catch(error => {
  console.error(error.message)
  process.exit(1)
})
