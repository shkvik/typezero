// The errors of .tz files from tz's own compiler, the one written in tz
// (self/): `tzparse types <file> --ownership --quiet`. The C++ compiler of
// `tz lsp` does not know the language since the rules of ownership
// (docs/ownership.md), so its errors are not shown (server.js); the errors and
// the hints of the ownership check come from here.
//
// An open file is checked as a program of its own: the file and what it
// imports. A partial class part imports its class, so it is checked with all
// the parts. Declaration files (.d.tz) and the library of the repository of tz
// are not checked. The unsaved text of open files goes to the compiler through
// --overlay: each such text is written into a temporary file first. A file is
// checked when it is opened, after a pause in typing (kDelay) and when it is
// saved; a save checks the other open files too, since what they import may
// have changed. A newer check of a file stops the older one; at most
// kParallel checks run at once, the others wait.
//
// tzparse is the one of the setting tz.selfPath, else build/self/tzparse.exe
// of an open folder (the repository of tz). It runs in place: next to it lie
// LLVM-C.dll and lib/, the library.
"use strict"
const childProcess = require("child_process")
const crypto = require("crypto")
const fs = require("fs")
const os = require("os")
const path = require("path")
const vscode = require("vscode")

const kDelay = 400
const kParallel = 2
const kTimeout = 120000
const kError = /^(.+?):(\d+):(\d+): error (TZ\d+): (.*)$/
const kHint = /^(.+?):(\d+):(\d+): hint (\S+) (.*)$/
const kHints = {
  "read-param": name => `'${name}' is only read: it can be Read<…>`,
  "readonly-method": name => `'${name}' only reads this: it can be readonly`,
}

let collection
let output
let status
// The open files by keyOf: { document, timer, running }; `running` is the
// process of the check that goes on now.
const checks = new Map()
// The keys of the checks that are due and wait for a free place.
const waiting = []

function findSelf() {
  const configured = vscode.workspace.getConfiguration("tz").get("selfPath")
  if (configured) return configured
  for (const folder of vscode.workspace.workspaceFolders || []) {
    const file = path.join(folder.uri.fsPath, "build", "self", "tzparse.exe")
    if (fs.existsSync(file)) return file
  }
  return undefined
}

function isActive() {
  const self = findSelf()
  return self != undefined && fs.existsSync(self)
}

function isChecked(document) {
  if (document.languageId != "tz" || document.uri.scheme != "file") return false
  return !document.fileName.endsWith(".d.tz") && !isLibrary(document.fileName)
}

// A module of tz's library (library/ of the repository of tz) is not a program:
// it declares what the compiler makes itself.
function isLibrary(file) {
  const key = keyOf(file)
  for (const folder of vscode.workspace.workspaceFolders || []) {
    const root = folder.uri.fsPath
    if (!fs.existsSync(path.join(root, "self", "main.tz"))) continue
    if (key.startsWith(keyOf(path.join(root, "library")) + path.sep)) return true
  }
  return false
}

function keyOf(file) {
  return path.resolve(file).toLowerCase()
}

function schedule(document, delay) {
  if (!isChecked(document)) return
  const key = keyOf(document.fileName)
  const check = checks.get(key) || { timer: undefined, running: undefined }
  check.document = document
  checks.set(key, check)
  clearTimeout(check.timer)
  check.timer = setTimeout(() => due(key), delay)
}

// A check whose time has come. One that runs already checks an older text,
// so it stops.
function due(key) {
  const check = checks.get(key)
  if (!check) return
  kill(check)
  if (!waiting.includes(key)) waiting.push(key)
  pump()
}

function pump() {
  let running = [...checks.values()].filter(check => check.running).length
  while (running < kParallel && waiting.length > 0) {
    const check = checks.get(waiting.shift())
    if (check && run(check)) running += 1
  }
  showStatus()
}

function kill(check) {
  if (!check.running) return
  check.running.kill()
  check.running = undefined
}

function forget(key) {
  const check = checks.get(key)
  if (check) {
    clearTimeout(check.timer)
    kill(check)
  }
  checks.delete(key)
  const at = waiting.indexOf(key)
  if (at >= 0) waiting.splice(at, 1)
}

// The unsaved texts of open .tz files, each in a temporary file named by the
// path it replaces.
function overlays() {
  const dir = path.join(os.tmpdir(), "tz-check")
  const args = []
  for (const document of vscode.workspace.textDocuments) {
    if (!document.isDirty || !isChecked(document)) continue
    fs.mkdirSync(dir, { recursive: true })
    const name = crypto.createHash("sha1").update(keyOf(document.fileName)).digest("hex")
    const file = path.join(dir, `${name}.tz`)
    fs.writeFileSync(file, document.getText())
    args.push("--overlay", document.fileName, file)
  }
  return args
}

// Starts the check; false when it cannot start.
function run(check) {
  const document = check.document
  const self = findSelf()
  if (document.isClosed || !self || !fs.existsSync(self)) return false

  const file = document.fileName
  let args
  try {
    args = ["types", file, "--ownership", "--quiet", ...overlays()]
  } catch (e) {
    output.appendLine(`cannot write the unsaved text: ${e.message}`)
    return false
  }
  const started = Date.now()
  const child = childProcess.spawn(self, args, { cwd: path.dirname(file), windowsHide: true })
  check.running = child

  const out = []
  const err = []
  const timer = setTimeout(() => child.kill(), kTimeout)
  let isDone = false
  const done = () => {
    clearTimeout(timer)
    if (isDone) return false
    isDone = true
    if (check.running != child) return false
    check.running = undefined
    pump()
    return true
  }
  child.stdout.on("data", chunk => out.push(chunk))
  child.stderr.on("data", chunk => err.push(chunk))
  child.on("error", e => {
    if (done()) output.appendLine(`cannot run ${self}: ${e.message}`)
  })
  child.on("close", (code, signal) => {
    if (!done()) return
    if (signal) {
      output.appendLine(`${path.basename(file)}: stopped after ${Date.now() - started} ms`)
      return
    }
    const stdout = Buffer.concat(out).toString("utf8")
    const stderr = Buffer.concat(err).toString("utf8")
    const found = parse(document, stdout, stderr)
    output.appendLine(`${path.basename(file)}: ${found.length} in ${Date.now() - started} ms`)
    if (!document.isClosed) collection.set(document.uri, found)
  })
  return true
}

// The errors and hints of the checked file; the others belong to the checks
// of their own files. A message without a place (a file that cannot be read,
// a broken package.json) is put at the start of the file.
function parse(document, stdout, stderr) {
  const cwd = path.dirname(document.fileName)
  const own = keyOf(document.fileName)
  const found = []
  for (const line of stderr.split(/\r?\n/)) {
    const error = kError.exec(line)
    if (error) {
      if (keyOf(path.resolve(cwd, error[1])) != own) continue
      const range = rangeAt(document, Number(error[2]), Number(error[3]))
      const diagnostic = new vscode.Diagnostic(range, error[5], vscode.DiagnosticSeverity.Error)
      diagnostic.code = error[4]
      diagnostic.source = "tz"
      found.push(diagnostic)
      continue
    }
    if (line.startsWith("tzparse: ")) {
      const range = new vscode.Range(0, 0, 0, 0)
      const message = line.slice("tzparse: ".length)
      found.push(new vscode.Diagnostic(range, message, vscode.DiagnosticSeverity.Error))
    }
  }
  for (const line of stdout.split(/\r?\n/)) {
    const hint = kHint.exec(line)
    if (!hint || keyOf(path.resolve(cwd, hint[1])) != own) continue
    const text = kHints[hint[4]]
    const message = text ? text(hint[5]) : `${hint[4]} ${hint[5]}`
    const range = rangeAt(document, Number(hint[2]), Number(hint[3]))
    const diagnostic = new vscode.Diagnostic(range, message, vscode.DiagnosticSeverity.Hint)
    diagnostic.source = "tz"
    found.push(diagnostic)
  }
  return found
}

// The word at a place the compiler gives: line and column from 1, the column
// in characters (code points), which VS Code counts in UTF-16 units.
function rangeAt(document, line, column) {
  const index = Math.min(Math.max(line - 1, 0), document.lineCount - 1)
  const text = document.lineAt(index).text
  let unit = 0
  for (let point = 1; point < column && unit < text.length; point++) {
    unit += text.codePointAt(unit) > 0xffff ? 2 : 1
  }
  const position = new vscode.Position(index, unit)
  const word = document.getWordRangeAtPosition(position)
  if (word) return word
  const end = Math.min(unit + 1, text.length)
  return new vscode.Range(position, new vscode.Position(index, Math.max(end, unit)))
}

function showStatus() {
  const running = [...checks.values()].filter(check => check.running).length
  if (running == 0 && waiting.length == 0) {
    status.hide()
    return
  }
  status.text = "$(sync~spin) tz"
  status.tooltip = "tz checks the open files"
  status.show()
}

function saved(document) {
  schedule(document, 0)
  for (const other of vscode.workspace.textDocuments) {
    if (other != document) schedule(other, kDelay)
  }
}

function activate(context) {
  output = vscode.window.createOutputChannel("TypeZero Check")
  collection = vscode.languages.createDiagnosticCollection("tz")
  status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left)
  context.subscriptions.push(
    output,
    collection,
    status,
    vscode.workspace.onDidOpenTextDocument(document => schedule(document, 0)),
    vscode.workspace.onDidSaveTextDocument(saved),
    vscode.workspace.onDidChangeTextDocument(event => {
      if (event.contentChanges.length > 0) schedule(event.document, kDelay)
    }),
    vscode.workspace.onDidCloseTextDocument(document => {
      forget(keyOf(document.fileName))
      collection.delete(document.uri)
      showStatus()
    }),
    vscode.workspace.onDidChangeConfiguration(event => {
      if (!event.affectsConfiguration("tz.selfPath")) return
      for (const document of vscode.workspace.textDocuments) schedule(document, 0)
    }),
  )
  if (!isActive()) {
    output.appendLine("tzparse is not found: not in tz.selfPath, not in build/self of an open folder")
  }
  for (const document of vscode.workspace.textDocuments) schedule(document, 0)
}

function deactivate() {
  for (const key of [...checks.keys()]) forget(key)
}

module.exports = { activate, deactivate, isActive }
