#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The kinds of files the tests of Stats and Dirent ask about, as the bits of
// a mode (S_IFREG...).
std::optional<int> fileKind(llvm::StringRef method) {
  if (method == "isFile") {
    return 0100000;
  }
  if (method == "isDirectory") {
    return 0040000;
  }
  if (method == "isSymbolicLink") {
    return 0120000;
  }
  if (method == "isFIFO") {
    return 0010000;
  }
  if (method == "isSocket") {
    return 0140000;
  }
  if (method == "isBlockDevice") {
    return 0060000;
  }
  if (method == "isCharacterDevice") {
    return 0020000;
  }

  return std::nullopt;
}

} // namespace

FsDeclarer::FsDeclarer(Sema &sema) : sema_(sema) {}

void FsDeclarer::declareFsClasses() {
  auto declare = [&](llvm::StringRef name,
                     llvm::ArrayRef<std::pair<llvm::StringRef, const Type *>> fields) {
    auto *cls = sema_.ctx_.make<ClassDecl>(llvm::SMLoc(), name);
    cls->builtin = true;
    cls->resolved = cls->declared = true;
    sema_.classes_.push_back(cls);
    Type *type = Type::classType(name, cls);
    cls->type = type;
    std::vector<Field> typeFields;
    for (const auto &[fieldName, fieldType] : fields) {
      ClassField f;
      f.name = fieldName;
      f.resolved = fieldType;
      f.access = fieldName.starts_with("#") ? Access::Private : Access::Public;
      cls->fields.push_back(f);
      typeFields.push_back({fieldName.str(), fieldType, false});
    }
    type->defineFields(std::move(typeFields));

    return cls;
  };
  // The numbers of a Stats, in the order Node prints them. Its dates
  // (`mtime`...) are made from them when read, as Node's getters make them,
  // and so are no fields: Node prints none (see checkStatsDate).
  const Type *d = Type::doubleTy();
  sema_.statsClass_ = declare("Stats", {{"dev", d},
                                  {"mode", d},
                                  {"nlink", d},
                                  {"uid", d},
                                  {"gid", d},
                                  {"rdev", d},
                                  {"blksize", d},
                                  {"ino", d},
                                  {"size", d},
                                  {"blocks", d},
                                  {"atimeMs", d},
                                  {"mtimeMs", d},
                                  {"ctimeMs", d},
                                  {"birthtimeMs", d}});
  // A Dirent keeps the kind of the file as the bits of a mode, which only
  // its tests read.
  sema_.direntClass_ =
      declare("Dirent",
              {{"name", Type::string()}, {"parentPath", Type::string()}, {"#type", Type::intTy()}});
}

// `st.atime`, `st.mtime`, `st.ctime` and `st.birthtime`: a new date from
// the number of milliseconds next to it; null for another name.
Expr *FsDeclarer::checkStatsDate(MemberExpr *member, ClassDecl *cls) {
  static const llvm::StringLiteral dates[] = {"atime", "mtime", "ctime", "birthtime"};
  if (cls != sema_.statsClass_ || !llvm::is_contained(dates, member->name)) {
    return nullptr;
  }

  return sema_.processChecker().builtinCall(member->loc(), member, Builtin::FsStatsDate, {}, Type::date());
}

bool FsDeclarer::isRuntimeClass(const ClassDecl *cls) const {
  return cls && (cls == sema_.systemErrorClass_ || cls == sema_.statsClass_ || cls == sema_.direntClass_ ||
                 cls == sema_.execSyncErrorClass_);
}

Expr *FsDeclarer::checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  if (cls != sema_.statsClass_ && cls != sema_.direntClass_) {
    return nullptr;
  }
  std::optional<int> kind = fileKind(member->name);
  if (!kind) {
    return nullptr;
  }
  if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
    return call;
  }
  auto *lit = sema_.ctx_.make<IntLitExpr>(member->loc(), uint64_t(*kind));
  lit->type = Type::intTy();
  call->args = {lit};
  call->builtin = Builtin::FsIsKind;
  call->type = Type::boolTy();
  member->type = Type::function({}, 0, false, Type::boolTy());

  return call;
}

} // namespace tz
