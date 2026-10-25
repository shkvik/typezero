// Checks the files of a tsconfig.json with TypeScript's checker, as the editor
// does: node tsc.cjs <lib/typescript.js> <tsconfig.json>. Prints every error
// and exits with 1 if there is one (tests/cli/types.cmake).
const path = require("path")
const ts = require(process.argv[2])
const configFile = process.argv[3]

const read = ts.readConfigFile(configFile, ts.sys.readFile)
if (read.error) {
  console.log(ts.flattenDiagnosticMessageText(read.error.messageText, "\n"))
  process.exit(1)
}
const config = ts.parseJsonConfigFileContent(read.config, ts.sys, path.dirname(configFile))
const program = ts.createProgram(config.fileNames, config.options)
const errors = [...config.errors, ...ts.getPreEmitDiagnostics(program)]
for (const e of errors) {
  const text = ts.flattenDiagnosticMessageText(e.messageText, "\n")
  if (e.file) {
    const { line, character } = e.file.getLineAndCharacterOfPosition(e.start)
    console.log(`${e.file.fileName}:${line + 1}:${character + 1}: TS${e.code}: ${text}`)
  } else {
    console.log(`TS${e.code}: ${text}`)
  }
}
if (config.fileNames.length == 0) {
  console.log("no files to check")
  process.exit(1)
}
process.exit(errors.length == 0 ? 0 : 1)
