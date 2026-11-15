#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/url/url_setter_table/url_setter_table.h"
#include "compiler/program/types/types.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/tz_modules/library_tz_modules_registry_errors.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace tz {

class SemanticState;

enum class RelativeUrls {
  REJECTED,
  ALLOWED,
};

class LibraryTzModules {
public:
  explicit LibraryTzModules(SemanticState &owner);

  bool proveUrlSetter(MemberExpr *member, const CallExpr *call);


  bool isAuthorityEnd(char character) const;
  bool isKnownAbsolute(const Expr *expr) const;
  bool isLibraryClass(const Type *type, LibraryClass libraryClass) const;
  bool isValidUrlText(llvm::StringRef text, RelativeUrls relative) const;


  std::string knownPrefix(const Expr *expr) const;
  const Expr *unconverted(const Expr *expr) const;
  std::optional<std::string> literalText(const Expr *expr) const;

private:
  using ResultSettableText = Result<llvm::StringRef, LibraryTzModulesSetterValueError>;

  struct HostPort {
    llvm::StringRef host;
    llvm::StringRef port;
  };

  bool isHostChar(char character) const;
  bool isSchemeChar(char character) const;
  bool isAddressChar(char character) const;
  bool isValidPort(llvm::StringRef port) const;
  bool isValidHost(llvm::StringRef host) const;
  bool isValidBracketHost(llvm::StringRef host) const;
  bool hasScheme(llvm::StringRef text, size_t colon) const;


  ResultSettableText checkHrefValue(llvm::StringRef value) const;
  ResultSettableText checkPortValue(llvm::StringRef value) const;
  ResultSettableText checkProtocolValue(llvm::StringRef value) const;
  ResultSettableText checkHostValue(UrlSetter setter, llvm::StringRef value) const;
  ResultSettableText checkSetterValue(UrlSetter setter, llvm::StringRef value) const;


  std::string ownPrefix(const Expr *expr) const;
  std::string asciiPrefix(const std::u16string &text) const;


  const Expr *prefixInner(const Expr *expr) const;
  const Expr *addressArgument(const NewExpr *made) const;


  HostPort settableHostPort(llvm::StringRef value) const;
  std::optional<HostPort> splitHostPort(llvm::StringRef authority) const;
  std::optional<std::u16string> literalUnits(const Expr *expr) const;

  SemanticState &state;
};

}
