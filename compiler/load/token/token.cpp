#include "compiler/load/token/token.h"

namespace tz {

const char *TokenKinds::tokSpelling(Tok kind) {
  switch (kind) {
  case Tok::Eof:
    return "end of file";
  case Tok::Identifier:
    return "identifier";
  case Tok::IntLiteral:
    return "integer literal";
  case Tok::FloatLiteral:
    return "number literal";
  case Tok::StringLiteral:
    return "string literal";
  case Tok::TemplateString:
  case Tok::TemplateHead:
    return "template literal";
  case Tok::TemplateMiddle:
  case Tok::TemplateTail:
    return "'}'";
  case Tok::RegexLiteral:
    return "regular expression";
  case Tok::KwAbstract:
    return "'abstract'";
  case Tok::KwAccessor:
    return "'accessor'";
  case Tok::KwAs:
    return "'as'";
  case Tok::KwAsync:
    return "'async'";
  case Tok::KwBreak:
    return "'break'";
  case Tok::KwCase:
    return "'case'";
  case Tok::KwClass:
    return "'class'";
  case Tok::KwConst:
    return "'const'";
  case Tok::KwConstructor:
    return "'constructor'";
  case Tok::KwContinue:
    return "'continue'";
  case Tok::KwDebugger:
    return "'debugger'";
  case Tok::KwDeclare:
    return "'declare'";
  case Tok::KwDefault:
    return "'default'";
  case Tok::KwDelete:
    return "'delete'";
  case Tok::KwDo:
    return "'do'";
  case Tok::KwElse:
    return "'else'";
  case Tok::KwEnum:
    return "'enum'";
  case Tok::KwExport:
    return "'export'";
  case Tok::KwExtends:
    return "'extends'";
  case Tok::KwFalse:
    return "'false'";
  case Tok::KwFor:
    return "'for'";
  case Tok::KwFrom:
    return "'from'";
  case Tok::KwFunction:
    return "'function'";
  case Tok::KwGet:
    return "'get'";
  case Tok::KwIf:
    return "'if'";
  case Tok::KwImplements:
    return "'implements'";
  case Tok::KwImport:
    return "'import'";
  case Tok::KwIn:
    return "'in'";
  case Tok::KwInstanceof:
    return "'instanceof'";
  case Tok::KwInterface:
    return "'interface'";
  case Tok::KwLet:
    return "'let'";
  case Tok::KwModule:
    return "'module'";
  case Tok::KwNamespace:
    return "'namespace'";
  case Tok::KwNew:
    return "'new'";
  case Tok::KwOf:
    return "'of'";
  case Tok::KwOverride:
    return "'override'";
  case Tok::KwPrivate:
    return "'private'";
  case Tok::KwProtected:
    return "'protected'";
  case Tok::KwPublic:
    return "'public'";
  case Tok::KwReadonly:
    return "'readonly'";
  case Tok::KwReturn:
    return "'return'";
  case Tok::KwSet:
    return "'set'";
  case Tok::KwStatic:
    return "'static'";
  case Tok::KwSwitch:
    return "'switch'";
  case Tok::KwTrue:
    return "'true'";
  case Tok::KwType:
    return "'type'";
  case Tok::KwTypeof:
    return "'typeof'";
  case Tok::KwVar:
    return "'var'";
  case Tok::KwVoid:
    return "'void'";
  case Tok::KwWhile:
    return "'while'";
  case Tok::LParen:
    return "'('";
  case Tok::RParen:
    return "')'";
  case Tok::LBrace:
    return "'{'";
  case Tok::RBrace:
    return "'}'";
  case Tok::LBracket:
    return "'['";
  case Tok::RBracket:
    return "']'";
  case Tok::Comma:
    return "','";
  case Tok::Semi:
    return "';'";
  case Tok::Colon:
    return "':'";
  case Tok::Dot:
    return "'.'";
  case Tok::Ellipsis:
    return "'...'";
  case Tok::Question:
    return "'?'";
  case Tok::QuestionQuestion:
    return "'??'";
  case Tok::QuestionDot:
    return "'?.'";
  case Tok::Arrow:
    return "'=>'";
  case Tok::At:
    return "'@'";
  case Tok::Plus:
    return "'+'";
  case Tok::Minus:
    return "'-'";
  case Tok::Star:
    return "'*'";
  case Tok::StarStar:
    return "'**'";
  case Tok::Slash:
    return "'/'";
  case Tok::Percent:
    return "'%'";
  case Tok::PlusPlus:
    return "'++'";
  case Tok::MinusMinus:
    return "'--'";
  case Tok::Amp:
    return "'&'";
  case Tok::Pipe:
    return "'|'";
  case Tok::Caret:
    return "'^'";
  case Tok::Tilde:
    return "'~'";
  case Tok::Bang:
    return "'!'";
  case Tok::AmpAmp:
    return "'&&'";
  case Tok::PipePipe:
    return "'||'";
  case Tok::Shl:
    return "'<<'";
  case Tok::Shr:
    return "'>>'";
  case Tok::UShr:
    return "'>>>'";
  case Tok::Less:
    return "'<'";
  case Tok::LessEq:
    return "'<='";
  case Tok::Greater:
    return "'>'";
  case Tok::GreaterEq:
    return "'>='";
  case Tok::EqEq:
    return "'=='";
  case Tok::EqEqEq:
    return "'==='";
  case Tok::BangEq:
    return "'!='";
  case Tok::BangEqEq:
    return "'!=='";
  case Tok::Assign:
    return "'='";
  case Tok::PlusAssign:
    return "'+='";
  case Tok::MinusAssign:
    return "'-='";
  case Tok::StarAssign:
    return "'*='";
  case Tok::StarStarAssign:
    return "'**='";
  case Tok::SlashAssign:
    return "'/='";
  case Tok::PercentAssign:
    return "'%='";
  case Tok::AmpAssign:
    return "'&='";
  case Tok::PipeAssign:
    return "'|='";
  case Tok::CaretAssign:
    return "'^='";
  case Tok::ShlAssign:
    return "'<<='";
  case Tok::ShrAssign:
    return "'>>='";
  case Tok::UShrAssign:
    return "'>>>='";
  case Tok::QuestionQuestionAssign:
    return "'??='";
  case Tok::AmpAmpAssign:
    return "'&&='";
  case Tok::PipePipeAssign:
    return "'||='";
  }

  return "token";
}

bool TokenKinds::isKeyword(Tok kind) {
  return kind >= Tok::KwAbstract && kind <= Tok::KwWhile;
}

bool TokenKinds::isContextual(Tok kind) {
  switch (kind) {
    case Tok::KwAbstract:
    case Tok::KwAccessor:
    case Tok::KwAsync:
    case Tok::KwConstructor:
    case Tok::KwDeclare:
    case Tok::KwFrom:
    case Tok::KwGet:
    case Tok::KwModule:
    case Tok::KwNamespace:
    case Tok::KwOf:
    case Tok::KwOverride:
    case Tok::KwReadonly:
    case Tok::KwSet:
    case Tok::KwType:
      return true;
    default:
      return false;
  }
}

bool Token::isName() const {
  return this->kind == Tok::Identifier || TokenKinds::isContextual(this->kind);
}

}
