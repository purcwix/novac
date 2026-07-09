#pragma once
#include "ast.h"
#include "../lex/lexer.h"
#include "../error/novac_error.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace novac {

class Parser {
public:
    explicit Parser(std::string source, std::string filename = "<repl>");

    // Lex + parse → program node
    NodePtr parse();
    std::string          _source;
    std::string          _filename;
    std::vector<Token>   _tokens;
    size_t               _cur = 0;

    // symbol table: name → {type, isConst}
    struct SymInfo { std::optional<TypeExpr> type; bool isConst = false; };
    std::unordered_map<std::string, SymInfo> _symbols;

    // ── token primitives ────────────────────────────────────────────────────
    Token&       peek(int offset = 0);
    Token&       previous();
    bool         atEnd() const;
    Token        advance();
    bool         check(TT type, const std::string& value = "") const;
    bool         peekAsyncIsLambda() const;
    bool         match(TT type, const std::string& value = "");
    bool         matchIdent(const std::string& v);
    bool         isRC(const std::string& suffix = "");
    Token        consume(TT type, const std::string& msg);
    Token        consume(TT type, const std::string& value, const std::string& msg);
    Token        consumeName(const std::string& msg);   // IDENT | KEYWORD | OPERATOR

    // ── location helper ─────────────────────────────────────────────────────
    // Stamps current token position onto a node
    NodePtr      nd(NodePtr n);

    // ── statements ──────────────────────────────────────────────────────────
    NodePtr statement();
    NodePtr expressionStatement();

    NodePtr typeDeclaration();
    NodePtr structDeclaration();
    NodePtr interfaceDeclaration();
    NodePtr enumDeclaration();
    NodePtr traitDeclaration();
    NodePtr implDeclaration();
    NodePtr varDeclaration(bool isConst);
    NodePtr funcDeclaration();
    NodePtr classDeclaration(NodeList decorators);
    NodePtr forStatement();
    NodePtr eachStatement();
    NodePtr switchStatement();
    NodePtr matchStatement();
    NodePtr returnStatement(bool isGive);
    NodePtr tryStatement();
    NodePtr importStatement();
    NodePtr importBuiltinStatement();
    NodePtr importKitStatement();
    NodePtr fromStatement();
    NodePtr exportStatement();
    NodePtr defaultStatement();
    NodePtr namespaceStatement();
    NodePtr emitStatement();
    NodePtr onStatement();
    NodePtr doWhileStatement();
    NodePtr withStatement();
    NodePtr serverStatement();
    NodePtr fetchStatement();
    NodePtr branchBody(const std::string& name);
    NodeList blockBody();

    // ── expressions ─────────────────────────────────────────────────────────
    NodePtr expression(int minPrec = 0);
    NodePtr objectLiteral();
    NodePtr fstringExpr();
    NodeList parseCallArgs();
    NodePtr dotCmd();

    // ── type parsing ────────────────────────────────────────────────────────
    TypeExpr parseTypeExpr();
    TypeExpr parseTypeAtom();
    std::vector<std::string> parseTypeParams();

    // ── destructure ─────────────────────────────────────────────────────────
    ObjPattern  objectPattern();
    ArrPattern  arrayPattern(bool consumed = false);

    // ── func body ───────────────────────────────────────────────────────────
    struct FuncBody { std::vector<FuncArg> args; NodeList body; };
    FuncBody parseFuncBody(bool isArrow = false);

    // ── regex ────────────────────────────────────────────────────────────────
    // Tries to parse /pattern/flags at current position (manual source scan).
    // Returns nullptr if the / cannot be a regex (e.g. division context).
    NodePtr tryParseRegex();
    bool peekGeneratorIsLambda() const;

    // ── error ────────────────────────────────────────────────────────────────
    [[noreturn]] void error(const std::string& msg);
};

} // namespace novac