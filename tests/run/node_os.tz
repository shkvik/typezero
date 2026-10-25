// same-as-node
// node:os: what is true of the system wherever the test runs.
import { EOL, devNull, platform, homedir, tmpdir, hostname, cpus, availableParallelism, totalmem,
         freemem, arch, type, release, version, machine, uptime, endianness, loadavg,
         userInfo } from "node:os"
import { isAbsolute } from "node:path"

const windows = platform() == "win32"
console.log(platform() == process.platform, JSON.stringify(EOL) == (windows ? '"\\r\\n"' : '"\\n"'))
console.log(devNull == (windows ? "\\\\.\\nul" : "/dev/null"), endianness())
console.log(isAbsolute(homedir()), isAbsolute(tmpdir()), hostname().length > 0)
console.log(windows ? homedir() == process.env.USERPROFILE : homedir() == process.env.HOME)
const list = cpus()
console.log(list.length > 0, list.length == availableParallelism())
for (const cpu of list.slice(0, 1))
  console.log(cpu.model.length > 0, cpu.speed > 0, cpu.times.idle >= 0, cpu.times.nice,
              Object.keys(cpu), Object.keys(cpu.times))
console.log(totalmem() > freemem(), freemem() > 0, uptime() > 0, Number.isInteger(totalmem()))
console.log(["x64", "arm64", "ia32"].includes(arch()), ["x86_64", "arm64", "i686"].includes(machine()))
console.log(type() == (windows ? "Windows_NT" : platform() == "darwin" ? "Darwin" : "Linux"))
console.log(/^\d+\.\d+/.test(release()), version().length > 0)
console.log(loadavg().length, windows ? loadavg().join() == "0,0,0" : true)
const user = userInfo()
console.log(user.username.length > 0, user.homedir == homedir(),
            windows ? user.uid == -1 && user.gid == -1 : true)
