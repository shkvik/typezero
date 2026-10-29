// The language server: `tz lsp`, which shows the errors of .tz files as they
// are typed (docs/lsp.md). The client is vscode-languageclient, which
// install.ps1 copies here as client.js from third_party/vscode-languageclient.
//
// tz is the one of the setting tz.path, else the one on PATH, else
// build/dev/bin/tz.exe of an open folder (the repository of tz). The server
// runs from a copy of tz and its lib directory, so that the build of tz can
// replace tz.exe while the editor is open; when tz.exe changes, the server
// starts again from a new copy.
"use strict"
const fs = require("fs")
const os = require("os")
const path = require("path")
const vscode = require("vscode")
const { LanguageClient } = require("./client.js")

const exe = process.platform == "win32" ? "tz.exe" : "tz"
let client
let watched
let output

function findTz() {
  const configured = vscode.workspace.getConfiguration("tz").get("path")
  if (configured) return configured
  for (const dir of (process.env.PATH || "").split(path.delimiter)) {
    const file = dir && path.join(dir, exe)
    if (file && fs.existsSync(file)) return file
  }
  for (const folder of vscode.workspace.workspaceFolders || []) {
    const file = path.join(folder.uri.fsPath, "build", "dev", "bin", exe)
    if (fs.existsSync(file)) return file
  }
  return undefined
}

// A copy of tz and its lib directory in the temporary directory, named by the
// time tz.exe was written; older copies go away.
function copyOf(tz) {
  const stamp = String(Math.floor(fs.statSync(tz).mtimeMs))
  const root = path.join(os.tmpdir(), "tz-lsp")
  const dir = path.join(root, stamp)
  const copy = path.join(dir, exe)
  if (!fs.existsSync(copy)) {
    fs.mkdirSync(dir, { recursive: true })
    const lib = path.join(path.dirname(tz), "lib")
    if (fs.existsSync(lib)) fs.cpSync(lib, path.join(dir, "lib"), { recursive: true })
    fs.copyFileSync(tz, copy + ".part")
    fs.renameSync(copy + ".part", copy)
  }
  for (const old of fs.readdirSync(root)) {
    if (old == stamp) continue
    try {
      fs.rmSync(path.join(root, old), { recursive: true, force: true })
    } catch {
      // A copy that another window still runs stays until that window closes.
    }
  }
  return copy
}

async function start() {
  const tz = findTz()
  if (!tz || !fs.existsSync(tz)) {
    output.appendLine(tz ? `tz is not at ${tz}` : "tz is not found: not in tz.path, not on PATH")
    return
  }
  let command
  try {
    command = copyOf(tz)
  } catch (e) {
    output.appendLine(`cannot copy ${tz}: ${e.message}; running it in place`)
    command = tz
  }
  output.appendLine(`starting ${tz} lsp`)
  client = new LanguageClient(
    "tz",
    "TypeZero",
    { command, args: ["lsp"] },
    {
      documentSelector: [{ scheme: "file", language: "tz" }],
      synchronize: { fileEvents: vscode.workspace.createFileSystemWatcher("**/*.{tz,json}") },
      outputChannel: output,
    },
  )
  await client.start()
  watch(tz)
}

async function stop() {
  if (!client) return
  const running = client
  client = undefined
  await running.stop()
}

async function restart() {
  await stop()
  await start()
}

// Starts the server again from a new copy when tz.exe is built again.
function watch(tz) {
  if (watched == tz) return
  if (watched) fs.unwatchFile(watched)
  watched = tz
  let timer
  fs.watchFile(tz, { interval: 2000 }, (now, before) => {
    if (now.mtimeMs == before.mtimeMs || now.size == 0) return
    clearTimeout(timer)
    timer = setTimeout(() => restart(), 3000)
  })
}

function activate(context) {
  output = vscode.window.createOutputChannel("TypeZero")
  context.subscriptions.push(
    output,
    vscode.commands.registerCommand("tz.restartServer", () => restart()),
    vscode.workspace.onDidChangeConfiguration(event => {
      if (event.affectsConfiguration("tz.path")) restart()
    }),
  )
  start().catch(e => output.appendLine(`cannot start tz lsp: ${e.message}`))
}

function deactivate() {
  if (watched) fs.unwatchFile(watched)
  return stop()
}

module.exports = { activate, deactivate }
