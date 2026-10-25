import { x } from "./c"
// expect-error: cannot tell what 'g' returns: it uses a file that imports it back and calls it while loading; declare its return type
export const g = () => x + 1
