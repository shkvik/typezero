#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/errors/errors.h"
#include "compiler/program/library/decimal/decimal_literal/decimal_literal.h"
#include "compiler/program/pass_done/pass_done.h"
#include "compiler/program/program_inventory/program_inventory.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/narrowed_part/narrowed_part.h"
#include "compiler/semantic/semantic_state/event_store/event_store.h"
#include "compiler/semantic/semantic_state/widening_store/widening_store.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

class SemanticState;
class FeatureRegistry;
class ReturnCompleter;

class SemanticExternalFacade {
public:
  using ResultBodiesChecked = Result<PassDone, Errors>;

  SemanticExternalFacade(AstContext &ast, Diagnostics &diagnostics, SourceFile &loaded,
                         EventInference &inference);
  ~SemanticExternalFacade();

  void finishProgram();
  Errors inferProgram();
  Errors declareProgram();
  Errors checkInstances();
  Diagnostics &getDiagnostics();
  ResultBodiesChecked walkBodies();
  ProgramInventory takeInventory();
  bool isInsideLibrary(llvm::SMLoc loc) const;
  const WideningStore &getWideningStore() const;

  static std::string quotedName(llvm::StringRef name);
  static const Type *awaitedType(const Type *type);
  static bool copiesIntoRecord(const Type *from, const Type *to);
  static std::string loadChain(const Module *reader, const Module *source);
  static std::string notLoadedText(llvm::StringRef reader, llvm::StringRef what,
                                   llvm::StringRef source, llvm::StringRef chain,
                                   llvm::StringRef declared);
  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static std::optional<DecimalLiteral> decimalLiteral(const Expr *expr);
  static std::vector<NarrowedPart> narrowingsOf(const Type *from, const Type *to);

private:
  void checkClass(ClassDecl *cls);
  void checkFunction(FunctionDecl *fn);
  void checkLiftedFunction(FunctionDecl *fn);

  SourceFile &file;
  std::unique_ptr<FeatureRegistry> features;
  std::unique_ptr<SemanticState> state;
  std::unique_ptr<ReturnCompleter> completer;
};

}
