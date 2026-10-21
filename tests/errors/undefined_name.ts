// Expectations sit on the line above the error, so that the source line the
// compiler echoes back cannot satisfy them by accident.

// expect-error: cannot find name 'y'
let x = y + 1
