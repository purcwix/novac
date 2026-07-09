#pragma once
#include "token.h"
#include "../error/novac_error.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace novac
{

    // Custom escape registry — populated by #register escape directives
    // string → either a literal string or a handler function
    using EscapeHandler = std::function<std::string(const std::vector<std::string> &)>;

    struct CustomEscape
    {
        bool isHandler = false;
        std::string literal;
        EscapeHandler handler;
    };

    inline std::unordered_map<std::string, CustomEscape> &customEscapes()
    {
        static std::unordered_map<std::string, CustomEscape> reg;
        return reg;
    }

    class Lexer
    {
    public:
        explicit Lexer(std::string source, std::string filename = "<repl>");

        // Tokenize the full source. Returns token list ending with EOF_TOKEN.
        std::vector<Token> tokenize();

        // Inherit macro definitions from a parent lexer (used for f-string inner lexers)
        void setDefinitions(const std::unordered_map<std::string, std::string> &defs);

    private:
        std::string _source;
        std::string _filename;
        size_t _pos = 0;
        int _line = 1;
        int _col = 1;
        bool _skip = false; // ifdef/ifndef skip mode

        std::vector<Token> _tokens;
        std::unordered_map<std::string, std::string> _definitions;

        // ── character helpers ────────────────────────────────────────────────
        char peek(int offset = 0) const;
        char advance();
        bool atEnd() const;
        bool isWS(char c) const;
        bool isDigit(char c) const;
        bool isAlpha(char c) const;
        bool isAlNum(char c) const;
        bool isHex(char c) const;

        // ── token emitters ───────────────────────────────────────────────────
        void addToken(TT type, const std::string &value, int col = -1);
        void addToken(TT type, const std::string &value, double numval, int col);
        void addOpToken(const std::string &op, int col);

        // ── scanners ─────────────────────────────────────────────────────────
        void scanToken();
        void scanNumber(char first);
        void scanString(char quote);
        void scanRawString(char quote);
        void scanFString(char quote);
        void scanDoubleUnderscore(); // __ident__
        void scanSymbol(char ch);
        void scanIdentifier(char first);

        // ── preprocessor ────────────────────────────────────────────────────
        void handleDirective();
        std::string readBareIdent();
        std::string readRestOfLine();
        void skipLine();
        void injectReplacement(const std::string &str);

        // ── comments ─────────────────────────────────────────────────────────
        void skipLineComment();
        void skipBlockComment();

        // ── escape engine ────────────────────────────────────────────────────
        std::string readEscape();
        std::string processEscape(const std::string &name,
                                  const std::vector<std::string> &args);
        std::string resolveAnsiRaw(const std::string &raw); // fallback raw ANSI parser
        std::vector<std::string> splitArgs(const std::string &s);

        // ── error helper ─────────────────────────────────────────────────────
        [[noreturn]] void error(const std::string &msg);
    };

} // namespace novac
