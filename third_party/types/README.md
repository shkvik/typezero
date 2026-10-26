# Declarations for the editor

TypeScript's declarations that tz puts next to itself, in `types/node_modules`, for the
editor (tsc in VS Code; docs/modules.md, section 6). Programs never contain them.

| Directory | Package | Version | License |
|---|---|---|---|
| `node` | `@types/node` | 24.19.2 | MIT |
| `undici-types` | `undici-types` (what `@types/node` uses for fetch) | 7.24.6 | MIT |

They come unchanged from npm (`npm pack <package>@<version>`).

To update: unpack the new versions here, keep the version of `undici-types` that
`@types/node` asks for, and fix the table.
