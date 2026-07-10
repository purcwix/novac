#include "parser.h"
#include <cassert>
#include <algorithm>

namespace novac
{

    // ════════════════════════════════════════════════════════════════════════════
    //  Construction / entry
    // ════════════════════════════════════════════════════════════════════════════

    Parser::Parser(std::string source, std::string filename)
        : _source(std::move(source)), _filename(std::move(filename))
    {
        Lexer lex(_source, _filename);
        _tokens = lex.tokenize();
    }

    NodePtr Parser::parse()
    {
        auto prog = makeNode(Node::Kind::Program, 1, 1);
        while (!atEnd())
        {
            auto s = statement();
            if (s)
                prog->body.push_back(std::move(s));
        }
        return prog;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Token primitives
    // ════════════════════════════════════════════════════════════════════════════

    Token &Parser::peek(int offset)
    {
        size_t idx = _cur + offset;
        if (idx >= _tokens.size())
            return _tokens.back(); // EOF
        return _tokens[idx];
    }

    Token &Parser::previous()
    {
        return _tokens[_cur > 0 ? _cur - 1 : 0];
    }

    bool Parser::atEnd() const
    {
        return _tokens[_cur].type == TT::EOF_TOKEN;
    }

    Token Parser::advance()
    {
        if (!atEnd())
            _cur++;
        return _tokens[_cur - 1];
    }

    bool Parser::check(TT type, const std::string &value) const
    {
        if (_cur >= _tokens.size())
            return false;
        const auto &t = _tokens[_cur];
        // allow DVAR 'default' to match KEYWORD 'default'
        if (type == TT::KEYWORD && value == "default" &&
            t.type == TT::DVAR && t.value == "default")
            return true;
        if (t.type != type)
            return false;
        return value.empty() || t.value == value;
    }

    bool Parser::match(TT type, const std::string &value)
    {
        if (check(type, value))
        {
            advance();
            return true;
        }
        return false;
    }

    bool Parser::matchIdent(const std::string &v)
    {
        if (check(TT::IDENTIFIER, v))
        {
            advance();
            return true;
        }
        return false;
    }

    bool Parser::isRC(const std::string &suffix)
    {
        if (!check(TT::RC))
            return false;
        if (suffix.empty())
            return true;
        return peek().value == "RC_" + suffix;
    }

    Token Parser::consume(TT type, const std::string &msg)
    {
        if (check(type))
            return advance();
        error(msg);
    }

    Token Parser::consume(TT type, const std::string &value, const std::string &msg)
    {
        if (check(type, value))
            return advance();
        error(msg);
    }

    Token Parser::consumeName(const std::string &msg)
    {
        auto &t = peek();
        if (t.type == TT::IDENTIFIER || t.type == TT::KEYWORD || t.type == TT::OPERATOR)
            return advance();
        error(msg);
    }

    NodePtr Parser::nd(NodePtr n)
    {
        // stamp with the current (pre-advance) token location — already set at call site
        return n;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Type expressions
    // ════════════════════════════════════════════════════════════════════════════

    std::vector<std::string> Parser::parseTypeParams()
    {
        std::vector<std::string> params;
        if (check(TT::OPERATOR, "<"))
        {
            advance();
            do
            {
                params.push_back(consume(TT::IDENTIFIER, "Expected type param").value);
            } while (match(TT::PUNCTUATION, ","));
            consume(TT::OPERATOR, ">", "Expected >");
        }
        return params;
    }

    TypeExpr Parser::parseTypeAtom()
    {
        // Object shape type
        if (check(TT::PUNCTUATION, "{"))
        {
            advance();
            TypeExpr te;
            te.kind = TypeExpr::Kind::Shape;
            while (!check(TT::PUNCTUATION, "}") && !atEnd())
            {
                std::string key = consume(TT::IDENTIFIER, "Expected field name").value;
                bool isOpt = match(TT::PUNCTUATION, "?");
                consume(TT::PUNCTUATION, ":", "Expected : in type shape");
                TypeExpr::ShapeField sf;
                sf.type = std::make_shared<TypeExpr>(parseTypeExpr());
                sf.optional = isOpt;
                te.fields[key] = std::move(sf);
                match(TT::PUNCTUATION, ",");
                match(TT::PUNCTUATION, ";");
            }
            consume(TT::PUNCTUATION, "}", "Expected }");
            return te;
        }
        // Literal value atoms
        if (check(TT::NUMBER))
        {
            TypeExpr te;
            te.kind = TypeExpr::Kind::Value;
            te.literalValue = advance().value;
            return te;
        }
        if (check(TT::STRING))
        {
            TypeExpr te;
            te.kind = TypeExpr::Kind::Value;
            te.literalValue = advance().value;
            return te;
        }
        if (check(TT::LITERAL))
        {
            TypeExpr te;
            te.kind = TypeExpr::Kind::Value;
            te.literalValue = advance().value;
            return te;
        }
        // Named type
        TypeExpr te;
        te.kind = TypeExpr::Kind::Named;
        te.name = consume(TT::IDENTIFIER, "Expected type name").value;
        te.args = [&]() -> std::vector<TypeExpr>
        {
            std::vector<TypeExpr> a;
            for (auto &s : parseTypeParams())
            {
                TypeExpr p;
                p.kind = TypeExpr::Kind::Named;
                p.name = s;
                a.push_back(std::move(p));
            }
            return a;
        }();
        // [] suffix → array type
        while (check(TT::PUNCTUATION, "[") && peek(1).value == "]")
        {
            advance();
            advance();
            TypeExpr arr;
            arr.kind = TypeExpr::Kind::Array;
            arr.of = std::make_unique<TypeExpr>(std::move(te));
            te = std::move(arr);
        }
        // () → function type
        if (check(TT::PUNCTUATION, "("))
        {
            advance();
            consume(TT::PUNCTUATION, ")", "Expected )");
            TypeExpr fn;
            fn.kind = TypeExpr::Kind::Function;
            fn.of = std::make_unique<TypeExpr>(std::move(te));
            te = std::move(fn);
        }
        return te;
    }

    TypeExpr Parser::parseTypeExpr()
    {
        TypeExpr base = parseTypeAtom();
        // union: |
        if (check(TT::OPERATOR, "|"))
        {
            TypeExpr u;
            u.kind = TypeExpr::Kind::Union;
            u.args.push_back(std::move(base));
            while (match(TT::OPERATOR, "|"))
                u.args.push_back(parseTypeAtom());
            return u;
        }
        // intersection: &
        if (check(TT::OPERATOR, "&"))
        {
            TypeExpr i;
            i.kind = TypeExpr::Kind::Intersect;
            i.args.push_back(std::move(base));
            while (match(TT::OPERATOR, "&"))
                i.args.push_back(parseTypeAtom());
            return i;
        }
        return base;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Destructure patterns
    // ════════════════════════════════════════════════════════════════════════════

    ObjPattern Parser::objectPattern()
    {
        consume(TT::PUNCTUATION, "{", "Expected {");
        ObjPattern pat;
        if (!check(TT::PUNCTUATION, "}"))
        {
            do
            {
                ObjPatternProp p;
                p.key = consume(TT::IDENTIFIER, "Expected property name").value;
                p.alias = p.key;
                if (match(TT::PUNCTUATION, ":"))
                    p.alias = consume(TT::IDENTIFIER, "Expected alias").value;
                if (match(TT::OPERATOR, "="))
                    p.defaultValue = expression();
                pat.props.push_back(std::move(p));
            } while (match(TT::PUNCTUATION, ","));
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return pat;
    }

    ArrPattern Parser::arrayPattern(bool consumed)
    {
        if (!consumed)
            consume(TT::PUNCTUATION, "[", "Expected [");
        ArrPattern pat;
        if (!check(TT::PUNCTUATION, "]"))
        {
            do
            {
                if (match(TT::OPERATOR, "..."))
                {
                    ArrPatternElem e;
                    e.name = consume(TT::IDENTIFIER, "Expected name").value;
                    e.rest = true;
                    pat.elements.push_back(std::move(e));
                    break;
                }
                ArrPatternElem e;
                e.name = consume(TT::IDENTIFIER, "Expected element name").value;
                if (match(TT::OPERATOR, "="))
                    e.defaultValue = expression();
                pat.elements.push_back(std::move(e));
            } while (match(TT::PUNCTUATION, ","));
        }
        consume(TT::PUNCTUATION, "]", "Expected ]");
        return pat;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Function body
    // ════════════════════════════════════════════════════════════════════════════

    Parser::FuncBody Parser::parseFuncBody(bool isArrow)
    {
        consume(TT::PUNCTUATION, "(", "Expected (");
        FuncBody fb;
        if (!check(TT::PUNCTUATION, ")"))
        {
            do
            {
                if (match(TT::OPERATOR, "..."))
                {
                    FuncArg a;
                    a.name = consume(TT::IDENTIFIER, "Expected rest param name").value;
                    a.rest = true;
                    fb.args.push_back(std::move(a));
                    break;
                }
                FuncArg a;
                a.name = consume(TT::IDENTIFIER, "Expected argument name").value;
                if (match(TT::PUNCTUATION, ":"))
                    a.type = parseTypeExpr();
                if (match(TT::OPERATOR, "="))
                    a.defaultValue = expression();
                fb.args.push_back(std::move(a));
            } while (match(TT::PUNCTUATION, ","));
        }
        consume(TT::PUNCTUATION, ")", "Expected )");
        if (isArrow)
            consume(TT::OPERATOR, "=>", "Expected =>");
        // `{...}` blocks and `RC_REST` chains are both handled transparently by
        // statement() itself (see the RC_REST case there), so a function body is
        // just a single statement.
        if (!atEnd())
            fb.body.push_back(statement());
        return fb;
    }

    NodeList Parser::blockBody()
    {
        NodeList body;
        if (!atEnd())
            body.push_back(statement());
        return body;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  STATEMENTS
    // ════════════════════════════════════════════════════════════════════════════

    NodePtr Parser::statement()
    {
        int line = peek().line, col = peek().column;

        // decorators
        NodeList decorators;
        while (check(TT::PUNCTUATION, "@"))
        {
            advance();
            decorators.push_back(expression());
        }

        if (match(TT::KEYWORD, "class"))
            return classDeclaration(std::move(decorators));
        if (match(TT::KEYWORD, "type"))
            return typeDeclaration();
        if (match(TT::KEYWORD, "struct"))
            return structDeclaration();
        if (match(TT::KEYWORD, "interface"))
            return interfaceDeclaration();
        if (match(TT::KEYWORD, "enum"))
            return enumDeclaration();
        if (match(TT::KEYWORD, "trait"))
            return traitDeclaration();
        if (match(TT::KEYWORD, "impl"))
            return implDeclaration();
        if (match(TT::KEYWORD, "const"))
            return varDeclaration(true);
        if (match(TT::KEYWORD, "var") || match(TT::KEYWORD, "let"))
            return varDeclaration(false);
        if (match(TT::KEYWORD, "function"))
            return funcDeclaration();
        if (match(TT::KEYWORD, "if"))
            return branchBody("if");
        if (match(TT::KEYWORD, "while"))
            return branchBody("while");
        if (match(TT::KEYWORD, "do"))
            return doWhileStatement();
        if (match(TT::KEYWORD, "repeat"))
            return branchBody("repeat");
        if (match(TT::KEYWORD, "until"))
            return branchBody("until");
        if (match(TT::KEYWORD, "unless"))
            return branchBody("unless");
        if (match(TT::KEYWORD, "for"))
            return forStatement();
        if (match(TT::KEYWORD, "each"))
            return eachStatement();
        if (match(TT::KEYWORD, "switch"))
            return switchStatement();
        if (match(TT::KEYWORD, "match"))
            return matchStatement();
        if (match(TT::KEYWORD, "return"))
            return returnStatement(false);
        if (match(TT::KEYWORD, "give"))
            return returnStatement(true);
        if (match(TT::KEYWORD, "try"))
            return tryStatement();
        if (match(TT::KEYWORD, "emit"))
            return emitStatement();
        if (match(TT::KEYWORD, "on"))
            return onStatement();
        if (match(TT::KEYWORD, "with"))
            return withStatement();
        if (match(TT::KEYWORD, "import"))
            return importStatement();
        if (match(TT::KEYWORD, "import_builtin"))
            return importBuiltinStatement();
        if (match(TT::KEYWORD, "import_kit"))
            return importKitStatement();
        if (match(TT::KEYWORD, "from"))
            return fromStatement();
        if (match(TT::KEYWORD, "server"))
            return serverStatement();
        if (match(TT::KEYWORD, "fetch"))
            return fetchStatement();
        if (match(TT::KEYWORD, "export"))
            return exportStatement();
        if (match(TT::KEYWORD, "default"))
            return defaultStatement();
        if (match(TT::KEYWORD, "namespace"))
            return namespaceStatement();

        if (match(TT::KEYWORD, "break"))
        {
            match(TT::PUNCTUATION, ";");
            return makeNode(Node::Kind::Break, line, col);
        }
        if (match(TT::KEYWORD, "continue"))
        {
            match(TT::PUNCTUATION, ";");
            return makeNode(Node::Kind::Continue, line, col);
        }
        if (match(TT::KEYWORD, "goback"))
        {
            match(TT::PUNCTUATION, ";");
            return makeNode(Node::Kind::Goback, line, col);
        }
        if (match(TT::KEYWORD, "yield"))
        {
            auto n = makeNode(Node::Kind::Yield, line, col);
            n->value = expression();
            match(TT::PUNCTUATION, ";");
            return n;
        }
        if (check(TT::EXEC_COMMENT))
        {
            auto n = makeNode(Node::Kind::ExecComment, line, col);
            n->strval = advance().value;
            return n;
        }
        if (match(TT::PUNCTUATION, "{"))
        {
            auto n = makeNode(Node::Kind::Block, line, col);
            while (!check(TT::PUNCTUATION, "}") && !atEnd())
                n->body.push_back(statement());
            consume(TT::PUNCTUATION, "}", "Expected }");
            return n;
        }
        // RC_REST captures "everything until the next RC_* token (or EOF)" as a
        // single block. This lets any "body" position — function bodies, branch
        // bodies, namespace bodies, etc. — be written as `statement()` and get
        // RC_REST support automatically, instead of every call site re-handling it.
        if (check(TT::RC, "RC_REST"))
        {
            advance();
            match(TT::PUNCTUATION, ";");
            auto n = makeNode(Node::Kind::Block, line, col);
            while (!atEnd() && !isRC())
                n->body.push_back(statement());
            return n;
        }
        if (match(TT::PUNCTUATION, "."))
            return dotCmd();

        // RC_stop used as a bare statement (e.g. at top level after a chain)
        if (isRC("stop"))
        {
            advance();
            match(TT::PUNCTUATION, ";");
            return nullptr;
        }

        return expressionStatement();
    }

    // ── type declarations ────────────────────────────────────────────────────────

    NodePtr Parser::typeDeclaration()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::TypeDecl, line, col);
        n->name = consume(TT::IDENTIFIER, "Expected type name").value;
        n->typeParams = parseTypeParams();
        consume(TT::OPERATOR, "=", "Expected = in type declaration");
        n->typeDef = parseTypeExpr();
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::structDeclaration()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::StructDecl, line, col);
        n->name = consume(TT::IDENTIFIER, "Expected struct name").value;
        n->typeParams = parseTypeParams();
        consume(TT::PUNCTUATION, "{", "Expected {");
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            NodeList decs;
            while (match(TT::PUNCTUATION, "@"))
                decs.push_back(expression());
            StructField sf;
            sf.name = consume(TT::IDENTIFIER, "Expected field name").value;
            consume(TT::PUNCTUATION, ":", "Expected : after field name");
            sf.type = parseTypeExpr();
            if (match(TT::OPERATOR, "="))
                sf.defaultValue = expression();
            sf.decorators = std::move(decs);
            n->structFields.push_back(std::move(sf));
            match(TT::PUNCTUATION, ",");
            match(TT::PUNCTUATION, ";");
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    NodePtr Parser::interfaceDeclaration()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::InterfaceDecl, line, col);
        n->name = consume(TT::IDENTIFIER, "Expected interface name").value;
        n->typeParams = parseTypeParams();
        if (match(TT::KEYWORD, "extends"))
        {
            do
            {
                n->extendsNames.push_back(consume(TT::IDENTIFIER, "Expected interface name").value);
            } while (match(TT::PUNCTUATION, ","));
        }
        consume(TT::PUNCTUATION, "{", "Expected {");
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            InterfaceMember m;
            m.name = consume(TT::IDENTIFIER, "Expected member name").value;
            m.isOptional = match(TT::PUNCTUATION, "?");
            if (check(TT::PUNCTUATION, "("))
            {
                advance();
                m.isMethod = true;
                if (!check(TT::PUNCTUATION, ")"))
                {
                    do
                    {
                        std::string pname = consume(TT::IDENTIFIER, "Expected param name").value;
                        std::optional<TypeExpr> pt;
                        if (match(TT::PUNCTUATION, ":"))
                            pt = parseTypeExpr();
                        m.params.emplace_back(pname, std::move(pt));
                    } while (match(TT::PUNCTUATION, ","));
                }
                consume(TT::PUNCTUATION, ")", "Expected )");
            }
            if (match(TT::PUNCTUATION, ":"))
                m.returnType = parseTypeExpr();
            n->ifaceMembers.push_back(std::move(m));
            match(TT::PUNCTUATION, ",");
            match(TT::PUNCTUATION, ";");
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    NodePtr Parser::enumDeclaration()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::EnumDecl, line, col);
        n->name = consume(TT::IDENTIFIER, "Expected enum name").value;
        consume(TT::PUNCTUATION, "{", "Expected {");
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            EnumVariant v;
            v.name = consume(TT::IDENTIFIER, "Expected variant name").value;
            if (check(TT::PUNCTUATION, "("))
            {
                advance();
                // tuple types — store as a single node representing the type list
                // we just skip for now and store a placeholder
                auto tup = makeNode(Node::Kind::Array, line, col);
                if (!check(TT::PUNCTUATION, ")"))
                {
                    do
                    {
                        auto te = parseTypeExpr();
                        auto tn = makeNode(Node::Kind::Value, line, col);
                        tn->strval = te.name;
                        tup->elements.push_back(std::move(tn));
                    } while (match(TT::PUNCTUATION, ","));
                }
                consume(TT::PUNCTUATION, ")", "Expected )");
                v.value = std::move(tup);
            }
            else if (match(TT::OPERATOR, "="))
            {
                v.value = expression();
            }
            n->enumVariants.push_back(std::move(v));
            match(TT::PUNCTUATION, ",");
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    NodePtr Parser::traitDeclaration()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::TraitDecl, line, col);
        n->name = consume(TT::IDENTIFIER, "Expected trait name").value;
        n->typeParams = parseTypeParams();
        consume(TT::PUNCTUATION, "{", "Expected {");
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            ClassMember m;
            m.kind = ClassMember::Kind::Method;
            m.name = consume(TT::IDENTIFIER, "Expected method name").value;
            auto fb = parseFuncBody();
            m.args = std::move(fb.args);
            m.body = std::move(fb.body);
            n->members.push_back(std::move(m));
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    NodePtr Parser::implDeclaration()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::ImplDecl, line, col);
        n->traitName = consume(TT::IDENTIFIER, "Expected trait name").value;
        if (match(TT::KEYWORD, "of") || matchIdent("for"))
            n->forType = consume(TT::IDENTIFIER, "Expected type name").value;
        consume(TT::PUNCTUATION, "{", "Expected {");
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            ClassMember m;
            m.kind = ClassMember::Kind::Method;
            m.name = consume(TT::IDENTIFIER, "Expected method name").value;
            auto fb = parseFuncBody();
            m.args = std::move(fb.args);
            m.body = std::move(fb.body);
            n->members.push_back(std::move(m));
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    // ── varDeclaration ───────────────────────────────────────────────────────────

    NodePtr Parser::varDeclaration(bool isConst)
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Declare, line, col);
        n->isConst = isConst;

        if (match(TT::OPERATOR, "*"))
            n->isPointer = true;

        if (check(TT::PUNCTUATION, "{"))
        {
            n->destructure = objectPattern();
        }
        else if (check(TT::PUNCTUATION, "["))
        {
            n->destructure = arrayPattern();
        }
        else
        {
            n->name = consume(TT::IDENTIFIER, "Expected variable name").value;

            // smart modifiers
            auto mods = std::make_shared<VarModifiers>();
            bool hasMods = false;
            while (true)
            {
                if (match(TT::KEYWORD, "setter"))
                {
                    mods->setter = expression(15);
                    hasMods = true;
                }
                else if (match(TT::KEYWORD, "getter"))
                {
                    mods->getter = expression(15);
                    hasMods = true;
                }
                else if (match(TT::KEYWORD, "frozen"))
                {
                    mods->frozen = true;
                    hasMods = true;
                }
                else if (match(TT::KEYWORD, "lazy"))
                {
                    mods->lazy = true;
                    hasMods = true;
                }
                else if (match(TT::KEYWORD, "tracked"))
                {
                    mods->tracked = true;
                    hasMods = true;
                }
                else if (match(TT::KEYWORD, "nonull"))
                {
                    mods->nonull = true;
                    hasMods = true;
                }
                else if (match(TT::KEYWORD, "once"))
                {
                    mods->once = true;
                    hasMods = true;
                }
                else if (match(TT::KEYWORD, "as"))
                {
                    if (match(TT::KEYWORD, "fnum") || match(TT::KEYWORD, "fint"))
                    {
                        mods->clampType = previous().value;
                        mods->clampExpr = expression(15);
                        hasMods = true;
                    }
                    else
                    {
                        n->explicitType = parseTypeExpr();
                    }
                }
                else
                    break;
            }
            if (hasMods)
                n->modifiers = std::move(mods);
            if (!n->explicitType && match(TT::PUNCTUATION, ":"))
                n->explicitType = parseTypeExpr();

            _symbols[n->name] = {n->explicitType, isConst};
        }

        if (match(TT::OPERATOR, "="))
            n->value = expression();
        match(TT::PUNCTUATION, ";");
        return n;
    }

    // ── funcDeclaration ──────────────────────────────────────────────────────────

    NodePtr Parser::funcDeclaration()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Function, line, col);

        // option flags: async, Strict, once, memo, generator, defer
        while (true)
        {
            if (match(TT::KEYWORD, "async") || matchIdent("async"))
            {
                n->isAsync = true;
            }
            else if (matchIdent("Strict"))
            {
                n->strictArgs = true;
            }
            else if (matchIdent("once"))
            {
                n->once_fn = true;
            }
            else if (matchIdent("memo"))
            {
                n->memoize = true;
            }
            else if (matchIdent("generator"))
            {
                n->isGenerator = true;
            }
            else if (matchIdent("defer"))
            {
                n->defer_fn = true;
                n->deferStmt = statement();
            }
            else if (matchIdent("timeout"))
            {
                n->timeout = expression();
            }
            else
                break;
        }

        n->name = consumeName("Expected function name").value;
        auto fb = parseFuncBody();
        n->funcArgs = std::move(fb.args);
        n->body = std::move(fb.body);
        if (match(TT::PUNCTUATION, ":"))
            n->returnType = parseTypeExpr();
        return n;
    }

    // ── classDeclaration ─────────────────────────────────────────────────────────

    NodePtr Parser::classDeclaration(NodeList decorators)
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Class, line, col);
        n->decorators = std::move(decorators);
        n->name = consume(TT::IDENTIFIER, "Expected class name").value;

        if (match(TT::KEYWORD, "extends"))
        {
            auto sc = makeNode(Node::Kind::Ref, peek().line, peek().column);
            sc->name = consume(TT::IDENTIFIER, "Expected superclass name").value;
            n->superClass = std::move(sc);
        }
        if (matchIdent("implements"))
        {
            do
            {
                n->impls.push_back(consume(TT::IDENTIFIER, "Expected interface name").value);
            } while (match(TT::PUNCTUATION, ","));
        }
        consume(TT::PUNCTUATION, "{", "Expected {");
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            NodeList mDecs;
            while (match(TT::PUNCTUATION, "@"))
                mDecs.push_back(expression());

            std::string accessor;
            if ((check(TT::KEYWORD, "get") || check(TT::KEYWORD, "set")) &&
                peek(1).type == TT::IDENTIFIER)
            {
                accessor = advance().value;
            }
            auto &tok = peek();
            if (tok.type == TT::IDENTIFIER || tok.type == TT::KEYWORD)
            {
                ClassMember m;
                m.name = advance().value;
                m.accessor = accessor;
                m.decorators = std::move(mDecs);
                if (check(TT::PUNCTUATION, "("))
                {
                    m.kind = ClassMember::Kind::Method;
                    auto fb = parseFuncBody();
                    m.args = std::move(fb.args);
                    m.body = std::move(fb.body);
                    if (match(TT::PUNCTUATION, ":"))
                        m.returnType = parseTypeExpr();
                }
                else
                {
                    m.kind = ClassMember::Kind::Field;
                    consume(TT::PUNCTUATION, ":", "Expected : after field name");
                    m.value = expression();
                    match(TT::PUNCTUATION, ",");
                }
                n->members.push_back(std::move(m));
            }
            else
            {
                error("Expected member name in class body");
            }
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    // ── control flow ─────────────────────────────────────────────────────────────
    NodePtr Parser::forStatement()
    {
        int line = peek().line, col = peek().column;
        size_t saved = _cur;

        // collect chain of loop heads
        struct LoopHead
        {
            enum class Kind
            {
                Of,
                In,
                Classic
            } kind;
            // ForOf / ForIn
            std::string varName, indexName;
            NodePtr iterable, object;
            // Classic
            NodePtr init, cond, update;
        };

        std::vector<LoopHead> heads;

        auto tryParseHead = [&]() -> bool
        {
            size_t s = _cur;
            // for let/var/const x of/in expr
            if (check(TT::KEYWORD, "var") || check(TT::KEYWORD, "let") || check(TT::KEYWORD, "const"))
            {
                advance();
                if (check(TT::IDENTIFIER))
                {
                    std::string vname = advance().value;
                    if (match(TT::KEYWORD, "of"))
                    {
                        LoopHead h;
                        h.kind = LoopHead::Kind::Of;
                        h.varName = vname;
                        // optional index: "of expr, idx" — but only if next after expr is comma then identifier then comma/brace
                        h.iterable = expression();
                        heads.push_back(std::move(h));
                        return true;
                    }
                    if (check(TT::OPERATOR, "in") || check(TT::KEYWORD, "in"))
                    {
                        advance();
                        LoopHead h;
                        h.kind = LoopHead::Kind::In;
                        h.varName = vname;
                        h.object = expression();
                        heads.push_back(std::move(h));
                        return true;
                    }
                }
                _cur = s;
            }
            // classic: init; cond; update
            // only try if we see something that looks like a classic for
            // (no 'of'/'in' keyword following a var decl)
            {
                LoopHead h;
                h.kind = LoopHead::Kind::Classic;
                h.init = statement();
                match(TT::PUNCTUATION, ";");
                if (!check(TT::PUNCTUATION, ";") && !check(TT::PUNCTUATION, ")") && !check(TT::PUNCTUATION, ",") && !check(TT::PUNCTUATION, "{"))
                    h.cond = expression();
                match(TT::PUNCTUATION, ";");
                if (!check(TT::PUNCTUATION, ")") && !check(TT::PUNCTUATION, ",") && !check(TT::PUNCTUATION, "{"))
                    h.update = expressionStatement();
                heads.push_back(std::move(h));
                return true;
            }
        };

        // parse first head
        if (!tryParseHead())
        {
            _cur = saved;
            // fallback — shouldn't happen
            auto n = makeNode(Node::Kind::Branch, line, col);
            n->branchType = "for";
            return n;
        }

        // parse additional heads separated by commas
        while (match(TT::PUNCTUATION, ","))
        {
            if (!tryParseHead())
                break;
        }

        // parse body
        NodeList body = blockBody();

        // build from innermost outward
        // innermost gets the real body
        NodeList currentBody = std::move(body);

        for (int i = (int)heads.size() - 1; i >= 0; i--)
        {
            auto &h = heads[i];
            NodeList wrappedBody = std::move(currentBody);

            if (h.kind == LoopHead::Kind::Of)
            {
                auto n = makeNode(Node::Kind::ForOf, line, col);
                n->varName = h.varName;
                n->iterable = std::move(h.iterable);
                n->body = std::move(wrappedBody);
                currentBody.clear();
                currentBody.push_back(std::move(n));
            }
            else if (h.kind == LoopHead::Kind::In)
            {
                auto n = makeNode(Node::Kind::ForIn, line, col);
                n->varName = h.varName;
                n->object = std::move(h.object);
                n->body = std::move(wrappedBody);
                currentBody.clear();
                currentBody.push_back(std::move(n));
            }
            else
            {
                auto n = makeNode(Node::Kind::Branch, line, col);
                n->branchType = "for";
                n->init = std::move(h.init);
                n->cond = std::move(h.cond);
                n->update = std::move(h.update);
                n->body = std::move(wrappedBody);
                currentBody.clear();
                currentBody.push_back(std::move(n));
            }
        }

        match(TT::PUNCTUATION, ";");
        return std::move(currentBody[0]);
    }
    NodePtr Parser::eachStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Each, line, col);
        n->varName = consume(TT::IDENTIFIER, "Expected variable name after each").value;
        if (match(TT::PUNCTUATION, ","))
            n->indexName = consume(TT::IDENTIFIER, "Expected index name").value;
        if (!match(TT::KEYWORD, "of"))
            error("Expected 'of' in each loop");
        n->iterable = expression();
        n->body = blockBody();
        return n;
    }

    NodePtr Parser::switchStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Switch, line, col);
        n->subject = expression();
        consume(TT::PUNCTUATION, "{", "Expected {");
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            if (match(TT::KEYWORD, "case"))
            {
                CaseNode c;
                c.kind = CaseNode::Kind::Case;
                c.line = peek().line;
                c.column = peek().column;
                c.value = expression();
                consume(TT::PUNCTUATION, ":", "Expected :");
                while (!check(TT::KEYWORD, "case") && !check(TT::KEYWORD, "default") &&
                       !check(TT::PUNCTUATION, "}") && !atEnd())
                    c.body.push_back(statement());
                n->cases.push_back(std::move(c));
            }
            else if (match(TT::KEYWORD, "default"))
            {
                CaseNode c;
                c.kind = CaseNode::Kind::Default;
                consume(TT::PUNCTUATION, ":", "Expected :");
                while (!check(TT::KEYWORD, "case") && !check(TT::PUNCTUATION, "}") && !atEnd())
                    c.body.push_back(statement());
                n->cases.push_back(std::move(c));
            }
            else
                error("Expected case or default");
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    NodePtr Parser::matchStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Match, line, col);
        n->subject = expression();
        consume(TT::PUNCTUATION, "{", "Expected {");
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            if (matchIdent("when") || match(TT::KEYWORD, "when"))
            {
                WhenNode w;
                w.line = peek().line;
                w.column = peek().column;
                w.patterns.push_back(expression());
                while (match(TT::PUNCTUATION, ","))
                    w.patterns.push_back(expression());
                w.body = blockBody();
                n->whenCases.push_back(std::move(w));
            }
            else if (match(TT::KEYWORD, "default"))
            {
                WhenNode w;
                w.body = blockBody();
                n->whenCases.push_back(std::move(w));
            }
            else
                error("Expected when or default");
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    NodePtr Parser::doWhileStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Branch, line, col);
        n->branchType = "do";
        n->body = blockBody();
        consume(TT::KEYWORD, "while", "Expected while after do block");
        n->cond = expression();
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::tryStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Try, line, col);
        n->tryBody = blockBody();

        if (match(TT::KEYWORD, "catch") || match(TT::RC, "RC_catch"))
        {
            if (check(TT::IDENTIFIER))
            {
                n->catchName = advance().value;
            }
            n->catchBody = blockBody();
        }
        if (match(TT::KEYWORD, "else") || match(TT::RC, "RC_else"))
            n->elseBody = blockBody();

        while (check(TT::KEYWORD, "handle") || isRC("handle"))
        {
            advance();
            HandleClause h;
            h.name = consume(TT::IDENTIFIER, "Expected effect name").value;
            if (match(TT::PUNCTUATION, "("))
            {
                if (!check(TT::PUNCTUATION, ")"))
                {
                    do
                    {
                        h.params.push_back(consume(TT::IDENTIFIER, "Expected param name").value);
                    } while (match(TT::PUNCTUATION, ","));
                }
                consume(TT::PUNCTUATION, ")", "Expected )");
            }
            h.body = blockBody();
            n->handleClauses.push_back(std::move(h));
        }
        if (match(TT::KEYWORD, "finally") || match(TT::RC, "RC_finally"))
            n->finallyBody = blockBody();
        if (isRC("stop"))
            advance(); // RC_stop closes the chain

        if (n->catchBody.empty() && n->finallyBody.empty() && n->handleClauses.empty())
            error("Try needs catch, handle, or finally");
        return n;
    }

    NodePtr Parser::returnStatement(bool isGive)
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Return, line, col);
        n->terminate = isGive;
        if (!check(TT::PUNCTUATION, ";") && !atEnd())
            n->value = expression();
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::branchBody(const std::string &name)
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Branch, line, col);
        n->branchType = name;

        if (name == "for")
        {
            n->init = statement();
            match(TT::PUNCTUATION, ";");
            if (!check(TT::PUNCTUATION, ";"))
                n->cond = expression();
            match(TT::PUNCTUATION, ";");
            if (!check(TT::PUNCTUATION, ")"))
                n->update = expressionStatement();
        }
        else
        {
            n->cond = expression();
        }
        n->body = blockBody();

        if (match(TT::KEYWORD, "else") || match(TT::RC, "RC_else"))
        {
            auto elseN = makeNode(Node::Kind::Branch, peek().line, peek().column);
            elseN->branchType = "else";
            // RC_else chains into another blockBody (may itself be RC_REST)
            if (previous().value == "RC_else")
                elseN->body = blockBody();
            else
                elseN->body.push_back(statement());
            n->next = std::move(elseN);
        }
        if (isRC("stop"))
            advance(); // RC_stop closes the chain
        match(TT::PUNCTUATION, ";");
        return n;
    }

    // ── import / export ──────────────────────────────────────────────────────────

    NodePtr Parser::importStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Import, line, col);
        // Use a simple source token (STRING/URL/IDENTIFIER) — avoids "as" cast ambiguity
        {
            auto src = makeNode(Node::Kind::Value, peek().line, peek().column);
            if (check(TT::STRING) || check(TT::URL) || check(TT::IDENTIFIER))
            {
                auto t = advance();
                src->strval = t.value;
            }
            else
            {
                src = expression();
            }
            n->source = std::move(src);
        }
        if (match(TT::KEYWORD, "as") || match(TT::OPERATOR, "as") || matchIdent("as"))
        {
            do
            {
                n->names.push_back(consume(TT::IDENTIFIER, "Expected imported name").value);
            } while (match(TT::PUNCTUATION, ","));
        }
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::importBuiltinStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::ImportBuiltin, line, col);
        do
        {
            n->names.push_back(consume(TT::IDENTIFIER, "Expected imported name").value);
        } while (match(TT::PUNCTUATION, ","));
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::importKitStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::ImportKit, line, col);
        n->kitName = consume(TT::IDENTIFIER, "Expected kit name").value;
        if (match(TT::KEYWORD, "as"))
        {
            do
            {
                n->names.push_back(consume(TT::IDENTIFIER, "Expected imported name").value);
            } while (match(TT::PUNCTUATION, ","));
        }
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::fromStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::FromImport, line, col);
        n->source = expression();
        consume(TT::KEYWORD, "import", "Expected import after from");
        do
        {
            n->names.push_back(consume(TT::IDENTIFIER, "Expected imported name").value);
        } while (match(TT::PUNCTUATION, ","));
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::exportStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Export, line, col);
        if (!check(TT::PUNCTUATION, ";"))
            n->value = expression();
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::defaultStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::DefaultExport, line, col);
        if (!check(TT::PUNCTUATION, ";"))
            n->value = expression();
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::namespaceStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Namespace, line, col);
        n->name = consume(TT::IDENTIFIER, "Expected namespace name").value;
        n->body = blockBody();
        return n;
    }

    NodePtr Parser::emitStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::EmitEvent, line, col);
        if (check(TT::STRING) || check(TT::IDENTIFIER))
        {
            auto ev = makeNode(Node::Kind::Value, peek().line, peek().column);
            ev->strval = advance().value;
            n->event = std::move(ev);
        }
        else
        {
            n->event = expression();
        }
        if (match(TT::PUNCTUATION, ","))
            n->value = expression();
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::onStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::OnEvent, line, col);
        if (check(TT::STRING) || check(TT::IDENTIFIER))
        {
            auto ev = makeNode(Node::Kind::Value, peek().line, peek().column);
            ev->strval = advance().value;
            n->event = std::move(ev);
        }
        else
        {
            n->event = expression();
        }
        if (match(TT::PUNCTUATION, "("))
        {
            n->param = consume(TT::IDENTIFIER, "Expected param name").value;
            consume(TT::PUNCTUATION, ")", "Expected )");
        }
        n->body = blockBody();
        return n;
    }

    NodePtr Parser::withStatement()
    {
        int line = peek().line, col = peek().column;
        if (check(TT::KEYWORD, "option") || check(TT::IDENTIFIER, "option"))
        {
            advance();
            auto n = makeNode(Node::Kind::WithOption, line, col);
            n->flag = consumeName("Expected option name").value;
            n->body = blockBody();
            return n;
        }
        auto n = makeNode(Node::Kind::WithCtx, line, col);
        n->target = expression();
        n->body = blockBody();
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::serverStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::Server, line, col);
        n->port = expression();
        consume(TT::PUNCTUATION, "{", "Expected {");
        static const std::unordered_set<std::string> HTTP_METHODS =
            {"get", "post", "put", "delete", "patch", "head", "options"};
        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            auto &mt = peek();
            if ((mt.type == TT::KEYWORD || mt.type == TT::OPERATOR || mt.type == TT::IDENTIFIER) &&
                HTTP_METHODS.count(mt.value))
            {
                ServerRoute r;
                r.line = mt.line;
                r.column = mt.column;
                r.method = advance().value;
                if (check(TT::URL))
                {
                    auto pn = makeNode(Node::Kind::UrlLiteral, peek().line, peek().column);
                    pn->strval = advance().value;
                    r.path = std::move(pn);
                }
                else if (check(TT::STRING))
                {
                    auto pn = makeNode(Node::Kind::Value, peek().line, peek().column);
                    pn->strval = advance().value;
                    r.path = std::move(pn);
                }
                else
                    r.path = expression();
                if (match(TT::PUNCTUATION, "("))
                {
                    if (!check(TT::PUNCTUATION, ")"))
                    {
                        do
                        {
                            r.params.push_back(consume(TT::IDENTIFIER, "Expected param name").value);
                        } while (match(TT::PUNCTUATION, ","));
                    }
                    consume(TT::PUNCTUATION, ")", "Expected )");
                }
                r.body = blockBody();
                n->routes.push_back(std::move(r));
            }
            else
                error("Expected HTTP method keyword in server block");
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    NodePtr Parser::fetchStatement()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::FetchStmt, line, col);
        consume(TT::PUNCTUATION, "(", "Expected ( after fetch");
        n->url = expression();
        if (match(TT::PUNCTUATION, ","))
            n->options = expression();
        consume(TT::PUNCTUATION, ")", "Expected )");
        if (match(TT::OPERATOR, "=>"))
            n->varNameOut = consume(TT::IDENTIFIER, "Expected variable name").value;
        match(TT::PUNCTUATION, ";");
        return n;
    }

    NodePtr Parser::expressionStatement()
    {
        int line = peek().line, col = peek().column;
        auto expr = expression();
        static const std::vector<std::string> ASSIGN_OPS =
            {"=", "+=", "-=", "*=", "/=", "%=", "**=", "&&=", "||=", "?\?="};
        for (const auto &op : ASSIGN_OPS)
        {
            if (match(TT::OPERATOR, op))
            {
                auto val = expression();
                auto n = makeNode(op == "=" ? Node::Kind::Assign : Node::Kind::CompoundAssign, line, col);
                n->left = std::move(expr);
                n->value = std::move(val);
                n->op = op;
                match(TT::PUNCTUATION, ";");
                auto wrap = makeNode(Node::Kind::Exec, line, col);
                wrap->value = std::move(n);
                return wrap;
            }
        }
        match(TT::PUNCTUATION, ";");
        auto wrap = makeNode(Node::Kind::Exec, line, col);
        wrap->value = std::move(expr);
        return wrap;
    }

    NodePtr Parser::dotCmd()
    {
        int line = peek().line, col = peek().column;
        auto n = makeNode(Node::Kind::DotCmd, line, col);
        n->cmd = makeNode(Node::Kind::Value, line, col);
        n->cmd->strval = consumeName("Expected command name.").value;
        n->callArgs = parseCallArgs();
        return n;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  EXPRESSIONS
    // ════════════════════════════════════════════════════════════════════════════

    NodeList Parser::parseCallArgs()
    {
        NodeList args;
        if (!check(TT::PUNCTUATION, ")"))
        {
            do
            {
                if (match(TT::OPERATOR, "..."))
                {
                    auto s = makeNode(Node::Kind::Spread, peek().line, peek().column);
                    s->value = expression();
                    args.push_back(std::move(s));
                }
                else
                {
                    args.push_back(expression());
                }
            } while (match(TT::PUNCTUATION, ","));
        }
        consume(TT::PUNCTUATION, ")", "Expected )");
        return args;
    }

    NodePtr Parser::tryParseRegex()
    {
        // Current token must be OPERATOR "/"
        // We manually scan _source to extract /pattern/flags, then skip tokens.
        const Token &slash = peek();
        int line = slash.line, col = slash.column;

        // Find byte offset of the opening / in _source by counting lines/cols
        size_t offset = 0;
        int curLine = 1, curCol = 1;
        while (offset < _source.size())
        {
            if (curLine == line && curCol == col)
                break;
            if (_source[offset] == '\n')
            {
                curLine++;
                curCol = 1;
            }
            else
            {
                curCol++;
            }
            offset++;
        }
        if (offset >= _source.size() || _source[offset] != '/')
            return nullptr; // couldn't locate it

        // Scan pattern: from offset+1 until unescaped /
        size_t p = offset + 1;
        std::string pattern;
        bool inCharClass = false;
        while (p < _source.size())
        {
            char c = _source[p];
            if (c == '\\' && p + 1 < _source.size())
            {
                pattern += c;
                pattern += _source[p + 1];
                p += 2;
                continue;
            }
            if (c == '[')
                inCharClass = true;
            if (c == ']')
                inCharClass = false;
            if (c == '/' && !inCharClass)
                break; // closing /
            if (c == '\n')
                return nullptr; // unterminated
            pattern += c;
            p++;
        }
        if (p >= _source.size() || _source[p] != '/')
            return nullptr; // no closing /

        p++; // skip closing /

        // Scan flags: consecutive alpha chars after closing /
        std::string flags;
        while (p < _source.size() && std::isalpha((unsigned char)_source[p]))
            flags += _source[p++];

        // Pattern is valid. Now skip tokens in _tokens that fall within
        // [offset, p) so the parser cursor moves past the regex literal.
        // The opening / is the current token — advance past it and any further
        // tokens whose source position falls before our end position (line/col
        // of offset+p).
        advance(); // consume the opening / token

        // Compute end line/col for p
        int endLine = line, endCol = col + 1;
        for (size_t i = offset + 1; i < p; i++)
        {
            if (_source[i] == '\n')
            {
                endLine++;
                endCol = 1;
            }
            else
                endCol++;
        }

        // Skip any tokens the lexer produced inside the regex body
        // (e.g. identifiers, operators, numbers from pattern content)
        while (!atEnd())
        {
            const Token &t = peek();
            if (t.line > endLine)
                break;
            if (t.line == endLine && t.column >= endCol)
                break;
            advance();
        }

        auto n = makeNode(Node::Kind::Regex, line, col);
        n->strval = pattern;
        n->flags = flags;
        return n;
    }

    NodePtr Parser::fstringExpr()
    {
        int line = peek().line, col = peek().column;
        consume(TT::FSTRING_START, "Expected f-string start");
        auto n = makeNode(Node::Kind::FString, line, col);
        while (!check(TT::FSTRING_END) && !atEnd())
        {
            if (match(TT::STRING_PART))
            {
                auto p = makeNode(Node::Kind::Value, previous().line, previous().column);
                p->strval = previous().value;
                n->parts.push_back(std::move(p));
                continue;
            }
            if (match(TT::INTERPOLATION_START))
            {
                n->parts.push_back(expression());
                consume(TT::INTERPOLATION_END, "Expected }");
                continue;
            }
            error("Unexpected token in f-string: " + peek().value);
        }
        consume(TT::FSTRING_END, "Unterminated f-string");
        return n;
    }

    NodePtr Parser::objectLiteral()
    {
        int line = peek().line, col = peek().column;
        consume(TT::PUNCTUATION, "{", "Expected {");
        auto n = makeNode(Node::Kind::Object, line, col);

        while (!check(TT::PUNCTUATION, "}") && !atEnd())
        {
            ObjectProp p;
            // spread: { ...expr }
            if (match(TT::OPERATOR, "..."))
            {
                p.kind = ObjectProp::Kind::Spread;
                p.value = expression();
                n->props.push_back(std::move(p));
                match(TT::PUNCTUATION, ",");
                continue;
            }
            // computed: { [expr]: val }
            if (match(TT::PUNCTUATION, "["))
            {
                p.kind = ObjectProp::Kind::Computed;
                p.keyExpr = expression();
                consume(TT::PUNCTUATION, "]", "Expected ]");
                consume(TT::PUNCTUATION, ":", "Expected :");
                p.value = expression();
                n->props.push_back(std::move(p));
                match(TT::PUNCTUATION, ",");
                continue;
            }
            // getter/setter: { get prop(){} }
            if ((check(TT::KEYWORD, "get") || check(TT::KEYWORD, "set")) &&
                peek(1).type == TT::IDENTIFIER)
            {
                p.kind = ObjectProp::Kind::Accessor;
                p.accessor = advance().value;
                p.key = advance().value;
                auto fb = parseFuncBody();
                auto fn = makeNode(Node::Kind::Function, line, col);
                fn->name = p.key;
                fn->funcArgs = std::move(fb.args);
                fn->body = std::move(fb.body);
                p.value = std::move(fn);
                n->props.push_back(std::move(p));
                match(TT::PUNCTUATION, ",");
                continue;
            }
            // normal key / method shorthand
            p.kind = ObjectProp::Kind::Normal;
            Token nameTok;
            if (check(TT::STRING) || check(TT::NUMBER))
                nameTok = advance();
            else
                nameTok = consumeName("Expected property name");
            p.key = nameTok.value;

            if (check(TT::PUNCTUATION, "("))
            {
                // method shorthand
                auto fb = parseFuncBody();
                auto fn = makeNode(Node::Kind::Function, line, col);
                fn->name = p.key;
                fn->funcArgs = std::move(fb.args);
                fn->body = std::move(fb.body);
                p.value = std::move(fn);
            }
            else if (match(TT::PUNCTUATION, "?"))
            {
                // boolean presence: { key? true/false/expr }
                p.boolProp = true;
                if (match(TT::LITERAL, "true") || matchIdent("true"))
                {
                    auto v = makeNode(Node::Kind::Value, line, col);
                    v->strval = "true";
                    v->numval = 1;
                    v->valueKind = Node::ValueKind::Literal;
                    p.value = std::move(v);
                }
                else if (match(TT::LITERAL, "false") || matchIdent("false"))
                {
                    auto v = makeNode(Node::Kind::Value, line, col);
                    v->strval = "false";
                    v->valueKind = Node::ValueKind::Literal;
                    p.value = std::move(v);
                }
                else
                    p.value = expression();
            }
            else if (match(TT::PUNCTUATION, ":"))
            {
                p.value = expression();
            }
            else
            {
                // shorthand: { key } → { key: key }
                auto ref = makeNode(Node::Kind::Ref, line, col);
                ref->name = p.key;
                p.value = std::move(ref);
            }
            n->props.push_back(std::move(p));
            match(TT::PUNCTUATION, ",");
        }
        consume(TT::PUNCTUATION, "}", "Expected }");
        return n;
    }

    // ── lambda lookahead ─────────────────────────────────────────────────────────

    bool Parser::peekAsyncIsLambda() const
    {
        const Token &n1 = _tokens[std::min(_cur + 1, _tokens.size() - 1)];
        if (n1.type == TT::IDENTIFIER && n1.value == "generator")
            return true;
        if (n1.type == TT::PUNCTUATION && n1.value == "(")
        {
            int depth = 0;
            size_t i = _cur + 1;
            while (i < _tokens.size())
            {
                const auto &t = _tokens[i++];
                if (t.type == TT::PUNCTUATION && t.value == "(")
                    depth++;
                else if (t.type == TT::PUNCTUATION && t.value == ")")
                {
                    if (--depth == 0)
                        break;
                }
            }
            if (i < _tokens.size())
                return _tokens[i].type == TT::OPERATOR && _tokens[i].value == "=>";
        }
        if (n1.type == TT::IDENTIFIER)
        {
            const Token &n2 = _tokens[std::min(_cur + 2, _tokens.size() - 1)];
            return n2.type == TT::OPERATOR && n2.value == "=>";
        }
        return false;
    }

    bool Parser::peekGeneratorIsLambda() const
    {
        const Token &n1 = _tokens[std::min(_cur + 1, _tokens.size() - 1)];
        if (n1.type == TT::KEYWORD && n1.value == "async")
            return true;
        if (n1.type == TT::PUNCTUATION && n1.value == "(")
        {
            int depth = 0;
            size_t i = _cur + 1;
            while (i < _tokens.size())
            {
                const auto &t = _tokens[i++];
                if (t.type == TT::PUNCTUATION && t.value == "(")
                    depth++;
                else if (t.type == TT::PUNCTUATION && t.value == ")")
                {
                    if (--depth == 0)
                        break;
                }
            }
            if (i < _tokens.size())
                return _tokens[i].type == TT::OPERATOR && _tokens[i].value == "=>";
        }
        if (n1.type == TT::IDENTIFIER)
        {
            const Token &n2 = _tokens[std::min(_cur + 2, _tokens.size() - 1)];
            return n2.type == TT::OPERATOR && n2.value == "=>";
        }
        return false;
    }

    // ── main expression parser ───────────────────────────────────────────────────

    NodePtr Parser::expression(int minPrec)
    {
        if (atEnd())
        {
            auto n = makeNode(Node::Kind::Value, peek().line, peek().column);
            n->strval = "EOF";
            return n;
        }

        int line = peek().line, col = peek().column;
        NodePtr left;

        // ── prefix / primary ──────────────────────────────────────────────────────
        auto &tok = peek();

        // fetch expression
        if (check(TT::KEYWORD, "fetch"))
        {
            advance();
            consume(TT::PUNCTUATION, "(", "Expected ( after fetch");
            auto n = makeNode(Node::Kind::FetchExpr, line, col);
            n->url = expression();
            if (match(TT::PUNCTUATION, ","))
                n->options = expression();
            consume(TT::PUNCTUATION, ")", "Expected )");
            left = std::move(n);
        }
        // async / generator lambda
        else if ((check(TT::KEYWORD, "async") && peekAsyncIsLambda()) ||
                 (check(TT::IDENTIFIER, "generator") && peekGeneratorIsLambda()))
        {
            bool isAsync = false, isGen = false;
            bool consumed = true;
            while (consumed)
            {
                consumed = false;
                if (match(TT::KEYWORD, "async"))
                {
                    isAsync = true;
                    consumed = true;
                }
                if (matchIdent("generator"))
                {
                    isGen = true;
                    consumed = true;
                }
            }
            std::vector<std::string> arrowArgs;
            if (match(TT::PUNCTUATION, "("))
            {
                if (!check(TT::PUNCTUATION, ")"))
                {
                    do
                    {
                        auto &t = peek();
                        if (t.type == TT::IDENTIFIER)
                        {
                            advance();
                            arrowArgs.push_back(t.value);
                        }
                        else
                        {
                            auto e = expression();
                            arrowArgs.push_back(e ? e->name : "");
                        }
                    } while (match(TT::PUNCTUATION, ","));
                }
                consume(TT::PUNCTUATION, ")", "Expected )");
            }
            else
            {
                arrowArgs.push_back(consume(TT::IDENTIFIER, "Expected argument").value);
            }
            consume(TT::OPERATOR, "=>", "Expected =>");
            auto n = makeNode(Node::Kind::ArrowFunc, line, col);
            n->names = arrowArgs;
            n->isAsync = isAsync;
            n->isGenerator = isGen;
            if (check(TT::PUNCTUATION, "{"))
                n->body = blockBody();
            else
            {
                auto r = makeNode(Node::Kind::Return, peek().line, peek().column);
                r->value = expression();
                r->terminate = true;
                n->body.push_back(std::move(r));
            }
            left = std::move(n);
        }
        // await
        else if (check(TT::KEYWORD, "await"))
        {
            advance();
            auto n = makeNode(Node::Kind::Await, line, col);
            n->operand = expression(14);
            left = std::move(n);
        }
        // using
        else if (check(TT::KEYWORD, "using"))
        {
            advance();
            auto nm = consumeName("Expected option name.");
            if (match(TT::PUNCTUATION, "?"))
            {
                auto n = makeNode(Node::Kind::HasOpt, line, col);
                n->name = nm.value;
                left = std::move(n);
            }
            else if (match(TT::OPERATOR, "="))
            {
                auto n = makeNode(Node::Kind::OptDecl, line, col);
                n->name = nm.value;
                n->value = expression();
                left = std::move(n);
            }
            else
            {
                auto n = makeNode(Node::Kind::GetOpt, line, col);
                n->name = nm.value;
                left = std::move(n);
            }
        }
        // partial
        else if (check(TT::KEYWORD, "partial"))
        {
            advance();
            consume(TT::PUNCTUATION, "[", "Expected [ after partial");
            auto n = makeNode(Node::Kind::Partial, line, col);
            n->callee = expression();
            consume(TT::PUNCTUATION, "]", "Expected ]");
            consume(TT::PUNCTUATION, "(", "Expected (");
            n->callArgs = parseCallArgs();
            left = std::move(n);
        }
        // new
        else if (check(TT::KEYWORD, "new"))
        {
            advance();
            auto n = makeNode(Node::Kind::New, line, col);
            n->callee = expression(16);
            left = std::move(n);
        }
        // link
        else if (check(TT::KEYWORD, "link"))
        {
            advance();
            consume(TT::PUNCTUATION, "(", "Expected ( after link");
            auto n = makeNode(Node::Kind::Link, line, col);
            n->value = expression();
            consume(TT::PUNCTUATION, ")", "Expected )");
            left = std::move(n);
        }
        // rate
        else if (check(TT::KEYWORD, "rate"))
        {
            advance();
            consume(TT::PUNCTUATION, "(", "Expected ( after rate");
            auto n = makeNode(Node::Kind::RateCast, line, col);
            n->value = expression();
            consume(TT::PUNCTUATION, ")", "Expected )");
            n->castType = consumeName("Expected cast type").value;
            left = std::move(n);
        }
        // prefix ++ --
        else if (tok.type == TT::OPERATOR && (tok.value == "++" || tok.value == "--"))
        {
            auto n = makeNode(Node::Kind::Prefix, line, col);
            n->op = advance().value;
            n->operand = expression(14);
            left = std::move(n);
        }
        // other prefix unary
        else if (tok.type == TT::OPERATOR && tok.isUnary && !tok.isPostfix)
        {
            std::string op = advance().value;
            if (op == "*")
            {
                auto n = makeNode(Node::Kind::Deref, line, col);
                n->operand = expression(14);
                left = std::move(n);
            }
            else if (op == "...")
            {
                auto n = makeNode(Node::Kind::Spread, line, col);
                n->value = expression(14);
                left = std::move(n);
            }
            else if (op == "delete" && (check(TT::URL) || check(TT::STRING)))
            {
                auto n = makeNode(Node::Kind::HttpRequest, line, col);
                n->method = "delete";
                auto un = makeNode(check(TT::URL) ? Node::Kind::UrlLiteral : Node::Kind::Value, peek().line, peek().column);
                un->strval = advance().value;
                n->url = std::move(un);
                if (match(TT::PUNCTUATION, "("))
                {
                    n->callArgs = parseCallArgs();
                }
                else
                    consume(TT::PUNCTUATION, ")", "Expected )");
                left = std::move(n);
            }
            else
            {
                auto n = makeNode(Node::Kind::Unary, line, col);
                n->op = op;
                n->operand = expression(14);
                left = std::move(n);
            }
        }
        // grouped expression / arrow func
        else if (tok.type == TT::PUNCTUATION && tok.value == "(")
        {
            advance();
            NodeList args;
            if (!check(TT::PUNCTUATION, ")"))
            {
                do
                {
                    args.push_back(expression());
                } while (match(TT::PUNCTUATION, ","));
            }
            consume(TT::PUNCTUATION, ")", "Expected )");
            if (match(TT::OPERATOR, "=>"))
            {
                auto n = makeNode(Node::Kind::ArrowFunc, line, col);
                for (auto &a : args)
                    if (a)
                        n->names.push_back(a->name);
                n->body = check(TT::PUNCTUATION, "{") ? blockBody() : [&]()
                {
                    NodeList b;
                    auto r = makeNode(Node::Kind::Return, peek().line, peek().column);
                    r->value = expression();
                    r->terminate = true;
                    b.push_back(std::move(r));
                    return b;
                }();
                left = std::move(n);
            }
            else
            {
                left = args.empty() ? makeNode(Node::Kind::Value, line, col) : std::move(args[0]);
            }
        }
        // array literal
        else if (match(TT::PUNCTUATION, "["))
        {
            auto n = makeNode(Node::Kind::Array, line, col);
            if (!check(TT::PUNCTUATION, "]"))
            {
                do
                {
                    if (match(TT::OPERATOR, "..."))
                    {
                        auto s = makeNode(Node::Kind::Spread, peek().line, peek().column);
                        s->value = expression();
                        n->elements.push_back(std::move(s));
                    }
                    else
                        n->elements.push_back(expression());
                } while (match(TT::PUNCTUATION, ","));
            }
            consume(TT::PUNCTUATION, "]", "Expected ]");
            left = std::move(n);
        }
        // object literal
        else if (tok.type == TT::PUNCTUATION && tok.value == "{")
        {
            left = objectLiteral();
        }
        // regex literal: /pattern/flags  (manual source scan — disambiguated at parser level)
        else if (tok.type == TT::OPERATOR && tok.value == "/")
        {
            NodePtr rx = tryParseRegex();
            if (rx)
            {
                left = std::move(rx);
            }
            else
            {
                advance();
                auto n = makeNode(Node::Kind::Unary, line, col);
                n->op = "/";
                n->operand = expression(14);
                left = std::move(n);
            }
        }
        // f-string
        else if (tok.type == TT::FSTRING_START)
        {
            left = fstringExpr();
        }
        // number / string / literal
        else if (tok.type == TT::NUMBER || tok.type == TT::STRING || tok.type == TT::LITERAL)
        {
            auto n = makeNode(Node::Kind::Value, line, col);
            auto t = advance();
            n->strval = t.value;
            n->numval = t.numval;
            n->valueKind = (t.type == TT::NUMBER)    ? Node::ValueKind::Number
                           : (t.type == TT::LITERAL) ? Node::ValueKind::Literal
                                                     : Node::ValueKind::String;
            left = std::move(n);
        }
        // interpolation end guard
        else if (tok.type == TT::INTERPOLATION_END)
        {
            return nullptr;
        }
        // identifier / arrow func
        else if (tok.type == TT::IDENTIFIER)
        {
            std::string name = advance().value;
            if (match(TT::OPERATOR, "=>"))
            {
                auto n = makeNode(Node::Kind::ArrowFunc, line, col);
                n->names = {name};
                n->body = check(TT::PUNCTUATION, "{") ? blockBody() : [&]()
                {
                    NodeList b;
                    auto r = makeNode(Node::Kind::Return, peek().line, peek().column);
                    r->value = expression();
                    r->terminate = true;
                    b.push_back(std::move(r));
                    return b;
                }();
                left = std::move(n);
            }
            else
            {
                auto n = makeNode(Node::Kind::Ref, line, col);
                n->name = name;
                auto it = _symbols.find(name);
                if (it != _symbols.end())
                    n->explicitType = it->second.type;
                left = std::move(n);
            }
        }
        // dvar
        else if (tok.type == TT::DVAR)
        {
            auto t = advance();
            auto n = makeNode(Node::Kind::DVar, line, col);
            n->name = t.value;
            n->srcLine = t.srcLine;
            n->srcCol = t.srcCol;
            left = std::move(n);
        }
        // URL literal
        else if (tok.type == TT::URL)
        {
            auto n = makeNode(Node::Kind::UrlLiteral, line, col);
            n->strval = advance().value;
            left = std::move(n);
        }
        // HTTP method keywords as first-class expressions
        else if (tok.type == TT::KEYWORD &&
                 (tok.value == "get" || tok.value == "post" || tok.value == "put" || tok.value == "patch" ||
                  tok.value == "head" || tok.value == "options"))
        {
            std::string method = advance().value;
            auto n = makeNode(Node::Kind::HttpRequest, line, col);
            n->method = method;
            if (check(TT::URL) || check(TT::STRING))
            {
                auto un = makeNode(check(TT::URL) ? Node::Kind::UrlLiteral : Node::Kind::Value, peek().line, peek().column);
                un->strval = advance().value;
                n->url = std::move(un);
            }
            else
                n->url = expression(15);
            if (match(TT::PUNCTUATION, "("))
            {
                if (!check(TT::PUNCTUATION, ")"))
                {
                    do
                    {
                        if (match(TT::OPERATOR, "..."))
                        {
                            auto s = makeNode(Node::Kind::Spread, peek().line, peek().column);
                            s->value = expression();
                            n->callArgs.push_back(std::move(s));
                        }
                        else
                            n->callArgs.push_back(expression());
                    } while (match(TT::PUNCTUATION, ","));
                }
                consume(TT::PUNCTUATION, ")", "Expected )");
            }
            left = std::move(n);
        }
        // nat / unat
        else if (tok.type == TT::KEYWORD && tok.value == "nat")
        {
            advance();
            auto n = makeNode(Node::Kind::Native, line, col);
            n->value = expression();
            left = std::move(n);
        }
        else if (tok.type == TT::KEYWORD && tok.value == "unat")
        {
            advance();
            auto n = makeNode(Node::Kind::Unative, line, col);
            n->value = expression();
            left = std::move(n);
        }
        // throw expression
        else if (check(TT::KEYWORD, "throw"))
        {
            advance();
            auto n = makeNode(Node::Kind::Throw, line, col);
            n->value = expression(14);
            n->isExpr = true;
            left = std::move(n);
        }
        // perform effect
        else if (check(TT::KEYWORD, "perform"))
        {
            advance();
            auto n = makeNode(Node::Kind::Perform, line, col);
            n->name = consume(TT::IDENTIFIER, "Expected effect name").value;
            if (match(TT::PUNCTUATION, "("))
                n->callArgs = parseCallArgs();
            left = std::move(n);
        }
        // run
        else if (tok.type == TT::KEYWORD && tok.value == "run")
        {
            advance();
            auto n = makeNode(Node::Kind::Run, line, col);
            n->value = statement();
            left = std::move(n);
        }
        else
        {
            error("Unexpected token '" + tok.value + "'");
        }

        // ── postfix / call / property chain ──────────────────────────────────────
        while (!atEnd())
        {
            auto &t = peek();
            if (t.type == TT::PUNCTUATION && (t.value == ";" || t.value == "}"))
                break;
            if (t.type == TT::PUNCTUATION && t.value == "{")
                break;

            // inline if ternary:  expr if cond [else alt]
            if (t.type == TT::KEYWORD && t.value == "if")
            {
                advance();
                auto n = makeNode(Node::Kind::IfTernary, t.line, t.column);
                n->cond = expression();
                if (match(TT::KEYWORD, "else"))
                    n->alternate = expression();
                n->operand = std::move(left);
                left = std::move(n);
                continue;
            }
            // postfix ++ --
            if (t.type == TT::OPERATOR && t.isPostfix)
            {
                auto n = makeNode(Node::Kind::Postfix, t.line, t.column);
                n->op = advance().value;
                n->operand = std::move(left);
                left = std::move(n);
                continue;
            }
            // optional chaining ?.
            if (t.type == TT::OPERATOR && t.value == "?.")
            {
                advance();
                if (match(TT::PUNCTUATION, "("))
                {
                    auto n = makeNode(Node::Kind::OptionalCall, t.line, t.column);
                    n->callee = std::move(left);
                    n->callArgs = parseCallArgs();
                    left = std::move(n);
                }
                else if (match(TT::PUNCTUATION, "["))
                {
                    auto n = makeNode(Node::Kind::OptionalSubscript, t.line, t.column);
                    n->left = std::move(left);
                    n->index = expression();
                    consume(TT::PUNCTUATION, "]", "Expected ]");
                    left = std::move(n);
                }
                else
                {
                    auto n = makeNode(Node::Kind::OptionalProp, t.line, t.column);
                    n->left = std::move(left);
                    n->name = consumeName("Expected property name after ?.").value;
                    left = std::move(n);
                }
                continue;
            }
            // as cast
            if (t.type == TT::KEYWORD && t.value == "as")
            {
                advance();
                auto n = makeNode(Node::Kind::Cast, t.line, t.column);
                n->left = std::move(left);
                n->typeDef = parseTypeExpr();
                left = std::move(n);
                continue;
            }
            // function call
            if (t.type == TT::PUNCTUATION && t.value == "(")
            {
                advance();
                auto n = makeNode(Node::Kind::Call, t.line, t.column);
                n->callee = std::move(left);
                n->callArgs = parseCallArgs();
                // struct call: Name(...).{ field: val, ... }
                // A `. {` immediately following the call's closing `)` is treated
                // as a struct literal supplying the call's field map, rather
                // than the start of a new (unrelated) block statement.
                if (check(TT::PUNCTUATION, ".") && peek(1).type == TT::PUNCTUATION && peek(1).value == "{")
                {
                    advance();
                    auto sc = makeNode(Node::Kind::StructCall, t.line, t.column);
                    sc->callee = std::move(n->callee);
                    sc->callArgs = std::move(n->callArgs);
                    sc->value = objectLiteral();
                    left = std::move(sc);
                }
                else
                {
                    left = std::move(n);
                }
                continue;
            }
            // property access
            if (t.type == TT::PUNCTUATION && t.value == ".")
            {
                advance();
                if (match(TT::PUNCTUATION, "("))
                {
                    // .( expr ) — dynamic prop call
                    auto idx = makeNode(Node::Kind::Value, t.line, t.column);
                    idx->strval = ".expr";
                    auto sub = makeNode(Node::Kind::Subscript, t.line, t.column);
                    sub->left = std::move(left);
                    sub->index = std::move(idx);
                    auto n = makeNode(Node::Kind::Call, t.line, t.column);
                    n->callee = std::move(sub);
                    n->callArgs = parseCallArgs();
                    left = std::move(n);
                }
                else
                {
                    auto n = makeNode(Node::Kind::Prop, t.line, t.column);
                    n->left = std::move(left);
                    n->name = consumeName("Expected property name after .").value;
                    left = std::move(n);
                }
                continue;
            }
            // subscript
            if (t.type == TT::PUNCTUATION && t.value == "[")
            {
                advance();
                auto n = makeNode(Node::Kind::Subscript, t.line, t.column);
                n->left = std::move(left);
                n->index = expression();
                consume(TT::PUNCTUATION, "]", "Expected ]");
                left = std::move(n);
                continue;
            }
            break;
        }

        // ── binary operators ──────────────────────────────────────────────────────
        while (!atEnd())
        {
            auto &t = peek();
            // ternary ?
            if (t.type == TT::PUNCTUATION && t.value == "?" && minPrec <= 0)
            {
                advance();
                auto n = makeNode(Node::Kind::Ternary, t.line, t.column);
                n->cond = std::move(left);
                n->consequent = expression(0);
                consume(TT::PUNCTUATION, ":", "Expected : in ternary");
                n->alternate = expression(0);
                left = std::move(n);
                continue;
            }
            if (t.type != TT::OPERATOR)
                break;
            static const std::vector<std::string> ASSIGN_OPS =
                {"=", "+=", "-=", "*=", "/=", "%=", "**=", "&&=", "||=", "?\?="};
            if (std::find(ASSIGN_OPS.begin(), ASSIGN_OPS.end(), t.value) != ASSIGN_OPS.end())
                break;
            if (t.isPostfix)
                break;
            int prec = t.precedence;
            if (prec < minPrec)
                break;
            std::string op = advance().value;
            auto n = makeNode(Node::Kind::Binary, t.line, t.column);
            n->op = op;
            n->left = std::move(left);
            n->right = expression(t.rightAssoc ? prec : prec + 1);
            left = std::move(n);
        }

        return left;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Error
    // ════════════════════════════════════════════════════════════════════════════

    void Parser::error(const std::string &msg)
    {
        auto &t = peek();
        SourceLoc loc{t.line, t.column, _filename};
        throw ParseError("Parser", msg, loc, _source);
    }

} // namespace novac