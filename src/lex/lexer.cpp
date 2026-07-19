#include "lexer.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace novac
{

    namespace fs = std::filesystem;

    // ── escapeSequences() defined here (not in header) to avoid SIOF ────────────
    const std::unordered_map<std::string, std::string> &escapeSequences()
    {
        static const std::unordered_map<std::string, std::string> TABLE = {
            // classic single-char
            {"n", "\n"},
            {"t", "\t"},
            {"r", "\r"},
            {"\\", "\\"},
            {"'", "'"},
            {"\"", "\""},
            {"0", std::string(1, '\0')},
            {"a", "\x07"},
            {"b", "\x08"},
            {"v", "\x0B"},
            {"f", "\x0C"},
            {"e", "\x1b"},
            // ANSI reset / clear
            {"reset", "\x1b[0m"},
            {"clr", "\x1b[0m"},
            // text styles
            {"bold", "\x1b[1m"},
            {"dim", "\x1b[2m"},
            {"italic", "\x1b[3m"},
            {"underline", "\x1b[4m"},
            {"blink", "\x1b[5m"},
            {"reverse", "\x1b[7m"},
            {"hidden", "\x1b[8m"},
            {"strike", "\x1b[9m"},
            {"overline", "\x1b[53m"},
            {"resetbold", "\x1b[22m"},
            {"resetdim", "\x1b[22m"},
            {"resetitalic", "\x1b[23m"},
            {"resetul", "\x1b[24m"},
            {"resetblink", "\x1b[25m"},
            {"resetrev", "\x1b[27m"},
            // cursor movement
            {"home", "\x1b[H"},
            {"cls", "\x1b[2J"},
            {"clearline", "\x1b[2K"},
            {"clearright", "\x1b[0K"},
            {"clearleft", "\x1b[1K"},
            {"cleardown", "\x1b[0J"},
            {"clearup", "\x1b[1J"},
            {"savecursor", "\x1b[s"},
            {"restorecursor", "\x1b[u"},
            {"hidecursor", "\x1b[?25l"},
            {"showcursor", "\x1b[?25h"},
            {"cursorup", "\x1b[1A"},
            {"cursordown", "\x1b[1B"},
            {"cursorright", "\x1b[1C"},
            {"cursorleft", "\x1b[1D"},
            {"cursorbol", "\x1b[0G"},
            // foreground color shortcuts
            {"black", "\x1b[30m"},
            {"red", "\x1b[31m"},
            {"green", "\x1b[32m"},
            {"yellow", "\x1b[33m"},
            {"blue", "\x1b[34m"},
            {"magenta", "\x1b[35m"},
            {"cyan", "\x1b[36m"},
            {"white", "\x1b[37m"},
            // background shortcuts
            {"bgblack", "\x1b[40m"},
            {"bgred", "\x1b[41m"},
            {"bggreen", "\x1b[42m"},
            {"bgyellow", "\x1b[43m"},
            {"bgblue", "\x1b[44m"},
            {"bgmagenta", "\x1b[45m"},
            {"bgcyan", "\x1b[46m"},
            {"bgwhite", "\x1b[47m"},
            // misc chars
            {"bell", "\x07"},
            {"tab", "\t"},
            {"cr", "\r"},
            {"lf", "\n"},
            {"crlf", "\r\n"},
            {"null", std::string(1, '\0')},
            {"esc", "\x1b"},
            {"space", " "},
            // box-drawing
            {"hline", "\u2500"},
            {"vline", "\u2502"},
            {"tlcorner", "\u250C"},
            {"trcorner", "\u2510"},
            {"blcorner", "\u2514"},
            {"brcorner", "\u2518"},
            {"ttee", "\u252C"},
            {"btee", "\u2534"},
            {"ltee", "\u251C"},
            {"rtee", "\u2524"},
            {"cross", "\u253C"},
            // double box
            {"dhline", "\u2550"},
            {"dvline", "\u2551"},
            {"dtlcorner", "\u2554"},
            {"dtrcorner", "\u2557"},
            {"dblcorner", "\u255A"},
            {"dbrcorner", "\u255D"},
            // arrows
            {"uarr", "\u2191"},
            {"darr", "\u2193"},
            {"larr", "\u2190"},
            {"rarr", "\u2192"},
            {"udarr", "\u2195"},
            {"lrarr", "\u2194"},
            // misc unicode
            {"check", "\u2713"},
            {"cross2", "\u2717"},
            {"bullet", "\u2022"},
            {"ellipsis", "\u2026"},
            {"degree", "\u00B0"},
            {"copyright", "\u00A9"},
            {"registered", "\u00AE"},
            {"trademark", "\u2122"},
            {"section", "\u00A7"},
            {"dagger", "\u2020"},
            {"middot", "\u00B7"},
            {"endash", "\u2013"},
            {"emdash", "\u2014"},
        };
        return TABLE;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Construction
    // ════════════════════════════════════════════════════════════════════════════

    Lexer::Lexer(std::string source, std::string filename)
        : _source(std::move(source)), _filename(std::move(filename))
    {

        // ── Operating systems ────────────────────────────────────────────
#if defined(__linux__)
        _definitions["_LINUX"] = "1";
#endif
#if defined(__ANDROID__)
        _definitions["_ANDROID"] = "1";
#endif
#if defined(__APPLE__)
        _definitions["_APPLE"] = "1";
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
        _definitions["_IOS"] = "1";
#elif TARGET_OS_MAC
        _definitions["_MACOS"] = "1";
#endif
#endif
#if defined(_WIN32)
        _definitions["_WIN32"] = "1";
#endif
#if defined(_WIN64)
        _definitions["_WIN64"] = "1";
#endif
#if defined(__CYGWIN__)
        _definitions["_CYGWIN"] = "1";
#endif
#if defined(__FreeBSD__)
        _definitions["_FREEBSD"] = "1";
#endif
#if defined(__OpenBSD__)
        _definitions["_OPENBSD"] = "1";
#endif
#if defined(__NetBSD__)
        _definitions["_NETBSD"] = "1";
#endif
#if defined(__unix__) || defined(__unix)
        _definitions["_UNIX"] = "1";
#endif
#if defined(__sun)
        _definitions["_SOLARIS"] = "1";
#endif
#if defined(_AIX)
        _definitions["_AIX"] = "1";
#endif
#if defined(__HAIKU__)
        _definitions["_HAIKU"] = "1";
#endif
        // ── Architecture ─────────────────────────────────────────────────
#if defined(__x86_64__) || defined(_M_X64)
        _definitions["_x86_64"] = "1";
#endif
#if defined(__i386__) || defined(_M_IX86)
        _definitions["_x86_32"] = "1";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
        _definitions["_ARM64"] = "1";
#endif
#if defined(__arm__) || defined(_M_ARM)
        _definitions["_ARM"] = "1";
#endif
#if defined(__powerpc64__)
        _definitions["_PPC64"] = "1";
#elif defined(__powerpc__)
        _definitions["_PPC"] = "1";
#endif
#if defined(__riscv)
        _definitions["_RISCV"] = "1";
#endif
#if defined(__mips__)
        _definitions["_MIPS"] = "1";
#endif
#if defined(__sparc__)
        _definitions["_SPARC"] = "1";
#endif

        // ── Pointer width / endianness ──────────────────────────────────
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFULL
        _definitions["_64BIT"] = "1";
#elif UINTPTR_MAX == 0xFFFFFFFF
        _definitions["_32BIT"] = "1";
#endif
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        _definitions["_LITTLE_ENDIAN"] = "1";
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        _definitions["_BIG_ENDIAN"] = "1";
#endif
    }

    void Lexer::setDefinitions(const std::unordered_map<std::string, std::string> &defs)
    {
        _definitions = defs;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Character helpers
    // ════════════════════════════════════════════════════════════════════════════

    char Lexer::peek(int offset) const
    {
        size_t idx = _pos + offset;
        return (idx < _source.size()) ? _source[idx] : '\0';
    }

    char Lexer::advance()
    {
        char c = (_pos < _source.size()) ? _source[_pos] : '\0';
        _pos++;
        _col++;
        return c;
    }

    bool Lexer::atEnd() const { return _pos >= _source.size(); }
    bool Lexer::isWS(char c) const { return c == ' ' || c == '\t' || c == '\r'; }
    bool Lexer::isDigit(char c) const { return c >= '0' && c <= '9'; }
    bool Lexer::isAlpha(char c) const
    {
        unsigned char u = (unsigned char)c;
        return std::isalpha(u) || c == '_' || c == '#' ||
               (u >= 0xC3 && u <= 0xC9); // UTF-8 lead bytes for Latin Extended
    }

    bool Lexer::isAlNum(char c) const
    {
        unsigned char u = (unsigned char)c;
        return std::isalnum(u) || c == '_' || c == '#' ||
               (u >= 0xC3 && u <= 0xC9) || // lead bytes
               (u >= 0x80 && u <= 0xBF);   // continuation bytes
    }
    bool Lexer::isHex(char c) const
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Token emitters
    // ════════════════════════════════════════════════════════════════════════════

    void Lexer::addToken(TT type, const std::string &value, int col)
    {
        _tokens.push_back({type, value, 0.0, _line, col < 0 ? _col : col});
    }

    void Lexer::addToken(TT type, const std::string &value, double numval, int col)
    {
        _tokens.push_back({type, value, numval, _line, col < 0 ? _col : col});
    }

    void Lexer::addOpToken(const std::string &op, int col)
    {
        auto &ops = operators();
        auto it = ops.find(op);
        Token t;
        t.type = TT::OPERATOR;
        t.value = op;
        t.line = _line;
        t.column = col;
        if (it != ops.end())
        {
            t.precedence = it->second.precedence;
            t.isUnary = it->second.isUnary;
            t.isPostfix = it->second.isPostfix;
            t.rightAssoc = it->second.rightAssoc;
        }
        _tokens.push_back(t);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Main tokenize loop
    // ════════════════════════════════════════════════════════════════════════════

    std::vector<Token> Lexer::tokenize()
    {
        while (!atEnd())
        {
            char c = peek();

            // preprocessor directive
            if (c == '$')
            {
                if (peek(1) == ':')
                {
                    advance();
                    scanSymbol('$');
                    continue;
                }
                advance();
                handleDirective();
                continue;
            }

            // skip mode (ifdef/ifndef block)
            if (_skip)
            {
                if (c == '\n')
                {
                    _line++;
                    _col = 1;
                }
                else if (c == '\t')
                {
                    _col += 3;
                }
                _pos++;
                continue;
            }

            advance(); // consume c

            // whitespace
            if (isWS(c))
            {
                if (c == '\t')
                    _col += 3;
                continue;
            }
            if (c == '\n')
            {
                _line++;
                _col = 1;
                continue;
            }

            // comments
            if (c == '/')
            {
                char n = peek();
                // /?/ executable comment
                if (n == '?')
                {
                    advance();
                    if (peek() == '/')
                    {
                        advance();
                        std::string expr;
                        while (!atEnd() && peek() != '\n')
                        {
                            char ec = advance();
                            if (ec == '/' && !expr.empty())
                            {
                                // strip trailing space before closing /
                                while (!expr.empty() && isWS(expr.back()))
                                    expr.pop_back();
                                break;
                            }
                            expr += ec;
                        }
                        while (!expr.empty() && isWS(expr.front()))
                            expr.erase(expr.begin());
                        while (!expr.empty() && isWS(expr.back()))
                            expr.pop_back();
                        addToken(TT::EXEC_COMMENT, expr, _col);
                        continue;
                    }
                }
                if (n == '!' || n == '/')
                {
                    advance();
                    skipLineComment();
                    continue;
                }
                if (n == '*')
                {
                    advance();
                    skipBlockComment();
                    continue;
                }
            }

            // strings
            if (c == '"' || c == '\'')
            {
                scanString(c);
                continue;
            }

            // __ident__
            if (c == '_' && peek() == '_')
            {
                advance();
                scanDoubleUnderscore();
                continue;
            }

            // numbers
            if (isDigit(c))
            {
                scanNumber(c);
                continue;
            }

            // identifiers / keywords / f-strings / r-strings / urls
            // Special: #: must be caught as operator before identifier path
            if (c == '#' && peek() == ':')
            {
                scanSymbol(c);
                continue;
            }
            if (isAlpha(c))
            {
                scanIdentifier(c);
                continue;
            }

            // operators / punctuation
            scanSymbol(c);
        }

        addToken(TT::EOF_TOKEN, "", _col);
        return _tokens;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Preprocessor
    // ════════════════════════════════════════════════════════════════════════════

    std::string Lexer::readBareIdent()
    {
        std::string v;
        while (!atEnd() && isAlNum(peek()))
            v += advance();
        return v;
    }

    std::string Lexer::readRestOfLine()
    {
        std::string v;
        while (!atEnd() && peek() != '\n')
            v += advance();
        // trim
        size_t l = v.find_first_not_of(" \t");
        size_t r = v.find_last_not_of(" \t");
        if (l == std::string::npos)
            return "";
        return v.substr(l, r - l + 1);
    }

    void Lexer::skipLine()
    {
        while (!atEnd() && peek() != '\n')
            advance();
        if (!atEnd() && peek() == '\n')
        {
            _line++;
            _col = 1;
            _pos++;
        }
    }

    void Lexer::injectReplacement(const std::string &str)
    {
        _source = str + _source.substr(_pos);
        _pos = 0;
        _col = 1;
    }

    void Lexer::handleDirective()
    {
        // skip leading whitespace on same line
        while (!atEnd() && isWS(peek()))
            advance();
        bool shouldSkipLine = true;

        std::string dirName = readBareIdent();
        while (!atEnd() && isWS(peek()))
            advance();
        std::string dirArg = readBareIdent();

        if (dirName == "define")
        {
            if (!dirArg.empty())
            {
                std::string val = readRestOfLine();
                shouldSkipLine = false;
                if (val.empty())
                    val = "1";
                // bare identifier word → quote it so injection works as a string literal
                // but NOT if it's a number
                else if (std::all_of(val.begin(), val.end(), [](char c)
                                     { return std::isalpha((unsigned char)c) || c == '_'; }))
                {
                    val = "\"" + val + "\"";
                }
                // numeric / operator values are injected raw
                _definitions[dirArg] = val;
            }
        }
        else if (dirName == "undef")
        {
            _definitions.erase(dirArg);
        }
        else if (dirName == "register")
        {
            if (dirArg == "operator")
            {
                std::string name = readBareIdent();
                std::string rest = readRestOfLine();
                shouldSkipLine = false;
                // minimal: just register with precedence parsed as single int
                OpInfo info;
                try
                {
                    info.precedence = std::stoi(rest);
                }
                catch (...)
                {
                }
                info.custom = true;
                operators()[name] = info;
            }
            else if (dirArg == "escape")
            {
                while (!atEnd() && isWS(peek()))
                    advance();
                std::string escapeName = readBareIdent();
                while (!atEnd() && isWS(peek()))
                    advance();
                std::string rest = readRestOfLine();
                shouldSkipLine = false;
                // strip surrounding quotes if present
                if (rest.size() >= 2 &&
                    ((rest.front() == '"' && rest.back() == '"') ||
                     (rest.front() == '\'' && rest.back() == '\'')))
                {
                    rest = rest.substr(1, rest.size() - 2);
                }
                // resolve basic C-style escapes in the literal
                std::string resolved;
                for (size_t i = 0; i < rest.size(); i++)
                {
                    if (rest[i] == '\\' && i + 1 < rest.size())
                    {
                        char n = rest[++i];
                        if (n == 'n')
                            resolved += '\n';
                        else if (n == 't')
                            resolved += '\t';
                        else if (n == 'r')
                            resolved += '\r';
                        else if (n == 'e' || (n == 'x' && i + 2 < rest.size()))
                        {
                            if (n == 'e')
                            {
                                resolved += '\x1b';
                            }
                            else
                            {
                                std::string h = rest.substr(i + 1, 2);
                                i += 2;
                                resolved += (char)std::stoi(h, nullptr, 16);
                            }
                        }
                        else
                            resolved += n;
                    }
                    else if (rest[i] == '"' || rest[i] == '\'')
                    {
                        // strip surrounding quotes
                    }
                    else
                    {
                        resolved += rest[i];
                    }
                }
                CustomEscape ce;
                ce.isHandler = false;
                ce.literal = resolved;
                customEscapes()[escapeName] = ce;
            }
        }
        else if (dirName == "inject")
        {
            std::string rest = readRestOfLine();
            shouldSkipLine = false;
            // raw token injection: value is treated as a STRING token
            addToken(TT::STRING, rest, _col);
        }
        else if (dirName == "ifdef")
        {
            if (!_skip)
                _skip = (_definitions.find(dirArg) == _definitions.end());
        }
        else if (dirName == "ifndef")
        {
            if (!_skip)
                _skip = (_definitions.find(dirArg) != _definitions.end());
        }
        else if (dirName == "endif")
        {
            _skip = false;
        }
        else if (dirName == "io")
        {
            if (dirArg == "log") std::cout << readRestOfLine() << std::endl;
            if (dirArg == "error") std::cerr << readRestOfLine() << std::endl;
            if (dirArg == "ask") { std::cout << readRestOfLine(); std::string in; std::getline(std::cin, in); addToken(TT::STRING, in, _col); }
        }
        else
        {
            addToken(TT::STRING, dirArg, _col);
            shouldSkipLine = false;
        }

        if (shouldSkipLine)
            skipLine();
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Comments
    // ════════════════════════════════════════════════════════════════════════════

    void Lexer::skipLineComment()
    {
        while (!atEnd() && peek() != '\n')
            advance();
    }

    void Lexer::skipBlockComment()
    {
        while (!atEnd())
        {
            char c = advance();
            if (c == '*' && peek() == '/')
            {
                advance();
                return;
            }
            if (c == '\n')
            {
                _line++;
                _col = 1;
            }
        }
        error("Unterminated block comment");
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Escape engine
    // ════════════════════════════════════════════════════════════════════════════

    std::vector<std::string> Lexer::splitArgs(const std::string &s)
    {
        std::vector<std::string> args;
        std::string cur;
        int depth = 0;
        for (char c : s)
        {
            if (c == '{' || c == '(' || c == '[')
            {
                depth++;
                cur += c;
            }
            else if (c == '}' || c == ')' || c == ']')
            {
                depth--;
                cur += c;
            }
            else if (c == ',' && depth == 0)
            {
                // trim and push
                size_t l = cur.find_first_not_of(" \t");
                size_t r = cur.find_last_not_of(" \t");
                args.push_back(l == std::string::npos ? "" : cur.substr(l, r - l + 1));
                cur.clear();
            }
            else
                cur += c;
        }
        size_t l = cur.find_first_not_of(" \t");
        size_t r = cur.find_last_not_of(" \t");
        if (l != std::string::npos)
            args.push_back(cur.substr(l, r - l + 1));
        return args;
    }

    // Convert a unicode codepoint to a UTF-8 string
    static std::string cpToUtf8(uint32_t cp)
    {
        std::string out;
        if (cp < 0x80)
        {
            out += (char)cp;
        }
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        return out;
    }

    // Try to parse a raw ANSI/VT escape that was written literally in source,
    // e.g. \x1b[33m  \033[1;32m  \e[0m  \27[...X
    // Returns the resolved string if it looks like a valid ANSI sequence,
    // otherwise returns empty string so caller can fall through.
    std::string Lexer::resolveAnsiRaw(const std::string &raw)
    {
        // Accepted prefixes for ESC character
        std::string esc;
        size_t i = 0;

        // Match ESC prefix variants:
        //   x1b  x1B  033  27  e  (already consumed leading \)
        if (raw.size() >= 3 && (raw.substr(0, 3) == "x1b" || raw.substr(0, 3) == "x1B"))
        {
            esc = "\x1b";
            i = 3;
        }
        else if (raw.size() >= 3 && raw.substr(0, 3) == "033")
        {
            esc = "\x1b";
            i = 3;
        }
        else if (raw.size() >= 2 && raw.substr(0, 2) == "27")
        {
            esc = "\x1b";
            i = 2;
        }
        else if (raw.size() >= 1 && raw[0] == 'e')
        {
            esc = "\x1b";
            i = 1;
        }
        else
        {
            return "";
        }

        if (i >= raw.size())
            return esc; // bare ESC

        // After ESC, expect optional [ (CSI), ] (OSC), or other introducer
        std::string tail = raw.substr(i);
        // Just return ESC + tail verbatim — it was written raw by the programmer
        // and we trust they know their terminal.
        return esc + tail;
    }

    std::string Lexer::processEscape(const std::string &name,
                                     const std::vector<std::string> &args)
    {
        // 1. custom registry (overrides builtins)
        {
            auto &reg = customEscapes();
            auto it = reg.find(name);
            if (it != reg.end())
            {
                if (it->second.isHandler)
                    return it->second.handler(args);
                return it->second.literal;
            }
        }

        // 2. argument-taking built-ins
        if (name == "color" || name == "fg")
        {
            std::string raw = args.empty() ? "" : args[0];
            // reassemble if split on rgb/256 inner comma
            std::string key = raw;
            if (args.size() > 1 &&
                (raw.rfind("rgb:", 0) == 0 || raw.rfind("256:", 0) == 0))
            {
                std::string joined;
                for (size_t i = 0; i < args.size(); i++)
                {
                    if (i)
                        joined += ",";
                    joined += args[i];
                }
                // remove spaces
                key.clear();
                for (char c : joined)
                    if (c != ' ')
                        key += std::tolower((unsigned char)c);
            }
            else
            {
                for (char &c : key)
                    c = std::tolower((unsigned char)c);
            }
            auto it = FG.find(key);
            if (it != FG.end())
                return it->second;
            if (key.rfind("256:", 0) == 0)
                return "\x1b[38;5;" + key.substr(4) + "m";
            if (key.rfind("rgb:", 0) == 0)
            {
                std::string rgb = key.substr(4);
                std::string r, g, b;
                size_t p1 = rgb.find(','), p2 = rgb.find(',', p1 + 1);
                if (p1 != std::string::npos && p2 != std::string::npos)
                {
                    r = rgb.substr(0, p1);
                    g = rgb.substr(p1 + 1, p2 - p1 - 1);
                    b = rgb.substr(p2 + 1);
                    return "\x1b[38;2;" + r + ";" + g + ";" + b + "m";
                }
            }
            if (key.size() == 7 && key[0] == '#')
            {
                int r = std::stoi(key.substr(1, 2), nullptr, 16);
                int g = std::stoi(key.substr(3, 2), nullptr, 16);
                int b = std::stoi(key.substr(5, 2), nullptr, 16);
                return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
            }
            return "\x1b[39m";
        }

        if (name == "bg")
        {
            std::string key = args.empty() ? "" : args[0];
            for (char &c : key)
                c = std::tolower((unsigned char)c);
            // reassemble rgb/256
            if (args.size() > 1 &&
                (key.rfind("rgb:", 0) == 0 || key.rfind("256:", 0) == 0))
            {
                std::string joined;
                for (size_t i = 0; i < args.size(); i++)
                {
                    if (i)
                        joined += ",";
                    joined += args[i];
                }
                key.clear();
                for (char c : joined)
                    if (c != ' ')
                        key += std::tolower((unsigned char)c);
            }
            auto it = BG.find(key);
            if (it != BG.end())
                return it->second;
            if (key.rfind("256:", 0) == 0)
                return "\x1b[48;5;" + key.substr(4) + "m";
            if (key.rfind("rgb:", 0) == 0)
            {
                std::string rgb = key.substr(4);
                size_t p1 = rgb.find(','), p2 = rgb.find(',', p1 + 1);
                if (p1 != std::string::npos && p2 != std::string::npos)
                {
                    std::string r = rgb.substr(0, p1), g = rgb.substr(p1 + 1, p2 - p1 - 1), b = rgb.substr(p2 + 1);
                    return "\x1b[48;2;" + r + ";" + g + ";" + b + "m";
                }
            }
            if (key.size() == 7 && key[0] == '#')
            {
                int r = std::stoi(key.substr(1, 2), nullptr, 16);
                int g = std::stoi(key.substr(3, 2), nullptr, 16);
                int b = std::stoi(key.substr(5, 2), nullptr, 16);
                return "\x1b[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
            }
            return "\x1b[49m";
        }

        if (name == "textType")
        {
            std::string key = args.empty() ? "" : args[0];
            for (char &c : key)
                c = std::tolower((unsigned char)c);
            auto it = TEXT_TYPES.find(key);
            if (it != TEXT_TYPES.end())
                return it->second;
            // raw SGR code
            bool allDigit = !key.empty();
            for (char c : key)
                if (!std::isdigit((unsigned char)c))
                {
                    allDigit = false;
                    break;
                }
            if (allDigit)
                return "\x1b[" + key + "m";
            return "";
        }

        if (name == "sgr")
        {
            std::string out;
            for (auto &a : args)
            {
                std::string t = a;
                while (!t.empty() && isWS(t.front()))
                    t.erase(t.begin());
                while (!t.empty() && isWS(t.back()))
                    t.pop_back();
                out += "\x1b[" + t + "m";
            }
            return out;
        }

        if (name == "cursor")
        {
            std::string dir = args.empty() ? "" : args[0];
            for (char &c : dir)
                c = std::tolower((unsigned char)c);
            int n = args.size() >= 2 ? std::atoi(args[1].c_str()) : 1;
            if (n < 1)
                n = 1;
            static const std::unordered_map<std::string, char> codes = {
                {"up", 'A'}, {"down", 'B'}, {"right", 'C'}, {"left", 'D'}, {"nextline", 'E'}, {"prevline", 'F'}, {"col", 'G'}, {"row", 'd'}};
            auto it = codes.find(dir);
            if (it != codes.end())
                return "\x1b[" + std::to_string(n) + it->second;
            return "";
        }

        if (name == "pos")
        {
            int row = args.size() >= 1 ? std::atoi(args[0].c_str()) : 1;
            int col = args.size() >= 2 ? std::atoi(args[1].c_str()) : 1;
            return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
        }

        if (name == "unicode")
        {
            std::string raw = args.empty() ? "" : args[0];
            while (!raw.empty() && isWS(raw.front()))
                raw.erase(raw.begin());
            // strip U+ prefix
            if (raw.size() >= 2 && (raw[0] == 'U' || raw[0] == 'u') && raw[1] == '+')
                raw = raw.substr(2);
            try
            {
                uint32_t cp = (uint32_t)std::stoul(raw, nullptr, 16);
                return cpToUtf8(cp);
            }
            catch (...)
            {
                return "";
            }
        }

        if (name == "hex")
        {
            std::string out;
            for (auto &a : args)
            {
                std::string t = a;
                while (!t.empty() && isWS(t.front()))
                    t.erase(t.begin());
                while (!t.empty() && isWS(t.back()))
                    t.pop_back();
                try
                {
                    out += (char)std::stoi(t, nullptr, 16);
                }
                catch (...)
                {
                }
            }
            return out;
        }

        if (name == "repeat")
        {
            std::string ch = args.empty() ? " " : args[0];
            int n = args.size() >= 2 ? std::atoi(args[1].c_str()) : 1;
            std::string out;
            for (int i = 0; i < n; i++)
                out += ch;
            return out;
        }

        if (name == "pad")
        {
            std::string text = args.empty() ? "" : args[0];
            int width = args.size() >= 2 ? std::atoi(args[1].c_str()) : 0;
            std::string fill = args.size() >= 3 ? args[2] : " ";
            if (fill.empty())
                fill = " ";
            while ((int)text.size() < width)
                text += fill;
            return text.substr(0, width);
        }

        if (name == "padl")
        {
            std::string text = args.empty() ? "" : args[0];
            int width = args.size() >= 2 ? std::atoi(args[1].c_str()) : 0;
            std::string fill = args.size() >= 3 ? args[2] : " ";
            if (fill.empty())
                fill = " ";
            while ((int)text.size() < width)
                text = fill + text;
            if ((int)text.size() > width)
                text = text.substr(text.size() - width);
            return text;
        }

        if (name == "upper")
        {
            std::string s = args.empty() ? "" : args[0];
            for (char &c : s)
                c = std::toupper((unsigned char)c);
            return s;
        }
        if (name == "lower")
        {
            std::string s = args.empty() ? "" : args[0];
            for (char &c : s)
                c = std::tolower((unsigned char)c);
            return s;
        }

        if (name == "256fg")
        {
            int n = args.empty() ? 0 : std::atoi(args[0].c_str());
            return "\x1b[38;5;" + std::to_string(n) + "m";
        }
        if (name == "256bg")
        {
            int n = args.empty() ? 0 : std::atoi(args[0].c_str());
            return "\x1b[48;5;" + std::to_string(n) + "m";
        }
        if (name == "rgbfg")
        {
            int r = args.size() > 0 ? std::atoi(args[0].c_str()) : 0;
            int g = args.size() > 1 ? std::atoi(args[1].c_str()) : 0;
            int b = args.size() > 2 ? std::atoi(args[2].c_str()) : 0;
            return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
        }
        if (name == "rgbbg")
        {
            int r = args.size() > 0 ? std::atoi(args[0].c_str()) : 0;
            int g = args.size() > 1 ? std::atoi(args[1].c_str()) : 0;
            int b = args.size() > 2 ? std::atoi(args[2].c_str()) : 0;
            return "\x1b[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
        }

        // 3. no-arg built-ins
        {
            auto it = escapeSequences().find(name);
            if (it != escapeSequences().end())
                return it->second;
        }

        // 4. octal: \123
        {
            bool allOctal = !name.empty();
            for (char c : name)
                if (c < '0' || c > '7')
                {
                    allOctal = false;
                    break;
                }
            if (allOctal)
            {
                uint32_t cp = (uint32_t)std::stoul(name, nullptr, 8);
                return cpToUtf8(cp);
            }
        }

        // 5. hex: \xFF  \x1b
        if (name.size() >= 2 && name[0] == 'x')
        {
            bool allHex = true;
            for (size_t i = 1; i < name.size(); i++)
                if (!isHex(name[i]))
                {
                    allHex = false;
                    break;
                }
            if (allHex)
            {
                uint32_t cp = (uint32_t)std::stoul(name.substr(1), nullptr, 16);
                return cpToUtf8(cp);
            }
        }

        // 6. unicode: \uXXXX  \UXXXXXXXX
        if ((name[0] == 'u' || name[0] == 'U') && name.size() >= 5)
        {
            bool allHex = true;
            for (size_t i = 1; i < name.size(); i++)
                if (!isHex(name[i]))
                {
                    allHex = false;
                    break;
                }
            if (allHex)
            {
                uint32_t cp = (uint32_t)std::stoul(name.substr(1), nullptr, 16);
                return cpToUtf8(cp);
            }
        }

        // 7. raw ANSI sequence fallback: \x1b[...  \033[...  \e[...  \27[...
        {
            std::string ansi = resolveAnsiRaw(name);
            if (!ansi.empty())
                return ansi;
        }

        // 8. Truly unrecognized — return backslash + name literally
        return "\\" + name;
    }

    std::string Lexer::readEscape()
    {
        if (atEnd())
            return "\\";
        char first = peek();

        // Single-symbol escapes: \\ \' \" \{ \} \` \space
        if (first == '\\' || first == '\'' || first == '"' ||
            first == '{' || first == '}' || first == '`' || first == ' ')
        {
            advance();
            return processEscape(std::string(1, first), {});
        }

        // Read multi-char escape name: alphanumeric, underscore, dash
        std::string name;
        // Known multi-char handler names (could share prefix with single-char escapes)
        static const std::vector<std::string> HANDLER_NAMES = {
            "color",
            "fg",
            "bg",
            "textType",
            "sgr",
            "cursor",
            "pos",
            "unicode",
            "hex",
            "repeat",
            "pad",
            "padl",
            "upper",
            "lower",
            "256fg",
            "256bg",
            "rgbfg",
            "rgbbg",
        };
        while (!atEnd() && (std::isalnum((unsigned char)peek()) || peek() == '_' || peek() == '-'))
        {
            name += advance();
            // Stop early only if name is a known exact escape AND extending it
            // wouldn't match any longer escape OR handler name
            if (escapeSequences().count(name) || customEscapes().count(name))
            {
                char nx = peek();
                if (!std::isalnum((unsigned char)nx) && nx != '_' && nx != '-')
                    break;
                std::string ext = name + nx;
                bool anyMatch = false;
                // check no-arg table
                for (const auto &kv : escapeSequences())
                    if (kv.first.size() >= ext.size() && kv.first.compare(0, ext.size(), ext) == 0)
                    {
                        anyMatch = true;
                        break;
                    }
                // check handler names
                if (!anyMatch)
                    for (const auto &h : HANDLER_NAMES)
                        if (h.size() >= ext.size() && h.compare(0, ext.size(), ext) == 0)
                        {
                            anyMatch = true;
                            break;
                        }
                // check custom escapes
                if (!anyMatch)
                    for (const auto &kv : customEscapes())
                        if (kv.first.size() >= ext.size() && kv.first.compare(0, ext.size(), ext) == 0)
                        {
                            anyMatch = true;
                            break;
                        }
                if (!anyMatch)
                    break;
            }
        }

        if (name.empty())
        {
            // single non-alnum char
            char c = advance();
            return processEscape(std::string(1, c), {});
        }

        // Read optional arg block {arg1, arg2, ...}
        std::vector<std::string> args;
        if (!atEnd() && peek() == '{')
        {
            advance(); // consume {
            std::string argStr;
            int depth = 1;
            while (!atEnd() && depth > 0)
            {
                char c = peek();
                if (c == '{')
                {
                    depth++;
                    argStr += advance();
                }
                else if (c == '}')
                {
                    depth--;
                    if (depth > 0)
                        argStr += advance();
                    else
                        advance(); // consume closing }
                }
                else if (c == '\n')
                {
                    _line++;
                    _col = 1;
                    _pos++;
                    argStr += '\n';
                }
                else
                {
                    argStr += advance();
                }
            }
            args = splitArgs(argStr);
        }

        return processEscape(name, args);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  String scanners
    // ════════════════════════════════════════════════════════════════════════════

    void Lexer::scanString(char quote)
    {
        int col = _col - 1;
        std::string value;
        while (!atEnd() && peek() != quote)
        {
            char c = advance();
            if (c == '\n')
            {
                _line++;
                _col = 1;
                value += c;
                continue;
            }
            if (c == '\\')
            {
                value += readEscape();
                continue;
            }
            value += c;
        }
        if (atEnd())
            error("Unterminated string literal");
        advance(); // closing quote
        addToken(TT::STRING, value, col);
    }

    void Lexer::scanRawString(char quote)
    {
        int col = _col - 1;
        std::string value;
        while (!atEnd() && peek() != quote)
        {
            char c = advance();
            if (c == '\n')
            {
                _line++;
                _col = 1;
            }
            value += c;
        }
        if (atEnd())
            error("Unterminated raw string literal");
        advance();
        addToken(TT::STRING, value, col);
    }

    void Lexer::scanFString(char quote)
    {
        int col = _col - 2;
        addToken(TT::FSTRING_START, "f\"", col);

        std::string part;
        auto flush = [&]()
        {
            if (!part.empty())
            {
                addToken(TT::STRING_PART, part, _col - (int)part.size() - 1);
                part.clear();
            }
        };

        while (!atEnd())
        {
            char c = advance();

            if (c == '{')
            {
                flush();
                addToken(TT::INTERPOLATION_START, "{", _col - 2);
                std::string expr;
                int depth = 1;
                while (!atEnd() && depth > 0)
                {
                    char ic = advance();
                    if (ic == '{')
                        depth++;
                    else if (ic == '}')
                        depth--;
                    if (depth > 0)
                        expr += ic;
                }
                // Lex the interpolated expression with a sub-lexer
                Lexer inner(expr, _filename);
                inner.setDefinitions(_definitions);
                auto innerTokens = inner.tokenize();
                innerTokens.pop_back(); // remove EOF
                for (auto &t : innerTokens)
                    _tokens.push_back(t);
                addToken(TT::INTERPOLATION_END, "}", _col - 1);
                continue;
            }

            if (c == '\\')
            {
                part += readEscape();
                continue;
            }
            if (c == quote)
            {
                flush();
                addToken(TT::FSTRING_END, "f\"", _col - 1);
                return;
            }
            if (c == '\n')
            {
                _line++;
                _col = 1;
                part += c;
                continue;
            }
            part += c;
        }
        error("Unterminated f-string");
    }

    void Lexer::scanDoubleUnderscore()
    {
        // We've consumed the first two underscores. Read until __ closing.
        int col = _col - 2;
        std::string value;
        while (!atEnd())
        {
            char c = advance();
            if (c == '_' && peek() == '_')
            {
                advance(); // second _
                std::string fullName = "__" + value + "__";
                if (DYNAMIC_VARS.count(fullName))
                {
                    Token t;
                    t.type = TT::DVAR;
                    t.value = fullName;
                    t.line = _line;
                    t.column = col;
                    t.srcLine = _line;
                    t.srcCol = col;
                    _tokens.push_back(t);
                }
                else
                {
                    addToken(TT::IDENTIFIER, fullName, col);
                }
                return;
            }
            value += c;
        }
        error("Unterminated __identifier__");
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Number scanner
    // ════════════════════════════════════════════════════════════════════════════

    void Lexer::scanNumber(char first)
    {
        int col = _col - 1;
        std::string raw(1, first);

        // hex 0x
        if (first == '0' && (peek() == 'x' || peek() == 'X'))
        {
            raw += advance();
            while (!atEnd() && (isHex(peek()) || peek() == '_'))
            {
                char c = advance();
                if (c != '_')
                    raw += c;
            }
            addToken(TT::NUMBER, raw, (double)std::stoull(raw, nullptr, 16), col);
            return;
        }

        // binary 0b
        if (first == '0' && (peek() == 'b' || peek() == 'B'))
        {
            raw += advance();
            while (!atEnd() && (peek() == '0' || peek() == '1' || peek() == '_'))
            {
                char c = advance();
                if (c != '_')
                    raw += c;
            }
            addToken(TT::NUMBER, raw, (double)std::stoull(raw, nullptr, 2), col);
            return;
        }

        // custom base 0rBASE_DIGITS
        if (first == '0' && (peek() == 'r' || peek() == 'R'))
        {
            advance(); // consume r
            std::string baseStr;
            while (!atEnd() && isDigit(peek()))
                baseStr += advance();
            int base = std::atoi(baseStr.c_str());
            if (base < 2 || base > 36)
                error("Invalid numeric base '" + baseStr + "'");
            std::string digits;
            while (!atEnd())
            {
                char c = peek();
                if (c == '_')
                {
                    advance();
                    continue;
                }
                int d = std::isdigit((unsigned char)c) ? c - '0' : std::tolower((unsigned char)c) - 'a' + 10;
                if (d < 0 || d >= base)
                    break;
                digits += advance();
            }
            addToken(TT::NUMBER, raw, (double)std::stoull(digits, nullptr, base), col);
            return;
        }

        // H_UNIT: 0hNAME
        if (first == '0' && (peek() == 'h' || peek() == 'H'))
        {
            advance(); // consume h
            std::string unitName;
            while (!atEnd() && (isAlpha(peek()) || peek() == '_'))
                unitName += advance();
            // strip underscores for lookup
            std::string key;
            for (char c : unitName)
                if (c != '_')
                    key += std::tolower((unsigned char)c);
            auto it = H_UNIT.find(key);
            if (it == H_UNIT.end())
                error("Unknown H_UNIT literal '" + unitName + "'");
            addToken(TT::NUMBER, "0h" + unitName, it->second, col);
            return;
        }

        // decimal (with _ separators)
        while (!atEnd() && (isDigit(peek()) || peek() == '_'))
        {
            char c = advance();
            if (c != '_')
                raw += c;
        }
        // fractional
        if (!atEnd() && peek() == '.' && isDigit(_pos + 1 < _source.size() ? _source[_pos + 1] : '\0'))
        {
            raw += advance(); // .
            while (!atEnd() && (isDigit(peek()) || peek() == '_'))
            {
                char c = advance();
                if (c != '_')
                    raw += c;
            }
        }
        // exponent
        if (!atEnd() && (peek() == 'e' || peek() == 'E'))
        {
            raw += advance();
            if (!atEnd() && (peek() == '+' || peek() == '-'))
                raw += advance();
            while (!atEnd() && isDigit(peek()))
                raw += advance();
        }

        // BigInt suffix n — store as double (precision loss for very large, but sufficient)
        if (!atEnd() && peek() == 'n')
        {
            advance();
            addToken(TT::NUMBER, raw + "n", std::stod(raw), col);
            return;
        }

        // short suffixes k/m/b/t
        if (!atEnd())
        {
            char sfx = std::tolower((unsigned char)peek());
            static const std::unordered_map<char, double> SFX = {
                {'k', 1e3}, {'m', 1e6}, {'b', 1e9}, {'t', 1e12}};
            auto it = SFX.find(sfx);
            if (it != SFX.end())
            {
                advance();
                addToken(TT::NUMBER, raw + sfx, std::stod(raw) * it->second, col);
                return;
            }
        }

        addToken(TT::NUMBER, raw, std::stod(raw), col);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Identifier scanner
    // ════════════════════════════════════════════════════════════════════════════

    void Lexer::scanIdentifier(char first)
    {
        int col = _col - 1;
        std::string ident(1, first);
        while (!atEnd() && isAlNum(peek()))
            ident += advance();

        // f-string
        if (ident == "f" && !atEnd() && (peek() == '"' || peek() == '\''))
        {
            char q = advance();
            scanFString(q);
            return;
        }
        // raw string
        if (ident == "r" && !atEnd() && (peek() == '"' || peek() == '\''))
        {
            char q = advance();
            scanRawString(q);
            return;
        }

        // URL detection
        static const std::unordered_set<std::string> URL_SCHEMES = {
            "http", "https", "ftp", "ws", "wss", "content"};
        if (URL_SCHEMES.count(ident) &&
            _pos + 2 < _source.size() &&
            peek(0) == ':' && peek(1) == '/' && peek(2) == '/')
        {
            std::string url = ident;
            while (!atEnd())
            {
                char c = peek();
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == ',' ||
                    c == ')' || c == '}' || c == ']' || c == '"' || c == '\'' || c == '(')
                    break;
                url += advance();
            }
            addToken(TT::URL, url, col);
            return;
        }
        if (ident == "localhost" && !atEnd() && peek() == ':' &&
            _pos + 1 < _source.size() && isDigit(_source[_pos + 1]))
        {
            std::string url = ident;
            while (!atEnd())
            {
                char c = peek();
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == ',' ||
                    c == ')' || c == '}' || c == ']' || c == '"' || c == '\'' || c == '(')
                    break;
                url += advance();
            }
            addToken(TT::URL, url, col);
            return;
        }

        // preprocessor replacement
        {
            auto it = _definitions.find(ident);
            if (it != _definitions.end())
            {
                injectReplacement(it->second);
                return;
            }
        }

        // literal true/false/null
        if (ident == "true")
        {
            addToken(TT::LITERAL, "true", 1.0, col);
            return;
        }
        if (ident == "false")
        {
            addToken(TT::LITERAL, "false", 0.0, col);
            return;
        }
        if (ident == "null")
        {
            addToken(TT::LITERAL, "null", 0.0, col);
            return;
        }

        // dynamic vars
        if (DYNAMIC_VARS.count(ident))
        {
            Token t;
            t.type = TT::DVAR;
            t.value = ident;
            t.line = _line;
            t.column = col;
            t.srcLine = _line;
            t.srcCol = col;
            _tokens.push_back(t);
            return;
        }

        // keyword operators (in, instanceof, typeof, etc.)
        {
            auto &ops = operators();
            auto it = ops.find(ident);
            if (it != ops.end())
            {
                addOpToken(ident, col);
                return;
            }
        }

        // RC_* family: RC_REST, RC_stop, RC_<keyword>
        if (ident.size() > 3 && ident[0] == 'R' && ident[1] == 'C' && ident[2] == '_')
        {
            std::string suffix = ident.substr(3);
            if (suffix == "REST" || suffix == "stop" || KEYWORDS.count(suffix))
            {
                addToken(TT::RC, ident, col);
                return;
            }
        }

        // keyword or identifier
        if (KEYWORDS.count(ident))
            addToken(TT::KEYWORD, ident, col);
        else
            addToken(TT::IDENTIFIER, ident, col);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Symbol / operator scanner
    // ════════════════════════════════════════════════════════════════════════════

    void Lexer::scanSymbol(char ch)
    {
        int col = _col - 1;
        char n1 = peek(0);
        char n2 = peek(1);

        // Try 3-char, then 2-char, then 1-char operators
        std::string s3 = {ch, n1, n2};
        std::string s2 = {ch, n1};
        std::string s1 = {ch};

        auto &ops = operators();
        if (ops.count(s3))
        {
            advance();
            advance();
            addOpToken(s3, col);
            return;
        }
        if (ops.count(s2))
        {
            advance();
            addOpToken(s2, col);
            return;
        }
        if (ops.count(s1))
        {
            addOpToken(s1, col);
            return;
        }

        if (PUNCTUATION.count(ch))
        {
            addToken(TT::PUNCTUATION, s1, col);
            return;
        }

        error(std::string("Unexpected character '") + ch + "'");
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  Error
    // ════════════════════════════════════════════════════════════════════════════

    void Lexer::error(const std::string &msg)
    {
        SourceLoc loc{_line, _col, _filename};
        throw LexError("Lex", msg, loc, _source);
    }

} // namespace novac