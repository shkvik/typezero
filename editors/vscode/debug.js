// Debugging of .tz files (docs/debug.md): F5 builds the program with
// `tz build --debug` into the temporary directory and starts it under the
// debugger of the C/C++ extension ("cppvsdbg"), which reads the .pdb that tz
// writes next to the executable: breakpoints and steps in .tz files, the call
// stack, and the variables with their values.
"use strict"
const childProcess = require("child_process")
const fs = require("fs")
const os = require("os")
const path = require("path")
const vscode = require("vscode")

let output

// ${file}, ${fileDirname} and ${workspaceFolder} in a setting of launch.json.
function substitute(text, folder) {
  if (typeof text != "string") return text
  const editor = vscode.window.activeTextEditor
  const file = editor ? editor.document.fileName : ""
  const workspace = folder ? folder.uri.fsPath : ""
  return text
    .replace(/\$\{file\}/g, file)
    .replace(/\$\{fileDirname\}/g, file ? path.dirname(file) : "")
    .replace(/\$\{workspaceFolder\}/g, workspace)
}

function build(tz, program, exe) {
  return new Promise((resolve) => {
    childProcess.execFile(
      tz,
      ["build", "--debug", "-o", exe, program],
      { cwd: path.dirname(program), windowsHide: true },
      (error, stdout, stderr) => resolve({ failed: !!error, text: stdout + stderr }),
    )
  })
}

function provider(findTz) {
  return {
    async resolveDebugConfiguration(folder, config) {
      if (!config.type && !config.request && !config.name) {
        const editor = vscode.window.activeTextEditor
        if (!editor || editor.document.languageId != "tz") {
          vscode.window.showErrorMessage("Open a .tz file to debug it")
          return undefined
        }
        config = { type: "tz", request: "launch", name: "Debug the .tz file", program: "${file}" }
      }
      if (!vscode.extensions.getExtension("ms-vscode.cpptools")) {
        vscode.window.showErrorMessage(
          "Debugging of .tz files needs the C/C++ extension (ms-vscode.cpptools)",
        )
        return undefined
      }
      const tz = findTz()
      if (!tz) {
        vscode.window.showErrorMessage("tz is not found: not in tz.path, not on PATH")
        return undefined
      }
      const program = path.resolve(substitute(config.program || "${file}", folder))
      const dir = path.join(os.tmpdir(), "tz-debug")
      fs.mkdirSync(dir, { recursive: true })
      const exe = path.join(dir, path.basename(program, path.extname(program)) + ".exe")
      const title = `tz build --debug ${path.basename(program)}`
      const built = await vscode.window.withProgress(
        { location: vscode.ProgressLocation.Window, title },
        () => build(tz, program, exe),
      )
      if (built.failed) {
        output.clear()
        output.append(built.text)
        output.show(true)
        vscode.window.showErrorMessage(`tz could not build ${path.basename(program)}: see Output`)
        return undefined
      }
      const env = config.env || {}
      return {
        type: "cppvsdbg",
        request: "launch",
        name: config.name || "Debug the .tz file",
        program: exe,
        args: (config.args || []).map((arg) => substitute(arg, folder)),
        cwd: substitute(config.cwd, folder) || path.dirname(program),
        environment: Object.keys(env).map((name) => ({ name, value: String(env[name]) })),
        console: "integratedTerminal",
        stopAtEntry: !!config.stopAtEntry,
        visualizerFile: path.join(path.dirname(tz), "tz.natvis"),
      }
    },
  }
}

function activate(context, findTz) {
  output = vscode.window.createOutputChannel("TypeZero Debug")
  context.subscriptions.push(
    output,
    vscode.debug.registerDebugConfigurationProvider("tz", provider(findTz)),
  )
}

module.exports = { activate }
