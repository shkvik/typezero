// For tests/cli/http.cmake: serves a directory over HTTP on 127.0.0.1, the
// way JSR serves its API, and runs a command with TZ_JSR_URL set to the
// server's address; exits with the command's code.
//
//   node serve.mjs <directory> <command> [arguments]
import { spawn } from "node:child_process"
import { readFile } from "node:fs/promises"
import { createServer } from "node:http"
import { join, normalize } from "node:path"

const [root, program, ...args] = process.argv.slice(2)
const server = createServer(async (request, response) => {
  const path = normalize(decodeURIComponent(new URL(request.url, "http://localhost").pathname))
  try {
    const body = await readFile(join(root, path))
    response.writeHead(200).end(body)
  } catch {
    response.writeHead(404).end("not found")
  }
})
server.listen(0, "127.0.0.1", () => {
  const env = { ...process.env, TZ_JSR_URL: `http://127.0.0.1:${server.address().port}/` }
  const child = spawn(program, args, { stdio: "inherit", env })
  child.on("exit", code => {
    server.close()
    process.exit(code ?? 1)
  })
})
