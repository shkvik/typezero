// The formatter of the VS Code extension (editors/vscode/format.js), for
// tests/cli/format.cmake:
//
//   node format.cjs <format.js> print <file> [<tab size> [tabs]]
//     prints the file formatted;
//   node format.cjs <format.js> ends <file>
//     prints how the lines of the file formatted end, "crlf <n>, lf <n>"
//     (the output of a process reaches CMake with "\r\n" turned into "\n");
//   node format.cjs <format.js> check <dir>...
//     formats every .tz file under the directories, except those under a
//     directory `errors`, whose syntax is wrong on purpose; prints the ones
//     that do not parse and exits with 1 if there is one.
const fs = require("fs")
const path = require("path")
const { format } = require(process.argv[2])
const [command, ...args] = process.argv.slice(3)

function files(dir) {
  const found = []
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const file = path.join(dir, entry.name)
    if (entry.isDirectory()) {
      if (entry.name != "errors") found.push(...files(file))
    } else if (entry.name.endsWith(".tz")) {
      found.push(file)
    }
  }
  return found
}

async function main() {
  if (command == "print") {
    const [file, tabSize, tabs] = args
    const options = { tabSize: tabSize ? Number(tabSize) : 2, insertSpaces: tabs != "tabs" }
    process.stdout.write(await format(fs.readFileSync(file, "utf8"), options))
    return
  }
  if (command == "ends") {
    const text = await format(fs.readFileSync(args[0], "utf8"))
    const crlf = text.split("\r\n").length - 1
    console.log(`crlf ${crlf}, lf ${text.split("\n").length - 1 - crlf}`)
    return
  }
  let count = 0
  let failed = 0
  for (const file of args.flatMap(files)) {
    count++
    try {
      await format(fs.readFileSync(file, "utf8"))
    } catch (e) {
      failed++
      console.log(`${file}: ${String(e.message).split("\n")[0]}`)
    }
  }
  console.log(`${count} files, ${failed} do not parse`)
  if (count == 0 || failed > 0) process.exitCode = 1
}

main().catch((e) => {
  console.log(String(e.message).split("\n")[0])
  process.exitCode = 1
})
