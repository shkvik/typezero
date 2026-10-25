// Formatting of .tz files: Prettier reads them as TypeScript, whose syntax tz
// has. Prettier lies next to this file in `prettier`: install.ps1 copies
// third_party/prettier there. The extension (extension.js) and the test
// tests/cli/format.cmake both format through this file.
"use strict"
const prettier = require("./prettier/standalone.js")
const plugins = [require("./prettier/plugins/typescript.js"), require("./prettier/plugins/estree.js")]

// The text formatted as Prettier formats by default: semicolons, double
// quotes, lines up to 80 characters. Indented by `tabSize` spaces, or by tabs
// when `insertSpaces` is false; lines end as they end in the text. With
// `range` ({ start, end }, offsets in the text), only the statements it
// touches change. A Promise of the whole text; it rejects on a syntax error,
// with "(line:column)" at the end of the first line of its message.
function format(text, { tabSize = 2, insertSpaces = true, range } = {}) {
  const options = {
    parser: "typescript",
    plugins,
    tabWidth: tabSize,
    useTabs: !insertSpaces,
    endOfLine: "auto",
  }
  if (range) {
    options.rangeStart = range.start
    options.rangeEnd = range.end
  }
  return prettier.format(text, options)
}

module.exports = { format }
