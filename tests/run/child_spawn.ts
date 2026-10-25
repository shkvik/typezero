// same-as-node
// spawnSync runs a program and gives what happened: the program here is
// this one again, which does what its arguments after --child say.
import { spawnSync } from "node:child_process"
import { readFileSync } from "node:fs"
import { tmpdir } from "node:os"

const at = process.argv.indexOf("--child")
if (at >= 0) {
  const args = process.argv.slice(at + 1)
  const what = args[0] ?? ""
  if (what == "print") {
    process.stdout.write(args[1] ?? "")
    process.stderr.write(args[2] ?? "")
    process.exitCode = Number(args[3] ?? "0") | 0
  } else if (what == "echo") {
    process.stdout.write(readFileSync(0, "utf8").toUpperCase())
  } else if (what == "env") {
    console.log(process.env.TZ_CHILD_TEST ?? "-", process.env.TZ_OTHER ?? "-")
  } else if (what == "cwd") {
    console.log(process.cwd() == tmpdir())
  } else if (what == "sleep") {
    setTimeout(() => console.log("woke up"), 20000)
  } else if (what == "big") {
    process.stdout.write("x".repeat(100000))
  }
} else {
  const self = [process.argv[1] ?? "", "--child"]

  const bytes = spawnSync(process.execPath, [...self, "print", "out", "err", "3"])
  console.log(bytes.status, bytes.stdout, bytes.stderr, bytes.output.length, bytes.pid > 0)
  console.log(bytes.error == undefined, bytes.signal == undefined, bytes.output[0] == undefined)

  const text = spawnSync(process.execPath, [...self, "print", "привет", "мир"], { encoding: "utf8" })
  console.log(text.status, text.stdout, text.stderr, text.output[1] == text.stdout)
  const hex = spawnSync(process.execPath, [...self, "print", "hi"], { encoding: "hex" })
  console.log(hex.stdout, hex.stderr == "")
  const latin1 = spawnSync(process.execPath, [...self, "print", "é"], { encoding: "latin1" })
  console.log(latin1.stdout.length)
  const buffer = spawnSync(process.execPath, [...self, "print", "b"], { encoding: "buffer" })
  console.log(buffer.stdout)

  // input: text in the encoding of the call, or bytes.
  console.log(spawnSync(process.execPath, [...self, "echo"], { input: "abc", encoding: "utf8" }).stdout)
  console.log(spawnSync(process.execPath, [...self, "echo"], { input: Buffer.from("xyz") }).stdout)
  console.log(spawnSync(process.execPath, [...self, "echo"], { input: "68690a", encoding: "hex" }).stdout)
  console.log(spawnSync(process.execPath, [...self, "echo"], { input: new Uint8Array([111, 107]), encoding: "utf8" }).stdout)
  const large = "line of text\n".repeat(20000)
  console.log(spawnSync(process.execPath, [...self, "echo"], { input: large, encoding: "utf8" }).stdout.length)
  console.log(JSON.stringify(spawnSync(process.execPath, [...self, "echo"], { encoding: "utf8" }).stdout))

  // env and cwd.
  console.log(spawnSync(process.execPath, [...self, "env"], { encoding: "utf8", env: { ...process.env, TZ_CHILD_TEST: "set" } }).stdout.trim())
  const only: Record<string, string> = { TZ_OTHER: "only" }
  const path = process.env.PATH
  if (path != undefined) only.PATH = path
  console.log(spawnSync(process.execPath, [...self, "env"], { encoding: "utf8", env: only }).stdout.trim())
  console.log(spawnSync(process.execPath, [...self, "cwd"], { encoding: "utf8", cwd: tmpdir() }).stdout.trim())

  // stdio: a stream the child does not write to a pipe gives nothing.
  const ignored = spawnSync(process.execPath, [...self, "print", "a", "b"], { stdio: "ignore", encoding: "utf8" })
  console.log(ignored.status, ignored.stdout == undefined, ignored.stderr == undefined,
              ignored.output.every(o => o == undefined))
  const mixed = spawnSync(process.execPath, [...self, "print", "a", "b"], { stdio: ["pipe", "pipe", "ignore"], encoding: "utf8" })
  console.log(mixed.stdout, mixed.stderr == undefined)
  const inherited = spawnSync(process.execPath, [...self, "print", "inherited\n", ""], { stdio: [0, "inherit", 2] })
  console.log("after", inherited.stdout == undefined, inherited.status)

  // A program that is not there.
  const missing = spawnSync("tz-no-such-program", ["a", "b"], { encoding: "utf8" })
  const error = missing.error
  if (error != undefined)
    console.log(error.message, error.code, error.errno, error.syscall, error.path, error instanceof SystemError)
  console.log(missing.status == undefined, missing.signal == undefined, missing.pid)

  // Out of time: killed with killSignal, SIGTERM by default.
  const slow = spawnSync(process.execPath, [...self, "sleep"], { timeout: 300, encoding: "utf8" })
  console.log(slow.error?.code, slow.error?.message.startsWith("spawnSync "), slow.status == undefined,
              slow.signal, slow.stdout == "")
  const killed = spawnSync(process.execPath, [...self, "sleep"], { timeout: 300, killSignal: "SIGKILL" })
  console.log(killed.error?.code, killed.signal)

  // Too much output: ENOBUFS, and the program is killed.
  const big = spawnSync(process.execPath, [...self, "big"], { maxBuffer: 1000, encoding: "utf8" })
  console.log(big.error?.code, big.error?.errno, big.signal, big.stdout.length > 1000)
  console.log(spawnSync(process.execPath, [...self, "big"], { encoding: "utf8" }).stdout.length)
}
