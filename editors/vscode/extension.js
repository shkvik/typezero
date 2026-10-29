// The extension's code: the formatter of .tz files (Format Document,
// Alt+Shift+F, and Format Selection) through format.js, and the errors as you
// type through the language server, server.js. Everything else, the language
// and its highlighting, is in package.json.
"use strict"
const path = require("path")
const vscode = require("vscode")
const { format } = require("./format.js")
const server = require("./server.js")

// The edits that format the document, or the statements that `range` touches.
// A syntax error leaves the text as it is and is shown with its line.
async function edits(document, options, range) {
  const text = document.getText()
  const whole = new vscode.Range(document.positionAt(0), document.positionAt(text.length))
  const settings = { tabSize: options.tabSize, insertSpaces: options.insertSpaces }
  if (range) {
    settings.range = { start: document.offsetAt(range.start), end: document.offsetAt(range.end) }
  }
  try {
    const formatted = await format(text, settings)
    return formatted == text ? [] : [vscode.TextEdit.replace(whole, formatted)]
  } catch (e) {
    const message = String(e && e.message).split("\n")[0]
    vscode.window.showErrorMessage(`Cannot format ${path.basename(document.fileName)}: ${message}`)
    return []
  }
}

function activate(context) {
  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider("tz", {
      provideDocumentFormattingEdits: (document, options) => edits(document, options),
    }),
    vscode.languages.registerDocumentRangeFormattingEditProvider("tz", {
      provideDocumentRangeFormattingEdits: (document, range, options) =>
        edits(document, options, range),
    }),
  )
  server.activate(context)
}

function deactivate() {
  return server.deactivate()
}

module.exports = { activate, deactivate }
