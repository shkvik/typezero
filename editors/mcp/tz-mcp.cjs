// Starts `tz mcp`, the tools of the compiler for AI agents (docs/lsp.md), for
// .mcp.json of this repository: node editors/mcp/tz-mcp.cjs.
//
// tz is the one of the environment variable TZ_EXE, else build/dev/bin/tz.exe
// of this repository, else tz on PATH. It runs from a copy in the temporary
// directory (with lib and ai.md), so that the build of tz can replace tz.exe
// while the agent runs; the agent's session picks up a new build when it
// starts the server again.
"use strict"
const { spawn } = require("child_process")
const fs = require("fs")
const os = require("os")
const path = require("path")

const exe = process.platform == "win32" ? "tz.exe" : "tz"

function findTz() {
  if (process.env.TZ_EXE) return process.env.TZ_EXE
  const built = path.join(__dirname, "..", "..", "build", "dev", "bin", exe)
  if (fs.existsSync(built)) return built
  for (const dir of (process.env.PATH || "").split(path.delimiter)) {
    const file = dir && path.join(dir, exe)
    if (file && fs.existsSync(file)) return file
  }
  return undefined
}

function copyOf(tz) {
  const stamp = String(Math.floor(fs.statSync(tz).mtimeMs))
  const root = path.join(os.tmpdir(), "tz-mcp")
  const dir = path.join(root, stamp)
  const copy = path.join(dir, exe)
  if (!fs.existsSync(copy)) {
    fs.mkdirSync(dir, { recursive: true })
    const from = path.dirname(tz)
    const lib = path.join(from, "lib")
    if (fs.existsSync(lib)) fs.cpSync(lib, path.join(dir, "lib"), { recursive: true })
    const card = path.join(from, "ai.md")
    if (fs.existsSync(card)) fs.copyFileSync(card, path.join(dir, "ai.md"))
    fs.copyFileSync(tz, copy + ".part")
    fs.renameSync(copy + ".part", copy)
  }
  for (const old of fs.readdirSync(root)) {
    if (old == stamp) continue
    try {
      fs.rmSync(path.join(root, old), { recursive: true, force: true })
    } catch {
      // A copy that another session still runs stays until that session ends.
    }
  }
  return copy
}

const tz = findTz()
if (!tz) {
  console.error("tz is not found: set TZ_EXE, build tz (build/dev/bin) or put tz on PATH")
  process.exit(1)
}
let command = tz
try {
  command = copyOf(tz)
} catch (e) {
  console.error(`cannot copy ${tz}: ${e.message}; running it in place`)
}
const child = spawn(command, ["mcp"], { stdio: "inherit" })
child.on("exit", code => process.exit(code ?? 1))
child.on("error", e => {
  console.error(`cannot start ${command}: ${e.message}`)
  process.exit(1)
})
