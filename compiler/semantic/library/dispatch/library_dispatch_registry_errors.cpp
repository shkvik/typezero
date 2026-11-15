#include "compiler/semantic/library/dispatch/library_dispatch_registry_errors.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

std::string ErrorTextNodeImport::conflict(llvm::StringRef alias, llvm::StringRef name,
                                          llvm::StringRef path) {
  return "import '" + alias.str() + "' conflicts with the declaration of '" + alias.str() +
         "' in this file; import it under another name, e.g. 'import { " + name.str() + " as " +
         name.str() + "2 } from \"" + path.str() + "\"'";
}

std::string ErrorTextNodeImport::declared(llvm::StringRef alias) {
  return "'" + alias.str() + "' is declared here";
}

std::string ErrorTextNodeImport::duplicate(llvm::StringRef alias) {
  return "duplicate import of '" + alias.str() + "'";
}

std::string ErrorTextNodeImport::list(llvm::ArrayRef<llvm::StringRef> used) {
  auto names = std::string();
  for (llvm::StringRef name : used) {
    names += (names.empty() ? "" : ", ") + name.str();
  }

  return names;
}

std::string ErrorTextNodeImport::form(bool isDefault) {
  return isDefault ? "default imports are not supported: there is no 'export default'"
                   : "'import * as' is not supported";
}

std::string ErrorTextNodeImport::process(llvm::StringRef form) {
  return form.str() + "; 'process' is global, as in Node, so the import is "
                      "not needed: drop it";
}

std::string ErrorTextNodeImport::buffer(llvm::StringRef form, llvm::StringRef path) {
  return form.str() +
         "; 'Buffer' is global, as in Node: import it as "
         "'import { Buffer } from \"" +
         path.str() + "\"', or drop the import";
}

std::string ErrorTextNodeImport::uses(llvm::StringRef form, llvm::StringRef names,
                                      llvm::StringRef path) {
  return form.str() + "; import the names this file uses: 'import { " + names.str() + " } from \"" +
         path.str() + "\"'";
}

std::string ErrorTextNodeImport::typical(llvm::StringRef form, llvm::StringRef name,
                                         llvm::StringRef path) {
  return form.str() + "; import the names you need, e.g. 'import { " + name.str() + " } from \"" +
         path.str() + "\"'";
}

std::string ErrorTextNodeValue::notValue(llvm::StringRef label, bool path,
                                         llvm::StringRef typical) {
  return "this is not a value: the functions of '" + label.str() +
         "' are used by their names, e.g. '" +
         (path ? std::string("posix.join(a, b)'") : typical.str() + "(...)'");
}

std::string ErrorTextNodeValue::onlyCalled(llvm::StringRef name, llvm::StringRef label) {
  return "'" + name.str() + "' of '" + label.str() +
         "' can only be called; where a function is expected, pass an "
         "arrow that calls it, e.g. 'x => " +
         name.str() + "(x)'";
}

std::string ErrorTextNodeValue::notFunc(llvm::StringRef name, llvm::StringRef label) {
  return "'" + name.str() + "' is not a function: call the functions of '" + label.str() +
         "' by their names";
}

std::string ErrorTextModuleExport::none(llvm::StringRef path, llvm::StringRef name) {
  return "'" + path.str() + "' has no export named '" + name.str() + "'";
}

std::string ErrorTextModuleExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() + "' is not supported yet";
}

std::string ErrorTextRuntimeObject::members(Type::Runtime kind) {
  switch (kind) {
    case Type::Runtime::Readline:
    case Type::Runtime::ReadlinePromises:
      return "; it has 'question()' and 'close()', and 'for await (const line of rl)' gives the "
             "lines";
    case Type::Runtime::AsyncIterator:
      return "; iterate it with 'for await'";
    default:
      return "";
  }
}

std::string ErrorTextRuntimeObject::noMember(llvm::StringRef name, llvm::StringRef type,
                                             Type::Runtime kind) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'" +
         ErrorTextRuntimeObject::members(kind);
}

std::string ErrorTextRuntimeObject::assigned(llvm::StringRef name, llvm::StringRef type,
                                             Type::Runtime kind) {
  return "property '" + name.str() + "' of type '" + type.str() + "' cannot be assigned to" +
         ErrorTextRuntimeObject::members(kind);
}

std::string ErrorTextReadlineMethod::callback() {
  return "'question' of node:readline takes a callback for the answer, "
         "'rl.question(query, answer => ...)'; for a Promise, import "
         "'createInterface' from \"node:readline/promises\"";
}

std::string ErrorTextReadlineMethod::options() {
  return "the options of 'question' with a callback are not "
         "supported yet";
}

std::string ErrorTextReadlineMethod::events(llvm::StringRef name) {
  return "'" + name.str() +
         "' of readline is not supported yet: its Interface is no "
         "EventEmitter yet (docs/events.md); read the lines with "
         "'for await (const line of rl)' or ask with 'question()'";
}

std::string ErrorTextAsyncIterator::noThrow() {
  return "an iterator has no 'throw()' in tz: an error is a "
         "value, not something thrown into a generator";
}

std::string ErrorTextAsyncIterator::notYet(llvm::StringRef name) {
  return "'" + name.str() +
         "()' of an async iterator is not supported yet; "
         "iterate it with 'for await'";
}

std::string ErrorTextLibraryCall::listener() {
  return "'asListener' gives a function";
}

std::string ErrorTextLibraryCall::asClass() {
  return "'asClass' takes a class";
}

std::string ErrorTextLibraryCall::ahead(llvm::StringRef name) {
  return "'" + name.str() +
         "' takes what an await waits for: a promise, or a "
         "value or a promise";
}

std::string ErrorTextLibraryCall::context() {
  return "'withContext' takes a function without parameters";
}

std::string ErrorTextFsExport::inTzFs(llvm::StringRef path, llvm::StringRef name) {
  return ErrorTextModuleExport::none(path, name) + "; it is in \"tz:fs\"";
}

std::string ErrorTextFsExport::callback(llvm::StringRef name, llvm::StringRef path,
                                        llvm::StringRef sync) {
  return "'" + name.str() + "' of '" + path.str() +
         "' takes a callback, which tz does not have; import '" + name.str() +
         "' from \"node:fs/promises\", which gives a Promise, or '" + sync.str() +
         "', which gives what it reads" +
         (name == "readFile" ? ", e.g. 'const text = await readFile(path, \"utf8\")'" : "");
}

std::string ErrorTextFsExport::syncOnly(llvm::StringRef name, llvm::StringRef path,
                                        llvm::StringRef sync) {
  return "'" + name.str() + "' of '" + path.str() +
         "' takes a callback, which tz does not have; import '" + sync.str() +
         "' and use what it returns";
}

std::string ErrorTextFsExport::waiting(llvm::StringRef path, llvm::StringRef name) {
  return ErrorTextModuleExport::none(path, name) + "; the function that waits is '" +
         name.drop_back(4).str() + "', or import '" + name.str() + "' from \"node:fs\"";
}

std::string ErrorTextFsExport::readline(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) + "; tz has createInterface";
}

std::string ErrorTextFsExport::tzFs(llvm::StringRef path, llvm::StringRef name, bool inNodeFs) {
  std::string message =
      ErrorTextModuleExport::none(path, name) + "; it has replaceSync, replace and ReplaceError";
  if (inNodeFs) {
    message += "; '" + name.str() + "' is in \"node:fs\"";
  }

  return message;
}

std::string ErrorTextFsExport::tzIo(llvm::StringRef path, llvm::StringRef name) {
  return ErrorTextModuleExport::none(path, name) + "; it has readLine";
}

std::string ErrorTextCryptoExport::webcrypto(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() +
         "' is the global crypto of Web Crypto, which needs no import: use "
         "'crypto.subtle', 'crypto.getRandomValues' and 'crypto.randomUUID'";
}

std::string ErrorTextCryptoExport::subtle(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() +
         "' is the global crypto.subtle, which needs no import: use 'crypto.subtle'";
}

std::string ErrorTextChildExport::async(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() +
         "' is not supported yet: it comes with EventEmitter and the streams of Node "
         "(docs/process.md, step 2); use '" +
         name.str() + "Sync', which waits for the program and gives what it printed";
}

std::string ErrorTextChildExport::fork(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() +
         "' is not possible: it starts another Node with a channel to it, and tz has "
         "no Node (docs/process.md, section 4); for parallel work use async functions "
         "(docs/async.md), and to run a program 'spawnSync(process.execPath, [...])'";
}

std::string ErrorTextChildExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) +
         "; tz has spawnSync, execFileSync and execSync";
}

std::string ErrorTextProcessExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) +
         "; tz has: argv, env, exit, exitCode, cwd, platform, "
         "execPath, stdin, stdout, stderr, uptime and pid";
}

std::string ErrorTextBufferExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) + "; tz has Buffer";
}

std::string ErrorTextTimersExport::none(llvm::StringRef path, llvm::StringRef name) {
  return ErrorTextModuleExport::none(path, name) +
         "; it has setTimeout, setImmediate, setInterval and scheduler";
}

}
