// same-as-node
// node:path in both styles on many paths, compared with Node: the default
// style is the system's (win32 on Windows), and posix and win32 give the
// other.
import { posix, win32, sep, delimiter, join, basename } from "node:path"

const paths = [
  "", ".", "..", "/", "//", "///", "\\", "\\\\", "a", "a/", "a//", "/a", "a/b", "a/b/", "a/./b",
  "a/../b", "a/b/../..", "a/b/../../..", "../a", "./a", "/a/b/c.txt", "/a/b/.c", "/a/b/c.",
  "/a/b/c..", "/a/b/..c", "a.b.c", ".a", "..a", "a..", "...", "/..", "/../a", "a\\b",
  "a\\b\\..\\c", "C:", "C:\\", "C:/", "c:a", "C:a\\b", "C:\\a\\b", "C:/a/b/", "C:\\a\\..\\..",
  "\\\\server", "\\\\server\\share", "\\\\server\\share\\", "\\\\server\\share\\a\\b.txt",
  "//server/share/a", "\\\\?\\C:\\a", "\\\\.\\pipe\\x", "\\\\.\\COM1:x", "\\\\?\\COM1:", "//./y",
  "CON", "CON:", "con:x", "LPT1:a", "COM¹:a", "CONx",
  "x/CON:y", "a:b", "a:", "dir/file.tar.gz", "привет/мир.тхт", "😀/a.😀", "a b/c d.e f",
];
console.log("posix", JSON.stringify(posix.sep), JSON.stringify(posix.delimiter));
for (const path of paths) {
  const parsed = posix.parse(path);
  console.log(
    JSON.stringify(path),
    JSON.stringify(posix.normalize(path)),
    JSON.stringify(posix.dirname(path)),
    JSON.stringify(posix.basename(path)),
    JSON.stringify(posix.basename(path, ".txt")),
    JSON.stringify(posix.basename(path, "b")),
    JSON.stringify(posix.extname(path)),
    posix.isAbsolute(path),
    JSON.stringify(parsed),
    JSON.stringify(posix.format(parsed)),
    JSON.stringify(posix.join(path, "x")),
    JSON.stringify(posix.join("x", path)),
    JSON.stringify(posix.join(path, "..", path)),
  );
}
console.log("win32", JSON.stringify(win32.sep), JSON.stringify(win32.delimiter));
for (const path of paths) {
  const parsed = win32.parse(path);
  console.log(
    JSON.stringify(path),
    JSON.stringify(win32.normalize(path)),
    JSON.stringify(win32.dirname(path)),
    JSON.stringify(win32.basename(path)),
    JSON.stringify(win32.basename(path, ".txt")),
    JSON.stringify(win32.basename(path, "b")),
    JSON.stringify(win32.extname(path)),
    win32.isAbsolute(path),
    JSON.stringify(parsed),
    JSON.stringify(win32.format(parsed)),
    JSON.stringify(win32.join(path, "x")),
    JSON.stringify(win32.join("x", path)),
    JSON.stringify(win32.join(path, "..", path)),
  );
}

// Absolute paths only, as the rest depends on the current directory.
const absolute = ["/a/b/c", "/a/b", "/a/x/y", "/", "/a/b/c/", "/A/B"];
for (const from of absolute)
  for (const to of absolute)
    console.log(JSON.stringify(posix.relative(from, to)), JSON.stringify(posix.resolve(from, to)),
                JSON.stringify(posix.resolve(from, "q", "..", "r")));
const drives = ["C:\\a\\b", "C:\\a\\c\\d", "c:\\A\\B\\e", "D:\\x", "C:\\", "\\\\s\\h\\a", "\\\\s\\h",
                "C:\\a\\b\\", "C:\\İ\\x", "C:\\i̇\\y"];
for (const from of drives)
  for (const to of drives)
    console.log(JSON.stringify(win32.relative(from, to)), JSON.stringify(win32.resolve(from, to)),
                JSON.stringify(win32.resolve(from, "q", "..", "r")),
                JSON.stringify(win32.toNamespacedPath(to)));

console.log(JSON.stringify(win32.format({ root: "C:\\", name: "f", ext: "txt" })));
console.log(JSON.stringify(win32.format({ dir: "C:\\d", base: "b", name: "ignored" })));
console.log(JSON.stringify(posix.format({ root: "/", dir: "/", base: "x" })));
console.log(JSON.stringify(posix.format({ ext: ".e" })), JSON.stringify(posix.format({})));
console.log(JSON.stringify(posix.join()), JSON.stringify(win32.join("", "")));
const parts = ["one", "two", "three.txt"];
console.log(JSON.stringify(posix.join(...parts)), JSON.stringify(win32.join("root", ...parts)));
console.log(JSON.stringify(posix.toNamespacedPath("/a/b")));
// The default style is the system's.
console.log(sep == win32.sep || sep == posix.sep, delimiter.length, join("a", "b") == ["a", "b"].join(sep));
console.log(["/x/a.ts", "/y/b.ts"].map(f => basename(f, ".ts")));
