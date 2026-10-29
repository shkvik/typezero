# Writes a tz program of about N lines for measuring the check, the editor and
# the AI tools on big code (docs/lsp.md, step 3):
#
#   python bench/scale/gen.py <dir> <lines>
#
# Modules of ~500 lines: a record type, an error type, a class with methods,
# a constructor function and functions with loops, Result errors and calls into
# the modules they import. Module i imports 2i+1, 2i+2 and 2i+3, so every module
# is reachable from main.tz and the import graph is shallow (depth log2 N), as
# in real code. Everything is deterministic.
import os
import sys

MODULE_LINES = 500


def module(i, count, functions):
    imports = [c for c in (2 * i + 1, 2 * i + 2, 2 * i + 3) if c < count]
    out = []
    for c in imports:
        out.append(f'import {{ Rec{c}, make{c}, Store{c} }} from "./m{c}.tz"')
    out.append("")
    out.append(f"export type Rec{i} = {{ id: int; name: string; tags: string[]; weight: double }}")
    out.append(f'export type Problem{i} = {{ kind: "INVALID"; code: "ERR_FULL" | "ERR_LIMIT"; message: string }}')
    out.append("")
    out.append(f"export class Store{i} {{")
    out.append(f"  private items: Rec{i}[]")
    out.append("  private limit: int")
    out.append("")
    out.append("  public constructor(limit: int) {")
    out.append("    this.items = []")
    out.append("    this.limit = limit")
    out.append("  }")
    out.append("")
    out.append(f"  public add(item: Rec{i}): Result<int, Problem{i}> {{")
    out.append("    if (this.items.length >= this.limit) {")
    out.append('      return err({ kind: "INVALID", code: "ERR_FULL", message: `full: ${item.name}` })')
    out.append("    }")
    out.append("    this.items.push(item)")
    out.append("")
    out.append("    return ok(this.items.length)")
    out.append("  }")
    out.append("")
    out.append("  public total(): double {")
    out.append("    let sum = 0.0")
    out.append("    for (const item of this.items) {")
    out.append("      sum += item.weight")
    out.append("    }")
    out.append("")
    out.append("    return sum")
    out.append("  }")
    out.append("}")
    out.append("")
    out.append(f"export function make{i}(id: int, name: string): Rec{i} {{")
    out.append("  return { id: id, name: name, tags: [name], weight: id.toDouble() * 0.5 }")
    out.append("}")
    out.append("")
    for k in range(functions):
        out.append(f"export function score{i}x{k}(items: Rec{i}[], limit: int): Result<int, Problem{i}> {{")
        out.append("  let total = 0")
        out.append("  for (const item of items) {")
        out.append("    if (item.id > limit) {")
        out.append('      return err({ kind: "INVALID", code: "ERR_LIMIT", message: `too big: ${item.name}` })')
        out.append("    }")
        out.append("    total += item.id + item.tags.length")
        out.append("  }")
        if imports:
            c = imports[k % len(imports)]
            out.append(f'  const other = make{c}({k}, "m{c}")')
            out.append(f"  const store = new Store{c}(4)")
            out.append("  const added = store.add(other)")
            out.append("  if (added.isErr()) {")
            out.append('    return err({ kind: "INVALID", code: "ERR_FULL", message: added.error.message })')
            out.append("  }")
        out.append('  const label = total > 10 ? "many" : "few"')
        out.append("  console.log(label, total)")
        out.append("")
        out.append("  return ok(total)")
        out.append("}")
        out.append("")
    return "\n".join(out)


def main():
    target = sys.argv[1]
    lines = int(sys.argv[2])
    per_function = 17
    functions = (MODULE_LINES - 45) // per_function
    count = max(1, lines // MODULE_LINES)
    os.makedirs(target, exist_ok=True)
    written = 0
    for i in range(count):
        text = module(i, count, functions)
        written += text.count("\n") + 1
        with open(os.path.join(target, f"m{i}.tz"), "w", encoding="utf-8", newline="\n") as f:
            f.write(text + "\n")
    main_text = (
        'import { make0, score0x0 } from "./m0.tz"\n'
        'const checked = score0x0([make0(1, "one")], 10)\n'
        "if (checked.isOk()) console.log(checked.value)\n"
    )
    with open(os.path.join(target, "main.tz"), "w", encoding="utf-8", newline="\n") as f:
        f.write(main_text)
    print(f"{count} modules, {written + 3} lines in {target}")


main()
