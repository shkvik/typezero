#pragma once

#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/url/url_setter_table/url_setter_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/tz_modules/url_checker/url_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class LibraryCalls;

enum class RelativeUrls {
  REJECTED,
  ALLOWED,
};

class UrlChecker {
public:
  UrlChecker(SemanticState &owner, SemanticParts &all);

  void walkUrlNew(NewExpr *expr);
  bool proveUrlSetter(MemberExpr *member, const CallExpr *call);
  bool isValidUrlText(llvm::StringRef text, RelativeUrls relative) const;
  std::optional<std::string> literalText(const Expr *expr) const;
  const Expr *unconverted(const Expr *expr) const;
  bool isKnownAbsolute(const Expr *expr) const;

private:
  using ResultKnownBaseUrl = Result<const Expr *, UrlCheckerKnownBaseError>;
  using ResultUrlTextValid = Result<llvm::StringRef, UrlCheckerUrlTextError>;
  using ResultInputIsKnown = Result<const Expr *, UrlCheckerKnownInputError>;
  using ResultSettableText = Result<llvm::StringRef, UrlCheckerSetterValueError>;

  struct HostPort {
    llvm::StringRef host;
    llvm::StringRef port;
  };

  ResultKnownBaseUrl checkKnownBase(const Expr *base) const;
  ResultInputIsKnown checkKnownInput(const Expr *input, bool isBased) const;
  ResultUrlTextValid checkUrlText(llvm::StringRef text, bool isBased) const;
  ResultSettableText checkSetterValue(UrlSetter setter, llvm::StringRef value) const;
  ResultSettableText checkHrefValue(llvm::StringRef value) const;
  ResultSettableText checkProtocolValue(llvm::StringRef value) const;
  ResultSettableText checkPortValue(llvm::StringRef value) const;
  ResultSettableText checkHostValue(UrlSetter setter, llvm::StringRef value) const;
  HostPort settableHostPort(llvm::StringRef value) const;
  std::optional<std::u16string> literalUnits(const Expr *expr) const;
  bool isValidHost(llvm::StringRef host) const;
  bool isValidBracketHost(llvm::StringRef host) const;
  bool isAddressChar(char character) const;
  bool isHostChar(char character) const;
  bool isSchemeChar(char character) const;
  bool isAuthorityEnd(char character) const;
  bool isValidPort(llvm::StringRef port) const;
  bool hasScheme(llvm::StringRef text, size_t colon) const;
  std::optional<HostPort> splitHostPort(llvm::StringRef authority) const;
  bool isFailedArg(const Expr *arg) const;
  bool isLibraryClass(const Type *type, LibraryClass libraryClass) const;

  SemanticState &state;
  LibraryCalls *libraryCalls = nullptr;
};

}
