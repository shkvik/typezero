#include "compiler/cli/syntax/ast_dump/ast_dump.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>

namespace tz {

using llvm::cast;

AstDump::AstDump(llvm::raw_ostream &target, const char *begin)
    : out(target),
      text(begin) {}

void AstDump::dumpFile(const SourceFile &file) {
  this->open() << "items " << file.items.size() << "\n";
  for (const Stmt *item : file.items) {
    this->dumpStmt(item);
  }
  this->dumpModules(file);
  return this->dumpTables(file);
}

llvm::raw_ostream &AstDump::open() {
  for (int i = 0; i < this->depth; ++i) {
    this->out << "  ";
  }

  return this->out;
}

void AstDump::missing() {
  this->open() << "-\n";
}

void AstDump::dumpModules(const SourceFile &file) {
  this->open() << "imports " << file.imports.size() << "\n";
  for (const ImportDecl &decl : file.imports) {
    this->open() << "Import " << this->text.offset(decl.loc) << " " << this->text.quote(decl.path)
                 << " " << this->text.offset(decl.pathLoc) << " " << this->text.quote(decl.whole)
                 << " " << this->text.offset(decl.wholeLoc) << " " << (decl.wholeIsDefault ? 1 : 0)
                 << " " << decl.names.size() << "\n";
    this->dumpModuleNames(decl.names);
  }
  this->open() << "exports " << file.exports.size() << "\n";
  for (const ExportDecl &decl : file.exports) {
    const ExportSource *source = decl.source();
    llvm::StringRef path = source != nullptr ? llvm::StringRef(source->path) : "";
    llvm::SMLoc pathLoc = source != nullptr ? source->pathLoc : llvm::SMLoc();
    this->open() << "Export " << this->text.offset(decl.loc) << " "
                 << static_cast<int>(decl.form.kind()) << " " << this->text.quote(path) << " "
                 << this->text.offset(pathLoc) << " " << decl.names().size() << "\n";
    this->dumpModuleNames(decl.names());
  }
}

void AstDump::dumpModuleNames(llvm::ArrayRef<ModuleName> names) {
  ++this->depth;
  for (const ModuleName &name : names) {
    this->open() << "Name " << this->text.quote(name.name) << " " << this->text.offset(name.loc)
                 << " " << this->text.quote(name.as) << " " << this->text.offset(name.asLoc)
                 << "\n";
  }
  --this->depth;
}

void AstDump::dumpTables(const SourceFile &file) {
  std::vector<std::string> fields;
  for (const auto &entry : file.fieldNames) {
    fields.push_back(entry.getKey().str());
  }
  std::sort(fields.begin(), fields.end());
  this->open() << "fields " << fields.size();
  for (const std::string &field : fields) {
    this->out << " " << this->text.quote(field);
  }
  this->out << "\n";
  std::vector<std::string> keys;
  for (const auto &entry : file.stringConstants) {
    keys.push_back(entry.getKey().str());
  }
  std::sort(keys.begin(), keys.end());
  this->open() << "constants " << keys.size() << "\n";
  for (const std::string &key : keys) {
    const std::vector<std::string> &values = file.stringConstants.find(key)->second;
    this->open() << "  " << this->text.quote(key) << " " << values.size();
    for (const std::string &value : values) {
      this->out << " " << this->text.quote(value);
    }
    this->out << "\n";
  }
}

void AstDump::dumpOptionalStmt(const Stmt *stmt) {
  if (stmt == nullptr) {
    return this->missing();
  }

  return this->dumpStmt(stmt);
}

void AstDump::dumpStmt(const Stmt *stmt) {
  int64_t loc = this->text.offset(stmt->loc());
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      return this->dumpBlock(cast<BlockStmt>(stmt));
    case Stmt::Kind::VarDecl:
      return this->dumpVarDecl(cast<VarDeclStmt>(stmt));
    case Stmt::Kind::Function:
      return this->dumpFunction(cast<FunctionDecl>(stmt));
    case Stmt::Kind::Expr:
      this->open() << "ExprStmt " << loc << "\n";
      ++this->depth;
      this->dumpExpr(cast<ExprStmt>(stmt)->expr);
      --this->depth;
      return;
    case Stmt::Kind::If: {
      const auto *branch = cast<IfStmt>(stmt);
      this->open() << "If " << loc << "\n";
      ++this->depth;
      this->dumpExpr(branch->cond);
      this->dumpStmt(branch->thenStmt);
      this->dumpOptionalStmt(branch->elseStmt);
      --this->depth;
      return;
    }
    case Stmt::Kind::While: {
      const auto *loop = cast<WhileStmt>(stmt);
      this->open() << "While " << loc << "\n";
      ++this->depth;
      this->dumpExpr(loop->cond);
      this->dumpStmt(loop->body);
      --this->depth;
      return;
    }
    case Stmt::Kind::DoWhile: {
      const auto *loop = cast<DoWhileStmt>(stmt);
      this->open() << "DoWhile " << loc << "\n";
      ++this->depth;
      this->dumpStmt(loop->body);
      this->dumpExpr(loop->cond);
      --this->depth;
      return;
    }
    case Stmt::Kind::For:
      return this->dumpFor(cast<ForStmt>(stmt));
    case Stmt::Kind::ForOf: {
      const auto *loop = cast<ForOfStmt>(stmt);
      this->open() << "ForOf " << loc << " const=" << loop->isConst << " keys=" << loop->keys
                   << " await=" << loop->isAwait << "\n";
      ++this->depth;
      this->dumpDeclarator(loop->binding);
      this->dumpExpr(loop->iterable);
      this->dumpStmt(loop->body);
      --this->depth;
      return;
    }
    case Stmt::Kind::Switch:
      return this->dumpSwitch(cast<SwitchStmt>(stmt));
    case Stmt::Kind::Labeled: {
      const auto *labeled = cast<LabeledStmt>(stmt);
      this->open() << "Labeled " << loc << " " << this->text.quote(labeled->label) << "\n";
      ++this->depth;
      this->dumpStmt(labeled->body);
      --this->depth;
      return;
    }
    case Stmt::Kind::Return:
      this->open() << "Return " << loc << "\n";
      ++this->depth;
      this->dumpOptionalExpr(cast<ReturnStmt>(stmt)->value);
      --this->depth;
      return;
    case Stmt::Kind::Break:
      this->open() << "Break " << loc << " " << this->text.quote(cast<BreakStmt>(stmt)->label)
                   << "\n";
      return;
    case Stmt::Kind::Continue:
      this->open() << "Continue " << loc << " " << this->text.quote(cast<ContinueStmt>(stmt)->label)
                   << "\n";
      return;
    case Stmt::Kind::Empty:
      this->open() << "Empty " << loc << "\n";
      return;
    case Stmt::Kind::Debugger:
      this->open() << "Debugger " << loc << "\n";
      return;
    case Stmt::Kind::TypeAlias:
      return this->dumpTypeAlias(cast<TypeAliasDecl>(stmt));
    case Stmt::Kind::Enum:
      return this->dumpEnum(cast<EnumDecl>(stmt));
    case Stmt::Kind::Class:
      return this->dumpClass(cast<ClassDecl>(stmt));
  }
}

void AstDump::dumpBlock(const BlockStmt *block) {
  this->open() << "Block " << this->text.offset(block->loc())
               << " end=" << this->text.offset(block->end) << " n=" << block->body.size() << "\n";
  ++this->depth;
  for (const Stmt *stmt : block->body) {
    this->dumpStmt(stmt);
  }
  --this->depth;
}

void AstDump::dumpVarDecl(const VarDeclStmt *decl) {
  this->open() << "VarDecl " << this->text.offset(decl->loc()) << " const=" << decl->isConst
               << " using=" << decl->isUsing << " n=" << decl->decls.size() << "\n";
  ++this->depth;
  for (const VarDeclarator &d : decl->decls) {
    this->dumpDeclarator(d);
  }
  --this->depth;
}

void AstDump::dumpDeclarator(const VarDeclarator &d) {
  this->open() << "Declarator " << this->text.offset(d.loc) << " " << this->text.quote(d.name);
  if (d.objectPattern()) {
    this->out << " form=object\n";
  } else if (d.destructured()) {
    this->out << " form=array " << d.patternLength() << " " << d.patternRest() << "\n";
  } else {
    this->out << " form=plain\n";
  }
  ++this->depth;
  this->dumpOptionalType(d.typeAnn);
  this->dumpOptionalExpr(d.init);
  --this->depth;
}

void AstDump::dumpFor(const ForStmt *loop) {
  this->open() << "For " << this->text.offset(loop->loc()) << "\n";
  ++this->depth;
  this->dumpOptionalStmt(loop->init);
  this->dumpOptionalExpr(loop->cond);
  this->dumpOptionalExpr(loop->step);
  this->dumpOptionalStmt(loop->body);
  --this->depth;
}

void AstDump::dumpSwitch(const SwitchStmt *stmt) {
  this->open() << "Switch " << this->text.offset(stmt->loc()) << " n=" << stmt->cases.size()
               << "\n";
  ++this->depth;
  this->dumpExpr(stmt->discriminant);
  for (const SwitchCase &c : stmt->cases) {
    this->open() << "Case " << this->text.offset(c.loc) << " n=" << c.body.size() << "\n";
    ++this->depth;
    this->dumpOptionalExpr(c.test);
    for (const Stmt *body : c.body) {
      this->dumpStmt(body);
    }
    --this->depth;
  }
  --this->depth;
}

void AstDump::dumpTypeAlias(const TypeAliasDecl *alias) {
  this->open() << "TypeAlias " << this->text.offset(alias->loc()) << " "
               << this->text.quote(alias->name) << " handle=" << alias->isHandle
               << " tp=" << alias->typeParams.size() << " bases=" << alias->bases.size() << "\n";
  ++this->depth;
  this->dumpTypeParams(alias->typeParams);
  this->dumpType(alias->type);
  this->dumpTypes(alias->bases);
  --this->depth;
}

void AstDump::dumpEnum(const EnumDecl *decl) {
  this->open() << "Enum " << this->text.offset(decl->loc()) << " " << this->text.quote(decl->name)
               << " n=" << decl->members.size() << "\n";
  ++this->depth;
  for (const EnumMember &member : decl->members) {
    this->open() << "EnumMember " << this->text.offset(member.loc) << " "
                 << this->text.quote(member.name) << "\n";
    ++this->depth;
    this->dumpOptionalExpr(member.init);
    --this->depth;
  }
  --this->depth;
}

void AstDump::dumpFunction(const FunctionDecl *fn) {
  this->open() << "Function " << this->text.offset(fn->loc()) << " " << this->text.quote(fn->name)
               << " arrow=" << fn->isArrow() << " concise=" << fn->concise()
               << " own=" << this->text.quote(fn->ownName())
               << " place=" << static_cast<int>(fn->place)
               << " body=" << static_cast<int>(fn->bodySource) << " gen=" << fn->isGenerator
               << " async=" << fn->isAsync << " role=" << this->text.role(fn)
               << " src=" << this->text.span(fn->source) << " tp=" << fn->typeParams.size()
               << " params=" << fn->params.size() << "\n";
  ++this->depth;
  const ClassMember *member = fn->member();
  if (member != nullptr) {
    this->dumpDecorators(member->decorators);
  }
  this->dumpTypeParams(fn->typeParams);
  for (const Param &param : fn->params) {
    this->dumpParam(param);
  }
  this->dumpOptionalType(fn->returnType);
  this->dumpOptionalStmt(fn->body);
  --this->depth;
}

void AstDump::dumpParam(const Param &param) {
  this->open() << "Param " << this->text.offset(param.loc) << " " << this->text.quote(param.name)
               << " form=" << static_cast<int>(param.form.kind()) << " prop=";
  if (param.property) {
    this->out << param.property->readonly << static_cast<int>(param.property->access);
  } else {
    this->out << "-";
  }
  this->out << " pattern=" << this->text.span(param.pattern) << " dec=" << param.decorators.size()
            << "\n";
  ++this->depth;
  this->dumpDecorators(param.decorators);
  this->dumpOptionalType(param.type);
  if (param.defaultValue() != nullptr) {
    this->dumpExpr(param.defaultValue());
  }
  --this->depth;
}

void AstDump::dumpTypeParams(const std::vector<TypeParamDecl> &params) {
  for (const TypeParamDecl &param : params) {
    this->open() << "TypeParam " << this->text.offset(param.loc) << " "
                 << this->text.quote(param.name) << "\n";
    ++this->depth;
    this->dumpOptionalType(param.constraint);
    this->dumpOptionalType(param.defaultType);
    --this->depth;
  }
}

void AstDump::dumpDecorators(const std::vector<Decorator> &decorators) {
  for (const Decorator &decorator : decorators) {
    this->open() << "Decorator " << this->text.offset(decorator.loc) << "\n";
    ++this->depth;
    this->dumpExpr(decorator.expr);
    --this->depth;
  }
}

void AstDump::dumpClass(const ClassDecl *cls) {
  bool generic = cls->generics.is<GenericTemplate>();
  this->open() << "Class " << this->text.offset(cls->loc()) << " " << this->text.quote(cls->name)
               << " abstract=" << cls->isAbstract << " expr=" << cls->expression()
               << " generic=" << generic << " src=" << this->text.span(cls->source)
               << " tp=" << cls->typeParams.size() << " base=" << cls->base.has_value()
               << " baseExpr=" << (cls->baseExpr != nullptr) << " ifaces=" << cls->interfaces.size()
               << " dec=" << cls->decorators.size() << " fields=" << cls->fields.size()
               << " methods=" << cls->methods.size() << " ctors=" << cls->constructors.size()
               << "\n";
  ++this->depth;
  this->dumpDecorators(cls->decorators);
  this->dumpTypeParams(cls->typeParams);
  if (cls->base) {
    this->dumpType(*cls->base);
  }
  if (cls->baseExpr != nullptr) {
    this->dumpExpr(cls->baseExpr);
  }
  this->dumpTypes(cls->interfaces);
  for (const ClassField &field : cls->fields) {
    this->dumpField(field);
  }
  for (const FunctionDecl *method : cls->methods) {
    this->dumpFunction(method);
  }
  for (const FunctionDecl *ctor : cls->constructors) {
    this->dumpFunction(ctor);
  }
  --this->depth;
}

void AstDump::dumpField(const ClassField &field) {
  this->open() << "Field " << this->text.offset(field.loc) << " " << this->text.quote(field.name)
               << " form=" << static_cast<int>(field.form.kind())
               << " presence=" << static_cast<int>(field.presence) << " ro=" << field.readonly
               << " override=" << field.isOverride << " access=" << static_cast<int>(field.access)
               << " accessor=" << field.accessor << " dec=" << field.decorators.size() << "\n";
  ++this->depth;
  this->dumpDecorators(field.decorators);
  this->dumpOptionalType(field.type);
  this->dumpOptionalExpr(field.init);
  --this->depth;
}

void AstDump::dumpOptionalExpr(const Expr *expr) {
  if (expr == nullptr) {
    return this->missing();
  }

  return this->dumpExpr(expr);
}

void AstDump::dumpChildren(llvm::ArrayRef<Expr *> exprs) {
  ++this->depth;
  for (const Expr *expr : exprs) {
    this->dumpExpr(expr);
  }
  --this->depth;
}

void AstDump::dumpExpr(const Expr *expr) {
  int64_t loc = this->text.offset(expr->loc());
  switch (expr->kind()) {
    case Expr::Kind::IntLit:
    case Expr::Kind::FloatLit:
    case Expr::Kind::BoolLit:
    case Expr::Kind::StringLit:
    case Expr::Kind::Undefined:
    case Expr::Kind::RegexLit:
      return this->dumpLiteral(expr);
    case Expr::Kind::Template:
      return this->dumpTemplate(cast<TemplateExpr>(expr));
    case Expr::Kind::ArrayLit:
      return this->dumpArray(cast<ArrayLitExpr>(expr));
    case Expr::Kind::ObjectLit:
      return this->dumpObject(cast<ObjectLitExpr>(expr));
    case Expr::Kind::Name:
    case Expr::Kind::Member:
    case Expr::Kind::Index:
      return this->dumpAccess(expr);
    case Expr::Kind::Unary:
    case Expr::Kind::Binary:
    case Expr::Kind::Assign:
    case Expr::Kind::Update:
    case Expr::Kind::Conditional:
    case Expr::Kind::Cast:
    case Expr::Kind::Sequence:
      return this->dumpOperation(expr);
    case Expr::Kind::Call:
      return this->dumpCall(cast<CallExpr>(expr));
    case Expr::Kind::New:
      return this->dumpNew(cast<NewExpr>(expr));
    case Expr::Kind::OptionalChain:
      this->open() << "OptionalChain " << loc << "\n";
      return this->dumpChildren({cast<OptionalChainExpr>(expr)->chain});
    case Expr::Kind::Function:
      this->open() << "FunctionExpr " << loc << "\n";
      ++this->depth;
      this->dumpFunction(cast<FunctionExpr>(expr)->fn);
      --this->depth;
      return;
    case Expr::Kind::Yield: {
      const auto *yield = cast<YieldExpr>(expr);
      this->open() << "Yield " << loc << " delegate=" << yield->delegate << "\n";
      ++this->depth;
      this->dumpOptionalExpr(yield->value);
      --this->depth;
      return;
    }
    case Expr::Kind::Await:
      this->open() << "Await " << loc << "\n";
      return this->dumpChildren({cast<AwaitExpr>(expr)->value});
    case Expr::Kind::Class:
      this->open() << "ClassExpr " << loc << "\n";
      ++this->depth;
      this->dumpClass(cast<ClassExpr>(expr)->decl);
      --this->depth;
      return;
  }
}

void AstDump::dumpLiteral(const Expr *expr) {
  int64_t loc = this->text.offset(expr->loc());
  switch (expr->kind()) {
    case Expr::Kind::IntLit: {
      const auto *lit = cast<IntLitExpr>(expr);
      this->open() << "IntLit " << loc << " " << lit->value << " " << this->text.quote(lit->text)
                   << "\n";
      return;
    }
    case Expr::Kind::FloatLit: {
      const auto *lit = cast<FloatLitExpr>(expr);
      this->open() << "FloatLit " << loc << " " << this->text.bits(lit->value) << " "
                   << this->text.quote(lit->text) << "\n";
      return;
    }
    case Expr::Kind::BoolLit:
      this->open() << "BoolLit " << loc << " " << cast<BoolLitExpr>(expr)->value << "\n";
      return;
    case Expr::Kind::StringLit:
      this->open() << "StringLit " << loc << " "
                   << this->text.units(cast<StringLitExpr>(expr)->value) << "\n";
      return;
    case Expr::Kind::RegexLit: {
      const auto *lit = cast<RegexLitExpr>(expr);
      this->open() << "RegexLit " << loc << " " << this->text.units(lit->pattern) << " "
                   << this->text.units(lit->flags) << " written=" << lit->written << "\n";
      return;
    }
    default:
      this->open() << "Undefined " << loc << "\n";
      return;
  }
}

void AstDump::dumpTemplate(const TemplateExpr *tmpl) {
  this->open() << "Template " << this->text.offset(tmpl->loc()) << " n=" << tmpl->quasis.size();
  for (const std::u16string &quasi : tmpl->quasis) {
    this->out << " " << this->text.units(quasi);
  }
  this->out << "\n";

  return this->dumpChildren(tmpl->exprs);
}

void AstDump::dumpArray(const ArrayLitExpr *lit) {
  this->open() << "ArrayLit " << this->text.offset(lit->loc()) << " n=" << lit->elements.size()
               << "\n";
  ++this->depth;
  for (const ArrayLitExpr::Element &element : lit->elements) {
    this->open() << "Element " << element.spread << " " << element.hole << "\n";
    this->dumpChildren({element.expr});
  }
  --this->depth;
}

std::string AstDump::propertyForm(const ObjectLitExpr::Property &prop) const {
  if (prop.method()) {
    return "Method";
  }
  if (prop.defaulted()) {
    return "Defaulted";
  }
  if (prop.key() != nullptr) {
    return "Computed";
  }
  if (!prop.spread()) {
    return "Value";
  }
  std::vector<llvm::StringRef> omit(prop.omit().begin(), prop.omit().end());
  std::string result = "Spread omit=" + this->text.quoteAll(omit) + " keys=[";
  for (size_t i = 0; i < prop.omitKeys().size(); ++i) {
    result += (i == 0 ? "" : ",") + std::to_string(this->text.offset(prop.omitKeys()[i]->loc()));
  }

  return result + "]";
}

void AstDump::dumpObject(const ObjectLitExpr *lit) {
  this->open() << "ObjectLit " << this->text.offset(lit->loc()) << " n=" << lit->props.size()
               << "\n";
  ++this->depth;
  for (const ObjectLitExpr::Property &prop : lit->props) {
    this->open() << "Property " << this->text.offset(prop.loc) << " " << this->text.quote(prop.name)
                 << " " << this->propertyForm(prop) << "\n";
    ++this->depth;
    if (prop.key() != nullptr) {
      this->dumpExpr(prop.key());
    }
    this->dumpExpr(prop.value);
    --this->depth;
  }
  --this->depth;
}

void AstDump::dumpAccess(const Expr *expr) {
  int64_t loc = this->text.offset(expr->loc());
  if (const auto *name = llvm::dyn_cast<NameExpr>(expr)) {
    this->open() << "Name " << loc << " " << this->text.quote(name->name)
                 << " targs=" << name->typeArgs.size() << "\n";
    ++this->depth;
    this->dumpTypes(name->typeArgs);
    --this->depth;
    return;
  }
  if (const auto *member = llvm::dyn_cast<MemberExpr>(expr)) {
    this->open() << "Member " << loc << " " << this->text.quote(member->name)
                 << " opt=" << member->optional << " targs=" << member->typeArgs.size() << "\n";
    ++this->depth;
    this->dumpExpr(member->object);
    this->dumpTypes(member->typeArgs);
    --this->depth;
    return;
  }
  const auto *index = cast<IndexExpr>(expr);
  this->open() << "Index " << loc << " opt=" << index->optional << "\n";

  return this->dumpChildren({index->object, index->index});
}

void AstDump::dumpOperation(const Expr *expr) {
  int64_t loc = this->text.offset(expr->loc());
  switch (expr->kind()) {
    case Expr::Kind::Unary: {
      const auto *unary = cast<UnaryExpr>(expr);
      this->open() << "Unary " << loc << " " << static_cast<int>(unary->op) << "\n";
      return this->dumpChildren({unary->operand});
    }
    case Expr::Kind::Binary: {
      const auto *binary = cast<BinaryExpr>(expr);
      this->open() << "Binary " << loc << " " << static_cast<int>(binary->op)
                   << " src=" << this->text.span(binary->source) << "\n";
      return this->dumpChildren({binary->lhs, binary->rhs});
    }
    case Expr::Kind::Assign: {
      const auto *assign = cast<AssignExpr>(expr);
      this->open() << "Assign " << loc << " ";
      if (assign->op) {
        this->out << static_cast<int>(*assign->op);
      } else {
        this->out << "-";
      }
      this->out << " src=" << this->text.span(assign->source) << "\n";
      return this->dumpChildren({assign->target, assign->value});
    }
    case Expr::Kind::Update: {
      const auto *update = cast<UpdateExpr>(expr);
      this->open() << "Update " << loc << " inc=" << update->increment
                   << " prefix=" << update->prefix << "\n";
      return this->dumpChildren({update->target});
    }
    case Expr::Kind::Conditional: {
      const auto *conditional = cast<ConditionalExpr>(expr);
      this->open() << "Conditional " << loc << "\n";
      return this->dumpChildren({conditional->cond, conditional->thenExpr, conditional->elseExpr});
    }
    case Expr::Kind::Cast: {
      const auto *castExpr = cast<CastExpr>(expr);
      this->open() << "Cast " << loc << "\n";
      ++this->depth;
      this->dumpExpr(castExpr->operand);
      this->dumpOptionalType(castExpr->target);
      --this->depth;
      return;
    }
    default: {
      const auto *sequence = cast<SequenceExpr>(expr);
      this->open() << "Sequence " << loc << " n=" << sequence->exprs.size() << "\n";
      return this->dumpChildren(sequence->exprs);
    }
  }
}

void AstDump::dumpCall(const CallExpr *call) {
  this->open() << "Call " << this->text.offset(call->loc()) << " opt=" << call->optional
               << " tagged=" << call->tagged << " n=" << call->args.size()
               << " spread=" << this->text.flags(call->spread) << "\n";
  ++this->depth;
  this->dumpExpr(call->callee);
  for (const Expr *arg : call->args) {
    this->dumpExpr(arg);
  }
  --this->depth;
}

void AstDump::dumpNew(const NewExpr *create) {
  this->open() << "New " << this->text.offset(create->loc()) << " "
               << this->text.quote(create->name) << " " << this->text.quote(create->member)
               << " targs=" << create->typeArgs.size() << " n=" << create->args.size()
               << " callee=" << (create->callee != nullptr) << "\n";
  ++this->depth;
  if (create->callee != nullptr) {
    this->dumpExpr(create->callee);
  }
  this->dumpTypes(create->typeArgs);
  for (const Expr *arg : create->args) {
    this->dumpExpr(arg);
  }
  --this->depth;
}

void AstDump::dumpOptionalType(const std::optional<TypeRef> &ref) {
  if (!ref) {
    return this->missing();
  }

  return this->dumpType(*ref);
}

void AstDump::dumpTypes(const std::vector<TypeRef> &refs) {
  for (const TypeRef &ref : refs) {
    this->dumpType(ref);
  }
}

void AstDump::dumpType(const TypeRef &ref) {
  this->open() << "Type " << this->text.offset(ref.loc) << " " << this->text.typeKind(ref) << " "
               << this->text.quote(ref.name) << " src=" << this->text.span(ref.source)
               << " n=" << ref.args.size() << this->text.typeExtras(ref) << "\n";
  ++this->depth;
  this->dumpTypes(ref.args);
  --this->depth;
}

}
