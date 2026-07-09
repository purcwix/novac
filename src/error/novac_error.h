#pragma once
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

namespace novac {

// ── Source location ──────────────────────────────────────────────────────────
struct SourceLoc {
    int  line   = 1;
    int  column = 1;
    std::string file = "<repl>";
};

// ── Base novac exception ─────────────────────────────────────────────────────
class NovacError : public std::exception {
public:
    NovacError(std::string kind, std::string msg, SourceLoc loc, std::string src)
        : _kind(std::move(kind))
        , _msg(std::move(msg))
        , _loc(std::move(loc))
        , _src(std::move(src))
    {
        _formatted = _format();
    }

    const char* what() const noexcept override { return _formatted.c_str(); }
    const std::string& kind()    const { return _kind; }
    const std::string& message() const { return _msg;  }
    const SourceLoc&   loc()     const { return _loc;  }

private:
    std::string _kind, _msg, _formatted;
    SourceLoc   _loc;
    std::string _src;

    std::string _format() const {
        // split source into lines
        std::vector<std::string> lines;
        std::istringstream ss(_src);
        std::string l;
        while (std::getline(ss, l)) lines.push_back(l);

        std::ostringstream out;
        out << "[Novac" << _kind << " " << _loc.line << ":" << _loc.column << "] " << _msg << "\n";
        out << " error at:\n";
        if (_loc.line >= 1 && _loc.line <= (int)lines.size()) {
            out << " " << lines[_loc.line - 1] << "\n";
            int pad = std::max(_loc.column - 1, 0);
            out << " " << std::string(pad, ' ') << "^^";
        }
        return out.str();
    }
};

// ── Specific error types ─────────────────────────────────────────────────────
struct LexError   : NovacError { using NovacError::NovacError; };
struct ParseError : NovacError { using NovacError::NovacError; };
struct RuntimeError : NovacError { using NovacError::NovacError; };

} // namespace novac
