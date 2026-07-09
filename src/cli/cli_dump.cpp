#include "cli.h"
#include "../lex/lexer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
  #include <conio.h>
#else
  #include <termios.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace novac::cli {

// ════════════════════════════════════════════════════════════════════════════
//  JSON string escaping
// ════════════════════════════════════════════════════════════════════════════

static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
//  Token → JSON
// ════════════════════════════════════════════════════════════════════════════

static const char* ttName(TT t) {
    switch (t) {
        case TT::NUMBER:              return "NUMBER";
        case TT::STRING:              return "STRING";
        case TT::STRING_PART:         return "STRING_PART";
        case TT::FSTRING_START:       return "FSTRING_START";
        case TT::FSTRING_END:         return "FSTRING_END";
        case TT::INTERPOLATION_START: return "INTERPOLATION_START";
        case TT::INTERPOLATION_END:   return "INTERPOLATION_END";
        case TT::LITERAL:             return "LITERAL";
        case TT::URL:                 return "URL";
        case TT::IDENTIFIER:          return "IDENTIFIER";
        case TT::KEYWORD:             return "KEYWORD";
        case TT::DVAR:                return "DVAR";
        case TT::EXEC_COMMENT:        return "EXEC_COMMENT";
        case TT::OPERATOR:            return "OPERATOR";
        case TT::PUNCTUATION:         return "PUNCTUATION";
        case TT::EOF_TOKEN:           return "EOF";
        default:                      return "UNKNOWN";
    }
}

static void printToken(const Token& t, int indent) {
    std::string pad(indent, ' ');
    std::cout << pad << "{\n";
    std::cout << pad << "  \"type\": \"" << ttName(t.type) << "\",\n";
    if (t.type == TT::NUMBER)
        std::cout << pad << "  \"value\": " << t.numval << ",\n";
    else
        std::cout << pad << "  \"value\": \"" << jsonEscape(t.value) << "\",\n";
    std::cout << pad << "  \"line\": " << t.line << ",\n";
    std::cout << pad << "  \"column\": " << t.column;
    if (t.type == TT::OPERATOR) {
        std::cout << ",\n" << pad << "  \"precedence\": " << t.precedence << ",\n";
        std::cout << pad << "  \"isUnary\": " << (t.isUnary ? "true" : "false") << ",\n";
        std::cout << pad << "  \"isPostfix\": " << (t.isPostfix ? "true" : "false") << ",\n";
        std::cout << pad << "  \"rightAssoc\": " << (t.rightAssoc ? "true" : "false");
    }
    if (t.type == TT::DVAR) {
        std::cout << ",\n" << pad << "  \"srcLine\": " << t.srcLine << ",\n";
        std::cout << pad << "  \"srcCol\": " << t.srcCol;
    }
    std::cout << "\n" << pad << "}";
}

void dumpTokensJSON(const std::string& src, const std::string& fname, bool multi) {
    if (multi) std::cout << bold("\n// ── " + fname + " ──") << "\n";
    try {
        Lexer lex(src, fname);
        auto tokens = lex.tokenize();
        std::cout << "[\n";
        for (size_t i = 0; i < tokens.size(); i++) {
            printToken(tokens[i], 2);
            if (i + 1 < tokens.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "]\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::exit(1);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  AST → JSON
// ════════════════════════════════════════════════════════════════════════════

static const char* kindName(Node::Kind k) {
    switch (k) {
        case Node::Kind::Program:        return "program";
        case Node::Kind::Declare:        return "declare";
        case Node::Kind::Function:       return "function";
        case Node::Kind::Class:          return "class";
        case Node::Kind::Branch:         return "branch";
        case Node::Kind::ForOf:          return "for_of";
        case Node::Kind::ForIn:          return "for_in";
        case Node::Kind::Each:           return "each";
        case Node::Kind::Switch:         return "switch";
        case Node::Kind::Match:          return "match";
        case Node::Kind::Try:            return "try";
        case Node::Kind::Return:         return "return";
        case Node::Kind::Break:          return "break";
        case Node::Kind::Continue:       return "continue";
        case Node::Kind::Goback:         return "goback";
        case Node::Kind::Yield:          return "yield";
        case Node::Kind::Exec:           return "exec";
        case Node::Kind::EmitEvent:      return "emit";
        case Node::Kind::OnEvent:        return "on";
        case Node::Kind::Import:         return "import";
        case Node::Kind::ImportBuiltin:  return "import_builtin";
        case Node::Kind::ImportKit:      return "import_kit";
        case Node::Kind::FromImport:     return "from_import";
        case Node::Kind::Export:         return "export";
        case Node::Kind::DefaultExport:  return "default_export";
        case Node::Kind::Namespace:      return "namespace";
        case Node::Kind::Server:         return "server";
        case Node::Kind::TypeDecl:       return "type_decl";
        case Node::Kind::StructDecl:     return "struct_decl";
        case Node::Kind::EnumDecl:       return "enum_decl";
        case Node::Kind::InterfaceDecl:  return "interface_decl";
        case Node::Kind::TraitDecl:      return "trait_decl";
        case Node::Kind::ImplDecl:       return "impl_decl";
        case Node::Kind::Block:          return "block";
        case Node::Kind::Value:          return "value";
        case Node::Kind::Ref:            return "ref";
        case Node::Kind::DVar:           return "dvar";
        case Node::Kind::Array:          return "array";
        case Node::Kind::Object:         return "object";
        case Node::Kind::FString:        return "fstring";
        case Node::Kind::Regex:          return "regex";
        case Node::Kind::UrlLiteral:     return "url";
        case Node::Kind::Unary:          return "unary";
        case Node::Kind::Prefix:         return "prefix";
        case Node::Kind::Postfix:        return "postfix";
        case Node::Kind::Binary:         return "binary";
        case Node::Kind::Ternary:        return "ternary";
        case Node::Kind::IfTernary:      return "if_ternary";
        case Node::Kind::Assign:         return "assign";
        case Node::Kind::CompoundAssign: return "compound_assign";
        case Node::Kind::Call:           return "call";
        case Node::Kind::OptionalCall:   return "optional_call";
        case Node::Kind::Prop:           return "prop";
        case Node::Kind::OptionalProp:   return "optional_prop";
        case Node::Kind::Subscript:      return "subscript";
        case Node::Kind::OptionalSubscript: return "optional_subscript";
        case Node::Kind::Spread:         return "spread";
        case Node::Kind::ArrowFunc:      return "arrow";
        case Node::Kind::Await:          return "await";
        case Node::Kind::New:            return "new";
        case Node::Kind::Cast:           return "cast";
        case Node::Kind::Deref:          return "deref";
        case Node::Kind::Link:           return "link";
        case Node::Kind::RateCast:       return "rate_cast";
        case Node::Kind::Partial:        return "partial";
        case Node::Kind::Native:         return "native";
        case Node::Kind::Unative:        return "unative";
        case Node::Kind::Throw:          return "throw";
        case Node::Kind::Perform:        return "perform";
        case Node::Kind::Run:            return "run";
        case Node::Kind::WithCtx:        return "with";
        case Node::Kind::WithOption:     return "with_option";
        case Node::Kind::HttpRequest:    return "http_request";
        case Node::Kind::FetchExpr:      return "fetch";
        case Node::Kind::FetchStmt:      return "fetch_stmt";
        case Node::Kind::DotCmd:         return "dot_cmd";
        case Node::Kind::ExecComment:    return "exec_comment";
        case Node::Kind::HasOpt:         return "has_opt";
        case Node::Kind::GetOpt:         return "get_opt";
        case Node::Kind::OptDecl:        return "opt_decl";
        default:                         return "unknown";
    }
}

static void printNodeJSON(const Node* n, int indent);

static void printNodeListJSON(const NodeList& list, const std::string& key, int indent, bool& first) {
    if (list.empty()) return;
    std::string pad(indent, ' ');
    if (!first) std::cout << ",\n";
    first = false;
    std::cout << pad << "\"" << key << "\": [\n";
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i]) printNodeJSON(list[i].get(), indent + 2);
        else std::cout << std::string(indent+2,' ') << "null";
        if (i + 1 < list.size()) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << pad << "]";
}

static void printChildJSON(const NodePtr& child, const std::string& key, int indent, bool& first) {
    if (!child) return;
    std::string pad(indent, ' ');
    if (!first) std::cout << ",\n";
    first = false;
    std::cout << pad << "\"" << key << "\": ";
    printNodeJSON(child.get(), indent);
}

static void printNodeJSON(const Node* n, int indent) {
    std::string pad(indent, ' ');
    std::string pad2(indent + 2, ' ');
    if (!n) { std::cout << "null"; return; }

    std::cout << "{\n";
    bool first = true;

    auto field = [&](const std::string& key, const std::string& val, bool quote=true) {
        if (!first) std::cout << ",\n";
        first = false;
        std::cout << pad2 << "\"" << key << "\": ";
        if (quote) std::cout << "\"" << jsonEscape(val) << "\"";
        else       std::cout << val;
    };

    field("kind", kindName(n->kind));
    field("line", std::to_string(n->line), false);
    field("column", std::to_string(n->column), false);

    if (!n->name.empty())   field("name", n->name);
    if (!n->op.empty())     field("op", n->op);
    if (n->kind == Node::Kind::Value) {
        if (!n->strval.empty()) field("value", n->strval);
        if (n->numval != 0.0)   field("numval", std::to_string(n->numval), false);
    }
    if (!n->strval.empty() && n->kind != Node::Kind::Value) field("strval", n->strval);
    if (n->isConst)    field("isConst", "true", false);
    if (n->isAsync)    field("isAsync", "true", false);
    if (n->isGenerator)field("isGenerator", "true", false);
    if (n->terminate)  field("terminate", "true", false);
    if (!n->branchType.empty()) field("branchType", n->branchType);
    if (!n->varName.empty())    field("varName", n->varName);
    if (!n->indexName.empty())  field("indexName", n->indexName);
    if (!n->method.empty())     field("method", n->method);
    if (!n->catchName.empty())  field("catchName", n->catchName);
    if (!n->flag.empty())       field("flag", n->flag);
    if (!n->kitName.empty())    field("kitName", n->kitName);

    printChildJSON(n->left,       "left",       indent+2, first);
    printChildJSON(n->right,      "right",      indent+2, first);
    printChildJSON(n->cond,       "cond",       indent+2, first);
    printChildJSON(n->consequent, "consequent", indent+2, first);
    printChildJSON(n->alternate,  "alternate",  indent+2, first);
    printChildJSON(n->value,      "value",      indent+2, first);
    printChildJSON(n->callee,     "callee",     indent+2, first);
    printChildJSON(n->operand,    "operand",    indent+2, first);
    printChildJSON(n->next,       "next",       indent+2, first);
    printChildJSON(n->init,       "init",       indent+2, first);
    printChildJSON(n->update,     "update",     indent+2, first);
    printChildJSON(n->iterable,   "iterable",   indent+2, first);
    printChildJSON(n->object,     "object",     indent+2, first);
    printChildJSON(n->subject,    "subject",    indent+2, first);
    printChildJSON(n->index,      "index",      indent+2, first);
    printChildJSON(n->superClass, "superClass", indent+2, first);
    printChildJSON(n->source,     "source",     indent+2, first);
    printChildJSON(n->event,      "event",      indent+2, first);
    printChildJSON(n->target,     "target",     indent+2, first);
    printChildJSON(n->url,        "url",        indent+2, first);
    printChildJSON(n->options,    "options",    indent+2, first);
    printChildJSON(n->port,       "port",       indent+2, first);
    printChildJSON(n->cmd,        "cmd",        indent+2, first);

    printNodeListJSON(n->body,     "body",     indent+2, first);
    printNodeListJSON(n->args,     "args",     indent+2, first);
    printNodeListJSON(n->elements, "elements", indent+2, first);
    printNodeListJSON(n->parts,    "parts",    indent+2, first);
    printNodeListJSON(n->callArgs, "callArgs", indent+2, first);
    printNodeListJSON(n->decorators, "decorators", indent+2, first);
    printNodeListJSON(n->tryBody,    "tryBody",    indent+2, first);
    printNodeListJSON(n->catchBody,  "catchBody",  indent+2, first);
    printNodeListJSON(n->elseBody,   "elseBody",   indent+2, first);
    printNodeListJSON(n->finallyBody,"finallyBody",indent+2, first);

    // func args
    if (!n->funcArgs.empty()) {
        if (!first) std::cout << ",\n";
        first = false;
        std::cout << pad2 << "\"funcArgs\": [\n";
        for (size_t i = 0; i < n->funcArgs.size(); i++) {
            auto& fa = n->funcArgs[i];
            std::cout << std::string(indent+4,' ') << "{ \"name\": \"" << jsonEscape(fa.name) << "\"";
            if (fa.rest) std::cout << ", \"rest\": true";
            std::cout << " }";
            if (i+1 < n->funcArgs.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << pad2 << "]";
    }

    // names (arrow func params)
    if (!n->names.empty()) {
        if (!first) std::cout << ",\n";
        first = false;
        std::cout << pad2 << "\"names\": [";
        for (size_t i = 0; i < n->names.size(); i++) {
            std::cout << "\"" << jsonEscape(n->names[i]) << "\"";
            if (i+1 < n->names.size()) std::cout << ", ";
        }
        std::cout << "]";
    }

    std::cout << "\n" << pad << "}";
}

void dumpAstJSON(const std::string& src, const std::string& fname, bool multi) {
    if (multi) std::cout << bold("\n// ── " + fname + " ──") << "\n";
    try {
        Parser p(src, fname);
        auto ast = p.parse();
        printNodeJSON(ast.get(), 0);
        std::cout << "\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::exit(1);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  novac tokens / ast
// ════════════════════════════════════════════════════════════════════════════

int cmdTokens(const std::vector<std::string>& files) {
    auto resolved = expandFileArgs(files);
    if (resolved.empty()) {
        std::cerr << red("No files matched: ");
        for (auto& f : files) std::cerr << f << " ";
        std::cerr << "\n";
        return 1;
    }
    for (auto& f : resolved) {
        std::string src = readFileOrExit(f);
        dumpTokensJSON(src, f, resolved.size() > 1);
    }
    return 0;
}

int cmdAst(const std::vector<std::string>& files) {
    auto resolved = expandFileArgs(files);
    if (resolved.empty()) {
        std::cerr << red("No files matched: ");
        for (auto& f : files) std::cerr << f << " ";
        std::cerr << "\n";
        return 1;
    }
    for (auto& f : resolved) {
        std::string src = readFileOrExit(f);
        dumpAstJSON(src, f, resolved.size() > 1);
    }
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  novac test <files...>
// ════════════════════════════════════════════════════════════════════════════

int cmdTest(const std::vector<std::string>& files) {
    auto resolved = expandFileArgs(files);
    if (resolved.empty()) {
        std::cerr << red("No files matched: ");
        for (auto& f : files) std::cerr << f << " ";
        std::cerr << "\n";
        return 1;
    }

    int ok = 0, fail = 0;
    for (auto& f : resolved) {
        std::string src;
        try {
            std::ifstream in(f, std::ios::binary);
            if (!in.is_open()) throw std::runtime_error("cannot read file");
            src = std::string(std::istreambuf_iterator<char>(in), {});
        } catch (std::exception& e) {
            std::cerr << red("✗") << " " << gray(f) << " " << red(std::string("— cannot read file: ") + e.what()) << "\n";
            fail++; continue;
        }
        try {
            Parser p(src, f);
            p.parse();
            std::cout << green("✓") << " " << gray(f) << "\n";
            ok++;
        } catch (const std::exception& e) {
            std::cerr << red("✗") << " " << gray(f) << " " << red(std::string("— ") + e.what()) << "\n";
            fail++;
        }
    }
    if (resolved.size() > 1)
        std::cout << blue("\n" + std::to_string(ok) + " passed, " + std::to_string(fail) + " failed") << "\n";

    return fail > 0 ? 1 : 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  Raw-mode line editor (arrow keys, history navigation) — zero deps
// ════════════════════════════════════════════════════════════════════════════

namespace {

enum class RKey { CHAR, ENTER, BACKSPACE, DEL, LEFT, RIGHT, UP, DOWN, HOME, END, EOF_KEY, CTRL_C, TAB, UNKNOWN };

struct RKeyEvent {
    RKey key;
    char ch = 0;
};

#ifdef _WIN32

static void enableAnsiOnWindows() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

struct RawModeGuard {
    RawModeGuard()  { enableAnsiOnWindows(); }
    ~RawModeGuard() {}
};

static RKeyEvent readKey() {
    int c = _getch();
    if (c == 0 || c == 224) { // extended key prefix
        int c2 = _getch();
        switch (c2) {
            case 72: return {RKey::UP};
            case 80: return {RKey::DOWN};
            case 75: return {RKey::LEFT};
            case 77: return {RKey::RIGHT};
            case 83: return {RKey::DEL};
            case 71: return {RKey::HOME};
            case 79: return {RKey::END};
            default: return {RKey::UNKNOWN};
        }
    }
    if (c == 13) return {RKey::ENTER};
    if (c == 8)  return {RKey::BACKSPACE};
    if (c == 3)  return {RKey::CTRL_C};
    if (c == 9)  return {RKey::TAB};
    if (c == 26) return {RKey::EOF_KEY}; // Ctrl+Z
    return {RKey::CHAR, (char)c};
}

#else

struct RawModeGuard {
    termios orig{};
    bool active = false;
    RawModeGuard() {
        if (tcgetattr(STDIN_FILENO, &orig) == 0) {
            termios raw = orig;
            raw.c_lflag &= ~(ECHO | ICANON);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) active = true;
        }
    }
    ~RawModeGuard() {
        if (active) tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    }
};

static RKeyEvent readKey() {
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return {RKey::EOF_KEY};
    if (c == '\r' || c == '\n') return {RKey::ENTER};
    if (c == 127 || c == 8)     return {RKey::BACKSPACE};
    if (c == 3)  return {RKey::CTRL_C};
    if (c == 4)  return {RKey::EOF_KEY}; // Ctrl+D
    if (c == 9)  return {RKey::TAB};
    if (c == 27) { // ESC sequence
        char s0;
        if (read(STDIN_FILENO, &s0, 1) <= 0) return {RKey::UNKNOWN};
        if (s0 != '[' && s0 != 'O') return {RKey::UNKNOWN};
        char s1;
        if (read(STDIN_FILENO, &s1, 1) <= 0) return {RKey::UNKNOWN};
        switch (s1) {
            case 'A': return {RKey::UP};
            case 'B': return {RKey::DOWN};
            case 'C': return {RKey::RIGHT};
            case 'D': return {RKey::LEFT};
            case 'H': return {RKey::HOME};
            case 'F': return {RKey::END};
            case '3': { char tilde; read(STDIN_FILENO, &tilde, 1); return {RKey::DEL}; }
            default:  return {RKey::UNKNOWN};
        }
    }
    return {RKey::CHAR, c};
}

#endif

static void redrawLine(const std::string& prompt, const std::string& buffer, size_t cursor) {
    std::cout << "\r\x1b[K" << prompt << buffer;
    size_t back = buffer.size() - cursor;
    if (back > 0) std::cout << "\x1b[" << back << "D";
    std::cout.flush();
}

// Reads one line with arrow-key editing + history navigation.
// Returns false only on EOF with an empty buffer (i.e. the REPL should exit).
static bool readLineEditor(const std::string& prompt, std::vector<std::string>& history, std::string& outLine) {
    std::string buffer;
    size_t cursor = 0;
    size_t histPos = history.size();
    std::string saved;

    std::cout << prompt;
    std::cout.flush();

    while (true) {
        RKeyEvent ev = readKey();
        switch (ev.key) {
            case RKey::EOF_KEY:
                if (buffer.empty()) { std::cout << "\n"; return false; }
                break; // ignore mid-line EOF, like bash
            case RKey::CTRL_C:
                std::cout << "^C\n";
                buffer.clear(); cursor = 0; histPos = history.size();
                std::cout << prompt;
                std::cout.flush();
                break;
            case RKey::ENTER:
                std::cout << "\n";
                outLine = buffer;
                return true;
            case RKey::BACKSPACE:
                if (cursor > 0) {
                    buffer.erase(cursor - 1, 1);
                    cursor--;
                    redrawLine(prompt, buffer, cursor);
                }
                break;
            case RKey::DEL:
                if (cursor < buffer.size()) {
                    buffer.erase(cursor, 1);
                    redrawLine(prompt, buffer, cursor);
                }
                break;
            case RKey::LEFT:
                if (cursor > 0) { cursor--; std::cout << "\x1b[D"; std::cout.flush(); }
                break;
            case RKey::RIGHT:
                if (cursor < buffer.size()) { cursor++; std::cout << "\x1b[C"; std::cout.flush(); }
                break;
            case RKey::HOME:
                cursor = 0;
                redrawLine(prompt, buffer, cursor);
                break;
            case RKey::END:
                cursor = buffer.size();
                redrawLine(prompt, buffer, cursor);
                break;
            case RKey::UP:
                if (!history.empty() && histPos > 0) {
                    if (histPos == history.size()) saved = buffer;
                    histPos--;
                    buffer = history[histPos];
                    cursor = buffer.size();
                    redrawLine(prompt, buffer, cursor);
                }
                break;
            case RKey::DOWN:
                if (histPos < history.size()) {
                    histPos++;
                    buffer = (histPos == history.size()) ? saved : history[histPos];
                    cursor = buffer.size();
                    redrawLine(prompt, buffer, cursor);
                }
                break;
            case RKey::TAB:
                break; // reserved for future completion
            case RKey::CHAR:
                buffer.insert(cursor, 1, ev.ch);
                cursor++;
                redrawLine(prompt, buffer, cursor);
                break;
            default:
                break;
        }
    }
}

static fs::path historyFilePath() {
    std::string home;
    if (const char* h = std::getenv("HOME")) home = h;
    else if (const char* h = std::getenv("USERPROFILE")) home = h;
    return home.empty() ? fs::path(".nvreplhistory") : fs::path(home) / ".nvreplhistory";
}

static std::vector<std::string> loadHistory(const fs::path& file) {
    std::vector<std::string> h;
    std::ifstream hf(file);
    std::string l;
    while (std::getline(hf, l)) {
        if (!l.empty()) h.push_back(l);
    }
    if (h.size() > 1000) h.erase(h.begin(), h.end() - 1000);
    return h;
}

// Adds an entry to history (in-memory + persisted). Multi-line entries are
// flattened to a single line so the history file stays one-entry-per-line
// and recall via Up/Down always lands in a clean, re-runnable single line.
static void addHistory(std::vector<std::string>& history, const fs::path& file, std::string entry) {
    for (auto& c : entry) if (c == '\n' || c == '\r') c = ' ';
    size_t s = entry.find_first_not_of(" \t");
    size_t e = entry.find_last_not_of(" \t");
    entry = (s == std::string::npos) ? "" : entry.substr(s, e - s + 1);
    if (entry.empty()) return;
    if (!history.empty() && history.back() == entry) return; // dedup consecutive
    history.push_back(entry);
    std::ofstream hf(file, std::ios::app);
    if (hf.is_open()) hf << entry << "\n";
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════════════════
//  novac repl
// ════════════════════════════════════════════════════════════════════════════

int cmdRepl() {
    std::cout << bold("Novac REPL") << " " << gray("v0.1.0") << "\n";
    std::cout << "Type " << yellow(".exit") << " to exit, " << yellow(".help") << " for commands\n\n";

    Executor exe("", "<repl>");
    exe.replMode = true;
    auto replScope = exe.globalScope->child(Scope::Kind::Block);

    fs::path historyFile = historyFilePath();
    std::vector<std::string> history = loadHistory(historyFile);

    RawModeGuard rawGuard; // enables raw input for the lifetime of the REPL

    std::string session; // full transcript, used by .save
    int stmtCount = 0;

    std::string multiline;
    int depth = 0;
    bool editMode = false;

    auto countDepth = [](const std::string& line) {
        int d = 0;
        for (char c : line) {
            if (c=='{'||c=='('||c=='[') d++;
            else if (c=='}'||c==')'||c==']') d--;
        }
        return d;
    };

    auto runSource = [&](const std::string& src, bool timeIt) -> void {
        if (src.empty() || std::all_of(src.begin(), src.end(), [](char c){ return std::isspace((unsigned char)c); }))
            return;

        session += src;
        if (!session.empty() && session.back() != '\n') session += "\n";

        try {
            Parser p(src, "<repl>");
            auto ast = p.parse();
            if (!ast || ast->body.empty()) return;

            auto t0 = std::chrono::steady_clock::now();

            Val result = nova_null();
            for (auto& stmt : ast->body) {
                auto r = exe.execute(stmt.get(), replScope);
                result = r.value;
                if (!r.isNormal()) break;
            }

            auto t1 = std::chrono::steady_clock::now();

            if (result && !result.isNull()) {
                std::string out = exe.stringify(result, 2);
                std::cout << gray("= ") << out << "\n";
                session += "= " + out + "\n";
            }

            if (timeIt) {
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                std::ostringstream oss;
                oss.precision(3);
                oss << std::fixed << ms;
                std::cout << gray("  (" + oss.str() + " ms)") << "\n";
                session += "  (" + oss.str() + " ms)\n";
            }

        } catch (ReturnSignal& r) {
            if (r.value && !r.value.isNull()) {
                std::string out = exe.stringify(r.value, 2);
                std::cout << gray("= ") << out << "\n";
                session += "= " + out + "\n";
            }
        } catch (const ThrowSignal& e) {
            std::cout << red("[throw] ");
            std::string out;
            if (e.value) out = e.value.asString();
            std::cout << out << "\n";
            session += "[throw] " + out + "\n";
        } catch (const NovacError& e) {
            std::cout << e.what() << "\n";
            session += std::string(e.what()) + "\n";
        } catch (const std::exception& e) {
            std::string msg = std::string("Error: ") + e.what();
            std::cout << red(msg) << "\n";
            session += msg + "\n";
        }
    };

    while (true) {
        std::string prompt = editMode
            ? clr("36", "edit> ")
            : (multiline.empty()
                ? clr("35", "nova:" + std::to_string(stmtCount + 1) + "> ")
                : clr("90", "...   "));

        std::string line;
        if (!readLineEditor(prompt, history, line)) break; // EOF on empty line

        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        size_t e = trimmed.find_last_not_of(" \t\r\n");
        trimmed = (e == std::string::npos) ? "" : trimmed.substr(0, e+1);

        // ── .edit mode: swallow everything until .editend / .editcancel ──
        if (editMode) {
            if (trimmed == ".editend") {
                editMode = false;
                std::string src = multiline;
                multiline.clear();
                if (!src.empty()) {
                    addHistory(history, historyFile, src);
                    stmtCount++;
                    runSource(src, false);
                }
                continue;
            }
            if (trimmed == ".editcancel") {
                editMode = false;
                multiline.clear();
                std::cout << yellow("Edit cancelled.") << "\n";
                continue;
            }
            multiline += (multiline.empty() ? "" : "\n") + line;
            continue;
        }

        if (trimmed == ".exit") break;
        if (trimmed == ".clear") { std::cout << "\x1b[2J\x1b[H"; multiline.clear(); depth = 0; continue; }
        if (trimmed == ".reset") {
            exe = Executor("", "<repl>");
            exe.replMode = true;
            replScope = exe.globalScope->child(Scope::Kind::Block);
            stmtCount = 0;
            session.clear();
            std::cout << green("REPL state reset.") << "\n";
            continue;
        }
        if (trimmed == ".edit") {
            addHistory(history, historyFile, trimmed);
            editMode = true;
            multiline.clear();
            std::cout << gray("Entering edit mode. Type ") << yellow(".editend")
                      << gray(" to run, ") << yellow(".editcancel") << gray(" to abort.") << "\n";
            continue;
        }
        if (trimmed.rfind(".time", 0) == 0) {
            addHistory(history, historyFile, trimmed);
            std::string code = trimmed.substr(5);
            code.erase(0, code.find_first_not_of(" \t"));
            if (code.empty()) { std::cerr << "Usage: .time <code>\n"; continue; }
            stmtCount++;
            runSource(code, true);
            continue;
        }
        if (trimmed.rfind(".history", 0) == 0) {
            std::string arg = trimmed.substr(8);
            arg.erase(0, arg.find_first_not_of(" \t"));
            int n = 20;
            if (!arg.empty()) { try { n = std::max(1, std::stoi(arg)); } catch (...) {} }
            size_t start = history.size() > (size_t)n ? history.size() - (size_t)n : 0;
            for (size_t i = start; i < history.size(); i++)
                std::cout << gray(std::to_string(i - start + 1) + ": ") << history[i] << "\n";
            continue;
        }
        if (trimmed.rfind(".save", 0) == 0) {
            std::string path = trimmed.substr(5);
            path.erase(0, path.find_first_not_of(" \t"));
            if (path.empty()) { std::cerr << "Usage: .save <file>\n"; continue; }
            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) {
                std::cerr << red("Could not open " + path + " for writing") << "\n";
            } else {
                out << session;
                std::cout << green("Session saved to ") << gray(path) << "\n";
            }
            continue;
        }
        if (trimmed.rfind(".ast", 0) == 0) {
            addHistory(history, historyFile, trimmed);
            std::string code = trimmed.substr(4);
            code.erase(0, code.find_first_not_of(" \t"));
            if (!code.empty()) dumpAstJSON(code, "<repl>", false);
            continue;
        }
        if (trimmed.rfind(".tokens", 0) == 0) {
            addHistory(history, historyFile, trimmed);
            std::string code = trimmed.substr(7);
            code.erase(0, code.find_first_not_of(" \t"));
            if (!code.empty()) dumpTokensJSON(code, "<repl>", false);
            continue;
        }
        if (trimmed.rfind(".load", 0) == 0) {
            addHistory(history, historyFile, trimmed);
            std::string pattern = trimmed.substr(5);
            pattern.erase(0, pattern.find_first_not_of(" \t"));
            if (pattern.empty()) { std::cerr << "Usage: .load <file|glob>\n"; continue; }
            auto files = expandFileArgs({pattern});
            if (files.empty()) { std::cerr << red("No files matched: " + pattern) << "\n"; continue; }
            for (auto& f : files) {
                try {
                    std::string src = readFileOrExit(f);
                    Parser p(src, f);
                    auto ast = p.parse();
                    exe.run(ast.get(), replScope);
                    std::cout << green("loaded") << " " << gray(f) << "\n";
                } catch (std::exception& e) {
                    std::cerr << red("Error loading " + f + ": ") << e.what() << "\n";
                }
            }
            continue;
        }
        if (trimmed == ".help") {
            std::cout << "Nova REPL Commands:\n"
                      << "  .exit               Exit REPL\n"
                      << "  .clear              Clear screen\n"
                      << "  .reset              Reset all variables and definitions\n"
                      << "  .edit               Enter multi-line edit mode (end with .editend, abort with .editcancel)\n"
                      << "  .time <code>        Run code and print elapsed time\n"
                      << "  .history [n]        Show last n history entries (default 20)\n"
                      << "  .save <file>        Save the full session transcript to a file\n"
                      << "  .ast <code>         Show AST for code\n"
                      << "  .tokens <code>      Show token stream for code\n"
                      << "  .load <file|glob>   Load Nova file(s) into REPL\n"
                      << "  .help               Show this help\n"
                      << "\nArrow keys: Left/Right move cursor, Up/Down browse history.\n";
            continue;
        }

        multiline += (multiline.empty() ? "" : "\n") + line;
        depth += countDepth(line);
        if (depth > 0) continue;
        depth = 0;

        std::string src = multiline;
        multiline.clear();
        addHistory(history, historyFile, src);
        stmtCount++;
        runSource(src, false);
    }

    std::cout << gray("Goodbye!") << "\n";
    return 0;
}

} // namespace novac::cli