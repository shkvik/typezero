#include "compiler/packages/native_manifest/native_manifest_errors.h"

namespace tz::pkg {

std::string ErrorTextNative::notList() {
  return "'native' is a list of C libraries: [{ \"name\": \"sqlite\", \"license\": \"blessing\", "
         "\"licenseFile\": \"vendor/sqlite/LICENSE.md\", \"sources\": "
         "[\"vendor/sqlite/sqlite3.c\"] }]";
}

std::string ErrorTextNative::notObject() {
  return "a C library of 'native' is an object: { \"name\": ..., \"license\": ..., "
         "\"licenseFile\": ..., \"sources\": [...] }";
}

std::string ErrorTextNative::unknown(llvm::StringRef key) {
  return "'" + key.str() +
         "' is not a field of a C library; it has name, license, licenseFile, sources or "
         "libraries, defines, includes, systemLibraries, dlls, os, cpu and debug";
}

std::string ErrorTextNative::notString(llvm::StringRef key) {
  return "'" + key.str() + "' is a string";
}

std::string ErrorTextNative::notStrings(llvm::StringRef key) {
  return "'" + key.str() + "' is a list of strings";
}

std::string ErrorTextNative::empty(llvm::StringRef key) {
  return "'" + key.str() + "' names at least one file";
}

std::string ErrorTextNative::missing(llvm::StringRef key) {
  return "a C library of 'native' needs '" + key.str() + "'";
}

std::string ErrorTextNative::bothKinds() {
  return "a C library has 'sources' (C files tz builds) or 'libraries' (built .lib files), not "
         "both";
}

std::string ErrorTextNative::noKind() {
  return "a C library needs 'sources' (C files tz builds) or 'libraries' (built .lib files)";
}

std::string ErrorTextNative::sourceOnly(llvm::StringRef key) {
  return "'" + key.str() + "' belongs to a C library built from 'sources'";
}

std::string ErrorTextNative::libOnly(llvm::StringRef key) {
  return "'" + key.str() + "' belongs to a C library of built 'libraries'";
}

std::string ErrorTextNative::flag(llvm::StringRef define) {
  return "write the define without '-D': \"" + define.drop_front(2).str() + "\"";
}

std::string ErrorTextNative::notDefine(llvm::StringRef define) {
  return "'" + define.str() + "' is not a define: write NAME or NAME=value";
}

std::string ErrorTextNative::system(llvm::StringRef value) {
  return "'" + value.str() + "' is not an operating system of tz: write \"win32\" or \"linux\"";
}

std::string ErrorTextNative::processor(llvm::StringRef value) {
  return "'" + value.str() + "' is not a processor of tz: write \"x64\" or \"arm64\"";
}

std::string ErrorTextNative::debug() {
  return "'debug' is an object: { \"defines\": [\"SQLITE_DEBUG\"] }";
}

std::string ErrorTextNative::debugKey(llvm::StringRef key) {
  return "'" + key.str() + "' is not a field of 'debug'; it has 'defines'";
}

}
