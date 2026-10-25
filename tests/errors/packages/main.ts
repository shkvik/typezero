// Imports of packages: by the names in tz.json, and only what they export.
import { isUnit } from "shapes"
// expect-error: 'zod' is not a dependency in
import { z } from "zod"
// expect-error: cannot find 'shapes/missing': package shapes has no missing.ts or missing/index.ts
import { y } from "shapes/missing"
// expect-error: import a package by its name in tz.json: add it with 'tz add jsr:@std/path' and write 'from "@std/path/join"'
import { join } from "jsr:@std/path@^1/join"
// expect-error: npm packages are not supported
import { parse } from "npm:yaml"
console.log(isUnit(1), z, y, join, parse)
