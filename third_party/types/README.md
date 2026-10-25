# Declarations for the editor

TypeScript's declarations that tz puts next to itself, in `types/node_modules`, for the
editor (tsc in VS Code; docs/modules.md, section 6). Programs never contain them.

| Directory | Package | Version | License |
|---|---|---|---|
| `node` | `@types/node` | 24.19.2 | MIT |
| `undici-types` | `undici-types` (what `@types/node` uses for fetch) | 7.24.6 | MIT |
| `neverthrow` | `neverthrow`, only `dist/index.d.ts` | 8.2.0 | MIT |

They come unchanged from npm (`npm pack <package>@<version>`), except for one change in
`neverthrow/index.d.ts`: in tz only an `Error` can be thrown, so the functions that turn
an exception into an `Err` get an `Error` instead of `unknown`, and without a function for
it the error of the `Result` is that `Error`:

- `ResultAsync.fromPromise`: `errorFn: (e: Error) => E` (was `unknown`);
- `ResultAsync.fromThrowable` and `Result.fromThrowable`: `E = Error`,
  `errorFn?: (e: Error) => E` (was `unknown`).

To update: unpack the new versions here, keep the version of `undici-types` that
`@types/node` asks for, make the change above again, and fix the table.
