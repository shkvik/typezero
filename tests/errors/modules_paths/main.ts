// Paths that do not name a .ts file of the program.
// expect-error: 'zod' is the name of a package, but there is no tz.json to list packages in; make one with 'tz init' and add the package with 'tz add zod' (a file of the program is imported by its relative path, e.g. './zod')
import { z } from "zod"
// expect-error: built-in module 'node:http' is not supported yet
import { createServer } from "node:http"
// expect-error: cannot find './missing': there is no missing.ts or missing/index.ts
import { q } from "./missing"
// expect-error: cannot find './lib/': there is no lib/index.ts
import { r } from "./lib/"
// expect-error: a .json file is not a module: only .ts files can be imported; embed the data with 'const data = embedJson("./data.json")'
import { d } from "./data.json"
// expect-error: only .ts files can be imported
import { m } from "./util.mjs"
// expect-error: an import names a file by its path relative to the importing file, e.g. './math' or '../lib/math'
import { a } from "/abs/path"
// expect-error: write '/' in the path of an import
import { b } from ".\\math"
// The path of a file that exists, with or without an extension.
import { add } from "./math.js"
console.log(add(1, 2))
