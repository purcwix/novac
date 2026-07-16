#define _USE_MATH_DEFINES
#include "vm.h"
#include "../runtime/utf8.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif
#if defined(_WIN32)
#include <conio.h>
#include <objbase.h>
#include <oleauto.h>
#include <oaidl.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM etc.
#include <commctrl.h> // buttons, edits, listbox, combobox, common ctrls
#include <commdlg.h>  // GetOpenFileName / GetSaveFileName / ChooseColor
#include <windows.h>
#include <ocidl.h> // IConnectionPoint, IConnectionPointContainer, IID_IConnectionPointContainer
#include <tlhelp32.h>
#include <shlobj.h>   // SHGetSpecialFolderPath, CSIDL_*
#include <shellapi.h> // ShellExecuteA
#include <mmsystem.h> // PlaySound, mciSendString
#include <dbt.h>      // DEV_BROADCAST_DEVICEINTERFACE, RegisterDeviceNotification
#else
#include <termios.h>
#include <fcntl.h>
#endif
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <mach/mach_time.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <unistd.h>
#endif
#include <regex>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <unordered_map>
#include <thread>
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <type_traits>
#include <cmath>
#include <ctime>
#include <chrono>
#include <stdexcept>
#include <filesystem>
#include <cassert>
#include <condition_variable>
#include <numbers> // C++20 math constants
#include <numeric> // gcd, lcm, iota, accumulate, etc.
#include <latch>   // C++20
#include <barrier> // C++20
#include <random>
#include <bitset>
#include <complex>
#include <valarray>
#include <charconv>

namespace novac
{
    // Apple's shipped libc++ (still true as of Xcode 16.4) does not implement
    // the floating-point overload of std::from_chars, only the integer one.
    // On that overload resolution falls through to a deleted bool overload
    // and fails to compile. Route through strtod there; use std::from_chars
    // everywhere else since it's faster and locale-independent.
    inline std::from_chars_result portable_from_chars_double(const char *first, const char *last, double &value)
    {
#if defined(__APPLE__)
        std::string tmp(first, last);
        char *endp = nullptr;
        errno = 0;
        double v = std::strtod(tmp.c_str(), &endp);
        if (endp == tmp.c_str())
        {
            return std::from_chars_result{first, std::errc::invalid_argument};
        }
        value = v;
        const char *newLast = first + (endp - tmp.c_str());
        return std::from_chars_result{newLast, std::errc{}};
#else
        return std::from_chars(first, last, value);
#endif
    }
}

#include <bit> // C++20

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif
#if defined(_WIN32)
#define NOVA_FFI_PLATFORM_SUPPORTED 1
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define NOVA_FFI_PLATFORM_SUPPORTED 1
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h> // sysconf(_SC_PAGESIZE)
#else
#define NOVA_FFI_PLATFORM_SUPPORTED 0
#endif

#if defined(NOVA_HAVE_LIBFFI) && NOVA_FFI_PLATFORM_SUPPORTED
#include <ffi.h>
#define NOVA_CCALL_AVAILABLE 1
#else
#define NOVA_CCALL_AVAILABLE 0
#endif
#ifdef __linux__
#include <sys/resource.h>
#endif
#define MKStream(StreamType, Stream) \
    NovaValue::makeStream(           \
        std::shared_ptr<StreamType>( \
            &Stream,                 \
            [](StreamType *) {}))

// ════════════════════════════════════════════════════════════════════════════
//  Minimal JSON parser (no dependencies)
// ════════════════════════════════════════════════════════════════════════════
namespace nova_json
{
    using namespace novac;
    struct JVal
    {
        enum class Kind
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        } kind = Kind::Null;
        bool b = false;
        double n = 0;
        std::string s;
        std::vector<JVal> arr;
        std::vector<std::pair<std::string, JVal>> obj;
    };

    struct Parser
    {
        const std::string &src;
        size_t pos = 0;

        Parser(const std::string &s) : src(s) {}

        void skipWs()
        {
            while (pos < src.size() && std::isspace((unsigned char)src[pos]))
                pos++;
        }

        char peek()
        {
            skipWs();
            return pos < src.size() ? src[pos] : 0;
        }
        char next()
        {
            skipWs();
            return pos < src.size() ? src[pos++] : 0;
        }

        std::string parseString()
        {
            pos++; // skip opening "
            std::string out;
            while (pos < src.size() && src[pos] != '"')
            {
                if (src[pos] == '\\')
                {
                    pos++;
                    switch (src[pos++])
                    {
                    case '"':
                        out += '"';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case '/':
                        out += '/';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'b':
                        out += '\b';
                        break;
                    case 'f':
                        out += '\f';
                        break;
                    case 'u':
                    {
                        // \uXXXX
                        uint32_t code = 0;
                        for (int i = 0; i < 4 && pos < src.size(); i++, pos++)
                        {
                            char c = src[pos];
                            code <<= 4;
                            if (c >= '0' && c <= '9')
                                code |= c - '0';
                            else if (c >= 'a' && c <= 'f')
                                code |= c - 'a' + 10;
                            else if (c >= 'A' && c <= 'F')
                                code |= c - 'A' + 10;
                        }
                        // encode as UTF-8
                        if (code < 0x80)
                            out += (char)code;
                        else if (code < 0x800)
                        {
                            out += (char)(0xC0 | (code >> 6));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        else
                        {
                            out += (char)(0xE0 | (code >> 12));
                            out += (char)(0x80 | ((code >> 6) & 0x3F));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
                else
                {
                    out += src[pos++];
                }
            }
            if (pos < src.size())
                pos++; // skip closing "
            return out;
        }

        JVal parseValue()
        {
            char c = peek();
            if (c == '"')
            {
                JVal v;
                v.kind = JVal::Kind::String;
                v.s = parseString();
                return v;
            }
            if (c == '{')
            {
                next(); // skip {
                JVal v;
                v.kind = JVal::Kind::Object;
                while (peek() != '}' && pos < src.size())
                {
                    if (peek() != '"')
                        break;
                    std::string key = parseString();
                    if (next() != ':')
                        break; // skip :, re-skipWs handled by next()
                    v.obj.push_back({key, parseValue()});
                    if (peek() == ',')
                        next();
                }
                if (peek() == '}')
                    next();
                return v;
            }
            if (c == '[')
            {
                next(); // skip [
                JVal v;
                v.kind = JVal::Kind::Array;
                while (peek() != ']' && pos < src.size())
                {
                    v.arr.push_back(parseValue());
                    if (peek() == ',')
                        next();
                }
                if (peek() == ']')
                    next();
                return v;
            }
            if (c == 't')
            {
                pos += 4;
                JVal v;
                v.kind = JVal::Kind::Bool;
                v.b = true;
                return v;
            }
            if (c == 'f')
            {
                pos += 5;
                JVal v;
                v.kind = JVal::Kind::Bool;
                v.b = false;
                return v;
            }
            if (c == 'n')
            {
                pos += 4;
                return JVal{};
            }
            // number
            {
                size_t start = pos;
                if (pos < src.size() && src[pos] == '-')
                    pos++;
                while (pos < src.size() && (std::isdigit((unsigned char)src[pos]) || src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' || src[pos] == '+' || src[pos] == '-'))
                    pos++;
                JVal v;
                v.kind = JVal::Kind::Number;
                try
                {
                    v.n = std::stod(src.substr(start, pos - start));
                }
                catch (...)
                {
                }
                return v;
            }
        }

        JVal parse() { return parseValue(); }
    };

    // convert JVal → Nova Val
    inline Val toVal(const JVal &j)
    {
        switch (j.kind)
        {
        case JVal::Kind::Null:
            return nova_null();
        case JVal::Kind::Bool:
            return nova_bool(j.b);
        case JVal::Kind::Number:
            return nova_num(j.n);
        case JVal::Kind::String:
            return nova_str(j.s);
        case JVal::Kind::Array:
        {
            ValVec arr;
            arr.reserve(j.arr.size());
            for (auto &e : j.arr)
                arr.push_back(toVal(e));
            return nova_arr(std::move(arr));
        }
        case JVal::Kind::Object:
        {
            auto obj = nova_obj();
            for (auto &[k, v] : j.obj)
                obj->obj->set(k, toVal(v));
            return obj;
        }
        }
        return nova_null();
    }

    inline Val parse(const std::string &s)
    {
        try
        {
            Parser p(s);
            return toVal(p.parse());
        }
        catch (...)
        {
            return nova_null();
        }
    }

    // get a string field from a parsed JSON object Val
    inline std::string getString(Val obj, const std::string &key)
    {
        if (!obj || !obj.isObject())
            return "";
        Val v = obj->obj->get(key);
        return (v && v.isString()) ? v->sval : "";
    }
}

// --------------------------------------------------------------------------
//  Minimal base64 encoder (RFC 4648) — used for MIME attachments & auth
// --------------------------------------------------------------------------
namespace nova_email_detail
{
    using namespace novac;

    static const char B64_CHARS[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    static std::string base64Encode(const std::string &in)
    {
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        size_t i = 0;
        while (i < in.size())
        {
            uint32_t b = (uint8_t)in[i++] << 16;
            if (i < in.size())
                b |= (uint8_t)in[i++] << 8;
            if (i < in.size())
                b |= (uint8_t)in[i++];
            out += B64_CHARS[(b >> 18) & 63];
            out += B64_CHARS[(b >> 12) & 63];
            out += (i > in.size() + 1) ? '=' : B64_CHARS[(b >> 6) & 63];
            out += (i > in.size()) ? '=' : B64_CHARS[b & 63];
        }
        return out;
    }

    static std::string base64Decode(const std::string &in)
    {
        auto val = [](char c) -> int
        {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1;
        };
        std::string out;
        int buf = 0, bits = 0;
        for (char c : in)
        {
            int v = val(c);
            if (v < 0)
                continue;
            buf = (buf << 6) | v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out += (char)((buf >> bits) & 0xFF);
            }
        }
        return out;
    }

    // --------------------------------------------------------------------------
    //  Quoted-Printable encoder (RFC 2045) — for text body encoding
    // --------------------------------------------------------------------------
    static std::string quotedPrintableEncode(const std::string &in)
    {
        std::string out;
        int col = 0;
        auto softBreak = [&]()
        {
            if (col >= 75)
            {
                out += "=\r\n";
                col = 0;
            }
        };
        for (unsigned char c : in)
        {
            if (c == '\r' || c == '\n')
            {
                out += (char)c;
                col = 0;
                continue;
            }
            if ((c >= 33 && c <= 126 && c != '=') || c == ' ' || c == '\t')
            {
                softBreak();
                out += (char)c;
                col++;
            }
            else
            {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "=%02X", c);
                if (col + 3 > 75)
                {
                    out += "=\r\n";
                    col = 0;
                }
                out += buf;
                col += 3;
            }
        }
        return out;
    }

    // --------------------------------------------------------------------------
    //  MIME boundary generator
    // --------------------------------------------------------------------------
    static std::string makeBoundary()
    {
        static std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<uint64_t> dist;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "----=_Nova_%016llx_%016llx",
                      (unsigned long long)dist(rng), (unsigned long long)dist(rng));
        return buf;
    }

    // --------------------------------------------------------------------------
    //  Fold long header lines at 78 chars (RFC 5322)
    // --------------------------------------------------------------------------
    static std::string foldHeader(const std::string &name, const std::string &val)
    {
        std::string line = name + ": " + val;
        if (line.size() <= 78)
            return line + "\r\n";
        std::string out;
        size_t pos = 0;
        bool first = true;
        while (pos < line.size())
        {
            size_t take = first ? 78 : 76;
            if (pos + take >= line.size())
            {
                out += line.substr(pos) + "\r\n";
                break;
            }
            // find a fold point (space/tab near the limit)
            size_t fold = line.rfind(' ', pos + take);
            if (fold == std::string::npos || fold <= pos)
                fold = pos + take;
            out += line.substr(pos, fold - pos) + "\r\n";
            pos = fold;
            while (pos < line.size() && line[pos] == ' ')
                pos++;
            if (pos < line.size())
            {
                out += "\t";
            }
            first = false;
        }
        return out;
    }

    // --------------------------------------------------------------------------
    //  RFC 2047 encode non-ASCII header value
    // --------------------------------------------------------------------------
    static std::string rfc2047Encode(const std::string &s)
    {
        for (unsigned char c : s)
            if (c > 127)
                return "=?UTF-8?B?" + base64Encode(s) + "?=";
        return s;
    }

    // --------------------------------------------------------------------------
    //  Parse a raw email address: "Name <addr>" → {name, addr}
    // --------------------------------------------------------------------------
    static std::pair<std::string, std::string> parseAddress(const std::string &s)
    {
        size_t lt = s.find('<'), gt = s.find('>');
        if (lt != std::string::npos && gt != std::string::npos && gt > lt)
        {
            std::string name = s.substr(0, lt);
            while (!name.empty() && (name.back() == ' ' || name.back() == '"'))
                name.pop_back();
            while (!name.empty() && (name.front() == ' ' || name.front() == '"'))
                name.erase(0, 1);
            return {name, s.substr(lt + 1, gt - lt - 1)};
        }
        // bare address
        std::string addr = s;
        while (!addr.empty() && addr.front() == ' ')
            addr.erase(0, 1);
        while (!addr.empty() && addr.back() == ' ')
            addr.pop_back();
        return {"", addr};
    }

    // --------------------------------------------------------------------------
    //  Format an address for a header
    // --------------------------------------------------------------------------
    static std::string formatAddress(const std::string &name, const std::string &addr)
    {
        if (name.empty())
            return addr;
        return rfc2047Encode(name) + " <" + addr + ">";
    }

    // --------------------------------------------------------------------------
    //  Struct holding a parsed email message
    // --------------------------------------------------------------------------
    struct ParsedEmail
    {
        std::string messageId;
        std::string date;
        std::string fromName, fromAddr;
        std::vector<std::pair<std::string, std::string>> to; // {name, addr}
        std::vector<std::pair<std::string, std::string>> cc;
        std::vector<std::pair<std::string, std::string>> bcc;
        std::string replyTo;
        std::string subject;
        std::string inReplyTo;
        std::string references;
        std::string body; // plaintext
        std::string htmlBody;
        std::unordered_map<std::string, std::string> headers; // all raw headers
        std::string contentType;
        std::string mimeVersion;
        // attachments: {filename, contentType, data (raw bytes)}
        std::vector<std::tuple<std::string, std::string, std::string>> attachments;
        bool isMultipart = false;
        std::string boundary;
    };

    // --------------------------------------------------------------------------
    //  Parse raw RFC 5322 message into ParsedEmail
    // --------------------------------------------------------------------------
    static ParsedEmail parseRaw(const std::string &raw)
    {
        ParsedEmail e;
        size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos)
            headerEnd = raw.find("\n\n");
        std::string headerBlock = (headerEnd != std::string::npos) ? raw.substr(0, headerEnd) : raw;
        std::string bodyBlock = (headerEnd != std::string::npos)
                                    ? raw.substr(headerEnd + (raw[headerEnd + 1] == '\n' ? 2 : 4))
                                    : "";

        // unfold headers
        std::string unfolded;
        {
            std::istringstream ss(headerBlock);
            std::string line;
            while (std::getline(ss, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
                {
                    unfolded += ' ';
                    size_t s = line.find_first_not_of(" \t");
                    unfolded += (s == std::string::npos ? "" : line.substr(s));
                }
                else
                {
                    if (!unfolded.empty())
                        unfolded += '\n';
                    unfolded += line;
                }
            }
        }

        // split into name: value pairs
        {
            std::istringstream ss(unfolded);
            std::string line;
            while (std::getline(ss, line))
            {
                size_t colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                while (!val.empty() && val.front() == ' ')
                    val.erase(0, 1);
                // lowercase key for matching
                std::string lkey = key;
                std::transform(lkey.begin(), lkey.end(), lkey.begin(), ::tolower);
                e.headers[lkey] = val;

                if (lkey == "from")
                {
                    auto [n, a] = parseAddress(val);
                    e.fromName = n;
                    e.fromAddr = a;
                }
                else if (lkey == "subject")
                    e.subject = val;
                else if (lkey == "message-id")
                    e.messageId = val;
                else if (lkey == "date")
                    e.date = val;
                else if (lkey == "in-reply-to")
                    e.inReplyTo = val;
                else if (lkey == "references")
                    e.references = val;
                else if (lkey == "reply-to")
                    e.replyTo = val;
                else if (lkey == "mime-version")
                    e.mimeVersion = val;
                else if (lkey == "content-type")
                {
                    e.contentType = val;
                    if (val.find("multipart") != std::string::npos)
                    {
                        e.isMultipart = true;
                        size_t bp = val.find("boundary=");
                        if (bp != std::string::npos)
                        {
                            e.boundary = val.substr(bp + 9);
                            if (!e.boundary.empty() && e.boundary.front() == '"')
                                e.boundary = e.boundary.substr(1, e.boundary.size() - 2);
                        }
                    }
                }
                else if (lkey == "to")
                {
                    // split on comma
                    std::istringstream ts(val);
                    std::string addr;
                    while (std::getline(ts, addr, ','))
                    {
                        while (!addr.empty() && addr.front() == ' ')
                            addr.erase(0, 1);
                        auto [n, a] = parseAddress(addr);
                        e.to.push_back({n, a});
                    }
                }
                else if (lkey == "cc")
                {
                    std::istringstream ts(val);
                    std::string addr;
                    while (std::getline(ts, addr, ','))
                    {
                        while (!addr.empty() && addr.front() == ' ')
                            addr.erase(0, 1);
                        auto [n, a] = parseAddress(addr);
                        e.cc.push_back({n, a});
                    }
                }
            }
        }

        // parse body / MIME parts
        if (e.isMultipart && !e.boundary.empty())
        {
            std::string delim = "--" + e.boundary;
            size_t pos = 0;
            while (pos < bodyBlock.size())
            {
                size_t start = bodyBlock.find(delim, pos);
                if (start == std::string::npos)
                    break;
                start += delim.size();
                if (bodyBlock.substr(start, 2) == "--")
                    break; // end boundary
                if (bodyBlock[start] == '\r')
                    start++;
                if (bodyBlock[start] == '\n')
                    start++;
                size_t end = bodyBlock.find(delim, start);
                std::string part = (end == std::string::npos) ? bodyBlock.substr(start) : bodyBlock.substr(start, end - start);

                // parse part headers
                size_t phe = part.find("\r\n\r\n");
                if (phe == std::string::npos)
                    phe = part.find("\n\n");
                std::string ph = (phe != std::string::npos) ? part.substr(0, phe) : "";
                std::string pb = (phe != std::string::npos) ? part.substr(phe + (part[phe + 1] == '\n' ? 2 : 4)) : part;

                std::string partCT, partCTE, partCD;
                std::istringstream pss(ph);
                std::string pl;
                while (std::getline(pss, pl))
                {
                    if (!pl.empty() && pl.back() == '\r')
                        pl.pop_back();
                    size_t c = pl.find(':');
                    if (c == std::string::npos)
                        continue;
                    std::string pk = pl.substr(0, c);
                    std::string pv = pl.substr(c + 1);
                    while (!pv.empty() && pv.front() == ' ')
                        pv.erase(0, 1);
                    std::transform(pk.begin(), pk.end(), pk.begin(), ::tolower);
                    if (pk == "content-type")
                        partCT = pv;
                    else if (pk == "content-transfer-encoding")
                        partCTE = pv;
                    else if (pk == "content-disposition")
                        partCD = pv;
                }

                // decode transfer encoding
                std::string decoded = pb;
                // strip trailing CRLF
                while (!decoded.empty() && (decoded.back() == '\r' || decoded.back() == '\n'))
                    decoded.pop_back();
                std::transform(partCTE.begin(), partCTE.end(), partCTE.begin(), ::tolower);
                if (partCTE == "base64")
                    decoded = base64Decode(decoded);
                // quoted-printable decode (simplified)
                else if (partCTE.find("quoted-printable") != std::string::npos)
                {
                    std::string qpd;
                    for (size_t i = 0; i < decoded.size(); i++)
                    {
                        if (decoded[i] == '=' && i + 2 < decoded.size() && decoded[i + 1] != '\n')
                        {
                            int hi = std::isxdigit((unsigned char)decoded[i + 1]) ? decoded[i + 1] : 0;
                            int lo = std::isxdigit((unsigned char)decoded[i + 2]) ? decoded[i + 2] : 0;
                            auto hval = [](char c) -> int
                            { return c >= '0' && c <= '9' ? c - '0' : std::tolower(c) - 'a' + 10; };
                            qpd += (char)((hval(hi) << 4) | hval(lo));
                            i += 2;
                        }
                        else
                        {
                            qpd += decoded[i];
                        }
                    }
                    decoded = qpd;
                }

                // classify part
                std::string ctLow = partCT;
                std::transform(ctLow.begin(), ctLow.end(), ctLow.begin(), ::tolower);
                if (ctLow.find("text/plain") != std::string::npos)
                    e.body = decoded;
                else if (ctLow.find("text/html") != std::string::npos)
                    e.htmlBody = decoded;
                else if (!partCD.empty() && partCD.find("attachment") != std::string::npos)
                {
                    // extract filename
                    std::string fname;
                    size_t fnpos = partCD.find("filename=");
                    if (fnpos != std::string::npos)
                    {
                        fname = partCD.substr(fnpos + 9);
                        if (!fname.empty() && fname.front() == '"')
                            fname = fname.substr(1, fname.size() - 2);
                    }
                    e.attachments.push_back({fname, partCT, decoded});
                }

                pos = (end == std::string::npos) ? bodyBlock.size() : end;
            }
        }
        else
        {
            e.body = bodyBlock;
        }

        return e;
    }

    // --------------------------------------------------------------------------
    //  Build raw RFC 5322 + MIME email string from parts
    // --------------------------------------------------------------------------
    struct EmailSpec
    {
        std::string fromName, fromAddr;
        std::vector<std::pair<std::string, std::string>> to;
        std::vector<std::pair<std::string, std::string>> cc;
        std::vector<std::pair<std::string, std::string>> bcc;
        std::string replyTo;
        std::string subject;
        std::string body;
        std::string htmlBody;
        std::string messageId;
        std::string date;
        std::string inReplyTo;
        std::string references;
        std::unordered_map<std::string, std::string> extraHeaders;
        // attachments: {filename, contentType, rawData}
        std::vector<std::tuple<std::string, std::string, std::string>> attachments;
        std::string encoding = "quoted-printable"; // "base64" | "quoted-printable" | "7bit"
    };

    static std::string buildRaw(const EmailSpec &spec)
    {
        std::string boundary = makeBoundary();
        bool multipart = !spec.htmlBody.empty() || !spec.attachments.empty();
        std::string altBoundary = makeBoundary();
        bool hasAlt = !spec.htmlBody.empty();
        bool hasAttach = !spec.attachments.empty();

        std::string msg;

        // ── headers ──────────────────────────────────────────────────────────────
        msg += foldHeader("From", formatAddress(spec.fromName, spec.fromAddr));
        // To
        {
            std::string toList;
            for (size_t i = 0; i < spec.to.size(); i++)
            {
                if (i)
                    toList += ", ";
                toList += formatAddress(spec.to[i].first, spec.to[i].second);
            }
            if (!toList.empty())
                msg += foldHeader("To", toList);
        }
        // Cc
        if (!spec.cc.empty())
        {
            std::string ccList;
            for (size_t i = 0; i < spec.cc.size(); i++)
            {
                if (i)
                    ccList += ", ";
                ccList += formatAddress(spec.cc[i].first, spec.cc[i].second);
            }
            msg += foldHeader("Cc", ccList);
        }
        // Reply-To
        if (!spec.replyTo.empty())
            msg += foldHeader("Reply-To", spec.replyTo);
        // Subject
        msg += foldHeader("Subject", rfc2047Encode(spec.subject));
        // Message-ID
        {
            std::string mid = spec.messageId;
            if (mid.empty())
            {
                static std::mt19937_64 rng(std::random_device{}());
                std::uniform_int_distribution<uint64_t> dist;
                char buf[64];
                std::snprintf(buf, sizeof(buf), "<%llx.%llx@novac>",
                              (unsigned long long)dist(rng), (unsigned long long)dist(rng));
                mid = buf;
            }
            msg += foldHeader("Message-ID", mid);
        }
        // Date
        {
            std::string d = spec.date;
            if (d.empty())
            {
                std::time_t t = std::time(nullptr);
                char buf[64];
                std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", std::gmtime(&t));
                d = buf;
            }
            msg += foldHeader("Date", d);
        }
        if (!spec.inReplyTo.empty())
            msg += foldHeader("In-Reply-To", spec.inReplyTo);
        if (!spec.references.empty())
            msg += foldHeader("References", spec.references);
        // Extra custom headers
        for (auto &[k, v] : spec.extraHeaders)
            msg += foldHeader(k, v);
        // MIME-Version
        msg += "MIME-Version: 1.0\r\n";
        // Content-Type
        if (multipart)
        {
            if (hasAttach)
            {
                msg += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n";
            }
            else
            {
                msg += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n";
            }
        }
        else
        {
            msg += "Content-Type: text/plain; charset=UTF-8\r\n";
            msg += "Content-Transfer-Encoding: " + spec.encoding + "\r\n";
        }
        msg += "\r\n"; // end headers

        // ── body ─────────────────────────────────────────────────────────────────
        if (!multipart)
        {
            if (spec.encoding == "base64")
                msg += base64Encode(spec.body);
            else if (spec.encoding == "quoted-printable")
                msg += quotedPrintableEncode(spec.body);
            else
                msg += spec.body;
            msg += "\r\n";
        }
        else
        {
            // multipart/mixed outer
            // If we also have HTML, wrap text+html in a multipart/alternative inner part
            if (hasAttach && hasAlt)
            {
                msg += "--" + boundary + "\r\n";
                msg += "Content-Type: multipart/alternative; boundary=\"" + altBoundary + "\"\r\n\r\n";
                // plain
                msg += "--" + altBoundary + "\r\n";
                msg += "Content-Type: text/plain; charset=UTF-8\r\n";
                msg += "Content-Transfer-Encoding: quoted-printable\r\n\r\n";
                msg += quotedPrintableEncode(spec.body) + "\r\n";
                // html
                msg += "--" + altBoundary + "\r\n";
                msg += "Content-Type: text/html; charset=UTF-8\r\n";
                msg += "Content-Transfer-Encoding: quoted-printable\r\n\r\n";
                msg += quotedPrintableEncode(spec.htmlBody) + "\r\n";
                msg += "--" + altBoundary + "--\r\n";
            }
            else if (hasAlt)
            {
                // multipart/alternative — plain + html
                msg += "--" + boundary + "\r\n";
                msg += "Content-Type: text/plain; charset=UTF-8\r\n";
                msg += "Content-Transfer-Encoding: quoted-printable\r\n\r\n";
                msg += quotedPrintableEncode(spec.body) + "\r\n";
                msg += "--" + boundary + "\r\n";
                msg += "Content-Type: text/html; charset=UTF-8\r\n";
                msg += "Content-Transfer-Encoding: quoted-printable\r\n\r\n";
                msg += quotedPrintableEncode(spec.htmlBody) + "\r\n";
            }
            else
            {
                // just plain text part
                msg += "--" + boundary + "\r\n";
                msg += "Content-Type: text/plain; charset=UTF-8\r\n";
                msg += "Content-Transfer-Encoding: quoted-printable\r\n\r\n";
                msg += quotedPrintableEncode(spec.body) + "\r\n";
            }
            // attachments
            for (auto &[fname, ct, data] : spec.attachments)
            {
                msg += "--" + boundary + "\r\n";
                msg += "Content-Type: " + ct + "; name=\"" + fname + "\"\r\n";
                msg += "Content-Transfer-Encoding: base64\r\n";
                msg += "Content-Disposition: attachment; filename=\"" + fname + "\"\r\n\r\n";
                msg += base64Encode(data) + "\r\n";
            }
            msg += "--" + boundary + "--\r\n";
        }

        return msg;
    }

    // --------------------------------------------------------------------------
    //  Tiny SMTP client over a raw TCP socket
    //  Returns {ok, log, response}
    // --------------------------------------------------------------------------
    struct SmtpResult
    {
        bool ok;
        std::string log;
        std::string lastResponse;
    };

    struct SmtpSession
    {
        std::string host;
        int port = 587;
        std::string username;
        std::string password;
        bool useTLS = false; // true = port 465 implicit TLS (not supported without OpenSSL)
        bool useSTARTTLS = true;
        int timeoutMs = 10000;
    };

    // We send raw SMTP over TCP using your existing NovaSocket
    // (no TLS — add OpenSSL wrapping to _smtpConnect for SSL support)
    static SmtpResult smtpSend(const SmtpSession &sess,
                               const std::string &from,
                               const std::vector<std::string> &rcpts,
                               const std::string &rawMsg)
    {
        SmtpResult result{false, "", ""};

        novac::sock_platform_init();
        // resolve host
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        std::string portStr = std::to_string(sess.port);
        if (getaddrinfo(sess.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        {
            result.log = "DNS resolution failed for: " + sess.host;
            return result;
        }

        sock_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd == SOCK_INVALID)
        {
            freeaddrinfo(res);
            result.log = "socket() failed";
            return result;
        }
        if (::connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0)
        {
            sock_close(fd);
            freeaddrinfo(res);
            result.log = "connect() failed to " + sess.host + ":" + portStr;
            return result;
        }
        freeaddrinfo(res);

        novac::NovaSocket sock(fd);
        std::string log;

        auto recv = [&]() -> std::string
        {
            std::string resp;
            char buf[4096];
            // read until we get a complete response (last line has no '-' continuation)
            while (true)
            {
                int n = ::recv(fd, buf, sizeof(buf) - 1, 0);
                if (n <= 0)
                    break;
                buf[n] = '\0';
                resp += buf;
                // SMTP multi-line: if last non-empty line has form "XYZ " (space, not dash), done
                size_t last = resp.rfind('\n', resp.size() - 2);
                std::string lastLine = last == std::string::npos ? resp : resp.substr(last + 1);
                if (lastLine.size() >= 4 && lastLine[3] == ' ')
                    break;
            }
            log += "S: " + resp;
            return resp;
        };

        auto send = [&](const std::string &line)
        {
            std::string full = line + "\r\n";
            log += "C: " + full;
            sock.sendAll(full.c_str(), (int)full.size());
        };

        auto code = [](const std::string &r) -> int
        {
            if (r.size() < 3)
                return 0;
            try
            {
                return std::stoi(r.substr(0, 3));
            }
            catch (...)
            {
                return 0;
            }
        };

        // greeting
        std::string r = recv();
        if (code(r) != 220)
        {
            result.log = log;
            result.lastResponse = r;
            return result;
        }

        // EHLO
        send("EHLO novac");
        r = recv();
        if (code(r) != 250)
        {
            result.log = log;
            result.lastResponse = r;
            return result;
        }

        // AUTH LOGIN
        if (!sess.username.empty())
        {
            send("AUTH LOGIN");
            r = recv();
            if (code(r) != 334)
            {
                result.log = log;
                result.lastResponse = r;
                return result;
            }
            send(base64Encode(sess.username));
            r = recv();
            if (code(r) != 334)
            {
                result.log = log;
                result.lastResponse = r;
                return result;
            }
            send(base64Encode(sess.password));
            r = recv();
            if (code(r) != 235)
            {
                result.log = log + "\n[AUTH FAILED]";
                result.lastResponse = r;
                return result;
            }
        }

        // MAIL FROM
        send("MAIL FROM:<" + from + ">");
        r = recv();
        if (code(r) != 250)
        {
            result.log = log;
            result.lastResponse = r;
            return result;
        }

        // RCPT TO
        for (auto &rcpt : rcpts)
        {
            send("RCPT TO:<" + rcpt + ">");
            r = recv();
            if (code(r) != 250 && code(r) != 251)
            {
                result.log = log;
                result.lastResponse = r;
                return result;
            }
        }

        // DATA
        send("DATA");
        r = recv();
        if (code(r) != 354)
        {
            result.log = log;
            result.lastResponse = r;
            return result;
        }

        // send the message (dot-stuff any lines starting with '.')
        std::string stuffed;
        {
            std::istringstream ss(rawMsg);
            std::string line;
            while (std::getline(ss, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty() && line.front() == '.')
                    stuffed += '.';
                stuffed += line + "\r\n";
            }
        }
        stuffed += ".\r\n";
        log += "C: [DATA " + std::to_string(stuffed.size()) + " bytes]\r\n";
        sock.sendAll(stuffed.c_str(), (int)stuffed.size());

        r = recv();
        if (code(r) != 250)
        {
            result.log = log;
            result.lastResponse = r;
            return result;
        }

        send("QUIT");
        recv();
        sock.close();

        result.ok = true;
        result.log = log;
        result.lastResponse = r;
        return result;
    }

    // --------------------------------------------------------------------------
    //  Convert ParsedEmail → Nova object
    // --------------------------------------------------------------------------
    static Val parsedToVal(const ParsedEmail &e)
    {
        auto obj = nova_obj();
        obj->obj->set("messageId", nova_str(e.messageId));
        obj->obj->set("date", nova_str(e.date));
        obj->obj->set("fromName", nova_str(e.fromName));
        obj->obj->set("fromAddr", nova_str(e.fromAddr));
        obj->obj->set("from", nova_str(formatAddress(e.fromName, e.fromAddr)));
        obj->obj->set("subject", nova_str(e.subject));
        obj->obj->set("body", nova_str(e.body));
        obj->obj->set("htmlBody", nova_str(e.htmlBody));
        obj->obj->set("replyTo", nova_str(e.replyTo));
        obj->obj->set("inReplyTo", nova_str(e.inReplyTo));
        obj->obj->set("references", nova_str(e.references));
        obj->obj->set("mimeVersion", nova_str(e.mimeVersion));
        obj->obj->set("contentType", nova_str(e.contentType));
        obj->obj->set("isMultipart", nova_bool(e.isMultipart));
        obj->obj->set("boundary", nova_str(e.boundary));

        // to / cc arrays
        auto mkAddrArr = [](const std::vector<std::pair<std::string, std::string>> &list) -> Val
        {
            ValVec arr;
            for (auto &[n, a] : list)
            {
                auto o = nova_obj();
                o->obj->set("name", nova_str(n));
                o->obj->set("addr", nova_str(a));
                o->obj->set("full", nova_str(formatAddress(n, a)));
                arr.push_back(o);
            }
            return nova_arr(arr);
        };
        obj->obj->set("to", mkAddrArr(e.to));
        obj->obj->set("cc", mkAddrArr(e.cc));
        obj->obj->set("bcc", mkAddrArr(e.bcc));

        // raw headers object
        auto hdrs = nova_obj();
        for (auto &[k, v] : e.headers)
            hdrs->obj->set(k, nova_str(v));
        obj->obj->set("headers", hdrs);

        // attachments
        ValVec attArr;
        for (auto &[fname, ct, data] : e.attachments)
        {
            auto a = nova_obj();
            a->obj->set("filename", nova_str(fname));
            a->obj->set("contentType", nova_str(ct));
            a->obj->set("size", nova_num((double)data.size()));
            a->obj->set("data", nova_str(data));
            a->obj->set("dataBase64", nova_str(base64Encode(data)));
            attArr.push_back(a);
        }
        obj->obj->set("attachments", nova_arr(attArr));
        obj->obj->set("attachmentCount", nova_num((double)attArr.size()));

        return obj;
    }

    // --------------------------------------------------------------------------
    //  Build EmailSpec from a Nova object
    // --------------------------------------------------------------------------
    static EmailSpec specFromVal(Val v)
    {
        EmailSpec s;
        if (!v || !v.isObject())
            return s;

        auto str = [&](const std::string &k) -> std::string
        {
            Val x = v->obj->get(k);
            return x ? x.asString() : "";
        };
        auto parseAddrList = [](Val x) -> std::vector<std::pair<std::string, std::string>>
        {
            std::vector<std::pair<std::string, std::string>> out;
            if (!x)
                return out;
            if (x.isString())
            {
                out.push_back(parseAddress(x.asString()));
                return out;
            }
            if (x.isArray())
            {
                for (auto &item : x->arr->inner)
                {
                    if (item.isString())
                    {
                        out.push_back(parseAddress(item.asString()));
                    }
                    else if (item.isObject())
                    {
                        std::string n = item->obj->get("name") ? item->obj->get("name").asString() : "";
                        std::string a = item->obj->get("addr") ? item->obj->get("addr").asString() : "";
                        out.push_back({n, a});
                    }
                }
            }
            return out;
        };

        // from: "Name <addr>" or {name, addr}
        Val fromV = v->obj->get("from");
        if (fromV && fromV.isString())
        {
            auto [n, a] = parseAddress(fromV.asString());
            s.fromName = n;
            s.fromAddr = a;
        }
        else if (fromV && fromV.isObject())
        {
            s.fromName = fromV->obj->get("name") ? fromV->obj->get("name").asString() : "";
            s.fromAddr = fromV->obj->get("addr") ? fromV->obj->get("addr").asString() : "";
        }

        s.to = parseAddrList(v->obj->get("to"));
        s.cc = parseAddrList(v->obj->get("cc"));
        s.bcc = parseAddrList(v->obj->get("bcc"));
        s.subject = str("subject");
        s.body = str("body");
        s.htmlBody = str("htmlBody");
        s.replyTo = str("replyTo");
        s.messageId = str("messageId");
        s.date = str("date");
        s.inReplyTo = str("inReplyTo");
        s.references = str("references");

        Val enc = v->obj->get("encoding");
        if (enc && enc.isString())
            s.encoding = enc.asString();

        // extra headers
        Val xh = v->obj->get("headers");
        if (xh && xh.isObject())
            for (auto &[k, hv] : xh->obj->inner)
                s.extraHeaders[k] = hv.asString();

        // attachments: [{filename, contentType, data}]
        Val atts = v->obj->get("attachments");
        if (atts && atts.isArray())
        {
            for (auto &a : atts->arr->inner)
            {
                if (!a.isObject())
                    continue;
                std::string fname = a->obj->get("filename") ? a->obj->get("filename").asString() : "file.bin";
                std::string ct = a->obj->get("contentType") ? a->obj->get("contentType").asString() : "application/octet-stream";
                std::string data;
                Val dv = a->obj->get("data");
                Val db = a->obj->get("dataBase64");
                if (dv)
                    data = dv.asString();
                else if (db)
                    data = base64Decode(db.asString());
                s.attachments.push_back({fname, ct, data});
            }
        }

        return s;
    }

} // namespace nova_email_detail

// ════════════════════════════════════════════════════════════════════════════
//  Minimal HTTP helpers (anonymous namespace — file-local)
// ════════════════════════════════════════════════════════════════════════════
namespace
{
    std::string nova_http_urlDecode(const std::string &s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == '%' && i + 2 < s.size() &&
                std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2]))
            {
                auto hexVal = [](char c) -> int
                {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    return std::tolower((unsigned char)c) - 'a' + 10;
                };
                out += (char)((hexVal(s[i + 1]) << 4) | hexVal(s[i + 2]));
                i += 2;
            }
            else if (s[i] == '+')
                out += ' ';
            else
                out += s[i];
        }
        return out;
    }

    std::vector<std::string> nova_http_splitPath(const std::string &path)
    {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : path)
        {
            if (c == '/')
            {
                if (!cur.empty())
                {
                    parts.push_back(cur);
                    cur.clear();
                }
            }
            else
                cur += c;
        }
        if (!cur.empty())
            parts.push_back(cur);
        return parts;
    }
}

namespace novac
{

    std::string BytesToChar(const ValVec &bytes)
    {
        std::string result;
        result.reserve(bytes.size());

        for (Val b : bytes)
            result.push_back(static_cast<char>(b->asNumber()));

        return result;
    }

    namespace
    {
        // Build a raw_ptr Val wrapping an arbitrary address. No backing buffer
        // is owned here (rawBuffer left null) since the memory belongs to the
        // OS/library, not us.
        Val makeRawPtrFromAddress(void *addr, NativeType t, size_t size = 0)
        {
            auto ptr = std::make_shared<NovaPointer>();
            ptr->isRaw = true;
            ptr->rawType = t;
            ptr->rawAddr = addr;
            ptr->rawSize = size;

            std::ostringstream ss;
            ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(addr);
            ptr->address = ss.str();

            auto out = std::make_shared<NovaValue>();
            out->kind = VK::Pointer;
            out->ptr = ptr;
            return TVal(out);
        }

        void *rawAddrOf(const Val &v)
        {
            if (!v || !v.isPointer() || !v->ptr || !v->ptr->isRaw)
                return nullptr;
            return v->ptr->rawAddr;
        }

        // ── async task driving ──────────────────────────────────────────────
        // Drives a `__async__` task object (the {__drive__, __isDone__, ...}
        // protocol produced by _runAsync / Chrono.asleep / etc.) to completion.
        //
        // This is the single place that correctly implements the suspend/
        // resume contract those task objects rely on: when a task is still
        // suspended, whatever it most recently yielded (`__drive__`'s return
        // value) might itself be ANOTHER unfinished async task — that's
        // exactly what happens when an async fn does `await someOtherAsyncFn()`
        // (see _runAsync's injected __await__, which yields the nested task
        // and suspends, expecting to be resumed with that nested task's real
        // result). The naive approach of resuming in a loop with a constant
        // `nova_null()` ignores that yielded nested task entirely, so the
        // inner await silently resolves to null. This function instead
        // recognizes a yielded nested task and recursively drives IT to
        // completion first, then feeds its real result back in as the resume
        // value — which is what makes nested `await` chains actually work.
        //
        // Used by both the top-level `await` fallback and by task.then()/
        // task.catch(), so the fix applies uniformly everywhere a task is
        // driven from outside its own fiber.
        Val driveAsyncTask(Executor *exe, Val task, std::shared_ptr<Scope> s)
        {
            if (!task || !task.isObject())
                return task;

            Val driveFn = task->obj->get("__drive__");
            if (!driveFn || !driveFn.isFunction())
            {
                Val result = task->obj->get("__result__");
                return result ? result : nova_null();
            }

            Val isDoneFn = task->obj->get("__isDone__");
            auto checkDone = [&]() -> bool
            {
                if (isDoneFn && isDoneFn.isFunction())
                    return exe->callFunction(isDoneFn, {}, s).asBool();
                Val d = task->obj->get("__done__");
                return d && d.asBool();
            };

            // Seed from __result__ only when the task is already done —
            // if it's still pending, __result__ is null (not the final value)
            // and we must enter the drive loop to get the real result.
            Val resumeWith = nova_null();
            Val result = checkDone() ? task->obj->get("__result__") : nova_null();
            if (!result)
                result = nova_null();

            while (!checkDone())
            {
                result = exe->callFunction(driveFn, {resumeWith}, s);
                resumeWith = nova_null();

                if (checkDone())
                    break;

                // Still suspended — if what it yielded is itself an
                // unfinished nested async task, resolve that one fully
                // before resuming us with the real value.
                if (result && result.isObject())
                {
                    Val nestedAsync = result->obj->get("__async__");
                    if (nestedAsync && nestedAsync.asBool())
                        resumeWith = driveAsyncTask(exe, result, s);
                }
            }
            return result ? result : nova_null();
        }

#if NOVA_FFI_PLATFORM_SUPPORTED
        // Shared symbol-lookup logic used by both handle.sym(name) and the
        // free-function Std.Experimental.Dlsym(handle, name).
        Val dlsymImpl(void *handle, const std::string &name)
        {
#ifdef _WIN32
            void *fn = (void *)GetProcAddress((HMODULE)handle, name.c_str());
#else
            ::dlerror(); // clear any pending error
            void *fn = ::dlsym(handle, name.c_str());
            if (!fn && ::dlerror() != nullptr)
                return nova_null();
#endif
            if (!fn)
                return nova_null();
            return makeRawPtrFromAddress(fn, NativeType::Ptr, 0);
        }
#endif

#if defined(_WIN32) && NOVA_CCALL_AVAILABLE
        // ────────────────────────────────────────────────────────────────────────
        //  Std.Windows.COM.MakeVTable support — a runtime-synthesized COM object.
        //
        //  A COM object IS a pointer to a vtable pointer, so NovaVTableObject's
        //  `vtbl` member must come first: `self` (the NovaVTableObject*) doubles
        //  as the "this" pointer any COM caller sees. Each user-declared method
        //  gets a real, executable trampoline built with libffi's ffi_closure,
        //  so native code calling INTO this object (WebView2 completion
        //  handlers, custom connection-point sinks, anything expecting a
        //  vtable) works exactly as if this were a compiled C++ COM object.
        // ────────────────────────────────────────────────────────────────────────
        struct ClosureCtx;
        struct NovaVTableObject
        {
            void **vtbl; // MUST be first member
            std::atomic<long> refCount{1};
            Executor *exe = nullptr;
            std::shared_ptr<Scope> scope;
            std::vector<Val> handlers;                           // one Nova fn per user method
            std::vector<void *> closureCodePtrs;                 // executable trampolines
            std::vector<ffi_closure *> closures;                 // for cleanup
            std::vector<void *> vtableSlots;                     // QI, AddRef, Release, method0, ...
            std::vector<std::vector<ffi_type *>> argTypeStorage; // keep ffi_cif inputs alive
            std::vector<ffi_cif> cifs;
            std::vector<ClosureCtx *> closureContexts; // owned, freed on destroy
        };

        struct ClosureCtx
        {
            NovaVTableObject *obj;
            size_t methodIndex;
            std::string retType;
            std::vector<std::string> argTypeNames;
        };

        // Fixed IUnknown methods — every synthesized COM object needs these
        // regardless of which higher-level interface it's impersonating. Real
        // function pointers (not lambdas/std::function) since they sit
        // directly in the vtable and get called by native COM machinery.
        long __stdcall NovaVT_AddRef(NovaVTableObject *self)
        {
            return ++self->refCount;
        }

        long __stdcall NovaVT_Release(NovaVTableObject *self)
        {
            long r = --self->refCount;
            if (r == 0)
            {
                for (auto *c : self->closures)
                    if (c)
                        ffi_closure_free(c);
                for (auto *ctx : self->closureContexts)
                    delete ctx;
                delete self;
            }
            return r;
        }

        // NOTE: always succeeds and hands back this same object/vtable,
        // regardless of the requested IID. That's correct for single-
        // interface callback objects (WebView2's environment-/controller-
        // created handlers, a lone connection-point sink) but not for
        // objects that must answer multiple distinct IIDs with distinct
        // vtables — extend this if that's ever needed.
        long __stdcall NovaVT_QueryInterface(NovaVTableObject *self, void *riid, void **ppv)
        {
            (void)riid;
            if (!ppv)
                return -2147467261; // E_POINTER
            *ppv = self;
            NovaVT_AddRef(self);
            return 0; // S_OK
        }

        ffi_type *novaVTableFfiTypeForName(const std::string &t)
        {
            if (t == "i32" || t == "hresult")
                return &ffi_type_sint32;
            if (t == "u32")
                return &ffi_type_uint32;
            if (t == "i64")
                return &ffi_type_sint64;
            if (t == "u64")
                return &ffi_type_uint64;
            if (t == "f32")
                return &ffi_type_float;
            if (t == "f64")
                return &ffi_type_double;
            if (t == "bool")
                return &ffi_type_uint8;
            return &ffi_type_pointer; // ptr, cstr
        }

        Val novaVTableArgToVal(void *argPtr, const std::string &t)
        {
            if (t == "i32" || t == "hresult")
                return nova_num((double)*(int32_t *)argPtr);
            if (t == "u32")
                return nova_num((double)*(uint32_t *)argPtr);
            if (t == "i64")
                return nova_num((double)*(int64_t *)argPtr);
            if (t == "u64")
                return nova_num((double)*(uint64_t *)argPtr);
            if (t == "f64")
                return nova_num(*(double *)argPtr);
            if (t == "f32")
                return nova_num((double)*(float *)argPtr);
            if (t == "bool")
                return nova_bool(*(uint8_t *)argPtr != 0);
            if (t == "ptr")
                return makeRawPtrFromAddress(*(void **)argPtr, NativeType::Ptr, 0);
            if (t == "cstr")
            {
                const char *s = *(const char **)argPtr;
                return s ? nova_str(s) : nova_null();
            }
            return nova_num(0);
        }

        // libffi's closure callback — invoked whenever native code calls one
        // of the synthesized user-method trampolines. Runs synchronously on
        // whatever thread the native caller used (e.g. WebView2's internal
        // thread), so Nova handlers registered here must not block.
        void novaVTableClosureHandler(ffi_cif * /*cif*/, void *ret, void **args, void *userData)
        {
            auto *ctx = (ClosureCtx *)userData;

            // args[0] is "this" (the NovaVTableObject*) — skip it, pass the rest to Nova
            ValVec novaArgs;
            for (size_t i = 0; i < ctx->argTypeNames.size(); i++)
                novaArgs.push_back(novaVTableArgToVal(args[i + 1], ctx->argTypeNames[i]));

            Val result = nova_null();
            try
            {
                result = ctx->obj->exe->callFunction(ctx->obj->handlers[ctx->methodIndex], novaArgs, ctx->obj->scope);
            }
            catch (...)
            {
                // swallow — an exception crossing back into the native caller's
                // stack (e.g. WebView2's internal thread) would be undefined behavior
            }

            if (ctx->retType == "void")
                return;
            if (ctx->retType == "i32" || ctx->retType == "hresult")
                *(int32_t *)ret = result ? (int32_t)result.asNumber() : 0;
            else if (ctx->retType == "u32")
                *(uint32_t *)ret = result ? (uint32_t)result.asNumber() : 0;
            else if (ctx->retType == "bool")
                *(uint8_t *)ret = (result && result.asBool()) ? 1 : 0;
            else if (ctx->retType == "f32")
                *(float *)ret = result ? (float)result.asNumber() : 0.0f;
            else if (ctx->retType == "f64")
                *(double *)ret = result ? result.asNumber() : 0.0;
            else if (ctx->retType == "ptr")
                *(void **)ret = (result && result.isPointer() && result->ptr) ? result->ptr->rawAddr : nullptr;
            else
                *(int64_t *)ret = result ? (int64_t)result.asNumber() : 0;
        }
#endif // defined(_WIN32) && NOVA_CCALL_AVAILABLE
    }

    namespace fs = std::filesystem;

    // ════════════════════════════════════════════════════════════════════════════
    //  Construction
    // ════════════════════════════════════════════════════════════════════════════

    Executor::Executor(std::string src, std::string fname)
        : source(std::move(src)), filename(std::move(fname))
    {
        globalScope = std::make_shared<Scope>(Scope::Kind::Global, nullptr, nullptr);
        globalScope->globalScope = globalScope;
        _setupGlobalScope();
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  run  —  execute a full program AST
    // ════════════════════════════════════════════════════════════════════════════

    Val Executor::run(Node *ast, std::shared_ptr<Scope> scope)
    {
        if (!ast)
            return nova_null();
        auto s = scope ? scope : globalScope;
        Val last = nova_null();
        try
        {
            for (auto &child : ast->body)
            {
                if (!child)
                    continue;
                ExecResult r = execute(child.get(), s);
                last = r.value;
                // A top-level return/break/continue halts the remaining
                // top-level statements (mirrors the old ReturnSignal catch —
                // break/continue at top level are erroneous programs anyway,
                // so we just stop gracefully rather than keep going).
                if (!r.isNormal())
                    break;
            }
        }
        catch (ReturnSignal &r)
        {
            // Safety net for `yield` used outside any generator (see
            // evaluate()'s Yield case) — that one path still throws since
            // it's a rare fallback, not worth threading through every
            // expression evaluator. Everything else above is exception-free.
            last = r.value;
        }
        return last;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  execute  —  statement dispatch
    // ════════════════════════════════════════════════════════════════════════════

    ExecResult Executor::execute(const Node *n, std::shared_ptr<Scope> s)
    {
        if (!n)
            return ExecResult::Val_(nova_null());
        _currentLine = n->line;
        _currentCol = n->column;

        switch (n->kind)
        {
        case Node::Kind::Program:
            // A nested Program node is its own return-absorbing boundary,
            // same as the top-level run() — matches prior behavior exactly.
            return ExecResult::Val_(run(const_cast<Node *>(n), s));

        case Node::Kind::Exec:
            return ExecResult::Val_(n->value ? evaluate(n->value.get(), s) : nova_null());

        case Node::Kind::Declare:
            return _execDeclare(n, s);
        case Node::Kind::Function:
            return _execFunction(n, s);
        case Node::Kind::Class:
            return _execClass(n, s);
        case Node::Kind::Branch:
            return _execBranch(n, s);
        case Node::Kind::ForOf:
            return _execForOf(n, s);
        case Node::Kind::ForIn:
            return _execForIn(n, s);
        case Node::Kind::Each:
            return _execEach(n, s);
        case Node::Kind::Switch:
            return _execSwitch(n, s);
        case Node::Kind::Match:
            return _execMatch(n, s);
        case Node::Kind::Try:
            return _execTry(n, s);
        case Node::Kind::Return:
            return _execReturn(n, s);
        case Node::Kind::Throw:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
            throw ThrowSignal{v};
        }
        case Node::Kind::Break:
            return ExecResult::Broke();
        case Node::Kind::Continue:
            return ExecResult::Continued();
        case Node::Kind::Goback:
            return ExecResult::Goback();
        case Node::Kind::Yield:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
            Val yieldFn = s->get("yield");
            if (yieldFn && yieldFn.isFunction())
                return ExecResult::Val_(callFunction(yieldFn, {v}, s));
            throw ReturnSignal{v, false}; // fallback: not inside a generator
        }
        case Node::Kind::EmitEvent:
            return _execEmit(n, s);
        case Node::Kind::OnEvent:
            return _execOn(n, s);
        case Node::Kind::WithCtx:
        case Node::Kind::WithOption:
            return _execWith(n, s);
        case Node::Kind::Import:
        case Node::Kind::ImportBuiltin:
        case Node::Kind::ImportKit:
        case Node::Kind::FromImport:
            return _execImport(n, s);
        case Node::Kind::Export:
        case Node::Kind::DefaultExport:
            return _execExport(n, s);
        case Node::Kind::Namespace:
            return _execNamespace(n, s);
        case Node::Kind::Server:
            return _execServer(n, s);
        case Node::Kind::TypeDecl:
            return _execTypeDecl(n, s);
        case Node::Kind::StructDecl:
            return _execStructDecl(n, s);
        case Node::Kind::EnumDecl:
            return _execEnumDecl(n, s);
        case Node::Kind::InterfaceDecl:
            return _execInterfaceDecl(n, s);
        case Node::Kind::TraitDecl:
            return _execTraitDecl(n, s);
        case Node::Kind::ImplDecl:
            return _execImplDecl(n, s);
        case Node::Kind::Block:
        {
            auto bs = s->child(Scope::Kind::Block);
            return _runBody(n->body, bs);
        }
        case Node::Kind::ExecComment:
            // Inline executable comment — parse and run as Nova
            if (!n->strval.empty())
            {
                Parser p(n->strval, filename);
                auto ast = p.parse();
                return ExecResult::Val_(run(ast.get(), s));
            }
            return ExecResult::Val_(nova_null());

        case Node::Kind::DotCmd:
        {
            // .command args... — syntactic sugar for function call
            if (!n->cmd)
                return ExecResult::Val_(nova_null());
            Val fn = s->get(n->cmd->strval);
            if (!fn || !fn.isFunction())
                _error("Unknown command: " + n->cmd->strval, n);
            ValVec args;
            for (auto &a : n->callArgs)
                args.push_back(a ? evaluate(a.get(), s) : nova_null());
            return ExecResult::Val_(callFunction(fn, args, s));
        }

        case Node::Kind::FetchStmt:
        {
            Val url = n->url ? evaluate(n->url.get(), s) : nova_null();
            Val opts = n->options ? evaluate(n->options.get(), s) : nullptr;
            Val res = _syncFetch(url.asString(), opts);
            if (!n->varNameOut.empty())
                s->set(n->varNameOut, res);
            return ExecResult::Val_(res);
        }

        default:
            // Treat as expression statement
            return ExecResult::Val_(evaluate(n, s));
        }
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  _runBody  —  run a list of statements, propagating signals
    // ════════════════════════════════════════════════════════════════════════════

    ExecResult Executor::_runBody(const NodeList &body, std::shared_ptr<Scope> s)
    {
        ExecResult last;
        for (auto &stmt : body)
        {
            if (!stmt)
                continue;
            last = execute(stmt.get(), s);
            if (!last.isNormal())
                return last; // return/break/continue — stop and propagate immediately
        }
        return last;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  STATEMENT HANDLERS
    // ════════════════════════════════════════════════════════════════════════════

    // ── declare ───────────────────────────────────────────────────────────────────

    ExecResult Executor::_execDeclare(const Node *n, std::shared_ptr<Scope> s)
    {
        // Destructure patterns
        if (std::holds_alternative<ObjPattern>(n->destructure))
        {
            Val rhs = n->value ? evaluate(n->value.get(), s) : nova_null();
            _destructureObj(std::get<ObjPattern>(n->destructure), rhs, s, n->isConst);
            return ExecResult::Val_(rhs);
        }
        if (std::holds_alternative<ArrPattern>(n->destructure))
        {
            Val rhs = n->value ? evaluate(n->value.get(), s) : nova_null();
            _destructureArr(std::get<ArrPattern>(n->destructure), rhs, s, n->isConst);
            return ExecResult::Val_(rhs);
        }

        // Normal declaration
        Val value = nova_null();
        if (n->value)
            value = evaluate(n->value.get(), s);
        // Smart variable modifiers
        if (n->modifiers)
        {
            auto &mods = *n->modifiers;
            VarDesc desc;
            desc.raw = value;
            desc.isConst = n->isConst;

            if (mods.frozen)
            {
                Val frozen = value;
                desc.read = [frozen]()
                { return frozen; };
                desc.write = [n, this](Val)
                { _error("Cannot modify frozen variable '" + n->name + "'", n); };
                desc.hasHooks = true;
            }
            if (mods.once)
            {
                bool written = (value && !value.isNull());
                auto storage = std::make_shared<Val>(value);
                auto flag = std::make_shared<bool>(written);
                desc.read = [storage]()
                { return *storage; };
                desc.write = [storage, flag, n, this](Val v)
                {
                    if (*flag)
                        _error("'once' variable '" + n->name + "' already set", n);
                    *storage = v;
                    *flag = true;
                };
                desc.hasHooks = true;
            }
            if (mods.tracked)
            {
                auto storage = std::make_shared<Val>(value);
                auto history = std::make_shared<ValVec>();
                desc.read = [storage]()
                { return *storage; };
                desc.write = [storage, history](Val v)
                { history->push_back(*storage); *storage = v; };
                // expose .history as a method on the var via a wrapper object
                // (simplified: just track mutations internally)
                desc.hasHooks = true;
            }
            if (mods.nonull)
            {
                auto storage = std::make_shared<Val>(value);
                desc.read = [storage]()
                { return *storage; };
                desc.write = [storage, n, this](Val v)
                {
                    if (!v || v.isNull())
                        _error("'nonull' variable '" + n->name + "' cannot be null", n);
                    *storage = v;
                };
                desc.hasHooks = true;
            }
            if (mods.lazy && n->value)
            {
                auto nodeRef = n->value.get();
                auto scopeRef = s;
                auto exe = this;
                auto computed = std::make_shared<bool>(false);
                auto cache = std::make_shared<Val>(nova_null());
                desc.read = [computed, cache, nodeRef, scopeRef, exe]()
                {
                    if (!*computed)
                    {
                        *cache = exe->evaluate(nodeRef, scopeRef);
                        *computed = true;
                    }
                    return *cache;
                };
                desc.write = [cache](Val v)
                { *cache = v; };
                desc.hasHooks = true;
            }
            if (mods.setter)
            {
                auto setFn = evaluate(mods.setter.get(), s);
                auto storage = std::make_shared<Val>(value);
                auto fn = setFn;
                desc.write = [this, fn, storage, s](Val v)
                {
                    ValVec args = {v};
                    *storage = callFunction(fn, args, s);
                };
                desc.read = [storage]()
                { return *storage; };
                desc.hasHooks = true;
            }
            if (mods.getter)
            {
                auto getFn = evaluate(mods.getter.get(), s);
                auto fn = getFn;
                auto storage = std::make_shared<Val>(value);
                desc.read = [this, fn, storage, s]()
                {
                    return callFunction(fn, {*storage}, s);
                };
                desc.hasHooks = true;
            }

            s->setDescriptor(n->name, std::move(desc));
        }
        else
        {
            // Wrap in pointer if declared with *
            if (n->isPointer)
            {
                Val ptrVal;
                if (value && value.isPointer())
                {
                    // share the same pointer — aliasing
                    ptrVal = value;
                }
                else
                {
                    auto storage = std::make_shared<Val>(value);
                    std::ostringstream addr;
                    addr << "0x" << std::hex << (uintptr_t)storage.get();

                    auto ptr = std::make_shared<NovaPointer>();
                    ptr->address = addr.str();
                    ptr->readFn = [storage]() -> Val
                    { return *storage; };
                    ptr->writeFn = [storage](Val v)
                    { *storage = v; };

                    ptrVal = std::make_shared<NovaValue>();
                    ptrVal->kind = VK::Pointer;
                    ptrVal->ptr = ptr;
                }
                s->setOwn(n->name, ptrVal, n->isConst);
            }
            else
            {

                if (n->explicitType)
                {
                    if (value && !value.isNull() && !types.check(value, *n->explicitType))
                    {
                        _error("Type mismatch for '" + n->name + "'", n);
                    }
                    VarDesc desc;
                    desc.raw = value;
                    desc.isConst = n->isConst;
                    desc.explicitType = n->explicitType; // std::optional<TypeExpr> copy
                    auto typeCopy = *n->explicitType;    // TypeExpr copy for lambda
                    auto nameCopy = n->name;
                    auto exe = this;
                    desc.write = [exe, typeCopy, nameCopy](Val v)
                    {
                        if (v && !v.isNull() && !exe->types.check(v, typeCopy))
                            exe->_error("Type mismatch: cannot assign incompatible type to '" + nameCopy + "'");
                        // NOTE: raw is updated by Scope::set after this hook returns
                    };
                    desc.hasHooks = true;
                    s->setDescriptor(n->name, std::move(desc));
                }
                else
                {
                    s->setOwn(n->name, value, n->isConst);
                }
            }
        }

        return ExecResult::Val_(value);
    }

    // ── function declaration ──────────────────────────────────────────────────────

    ExecResult Executor::_execFunction(const Node *n, std::shared_ptr<Scope> s)
    {
        auto fn = NovaValue::makeFunction(
            std::make_shared<Node>(*n), s);
        fn->fn->name = n->name;
        fn->fn->isAsync = n->isAsync;
        fn->fn->isGenerator = n->isGenerator;
        fn->fn->strictArgs = n->strictArgs;
        fn->fn->memoize = n->memoize;
        fn->fn->once = n->once_fn;
        if (n->memoize)
            fn->fn->memoCache = std::make_shared<std::unordered_map<std::string, Val>>();

        if (!n->name.empty())
            s->setOwn(n->name, fn);
        return ExecResult::Val_(fn);
    }

    // ── class declaration ─────────────────────────────────────────────────────────
    ExecResult Executor::_execClass(const Node *n, std::shared_ptr<Scope> s)
    {
        auto cls = std::make_shared<NovaClass>();
        cls->name = n->name;
        cls->node = std::make_shared<Node>(*n);
        cls->closureScope = s;

        if (n->superClass)
            cls->superClass = evaluate(n->superClass.get(), s);

        for (auto &m : n->members)
        {
            auto methodNode = std::make_shared<Node>();
            methodNode->kind = Node::Kind::Function;
            methodNode->name = m.name;
            methodNode->funcArgs = m.args;
            for (const auto &b : m.body)
                methodNode->body.push_back(b);

            if (m.kind == ClassMember::Kind::Method)
            {
                Val mfn = NovaValue::makeFunction(methodNode, s);
                mfn->fn->name = m.name;
                cls->methods[m.name] = mfn;
            }
            else if (m.kind == ClassMember::Kind::Field && m.value)
            {
                cls->methods[m.name] = evaluate(m.value.get(), s);
            }
        }

        Val v = NovaValue::makeClass(cls);
        if (!n->name.empty())
        {
            classRegistry[n->name] = v;
            s->setOwn(n->name, v);
        }
        return ExecResult::Val_(v);
    }

    Val Executor::_buildSuperProxy(Val superClassVal, Val thisVal)
    {
        auto &superCls = *superClassVal->cls;
        auto superObj = nova_obj();
        auto exe = this;

        // Super.method() — every non-constructor method, bound to current 'this'
        for (auto &[k, v] : superCls.methods)
        {
            if (k == "constructor")
                continue;
            auto method = v;
            auto inst = thisVal;
            superObj->obj->set(k, NovaValue::makeNative(
                                      [exe, method, inst](ValVec args, std::shared_ptr<Scope> cs) -> Val
                                      {
                                          return exe->callFunction(method, args, cs, inst);
                                      },
                                      k));
        }

        // Super() — calls the superclass constructor on the current 'this'
        auto ctorIt = superCls.methods.find("constructor");
        if (ctorIt != superCls.methods.end())
        {
            auto superCtor = ctorIt->second;
            auto inst = thisVal;
            Val ctorFn = NovaValue::makeNative(
                [exe, superCtor, inst](ValVec args, std::shared_ptr<Scope> cs) -> Val
                {
                    return exe->callFunction(superCtor, args, cs, inst);
                },
                "Super");

            // Make superObj itself callable via the "call" overload
            if (!superObj->overloads)
                superObj->overloads = std::make_shared<ValMap>();
            (*superObj->overloads)["call"] = ctorFn;
        }

        return superObj;
    }
    // ── branch (if / while / repeat / until / unless) ────────────────────────────

    ExecResult Executor::_execBranch(const Node *n, std::shared_ptr<Scope> s)
    {
        const std::string &bt = n->branchType;
        Val last = nova_null();

        if (bt == "if" || bt == "unless")
        {
            bool cond = _isTruthy(n->cond ? evaluate(n->cond.get(), s) : nova_null());
            if (bt == "unless")
                cond = !cond;
            if (cond)
            {
                auto bs = s->child(Scope::Kind::Block);
                // Pass the body's result straight through. `if` is not a loop
                // boundary, so return/break/continue raised inside it must
                // reach the nearest enclosing loop (or function), not be
                // absorbed here.
                return _runBody(n->body, bs);
            }
            else if (n->next)
            {
                return execute(n->next.get(), s);
            }
        }
        else if (bt == "while")
        {
            for (;;)
            {
                bool cond = _isTruthy(n->cond ? evaluate(n->cond.get(), s) : nova_null());
                if (!cond)
                    break;
                auto bs = s->child(Scope::Kind::Block);
                ExecResult r = _runBody(n->body, bs);
                if (r.sig == Signal::Break)
                    break;
                if (r.sig == Signal::Return)
                    return r;
                // Signal::Continue / Signal::Normal: loop again normally.
            }
        }
        else if (bt == "until")
        {
            for (;;)
            {
                bool cond = _isTruthy(n->cond ? evaluate(n->cond.get(), s) : nova_null());
                if (cond)
                    break;
                auto bs = s->child(Scope::Kind::Block);
                ExecResult r = _runBody(n->body, bs);
                if (r.sig == Signal::Break)
                    break;
                if (r.sig == Signal::Return)
                    return r;
            }
        }
        else if (bt == "repeat")
        {
            double count = n->cond ? evaluate(n->cond.get(), s).asNumber() : 0;
            for (double i = 0; i < count; i++)
            {
                auto bs = s->child(Scope::Kind::Block);
                bs->loopIndex = (int)i;
                ExecResult r = _runBody(n->body, bs);
                if (r.sig == Signal::Break)
                    break;
                if (r.sig == Signal::Return)
                    return r;
            }
        }
        else if (bt == "do")
        {
            for (;;)
            {
                auto bs = s->child(Scope::Kind::Block);
                ExecResult r = _runBody(n->body, bs);
                if (r.sig == Signal::Break)
                    break;
                if (r.sig == Signal::Return)
                    return r;
                // Continue (and Normal) fall through to the condition check,
                // matching do-while semantics: continue still re-evaluates
                // the condition rather than unconditionally looping again.
                bool cond = _isTruthy(n->cond ? evaluate(n->cond.get(), s) : nova_null());
                if (!cond)
                    break;
            }
        }
        else if (bt == "else")
        {
            auto bs = s->child(Scope::Kind::Block);
            // Same as `if`: not a loop boundary, pass the result through.
            return _runBody(n->body, bs);
        }
        else if (bt == "for")
        {
            auto bs = s->child(Scope::Kind::Block);
            if (n->init)
                execute(n->init.get(), bs);
            for (;;)
            {
                if (n->cond && !_isTruthy(evaluate(n->cond.get(), bs)))
                    break;
                auto body_s = bs->child(Scope::Kind::Block);
                ExecResult r = _runBody(n->body, body_s);
                if (r.sig == Signal::Break)
                    break;
                if (r.sig == Signal::Return)
                    return r;
                // Continue (and Normal) still run the update expression,
                // matching C-style for-loop continue semantics.
                if (n->update)
                    execute(n->update.get(), bs);
            }
        }

        return ExecResult::Val_(last);
    }

    // ── for of ────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execForOf(const Node *n, std::shared_ptr<Scope> s)
    {

        Val iter = n->iterable ? evaluate(n->iterable.get(), s) : nova_null();

        // top of _execForOf, before the isArray() check:
        if (iter->overloads)
        {
            auto it = iter->overloads->find("iter");
            if (it != iter->overloads->end())
            {
                Val result = callFunction(it->second, {iter}, s);
                // replace iter with the returned array/range
                iter = result;
            }
        }

        auto runIter = [&](Val item, int idx) -> ExecResult
        {
            auto bs = s->child(Scope::Kind::Block);
            bs->setOwn(n->varName, item);
            if (!n->indexName.empty())
                bs->setOwn(n->indexName, nova_num(idx));
            bs->loopIndex = idx;
            return _runBody(n->body, bs);
        };

        // handleSig(): true means the calling C++ for-loop should `break`.
        // A Return signal is stashed and propagated once we leave the loop.
        ExecResult pendingReturn;
        bool returned = false;
        auto handleSig = [&](const ExecResult &r) -> bool
        {
            if (r.sig == Signal::Break)
                return true;
            if (r.sig == Signal::Return)
            {
                pendingReturn = r;
                returned = true;
                return true;
            }
            return false; // Continue / Normal — keep iterating
        };

        if (iter.isArray())
        {
            auto &arr = iter->arr->inner;
            for (int i = 0; i < (int)arr.size(); i++)
                if (handleSig(runIter(arr[i], i)))
                    break;
        }
        else if (iter.isRange())
        {
            auto elems = iter->range->toArray();
            for (int i = 0; i < (int)elems.size(); i++)
                if (handleSig(runIter(elems[i], i)))
                    break;
        }
        else if (iter.isString())
        {
            auto cps = utf8::codepoints(iter->sval);
            for (int i = 0; i < (int)cps.size(); i++)
                if (handleSig(runIter(nova_str(cps[i]), i)))
                    break;
        }
        else if (iter.isObject())
        {
            int i = 0;
            for (auto &[k, v] : iter->obj->inner)
                if (handleSig(runIter(v, i++)))
                    break;
        }

        if (returned)
            return pendingReturn;
        return ExecResult::Val_(nova_null());
    }

    // ── for in ────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execForIn(const Node *n, std::shared_ptr<Scope> s)
    {
        Val obj = n->object ? evaluate(n->object.get(), s) : nova_null();

        auto runIter = [&](const std::string &key, int idx) -> ExecResult
        {
            auto bs = s->child(Scope::Kind::Block);
            bs->setOwn(n->varName, nova_str(key));
            bs->loopIndex = idx;
            return _runBody(n->body, bs);
        };

        ExecResult pendingReturn;
        bool returned = false;
        auto handleSig = [&](const ExecResult &r) -> bool
        {
            if (r.sig == Signal::Break)
                return true;
            if (r.sig == Signal::Return)
            {
                pendingReturn = r;
                returned = true;
                return true;
            }
            return false;
        };

        if (obj.isObject())
        {
            int i = 0;
            for (auto &k : obj->obj->keys())
                if (handleSig(runIter(k, i++)))
                    break;
        }
        else if (obj.isArray())
        {
            for (int i = 0; i < (int)obj->arr->inner.size(); i++)
                if (handleSig(runIter(std::to_string(i), i)))
                    break;
        }
        else if (obj.isStruct())
        {
            int i = 0;
            for (auto &[k, _] : obj->strct->inner)
                if (handleSig(runIter(k, i++)))
                    break;
        }

        if (returned)
            return pendingReturn;
        return ExecResult::Val_(nova_null());
    }

    // ── each ──────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execEach(const Node *n, std::shared_ptr<Scope> s)
    {
        Val iter = n->iterable ? evaluate(n->iterable.get(), s) : nova_null();

        // top of _execForOf, before the isArray() check:
        if (iter->overloads)
        {
            auto it = iter->overloads->find("iter");
            if (it != iter->overloads->end())
            {
                Val result = callFunction(it->second, {iter}, s);
                // replace iter with the returned array/range
                iter = result;
            }
        }

        ValVec items;
        if (iter.isArray())
            items = iter->arr->inner;
        else if (iter.isRange())
            items = iter->range->toArray();
        else if (iter.isString())
        {
            for (auto &cp : utf8::codepoints(iter->sval))
                items.push_back(nova_str(cp));
        }

        for (int i = 0; i < (int)items.size(); i++)
        {
            auto bs = s->child(Scope::Kind::Block);
            bs->setOwn(n->varName, items[i]);
            if (!n->indexName.empty())
                bs->setOwn(n->indexName, nova_num(i));
            bs->loopIndex = i;
            ExecResult r = _runBody(n->body, bs);
            if (r.sig == Signal::Break)
                break;
            if (r.sig == Signal::Return)
                return r;
            // Continue / Normal — keep iterating
        }
        return ExecResult::Val_(nova_null());
    }

    // ── switch ────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execSwitch(const Node *n, std::shared_ptr<Scope> s)
    {
        Val subject = n->subject ? evaluate(n->subject.get(), s) : nova_null();
        Val last = nova_null();
        bool matched = false;

        for (auto &c : n->cases)
        {
            if (c.kind == CaseNode::Kind::Default)
            {
                if (!matched)
                {
                    auto bs = s->child(Scope::Kind::Block);
                    ExecResult r = _runBody(c.body, bs);
                    if (r.sig == Signal::Break)
                        break; // swallow, stop iterating cases (matches old catch(BreakSignal&))
                    if (r.sig == Signal::Continue || r.sig == Signal::Return)
                        return r; // not ours to handle — propagate to enclosing loop/function
                    last = r.value;
                }
                break; // a Default case always terminates the case loop
            }
            Val caseVal = c.value ? evaluate(c.value.get(), s) : nova_null();
            if (!matched)
            {
                bool eq = false;
                if (subject->overloads)
                {
                    auto it = subject->overloads->find("binary:==");
                    if (it != subject->overloads->end())
                        eq = _isTruthy(callFunction(it->second, {subject, caseVal}, s));
                    else
                        eq = *subject == *caseVal;
                }
                else
                {
                    eq = *subject == *caseVal;
                }
                if (eq)
                    matched = true;
            }
            if (matched)
            {
                auto bs = s->child(Scope::Kind::Block);
                ExecResult r = _runBody(c.body, bs);
                if (r.sig == Signal::Break)
                    break;
                if (r.sig == Signal::Continue || r.sig == Signal::Return)
                    return r;
                last = r.value;
            }
        }
        return ExecResult::Val_(last);
    }

    // ── match ─────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execMatch(const Node *n, std::shared_ptr<Scope> s)
    {
        Val subject = n->subject ? evaluate(n->subject.get(), s) : nova_null();

        for (auto &w : n->whenCases)
        {
            if (w.patterns.empty())
            {
                // default
                auto bs = s->child(Scope::Kind::Block);
                return _runBody(w.body, bs);
            }
            for (auto &pat : w.patterns)
            {
                if (pat && _matchPattern(subject, pat.get(), s))
                {
                    auto bs = s->child(Scope::Kind::Block);
                    return _runBody(w.body, bs);
                }
            }
        }
        return ExecResult::Val_(nova_null());
    }

    // ── try ───────────────────────────────────────────────────────────────────────
    // ── try ───────────────────────────────────────────────────────────────────────
    ExecResult Executor::_execTry(const Node *n, std::shared_ptr<Scope> s)
    {
        Val result = nova_null();

        // Build handle clause map
        std::unordered_map<std::string, const HandleClause *> handlers;
        for (auto &h : n->handleClauses)
            handlers[h.name] = &h;

        // ── Resumable algebraic effects via fiber ────────────────────────────────
        if (!handlers.empty())
        {
            auto fiber = std::make_shared<Fiber>();
            auto exe = this;
            auto tryBodyPtr = &n->tryBody;

            // Captures the try-body's ExecResult across the fiber boundary.
            // _execTry's stack frame outlives the fiber — safe captured by ref.
            ExecResult tryBodyResult;

            fiber->body = [exe, tryBodyPtr, s, &tryBodyResult](Fiber &f)
            {
                auto bs = s->child(Scope::Kind::Block);

                ExecResult r;
                try
                {
                    r = exe->_runBody(*tryBodyPtr, bs);
                }
                catch (ReturnSignal &rs)
                {
                    // Safety net for `yield` used outside any generator
                    r = ExecResult::Returned(rs.value, rs.hard);
                }
                tryBodyResult = r;
                f.yieldedValue = r.value;
            };

            // One trampoline per try, shared by every effect name it declares.
            Fiber *fiberRef = fiber.get();
            Val trampoline = NovaValue::makeNative(
                [fiberRef](ValVec a, auto) -> Val
                {
                    auto payload = nova_obj();
                    payload->obj->set("__effect_name__",
                                      a.empty() ? nova_str("") : a[0]);
                    ValVec effectArgs(a.begin() + (a.empty() ? 0 : 1), a.end());
                    payload->obj->set("__effect_args__", nova_arr(effectArgs));

                    fiberRef->yieldedValue = payload;
                    fiberRef->suspend();
                    return fiberRef->sentValue ? fiberRef->sentValue : nova_null();
                },
                "__yield_perform__");

            // Register effect names globally with save/restore for nesting.
            std::unordered_map<std::string, Val> saved;
            std::vector<std::string> savedNew;
            for (auto &[name, _] : handlers)
            {
                auto it = _performHandlers.find(name);
                if (it != _performHandlers.end())
                    saved[name] = it->second;
                else
                    savedNew.push_back(name);
                _performHandlers[name] = trampoline;
            }
            auto restoreHandlers = [&]()
            {
                for (auto &[name, prev] : saved)
                    _performHandlers[name] = prev;
                for (auto &name : savedNew)
                    _performHandlers.erase(name);
            };

            try
            {
                fiberInit(*fiber);
                fiber->resume();

                while (!fiber->done)
                {
                    Val yielded = fiber->yieldedValue;
                    bool dispatched = false;

                    if (yielded && yielded.isObject())
                    {
                        Val nameVal = yielded->obj->get("__effect_name__");
                        if (nameVal && nameVal.isString())
                        {
                            auto it = handlers.find(nameVal->sval);
                            if (it != handlers.end())
                            {
                                auto &clause = *it->second;
                                auto bs = s->child(Scope::Kind::Catch);

                                Val argsVal = yielded->obj->get("__effect_args__");
                                if (argsVal && argsVal.isArray())
                                {
                                    for (int i = 0;
                                         i < (int)clause.params.size() &&
                                         i < (int)argsVal->arr->inner.size();
                                         i++)
                                        bs->setOwn(clause.params[i],
                                                   argsVal->arr->inner[i]);
                                }

                                // Handler's own return/break/continue just
                                // supplies the resume value; it doesn't escape
                                // into the suspended computation.
                                fiber->sentValue = _runBody(clause.body, bs).value;
                                fiber->resume();
                                dispatched = true;
                            }
                        }
                    }

                    if (!dispatched)
                        break;
                }
            }
            catch (...)
            {
                restoreHandlers();
                throw;
            }
            restoreHandlers();

            result = fiber->yieldedValue ? fiber->yieldedValue : nova_null();

            // else runs only if fiber completed normally
            if (!n->elseBody.empty() && fiber->done && tryBodyResult.isNormal())
            {
                auto bs = s->child(Scope::Kind::Block);
                ExecResult elseR = _runBody(n->elseBody, bs);
                result = elseR.value;
                if (!elseR.isNormal())
                    tryBodyResult = elseR;
            }
            // finally always runs; its own control-flow wins if non-Normal
            if (!n->finallyBody.empty())
            {
                auto bs = s->child(Scope::Kind::Block);
                ExecResult fr = _runBody(n->finallyBody, bs);
                if (!fr.isNormal())
                    return fr;
            }
            // propagate try-body's break/continue/return
            if (fiber->done && !tryBodyResult.isNormal())
                return tryBodyResult;
            return ExecResult::Val_(result);
        }

        // ── Standard (non-resumable) try/catch path ──────────────────────────────
        // break/continue/return from the try or catch body are now carried in
        // ExecResult — they never appear as C++ exceptions here, so the broad
        // catch(...) below can no longer misinterpret them as errors.
        ExecResult tryResult;
        bool needRethrow = false;
        std::exception_ptr pendingExc;

        try
        {
            auto bs = s->child(Scope::Kind::Block);
            ExecResult r = _runBody(n->tryBody, bs);
            if (!r.isNormal())
            {
                tryResult = r; // return/break/continue — skip else, run finally
            }
            else if (!n->elseBody.empty())
            {
                auto ebs = s->child(Scope::Kind::Block);
                tryResult = _runBody(n->elseBody, ebs);
            }
            else
            {
                tryResult = r;
            }
        }
        catch (PerformSignal &)
        {
            pendingExc = std::current_exception();
            needRethrow = true;
        }
        catch (ThrowSignal &e)
        {
            if (n->catchBody.empty())
            {
                pendingExc = std::current_exception();
                needRethrow = true;
            }
            else
            {
                auto bs = s->child(Scope::Kind::Catch);
                if (!n->catchName.empty())
                    bs->setOwn(n->catchName, e.value);
                tryResult = _runBody(n->catchBody, bs);
            }
        }
        catch (std::exception &e)
        {
            if (n->catchBody.empty())
            {
                pendingExc = std::current_exception();
                needRethrow = true;
            }
            else
            {
                auto bs = s->child(Scope::Kind::Catch);
                if (!n->catchName.empty())
                    bs->setOwn(n->catchName, nova_str(e.what()));
                tryResult = _runBody(n->catchBody, bs);
            }
        }
        catch (...)
        {
            if (n->catchBody.empty())
            {
                pendingExc = std::current_exception();
                needRethrow = true;
            }
            else
            {
                auto bs = s->child(Scope::Kind::Catch);
                if (!n->catchName.empty())
                    bs->setOwn(n->catchName, nova_str("Unknown error"));
                tryResult = _runBody(n->catchBody, bs);
            }
        }

        // finally always runs — fixes the bug where it was skipped on rethrow
        if (!n->finallyBody.empty())
        {
            auto bs = s->child(Scope::Kind::Block);
            ExecResult fr = _runBody(n->finallyBody, bs);
            if (!fr.isNormal())
                return fr; // finally's control-flow overrides try/catch
        }

        if (needRethrow)
            std::rethrow_exception(pendingExc);

        return tryResult;
    }
    // ── return ────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execReturn(const Node *n, std::shared_ptr<Scope> s)
    {
        Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
        return ExecResult::Returned(v, n->terminate);
    }

    // ── emit ──────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execEmit(const Node *n, std::shared_ptr<Scope> s)
    {
        if (!n->event)
            return ExecResult::Val_(nova_null());
        std::string evName = evaluate(n->event.get(), s).asString();
        Val payload = n->value ? evaluate(n->value.get(), s) : nova_null();

        auto it = eventListeners.find(evName);
        if (it != eventListeners.end())
        {
            for (auto &listener : it->second)
            {
                auto ls = listener.scope->child(Scope::Kind::Block);
                if (!listener.param.empty())
                    ls->setOwn(listener.param, payload);
                // A listener body's return/break/continue has no meaning to
                // the emitter's own context, so it's discarded here — a
                // listener is its own control-flow boundary.
                _runBody(listener.body, ls);
            }
        }
        return ExecResult::Val_(nova_null());
    }

    // ── on ────────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execOn(const Node *n, std::shared_ptr<Scope> s)
    {
        if (!n->event)
            return ExecResult::Val_(nova_null());
        std::string evName = evaluate(n->event.get(), s).asString();
        EventListener listener;
        listener.param = n->param;
        listener.scope = s;
        for (const auto &b : n->body)
            listener.body.push_back(b);
        eventListeners[evName].push_back(std::move(listener));
        return ExecResult::Val_(nova_null());
    }

    // ── with ─────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execWith(const Node *n, std::shared_ptr<Scope> s)
    {
        if (n->kind == Node::Kind::WithOption)
        {
            // with option FLAG { body }
            auto prev = options.find(n->flag);
            Val prevVal = prev != options.end() ? prev->second : nova_bool(false);
            options[n->flag] = nova_bool(true);
            auto bs = s->child(Scope::Kind::With);
            ExecResult result;
            try
            {
                result = _runBody(n->body, bs);
            }
            catch (...)
            {
                options[n->flag] = prevVal;
                throw;
            }
            options[n->flag] = prevVal;
            return result; // passes Normal/Return/Break/Continue straight through
        }
        // with ctx { body } — inject context object into scope
        Val ctx = n->target ? evaluate(n->target.get(), s) : nova_null();
        auto bs = s->child(Scope::Kind::With);
        if (ctx && ctx.isObject())
        {
            for (auto &[k, v] : ctx->obj->inner)
                bs->setOwn(k, v);
        }
        else if (ctx && ctx.isStruct())
        {
            for (auto &[k, v] : ctx->strct->inner)
                bs->setOwn(k, v);
        }
        return _runBody(n->body, bs);
    }

    // ── namespace ─────────────────────────────────────────────────────────────────

    ExecResult Executor::_execNamespace(const Node *n, std::shared_ptr<Scope> s)
    {
        auto ns = namespaces.find(n->name);
        std::shared_ptr<Scope> nsScope;
        if (ns != namespaces.end())
        {
            nsScope = ns->second;
        }
        else
        {
            nsScope = std::make_shared<Scope>(Scope::Kind::Namespace, globalScope, globalScope);
            nsScope->namespaceName = n->name;
            namespaces[n->name] = nsScope;
        }
        for (auto &stmt : n->body)
        {
            if (!stmt)
                continue;
            if (stmt->kind == Node::Kind::Block)
                _runBody(stmt->body, nsScope); // unwrap the block, use nsScope directly
            else
                execute(stmt.get(), nsScope);
        }
        auto nsObj = nova_obj();
        for (auto &[k, desc] : nsScope->variables)
        {
            Val v = (desc.hasHooks && desc.read) ? desc.read() : desc.raw;
            nsObj->obj->set(k, v);
        }
        s->setOwn(n->name, nsObj);
        return ExecResult::Val_(nsObj);
    }

    // ── import ────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execImport(const Node *n, std::shared_ptr<Scope> s)
    {
        if (n->kind == Node::Kind::ImportBuiltin)
        {
            // import_builtin name1, name2, ...
            // no op for now-auto injected
        }
        if (n->kind == Node::Kind::ImportKit)
        {
            return ExecResult::Val_(_loadKitBinary(n->kitName, s));
        }
        if (n->kind == Node::Kind::FromImport)
        {
            std::string path = n->source ? evaluate(n->source.get(), s).asString() : "";
            Val mod = _loadModule(path, s);
            for (auto &name : n->names)
            {
                Val v = (mod && mod.isObject()) ? mod->obj->get(name) : nullptr;
                s->setOwn(name, v ? v : nova_null());
            }
            return ExecResult::Val_(nova_null());
        }
        // regular import
        std::string path = n->source ? evaluate(n->source.get(), s).asString() : "";
        Val mod = _loadModule(path, s);
        if (n->names.empty())
        {
            // import "path" — expose all exports
            if (mod && mod.isObject())
                for (auto &k : mod->obj->keys())
                    s->setOwn(k, mod->obj->get(k));
        }
        else
        {
            // import "path" as name
            for (auto &name : n->names)
                s->setOwn(name, mod);
        }
        return ExecResult::Val_(mod);
    }

    // ── export ────────────────────────────────────────────────────────────────────

    ExecResult Executor::_execExport(const Node *n, std::shared_ptr<Scope> s)
    {
        Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
        Val exports = s->get("__exports__");
        if (!exports || !exports.isObject())
        {
            exports = nova_obj();
            s->setOwn("__exports__", exports);
        }
        if (n->kind == Node::Kind::DefaultExport)
        {
            exports->obj->set("default", v);
        }
        else if (v && v.isObject())
        {
            for (auto &k : v->obj->keys())
                exports->obj->set(k, v->obj->get(k));
        }
        else if (v && !v.isNull())
        {
            // try to get name from value
            if (v.isFunction() && v->fn && !v->fn->name.empty())
                exports->obj->set(v->fn->name, v);
            else
                exports->obj->set("default", v);
        }
        return ExecResult::Val_(v);
    }

    // ── type/struct/enum/interface/trait/impl decls ───────────────────────────────

    ExecResult Executor::_execTypeDecl(const Node *n, std::shared_ptr<Scope> s)
    {
        auto node = std::make_shared<Node>(*n);
        types.registerType(n->name, node);
        return ExecResult::Val_(nova_null());
    }
    ExecResult Executor::_execStructDecl(const Node *n, std::shared_ptr<Scope> s)
    {
        auto node = std::make_shared<Node>(*n);
        types.registerStruct(n->name, node);
        // Register a constructor function in scope
        auto exe = this;
        std::string tname = n->name;
        Val ctor = NovaValue::makeNative([exe, tname, node](ValVec args, std::shared_ptr<Scope> cs) -> Val
                                         {
        ValMap fields;
        if (!args.empty() && args[0] && args[0].isObject()) {
            fields = args[0]->obj->inner;
        } else {
            for (int i = 0; i < (int)node->structFields.size() && i < (int)args.size(); i++)
                fields[node->structFields[i].name] = args[i];
        }
        return exe->types.createStruct(tname, fields, *exe); }, n->name);
        s->setOwn(n->name, ctor);
        return ExecResult::Val_(nova_null());
    }
    ExecResult Executor::_execEnumDecl(const Node *n, std::shared_ptr<Scope> s)
    {
        auto node = std::make_shared<Node>(*n);
        types.registerEnum(n->name, node);
        // Build enum namespace object
        auto enumObj = nova_obj();
        for (auto &v : n->enumVariants)
        {
            Val val = v.value ? evaluate(v.value.get(), s) : nova_str(v.name);
            auto ev = NovaValue::makeEnum(n->name, v.name, val);
            enumObj->obj->set(v.name, ev);
        }
        s->setOwn(n->name, enumObj);
        return ExecResult::Val_(nova_null());
    }
    ExecResult Executor::_execInterfaceDecl(const Node *n, std::shared_ptr<Scope> s)
    {
        auto node = std::make_shared<Node>(*n);
        types.registerInterface(n->name, node);
        return ExecResult::Val_(nova_null());
    }
    ExecResult Executor::_execTraitDecl(const Node *n, std::shared_ptr<Scope> s)
    {
        auto node = std::make_shared<Node>(*n);
        types.registerTrait(n->name, node);
        return ExecResult::Val_(nova_null());
    }
    ExecResult Executor::_execImplDecl(const Node *n, std::shared_ptr<Scope> s)
    {
        // impl Trait for Type { method() {} }
        // Add methods to the struct/type's constructor
        Val typeVal = s->get(n->forType);
        if (!typeVal)
            return ExecResult::Val_(nova_null());
        for (auto &m : n->members)
        {
            if (m.kind != ClassMember::Kind::Method)
                continue;
            auto methodNode = std::make_shared<Node>();
            methodNode->kind = Node::Kind::Function;
            methodNode->name = m.name;
            methodNode->funcArgs = m.args;
            for (const auto &b : m.body)
                methodNode->body.push_back(b);
            Val mfn = NovaValue::makeFunction(methodNode, s);
            mfn->fn->name = m.name;
            if (typeVal.isObject())
                typeVal->obj->set(m.name, mfn);
        }
        return ExecResult::Val_(nova_null());
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  server { route METHOD "path" (params) { body } ... }
    // ════════════════════════════════════════════════════════════════════════════
    ExecResult Executor::_execServer(const Node *n, std::shared_ptr<Scope> s)
    {
        Val portVal = n->port ? evaluate(n->port.get(), s) : nova_num(3000);
        int port = (int)portVal.asNumber();

        // ── compiled routes ─────────────────────────────────────────────────────
        struct CompiledRoute
        {
            std::string method;
            std::vector<std::string> segments; // literal text or ":name"
            std::vector<std::string> paramNames;
            bool hasReqParam = false;
            Val handler;
        };

        auto routesTable = std::make_shared<std::vector<CompiledRoute>>();
        auto routesArr = nova_arr();

        for (auto &route : n->routes)
        {
            CompiledRoute cr;

            cr.method = route.method;
            std::transform(cr.method.begin(), cr.method.end(), cr.method.begin(), ::toupper);

            Val pathVal = route.path ? evaluate(route.path.get(), s) : nova_str("/");
            cr.segments = nova_http_splitPath(pathVal.asString());
            cr.paramNames = route.params;

            // ── build handler ────────────────────────────────────────────────────
            auto bodyNode = std::make_shared<Node>();
            bodyNode->kind = Node::Kind::Program;
            for (const auto &b : route.body)
                bodyNode->body.push_back(b);

            std::vector<FuncArg> fargs;
            bool hasReqParam = false;

            for (auto &p : route.params)
            {
                FuncArg fa;
                fa.name = p;
                fargs.push_back(fa);

                if (p == "req")
                    hasReqParam = true;
            }

            if (!hasReqParam)
            {
                FuncArg reqArg;
                reqArg.name = "req";
                fargs.push_back(reqArg);
            }

            cr.hasReqParam = hasReqParam;

            auto handlerFn = std::make_shared<Node>();
            handlerFn->kind = Node::Kind::Function;
            handlerFn->funcArgs = fargs;
            handlerFn->body.push_back(bodyNode);

            cr.handler = NovaValue::makeFunction(handlerFn, s);

            // ── route entry object ───────────────────────────────────────────────
            auto rEntry = nova_obj();
            rEntry->obj->set("method", nova_str(cr.method));
            rEntry->obj->set("path", pathVal);
            rEntry->obj->set("handler", cr.handler);

            routesArr->arr->push(rEntry);
            routesTable->push_back(std::move(cr));
        }

        auto serverObj = nova_obj();
        serverObj->obj->set("port", nova_num(port));
        serverObj->obj->set("routes", routesArr);

        // ── bind + listen ─────────────────────────────────────────────────────
        novac::sock_platform_init();

        sock_t listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd == SOCK_INVALID)
            _error("server: failed to create listening socket", n);

        int yes = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)port);

        if (::bind(listenFd, (sockaddr *)&addr, sizeof(addr)) == SOCK_ERR)
        {
            sock_close(listenFd);
            _error("server: port " + std::to_string(port) + " unavailable", n);
        }

        if (::listen(listenFd, 128) == SOCK_ERR)
        {
            sock_close(listenFd);
            _error("server: listen() failed", n);
        }

        auto listenSock = std::make_shared<novac::NovaSocket>(listenFd);
        listenSock->server = true;

        auto stopped = std::make_shared<std::atomic<bool>>(false);

        serverObj->obj->set("stop", NovaValue::makeNative(
                                        [stopped](ValVec, auto) -> Val
                                        {
                                            stopped->store(true);
                                            return nova_null();
                                        },
                                        "stop"));

        std::cout << "[novac] server listening on port " << port << "\n";

        // ── accept loop ───────────────────────────────────────────────────────
        while (!stopped->load())
        {
            if (!listenSock->waitReadable(200))
                continue;

            if (stopped->load())
                break;

            sockaddr_in cliAddr{};
            socklen_t cliLen = sizeof(cliAddr);

            sock_t cfd = ::accept(listenSock->fd, (sockaddr *)&cliAddr, &cliLen);
            if (cfd == SOCK_INVALID)
                continue;

            novac::NovaSocket cs(cfd);

            // ── request parsing ────────────────────────────────────────────────
            std::string headBlock = cs.recvUntil("\r\n\r\n", 1 << 16);
            if (headBlock.empty())
                continue;

            std::istringstream headStream(headBlock);
            std::string line;

            std::getline(headStream, line);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            std::istringstream rl(line);
            std::string method, target, httpVersion;

            rl >> method >> target >> httpVersion;
            std::transform(method.begin(), method.end(), method.begin(), ::toupper);

            Val headersObj = nova_obj();

            while (std::getline(headStream, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                if (line.empty())
                    break;

                size_t colon = line.find(':');
                if (colon == std::string::npos)
                    continue;

                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);

                size_t start = val.find_first_not_of(' ');
                val = (start == std::string::npos) ? "" : val.substr(start);

                headersObj->obj->set(key, nova_str(val));
            }

            auto getHeaderCI = [&](const std::string &name) -> std::string
            {
                for (auto &kv : headersObj->obj->inner)
                {
                    if (kv.first.size() != name.size())
                        continue;

                    bool eq = true;
                    for (size_t i = 0; i < kv.first.size(); i++)
                    {
                        if (std::tolower((unsigned char)kv.first[i]) !=
                            std::tolower((unsigned char)name[i]))
                        {
                            eq = false;
                            break;
                        }
                    }

                    if (eq)
                        return kv.second.asString();
                }
                return "";
            };

            std::string body;
            std::string clHeader = getHeaderCI("Content-Length");

            if (!clHeader.empty())
            {
                int len = std::atoi(clHeader.c_str());
                if (len > 0 && len < (16 * 1024 * 1024))
                    body = cs.recvExact(len);
            }

            std::string rawPath = target, rawQuery;
            size_t qpos = target.find('?');

            if (qpos != std::string::npos)
            {
                rawPath = target.substr(0, qpos);
                rawQuery = target.substr(qpos + 1);
            }

            std::string decodedPath = nova_http_urlDecode(rawPath);
            std::vector<std::string> reqSegs = nova_http_splitPath(decodedPath);

            // ── route match ────────────────────────────────────────────────────
            CompiledRoute *matched = nullptr;
            std::unordered_map<std::string, std::string> pathParams;
            bool wrongMethod = false;

            for (auto &cr : *routesTable)
            {
                if (cr.segments.size() != reqSegs.size())
                    continue;

                std::unordered_map<std::string, std::string> candidate;
                bool ok = true;

                for (size_t i = 0; i < cr.segments.size(); i++)
                {
                    const std::string &seg = cr.segments[i];

                    if (!seg.empty() && seg[0] == ':')
                    {
                        candidate[seg.substr(1)] =
                            nova_http_urlDecode(reqSegs[i]);
                    }
                    else if (seg != reqSegs[i])
                    {
                        ok = false;
                        break;
                    }
                }

                if (!ok)
                    continue;

                if (cr.method != method)
                {
                    wrongMethod = true;
                    continue;
                }

                matched = &cr;
                pathParams = std::move(candidate);
                break;
            }

            // ── build req object ───────────────────────────────────────────────
            Val reqObj = nova_obj();
            reqObj->obj->set("method", nova_str(method));
            reqObj->obj->set("path", nova_str(decodedPath));
            reqObj->obj->set("headers", headersObj);
            reqObj->obj->set("body", nova_str(body));
            reqObj->obj->set("server", serverObj);

            Val queryObj = nova_obj();

            {
                size_t pos = 0;

                while (pos <= rawQuery.size())
                {
                    size_t amp = rawQuery.find('&', pos);

                    std::string pair =
                        rawQuery.substr(pos,
                                        amp == std::string::npos ? std::string::npos : amp - pos);

                    if (!pair.empty())
                    {
                        size_t eq = pair.find('=');

                        std::string k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
                        std::string v = (eq == std::string::npos) ? "" : pair.substr(eq + 1);

                        if (!k.empty())
                        {
                            queryObj->obj->set(
                                nova_http_urlDecode(k),
                                nova_str(nova_http_urlDecode(v)));
                        }
                    }

                    if (amp == std::string::npos)
                        break;

                    pos = amp + 1;
                }
            }

            reqObj->obj->set("query", queryObj);

            Val paramsObj = nova_obj();
            for (auto &kv : pathParams)
                paramsObj->obj->set(kv.first, nova_str(kv.second));

            reqObj->obj->set("params", paramsObj);

            // ── execute handler ────────────────────────────────────────────────
            int status = 200;
            std::string contentType = "text/plain";
            std::string respBody;
            Val extraHeaders = nullptr;

            if (!matched)
            {
                status = wrongMethod ? 405 : 404;
                respBody = wrongMethod ? "405 Method Not Allowed" : "404 Not Found";
            }
            else
            {
                try
                {
                    ValVec args;

                    for (auto &pname : matched->paramNames)
                    {
                        if (pname == "req")
                        {
                            args.push_back(reqObj);
                            continue;
                        }

                        auto it = pathParams.find(pname);

                        args.push_back(
                            it != pathParams.end()
                                ? nova_str(it->second)
                                : nova_null());
                    }

                    if (!matched->hasReqParam)
                        args.push_back(reqObj);

                    Val result = callFunction(matched->handler, args, s);

                    if (!result || result.isNull())
                    {
                        status = 204;
                    }
                    else if (result.isObject() &&
                             (result->obj->get("body") || result->obj->get("status")))
                    {
                        Val sv = result->obj->get("status");
                        Val ctv = result->obj->get("contentType");
                        Val bv = result->obj->get("body");
                        Val hv = result->obj->get("headers");

                        if (sv && sv.isNumber())
                            status = (int)sv->nval;

                        if (ctv && ctv.isString())
                            contentType = ctv->sval;

                        if (bv)
                        {
                            if (bv.isString())
                                respBody = bv->sval;
                            else
                            {
                                respBody = stringify(bv);
                                if (!ctv)
                                    contentType = "application/json";
                            }
                        }

                        if (hv && hv.isObject())
                            extraHeaders = hv;
                    }
                    else if (result.isString())
                    {
                        respBody = result->sval;
                        contentType = "text/html";
                    }
                    else
                    {
                        respBody = stringify(result);
                        contentType = "application/json";
                    }
                }
                catch (ThrowSignal &e)
                {
                    status = 500;
                    respBody = "500 Internal Server Error: " + stringify(e.value);
                }
                catch (std::exception &e)
                {
                    status = 500;
                    respBody = std::string("500 Internal Server Error: ") + e.what();
                }
            }

            static const std::unordered_map<int, std::string> reasonPhrases = {
                {200, "OK"}, {201, "Created"}, {204, "No Content"}, {301, "Moved Permanently"}, {302, "Found"}, {400, "Bad Request"}, {401, "Unauthorized"}, {403, "Forbidden"}, {404, "Not Found"}, {405, "Method Not Allowed"}, {500, "Internal Server Error"}};

            std::string reason =
                reasonPhrases.count(status)
                    ? reasonPhrases.at(status)
                    : "OK";

            std::ostringstream resp;

            resp << "HTTP/1.1 " << status << " " << reason << "\r\n";
            resp << "Content-Type: " << contentType << "\r\n";
            resp << "Content-Length: " << respBody.size() << "\r\n";

            if (extraHeaders)
            {
                for (auto &kv : extraHeaders->obj->inner)
                    resp << kv.first << ": " << kv.second.asString() << "\r\n";
            }

            resp << "Connection: close\r\n\r\n";
            resp << respBody;

            std::string respStr = resp.str();

            cs.sendAll(respStr.c_str(), (int)respStr.size());
            cs.close();
        }

        listenSock->close();
        return ExecResult::Val_(serverObj);
    }
    // ════════════════════════════════════════════════════════════════════════════
    //  MODULE LOADING
    // ════════════════════════════════════════════════════════════════════════════

    Val Executor::_loadModule(const std::string &path, std::shared_ptr<Scope> s, bool isBuiltin)
    {
        // Check cache
        auto it = moduleCache.find(path);
        if (it != moduleCache.end())
            return it->second;

        std::string content;
        std::string fullPath = path;

        if (!isBuiltin)
        {
            // Resolve relative to current file
            try
            {
                fs::path base = fs::path(filename).parent_path();
                fs::path resolved = base / path;
                if (!fs::exists(resolved))
                {
                    // Try with .nv extension
                    resolved = base / (path + ".nv");
                }
                if (!fs::exists(resolved))
                    _error("Module not found: " + path);
                fullPath = resolved.string();
                std::ifstream f(fullPath);
                if (!f.is_open())
                    _error("Cannot open module: " + fullPath);
                content = std::string(std::istreambuf_iterator<char>(f), {});
            }
            catch (std::exception &e)
            {
                _error(std::string("Import error: ") + e.what());
            }
        }

        // Create module scope
        auto modScope = std::make_shared<Scope>(Scope::Kind::Module, globalScope, globalScope);
        auto exportsObj = nova_obj();
        modScope->setOwn("__exports__", exportsObj);

        // Parse and run
        Parser p(content, fullPath);
        auto ast = p.parse();
        Executor modExe(content, fullPath);
        modExe.globalScope = globalScope;
        modExe.run(ast.get(), modScope);

        // Collect exports
        Val exports = modScope->get("__exports__");
        if (!exports || (exports.isObject() && exports->obj->inner.empty()))
        {
            // If no explicit exports, expose everything from module scope
            exports = nova_obj();
            for (auto &[k, desc] : modScope->variables)
            {
                if (k.substr(0, 2) == "__")
                    continue;
                Val v = (desc.hasHooks && desc.read) ? desc.read() : desc.raw;
                exports->obj->set(k, v);
            }
        }

        moduleCache[path] = exports;
        return exports;
    }

    Val Executor::_loadKitBinary(const std::string &kitName, std::shared_ptr<Scope> s)
    {
#if !NOVA_FFI_PLATFORM_SUPPORTED
        _error("Kit loading is not supported on this platform: " + kitName);
        return nova_null();
#else
        // Kits live at ../../kits/<kitname>/out/main[.dll|.so]
        fs::path kitDir =
            fs::path(__FILE__).parent_path().parent_path().parent_path() / "kits" / kitName / "out";
#ifdef _WIN32
        fs::path kitBin = kitDir / "main.dll";
#else
        fs::path kitBin = kitDir / "main.so";
#endif
        std::string binPath;
        try
        {
            binPath = fs::absolute(kitBin).string();
        }
        catch (...)
        {
            binPath = kitBin.string();
        }

        std::string cacheKey = "kit:" + binPath;
        auto it = moduleCache.find(cacheKey);
        if (it != moduleCache.end())
        {
            s->setOwn(kitName, it->second);
            return it->second;
        }

#ifdef _WIN32
        HMODULE handle = LoadLibraryA(binPath.c_str());
        if (!handle)
            _error("Kit not found or failed to load: " + kitName +
                   " (tried: " + binPath + ", error " + std::to_string(GetLastError()) + ")");
        using KitRegFn = void (*)(Executor *);
        auto regFn = reinterpret_cast<KitRegFn>(GetProcAddress(handle, "nova_kit_register"));
        if (!regFn)
        {
            FreeLibrary(handle);
            _error("Kit '" + kitName + "' does not export nova_kit_register");
        }
#else
        ::dlerror();
        void *handle = ::dlopen(binPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!handle)
        {
            const char *e = ::dlerror();
            _error("Kit not found or failed to load: " + kitName +
                   " (tried: " + binPath + "): " + (e ? e : "unknown error"));
        }
        ::dlerror();
        using KitRegFn = void (*)(Executor *);
        auto regFn = reinterpret_cast<KitRegFn>(::dlsym(handle, "nova_kit_register"));
        const char *symErr = ::dlerror();
        if (symErr || !regFn)
        {
            ::dlclose(handle);
            _error("Kit '" + kitName + "' does not export nova_kit_register: " +
                   (symErr ? symErr : "symbol not found"));
        }
#endif
        try
        {
            regFn(this);
        }
        catch (std::exception &e)
        {
            _error(std::string("Kit '") + kitName + "' registration threw: " + e.what());
        }
        catch (...)
        {
            _error("Kit '" + kitName + "' registration threw an unknown exception");
        }

        Val kit = globalScope->get(kitName);
        if (!kit)
            kit = nova_null();

        moduleCache[cacheKey] = kit;
        s->setOwn(kitName, kit);
        return kit;
#endif
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  EVALUATE  —  expression dispatch
    // ════════════════════════════════════════════════════════════════════════════

    Val Executor::evaluate(const Node *n, std::shared_ptr<Scope> s)
    {
        if (!n)
            return nova_null();
        _currentLine = n->line;
        _currentCol = n->column;

        switch (n->kind)
        {
        // ── literals ──────────────────────────────────────────────────────────
        case Node::Kind::Value:
        {
            const std::string &sv = n->strval;
            if (n->valueKind == Node::ValueKind::Literal)
            {
                if (sv == "true")
                    return nova_bool(true);
                if (sv == "false")
                    return nova_bool(false);
                if (sv == "null")
                    return nova_null();
                return nova_null();
            }
            if (n->valueKind == Node::ValueKind::Number)
                return nova_num(n->numval);
            return nova_str(sv);
        }

        case Node::Kind::Ref:
        {
            Val v = s->get(n->name);
            s->globalScope->set("it", v);
            if (!v)
                return nova_null();
            if (v.isPointer() && v->ptr && v->ptr->isLink)
                return v->ptr->readFn();
            if (!v)
            {
                // undefined reference → null
                return nova_null();
            }
            return v;
        }

        case Node::Kind::DVar:
            return _evalDVar(n, s);
        case Node::Kind::Array:
            return _evalArray(n, s);
        case Node::Kind::Object:
            return _evalObject(n, s);
        case Node::Kind::FString:
            return _evalFString(n, s);
        case Node::Kind::UrlLiteral:
            return nova_str(n->strval);

        case Node::Kind::Regex:
        {
            std::string pat = n->strval;
            std::string flags = n->flags;
            auto obj = nova_obj();
            obj->obj->set("source", nova_str(pat));
            obj->obj->set("flags", nova_str(flags));
            obj->obj->set("_kind", nova_str("regex"));
            auto rxFlags = std::regex_constants::ECMAScript;
            for (char f : flags)
            {
                if (f == 'i')
                    rxFlags |= std::regex_constants::icase;
                if (f == 'm')
                    rxFlags |= std::regex_constants::multiline;
            }
            // .test(str) → bool
            obj->obj->set("test", NovaValue::makeNative([pat, rxFlags](ValVec a, auto) -> Val
                                                        {
                if (a.empty()) return nova_bool(false);
                try { std::regex re(pat, rxFlags); return nova_bool(std::regex_search(a[0].asString(), re)); }
                catch (...) { return nova_bool(false); } }, "test"));
            // .match(str) → array of matches or null
            obj->obj->set("match", NovaValue::makeNative([pat, rxFlags, flags](ValVec a, auto) -> Val
                                                         {
                if (a.empty()) return nova_null();
                try {
                    std::string subject = a[0].asString();
                    std::regex re(pat, rxFlags);
                    if (flags.find('g') != std::string::npos) {
                        auto arr = nova_arr();
                        auto it = std::sregex_iterator(subject.begin(), subject.end(), re);
                        for (; it != std::sregex_iterator(); ++it)
                            arr->arr->push(nova_str((*it)[0].str()));
                        return arr->arr->length() == 0 ? nova_null() : arr;
                    } else {
                        std::smatch m;
                        if (!std::regex_search(subject, m, re)) return nova_null();
                        auto arr = nova_arr();
                        for (size_t i = 0; i < m.size(); i++)
                            arr->arr->push(m[i].matched ? nova_str(m[i].str()) : nova_null());
                        return arr;
                    }
                } catch (...) { return nova_null(); } }, "match"));
            // .replace(str, repl) → string
            obj->obj->set("replace", NovaValue::makeNative([pat, rxFlags, flags](ValVec a, auto) -> Val
                                                           {
                if (a.size() < 2) return a.empty() ? nova_str("") : a[0];
                try {
                    std::regex re(pat, rxFlags);
                    bool global = flags.find('g') != std::string::npos;
                    if (global)
                        return nova_str(std::regex_replace(a[0].asString(), re, a[1].asString()));
                    return nova_str(std::regex_replace(a[0].asString(), re, a[1].asString(),
                        std::regex_constants::format_first_only));
                } catch (...) { return a[0]; } }, "replace"));
            // .split(str) → array
            obj->obj->set("split", NovaValue::makeNative([pat, rxFlags](ValVec a, auto) -> Val
                                                         {
                if (a.empty()) return nova_arr();
                try {
                    std::string subject = a[0].asString();
                    std::regex re(pat, rxFlags);
                    std::sregex_token_iterator it(subject.begin(), subject.end(), re, -1), end;
                    auto arr = nova_arr();
                    for (; it != end; ++it) arr->arr->push(nova_str(it->str()));
                    return arr;
                } catch (...) { return nova_arr(); } }, "split"));
            return obj;
        }

        // ── operators ─────────────────────────────────────────────────────────
        case Node::Kind::Binary:
            return _evalBinary(n, s);
        case Node::Kind::Unary:
            return _evalUnary(n, s);
        case Node::Kind::Prefix:
        {
            Val operand = evaluate(n->operand.get(), s);

            // overload — key: "prefix:<op>"
            if (operand && operand->overloads)
            {
                auto it = operand->overloads->find("prefix:" + n->op);
                if (it == operand->overloads->end())
                    it = operand->overloads->find("unary:" + n->op); // fallback
                if (it != operand->overloads->end())
                {
                    Val result = callFunction(it->second, {operand}, s);
                    if (n->operand->kind == Node::Kind::Ref)
                        s->set(n->operand->name, result);
                    else if (n->operand->kind == Node::Kind::Prop)
                        _setProp(evaluate(n->operand->left.get(), s), n->operand->name, result);
                    else if (n->operand->kind == Node::Kind::Subscript)
                    {
                        Val obj = evaluate(n->operand->left.get(), s);
                        Val idx = evaluate(n->operand->index.get(), s);
                        if (obj.isArray())
                            obj->arr->set((int)idx.asNumber(), result);
                        else if (obj.isObject())
                            obj->obj->set(idx.asString(), result);
                    }
                    return result;
                }
            }

            Val result = (n->op == "++")
                             ? nova_num(operand.asNumber() + 1)
                             : nova_num(operand.asNumber() - 1);

            if (n->operand->kind == Node::Kind::Ref)
                s->set(n->operand->name, result);
            else if (n->operand->kind == Node::Kind::Prop)
                _setProp(evaluate(n->operand->left.get(), s), n->operand->name, result);
            else if (n->operand->kind == Node::Kind::Subscript)
            {
                Val obj = evaluate(n->operand->left.get(), s);
                Val idx = evaluate(n->operand->index.get(), s);
                if (obj.isArray())
                    obj->arr->set((int)idx.asNumber(), result);
                else if (obj.isObject())
                    obj->obj->set(idx.asString(), result);
            }
            return result;
        }

        case Node::Kind::Postfix:
        {
            Val operand = evaluate(n->operand.get(), s);

            // overload — key: "postfix:<op>"
            if (operand && operand->overloads)
            {
                auto it = operand->overloads->find("postfix:" + n->op);
                if (it != operand->overloads->end())
                {
                    Val after = callFunction(it->second, {operand}, s);
                    if (n->operand->kind == Node::Kind::Ref)
                        s->set(n->operand->name, after);
                    else if (n->operand->kind == Node::Kind::Prop)
                        _setProp(evaluate(n->operand->left.get(), s), n->operand->name, after);
                    else if (n->operand->kind == Node::Kind::Subscript)
                    {
                        Val obj = evaluate(n->operand->left.get(), s);
                        Val idx = evaluate(n->operand->index.get(), s);
                        if (obj.isArray())
                            obj->arr->set((int)idx.asNumber(), after);
                        else if (obj.isObject())
                            obj->obj->set(idx.asString(), after);
                    }
                    return operand; // postfix always returns original
                }
            }

            Val after = (n->op == "++")
                            ? nova_num(operand.asNumber() + 1)
                            : nova_num(operand.asNumber() - 1);

            if (n->operand->kind == Node::Kind::Ref)
                s->set(n->operand->name, after);
            else if (n->operand->kind == Node::Kind::Prop)
                _setProp(evaluate(n->operand->left.get(), s), n->operand->name, after);
            else if (n->operand->kind == Node::Kind::Subscript)
            {
                Val obj = evaluate(n->operand->left.get(), s);
                Val idx = evaluate(n->operand->index.get(), s);
                if (obj.isArray())
                    obj->arr->set((int)idx.asNumber(), after);
                else if (obj.isObject())
                    obj->obj->set(idx.asString(), after);
            }
            return operand; // postfix always returns original
        }
        case Node::Kind::Ternary:
        {
            bool cond = _isTruthy(n->cond ? evaluate(n->cond.get(), s) : nova_null());
            return cond ? evaluate(n->consequent.get(), s) : evaluate(n->alternate.get(), s);
        }
        case Node::Kind::IfTernary:
        {
            Val operand = n->operand ? evaluate(n->operand.get(), s) : nova_null();
            bool cond = _isTruthy(n->cond ? evaluate(n->cond.get(), s) : nova_null());
            if (cond)
                return operand;
            return n->alternate ? evaluate(n->alternate.get(), s) : nova_null();
        }
        case Node::Kind::Assign:
        case Node::Kind::CompoundAssign:
            return _evalAssign(n, s);
        case Node::Kind::Spread:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
            return v; // spread is handled in array/call context
        }
        case Node::Kind::Cast:
        {
            Val v = n->left ? evaluate(n->left.get(), s) : nova_null();
            // Type cast — just validate and return
            return v;
        }

        // ── access ────────────────────────────────────────────────────────────
        case Node::Kind::Prop:
            return _evalProp(n, s, false);
        case Node::Kind::OptionalProp:
            return _evalProp(n, s, true);
        case Node::Kind::Subscript:
        case Node::Kind::OptionalSubscript:
            return _evalSubscript(n, s);
        case Node::Kind::Call:
        case Node::Kind::OptionalCall:
            return _evalCall(n, s);
        case Node::Kind::StructCall:
            return _evalStructCall(n, s);

        // ── functions ─────────────────────────────────────────────────────────
        case Node::Kind::ArrowFunc:
            return _evalArrow(n, s);
        case Node::Kind::Function:
            return _execFunction(n, s).value;
        case Node::Kind::New:
            return _evalNew(n, s);
        case Node::Kind::Await:
            return _evalAwait(n, s);
        case Node::Kind::Yield:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
            Val yieldFn = s->get("yield");
            if (yieldFn && yieldFn.isFunction())
                return callFunction(yieldFn, {v}, s);
            throw ReturnSignal{v, false};
        }
        case Node::Kind::Perform:
            return _evalPerform(n, s);

        // ── control expressions ───────────────────────────────────────────────
        case Node::Kind::Throw:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
            throw ThrowSignal{v};
        }
        case Node::Kind::Native:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
            return _toNative(v, n->castType);
        }
        case Node::Kind::Unative:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
            return _fromNative(v);
        }
        case Node::Kind::Run:
        {
            if (n->value)
                return execute(n->value.get(), s).value;
            return nova_null();
        }
        case Node::Kind::Link:
        {
            if (!n->value)
                return nova_null();
            const Node *target = n->value.get();

            std::function<Val()> getter;
            std::function<void(Val)> setter;

            if (target->kind == Node::Kind::Ref)
            {
                std::string name = target->name;
                auto scope = s;
                getter = [scope, name]() -> Val
                {
                    return scope->get(name);
                };
                setter = [scope, name](Val v)
                {
                    scope->set(name, v);
                };
            }
            else if (target->kind == Node::Kind::Prop)
            {
                Val obj = evaluate(target->left.get(), s);
                std::string key = target->name;
                getter = [obj, key]() -> Val
                {
                    return obj.isObject() ? obj->obj->get(key) : obj.isStruct() ? obj->strct->get(key)
                                                                                : nova_null();
                };
                setter = [obj, key](Val v)
                {
                    if (obj.isObject())
                        obj->obj->set(key, v);
                    else if (obj.isStruct())
                        obj->strct->set(key, v);
                };
            }
            else if (target->kind == Node::Kind::Subscript)
            {
                Val obj = evaluate(target->left.get(), s);
                Val idx = evaluate(target->index.get(), s);
                getter = [obj, idx]() -> Val
                {
                    if (obj.isArray() && idx.isNumber())
                        return obj->arr->get((int)idx->nval);
                    if (obj.isObject())
                        return obj->obj->get(idx.asString());
                    return nova_null();
                };
                setter = [obj, idx](Val v)
                {
                    if (obj.isArray() && idx.isNumber())
                        obj->arr->set((int)idx->nval, v);
                    else if (obj.isObject())
                        obj->obj->set(idx.asString(), v);
                };
            }
            else
            {
                // fallback — just return value
                return evaluate(target, s);
            }

            // Build a link object backed by getter/setter
            auto ptr = std::make_shared<NovaPointer>();
            ptr->readFn = getter;
            ptr->writeFn = setter;
            std::ostringstream addr;
            addr << "0x" << std::hex << (uintptr_t)ptr.get();
            ptr->address = addr.str();
            ptr->isLink = true;

            Val linkVal = std::make_shared<NovaValue>();
            linkVal->kind = VK::Pointer;
            linkVal->ptr = ptr;
            return linkVal;
        }
        case Node::Kind::RateCast:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_null();
            // Convert to target type
            const std::string &ct = n->castType;
            if (ct == "number")
                return nova_num(v.asNumber());
            if (ct == "string")
                return nova_str(v.asString());
            if (ct == "bool")
                return nova_bool(v.asBool());
            return v;
        }
        case Node::Kind::Deref:
        {
            Val v = n->operand ? evaluate(n->operand.get(), s) : nova_null();
            if (v.isPointer())
                return v->ptr->read();
            return v;
        }
        case Node::Kind::Partial:
        {
            Val fn = n->callee ? evaluate(n->callee.get(), s) : nova_null();
            ValVec partialArgs;
            for (auto &a : n->callArgs)
                partialArgs.push_back(a ? evaluate(a.get(), s) : nova_null());
            auto exe = this;
            return NovaValue::makeNative([exe, fn, partialArgs](ValVec args, std::shared_ptr<Scope> cs) -> Val
                                         {
                ValVec combined = partialArgs;
                combined.insert(combined.end(), args.begin(), args.end());
                return exe->callFunction(fn, combined, cs); }, "partial");
        }

        // ── http ──────────────────────────────────────────────────────────────
        case Node::Kind::HttpRequest:
            return _evalHttpRequest(n, s);
        case Node::Kind::FetchExpr:
            return _evalFetch(n, s);

        // ── options ───────────────────────────────────────────────────────────
        case Node::Kind::HasOpt:
        {
            auto it = options.find(n->name);
            return nova_bool(it != options.end() && _isTruthy(it->second));
        }
        case Node::Kind::GetOpt:
        {
            auto it = options.find(n->name);
            return it != options.end() ? it->second : nova_null();
        }
        case Node::Kind::OptDecl:
        {
            Val v = n->value ? evaluate(n->value.get(), s) : nova_bool(false);
            options[n->name] = v;
            return v;
        }

        default:
            // Fallback: try as statement
            return execute(n, s).value;
        }
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  EXPRESSION HELPERS
    // ════════════════════════════════════════════════════════════════════════════

    // ── binary ────────────────────────────────────────────────────────────────────

    Val Executor::_evalBinary(const Node *n, std::shared_ptr<Scope> s)
    {
        const std::string &op = n->op;

        // Short-circuit operators
        if (op == "&&")
        {
            Val l = evaluate(n->left.get(), s);
            return _isTruthy(l) ? evaluate(n->right.get(), s) : l;
        }
        if (op == "||")
        {
            Val l = evaluate(n->left.get(), s);
            return _isTruthy(l) ? l : evaluate(n->right.get(), s);
        }
        if (op == "??")
        {
            Val l = evaluate(n->left.get(), s);
            return (!l || l.isNull()) ? evaluate(n->right.get(), s) : l;
        }
        if (op == "and")
        {
            Val l = evaluate(n->left.get(), s);
            return _isTruthy(l) ? evaluate(n->right.get(), s) : l;
        }
        if (op == "or")
        {
            Val l = evaluate(n->left.get(), s);
            return _isTruthy(l) ? l : evaluate(n->right.get(), s);
        }

        // Namespace / member access: A::B
        // The right-hand side is a bare member name, not a variable to look
        // up — mirrors how `.` property access uses n->name directly rather
        // than evaluating it. Evaluating it as an expression (the old
        // behavior) looked up a variable named e.g. "B" in the current
        // scope, which is normally undefined, silently breaking `::`.
        if (op == "::")
        {
            Val left = evaluate(n->left.get(), s);
            std::string name;
            if (n->right && n->right->kind == Node::Kind::Ref)
                name = n->right->name;
            else if (n->right && n->right->kind == Node::Kind::Value)
                name = n->right->strval;
            else if (n->right)
                name = evaluate(n->right.get(), s).asString();
            return _getProp(left, name, s, false);
        }

        Val left = evaluate(n->left.get(), s);
        Val right = evaluate(n->right.get(), s);

        // Operator overloading
        if (left && left->overloads)
        {
            auto it = left->overloads->find("binary:" + op);
            if (it != left->overloads->end())
                return callFunction(it->second, {left, right}, s);
        }

        return _novaBinaryOp(op, left, right, n);
    }

    Val Executor::_novaBinaryOp(const std::string &op, Val left, Val right, const Node *n)
    {
        // ── BigInt / BigFloat arithmetic ──────────────────────────────────────────
        if (left.isBigInt() || right.isBigInt())
        {
            // coerce both to BigInt
            auto toBI = [](Val v) -> NovaBigInt
            {
                if (v.isBigInt())
                    return *v->bigint;
                return NovaBigInt((int64_t)v.asNumber());
            };
            NovaBigInt l = toBI(left), r = toBI(right);
            if (op == "+" || op == "plus")
                return NovaValue::makeBigInt(l + r);
            if (op == "-")
                return NovaValue::makeBigInt(l - r);
            if (op == "*")
                return NovaValue::makeBigInt(l * r);
            if (op == "/" || op == "div")
                return NovaValue::makeBigInt(l / r);
            if (op == "%")
                return NovaValue::makeBigInt(l % r);
            if (op == "**" || op == "pow")
            {
                uint64_t exp = (uint64_t)r.toDouble();
                return NovaValue::makeBigInt(l.pow(exp));
            }
            if (op == "==" || op == "===")
                return nova_bool(l == r);
            if (op == "!=" || op == "!==")
                return nova_bool(!(l == r));
            if (op == "<")
                return nova_bool(l < r);
            if (op == ">")
                return nova_bool(r < l);
            if (op == "<=")
                return nova_bool(!(r < l));
            if (op == ">=")
                return nova_bool(!(l < r));
            if (op == "cmp")
            {
                if (l < r)
                    return nova_num(-1);
                if (r < l)
                    return nova_num(1);
                return nova_num(0);
            }
            // bitwise — fall through to double for now
        }
        if (left.isBigFloat() || right.isBigFloat())
        {
            auto toBF = [](Val v) -> NovaBigFloat
            {
                if (v.isBigFloat())
                    return *v->bigfloat;
                if (v.isBigInt())
                    return NovaBigFloat(*v->bigint);
                return NovaBigFloat(v.asNumber());
            };
            NovaBigFloat l = toBF(left), r = toBF(right);
            if (op == "+" || op == "plus")
                return NovaValue::makeBigFloat(l + r);
            if (op == "-")
                return NovaValue::makeBigFloat(l - r);
            if (op == "*")
                return NovaValue::makeBigFloat(l * r);
            if (op == "/")
                return NovaValue::makeBigFloat(l / r);
            if (op == "==" || op == "===")
                return nova_bool(l == r);
            if (op == "!=" || op == "!==")
                return nova_bool(!(l == r));
            if (op == "<")
                return nova_bool(l < r);
            if (op == ">")
                return nova_bool(r < l);
            if (op == "<=")
                return nova_bool(!(r < l));
            if (op == ">=")
                return nova_bool(!(l < r));
        }
        // Arithmetic
        if (op == "+")
        {
            if ((left && left.isString()) || (right && right.isString()))
                return nova_str(left.asString() + right.asString());
            if (left && left.isArray())
            {
                auto new_arr = nova_arr(left->arr->inner);
                new_arr->arr->push(right);
                return new_arr;
            }
            if (left.isObject() && right.isObject())
            {
                auto out = left;
                for (auto &[k, v] : right->obj->inner)
                    out->obj->set(k, v);
                return out;
            }
            return nova_num(left.asNumber() + right.asNumber());
        }
        if (op == "-")
        {
            if (left && left.isString() && right.isNumber())
                return nova_str(left.asString().substr(0, left.asString().size() - right.asNumber()));
            if (left && left.isString())
            {
                std::string s = left.asString(), f = right.asString(), t = "";
                size_t pos = 0;
                while ((pos = s.find(f, pos)) != std::string::npos)
                {
                    s.replace(pos, f.size(), t);
                    pos += t.size();
                }
                return nova_str(s);
            }
            if (left.isArray())
            {
                ValVec out;
                for (auto &v : left->arr->inner)
                    if (!(*v == *right))
                        out.push_back(v);
                return nova_arr(out);
            }
            if (left.isObject() && right.isObject())
            {
                for (auto &[k, v] : right->obj->inner)
                    if (left->obj->has(k))
                        left->obj->del(k);
                return left;
            }
            return nova_num(left.asNumber() - right.asNumber());
        }
        if (op == "*")
        {
            // string repetition
            if (left && left.isString() && right && right.isNumber())
                return nova_str([&]
                                { std::string r; for (int i=0;i<(int)right->nval;i++) r+=left->sval; return r; }());
            return nova_num(left.asNumber() * right.asNumber());
        }
        if (op == "/")
        {
            double d = right.asNumber();
            if (d == 0)
                return nova_num(std::numeric_limits<double>::infinity());
            return nova_num(left.asNumber() / d);
        }
        if (op == "%")
            return nova_num(std::fmod(left.asNumber(), right.asNumber()));
        if (op == "**")
            return nova_num(std::pow(left.asNumber(), right.asNumber()));
        if (op == "pow")
            return nova_num(std::pow(left.asNumber(), right.asNumber()));

        // Bitwise
        if (op == "&")
            return nova_num((double)((long long)left.asNumber() & (long long)right.asNumber()));
        if (op == "|")
            return nova_num((double)((long long)left.asNumber() | (long long)right.asNumber()));
        if (op == "^")
            return nova_num((double)((long long)left.asNumber() ^ (long long)right.asNumber()));
        if (op == "<<")
            return nova_num((double)((long long)left.asNumber() << (int)right.asNumber()));
        if (op == ">>")
            return nova_num((double)((long long)left.asNumber() >> (int)right.asNumber()));
        if (op == ">>>")
            return nova_num((double)((uint64_t)left.asNumber() >> (int)right.asNumber()));
        if (op == "xor")
            return nova_num((double)((long long)left.asNumber() ^ (long long)right.asNumber()));

        // Comparison
        if (op == "==" || op == "===")
            return nova_bool(*left == *right);
        if (op == "!=" || op == "!==")
            return nova_bool(*left != *right);
        if (op == "<")
            return nova_bool(*left < *right);
        if (op == ">")
            return nova_bool(*right < *left);
        if (op == "<=")
            return nova_bool(!(*right < *left));
        if (op == ">=")
            return nova_bool(!(*left < *right));
        if (op == "bigger")
            return nova_bool(*right < *left);
        if (op == "smaller")
            return nova_bool(*left < *right);
        if (op == "equals")
            return nova_bool(*left == *right);
        if (op == "cmp")
        {
            if (*left < *right)
                return nova_num(-1);
            if (*right < *left)
                return nova_num(1);
            return nova_num(0);
        }
        if (op == "is")
        {
            if (right && right.isNull())
                return nova_bool(!left || left.isNull());
            return nova_bool(*left == *right);
        }
        if (op == "isnt")
            return nova_bool(!(*left == *right));
        if (op == "in")
        {
            if (right && right.isObject())
                return nova_bool(right->obj->has(left.asString()));
            if (right && right.isArray())
            {
                for (auto &v : right->arr->inner)
                    if (*v == *left)
                        return nova_bool(true);
                return nova_bool(false);
            }
            return nova_bool(false);
        }
        if (op == "instanceof")
        {
            // instance check via __class__ marker
            if (left && left.isObject())
            {
                Val cls = left->obj->get("__class__");
                if (cls && right && right.isObject())
                {
                    Val clsName = right->obj->get("__name__");
                    if (cls && clsName)
                        return nova_bool(cls.asString() == clsName.asString());
                }
            }
            return nova_bool(false);
        }
        if (op == "istypeof")
            return nova_bool(_typeOf(left) == right.asString());
        if (op == "matches")
        {
            if (right && right.isObject())
            {
                Val src = right->obj->get("source");
                Val flgsV = right->obj->get("flags");
                if (src)
                {
                    try
                    {
                        std::string flgs = flgsV ? flgsV.asString() : "";
                        auto rxFlags = std::regex_constants::ECMAScript;
                        for (char f : flgs)
                        {
                            if (f == 'i')
                                rxFlags |= std::regex_constants::icase;
                            if (f == 'm')
                                rxFlags |= std::regex_constants::multiline;
                        }
                        std::regex re(src.asString(), rxFlags);
                        return nova_bool(std::regex_search(left.asString(), re));
                    }
                    catch (...)
                    {
                    }
                }
            }
            return nova_bool(false);
        }
        if (op == "between")
        {
            // a between [lo, hi] or a between range
            if (right && right.isArray() && right->arr->length() == 2)
            {
                double lo = right->arr->get(0).asNumber();
                double hi = right->arr->get(1).asNumber();
                return nova_bool(left.asNumber() >= lo && left.asNumber() <= hi);
            }
            if (right && right.isRange())
                return nova_bool(right->range->includes(left.asNumber()));
            return nova_bool(false);
        }

        // Range
        if (op == "..")
        {
            double step = 1;
            return NovaValue::makeRange(left.asNumber(), right.asNumber(), step);
        }
        if (op == "step")
        {
            if (left && left.isRange())
            {
                left->range->step = right.asNumber();
                return left;
            }
            return nova_null();
        }

        // Pipe
        if (op == "|>")
            return callFunction(right, {left}, globalScope);

        // Array ops
        if (op == "concat")
        {
            if (left.isArray() && right.isArray())
            {
                ValVec out = left->arr->inner;
                for (auto &v : right->arr->inner)
                    out.push_back(v);
                return nova_arr(std::move(out));
            }
            return nova_str(left.asString() + right.asString());
        }
        if (op == "zip")
        {
            if (!left.isArray() || !right.isArray())
                return nova_null();
            ValVec out;
            size_t sz = std::min(left->arr->length(), right->arr->length());
            for (size_t i = 0; i < sz; i++)
                out.push_back(nova_arr({left->arr->get(i), right->arr->get(i)}));
            return nova_arr(std::move(out));
        }
        if (op == "intersect")
        {
            if (!left.isArray() || !right.isArray())
                return nova_null();
            ValVec out;
            for (auto &v : left->arr->inner)
                for (auto &u : right->arr->inner)
                    if (*v == *u)
                    {
                        out.push_back(v);
                        break;
                    }
            return nova_arr(std::move(out));
        }
        if (op == "union")
        {
            if (!left.isArray() || !right.isArray())
                return nova_null();
            ValVec out = left->arr->inner;
            for (auto &v : right->arr->inner)
            {
                bool found = false;
                for (auto &u : out)
                    if (*v == *u)
                    {
                        found = true;
                        break;
                    }
                if (!found)
                    out.push_back(v);
            }
            return nova_arr(std::move(out));
        }
        if (op == "diff_arr")
        {
            if (!left.isArray() || !right.isArray())
                return nova_null();
            ValVec out;
            for (auto &v : left->arr->inner)
            {
                bool found = false;
                for (auto &u : right->arr->inner)
                    if (*v == *u)
                    {
                        found = true;
                        break;
                    }
                if (!found)
                    out.push_back(v);
            }
            return nova_arr(std::move(out));
        }
        // Math helpers
        if (op == "avg")
        {
            if (!left.isArray())
                return nova_num((left.asNumber() + right.asNumber()) / 2.0);
            double sum = 0;
            for (auto &v : left->arr->inner)
                sum += v.asNumber();
            return nova_num(left->arr->inner.empty() ? 0 : sum / left->arr->inner.size());
        }
        if (op == "diff")
            return nova_num(std::abs(left.asNumber() - right.asNumber()));
        if (op == "ratio")
            return nova_num(right.asNumber() == 0 ? 0 : left.asNumber() / right.asNumber());
        if (op == "mult_of")
            return nova_bool(std::fmod(left.asNumber(), right.asNumber()) == 0);
        if (op == "gcd")
        {
            long long a = (long long)std::abs(left.asNumber()), b = (long long)std::abs(right.asNumber());
            while (b)
            {
                long long t = b;
                b = a % b;
                a = t;
            }
            return nova_num((double)a);
        }
        if (op == "lcm")
        {
            long long a = (long long)std::abs(left.asNumber()), b = (long long)std::abs(right.asNumber());
            if (a == 0 || b == 0)
                return nova_num(0);
            long long g = a;
            long long tmp = b;
            while (tmp)
            {
                long long t = tmp;
                tmp = g % tmp;
                g = t;
            }
            return nova_num((double)(a / g * b));
        }

        // String ops
        if (op == "pad_start")
            return nova_str([&]
                            {
        std::string r = left.asString(); int w = (int)right.asNumber();
        while ((int)r.size() < w) r = " " + r;
        return r; }());
        if (op == "pad_end")
            return nova_str([&]
                            {
        std::string r = left.asString(); int w = (int)right.asNumber();
        while ((int)r.size() < w) r += " ";
        return r; }());
        if (op == "equals_ignore")
            return nova_bool([&]
                             {
        std::string a = left.asString(), b = right.asString();
        std::transform(a.begin(),a.end(),a.begin(),::tolower);
        std::transform(b.begin(),b.end(),b.begin(),::tolower);
        return a == b; }());
        if (op == "extend")
        {
            if (left.isObject() && right.isObject())
            {
                for (auto &[k, v] : right->obj->inner)
                    left->obj->set(k, v);
                return left;
            }
            return left;
        }
        if (op == "nand")
            return nova_bool(!(_isTruthy(left) && _isTruthy(right)));
        if (op == "nor")
            return nova_bool(!(_isTruthy(left) || _isTruthy(right)));
        if (op == "xnor")
            return nova_bool(_isTruthy(left) == _isTruthy(right));

        // Namespace / stream access — only reached when _novaBinaryOp is called
        // directly with already-evaluated values (e.g. the generic binaryOp
        // builtin), since normal `A::B` expressions are now short-circuited
        // in _evalBinary before reaching here (see the comment there).
        if (op == "::")
        {
            return _getProp(left, right.asString(), std::shared_ptr<Scope>(), false);
        }

        _error("Unknown binary operator: " + op, n);
    }

    // ── unary ─────────────────────────────────────────────────────────────────────
    // ── unary ─────────────────────────────────────────────────────────────────────

    Val Executor::_evalUnary(const Node *n, std::shared_ptr<Scope> s)
    {
        const std::string &op = n->op;

        // delete / void need the raw node, handle before eval
        if (op == "delete")
        {
            if (n->operand && n->operand->kind == Node::Kind::Prop)
            {
                Val obj = evaluate(n->operand->left.get(), s);
                if (obj && obj.isObject())
                {
                    obj->obj->del(n->operand->name);
                    return nova_bool(true);
                }
            }
            if (n->operand && n->operand->kind == Node::Kind::Ref)
            {
                s->del(n->operand->name);
                return nova_bool(true);
            }
            return nova_bool(false);
        }
        if (op == "void")
        {
            evaluate(n->operand.get(), s);
            return nova_null();
        }

        Val v = evaluate(n->operand.get(), s);

        // overload check — key: "unary:<op>"
        if (v && v->overloads)
        {
            auto it = v->overloads->find("unary:" + op);
            if (it != v->overloads->end())
                return callFunction(it->second, {v}, s);
        }

        if (v.isBigInt())
        {
            if (op == "-")
                return NovaValue::makeBigInt(v->bigint->negate());
            if (op == "+" || op == "typeof")
            {
            } // fall through
        }
        if (v.isBigFloat())
        {
            if (op == "-")
            {
                NovaBigFloat neg = *v->bigfloat;
                neg.mantissa.negative = !neg.mantissa.negative && !neg.mantissa.isZero();
                return NovaValue::makeBigFloat(std::move(neg));
            }
        }

        if (op == "not" || op == "!")
            return nova_bool(!_isTruthy(v));
        if (op == "-")
            return nova_num(-v.asNumber());
        if (op == "+")
            return nova_num(v.asNumber());
        if (op == "~")
            return nova_num((double)(~(long long)v.asNumber()));
        if (op == "typeof" || op == "istypeof")
            return nova_str(_typeOf(v));

        _error("Unknown unary operator: " + op, n);
    }
    // ── call ──────────────────────────────────────────────────────────────────────

    Val Executor::_evalCall(const Node *n, std::shared_ptr<Scope> s)
    {
        const bool isOptional = (n->kind == Node::Kind::OptionalCall);

        // Collect arguments (handle spread)
        ValVec args;
        for (auto &a : n->callArgs)
        {
            if (!a)
                continue;
            if (a->kind == Node::Kind::Spread)
            {
                Val sv = evaluate(a->value.get(), s);
                if (sv && sv.isArray())
                    for (auto &v : sv->arr->inner)
                        args.push_back(v);
                else
                    args.push_back(sv);
            }
            else
            {
                args.push_back(evaluate(a.get(), s));
            }
        }

        // Evaluate callee
        Val fn;
        Val thisVal = nova_null();
        if (n->callee && n->callee->kind == Node::Kind::Prop)
        {
            thisVal = evaluate(n->callee->left.get(), s);
            std::string key = n->callee->name;
            fn = _getProp(thisVal, key, s, false);
        }
        else if (n->callee && n->callee->kind == Node::Kind::OptionalProp)
        {
            thisVal = evaluate(n->callee->left.get(), s);
            if (!thisVal || thisVal.isNull())
                return nova_null();
            fn = _getProp(thisVal, n->callee->name, s, false);
        }
        else if (n->callee)
        {
            fn = evaluate(n->callee.get(), s);
        }

        if (isOptional && (!fn || fn.isNull()))
            return nova_null();

        // class call — A() same as new A()
        if (fn && fn.isClass())
            return _instantiateClass(fn, args);

        if (!fn || !fn.isFunction())
        {
            if (fn && fn->overloads)
            {
                auto it = fn->overloads->find("call");
                if (it != fn->overloads->end())
                    return callFunction(it->second, args, s);
            }
            _error("Not a function: " + (n->callee ? n->callee->name : "?"), n);
        }

        return callFunction(fn, args, s, thisVal); // ← pass thisVal
    }

    // ── struct call ───────────────────────────────────────────────────────────────
    // Handles `Name(...) { field: val, ... }` — a normal call whose trailing
    // `{...}` supplies (or augments) the leading object/field-map argument.
    // This is how struct constructors (see _execStructDecl) are meant to be
    // invoked with named fields, but it works for any callee.
    Val Executor::_evalStructCall(const Node *n, std::shared_ptr<Scope> s)
    {
        Val fields = n->value ? evaluate(n->value.get(), s) : nova_obj();

        // Collect call arguments (spread-aware), same as a normal call.
        ValVec args;
        for (auto &a : n->callArgs)
        {
            if (!a)
                continue;
            if (a->kind == Node::Kind::Spread)
            {
                Val sv = evaluate(a->value.get(), s);
                if (sv && sv.isArray())
                    for (auto &v : sv->arr->inner)
                        args.push_back(v);
                else
                    args.push_back(sv);
            }
            else
            {
                args.push_back(evaluate(a.get(), s));
            }
        }

        // Merge the `{ ... }` fields into a leading object argument (creating
        // one if none was passed), matching how struct constructors read
        // args[0] as a field map.
        if (!args.empty() && args[0] && args[0].isObject())
        {
            if (fields && fields.isObject())
                for (auto &[k, v] : fields->obj->inner)
                    args[0]->obj->set(k, v);
        }
        else
        {
            args.insert(args.begin(), fields);
        }

        // Evaluate callee (mirrors _evalCall's callee resolution)
        Val fn;
        Val thisVal = nova_null();
        if (n->callee && n->callee->kind == Node::Kind::Prop)
        {
            thisVal = evaluate(n->callee->left.get(), s);
            fn = _getProp(thisVal, n->callee->name, s, false);
        }
        else if (n->callee && n->callee->kind == Node::Kind::OptionalProp)
        {
            thisVal = evaluate(n->callee->left.get(), s);
            if (!thisVal || thisVal.isNull())
                return nova_null();
            fn = _getProp(thisVal, n->callee->name, s, false);
        }
        else if (n->callee)
        {
            fn = evaluate(n->callee.get(), s);
        }

        // struct call — A(){...} same as new A({...})
        if (fn && fn.isClass())
            return _instantiateClass(fn, args);

        if (!fn || !fn.isFunction())
        {
            if (fn && fn->overloads)
            {
                auto it = fn->overloads->find("call");
                if (it != fn->overloads->end())
                    return callFunction(it->second, args, s);
            }
            _error("Not a struct/function: " + (n->callee ? n->callee->name : "?"), n);
        }

        return callFunction(fn, args, s, thisVal);
    }

    // ── property access ───────────────────────────────────────────────────────────

    Val Executor::_evalProp(const Node *n, std::shared_ptr<Scope> s, bool isOptional = false)
    {
        Val obj = n->left ? evaluate(n->left.get(), s) : nova_null();
        if (n->kind == Node::Kind::OptionalProp && (!obj || obj.isNull()))
            return nova_null();
        return _getProp(obj, n->name, s, isOptional);
    }
    Val Executor::_evalSubscript(const Node *n, std::shared_ptr<Scope> s)
    {
        Val obj = n->left ? evaluate(n->left.get(), s) : nova_null();
        if (n->kind == Node::Kind::OptionalSubscript && (!obj || obj.isNull()))
            return nova_null();
        Val idx = n->index ? evaluate(n->index.get(), s) : nova_null();
        if (!obj)
            return nova_null();

        const bool isOpt = (n->kind == Node::Kind::OptionalSubscript); // ← derive once

        if (obj->overloads)
        {
            auto it = obj->overloads->find("subscript:get");
            if (it != obj->overloads->end())
                return callFunction(it->second, {obj, idx}, s);
        }

        if (obj.isArray())
        {
            if (idx.isNumber())
                return obj->arr->get((int)idx->nval);
            return _getProp(obj, idx.asString(), s, isOpt); // ← was false
        }
        if (obj.isString())
        {
            if (idx.isNumber())
            {
                std::string cp = utf8::at(obj->sval, (int)idx->nval);
                return cp.empty() ? nova_null() : nova_str(cp);
            }
            return _getProp(obj, idx.asString(), s, isOpt); // ← was false
        }
        return _getProp(obj, idx.asString(), s, isOpt); // ← was false
    }
    // ── assign ────────────────────────────────────────────────────────────────────

    Val Executor::_evalAssign(const Node *n, std::shared_ptr<Scope> s)
    {
        Val rhs = n->value ? evaluate(n->value.get(), s) : nova_null();
        const Node *target = n->left.get();

        // Compound assign
        if (n->kind == Node::Kind::CompoundAssign && !n->op.empty() && n->op != "=")
        {
            Val lhs = evaluate(target, s);
            // strip '=' suffix
            std::string binop = n->op.substr(0, n->op.size() - 1);
            rhs = _novaBinaryOp(binop, lhs, rhs, n);
        }

        if (target->kind == Node::Kind::Ref)
        {
            Val existing = s->get(target->name);
            if (existing && existing.isPointer() && existing->ptr->isLink)
                existing->ptr->write(rhs);
            else if (existing && existing.isPointer() && !existing->ptr->isLink)
                existing->ptr->write(rhs);
            else
                s->set(target->name, rhs);
        }
        else if (target->kind == Node::Kind::Deref)
        {
            // *p = val — write through the pointer
            Val ptr = target->operand ? evaluate(target->operand.get(), s) : nova_null();
            if (ptr && ptr.isPointer() && ptr->ptr->writeFn)
                ptr->ptr->write(rhs);
        }
        else if (target->kind == Node::Kind::Link)
        {
            // link(x) = val — resolve the link and write through
            Val lnk = evaluate(target, s);
            if (lnk && lnk.isPointer() && lnk->ptr->writeFn)
                lnk->ptr->writeFn(rhs);
        }
        else if (target->kind == Node::Kind::Prop)
        {
            Val obj = evaluate(target->left.get(), s);
            _setProp(obj, target->name, rhs);
        }
        else if (target->kind == Node::Kind::Subscript)
        {
            Val obj = evaluate(target->left.get(), s);
            Val idx = evaluate(target->index.get(), s);

            // overload — key: "subscript:set"
            if (obj->overloads)
            {
                auto it = obj->overloads->find("subscript:set");
                if (it != obj->overloads->end())
                {
                    callFunction(it->second, {obj, idx, rhs}, s);
                    return rhs;
                }
            }

            if (obj.isArray())
                obj->arr->set((int)idx.asNumber(), rhs);
            else if (obj.isObject())
                obj->obj->set(idx.asString(), rhs);
            else if (obj.isStruct())
                obj->strct->set(idx.asString(), rhs);
        }
        else if (target->kind == Node::Kind::DVar)
        {
            // writing to dvar — no-op for most, allowed for __iter__ etc.
        }
        else
        {
            // destructure on left side
            if (std::holds_alternative<ObjPattern>(target->destructure))
                _destructureObj(std::get<ObjPattern>(target->destructure), rhs, s, false);
            else if (std::holds_alternative<ArrPattern>(target->destructure))
                _destructureArr(std::get<ArrPattern>(target->destructure), rhs, s, false);
        }
        return rhs;
    }

    // ── f-string ──────────────────────────────────────────────────────────────────

    Val Executor::_evalFString(const Node *n, std::shared_ptr<Scope> s)
    {
        std::string out;
        for (auto &part : n->parts)
        {
            if (!part)
                continue;
            if (part->kind == Node::Kind::Value)
            {
                out += part->strval;
                continue;
            }
            Val v = evaluate(part.get(), s);
            out += stringify(v);
        }
        return nova_str(out);
    }

    // ── object literal ────────────────────────────────────────────────────────────

    Val Executor::_evalObject(const Node *n, std::shared_ptr<Scope> s)
    {
        auto obj = nova_obj();
        for (auto &p : n->props)
        {
            switch (p.kind)
            {
            case ObjectProp::Kind::Spread:
            {
                Val sv = p.value ? evaluate(p.value.get(), s) : nova_null();
                if (sv && sv.isObject())
                    for (auto &[k, v] : sv->obj->inner)
                        obj->obj->set(k, v);
                else if (sv && sv.isStruct())
                    for (auto &[k, v] : sv->strct->inner)
                        obj->obj->set(k, v);
                break;
            }
            case ObjectProp::Kind::Computed:
            {
                Val k = p.keyExpr ? evaluate(p.keyExpr.get(), s) : nova_null();
                Val v = p.value ? evaluate(p.value.get(), s) : nova_null();
                obj->obj->set(k.asString(), v);
                break;
            }
            default:
            {
                Val v = p.value ? evaluate(p.value.get(), s) : nova_null();
                obj->obj->set(p.key, v);
                break;
            }
            }
        }
        return obj;
    }

    // ── array literal ─────────────────────────────────────────────────────────────

    Val Executor::_evalArray(const Node *n, std::shared_ptr<Scope> s)
    {
        ValVec out;
        for (auto &e : n->elements)
        {
            if (!e)
                continue;
            if (e->kind == Node::Kind::Spread)
            {
                Val sv = e->value ? evaluate(e->value.get(), s) : nova_null();
                if (sv && sv.isArray())
                    for (auto &v : sv->arr->inner)
                        out.push_back(v);
                else if (sv && sv.isRange())
                {
                    auto elems = sv->range->toArray();
                    for (auto &v : elems)
                        out.push_back(v);
                }
                else
                    out.push_back(sv);
            }
            else
            {
                out.push_back(evaluate(e.get(), s));
            }
        }
        return nova_arr(std::move(out));
    }

    // ── arrow function ────────────────────────────────────────────────────────────

    Val Executor::_evalArrow(const Node *n, std::shared_ptr<Scope> s)
    {
        // Build a synthetic function node
        auto fn = std::make_shared<Node>(*n);
        fn->kind = Node::Kind::Function;
        fn->funcArgs.clear();
        for (auto &name : n->names)
        {
            FuncArg a;
            a.name = name;
            fn->funcArgs.push_back(a);
        }

        Val v = NovaValue::makeFunction(fn, s);
        v->fn->isAsync = n->isAsync;
        v->fn->isGenerator = n->isGenerator;
        return v;
    }

    // ── new ───────────────────────────────────────────────────────────────────────

    Val Executor::_evalNew(const Node *n, std::shared_ptr<Scope> s)
    {
        Val classDef = n->callee ? evaluate(n->callee.get(), s) : nova_null();
        ValVec args;
        for (auto &a : n->callArgs)
            args.push_back(a ? evaluate(a.get(), s) : nova_null());
        return _instantiateClass(classDef, args);
    }

    Val Executor::_instantiateClass(Val classDef, ValVec args)
    {
        if (!classDef || !classDef.isClass())
            _error("Not a class");

        auto &cls = *classDef->cls;
        auto instance = nova_obj();
        instance->obj->set("__class__", nova_str(cls.name));

        // inherit from super first
        if (cls.superClass && cls.superClass.isClass())
        {
            for (auto &[k, v] : cls.superClass->cls->methods)
                if (k != "constructor")
                    instance->obj->set(k, v);
        }

        // copy instance methods onto instance
        for (auto &[k, v] : cls.methods)
            if (k != "constructor")
                instance->obj->set(k, v);

        // call constructor with this = instance
        auto ctorIt = cls.methods.find("constructor");
        if (ctorIt != cls.methods.end())
        {
            Val ctor = ctorIt->second;
            if (ctor && ctor.isFunction() && ctor->fn && ctor->fn->node)
            {
                auto ctorScope = std::make_shared<Scope>(
                    Scope::Kind::Function, cls.closureScope, globalScope);
                ctorScope->funcName = cls.name;
                ctorScope->setOwn("this", instance);
                if (cls.superClass && cls.superClass.isClass())
                    ctorScope->setOwn("Super", _buildSuperProxy(cls.superClass, instance));

                auto &node = *ctor->fn->node;
                for (int i = 0; i < (int)node.funcArgs.size(); i++)
                {
                    const FuncArg &fa = node.funcArgs[i];
                    Val val = i < (int)args.size() ? args[i] : nova_null();
                    if (fa.defaultValue && (!val || val.isNull()))
                        val = evaluate(fa.defaultValue.get(), ctorScope);
                    ctorScope->setOwn(fa.name, val);
                }

                _runBody(node.body, ctorScope); // ExecResult discarded — ctor has no meaningful return
            }
        }

        return instance;
    }

    // ── await ─────────────────────────────────────────────────────────────────────
    Val Executor::_evalAwait(const Node *n, std::shared_ptr<Scope> s)
    {
        Val v = n->operand ? evaluate(n->operand.get(), s) : nova_null();

        // call the injected __await__ if we're inside an async fiber
        Val awaitFn = s->get("__await__");
        if (awaitFn && awaitFn.isFunction())
            return callFunction(awaitFn, {v}, s);

        // fallback for top-level await — drive async task to completion,
        // correctly chaining through any nested async task it awaits along
        // the way (see driveAsyncTask for why the naive resume-with-null
        // loop silently drops nested await results).
        if (v && v.isObject())
        {
            Val driveFn = v->obj->get("__drive__");
            if (driveFn && driveFn.isFunction())
                return driveAsyncTask(this, v, s);

            Val result = v->obj->get("__result__");
            if (result)
                return result;
        }
        return v;
    }
    // ── perform (algebraic effect) ────────────────────────────────────────────────
    Val Executor::_evalPerform(const Node *n, std::shared_ptr<Scope> s)
    {
        ValVec args;
        for (auto &a : n->callArgs)
            args.push_back(a ? evaluate(a.get(), s) : nova_null());

        auto it = _performHandlers.find(n->name);
        if (it != _performHandlers.end() && it->second && it->second.isFunction())
        {
            ValVec yargs = {nova_str(n->name)};
            yargs.insert(yargs.end(), args.begin(), args.end());
            return callFunction(it->second, yargs, s);
        }

        _error("perform used outside a handle block", n);
    }
    // ── dvar ──────────────────────────────────────────────────────────────────────

    Val Executor::_evalDVar(const Node *n, std::shared_ptr<Scope> s)
    {
        const std::string &name = n->name;

        if (name == "default")
            return DEFAULT_SENTINEL();

        if (name == "__line__")
            return nova_num(n->srcLine > 0 ? n->srcLine : n->line);
        if (name == "__col__")
            return nova_num(n->srcCol > 0 ? n->srcCol : n->column);
        if (name == "__file__")
            return nova_str(filename);

        if (name == "__date__")
        {
            auto t = std::time(nullptr);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
            return nova_str(buf);
        }
        if (name == "__time__")
        {
            auto t = std::time(nullptr);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
            return nova_str(buf);
        }
        if (name == "__datetime__")
        {
            auto t = std::time(nullptr);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
            return nova_str(buf);
        }
        if (name == "__timestamp__")
        {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            return nova_num((double)std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        }
        if (name == "__version__")
            return nova_str("novac-0.1.0");
        if (name == "__platform__")
        {
#if defined(_WIN32)
            return nova_str("windows");
#elif defined(__APPLE__)
            return nova_str("macos");
#elif defined(__linux__)
            return nova_str("linux");
#else
            return nova_str("unknown");
#endif
        }
        if (name == "__random__")
            return nova_num((double)std::rand() / RAND_MAX);
        if (name == "__iter__")
            return nova_num(s->loopIndex);
        if (name == "__caller__")
            return nova_str(s->funcName);
        if (name == "__module__")
            return nova_str(filename);
        if (name == "__namespace__")
        {
            Scope *cur = s.get();
            while (cur)
            {
                if (cur->kind == Scope::Kind::Namespace)
                    return nova_str(cur->namespaceName);
                cur = cur->parent.get();
            }
            return nova_null();
        }
        if (name == "__scope__")
        {
            auto snap = s->snapshot();
            auto obj = nova_obj();
            for (auto &[k, v] : snap)
                obj->obj->set(k, v);
            return obj;
        }
        if (name == "__stack__")
            return nova_arr(); // simplified
        if (name == "__uuid__")
        {
            // UUID v4 (simplified, not cryptographically random)
            std::ostringstream ss;
            ss << std::hex;
            for (int i = 0; i < 8; i++)
                ss << (std::rand() % 16);
            ss << '-';
            for (int i = 0; i < 4; i++)
                ss << (std::rand() % 16);
            ss << "-4";
            for (int i = 0; i < 3; i++)
                ss << (std::rand() % 16);
            ss << '-';
            int v = 8 + (std::rand() % 4);
            ss << v;
            for (int i = 0; i < 3; i++)
                ss << (std::rand() % 16);
            ss << '-';
            for (int i = 0; i < 12; i++)
                ss << (std::rand() % 16);
            return nova_str(ss.str());
        }
        if (name == "__env__")
        {
            Val env = s->get("process");
            if (env && env.isObject())
                return env->obj->get("env");
            return nova_obj();
        }

        return nova_null();
    }

    // ── http request ─────────────────────────────────────────────────────────────

    Val Executor::_evalHttpRequest(const Node *n, std::shared_ptr<Scope> s)
    {
        Val url = n->url ? evaluate(n->url.get(), s) : nova_null();
        Val body = n->callArgs.empty() ? nullptr : evaluate(n->callArgs[0].get(), s);
        auto opts = nova_obj();
        opts->obj->set("method", nova_str(n->method));
        if (body)
            opts->obj->set("body", body);
        return _syncFetch(url.asString(), opts);
    }

    Val Executor::_evalFetch(const Node *n, std::shared_ptr<Scope> s)
    {
        Val url = n->url ? evaluate(n->url.get(), s) : nova_null();
        Val opts = n->options ? evaluate(n->options.get(), s) : nullptr;
        return _syncFetch(url.asString(), opts);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  callFunction
    // ════════════════════════════════════════════════════════════════════════════

    Val Executor::callFunction(Val fn, ValVec args, std::shared_ptr<Scope> callerScope, Val thisVal)
    {
        if (!fn || !fn.isFunction())
            _error("Attempt to call a non-function value");

        NovaFunction &f = *fn->fn;

        // Once functions
        if (f.once)
        {
            f.execCount++;
            if (f.execCount > 1)
                return nova_null();
        }

        // Memoize
        if (f.memoize && f.memoCache)
        {
            std::ostringstream key;
            for (auto &a : args)
                key << stringify(a) << "|";
            auto it = f.memoCache->find(key.str());
            if (it != f.memoCache->end())
                return it->second;
        }

        // Native function
        if (f.isNative)
        {
            Val result = f.native(args, callerScope ? callerScope : globalScope);
            if (f.memoize && f.memoCache)
            {
                std::ostringstream key;
                for (auto &a : args)
                    key << stringify(a) << "|";
                (*f.memoCache)[key.str()] = result;
            }
            return result ? result : nova_null();
        }

        // AST function
        if (!f.node)
            return nova_null();
        Val result = runFunctionNode(
            f.node.get(),
            f.closureScope ? f.closureScope : globalScope,
            args, f.strictArgs, thisVal); // ← add thisVal
        if (f.memoize && f.memoCache)
        {
            std::ostringstream key;
            for (auto &a : args)
                key << stringify(a) << "|";
            (*f.memoCache)[key.str()] = result;
        }
        return result;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  runFunctionNode
    // ════════════════════════════════════════════════════════════════════════════

    Val Executor::runFunctionNode(const Node *node, std::shared_ptr<Scope> closure,
                                  ValVec args, bool strictArgs, Val thisVal)
    {
        auto funcScope = std::make_shared<Scope>(
            Scope::Kind::Function, closure, globalScope);
        funcScope->funcName = node->name;

        if (thisVal)
            funcScope->setOwn("this", thisVal); // ← bind this
        if (thisVal && thisVal.isObject())
        {
            Val clsNameVal = thisVal->obj->get("__class__");
            if (clsNameVal && clsNameVal.isString())
            {
                auto cit = classRegistry.find(clsNameVal->sval);
                if (cit != classRegistry.end() && cit->second.isClass())
                {
                    Val superClassVal = cit->second->cls->superClass;
                    if (superClassVal && superClassVal.isClass())
                        funcScope->setOwn("Super", _buildSuperProxy(superClassVal, thisVal));
                }
            }
        }

        if (strictArgs && node->funcArgs.size() != args.size())
            _error("Argument count mismatch: expected " + std::to_string(node->funcArgs.size()) +
                   " arguments, got " + std::to_string(args.size()));
        for (int i = 0; i < (int)node->funcArgs.size(); i++)
        {
            const FuncArg &fa = node->funcArgs[i];
            Val val;

            if (fa.rest)
            {
                // collect remaining args into array
                ValVec rest;
                for (int j = i; j < (int)args.size(); j++)
                    rest.push_back(args[j]);
                val = nova_arr(std::move(rest));
            }
            else
            {
                if (i < (int)args.size() && !isDefault(args[i]))
                {
                    val = args[i];
                }
                else if (fa.defaultValue)
                {
                    val = evaluate(fa.defaultValue.get(), funcScope);
                }
                else
                {
                    val = nova_null();
                }
            }

            // Type check arg
            if (fa.type && val && !types.check(val, *fa.type))
                _error("Argument '" + fa.name + "': type mismatch");

            funcScope->setOwn(fa.name, val);
            if (fa.rest)
                break;
        }

        // async function → wrap in task
        if (node->isAsync)
            return _runAsync(node, funcScope, args);

        // generator function → wrap in generator object
        if (node->isGenerator)
            return _makeGenerator(node, funcScope, args);

        // Normal function — run body; ExecResult absorbs ordinary returns here.
        // A stray break/continue inside a function body can no longer leak to
        // the caller's enclosing loop, since they're not C++ exceptions anymore.
        ExecResult r = _runBody(node->body, funcScope);
        return r.value ? r.value : nova_null();
    }

    // ── async runner ──────────────────────────────────────────────────────────────
    Val Executor::_runAsync(const Node *fnNode, std::shared_ptr<Scope> s, ValVec)
    {
        auto fiber = std::make_shared<Fiber>();
        auto exe = this;
        auto nodeRef = fnNode;
        auto scope = s;

        fiber->body = [exe, nodeRef, scope](Fiber &f)
        {
            // inject await into scope — suspends fiber until value is ready
            scope->setOwn("__await__", NovaValue::makeNative([&f](ValVec a, auto) -> Val
                                                             {
            Val awaited = a.empty() ? nova_null() : a[0];

            // if awaited is itself an async task object, drive it to completion
            if (awaited && awaited.isObject()) {
                Val isAsync = awaited->obj->get("__async__");
                if (isAsync && isAsync.asBool()) {
                    // drive nested async fiber
                    Val thenFn = awaited->obj->get("__drive__");
                    if (thenFn && thenFn.isFunction()) {
                        // nested async — suspend ourselves, let scheduler drive it
                        f.yieldedValue = awaited;
                        f.suspend();
                        return f.sentValue ? f.sentValue : nova_null();
                    }
                    Val result = awaited->obj->get("__result__");
                    return result ? result : nova_null();
                }
            }
            // plain value — no suspension needed
            return awaited; }, "__await__"));

            ExecResult r;
            try
            {
                r = exe->_runBody(nodeRef->body, scope);
            }
            catch (ReturnSignal &rs)
            {
                // Safety net for `yield` used outside any generator
                r = ExecResult::Returned(rs.value, rs.hard);
            }
            f.yieldedValue = r.value;
        };

        fiberInit(*fiber);

        // run to first suspension or completion
        fiber->resume();

        auto taskObj = nova_obj();
        taskObj->obj->set("__async__", nova_bool(true));
        // Only store __result__ when the fiber finished immediately (no internal
        // awaits). If it suspended, yieldedValue holds an intermediate value
        // (e.g. the inner task object) — NOT the final return value.
        taskObj->obj->set("__result__", fiber->done ? fiber->yieldedValue : nova_null());
        taskObj->obj->set("__done__", nova_bool(fiber->done));

        // __drive__ — advance the fiber one step (used by await on nested tasks)
        // Before resuming, check what the fiber last yielded. If it suspended
        // waiting on a nested async task (the __await__ injection does exactly
        // this — yields the inner task object and suspends), drive that inner
        // task to completion first and use its result as sentValue. Without
        // this check, the caller (driveAsyncTask) would send null back in,
        // which is what __await__ returns to the `await expr` inside the
        // function body — silently making every nested await evaluate to null.
        taskObj->obj->set("__drive__", NovaValue::makeNative([fiber, exe](ValVec a, auto cs) -> Val
                                                             {
        if (fiber->done) return fiber->yieldedValue;

        Val resumeWith = a.empty() ? nova_null() : a[0];

        // If no explicit resumeWith was provided, check whether the fiber
        // is suspended on a nested async task and resolve it now.
        if (!resumeWith || resumeWith.isNull())
        {
            Val yielded = fiber->yieldedValue;
            if (yielded && yielded.isObject())
            {
                Val nestedAsync = yielded->obj->get("__async__");
                if (nestedAsync && nestedAsync.asBool())
                    resumeWith = driveAsyncTask(exe, yielded, cs);
            }
        }

        fiber->sentValue = resumeWith;
        fiber->resume();
        return fiber->yieldedValue; }, "__drive__"));

        // __isDone__ — live check of fiber completion (not a frozen snapshot like __done__)
        taskObj->obj->set("__isDone__", NovaValue::makeNative([fiber](ValVec, auto) -> Val
                                                              { return nova_bool(fiber->done); }, "__isDone__"));

        auto exe2 = this;
        taskObj->obj->set("then", NovaValue::makeNative([fiber, exe2](ValVec a, auto cs) -> Val
                                                        {
        // Drive to completion, chaining through any nested task this
        // fiber is awaiting (same logic as driveAsyncTask, applied
        // directly to `fiber` so the lambda doesn't need to capture
        // `taskObj` itself — capturing taskObj here would create a
        // self-referential shared_ptr cycle via this very closure).
        Val resumeWith = nova_null();
        Val result = nova_null();
        while (!fiber->done) {
            fiber->sentValue = resumeWith;
            fiber->resume();
            resumeWith = nova_null();
            result = fiber->yieldedValue;
            if (fiber->done)
                break;
            if (result && result.isObject()) {
                Val nestedAsync = result->obj->get("__async__");
                if (nestedAsync && nestedAsync.asBool())
                    resumeWith = driveAsyncTask(exe2, result, cs);
            }
        }
        result = fiber->yieldedValue;
        if (!a.empty() && a[0] && a[0].isFunction())
            return exe2->callFunction(a[0], {result}, cs);
        return result; }, "then"));

        taskObj->obj->set("catch", NovaValue::makeNative([fiber, exe2](ValVec a, auto cs) -> Val
                                                         {
        try {
            Val resumeWith = nova_null();
            while (!fiber->done) {
                fiber->sentValue = resumeWith;
                fiber->resume();
                resumeWith = nova_null();
                if (fiber->done)
                    break;
                Val result = fiber->yieldedValue;
                if (result && result.isObject()) {
                    Val nestedAsync = result->obj->get("__async__");
                    if (nestedAsync && nestedAsync.asBool())
                        resumeWith = driveAsyncTask(exe2, result, cs);
                }
            }
        } catch (ThrowSignal& e) {
            if (!a.empty() && a[0] && a[0].isFunction())
                return exe2->callFunction(a[0], {e.value}, cs);
        }
        return nova_null(); }, "catch"));

        return taskObj;
    }
    // ── generator runner ──────────────────────────────────────────────────────────
    Val Executor::_makeGenerator(const Node *fnNode, std::shared_ptr<Scope> s, ValVec args)
    {
        auto fiber = std::make_shared<Fiber>();

        // capture everything the fiber body needs
        auto exe = this;
        auto nodeRef = fnNode;
        auto scope = s;

        fiber->body = [exe, nodeRef, scope](Fiber &f)
        {
            // Create a fresh child scope for this fiber's execution so that
            // the `yield` injection below doesn't write into the shared
            // closure scope. Without this, calling the same generator
            // function twice would have the second call overwrite `yield`
            // in the scope the first call's fiber is still using.
            auto fiberScope = scope->child(Scope::Kind::Function);
            fiberScope->funcName = nodeRef->name;

            // inject yield as a Nova-callable into the fiber's own scope
            fiberScope->setOwn("yield", NovaValue::makeNative([&f](ValVec a, auto) -> Val
                                                              {
            f.yieldedValue = a.empty() ? nova_null() : a[0];
            f.suspend();
            // when resumed, sentValue holds what next(val) passed in
            return f.sentValue ? f.sentValue : nova_null(); }, "yield"));

            ExecResult r = exe->_runBody(nodeRef->body, fiberScope);
            f.yieldedValue = r.value; // final return value (if any)
        };

        fiberInit(*fiber);

        // build the Nova generator object
        auto gen = nova_obj();

        gen->obj->set("next", NovaValue::makeNative([fiber](ValVec a, auto) -> Val
                                                    {
        auto result = nova_obj();
        if (fiber->done) {
            result->obj->set("done",  nova_bool(true));
            result->obj->set("value", nova_null());
            return result;
        }
        fiber->sentValue = a.empty() ? nova_null() : a[0];
        fiber->resume(); // run until next yield or done
        result->obj->set("done",  nova_bool(fiber->done));
        result->obj->set("value", fiber->yieldedValue ? fiber->yieldedValue : nova_null());
        return result; }, "next"));

        gen->obj->set("return", NovaValue::makeNative([fiber](ValVec a, auto) -> Val
                                                      {
        fiber->done = true;
        auto result = nova_obj();
        result->obj->set("done",  nova_bool(true));
        result->obj->set("value", a.empty() ? nova_null() : a[0]);
        return result; }, "return"));

        gen->obj->set("throw", NovaValue::makeNative([fiber](ValVec a, auto) -> Val
                                                     {
        fiber->thrownException = std::make_exception_ptr(
            ThrowSignal{a.empty() ? nova_str("Generator error") : a[0]}
        );
        fiber->done = true;
        throw ThrowSignal{a.empty() ? nova_str("Generator error") : a[0]};
        return nova_null(); }, "throw"));

        // [Symbol.iterator] — make it work in for..of
        gen->obj->set("__iter__", NovaValue::makeNative([gen](ValVec, auto) -> Val
                                                        { return gen; }, "__iter__"));

        gen->obj->set("done", nova_bool(false));
        return gen;
    }
    // ════════════════════════════════════════════════════════════════════════════
    //  PROPERTY ACCESS / SET
    // ════════════════════════════════════════════════════════════════════════════

    Val Executor::_getProp(Val obj, const std::string &key, std::shared_ptr<Scope> s, bool isOptional)
    {
        if (obj->overloads)
        {
            auto it = obj->overloads->find("prop:get");
            if (it != obj->overloads->end())
                return callFunction(it->second, {obj, nova_str(key)}, s ? s : globalScope);
        }
        if (!obj || obj.isNull())
        {
            if (isOptional)
                return nova_null();
            _error("Cannot access property '" + key + "' on null");
        }

        switch (obj->kind)
        {
        case VK::Class:
        {
            if (key == "name" || key == "__name__")
                return nova_str(obj->cls->name);
            // statics first
            auto it = obj->cls->statics.find(key);
            if (it != obj->cls->statics.end())
                return it->second;
            // fall back to methods (e.g. accessing A.someMethod directly)
            auto mit = obj->cls->methods.find(key);
            if (mit != obj->cls->methods.end())
                return mit->second;
            break;
        }
        case VK::Object:
        {
            Val v = obj->obj->get(key);
            if (v)
                return v;
            break;
        }
        case VK::Struct:
        {
            Val v = obj->strct->get(key);
            if (v)
                return v;
            break;
        }
        case VK::Enum:
        {
            if (key == "variant")
                return nova_str(obj->enm->variant);
            if (key == "typeName" || key == "enumType")
                return nova_str(obj->enm->typeName);
            if (key == "value")
                return obj->enm->value ? obj->enm->value : nova_null();
            break;
        }
        case VK::Array:
        {
            // Numeric index
            try
            {
                size_t pos;
                int i = std::stoi(key, &pos);
                if (pos == key.size())
                    return obj->arr->get(i);
            }
            catch (...)
            {
            }

            // Array methods — all lambdas capture `obj` by value (shared_ptr
            // copy) so they safely keep the array alive. The old `auto &arr =
            // *obj->arr` + `[&arr]` capture was a dangling reference: the
            // lambda outlives the _getProp stack frame where `arr` lived.
            auto exe = this;
            if (key == "length")
                return nova_num((double)obj->arr->length());
            if (key == "push")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             { if(!a.empty()) obj->arr->push(a[0]); return nova_num(obj->arr->length()); }, "push");
            if (key == "pop")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             { return obj->arr->pop(); }, "pop");
            if (key == "shift")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             { return obj->arr->shift(); }, "shift");
            if (key == "unshift")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             { if(!a.empty()) obj->arr->unshift(a[0]); return nova_num(obj->arr->length()); }, "unshift");
            if (key == "join")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                std::string sep = a.empty() ? "," : a[0].asString();
                std::string out;
                for (size_t i=0;i<obj->arr->inner.size();i++) { if(i) out+=sep; out+=obj->arr->inner[i].asString(); }
                return nova_str(out); }, "join");
            if (key == "reverse")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                std::reverse(obj->arr->inner.begin(), obj->arr->inner.end()); return obj; }, "reverse");
            if (key == "indexOf")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_num(-1);
                for (int i=0;i<(int)obj->arr->inner.size();i++) if (*obj->arr->inner[i] == *a[0]) return nova_num(i);
                return nova_num(-1); }, "indexOf");
            if (key == "includes")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_bool(false);
                for (auto& v : obj->arr->inner) if (*v == *a[0]) return nova_bool(true);
                return nova_bool(false); }, "includes");
            if (key == "slice")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                int start = a.size()>0 ? (int)a[0].asNumber() : 0;
                int end   = a.size()>1 ? (int)a[1].asNumber() : (int)obj->arr->inner.size();
                int sz = (int)obj->arr->inner.size();
                if (start<0) start=std::max(0,sz+start);
                if (end<0)   end=std::max(0,sz+end);
                start=std::min(start,sz); end=std::min(end,sz);
                ValVec out(obj->arr->inner.begin()+start, obj->arr->inner.begin()+std::max(start,end));
                return nova_arr(out); }, "slice");
            if (key == "map")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.empty() || !a[0].isFunction()) return nova_arr(obj->arr->inner);
                ValVec out; out.reserve(obj->arr->inner.size());
                for (int i=0;i<(int)obj->arr->inner.size();i++)
                    out.push_back(exe->callFunction(a[0], {obj->arr->inner[i], nova_num(i)}, cs));
                return nova_arr(out); }, "map");
            if (key == "filter")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.empty() || !a[0].isFunction()) return nova_arr();
                ValVec out;
                for (int i=0;i<(int)obj->arr->inner.size();i++)
                    if (exe->callFunction(a[0],{obj->arr->inner[i],nova_num(i)},cs).asBool()) out.push_back(obj->arr->inner[i]);
                return nova_arr(out); }, "filter");
            if (key == "reduce")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.size()<2 || !a[0].isFunction()) return nova_null();
                Val acc = a[1];
                for (int i=0;i<(int)obj->arr->inner.size();i++)
                    acc = exe->callFunction(a[0],{acc,obj->arr->inner[i],nova_num(i)},cs);
                return acc; }, "reduce");
            if (key == "forEach")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.empty() || !a[0].isFunction()) return nova_null();
                for (int i=0;i<(int)obj->arr->inner.size();i++)
                    exe->callFunction(a[0],{obj->arr->inner[i],nova_num(i)},cs);
                return nova_null(); }, "forEach");
            if (key == "find")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.empty() || !a[0].isFunction()) return nova_null();
                for (int i=0;i<(int)obj->arr->inner.size();i++)
                    if (exe->callFunction(a[0],{obj->arr->inner[i],nova_num(i)},cs).asBool()) return obj->arr->inner[i];
                return nova_null(); }, "find");
            if (key == "findIndex")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.empty() || !a[0].isFunction()) return nova_num(-1);
                for (int i=0;i<(int)obj->arr->inner.size();i++)
                    if (exe->callFunction(a[0],{obj->arr->inner[i],nova_num(i)},cs).asBool()) return nova_num(i);
                return nova_num(-1); }, "findIndex");
            if (key == "some")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.empty() || !a[0].isFunction()) return nova_bool(false);
                for (int i=0;i<(int)obj->arr->inner.size();i++)
                    if (exe->callFunction(a[0],{obj->arr->inner[i],nova_num(i)},cs).asBool()) return nova_bool(true);
                return nova_bool(false); }, "some");
            if (key == "every")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.empty() || !a[0].isFunction()) return nova_bool(true);
                for (int i=0;i<(int)obj->arr->inner.size();i++)
                    if (!exe->callFunction(a[0],{obj->arr->inner[i],nova_num(i)},cs).asBool()) return nova_bool(false);
                return nova_bool(true); }, "every");
            if (key == "flat")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                int depth = a.empty() ? 1 : (int)a[0].asNumber();
                ValVec out;
                std::function<void(const ValVec&, int)> flatten = [&](const ValVec& v, int d) {
                    for (auto& item : v) {
                        if (item.isArray() && d > 0) flatten(item->arr->inner, d-1);
                        else out.push_back(item);
                    }
                };
                flatten(obj->arr->inner, depth);
                return nova_arr(out); }, "flat");
            if (key == "flatMap")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                if (a.empty() || !a[0].isFunction()) return nova_arr();
                ValVec out;
                for (int i=0;i<(int)obj->arr->inner.size();i++) {
                    Val r = exe->callFunction(a[0],{obj->arr->inner[i],nova_num(i)},cs);
                    if (r.isArray()) for (auto& v : r->arr->inner) out.push_back(v);
                    else out.push_back(r);
                }
                return nova_arr(out); }, "flatMap");
            if (key == "sort")
                return NovaValue::makeNative([obj, exe](ValVec a, auto cs)
                                             {
                auto& inner = obj->arr->inner;
                if (!a.empty() && a[0].isFunction()) {
                    std::sort(inner.begin(), inner.end(), [&](Val x, Val y) {
                        Val r = exe->callFunction(a[0],{x,y},cs);
                        return r.asNumber() < 0;
                    });
                } else {
                    std::sort(inner.begin(), inner.end(), [](Val x, Val y) { return *x < *y; });
                }
                return obj; }, "sort");
            if (key == "concat")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                ValVec out = obj->arr->inner;
                for (auto& arg : a) {
                    if (arg.isArray()) for (auto& v : arg->arr->inner) out.push_back(v);
                    else out.push_back(arg);
                }
                return nova_arr(out); }, "concat");
            if (key == "fill")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return obj;
                Val fill = a[0];
                int start = a.size()>1?(int)a[1].asNumber():0;
                int end   = a.size()>2?(int)a[2].asNumber():(int)obj->arr->inner.size();
                for (int i=start;i<end && i<(int)obj->arr->inner.size();i++) obj->arr->inner[i]=fill;
                return obj; }, "fill");
            if (key == "at")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_null();
                int i = (int)a[0].asNumber();
                return obj->arr->get(i); }, "at");
            if (key == "splice")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_arr();
                int start = (int)a[0].asNumber();
                int sz = (int)obj->arr->inner.size();
                if (start<0) start=std::max(0,sz+start);
                int deleteCount = a.size()>1?(int)a[1].asNumber():sz-start;
                deleteCount=std::min(deleteCount,sz-start);
                ValVec removed(obj->arr->inner.begin()+start, obj->arr->inner.begin()+start+deleteCount);
                obj->arr->inner.erase(obj->arr->inner.begin()+start, obj->arr->inner.begin()+start+deleteCount);
                for (int i=2;i<(int)a.size();i++) obj->arr->inner.insert(obj->arr->inner.begin()+start+i-2, a[i]);
                return nova_arr(removed); }, "splice");
            if (key == "keys")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                ValVec out; for (int i=0;i<(int)obj->arr->inner.size();i++) out.push_back(nova_num(i));
                return nova_arr(out); }, "keys");
            if (key == "values")
                return obj;
            if (key == "entries")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                ValVec out;
                for (int i=0;i<(int)obj->arr->inner.size();i++) out.push_back(nova_arr({nova_num(i),obj->arr->inner[i]}));
                return nova_arr(out); }, "entries");
            if (key == "toString")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             { return nova_str(obj.asString()); }, "toString");
            break;
        }
        case VK::String:
        {
            const std::string &str = obj->sval;
            if (key == "length")
                return nova_num((double)utf8::length(str));
            if (key == "toString" || key == "valueOf")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             { return obj; }, key);
            if (key == "toUpperCase")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                std::string s=obj->sval; std::transform(s.begin(),s.end(),s.begin(),::toupper); return nova_str(s); }, "toUpperCase");
            if (key == "toLowerCase")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                std::string s=obj->sval; std::transform(s.begin(),s.end(),s.begin(),::tolower); return nova_str(s); }, "toLowerCase");
            if (key == "trim")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                auto& s=obj->sval; size_t l=s.find_first_not_of(" \t\r\n"), r=s.find_last_not_of(" \t\r\n");
                return l==std::string::npos?nova_str(""):nova_str(s.substr(l,r-l+1)); }, "trim");
            if (key == "split")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                std::string sep = a.empty() ? "" : a[0].asString();
                ValVec out;
                if (sep.empty()) { for (auto &cp : utf8::codepoints(obj->sval)) out.push_back(nova_str(cp)); return nova_arr(out); }
                size_t pos=0, found;
                while ((found=obj->sval.find(sep,pos))!=std::string::npos) {
                    out.push_back(nova_str(obj->sval.substr(pos,found-pos))); pos=found+sep.size();
                }
                out.push_back(nova_str(obj->sval.substr(pos)));
                return nova_arr(out); }, "split");
            if (key == "includes")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             { return nova_bool(!a.empty() && obj->sval.find(a[0].asString()) != std::string::npos); }, "includes");
            if (key == "startsWith")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_bool(false);
                auto p=a[0].asString(); return nova_bool(obj->sval.size()>=p.size()&&obj->sval.substr(0,p.size())==p); }, "startsWith");
            if (key == "endsWith")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_bool(false);
                auto sf=a[0].asString(); return nova_bool(obj->sval.size()>=sf.size()&&obj->sval.substr(obj->sval.size()-sf.size())==sf); }, "endsWith");
            if (key == "slice")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                int sz=(int)utf8::length(obj->sval);
                int start=a.size()>0?(int)a[0].asNumber():0;
                int end  =a.size()>1?(int)a[1].asNumber():sz;
                if (start<0) start=std::max(0,sz+start);
                if (end<0)   end=std::max(0,sz+end);
                start=std::min(start,sz); end=std::min(end,sz);
                return nova_str(utf8::substr(obj->sval,start,end)); }, "slice");
            if (key == "substring")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                int sz=(int)utf8::length(obj->sval);
                int start=a.size()>0?std::max(0,std::min((int)a[0].asNumber(),sz)):0;
                int end  =a.size()>1?std::max(0,std::min((int)a[1].asNumber(),sz)):sz;
                if (start>end) std::swap(start,end);
                return nova_str(utf8::substr(obj->sval,start,end)); }, "substring");
            if (key == "indexOf")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_num(-1);
                return nova_num((double)utf8::indexOf(obj->sval, a[0].asString())); }, "indexOf");
            if (key == "lastIndexOf")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_num(-1);
                return nova_num((double)utf8::lastIndexOf(obj->sval, a[0].asString())); }, "lastIndexOf");
            if (key == "replace")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.size()<2) return obj;
                std::string s=obj->sval, f=a[0].asString(), t=a[1].asString();
                auto pos=s.find(f);
                if (pos!=std::string::npos) s.replace(pos,f.size(),t);
                return nova_str(s); }, "replace");
            if (key == "replaceAll")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.size()<2) return obj;
                std::string s=obj->sval, f=a[0].asString(), t=a[1].asString();
                size_t pos=0;
                while ((pos=s.find(f,pos))!=std::string::npos) { s.replace(pos,f.size(),t); pos+=t.size(); }
                return nova_str(s); }, "replaceAll");
            if (key == "repeat")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                int n=a.empty()?0:(int)a[0].asNumber(); std::string out; for(int i=0;i<n;i++) out+=obj->sval; return nova_str(out); }, "repeat");
            if (key == "padStart")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return obj;
                int w=(int)a[0].asNumber();
                std::string p=a.size()>1?a[1].asString():" ";
                return nova_str(utf8::padStart(obj->sval, w, p)); }, "padStart");
            if (key == "padEnd")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return obj;
                int w=(int)a[0].asNumber();
                std::string p=a.size()>1?a[1].asString():" ";
                return nova_str(utf8::padEnd(obj->sval, w, p)); }, "padEnd");
            if (key == "charAt")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_str("");
                std::string cp = utf8::at(obj->sval, (int)a[0].asNumber());
                return nova_str(cp); }, "charAt");
            if (key == "charCodeAt")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_num(std::numeric_limits<double>::quiet_NaN());
                std::string cp = utf8::at(obj->sval, (int)a[0].asNumber());
                if (cp.empty()) return nova_num(std::numeric_limits<double>::quiet_NaN());
                // decode UTF-8 codepoint to Unicode scalar value
                unsigned char c0 = (unsigned char)cp[0];
                uint32_t code = 0;
                if      (c0 < 0x80)                    code = c0;
                else if ((c0 & 0xE0) == 0xC0 && cp.size()>=2) code = ((c0&0x1F)<<6)  | ((unsigned char)cp[1]&0x3F);
                else if ((c0 & 0xF0) == 0xE0 && cp.size()>=3) code = ((c0&0x0F)<<12) | (((unsigned char)cp[1]&0x3F)<<6) | ((unsigned char)cp[2]&0x3F);
                else if ((c0 & 0xF8) == 0xF0 && cp.size()>=4) code = ((c0&0x07)<<18) | (((unsigned char)cp[1]&0x3F)<<12) | (((unsigned char)cp[2]&0x3F)<<6) | ((unsigned char)cp[3]&0x3F);
                return nova_num((double)code); }, "charCodeAt");
            if (key == "at")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (a.empty()) return nova_null();
                std::string cp = utf8::at(obj->sval, (int)a[0].asNumber());
                return cp.empty() ? nova_null() : nova_str(cp); }, "at");
            if (key == "trimStart" || key == "trimLeft")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                auto& s=obj->sval; size_t l=s.find_first_not_of(" \t\r\n");
                return l==std::string::npos?nova_str(""):nova_str(s.substr(l)); }, key);
            if (key == "trimEnd" || key == "trimRight")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                auto& s=obj->sval; size_t r=s.find_last_not_of(" \t\r\n");
                return r==std::string::npos?nova_str(""):nova_str(s.substr(0,r+1)); }, key);
            if (key == "toArray" || key == "chars")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             {
                ValVec out;
                for (auto &cp : utf8::codepoints(obj->sval)) out.push_back(nova_str(cp));
                return nova_arr(out); }, key);
            break;
        }
        case VK::Number:
        {
            if (key == "toString")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                int base = a.empty()?10:(int)a[0].asNumber();
                if (base == 10) return nova_str(obj.asString());
                long long n = (long long)obj->nval;
                if (n == 0) return nova_str("0");
                std::string result; bool neg = n<0; if(neg) n=-n;
                const char* digits="0123456789abcdefghijklmnopqrstuvwxyz";
                while(n>0){result=digits[n%base]+result;n/=base;}
                if(neg) result="-"+result;
                return nova_str(result); }, "toString");
            if (key == "toFixed")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                int digits = a.empty()?0:(int)a[0].asNumber();
                std::ostringstream ss; ss << std::fixed; ss.precision(digits); ss << obj->nval;
                return nova_str(ss.str()); }, "toFixed");
            break;
        }
        case VK::Range:
        {
            auto &r = *obj->range;
            if (key == "start")
                return nova_num(r.start);
            if (key == "end")
                return nova_num(r.end);
            if (key == "step")
                return nova_num(r.step);
            if (key == "length")
                return nova_num((double)r.length());
            if (key == "toArray")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             { return nova_arr(obj->range->toArray()); }, "toArray");
            if (key == "includes")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             { return nova_bool(!a.empty() && obj->range->includes(a[0].asNumber())); }, "includes");
            if (key == "forEach")
                return NovaValue::makeNative([obj, this](ValVec a, auto cs)
                                             {
                if (a.empty()||!a[0].isFunction()) return nova_null();
                auto elems = obj->range->toArray(); int i=0;
                for (auto& v : elems) callFunction(a[0],{v,nova_num(i++)},cs);
                return nova_null(); }, "forEach");
            break;
        }
        case VK::Pointer:
        {
            if (key == "read")
                return NovaValue::makeNative([obj](ValVec, auto)
                                             { return obj->ptr->read(); }, "read");
            if (key == "write")
                return NovaValue::makeNative([obj](ValVec a, auto)
                                             {
                if (!a.empty()) obj->ptr->write(a[0]); return nova_null(); }, "write");
            if (key == "address")
                return nova_str(obj->ptr->address);
            if (key == "value")
                return obj->ptr->inner;

            // ── raw_ptr API (unsafe) ──────────────────────────────────────
            if (key == "isRaw")
                return nova_bool(obj->ptr->isRaw);
            if (key == "addr")
                return nova_num((double)obj->ptr->addressNum());
            if (key == "size")
                return nova_num((double)obj->ptr->rawSize);
            if (key == "type")
                return nova_str(nativeTypeName(obj->ptr->rawType));
            if (key == "readAs")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
                NativeType t = a.empty() ? obj->ptr->rawType : parseNativeType(a[0].asString());
                return obj->ptr->readRaw(t); }, "readAs");
            if (key == "writeAs")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
                if (a.empty()) return nova_null();
                NativeType t = a.size() > 1 ? parseNativeType(a[1].asString()) : obj->ptr->rawType;
                obj->ptr->writeRaw(a[0], t);
                return nova_null(); }, "writeAs");
            if (key == "offset")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
                int n = a.empty() ? 0 : (int)a[0].asNumber();
                size_t elemSize = nativeTypeSize(obj->ptr->rawType);
                if (elemSize == 0) elemSize = 1;

                auto np = std::make_shared<NovaPointer>();
                np->isRaw     = true;
                np->rawType   = obj->ptr->rawType;
                np->rawBuffer = obj->ptr->rawBuffer; // keep backing buffer alive
                np->rawAddr   = reinterpret_cast<uint8_t*>(obj->ptr->rawAddr)
                              + (ptrdiff_t)n * (ptrdiff_t)elemSize;
                np->rawSize   = 0; // bounds unknown past an offset — caller's responsibility

                std::ostringstream ss; ss << "0x" << std::hex << np->addressNum();
                np->address = ss.str();

                auto out = std::make_shared<NovaValue>();
                out->kind = VK::Pointer;
                out->ptr  = np;
                return out; }, "offset");
            break;
        }
        case VK::BigInt:
        {
            if (!obj->bigint)
                break;
            auto &bi = *obj->bigint;
            if (key == "toString")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            int base = a.empty() ? 10 : (int)a[0].asNumber();
            if (base == 10) return nova_str(obj->bigint->toString());
            // non-base-10: convert via repeated division
            if (obj->bigint->isZero()) return nova_str("0");
            NovaBigInt n = obj->bigint->abs();
            std::string digits;
            NovaBigInt bval((int64_t)base);
            while (!n.isZero()) {
                NovaBigInt rem = n % bval;
                int d = (int)rem.toDouble();
                digits += (d < 10 ? '0'+d : 'a'+d-10);
                n = n / bval;
            }
            if (obj->bigint->negative) digits += '-';
            std::reverse(digits.begin(), digits.end());
            return nova_str(digits); }, "toString");
            if (key == "toNumber")
                return nova_num(bi.toDouble());
            if (key == "abs")
                return NovaValue::makeBigInt(bi.abs());
            if (key == "negate")
                return NovaValue::makeBigInt(bi.negate());
            if (key == "isZero")
                return nova_bool(bi.isZero());
            if (key == "sign")
                return nova_num(bi.isZero() ? 0 : bi.negative ? -1
                                                              : 1);
            if (key == "pow")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            uint64_t exp = a.empty() ? 0 : (uint64_t)a[0].asNumber();
            return NovaValue::makeBigInt(obj->bigint->pow(exp)); }, "pow");
            if (key == "add")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigInt r = a[0].isBigInt() ? *a[0]->bigint : NovaBigInt((int64_t)a[0].asNumber());
            return NovaValue::makeBigInt(*obj->bigint + r); }, "add");
            if (key == "sub")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigInt r = a[0].isBigInt() ? *a[0]->bigint : NovaBigInt((int64_t)a[0].asNumber());
            return NovaValue::makeBigInt(*obj->bigint - r); }, "sub");
            if (key == "mul")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigInt r = a[0].isBigInt() ? *a[0]->bigint : NovaBigInt((int64_t)a[0].asNumber());
            return NovaValue::makeBigInt(*obj->bigint * r); }, "mul");
            if (key == "div")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigInt r = a[0].isBigInt() ? *a[0]->bigint : NovaBigInt((int64_t)a[0].asNumber());
            return NovaValue::makeBigInt(*obj->bigint / r); }, "div");
            if (key == "mod")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigInt r = a[0].isBigInt() ? *a[0]->bigint : NovaBigInt((int64_t)a[0].asNumber());
            return NovaValue::makeBigInt(*obj->bigint % r); }, "mod");
            if (key == "cmp")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return nova_num(0);
            NovaBigInt r = a[0].isBigInt() ? *a[0]->bigint : NovaBigInt((int64_t)a[0].asNumber());
            int c = NovaBigInt::cmpAbs(*obj->bigint, r);
            if (obj->bigint->negative != r.negative)
                return nova_num(obj->bigint->negative ? -1 : 1);
            return nova_num(obj->bigint->negative ? -c : c); }, "cmp");
            if (key == "toBigFloat")
                return NovaValue::makeBigFloat(NovaBigFloat(*obj->bigint));
            break;
        }
        case VK::BigFloat:
        {
            if (!obj->bigfloat)
                break;
            if (key == "toString")
                return NovaValue::makeNative([obj](ValVec, auto) -> Val
                                             { return nova_str(obj->bigfloat->toString()); }, "toString");
            if (key == "toSciString")
                return NovaValue::makeNative([obj](ValVec, auto) -> Val
                                             { return nova_str(obj->bigfloat->toSciString()); }, "toSciString");
            if (key == "toNumber")
                return nova_num(obj->bigfloat->toDouble());
            if (key == "isZero")
                return nova_bool(obj->bigfloat->mantissa.isZero());
            if (key == "precision")
                return nova_num(obj->bigfloat->precision);
            if (key == "withPrecision")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            NovaBigFloat copy = *obj->bigfloat;
            if (!a.empty()) copy.precision = (int)a[0].asNumber();
            copy.normalize();
            return NovaValue::makeBigFloat(std::move(copy)); }, "withPrecision");
            if (key == "add")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigFloat r = a[0].isBigFloat() ? *a[0]->bigfloat : NovaBigFloat(a[0].asNumber());
            return NovaValue::makeBigFloat(*obj->bigfloat + r); }, "add");
            if (key == "sub")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigFloat r = a[0].isBigFloat() ? *a[0]->bigfloat : NovaBigFloat(a[0].asNumber());
            return NovaValue::makeBigFloat(*obj->bigfloat - r); }, "sub");
            if (key == "mul")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigFloat r = a[0].isBigFloat() ? *a[0]->bigfloat : NovaBigFloat(a[0].asNumber());
            return NovaValue::makeBigFloat(*obj->bigfloat * r); }, "mul");
            if (key == "div")
                return NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                             {
            if (a.empty()) return obj;
            NovaBigFloat r = a[0].isBigFloat() ? *a[0]->bigfloat : NovaBigFloat(a[0].asNumber());
            return NovaValue::makeBigFloat(*obj->bigfloat / r); }, "div");
            if (key == "toBigInt")
                return NovaValue::makeBigInt(NovaBigInt(obj->bigfloat->toString().substr(
                    obj->bigfloat->mantissa.negative ? 1 : 0))); // rough truncation
            break;
        }
        default:
            break;
        }

        return nova_null();
    }

    void Executor::_setProp(Val obj, const std::string &key, Val value)
    {
        if (!obj)
            return;
        if (obj->overloads)
        {
            auto it = obj->overloads->find("prop:set");
            if (it != obj->overloads->end())
            {
                callFunction(it->second, {obj, nova_str(key), value}, globalScope);
                return;
            }
        }
        if (obj.isObject())
            obj->obj->set(key, value);
        else if (obj.isStruct())
            obj->strct->set(key, value);
        else if (obj.isArray() && !key.empty() && std::isdigit(key[0]))
            obj->arr->set(std::stoi(key), value);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  DESTRUCTURING
    // ════════════════════════════════════════════════════════════════════════════

    void Executor::_destructureObj(const ObjPattern &pat, Val val, std::shared_ptr<Scope> s, bool isConst)
    {
        for (auto &prop : pat.props)
        {
            Val v;
            if (val && val.isObject())
                v = val->obj->get(prop.key);
            else if (val && val.isStruct())
                v = val->strct->get(prop.key);
            if (!v && prop.defaultValue)
                v = evaluate(prop.defaultValue.get(), s);
            s->setOwn(prop.alias, v ? v : nova_null(), isConst);
        }
    }

    void Executor::_destructureArr(const ArrPattern &pat, Val val, std::shared_ptr<Scope> s, bool isConst)
    {
        for (int i = 0; i < (int)pat.elements.size(); i++)
        {
            const auto &elem = pat.elements[i];
            Val v;
            if (elem.rest)
            {
                // collect remaining
                ValVec rest;
                if (val && val.isArray())
                    for (int j = i; j < (int)val->arr->inner.size(); j++)
                        rest.push_back(val->arr->inner[j]);
                v = nova_arr(std::move(rest));
            }
            else
            {
                if (val && val.isArray())
                    v = val->arr->get(i);
                if (!v && elem.defaultValue)
                    v = evaluate(elem.defaultValue.get(), s);
            }
            s->setOwn(elem.name, v ? v : nova_null(), isConst);
            if (elem.rest)
                break;
        }
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  MATCH PATTERN
    // ════════════════════════════════════════════════════════════════════════════

    bool Executor::_matchPattern(Val subject, const Node *pattern, std::shared_ptr<Scope> s)
    {
        if (!pattern)
            return false;
        Val pval = evaluate(pattern, s);
        if (!pval)
            return false;

        // Range check
        if (pval.isRange())
            return pval->range->includes(subject.asNumber());
        // Array pattern — match any element
        if (pval.isArray())
        {
            for (auto &v : pval->arr->inner)
                if (*v == *subject)
                    return true;
            return false;
        }
        // Function predicate
        if (pval.isFunction())
        {
            return _isTruthy(callFunction(pval, {subject}, s));
        }
        // Type string
        if (pval.isString())
        {
            if (pval->sval == _typeOf(subject))
                return true;
            if (pval->sval == "null" && subject.isNull())
                return true;
        }
        // Direct equality
        if (subject->overloads)
        {
            auto it = subject->overloads->find("binary:==");
            if (it != subject->overloads->end())
                return _isTruthy(callFunction(it->second, {subject, pval}, s));
        }
        return *pval == *subject;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  STRINGIFY
    // ════════════════════════════════════════════════════════════════════════════

    std::string Executor::stringify(Val v, int indent)
    {
        return _stringify(v, indent, 0);
    }

    std::string Executor::_stringify(Val v, int indent, int depth)
    {
        if (!v)
            return "null";
        if (v->overloads)
        {
            auto it = v->overloads->find("cast:string");
            if (it != v->overloads->end())
            {
                Val result = callFunction(it->second, {v}, globalScope);
                return result ? result.asString() : "null";
            }
        }
        if (v.isClass())
            return "[Class " + (v->cls ? v->cls->name : "?") + "]";
        if (v.isNull())
            return "null";
        if (v.isBool())
            return v->bval ? "true" : "false";
        if (v.isNumber())
            return v.asString();
        if (v.isString())
            return indent ? "\"" + v->sval + "\"" : v->sval;
        if (v.isFunction())
            return "[Function " + (v->fn ? v->fn->name : "anon") + "]";

        if (v.isArray())
        {
            if (v->arr->inner.empty())
                return "[]";
            std::string pad(indent * (depth + 1), ' ');
            std::string closePad(indent * depth, ' ');
            std::string nl = indent ? "\n" : "";
            std::string sep = indent ? (",\n" + pad) : ", ";
            std::string out = "[" + nl + (indent ? pad : "");
            for (size_t i = 0; i < v->arr->inner.size(); i++)
            {
                if (i)
                    out += sep;
                out += _stringify(v->arr->inner[i], indent, depth + 1);
            }
            return out + nl + (indent ? closePad : "") + "]";
        }
        if (v.isObject())
        {
            auto &inner = v->obj->inner;
            if (inner.empty())
                return "{}";
            std::string pad(indent * (depth + 1), ' ');
            std::string closePad(indent * depth, ' ');
            std::string nl = indent ? "\n" : "";
            std::string out = "{" + nl;
            bool first = true;
            for (auto &[k, val] : inner)
            {
                if (!first)
                    out += (indent ? ",\n" : ", ");
                if (indent)
                    out += pad;
                out += k + ": " + _stringify(val, indent, depth + 1);
                first = false;
            }
            return out + nl + (indent ? closePad : "") + "}";
        }
        if (v.isStruct())
            return v->strct->toString();
        if (v.isEnum())
            return v->enm->toString();
        if (v.isRange())
            return v->range->toString();
        if (v.isPointer())
            return v->ptr->toString();
        if (v.isBigInt())
            return v->bigint ? v->bigint->toString() : "0n";
        if (v.isBigFloat())
            return v->bigfloat ? v->bigfloat->toString() : "0bf";
        return v.asString();
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  nat / unat / raw_ptr
    // ════════════════════════════════════════════════════════════════════════════

    std::string Executor::_inferNativeType(Val v) const
    {
        if (!v)
            return "f64";
        if (v.isBool())
            return "bool";
        if (v.isString())
            return "cstr";
        if (v.isNativeVal())
            return nativeTypeName(v->natv->type);
        if (v.isNumber())
        {
            double n = v->nval;
            if (n == std::floor(n) && std::abs(n) < 2147483648.0)
                return "i32";
            return "f64";
        }
        return "f64";
    }

    Val Executor::_toNative(Val v, const std::string &typeStr)
    {
        NativeType t = parseNativeType(typeStr.empty() ? _inferNativeType(v) : typeStr);
        return NovaValue::makeNativeVal(t, nativeEncode(v, t));
    }

    Val Executor::_fromNative(Val v)
    {
        if (!v)
            return nova_null();
        if (!v.isNativeVal())
            return v; // pass through — already a normal Nova value
        return nativeDecode((const uint8_t *)v->natv->data(), v->natv->size(), v->natv->type);
    }

    Val Executor::_toRawPtr(Val v, const std::string &typeStr)
    {
        auto ptr = std::make_shared<NovaPointer>();
        ptr->isRaw = true;

        if (v && v.isNativeVal())
        {
            // alias the existing buffer — writes through this raw_ptr are
            // visible to anyone still holding the original nat() value
            ptr->rawBuffer = v->natv->buffer;
            ptr->rawType = typeStr.empty() ? v->natv->type : parseNativeType(typeStr);
        }
        else
        {
            NativeType t = parseNativeType(typeStr.empty() ? _inferNativeType(v) : typeStr);
            ptr->rawBuffer = std::make_shared<std::vector<uint8_t>>(nativeEncode(v, t));
            ptr->rawType = t;
        }

        ptr->rawAddr = ptr->rawBuffer->data();
        ptr->rawSize = ptr->rawBuffer->size();

        std::ostringstream ss;
        ss << "0x" << std::hex << ptr->addressNum();
        ptr->address = ss.str();

        auto out = std::make_shared<NovaValue>();
        out->kind = VK::Pointer;
        out->ptr = ptr;
        return out;
    }

    void Executor::_registerNative(Val obj)
    {
        auto exe = this;

        obj->obj->set("nat", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                   {
        if (a.empty()) return nova_null();
        std::string t = a.size() > 1 ? a[1].asString() : "";
        return exe->_toNative(a[0], t); }, "nat"));

        obj->obj->set("unat", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                    {
        if (a.empty()) return nova_null();
        return exe->_fromNative(a[0]); }, "unat"));

        obj->obj->set("raw_ptr", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                       {
        if (a.empty()) return nova_null();
        std::string t = a.size() > 1 ? a[1].asString() : "";
        return exe->_toRawPtr(a[0], t); }, "raw_ptr"));

        auto nt = nova_obj();
        for (auto name : {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64", "bool", "cstr", "ptr"})
            nt->obj->set(name, nova_str(name));
        obj->obj->set("native", nt);
    }

    // ── stdio control (C/C++ stdlib buffering & flush control) ───────────────────

    void Executor::_registerStdioControl(Val obj)
    {
        auto resolveFile = [](const std::string &name) -> FILE *
        {
            if (name == "stderr")
                return stderr;
            if (name == "stdin")
                return stdin;
            return stdout; // default, also covers "stdout"
        };

        // Std.SetvBuf(stream, mode, size)
        //   stream : "stdout" | "stderr" | "stdin"   (default "stdout")
        //   mode   : "full" | "line" | "none"        -> _IOFBF / _IOLBF / _IONBF
        //   size   : buffer size in bytes            (default BUFSIZ)
        obj->obj->set("SetvBuf", NovaValue::makeNative([resolveFile](ValVec a, auto) -> Val
                                                       {
            FILE *f = resolveFile(a.empty() ? "stdout" : a[0].asString());

            int mode = _IOFBF;
            if (a.size() > 1)
            {
                std::string m = a[1].asString();
                if (m == "line") mode = _IOLBF;
                else if (m == "none") mode = _IONBF;
            }
            size_t size = a.size() > 2 ? (size_t)a[2].asNumber() : BUFSIZ;

            return nova_bool(std::setvbuf(f, nullptr, mode, size) == 0); }, "SetvBuf"));

        // Std.SetBuf(stream, enabled) — shorthand: false = unbuffered, true = full buffering
        obj->obj->set("SetBuf", NovaValue::makeNative([resolveFile](ValVec a, auto) -> Val
                                                      {
            FILE *f = resolveFile(a.empty() ? "stdout" : a[0].asString());
            bool enabled = a.size() > 1 ? a[1].asBool() : true;
            return nova_bool(std::setvbuf(f, nullptr, enabled ? _IOFBF : _IONBF, BUFSIZ) == 0); }, "SetBuf"));

        // Std.Fflush(stream?) — flush a specific stream, or every open stream if omitted
        obj->obj->set("Fflush", NovaValue::makeNative([resolveFile](ValVec a, auto) -> Val
                                                      {
            FILE *f = a.empty() ? nullptr : resolveFile(a[0].asString());
            return nova_bool(std::fflush(f) == 0); }, "Fflush"));

        // Std.SyncWithStdio(bool) — std::ios_base::sync_with_stdio; returns the previous setting
        obj->obj->set("SyncWithStdio", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
            bool sync = a.empty() ? true : a[0].asBool();
            return nova_bool(std::ios_base::sync_with_stdio(sync)); }, "SyncWithStdio"));
    }

    // ── raw memory control (C <cstring> / malloc-style allocation) ───────────────

    void Executor::_registerMemory(Val obj)
    {
        auto exe = this;

        auto rawPtrOf = [exe](Val v) -> uint8_t *
        {
            if (!v || !v.isPointer() || !v->ptr || !v->ptr->isRaw)
                exe->_error("Expected a raw_ptr value");
            return reinterpret_cast<uint8_t *>(v->ptr->rawAddr);
        };

        // Std.Alloc(size) -> raw_ptr to a fresh, zero-filled byte buffer
        obj->obj->set("Alloc", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     {
            size_t size = a.empty() ? 0 : (size_t)a[0].asNumber();
            auto ptr = std::make_shared<NovaPointer>();
            ptr->isRaw     = true;
            ptr->rawType   = parseNativeType("u8");
            ptr->rawBuffer = std::make_shared<std::vector<uint8_t>>(size, 0);
            ptr->rawAddr   = ptr->rawBuffer->data();
            ptr->rawSize   = size;

            std::ostringstream ss; ss << "0x" << std::hex << ptr->addressNum();
            ptr->address = ss.str();

            auto out = std::make_shared<NovaValue>();
            out->kind = VK::Pointer;
            out->ptr  = ptr;
            return out; }, "Alloc"));

        // Std.Sizeof(typeName) -> byte size of a native type ("i32", "f64", "ptr", ...)
        obj->obj->set("Sizeof", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            std::string t = a.empty() ? "u8" : a[0].asString();
            return nova_num((double)nativeTypeSize(parseNativeType(t))); }, "Sizeof"));

        // Std.Memcpy(dest, src, n) — regions must not overlap
        obj->obj->set("Memcpy", NovaValue::makeNative([rawPtrOf](ValVec a, auto) -> Val
                                                      {
            if (a.size() < 3) return nova_null();
            std::memcpy(rawPtrOf(a[0]), rawPtrOf(a[1]), (size_t)a[2].asNumber());
            return a[0]; }, "Memcpy"));

        // Std.Memmove(dest, src, n) — safe for overlapping regions
        obj->obj->set("Memmove", NovaValue::makeNative([rawPtrOf](ValVec a, auto) -> Val
                                                       {
            if (a.size() < 3) return nova_null();
            std::memmove(rawPtrOf(a[0]), rawPtrOf(a[1]), (size_t)a[2].asNumber());
            return a[0]; }, "Memmove"));

        // Std.Memset(ptr, byteValue, n)
        obj->obj->set("Memset", NovaValue::makeNative([rawPtrOf](ValVec a, auto) -> Val
                                                      {
            if (a.size() < 3) return nova_null();
            std::memset(rawPtrOf(a[0]), (int)a[1].asNumber(), (size_t)a[2].asNumber());
            return a[0]; }, "Memset"));

        // Std.Memcmp(a, b, n) -> -1 / 0 / 1
        obj->obj->set("Memcmp", NovaValue::makeNative([rawPtrOf](ValVec a, auto) -> Val
                                                      {
            if (a.size() < 3) return nova_num(0);
            int r = std::memcmp(rawPtrOf(a[0]), rawPtrOf(a[1]), (size_t)a[2].asNumber());
            return nova_num(r < 0 ? -1 : r > 0 ? 1 : 0); }, "Memcmp"));
    }

    // ── low-level binary file control (C <cstdio> FILE* handles) ─────────────────

    void Executor::_registerCFile(Val obj)
    {
        obj->obj->set("Fopen", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     {
            if (a.empty()) return nova_null();
            std::string path = a[0].asString();
            std::string mode = a.size() > 1 ? a[1].asString() : "rb";

            FILE *raw = std::fopen(path.c_str(), mode.c_str());
            if (!raw) return nova_null();
            auto fp = std::shared_ptr<FILE>(raw, [](FILE *f) { if (f) std::fclose(f); });
            auto h  = nova_obj();

            h->obj->set("read", NovaValue::makeNative([fp](ValVec a, auto) -> Val
                                                       {
                size_t n = a.empty() ? 4096 : (size_t)a[0].asNumber();
                std::vector<char> buf(n);
                size_t got = std::fread(buf.data(), 1, n, fp.get());
                return nova_str(std::string(buf.data(), got)); }, "read"));

            h->obj->set("write", NovaValue::makeNative([fp](ValVec a, auto) -> Val
                                                        {
                if (a.empty()) return nova_num(0);
                std::string s = a[0].asString();
                return nova_num((double)std::fwrite(s.data(), 1, s.size(), fp.get())); }, "write"));

            h->obj->set("seek", NovaValue::makeNative([fp](ValVec a, auto) -> Val
                                                       {
                long off = a.empty() ? 0 : (long)a[0].asNumber();
                std::string whence = a.size() > 1 ? a[1].asString() : "set";
                int w = whence == "cur" ? SEEK_CUR : whence == "end" ? SEEK_END : SEEK_SET;
                return nova_bool(std::fseek(fp.get(), off, w) == 0); }, "seek"));

            h->obj->set("tell", NovaValue::makeNative([fp](ValVec, auto) -> Val
                                                       { return nova_num((double)std::ftell(fp.get())); }, "tell"));

            h->obj->set("rewind", NovaValue::makeNative([fp](ValVec, auto) -> Val
                                                         { std::rewind(fp.get()); return nova_null(); }, "rewind"));

            h->obj->set("eof", NovaValue::makeNative([fp](ValVec, auto) -> Val
                                                      { return nova_bool(std::feof(fp.get()) != 0); }, "eof"));

            h->obj->set("error", NovaValue::makeNative([fp](ValVec, auto) -> Val
                                                        { return nova_bool(std::ferror(fp.get()) != 0); }, "error"));

            h->obj->set("flush", NovaValue::makeNative([fp](ValVec, auto) -> Val
                                                        { return nova_bool(std::fflush(fp.get()) == 0); }, "flush"));

            h->obj->set("close", NovaValue::makeNative([fp](ValVec, auto) -> Val
                                                        { return nova_bool(std::fclose(fp.get()) == 0); }, "close"));

            h->obj->set("setvbuf", NovaValue::makeNative([fp](ValVec a, auto) -> Val
                                                          {
                int mode = _IOFBF;
                if (!a.empty())
                {
                    std::string m = a[0].asString();
                    if (m == "line") mode = _IOLBF;
                    else if (m == "none") mode = _IONBF;
                }
                size_t size = a.size() > 1 ? (size_t)a[1].asNumber() : BUFSIZ;
                return nova_bool(std::setvbuf(fp.get(), nullptr, mode, size) == 0); }, "setvbuf"));

            return h; }, "Fopen"));

        obj->obj->set("Remove", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      { return nova_bool(!a.empty() && std::remove(a[0].asString().c_str()) == 0); }, "Remove"));

        obj->obj->set("Rename", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.size() < 2) return nova_bool(false);
            return nova_bool(std::rename(a[0].asString().c_str(), a[1].asString().c_str()) == 0); }, "Rename"));

        // Std.Tmpfile() — anonymous temp file handle, deleted automatically on close
        obj->obj->set("Tmpfile", NovaValue::makeNative([](ValVec, auto) -> Val
                                                       {
            FILE *raw = std::tmpfile();
            if (!raw) return nova_null();
            auto fp = std::shared_ptr<FILE>(raw, [](FILE *f) { if (f) std::fclose(f); });
            auto h  = nova_obj();
            h->obj->set("write", NovaValue::makeNative([fp](ValVec a, auto) -> Val
                                                        {
                if (a.empty()) return nova_num(0);
                std::string s = a[0].asString();
                return nova_num((double)std::fwrite(s.data(), 1, s.size(), fp.get())); }, "write"));
            h->obj->set("read", NovaValue::makeNative([fp](ValVec a, auto) -> Val
                                                       {
                size_t n = a.empty() ? 4096 : (size_t)a[0].asNumber();
                std::vector<char> buf(n);
                size_t got = std::fread(buf.data(), 1, n, fp.get());
                return nova_str(std::string(buf.data(), got)); }, "read"));
            h->obj->set("rewind", NovaValue::makeNative([fp](ValVec, auto) -> Val
                                                         { std::rewind(fp.get()); return nova_null(); }, "rewind"));
            h->obj->set("close", NovaValue::makeNative([fp](ValVec, auto) -> Val
                                                        { return nova_bool(std::fclose(fp.get()) == 0); }, "close"));
            return h; }, "Tmpfile"));
    }

    // ── process / OS control (system, exit hooks, env vars) ──────────────────────

    namespace
    {
        std::vector<Val> g_atexitHandlers;
        Executor *g_atexitExecutor = nullptr;
    }

    static void __novaAtexitTrampoline()
    {
        if (!g_atexitExecutor)
            return;
        for (auto &fn : g_atexitHandlers)
        {
            try
            {
                g_atexitExecutor->callFunction(fn, {}, g_atexitExecutor->globalScope);
            }
            catch (...)
            {
            }
        }
    }

    void Executor::_registerSystemControl(Val obj)
    {
        auto exe = this;

        // Std.System(cmd) -> shell exit code
        obj->obj->set("System", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      { return a.empty() ? nova_num(-1) : nova_num((double)std::system(a[0].asString().c_str())); }, "System"));

        // Std.Abort() — terminate immediately, no cleanup, no atexit handlers
        obj->obj->set("Abort", NovaValue::makeNative([](ValVec, auto) -> Val
                                                     { std::abort(); return nova_null(); }, "Abort"));

        // Std.AtExit(fn) — register a Nova function to run at normal process exit
        obj->obj->set("AtExit", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                      {
            if (a.empty() || !a[0].isFunction()) return nova_bool(false);
            if (g_atexitHandlers.empty())
            {
                g_atexitExecutor = exe;
                std::atexit(__novaAtexitTrampoline);
            }
            g_atexitHandlers.push_back(a[0]);
            return nova_bool(true); }, "AtExit"));

        // Std.GetEnv(name) -> string | null
        obj->obj->set("GetEnv", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.empty()) return nova_null();
            const char *v = std::getenv(a[0].asString().c_str());
            return v ? nova_str(v) : nova_null(); }, "GetEnv"));

        // Std.SetEnv(name, value, overwrite=true) -> bool   [POSIX: setenv, Windows: _putenv_s]
        obj->obj->set("SetEnv", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
                                                          if (a.size() < 2)
                                                              return nova_bool(false);
                                                          std::string name = a[0].asString(), value = a[1].asString();
                                                          bool overwrite = a.size() > 2 ? a[2].asBool() : true;
#if defined(_WIN32)
                                                          if (!overwrite && std::getenv(name.c_str()))
                                                              return nova_bool(true);
                                                          return nova_bool(_putenv_s(name.c_str(), value.c_str()) == 0);
#else
                                                          return nova_bool(setenv(name.c_str(), value.c_str(), overwrite ? 1 : 0) == 0);
#endif
                                                      },
                                                      "SetEnv"));

        // Std.UnsetEnv(name) -> bool
        obj->obj->set("UnsetEnv", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
                                                            if (a.empty())
                                                                return nova_bool(false);
#if defined(_WIN32)
                                                            return nova_bool(_putenv_s(a[0].asString().c_str(), "") == 0);
#else
                                                            return nova_bool(unsetenv(a[0].asString().c_str()) == 0);
#endif
                                                        },
                                                        "UnsetEnv"));

        // Std.HardwareConcurrency() -> logical core count
        obj->obj->set("HardwareConcurrency", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   { return nova_num((double)std::thread::hardware_concurrency()); }, "HardwareConcurrency"));
    }

    // ── errno control ──────────────────────────────────────────────────────────

    void Executor::_registerErrnoControl(Val obj)
    {
        obj->obj->set("Errno", NovaValue::makeNative([](ValVec, auto) -> Val
                                                     { return nova_num((double)errno); }, "Errno"));

        obj->obj->set("ClearErrno", NovaValue::makeNative([](ValVec, auto) -> Val
                                                          { errno = 0; return nova_null(); }, "ClearErrno"));

        obj->obj->set("Strerror", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            int code = a.empty() ? errno : (int)a[0].asNumber();
            return nova_str(std::strerror(code)); }, "Strerror"));

        obj->obj->set("Perror", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            std::perror(a.empty() ? "" : a[0].asString().c_str());
            return nova_null(); }, "Perror"));
    }

    // ── signal control (<csignal>) ────────────────────────────────────────────
    // NOTE: real signal handlers can't safely allocate, lock, or run arbitrary
    // code. This trampoline calls back into the interpreter anyway for scripting
    // convenience — keep handlers small (set a flag, etc.) rather than doing
    // real work inside them.

    namespace
    {
        std::unordered_map<int, Val> g_signalHandlers;
        Executor *g_signalExecutor = nullptr;
    }

    static void __novaSignalTrampoline(int sig)
    {
        auto it = g_signalHandlers.find(sig);
        if (it == g_signalHandlers.end() || !g_signalExecutor)
            return;
        try
        {
            g_signalExecutor->callFunction(it->second, {nova_num(sig)}, g_signalExecutor->globalScope);
        }
        catch (...)
        {
        }
    }

    static int __novaResolveSignal(const std::string &name)
    {
        if (name == "SIGINT")
            return SIGINT;
        if (name == "SIGTERM")
            return SIGTERM;
        if (name == "SIGABRT")
            return SIGABRT;
        if (name == "SIGFPE")
            return SIGFPE;
        if (name == "SIGILL")
            return SIGILL;
        if (name == "SIGSEGV")
            return SIGSEGV;
#ifdef SIGHUP
        if (name == "SIGHUP")
            return SIGHUP;
#endif
        try
        {
            return std::stoi(name);
        }
        catch (...)
        {
            return 0;
        }
    }

    void Executor::_registerSignalControl(Val obj)
    {
        auto exe = this;

        // Std.Signal(name|code, handlerFn) — e.g. Std.Signal("SIGINT", fn(sig) {...})
        obj->obj->set("Signal", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                      {
            if (a.size() < 2 || !a[1].isFunction()) return nova_bool(false);
            int sig = a[0].isString() ? __novaResolveSignal(a[0]->sval) : (int)a[0].asNumber();
            if (!sig) return nova_bool(false);
            g_signalExecutor = exe;
            g_signalHandlers[sig] = a[1];
            return nova_bool(std::signal(sig, __novaSignalTrampoline) != SIG_ERR); }, "Signal"));

        // Std.Raise(name|code) — send a signal to this process
        obj->obj->set("Raise", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     {
            if (a.empty()) return nova_bool(false);
            int sig = a[0].isString() ? __novaResolveSignal(a[0]->sval) : (int)a[0].asNumber();
            return nova_bool(std::raise(sig) == 0); }, "Raise"));
    }

    // ── filesystem control (std::filesystem, exposed as Std.FS) ──────────────────

    void Executor::_registerFS(Val obj)
    {
        auto fsObj = nova_obj();

        fsObj->obj->set("Exists", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        { return nova_bool(!a.empty() && fs::exists(a[0].asString())); }, "Exists"));

        fsObj->obj->set("IsDirectory", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             { return nova_bool(!a.empty() && fs::is_directory(a[0].asString())); }, "IsDirectory"));

        fsObj->obj->set("IsFile", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        { return nova_bool(!a.empty() && fs::is_regular_file(a[0].asString())); }, "IsFile"));

        fsObj->obj->set("CreateDirectory", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
            if (a.empty()) return nova_bool(false);
            try { return nova_bool(fs::create_directory(a[0].asString())); } catch (...) { return nova_bool(false); } }, "CreateDirectory"));

        fsObj->obj->set("CreateDirectories", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
            if (a.empty()) return nova_bool(false);
            try { return nova_bool(fs::create_directories(a[0].asString())); } catch (...) { return nova_bool(false); } }, "CreateDirectories"));

        fsObj->obj->set("Remove", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.empty()) return nova_bool(false);
            try { return nova_bool(fs::remove(a[0].asString())); } catch (...) { return nova_bool(false); } }, "Remove"));

        fsObj->obj->set("RemoveAll", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                           {
            if (a.empty()) return nova_num(0);
            try { return nova_num((double)fs::remove_all(a[0].asString())); } catch (...) { return nova_num(0); } }, "RemoveAll"));

        fsObj->obj->set("Rename", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.size() < 2) return nova_bool(false);
            try { fs::rename(a[0].asString(), a[1].asString()); return nova_bool(true); } catch (...) { return nova_bool(false); } }, "Rename"));

        fsObj->obj->set("CopyFile", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.size() < 2) return nova_bool(false);
            try { return nova_bool(fs::copy_file(a[0].asString(), a[1].asString(), fs::copy_options::overwrite_existing)); } catch (...) { return nova_bool(false); } }, "CopyFile"));

        fsObj->obj->set("FileSize", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.empty()) return nova_num(-1);
            try { return nova_num((double)fs::file_size(a[0].asString())); } catch (...) { return nova_num(-1); } }, "FileSize"));

        fsObj->obj->set("AbsolutePath", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                              {
            if (a.empty()) return nova_str("");
            try { return nova_str(fs::absolute(a[0].asString()).string()); } catch (...) { return nova_str(""); } }, "AbsolutePath"));

        fsObj->obj->set("CurrentPath", NovaValue::makeNative([](ValVec, auto) -> Val
                                                             { return nova_str(fs::current_path().string()); }, "CurrentPath"));

        fsObj->obj->set("TempDir", NovaValue::makeNative([](ValVec, auto) -> Val
                                                         { return nova_str(fs::temp_directory_path().string()); }, "TempDir"));

        fsObj->obj->set("ListDir", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                         {
            if (a.empty()) return nova_arr();
            ValVec out;
            try { for (auto &entry : fs::directory_iterator(a[0].asString())) out.push_back(nova_str(entry.path().string())); }
            catch (...) {}
            return nova_arr(out); }, "ListDir"));

        obj->obj->set("FS", fsObj);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  _typeOf
    // ════════════════════════════════════════════════════════════════════════════

    std::string Executor::_typeOf(Val v) const
    {
        if (v.isClass())
            return "class";
        if (!v || v.isNull())
            return "null";
        if (v.isBool())
            return "bool";
        if (v.isNumber())
            return "number";
        if (v.isString())
            return "string";
        if (v.isArray())
            return "array";
        if (v.isFunction())
            return "function";
        if (v.isRange())
            return "range";
        if (v.isPointer())
            return "pointer";
        if (v.isEnum())
            return v->enm ? "enum:" + v->enm->typeName : "enum";
        if (v.isStruct())
            return v->strct ? "struct:" + v->strct->typeName : "struct";
        if (v.isObject())
        {
            Val cls = v->obj->get("__class__");
            if (cls)
                return cls.asString();
            return "object";
        }
        if (v.isNativeVal())
            return "native:" + nativeTypeName(v->natv->type);
        if (v.isBigInt())
            return "bigint";
        if (v.isBigFloat())
            return "bigfloat";
        return "unknown";
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  SYNC FETCH (platform-dispatched)
    Val Executor::_syncFetch(const std::string &url, Val opts)
    {
        FetchRequest req;
        req.url = url;
        req.method = "GET";

        if (opts && opts.isObject())
        {
            Val m = opts->obj->get("method");
            if (m && m.isString())
                req.method = m->sval;

            Val b = opts->obj->get("body");
            if (b && b.isString())
                req.body = b->sval;
            else if (b && b.isObject())
                req.body = stringify(b); // auto JSON

            Val h = opts->obj->get("headers");
            if (h && h.isObject())
                for (auto &[k, v] : h->obj->inner)
                    req.headers[k] = v.asString();
        }

        auto resp = nova_obj();
        try
        {
            FetchResponse r = syncFetch(req);

            resp->obj->set("status", nova_num(r.status));
            resp->obj->set("ok", nova_bool(r.ok));
            resp->obj->set("statusText", nova_str(r.statusText));
            resp->obj->set("url", nova_str(url));

            std::string bodyCapture = r.body;
            resp->obj->set("body", nova_str(bodyCapture));

            resp->obj->set("text", NovaValue::makeNative([bodyCapture](ValVec, auto) -> Val
                                                         { return nova_str(bodyCapture); }, "text"));

            auto exe = this;
            resp->obj->set("json", NovaValue::makeNative([bodyCapture, exe](ValVec, auto cs) -> Val
                                                         {
            try {
                Parser p(bodyCapture, "<fetch-response>");
                auto ast = p.parse();
                if (!ast->body.empty())
                    return exe->evaluate(ast->body[0]->value.get(), exe->globalScope);
            } catch (...) {}
            return nova_null(); }, "json"));

            // headers object
            auto hdrs = nova_obj();
            for (auto &[k, v] : r.headers)
                hdrs->obj->set(k, nova_str(v));
            resp->obj->set("headers", hdrs);
        }
        catch (std::exception &e)
        {
            resp->obj->set("status", nova_num(0));
            resp->obj->set("ok", nova_bool(false));
            resp->obj->set("body", nova_str(""));
            resp->obj->set("error", nova_str(e.what()));
            resp->obj->set("text", NovaValue::makeNative([](ValVec, auto)
                                                         { return nova_str(""); }, "text"));
            resp->obj->set("json", NovaValue::makeNative([](ValVec, auto)
                                                         { return nova_null(); }, "json"));
            resp->obj->set("headers", nova_obj());
        }

        return resp;
    }
    // ════════════════════════════════════════════════════════════════════════════
    //  GLOBAL SCOPE SETUP
    // ════════════════════════════════════════════════════════════════════════════

    void Executor::_setupGlobalScope()
    {
        auto s = globalScope;
        auto std_obj = nova_obj();

        // ── print / println ───────────────────────────────────────────────────────
        auto fn_print = NovaValue::makeNative([this](ValVec args, auto) -> Val
                                              {
            for (size_t i = 0; i < args.size(); i++) {
                if (i) std::cout << " ";
                std::cout << stringify(args[i]);
            }
            return nova_null(); }, "print");
        auto fn_println = NovaValue::makeNative([this](ValVec args, auto) -> Val
                                                {
            for (size_t i = 0; i < args.size(); i++) {
                if (i) std::cout << " ";
                std::cout << stringify(args[i]);
            }
            std::cout << "\n";
            return nova_null(); }, "println");
        std_obj->obj->set("print", fn_print);
        std_obj->obj->set("println", fn_println);

        // Std.GetFuncInfo(fn) -> object:
        // returns all function info (supports nova defined functions only, for now)
        // object is:
        // {
        // name,
        // args,
        // meta: {...}
        // }
        std_obj->obj->set("GetFuncInfo", NovaValue::makeNative([this, s](ValVec a, auto) -> Val
                                                               {
                                                                   auto fn = a[0];
                                                                   
                                                                   auto fnObj = nova_obj();
                                                                   fnObj->obj->set("name", nova_str(fn->fn->name));
                                                                   auto fnArgs = nova_arr();
                                                                   for (int i = 0; i < (int)fn->fn->node->funcArgs.size(); i++)
                                                                   {
                                                                       const FuncArg &fa = fn->fn->node->funcArgs[i];
                                                                       auto fnArg = nova_obj();
                                                                       fnArg->obj->set("name", nova_str(fa.name));
                                                                       fnArg->obj->set("rest", nova_bool(fa.rest));
                                                                       fnArg->obj->set("default", evaluate(fa.defaultValue.get(), s));
                                                                       fnArgs->arr->push(fnArg);
                                                                   };
                                                                   fnObj->obj->set("args", fnArgs);
                                                                   auto fnMeta = nova_obj();
                                                                   fnMeta->obj->set("isNative", nova_bool(fn->fn->isNative));
                                                                   fnMeta->obj->set("isAsync", nova_bool(fn->fn->isAsync));
                                                                   fnMeta->obj->set("isGen", nova_bool(fn->fn->isGenerator));
                                                                   fnMeta->obj->set("isStrict", nova_bool(fn->fn->strictArgs));
                                                                   fnMeta->obj->set("isMemo", nova_bool(fn->fn->memoize));
                                                                   auto mCacheObj = nova_obj();
                                                                   if (fn->fn->memoize) { 
                                                                    for (auto& [k, v] : *fn->fn->memoCache) {
                                                                       mCacheObj->obj->set(k, v);
                                                                    }
                                                                   }
                                                                   fnMeta->obj->set("memoCache", mCacheObj);
                                                                   fnMeta->obj->set("isOnce", nova_bool(fn->fn->once));
                                                                   if (fn->fn->once) fnMeta->obj->set("execCount", nova_num(fn->fn->execCount));
                                                                   fnObj->obj->set("meta", fnMeta);
                                                                   fnObj->obj->set("callable", NovaValue::makeNative([this, fn](ValVec args, auto) -> Val { return nova_bool(fn->fn->isCallable()); }));
                                                                   return fnObj; },
                                                               "GetFuncInfo"));

        std_obj->obj->set("GetFuncSyntax", NovaValue::makeNative([this](ValVec a, auto) -> Val
                                                                 {
                                                                     auto fn = a[0];
                                                                     std::string fnArgs = fn->fn->name + "(";
                                                                     for (int i = 0; i < (int)fn->fn->node->funcArgs.size(); i++)
                                                                     {
                                                                         const FuncArg &fa = fn->fn->node->funcArgs[i];
                                                                         auto sep = "";
                                                                         if (!(i == 0))
                                                                             sep = ", ";
                                                                         fnArgs = fnArgs + sep + fa.name;
                                                                     };
                                                                     return nova_str(fnArgs + ")"); },
                                                                 "GetFuncSyntax"));

        // Std.Satisfies(value, interfaceName) -> bool
        std_obj->obj->set("Satisfies", NovaValue::makeNative([this](ValVec a, auto) -> Val
                                                             {
        if (a.size() < 2) return nova_bool(false);
        return nova_bool(types.satisfies(a[0], a[1].asString(), *this)); }, "Satisfies"));

        // ── input ────────────────────────────────────────────────────────────────
        auto fn_input = NovaValue::makeNative([this](ValVec args, auto) -> Val
                                              {
            std::string prompt = args.empty() ? "" : args[0].asString();
            std::cout << prompt;
            std::string value;
            std::cin >> value;
            return nova_str(value); }, "input");
        std_obj->obj->set("input", fn_input);

        // ── built-in streams ──────────────────────────────────────────────────────
        std_obj->obj->set("stdout", NovaValue::makeStream(std::shared_ptr<std::ostream>(&std::cout, [](std::ostream *) {})));
        std_obj->obj->set("stderr", NovaValue::makeStream(std::shared_ptr<std::ostream>(&std::cerr, [](std::ostream *) {})));
        std_obj->obj->set("stdin", NovaValue::makeStream(std::shared_ptr<std::istream>(&std::cin, [](std::istream *) {})));
        std_obj->obj->set("stdlog", NovaValue::makeStream(std::shared_ptr<std::ostream>(&std::clog, [](std::ostream *) {})));

        // ── Threading ─────────────────────────────────────────────────────────────

        // ThisThread
        {
            auto tt = nova_obj();
            tt->obj->set("get_id", NovaValue::makeNative([](ValVec, auto) -> Val
                                                         { return nova_num((double)std::hash<std::thread::id>{}(std::this_thread::get_id())); }, "get_id"));
            tt->obj->set("sleep", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
                int ms = a.empty() ? 0 : (int)a[0].asNumber();
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                return nova_null(); }, "sleep"));
            tt->obj->set("yield", NovaValue::makeNative([](ValVec, auto) -> Val
                                                        { std::this_thread::yield(); return nova_null(); }, "yield"));
            std_obj->obj->set("ThisThread", tt);
        }

        // Thread — with pause/resume/stop support
        std_obj->obj->set("Thread", NovaValue::makeNative([this, s](ValVec args, auto) -> Val
                                                          {
            if (args.empty() || !args[0].isFunction()) return nova_null();

            auto paused   = std::make_shared<std::atomic<bool>>(false);
            auto stopped  = std::make_shared<std::atomic<bool>>(false);
            auto pause_cv = std::make_shared<std::condition_variable>();
            auto pause_mx = std::make_shared<std::mutex>();

            auto fn  = args[0];
            auto exe = this;

            auto t = std::make_shared<std::thread>([exe, fn, s, paused, stopped, pause_cv, pause_mx]() {
                {
                    std::unique_lock<std::mutex> lk(*pause_mx);
                    pause_cv->wait(lk, [&]{ return !paused->load(); });
                }
                if (stopped->load()) return;
                exe->callFunction(fn, ValVec{}, s);
            });

            auto t_obj = nova_obj();
            t_obj->obj->set("join",     NovaValue::makeNative([t](ValVec, auto) -> Val { if (t->joinable()) t->join(); return nova_null(); }, "join"));
            t_obj->obj->set("detach",   NovaValue::makeNative([t](ValVec, auto) -> Val { if (t->joinable()) t->detach(); return nova_null(); }, "detach"));
            t_obj->obj->set("joinable", NovaValue::makeNative([t](ValVec, auto) -> Val { return nova_bool(t->joinable()); }, "joinable"));
            t_obj->obj->set("get_id",   NovaValue::makeNative([t](ValVec, auto) -> Val { return nova_num((double)std::hash<std::thread::id>{}(t->get_id())); }, "get_id"));
            t_obj->obj->set("pause",    NovaValue::makeNative([paused, pause_cv, pause_mx](ValVec, auto) -> Val { paused->store(true); return nova_null(); }, "pause"));
            t_obj->obj->set("resume",   NovaValue::makeNative([paused, pause_cv](ValVec, auto) -> Val { paused->store(false); pause_cv->notify_all(); return nova_null(); }, "resume"));
            t_obj->obj->set("stop",     NovaValue::makeNative([stopped, paused, pause_cv, t](ValVec, auto) -> Val {
                stopped->store(true); paused->store(false); pause_cv->notify_all();
                if (t->joinable()) t->join();
                return nova_null();
            }, "stop"));

            pause_cv->notify_all();
            return t_obj; }, "Thread"));

        // WorkerThread — isolated Executor + scope, like Node.js Worker
        std_obj->obj->set("WorkerThread", NovaValue::makeNative([this](ValVec args, auto) -> Val
                                                                {
    if (args.empty()) return nova_null();

    std::string workerSrc;
    Val         workerFn = nullptr;

    if (args[0].isString())        workerSrc = args[0]->sval;
    else if (args[0].isFunction()) workerFn  = args[0];
    else                            return nova_null();

    auto result_slot = std::make_shared<Val>(nova_null());
    auto done_flag   = std::make_shared<std::atomic<bool>>(false);
    auto result_mu   = std::make_shared<std::mutex>();
    auto result_cv   = std::make_shared<std::condition_variable>();
    auto stopped     = std::make_shared<std::atomic<bool>>(false);

    using MsgQueue = std::vector<Val>;
    auto in_msgs  = std::make_shared<MsgQueue>();
    auto out_msgs = std::make_shared<MsgQueue>();
    auto msg_mu   = std::make_shared<std::mutex>();

    std::string src      = workerSrc;
    std::string fname_cp = filename;

    auto t = std::make_shared<std::thread>([
        src, fname_cp, workerFn, result_slot, done_flag,
        result_mu, result_cv, stopped, in_msgs, out_msgs, msg_mu
    ]() {
        try {
            auto worker_exe = std::make_unique<Executor>(src, fname_cp);

            // expose postMessage — worker sends OUT
            worker_exe->globalScope->setOwn("postMessage", NovaValue::makeNative(
                [out_msgs, msg_mu](ValVec a, auto) -> Val {
                    std::lock_guard<std::mutex> lk(*msg_mu);
                    out_msgs->push_back(a.empty() ? nova_null() : a[0]);
                    return nova_null();
                }, "postMessage"));

            // expose receive — worker reads IN
            worker_exe->globalScope->setOwn("receive", NovaValue::makeNative(
                [in_msgs, msg_mu](ValVec, auto) -> Val {
                    std::lock_guard<std::mutex> lk(*msg_mu);
                    auto arr = nova_arr(*in_msgs);
                    in_msgs->clear();
                    return arr;
                }, "receive"));

            Val result = nova_null();
            if (!src.empty()) {
                Parser p(src, fname_cp);
                auto ast = p.parse();
                result = worker_exe->run(ast.get(), worker_exe->globalScope);
            } else if (workerFn && workerFn.isFunction() && workerFn->fn->node) {
                result = worker_exe->run(workerFn->fn->node.get(), worker_exe->globalScope);
            }

            std::lock_guard<std::mutex> lk(*result_mu);
            *result_slot = result;
        } catch (...) {}

        done_flag->store(true);
        result_cv->notify_all();
    });

    auto w_obj = nova_obj();
    w_obj->obj->set("join",        NovaValue::makeNative([t](ValVec, auto) -> Val { if (t->joinable()) t->join(); return nova_null(); }, "join"));
    w_obj->obj->set("terminate",   NovaValue::makeNative([t, stopped](ValVec, auto) -> Val { stopped->store(true); if (t->joinable()) t->detach(); return nova_null(); }, "terminate"));
    w_obj->obj->set("wait",        NovaValue::makeNative([done_flag, result_cv, result_mu, result_slot, t](ValVec, auto) -> Val {
        std::unique_lock<std::mutex> lk(*result_mu);
        result_cv->wait(lk, [&]{ return done_flag->load(); });
        return *result_slot;
    }, "wait"));
    w_obj->obj->set("postMessage", NovaValue::makeNative([in_msgs, msg_mu](ValVec a, auto) -> Val { std::lock_guard<std::mutex> lk(*msg_mu); in_msgs->push_back(a.empty() ? nova_null() : a[0]); return nova_null(); }, "postMessage"));
    w_obj->obj->set("receive",     NovaValue::makeNative([out_msgs, msg_mu](ValVec, auto) -> Val { std::lock_guard<std::mutex> lk(*msg_mu); auto arr = nova_arr(*out_msgs); out_msgs->clear(); return arr; }, "receive"));
    w_obj->obj->set("isDone",      NovaValue::makeNative([done_flag](ValVec, auto) -> Val { return nova_bool(done_flag->load()); }, "isDone"));
    return w_obj; }, "WorkerThread"));
        // Mutex
        std_obj->obj->set("Mutex", NovaValue::makeNative([](ValVec, auto) -> Val
                                                         {
            auto mu     = std::make_shared<std::mutex>();
            auto locked = std::make_shared<std::atomic<bool>>(false);
            auto obj    = nova_obj();
            obj->obj->set("lock",     NovaValue::makeNative([mu, locked](ValVec, auto) -> Val { mu->lock(); locked->store(true); return nova_null(); }, "lock"));
            obj->obj->set("unlock",   NovaValue::makeNative([mu, locked](ValVec, auto) -> Val { locked->store(false); mu->unlock(); return nova_null(); }, "unlock"));
            obj->obj->set("tryLock",  NovaValue::makeNative([mu, locked](ValVec, auto) -> Val { bool ok = mu->try_lock(); if (ok) locked->store(true); return nova_bool(ok); }, "tryLock"));
            obj->obj->set("isLocked", NovaValue::makeNative([locked](ValVec, auto) -> Val { return nova_bool(locked->load()); }, "isLocked"));
            obj->obj->set("withLock", NovaValue::makeNative([mu, locked](ValVec, auto cs) -> Val {
                mu->lock(); locked->store(true);
                mu->unlock(); locked->store(false);
                return nova_null();
            }, "withLock"));
            return obj; }, "Mutex"));

        // Atomic
        std_obj->obj->set("Atomic", NovaValue::makeNative([](ValVec args, auto) -> Val
                                                          {
            double init = args.empty() ? 0.0 : args[0].asNumber();
            auto val    = std::make_shared<std::atomic<double>>(init);
            auto obj    = nova_obj();
            obj->obj->set("load",  NovaValue::makeNative([val](ValVec, auto) -> Val { return nova_num(val->load(std::memory_order_seq_cst)); }, "load"));
            obj->obj->set("store", NovaValue::makeNative([val](ValVec a, auto) -> Val { val->store(a.empty() ? 0.0 : a[0].asNumber(), std::memory_order_seq_cst); return nova_null(); }, "store"));
            obj->obj->set("add",   NovaValue::makeNative([val](ValVec a, auto) -> Val {
                double expected = val->load(), desired, delta = a.empty() ? 0.0 : a[0].asNumber();
                do { desired = expected + delta; } while (!val->compare_exchange_weak(expected, desired));
                return nova_num(desired);
            }, "add"));
            obj->obj->set("sub",   NovaValue::makeNative([val](ValVec a, auto) -> Val {
                double expected = val->load(), desired, delta = a.empty() ? 0.0 : a[0].asNumber();
                do { desired = expected - delta; } while (!val->compare_exchange_weak(expected, desired));
                return nova_num(desired);
            }, "sub"));
            obj->obj->set("compareExchange", NovaValue::makeNative([val](ValVec a, auto) -> Val {
                if (a.size() < 2) return nova_bool(false);
                double expected = a[0].asNumber(), desired = a[1].asNumber();
                return nova_bool(val->compare_exchange_strong(expected, desired));
            }, "compareExchange"));
            return obj; }, "Atomic"));

        // Latch (C++20) — single-use countdown to zero
        std_obj->obj->set("Latch", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                         {
            auto latch = std::make_shared<std::latch>(a.empty() ? 1 : (ptrdiff_t)a[0].asNumber());
            auto obj = nova_obj();
            obj->obj->set("countDown", NovaValue::makeNative([latch](ValVec a, auto) -> Val {
                latch->count_down(a.empty() ? 1 : (ptrdiff_t)a[0].asNumber());
                return nova_null();
            }, "countDown"));
            obj->obj->set("wait", NovaValue::makeNative([latch](ValVec, auto) -> Val {
                latch->wait(); return nova_null();
            }, "wait"));
            obj->obj->set("arriveAndWait", NovaValue::makeNative([latch](ValVec, auto) -> Val {
                latch->arrive_and_wait(); return nova_null();
            }, "arriveAndWait"));
            obj->obj->set("tryWait", NovaValue::makeNative([latch](ValVec, auto) -> Val {
                return nova_bool(latch->try_wait());
            }, "tryWait"));
            return obj; }, "Latch"));

        // Barrier (C++20) — reusable; optional Nova completion callback
        std_obj->obj->set("Barrier", NovaValue::makeNative([this](ValVec a, auto cs) -> Val
                                                           {
            ptrdiff_t count = a.empty() ? 1 : (ptrdiff_t)a[0].asNumber();
            Val completionFn = (a.size() > 1 && a[1] && a[1].isFunction()) ? a[1] : nullptr;
            auto exe = this;

            using Fn = std::function<void()>;
            std::shared_ptr<std::barrier<Fn>> barrier;
            if (completionFn) {
                barrier = std::make_shared<std::barrier<Fn>>(count,
                    Fn([exe, completionFn, cs]() noexcept {
                        try { exe->callFunction(completionFn, {}, cs); } catch (...) {}
                    }));
            } else {
                barrier = std::make_shared<std::barrier<Fn>>(count, Fn([]() noexcept {}));
            }

            auto obj = nova_obj();
            obj->obj->set("arriveAndWait", NovaValue::makeNative([barrier](ValVec, auto) -> Val {
                barrier->arrive_and_wait(); return nova_null();
            }, "arriveAndWait"));
            obj->obj->set("arriveAndDrop", NovaValue::makeNative([barrier](ValVec, auto) -> Val {
                barrier->arrive_and_drop(); return nova_null();
            }, "arriveAndDrop"));
            obj->obj->set("arrive", NovaValue::makeNative([barrier](ValVec, auto) -> Val {
                (void)barrier->arrive(); return nova_null();
            }, "arrive"));
            return obj; }, "Barrier"));

        // fetch
        auto fn_fetch = NovaValue::makeNative([this](ValVec args, auto) -> Val
                                              { return _syncFetch(args[0].asString(), args[1]); }, "Fetch");
        std_obj->obj->set("Fetch", fn_fetch);

        // ── operator overloading API ──────────────────────────────────────────────────
        std_obj->obj->set("SetOverload", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
    // SetOverload(obj, "binary"|"unary", op, fn) -> obj
    if (a.size() < 4 || !a[0] || !a[3] || !a[3].isFunction())
        return a.empty() ? nova_null() : a[0];
    Val obj = a[0];
    std::string key = a[1].asString() + ":" + a[2].asString();
    if (!obj->overloads)
        obj->overloads = std::make_shared<ValMap>();
    (*obj->overloads)[key] = a[3];
    return obj; }, "SetOverload"));

        std_obj->obj->set("SetOverloadRaw", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
    // SetOverloadRaw(obj, rawkey, fn) -> obj
    if (a.size() < 3 || !a[0] || !a[2] || !a[2].isFunction())
        return a.empty() ? nova_null() : a[0];
    Val obj = a[0];
    if (!obj->overloads)
        obj->overloads = std::make_shared<ValMap>();
    (*obj->overloads)[a[1].asString()] = a[2];
    return obj; }, "SetOverloadRaw"));

        std_obj->obj->set("GetOverload", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
    // GetOverload(obj, "binary"|"unary", op) -> fn | null
    if (a.size() < 3 || !a[0] || !a[0]->overloads)
        return nova_null();
    std::string key = a[1].asString() + ":" + a[2].asString();
    auto it = a[0]->overloads->find(key);
    return it != a[0]->overloads->end() ? it->second : nova_null(); }, "GetOverload"));

        std_obj->obj->set("GetOverloadRaw", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
    // GetOverloadRaw(obj, rawkey) -> fn | null
    if (a.size() < 2 || !a[0] || !a[0]->overloads)
        return nova_null();
    auto it = a[0]->overloads->find(a[1].asString());
    return it != a[0]->overloads->end() ? it->second : nova_null(); }, "GetOverloadRaw"));

        // ── Std.IOManip ───────────────────────────────────────────────────────────────
        // Since Nova streams are wrapped std::ostream objects, IOManip works by
        // returning a formatted string via an internal ostringstream. Stateful flags
        // (hex, fixed, etc.) are passed as options to format().
        {
            auto iom = nova_obj();

            // ── format(value, opts?) — the core function ──────────────────────────────
            // opts object keys (all optional):
            //   width      : number   — setw
            //   fill       : string   — setfill (first char used)
            //   precision  : number   — setprecision
            //   base       : 8|10|16  — setbase / hex / oct / dec
            //   floatFmt   : "fixed" | "scientific" | "hex" | "default"
            //   align      : "left" | "right" | "internal"
            //   boolalpha  : bool
            //   showbase   : bool
            //   showpoint  : bool
            //   showpos    : bool
            //   uppercase  : bool
            iom->obj->set("format", NovaValue::makeNative([this](ValVec a, auto) -> Val
                                                          {
        if (a.empty()) return nova_str("");
        Val val  = a[0];
        Val opts = a.size() > 1 ? a[1] : nullptr;

        std::ostringstream ss;

        if (opts && opts.isObject()) {
            // width
            Val w = opts->obj->get("width");
            if (w && w.isNumber()) ss << std::setw((int)w->nval);

            // fill
            Val f = opts->obj->get("fill");
            if (f && f.isString() && !f->sval.empty())
                ss << std::setfill(f->sval[0]);

            // precision
            Val p = opts->obj->get("precision");
            if (p && p.isNumber()) ss << std::setprecision((int)p->nval);

            // base
            Val b = opts->obj->get("base");
            if (b && b.isNumber()) {
                int base = (int)b->nval;
                if      (base == 8)  ss << std::oct;
                else if (base == 16) ss << std::hex;
                else                 ss << std::dec;
            }

            // float format
            Val ff = opts->obj->get("floatFmt");
            if (ff && ff.isString()) {
                if      (ff->sval == "fixed")      ss << std::fixed;
                else if (ff->sval == "scientific")  ss << std::scientific;
                else if (ff->sval == "hex")         ss << std::hexfloat;
                else                                ss << std::defaultfloat;
            }

            // alignment
            Val al = opts->obj->get("align");
            if (al && al.isString()) {
                if      (al->sval == "left")     ss << std::left;
                else if (al->sval == "right")    ss << std::right;
                else if (al->sval == "internal") ss << std::internal;
            }

            // boolean flags
            auto flag = [&](const std::string& key,
                            std::ios_base::fmtflags on,
                            std::ios_base::fmtflags off) {
                Val v = opts->obj->get(key);
                if (!v) return;
                if (v.asBool()) ss.setf(on);
                else            ss.unsetf(off);
            };
            flag("boolalpha",  std::ios::boolalpha,  std::ios::boolalpha);
            flag("showbase",   std::ios::showbase,   std::ios::showbase);
            flag("showpoint",  std::ios::showpoint,  std::ios::showpoint);
            flag("showpos",    std::ios::showpos,    std::ios::showpos);
            flag("uppercase",  std::ios::uppercase,  std::ios::uppercase);
        }

        // write the value
        if (val.isNumber())     ss << val->nval;
        else if (val.isBool())  ss << (val->bval ? "true" : "false");
        else                    ss << stringify(val);

        return nova_str(ss.str()); }, "format"));

            // ── setw(n) → returns opts object for use with format() ──────────────────
            iom->obj->set("setw", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
        auto o = nova_obj();
        o->obj->set("width", a.empty() ? nova_num(0) : a[0]);
        return o; }, "setw"));

            // ── setfill(ch) ──────────────────────────────────────────────────────────
            iom->obj->set("setfill", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                           {
        auto o = nova_obj();
        o->obj->set("fill", a.empty() ? nova_str(" ") : nova_str(a[0].asString()));
        return o; }, "setfill"));

            // ── setprecision(n) ──────────────────────────────────────────────────────
            iom->obj->set("setprecision", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                {
        auto o = nova_obj();
        o->obj->set("precision", a.empty() ? nova_num(6) : a[0]);
        return o; }, "setprecision"));

            // ── setbase(8|10|16) ─────────────────────────────────────────────────────
            iom->obj->set("setbase", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                           {
        auto o = nova_obj();
        o->obj->set("base", a.empty() ? nova_num(10) : a[0]);
        return o; }, "setbase"));

            // ── base shortcuts ────────────────────────────────────────────────────────
            for (auto &[name, base] : std::initializer_list<std::pair<const char *, int>>{
                     {"hex", 16}, {"oct", 8}, {"dec", 10}})
            {
                int b = base;
                iom->obj->set(name, NovaValue::makeNative([b](ValVec, auto) -> Val
                                                          {
            auto o = nova_obj();
            o->obj->set("base", nova_num(b));
            return o; }, name));
            }

            // ── float format shortcuts ────────────────────────────────────────────────
            for (auto &name : {"fixed", "scientific", "hexfloat", "defaultfloat"})
            {
                std::string n = name;
                std::string tag = (n == "hexfloat") ? "hex" : (n == "defaultfloat") ? "default"
                                                                                    : n;
                iom->obj->set(name, NovaValue::makeNative([tag](ValVec, auto) -> Val
                                                          {
            auto o = nova_obj();
            o->obj->set("floatFmt", nova_str(tag));
            return o; }, name));
            }

            // ── alignment shortcuts ───────────────────────────────────────────────────
            for (auto &name : {"left", "right", "internal"})
            {
                std::string n = name;
                iom->obj->set(name, NovaValue::makeNative([n](ValVec, auto) -> Val
                                                          {
            auto o = nova_obj();
            o->obj->set("align", nova_str(n));
            return o; }, name));
            }

            // ── boolean flag shortcuts ────────────────────────────────────────────────
            for (auto &[name, key, val] : std::initializer_list<std::tuple<const char *, const char *, bool>>{
                     {"boolalpha", "boolalpha", true},
                     {"noboolalpha", "boolalpha", false},
                     {"showbase", "showbase", true},
                     {"noshowbase", "showbase", false},
                     {"showpoint", "showpoint", true},
                     {"noshowpoint", "showpoint", false},
                     {"showpos", "showpos", true},
                     {"noshowpos", "showpos", false},
                     {"uppercase", "uppercase", true},
                     {"nouppercase", "uppercase", false}})
            {
                std::string k = key;
                bool v = val;
                iom->obj->set(name, NovaValue::makeNative([k, v](ValVec, auto) -> Val
                                                          {
            auto o = nova_obj();
            o->obj->set(k, nova_bool(v));
            return o; }, name));
            }

            // ── setiosflags / resetiosflags (raw bitmask, for power users) ───────────
            iom->obj->set("setiosflags", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
        auto o = nova_obj();
        o->obj->set("__iosflags_set__", a.empty() ? nova_num(0) : a[0]);
        return o; }, "setiosflags"));

            iom->obj->set("resetiosflags", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
        auto o = nova_obj();
        o->obj->set("__iosflags_reset__", a.empty() ? nova_num(0) : a[0]);
        return o; }, "resetiosflags"));

            // ── ws(streamObj) — skip whitespace from a Nova stdin stream ─────────────
            iom->obj->set("ws", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
                                                          if (a.empty() || !a[0].isObject())
                                                              return nova_null();
                                                          Val handle = a[0]->obj->get("__handle__");
                                                          if (!handle)
                                                              return a[0];
                                                          auto *is = nativeStream<std::istream>(handle);
                                                          if (is)
                                                              *is >> std::ws;
                                                          return a[0]; // return stream for chaining
                                                      },
                                                      "ws"));

            // ── endl / ends / flush — write to a Nova stream object ──────────────────
            iom->obj->set("endl", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
        if (a.empty() || !a[0].isObject()) return nova_null();
        Val handle = a[0]->obj->get("__handle__");
        if (!handle) return a[0];
        auto* os = nativeStream<std::ostream>(handle);
        if (os) *os << std::endl;
        return a[0]; }, "endl"));

            iom->obj->set("ends", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
        if (a.empty() || !a[0].isObject()) return nova_null();
        Val handle = a[0]->obj->get("__handle__");
        if (!handle) return a[0];
        auto* os = nativeStream<std::ostream>(handle);
        if (os) *os << std::ends;
        return a[0]; }, "ends"));

            iom->obj->set("flush", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                         {
        if (a.empty() || !a[0].isObject()) return nova_null();
        Val handle = a[0]->obj->get("__handle__");
        if (!handle) return a[0];
        auto* os = nativeStream<std::ostream>(handle);
        if (os) *os << std::flush;
        return a[0]; }, "flush"));

            std_obj->obj->set("IOManip", iom);
        }

        // ── eval ─────────────────────────────────────────────────────────────────
        auto fn_eval = NovaValue::makeNative([this](ValVec args, std::shared_ptr<Scope> cs) -> Val
                                             {
                                                 std::string content = args.empty() ? "" : args[0].asString();
                                                 Parser p(content, filename);
                                                 auto ast = p.parse();
                                                 Executor evalExe(content, filename);
                                                 evalExe.globalScope = globalScope;
                                                 return evalExe.run(ast.get(), cs); // ← cs instead of evalScope
                                             },
                                             "eval");
        std_obj->obj->set("eval", fn_eval);
        // ── expreval ──────────────────────────────────────────────────────────────────
        auto fn_expreval = NovaValue::makeNative([this](ValVec args, auto) -> Val
                                                 {
    if (args.empty()) return nova_null();
    std::string content = args[0].asString();
    std::string fname = args.size() > 1 ? args[1].asString() : filename;

    Parser p(content, fname);
    auto ast = p.parse();
    if (!ast || ast->body.empty()) return nova_null();

    // unwrap the Exec wrapper that statement() wraps expressions in
    const Node* exprNode = ast->body[0].get();
    if (exprNode && exprNode->kind == Node::Kind::Exec && exprNode->value)
        return evaluate(exprNode->value.get(), globalScope);
    return evaluate(exprNode, globalScope); }, "expreval");
        std_obj->obj->set("expreval", fn_expreval);

        // ── parser ────────────────────────────────────────────────────────────────────
        auto fn_parser = NovaValue::makeNative([this](ValVec args, std::shared_ptr<Scope> cs) -> Val
                                               {
    std::string src   = args.empty()      ? "" : args[0].asString();
    std::string fname = args.size() > 1   ? args[1].asString() : filename;

    auto srcPtr   = std::make_shared<std::string>(src);
    auto fnamePtr = std::make_shared<std::string>(fname);

    // ── nodeToVal: recursive Node* → Nova object ──────────────────────────────
    auto nodeToVal = std::make_shared<std::function<Val(const Node*)>>();
    *nodeToVal = [nodeToVal](const Node* n) -> Val {
        if (!n) return nova_null();
        auto obj = nova_obj();

        auto kindStr = [](Node::Kind k) -> std::string {
            switch (k) {
            case Node::Kind::Program:        return "Program";
            case Node::Kind::Exec:           return "Exec";
            case Node::Kind::Declare:        return "Declare";
            case Node::Kind::Function:       return "Function";
            case Node::Kind::Class:          return "Class";
            case Node::Kind::Branch:         return "Branch";
            case Node::Kind::ForOf:          return "ForOf";
            case Node::Kind::ForIn:          return "ForIn";
            case Node::Kind::Each:           return "Each";
            case Node::Kind::Switch:         return "Switch";
            case Node::Kind::Match:          return "Match";
            case Node::Kind::Try:            return "Try";
            case Node::Kind::Return:         return "Return";
            case Node::Kind::Throw:          return "Throw";
            case Node::Kind::Break:          return "Break";
            case Node::Kind::Continue:       return "Continue";
            case Node::Kind::Goback:         return "Goback";
            case Node::Kind::Yield:          return "Yield";
            case Node::Kind::Binary:         return "Binary";
            case Node::Kind::Unary:          return "Unary";
            case Node::Kind::Prefix:         return "Prefix";
            case Node::Kind::Postfix:        return "Postfix";
            case Node::Kind::Ternary:        return "Ternary";
            case Node::Kind::IfTernary:      return "IfTernary";
            case Node::Kind::Assign:         return "Assign";
            case Node::Kind::CompoundAssign: return "CompoundAssign";
            case Node::Kind::Prop:           return "Prop";
            case Node::Kind::OptionalProp:   return "OptionalProp";
            case Node::Kind::Subscript:      return "Subscript";
            case Node::Kind::OptionalSubscript: return "OptionalSubscript";
            case Node::Kind::Call:           return "Call";
            case Node::Kind::OptionalCall:   return "OptionalCall";
            case Node::Kind::StructCall:     return "StructCall";
            case Node::Kind::New:            return "New";
            case Node::Kind::Await:          return "Await";
            case Node::Kind::Spread:         return "Spread";
            case Node::Kind::Array:          return "Array";
            case Node::Kind::Object:         return "Object";
            case Node::Kind::Value:          return "Value";
            case Node::Kind::Ref:            return "Ref";
            case Node::Kind::DVar:           return "DVar";
            case Node::Kind::ArrowFunc:      return "ArrowFunc";
            case Node::Kind::FString:        return "FString";
            case Node::Kind::Regex:          return "Regex";
            case Node::Kind::Import:         return "Import";
            case Node::Kind::ImportBuiltin:  return "ImportBuiltin";
            case Node::Kind::ImportKit:      return "ImportKit";
            case Node::Kind::FromImport:     return "FromImport";
            case Node::Kind::Export:         return "Export";
            case Node::Kind::DefaultExport:  return "DefaultExport";
            case Node::Kind::Namespace:      return "Namespace";
            case Node::Kind::Block:          return "Block";
            case Node::Kind::TypeDecl:       return "TypeDecl";
            case Node::Kind::StructDecl:     return "StructDecl";
            case Node::Kind::EnumDecl:       return "EnumDecl";
            case Node::Kind::InterfaceDecl:  return "InterfaceDecl";
            case Node::Kind::TraitDecl:      return "TraitDecl";
            case Node::Kind::ImplDecl:       return "ImplDecl";
            case Node::Kind::Perform:        return "Perform";
            case Node::Kind::Link:           return "Link";
            case Node::Kind::Cast:           return "Cast";
            case Node::Kind::Native:         return "Native";
            case Node::Kind::Unative:        return "Unative";
            case Node::Kind::Partial:        return "Partial";
            case Node::Kind::Server:         return "Server";
            case Node::Kind::FetchStmt:      return "FetchStmt";
            case Node::Kind::FetchExpr:      return "FetchExpr";
            case Node::Kind::HttpRequest:    return "HttpRequest";
            case Node::Kind::EmitEvent:      return "EmitEvent";
            case Node::Kind::OnEvent:        return "OnEvent";
            case Node::Kind::WithCtx:        return "WithCtx";
            case Node::Kind::WithOption:     return "WithOption";
            case Node::Kind::DotCmd:         return "DotCmd";
            case Node::Kind::ExecComment:    return "ExecComment";
            case Node::Kind::Run:            return "Run";
            case Node::Kind::RateCast:       return "RateCast";
            case Node::Kind::Deref:          return "Deref";
            case Node::Kind::HasOpt:         return "HasOpt";
            case Node::Kind::GetOpt:         return "GetOpt";
            case Node::Kind::OptDecl:        return "OptDecl";
            case Node::Kind::UrlLiteral:     return "UrlLiteral";
            default:                         return "Unknown";
            }
        };

        obj->obj->set("kind",       nova_str(kindStr(n->kind)));
        obj->obj->set("line",       nova_num(n->line));
        obj->obj->set("column",     nova_num(n->column));

        if (!n->name.empty())        obj->obj->set("name",        nova_str(n->name));
        if (!n->strval.empty())      obj->obj->set("strval",      nova_str(n->strval));
        if (n->numval != 0.0)        obj->obj->set("numval",      nova_num(n->numval));
        if (!n->op.empty())          obj->obj->set("op",          nova_str(n->op));
        if (!n->branchType.empty())  obj->obj->set("branchType",  nova_str(n->branchType));
        if (!n->varName.empty())     obj->obj->set("varName",     nova_str(n->varName));
        if (!n->indexName.empty())   obj->obj->set("indexName",   nova_str(n->indexName));
        if (!n->catchName.empty())   obj->obj->set("catchName",   nova_str(n->catchName));
        if (!n->kitName.empty())     obj->obj->set("kitName",     nova_str(n->kitName));
        if (!n->traitName.empty())   obj->obj->set("traitName",   nova_str(n->traitName));
        if (!n->forType.empty())     obj->obj->set("forType",     nova_str(n->forType));
        if (!n->flags.empty())       obj->obj->set("flags",       nova_str(n->flags));
        if (!n->method.empty())      obj->obj->set("method",      nova_str(n->method));
        if (!n->castType.empty())    obj->obj->set("castType",    nova_str(n->castType));
        if (!n->param.empty())       obj->obj->set("param",       nova_str(n->param));
        if (!n->varNameOut.empty())  obj->obj->set("varNameOut",  nova_str(n->varNameOut));
        if (!n->flag.empty())        obj->obj->set("flag",        nova_str(n->flag));
        if (!n->cmd)                 {} else obj->obj->set("cmd", (*nodeToVal)(n->cmd.get()));

        // flags
        if (n->isConst)     obj->obj->set("isConst",     nova_bool(true));
        if (n->isPointer)   obj->obj->set("isPointer",   nova_bool(true));
        if (n->isAsync)     obj->obj->set("isAsync",     nova_bool(true));
        if (n->isGenerator) obj->obj->set("isGenerator", nova_bool(true));
        if (n->memoize)     obj->obj->set("memoize",     nova_bool(true));
        if (n->once_fn)     obj->obj->set("once",        nova_bool(true));
        if (n->strictArgs)  obj->obj->set("strictArgs",  nova_bool(true));
        if (n->terminate)   obj->obj->set("terminate",   nova_bool(true));
        if (n->isExpr)      obj->obj->set("isExpr",      nova_bool(true));
        if (n->defer_fn)    obj->obj->set("defer_fn",    nova_bool(true));
        if (n->once_fn)     obj->obj->set("once_fn",     nova_bool(true));

        if (n->kind == Node::Kind::Value) {
            std::string vk = "literal";
            if      (n->valueKind == Node::ValueKind::Number) vk = "number";
            else if (n->valueKind == Node::ValueKind::String) vk = "string";
            obj->obj->set("valueKind", nova_str(vk));
        }

        // child nodes
        auto addChild = [&](const std::string& key, const NodePtr& child) {
            if (child) obj->obj->set(key, (*nodeToVal)(child.get()));
        };
        addChild("value",      n->value);
        addChild("left",       n->left);
        addChild("right",      n->right);
        addChild("cond",       n->cond);
        addChild("consequent", n->consequent);
        addChild("alternate",  n->alternate);
        addChild("callee",     n->callee);
        addChild("operand",    n->operand);
        addChild("index",      n->index);
        addChild("init",       n->init);
        addChild("update",     n->update);
        addChild("next",       n->next);
        addChild("iterable",   n->iterable);
        addChild("object",     n->object);
        addChild("url",        n->url);
        addChild("options",    n->options);
        addChild("port",       n->port);
        addChild("superClass", n->superClass);
        addChild("target",     n->target);
        addChild("source",     n->source);
        addChild("event",      n->event);
        addChild("subject",    n->subject);
        addChild("deferStmt",  n->deferStmt);
        addChild("timeout",    n->timeout);

        if (!n->body.empty()) {
            ValVec arr;
            for (auto& s : n->body) arr.push_back((*nodeToVal)(s.get()));
            obj->obj->set("body", nova_arr(std::move(arr)));
        }
        if (!n->callArgs.empty()) {
            ValVec arr;
            for (auto& a : n->callArgs) arr.push_back((*nodeToVal)(a.get()));
            obj->obj->set("callArgs", nova_arr(std::move(arr)));
        }
        if (!n->elements.empty()) {
            ValVec arr;
            for (auto& e : n->elements) arr.push_back((*nodeToVal)(e.get()));
            obj->obj->set("elements", nova_arr(std::move(arr)));
        }
        if (!n->parts.empty()) {
            ValVec arr;
            for (auto& pt : n->parts) arr.push_back((*nodeToVal)(pt.get()));
            obj->obj->set("parts", nova_arr(std::move(arr)));
        }
        if (!n->decorators.empty()) {
            ValVec arr;
            for (auto& d : n->decorators) arr.push_back((*nodeToVal)(d.get()));
            obj->obj->set("decorators", nova_arr(std::move(arr)));
        }
        if (!n->funcArgs.empty()) {
            ValVec arr;
            for (auto& fa : n->funcArgs) {
                auto fobj = nova_obj();
                fobj->obj->set("name", nova_str(fa.name));
                if (fa.rest) fobj->obj->set("rest", nova_bool(true));
                if (fa.type) {
                    // store type as name string for simplicity
                    fobj->obj->set("typeName", nova_str(fa.type->name));
                }
                if (fa.defaultValue) fobj->obj->set("default", (*nodeToVal)(fa.defaultValue.get()));
                arr.push_back(fobj);
            }
            obj->obj->set("funcArgs", nova_arr(std::move(arr)));
        }
        if (!n->names.empty()) {
            ValVec arr;
            for (auto& nm : n->names) arr.push_back(nova_str(nm));
            obj->obj->set("names", nova_arr(std::move(arr)));
        }
        if (!n->typeParams.empty()) {
            ValVec arr;
            for (auto& tp : n->typeParams) arr.push_back(nova_str(tp));
            obj->obj->set("typeParams", nova_arr(std::move(arr)));
        }
        if (!n->extendsNames.empty()) {
            ValVec arr;
            for (auto& en : n->extendsNames) arr.push_back(nova_str(en));
            obj->obj->set("extendsNames", nova_arr(std::move(arr)));
        }
        if (!n->impls.empty()) {
            ValVec arr;
            for (auto& im : n->impls) arr.push_back(nova_str(im));
            obj->obj->set("impls", nova_arr(std::move(arr)));
        }
        if (!n->props.empty()) {
            ValVec arr;
            for (auto& p : n->props) {
                auto pobj = nova_obj();
                pobj->obj->set("key", nova_str(p.key));
                auto pkStr = [](ObjectProp::Kind k) -> std::string {
                    switch (k) {
                    case ObjectProp::Kind::Normal:   return "normal";
                    case ObjectProp::Kind::Spread:   return "spread";
                    case ObjectProp::Kind::Computed: return "computed";
                    case ObjectProp::Kind::Accessor: return "accessor";
                    default: return "normal";
                    }
                };
                pobj->obj->set("kind", nova_str(pkStr(p.kind)));
                if (p.value)   pobj->obj->set("value",    (*nodeToVal)(p.value.get()));
                if (p.keyExpr) pobj->obj->set("keyExpr",  (*nodeToVal)(p.keyExpr.get()));
                if (!p.accessor.empty()) pobj->obj->set("accessor", nova_str(p.accessor));
                if (p.boolProp) pobj->obj->set("boolProp", nova_bool(true));
                arr.push_back(pobj);
            }
            obj->obj->set("props", nova_arr(std::move(arr)));
        }
        // try/catch/finally/else/handle
        auto addNodeList = [&](const std::string& key, const NodeList& list) {
            if (!list.empty()) {
                ValVec arr;
                for (auto& s : list) arr.push_back((*nodeToVal)(s.get()));
                obj->obj->set(key, nova_arr(std::move(arr)));
            }
        };
        addNodeList("tryBody",     n->tryBody);
        addNodeList("catchBody",   n->catchBody);
        addNodeList("finallyBody", n->finallyBody);
        addNodeList("elseBody",    n->elseBody);

        if (!n->handleClauses.empty()) {
            ValVec arr;
            for (auto& h : n->handleClauses) {
                auto hobj = nova_obj();
                hobj->obj->set("name", nova_str(h.name));
                ValVec ps;
                for (auto& p : h.params) ps.push_back(nova_str(p));
                hobj->obj->set("params", nova_arr(std::move(ps)));
                ValVec hb;
                for (auto& s : h.body) hb.push_back((*nodeToVal)(s.get()));
                hobj->obj->set("body", nova_arr(std::move(hb)));
                arr.push_back(hobj);
            }
            obj->obj->set("handleClauses", nova_arr(std::move(arr)));
        }
        if (!n->cases.empty()) {
            ValVec arr;
            for (auto& c : n->cases) {
                auto cobj = nova_obj();
                cobj->obj->set("kind", nova_str(c.kind == CaseNode::Kind::Default ? "default" : "case"));
                cobj->obj->set("line",   nova_num(c.line));
                cobj->obj->set("column", nova_num(c.column));
                if (c.value) cobj->obj->set("value", (*nodeToVal)(c.value.get()));
                ValVec cb;
                for (auto& s : c.body) cb.push_back((*nodeToVal)(s.get()));
                cobj->obj->set("body", nova_arr(std::move(cb)));
                arr.push_back(cobj);
            }
            obj->obj->set("cases", nova_arr(std::move(arr)));
        }
        if (!n->whenCases.empty()) {
            ValVec arr;
            for (auto& w : n->whenCases) {
                auto wobj = nova_obj();
                wobj->obj->set("line",   nova_num(w.line));
                wobj->obj->set("column", nova_num(w.column));
                ValVec pats;
                for (auto& p : w.patterns) pats.push_back((*nodeToVal)(p.get()));
                wobj->obj->set("patterns", nova_arr(std::move(pats)));
                ValVec wb;
                for (auto& s : w.body) wb.push_back((*nodeToVal)(s.get()));
                wobj->obj->set("body", nova_arr(std::move(wb)));
                arr.push_back(wobj);
            }
            obj->obj->set("whenCases", nova_arr(std::move(arr)));
        }
        if (!n->members.empty()) {
            ValVec arr;
            for (auto& m : n->members) {
                auto mobj = nova_obj();
                mobj->obj->set("name", nova_str(m.name));
                mobj->obj->set("kind", nova_str(m.kind == ClassMember::Kind::Method ? "method" : "field"));
                if (!m.accessor.empty()) mobj->obj->set("accessor", nova_str(m.accessor));
                if (m.value)             mobj->obj->set("value",    (*nodeToVal)(m.value.get()));
                if (m.returnType)        mobj->obj->set("returnTypeName", nova_str(m.returnType->name));
                if (!m.args.empty()) {
                    ValVec margs;
                    for (auto& fa : m.args) {
                        auto fobj = nova_obj();
                        fobj->obj->set("name", nova_str(fa.name));
                        if (fa.rest) fobj->obj->set("rest", nova_bool(true));
                        if (fa.defaultValue) fobj->obj->set("default", (*nodeToVal)(fa.defaultValue.get()));
                        margs.push_back(fobj);
                    }
                    mobj->obj->set("args", nova_arr(std::move(margs)));
                }
                if (!m.body.empty()) {
                    ValVec mb;
                    for (auto& s : m.body) mb.push_back((*nodeToVal)(s.get()));
                    mobj->obj->set("body", nova_arr(std::move(mb)));
                }
                if (!m.decorators.empty()) {
                    ValVec md;
                    for (auto& d : m.decorators) md.push_back((*nodeToVal)(d.get()));
                    mobj->obj->set("decorators", nova_arr(std::move(md)));
                }
                arr.push_back(mobj);
            }
            obj->obj->set("members", nova_arr(std::move(arr)));
        }
        if (!n->structFields.empty()) {
            ValVec arr;
            for (auto& sf : n->structFields) {
                auto sfobj = nova_obj();
                sfobj->obj->set("name",     nova_str(sf.name));
                sfobj->obj->set("typeName", nova_str(sf.type.name));
                sfobj->obj->set("typeKind", nova_str([](TypeExpr::Kind k) -> std::string {
                    switch (k) {
                    case TypeExpr::Kind::Named:    return "named";
                    case TypeExpr::Kind::Union:    return "union";
                    case TypeExpr::Kind::Array:    return "array";
                    case TypeExpr::Kind::Function: return "function";
                    case TypeExpr::Kind::Shape:    return "shape";
                    case TypeExpr::Kind::Value:    return "value";
                    default: return "unknown";
                    }
                }(sf.type.kind)));
                if (sf.defaultValue) sfobj->obj->set("default", (*nodeToVal)(sf.defaultValue.get()));
                if (!sf.decorators.empty()) {
                    ValVec dd;
                    for (auto& d : sf.decorators) dd.push_back((*nodeToVal)(d.get()));
                    sfobj->obj->set("decorators", nova_arr(std::move(dd)));
                }
                arr.push_back(sfobj);
            }
            obj->obj->set("structFields", nova_arr(std::move(arr)));
        }
        if (!n->enumVariants.empty()) {
            ValVec arr;
            for (auto& ev : n->enumVariants) {
                auto evobj = nova_obj();
                evobj->obj->set("name", nova_str(ev.name));
                if (ev.value) evobj->obj->set("value", (*nodeToVal)(ev.value.get()));
                arr.push_back(evobj);
            }
            obj->obj->set("enumVariants", nova_arr(std::move(arr)));
        }
        if (!n->ifaceMembers.empty()) {
            ValVec arr;
            for (auto& im : n->ifaceMembers) {
                auto imobj = nova_obj();
                imobj->obj->set("name",       nova_str(im.name));
                imobj->obj->set("isOptional", nova_bool(im.isOptional));
                imobj->obj->set("isMethod",   nova_bool(im.isMethod));
                if (im.returnType) imobj->obj->set("returnTypeName", nova_str(im.returnType->name));
                if (!im.params.empty()) {
                    ValVec ps;
                    for (auto& [pname, ptype] : im.params) {
                        auto pobj = nova_obj();
                        pobj->obj->set("name", nova_str(pname));
                        if (ptype) pobj->obj->set("typeName", nova_str(ptype->name));
                        ps.push_back(pobj);
                    }
                    imobj->obj->set("params", nova_arr(std::move(ps)));
                }
                arr.push_back(imobj);
            }
            obj->obj->set("ifaceMembers", nova_arr(std::move(arr)));
        }
        if (!n->routes.empty()) {
            ValVec arr;
            for (auto& r : n->routes) {
                auto robj = nova_obj();
                robj->obj->set("method", nova_str(r.method));
                robj->obj->set("line",   nova_num(r.line));
                robj->obj->set("column", nova_num(r.column));
                if (r.path) robj->obj->set("path", (*nodeToVal)(r.path.get()));
                if (!r.params.empty()) {
                    ValVec ps;
                    for (auto& p : r.params) ps.push_back(nova_str(p));
                    robj->obj->set("params", nova_arr(std::move(ps)));
                }
                if (!r.body.empty()) {
                    ValVec rb;
                    for (auto& s : r.body) rb.push_back((*nodeToVal)(s.get()));
                    robj->obj->set("body", nova_arr(std::move(rb)));
                }
                arr.push_back(robj);
            }
            obj->obj->set("routes", nova_arr(std::move(arr)));
        }
        // destructure patterns
        if (std::holds_alternative<ObjPattern>(n->destructure)) {
            auto& pat = std::get<ObjPattern>(n->destructure);
            if (!pat.props.empty()) {
                ValVec arr;
                for (auto& p : pat.props) {
                    auto pobj = nova_obj();
                    pobj->obj->set("key",   nova_str(p.key));
                    pobj->obj->set("alias", nova_str(p.alias));
                    if (p.defaultValue) pobj->obj->set("default", (*nodeToVal)(p.defaultValue.get()));
                    arr.push_back(pobj);
                }
                obj->obj->set("objPattern", nova_arr(std::move(arr)));
            }
        }
        if (std::holds_alternative<ArrPattern>(n->destructure)) {
            auto& pat = std::get<ArrPattern>(n->destructure);
            if (!pat.elements.empty()) {
                ValVec arr;
                for (auto& e : pat.elements) {
                    auto eobj = nova_obj();
                    eobj->obj->set("name", nova_str(e.name));
                    if (e.rest) eobj->obj->set("rest", nova_bool(true));
                    if (e.defaultValue) eobj->obj->set("default", (*nodeToVal)(e.defaultValue.get()));
                    arr.push_back(eobj);
                }
                obj->obj->set("arrPattern", nova_arr(std::move(arr)));
            }
        }
        // modifiers
        if (n->modifiers) {
            auto mobj = nova_obj();
            auto& m = *n->modifiers;
            if (m.frozen)   mobj->obj->set("frozen",   nova_bool(true));
            if (m.lazy)     mobj->obj->set("lazy",     nova_bool(true));
            if (m.tracked)  mobj->obj->set("tracked",  nova_bool(true));
            if (m.nonull)   mobj->obj->set("nonull",   nova_bool(true));
            if (m.once)     mobj->obj->set("once",     nova_bool(true));
            if (!m.clampType.empty()) mobj->obj->set("clampType", nova_str(m.clampType));
            if (m.setter)   mobj->obj->set("setter",   (*nodeToVal)(m.setter.get()));
            if (m.getter)   mobj->obj->set("getter",   (*nodeToVal)(m.getter.get()));
            if (m.clampExpr) mobj->obj->set("clampExpr", (*nodeToVal)(m.clampExpr.get()));
            obj->obj->set("modifiers", mobj);
        }
        // explicit type
        if (n->explicitType) {
            auto tobj = nova_obj();
            tobj->obj->set("name", nova_str(n->explicitType->name));
            tobj->obj->set("kind", nova_str([](TypeExpr::Kind k) -> std::string {
                switch (k) {
                case TypeExpr::Kind::Named:    return "named";
                case TypeExpr::Kind::Union:    return "union";
                case TypeExpr::Kind::Intersect:return "intersect";
                case TypeExpr::Kind::Array:    return "array";
                case TypeExpr::Kind::Function: return "function";
                case TypeExpr::Kind::Shape:    return "shape";
                case TypeExpr::Kind::Value:    return "value";
                default: return "unknown";
                }
            }(n->explicitType->kind)));
            obj->obj->set("explicitType", tobj);
        }
        if (n->returnType) {
            obj->obj->set("returnTypeName", nova_str(n->returnType->name));
        }

        return obj;
    };

    // ── token helper ──────────────────────────────────────────────────────────
    auto tokensOf = [](const std::string& s, const std::string& f) -> Val {
        Lexer lex(s, f);
        auto toks = lex.tokenize();
        ValVec out;
        for (auto& t : toks) {
            if (t.type == TT::EOF_TOKEN) break;
            auto tobj = nova_obj();
            auto ttStr = [](TT ty) -> std::string {
                switch (ty) {
                case TT::KEYWORD:             return "keyword";
                case TT::IDENTIFIER:          return "identifier";
                case TT::NUMBER:              return "number";
                case TT::STRING:              return "string";
                case TT::LITERAL:             return "literal";
                case TT::OPERATOR:            return "operator";
                case TT::PUNCTUATION:         return "punctuation";
                case TT::DVAR:                return "dvar";
                case TT::URL:                 return "url";
                case TT::FSTRING_START:       return "fstring_start";
                case TT::STRING_PART:         return "string_part";
                case TT::INTERPOLATION_START: return "interp_start";
                case TT::INTERPOLATION_END:   return "interp_end";
                case TT::FSTRING_END:         return "fstring_end";
                case TT::EXEC_COMMENT:        return "exec_comment";
                case TT::RC:                  return "rc";
                default:                      return "other";
                }
            };
            tobj->obj->set("type",  nova_str(ttStr(t.type)));
            tobj->obj->set("value", nova_str(t.value));
            tobj->obj->set("line",  nova_num(t.line));
            tobj->obj->set("col",   nova_num(t.column));
            if (t.numval != 0.0)    tobj->obj->set("numval",     nova_num(t.numval));
            if (t.isUnary)          tobj->obj->set("isUnary",    nova_bool(true));
            if (t.isPostfix)        tobj->obj->set("isPostfix",  nova_bool(true));
            if (t.rightAssoc)       tobj->obj->set("rightAssoc", nova_bool(true));
            if (t.precedence != 0)  tobj->obj->set("precedence", nova_num(t.precedence));
            out.push_back(tobj);
        }
        return nova_arr(std::move(out));
    };

    // ── typeExpr → Val ─────────────────────────────────────────────────────────
    std::function<Val(const TypeExpr&)> teToVal = [&teToVal](const TypeExpr& t) -> Val {
        auto obj = nova_obj();
        auto kindStr = [](TypeExpr::Kind k) -> std::string {
            switch (k) {
            case TypeExpr::Kind::Named:    return "named";
            case TypeExpr::Kind::Union:    return "union";
            case TypeExpr::Kind::Intersect:return "intersect";
            case TypeExpr::Kind::Array:    return "array";
            case TypeExpr::Kind::Function: return "function";
            case TypeExpr::Kind::Shape:    return "shape";
            case TypeExpr::Kind::Value:    return "value";
            default: return "unknown";
            }
        };
        obj->obj->set("kind", nova_str(kindStr(t.kind)));
        if (!t.name.empty())         obj->obj->set("name",    nova_str(t.name));
        if (!t.literalValue.empty()) obj->obj->set("literal", nova_str(t.literalValue));
        if (t.of) obj->obj->set("of", teToVal(*t.of));
        if (!t.args.empty()) {
            ValVec args;
            for (auto& a : t.args) args.push_back(teToVal(a));
            obj->obj->set("args", nova_arr(std::move(args)));
        }
        if (!t.fields.empty()) {
            auto fobj = nova_obj();
            for (auto& [k, v] : t.fields) {
                auto fv = nova_obj();
                fv->obj->set("type",     teToVal(*v.type));
                fv->obj->set("optional", nova_bool(v.optional));
                fobj->obj->set(k, fv);
            }
            obj->obj->set("fields", fobj);
        }
        return obj;
    };
    // capture teToVal for lambdas below
    auto teToValShared = std::make_shared<std::function<Val(const TypeExpr&)>>(teToVal);

    // ── build the parser object ───────────────────────────────────────────────
    auto pobj = nova_obj();
    auto exe  = this;

    // .parse() → full program AST as Nova object// inside fn_parser, replace the .parse() registration with:
auto realAst = std::make_shared<std::shared_ptr<Node>>(nullptr);

pobj->obj->set("parse", NovaValue::makeNative([srcPtr, fnamePtr, nodeToVal, realAst](ValVec, auto) -> Val {
    try {
        Parser p(*srcPtr, *fnamePtr);
        auto ast = p.parse();
        *realAst = ast; // stash real AST
        return (*nodeToVal)(ast.get());
    } catch (std::exception& e) {
        throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
    }
}, "parse"));

pobj->obj->set("__getRealAst__", NovaValue::makeNative([realAst](ValVec, auto) -> Val {
    // returns a "ast handle" object wrapping the real Node*
    auto h = nova_obj();
    h->obj->set("__type__", nova_str("ast_handle"));
    h->obj->set("__ptr__", NovaValue::makeNative([realAst](ValVec, auto) -> Val {
        // encode the pointer as a number for retrieval — ugly but works
        // since ast handles are short-lived and never cross thread boundaries
        return nova_num((double)(uintptr_t)realAst->get());
    }, "__ptr__"));
    // store the shared_ptr itself to keep it alive
    auto keeper = *realAst;
    h->obj->set("__keep__", NovaValue::makeNative([keeper](ValVec, auto) -> Val {
        return nova_null(); // just holds keeper alive
    }, "__keep__"));
    return h;
}, "__getRealAst__"));

    // .statement() → parse exactly one statement from current source
    pobj->obj->set("statement", NovaValue::makeNative([srcPtr, fnamePtr, nodeToVal](ValVec, auto) -> Val {
        try {
            Parser p(*srcPtr, *fnamePtr);
            auto node = p.statement();
            return (*nodeToVal)(node.get());
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
        }
    }, "statement"));

    // .expression(minPrec?) → parse exactly one expression
    pobj->obj->set("expression", NovaValue::makeNative([srcPtr, fnamePtr, nodeToVal](ValVec a, auto) -> Val {
        try {
            Parser p(*srcPtr, *fnamePtr);
            int minPrec = a.empty() ? 0 : (int)a[0].asNumber();
            auto node = p.expression(minPrec);
            return (*nodeToVal)(node.get());
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
        }
    }, "expression"));

    // .parseExpr() → parse + EVALUATE one expression (same as old fn_expreval but scoped)
    pobj->obj->set("parseExpr", NovaValue::makeNative([srcPtr, fnamePtr, exe](ValVec, auto) -> Val {
        try {
            Parser p(*srcPtr, *fnamePtr);
            auto ast = p.parse();
            if (!ast || ast->body.empty()) return nova_null();
            const Node* n = ast->body[0].get();
            if (n && n->kind == Node::Kind::Exec && n->value)
                return exe->evaluate(n->value.get(), exe->globalScope);
            return exe->evaluate(n, exe->globalScope);
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
        }
    }, "parseExpr"));

    // .eval() → parse + execute all statements
    pobj->obj->set("eval", NovaValue::makeNative([srcPtr, fnamePtr, exe](ValVec, auto cs) -> Val {
        try {
            Parser p(*srcPtr, *fnamePtr);
            auto ast = p.parse();
            Executor evalExe(*srcPtr, *fnamePtr);
            evalExe.globalScope = exe->globalScope;
            return evalExe.run(ast.get(), cs);
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Eval error: ") + e.what())};
        }
    }, "eval"));

    // .tokens() → lex source and return token array
    pobj->obj->set("tokens", NovaValue::makeNative([srcPtr, fnamePtr, tokensOf](ValVec, auto) -> Val {
        return tokensOf(*srcPtr, *fnamePtr);
    }, "tokens"));

    // .parseType(src?) → parse a TypeExpr, return as object
    pobj->obj->set("parseType", NovaValue::makeNative([srcPtr, fnamePtr, teToValShared](ValVec a, auto) -> Val {
        std::string tsrc = a.empty() ? *srcPtr : a[0].asString();
        std::string tf   = a.empty() ? *fnamePtr : *fnamePtr;
        try {
            Parser p(tsrc, tf);
            TypeExpr te = p.parseTypeExpr();
            return (*teToValShared)(te);
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Type parse error: ") + e.what())};
        }
    }, "parseType"));

    // .parseTypeAtom(src?) → parse a single TypeAtom (no union/intersection)
    pobj->obj->set("parseTypeAtom", NovaValue::makeNative([srcPtr, fnamePtr, teToValShared](ValVec a, auto) -> Val {
        std::string tsrc = a.empty() ? *srcPtr : a[0].asString();
        try {
            Parser p(tsrc, *fnamePtr);
            TypeExpr te = p.parseTypeAtom();
            return (*teToValShared)(te);
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Type parse error: ") + e.what())};
        }
    }, "parseTypeAtom"));

    // .parseTypeParams(src?) → parse <T, U, ...>, return array of name strings
    pobj->obj->set("parseTypeParams", NovaValue::makeNative([srcPtr, fnamePtr](ValVec a, auto) -> Val {
        std::string tsrc = a.empty() ? *srcPtr : a[0].asString();
        try {
            Parser p(tsrc, *fnamePtr);
            auto params = p.parseTypeParams();
            ValVec arr;
            for (auto& s : params) arr.push_back(nova_str(s));
            return nova_arr(std::move(arr));
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
        }
    }, "parseTypeParams"));

    // .objectPattern(src?) → parse {key: alias = default, ...}, return props array
    pobj->obj->set("objectPattern", NovaValue::makeNative([srcPtr, fnamePtr, nodeToVal](ValVec a, auto) -> Val {
        std::string tsrc = a.empty() ? *srcPtr : a[0].asString();
        try {
            Parser p(tsrc, *fnamePtr);
            ObjPattern pat = p.objectPattern();
            ValVec arr;
            for (auto& prop : pat.props) {
                auto pobj = nova_obj();
                pobj->obj->set("key",   nova_str(prop.key));
                pobj->obj->set("alias", nova_str(prop.alias));
                if (prop.defaultValue)
                    pobj->obj->set("default", (*nodeToVal)(prop.defaultValue.get()));
                arr.push_back(pobj);
            }
            return nova_arr(std::move(arr));
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
        }
    }, "objectPattern"));

    // .arrayPattern(src?) → parse [a, b = default, ...rest], return elements array
    pobj->obj->set("arrayPattern", NovaValue::makeNative([srcPtr, fnamePtr, nodeToVal](ValVec a, auto) -> Val {
        std::string tsrc = a.empty() ? *srcPtr : a[0].asString();
        try {
            Parser p(tsrc, *fnamePtr);
            ArrPattern pat = p.arrayPattern();
            ValVec arr;
            for (auto& elem : pat.elements) {
                auto eobj = nova_obj();
                eobj->obj->set("name", nova_str(elem.name));
                if (elem.rest) eobj->obj->set("rest", nova_bool(true));
                if (elem.defaultValue)
                    eobj->obj->set("default", (*nodeToVal)(elem.defaultValue.get()));
                arr.push_back(eobj);
            }
            return nova_arr(std::move(arr));
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
        }
    }, "arrayPattern"));

    // .parseFuncBody(src?, isArrow?) → parse (args) [=>] body
    //   returns { args: [{name, rest, default, typeName}], body: Node[] }
    pobj->obj->set("parseFuncBody", NovaValue::makeNative([srcPtr, fnamePtr, nodeToVal](ValVec a, auto) -> Val {
        std::string tsrc  = a.empty()      ? *srcPtr : a[0].asString();
        bool isArrow      = a.size() > 1   ? a[1].asBool() : false;
        try {
            Parser p(tsrc, *fnamePtr);
            auto fb = p.parseFuncBody(isArrow);
            auto result = nova_obj();
            ValVec args;
            for (auto& fa : fb.args) {
                auto fobj = nova_obj();
                fobj->obj->set("name", nova_str(fa.name));
                if (fa.rest) fobj->obj->set("rest", nova_bool(true));
                if (fa.type) fobj->obj->set("typeName", nova_str(fa.type->name));
                if (fa.defaultValue)
                    fobj->obj->set("default", (*nodeToVal)(fa.defaultValue.get()));
                args.push_back(fobj);
            }
            result->obj->set("args", nova_arr(std::move(args)));
            ValVec body;
            for (auto& s : fb.body) body.push_back((*nodeToVal)(s.get()));
            result->obj->set("body", nova_arr(std::move(body)));
            return result;
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
        }
    }, "parseFuncBody"));

    // .blockBody(src?) → parse a single statement as a block body, return Node[]
    pobj->obj->set("blockBody", NovaValue::makeNative([srcPtr, fnamePtr, nodeToVal](ValVec a, auto) -> Val {
        std::string tsrc = a.empty() ? *srcPtr : a[0].asString();
        try {
            Parser p(tsrc, *fnamePtr);
            auto body = p.blockBody();
            ValVec arr;
            for (auto& s : body) arr.push_back((*nodeToVal)(s.get()));
            return nova_arr(std::move(arr));
        } catch (std::exception& e) {
            throw ThrowSignal{nova_str(std::string("Parser error: ") + e.what())};
        }
    }, "blockBody"));

    // .reset(newSrc, newFilename?) → update source, return self
    pobj->obj->set("reset", NovaValue::makeNative([srcPtr, fnamePtr, pobj](ValVec a, auto) -> Val {
        if (!a.empty())   *srcPtr   = a[0].asString();
        if (a.size() > 1) *fnamePtr = a[1].asString();
        return pobj;
    }, "reset"));

    // .source → current source string
    pobj->obj->set("source", NovaValue::makeNative([srcPtr](ValVec, auto) -> Val {
        return nova_str(*srcPtr);
    }, "source"));

    // .filename → current filename string
    pobj->obj->set("filename", NovaValue::makeNative([fnamePtr](ValVec, auto) -> Val {
        return nova_str(*fnamePtr);
    }, "filename"));

    // .atEnd() → whether the next token in a fresh parse would be EOF
    pobj->obj->set("atEnd", NovaValue::makeNative([srcPtr, fnamePtr](ValVec, auto) -> Val {
        try {
            Parser p(*srcPtr, *fnamePtr);
            return nova_bool(p.atEnd());
        } catch (...) {
            return nova_bool(true);
        }
    }, "atEnd"));

    // .check(type, value?) → whether the first token matches
    pobj->obj->set("check", NovaValue::makeNative([srcPtr, fnamePtr](ValVec a, auto) -> Val {
        if (a.empty()) return nova_bool(false);
        std::string typeStr  = a[0].asString();
        std::string valueStr = a.size() > 1 ? a[1].asString() : "";
        auto strToTT = [](const std::string& s) -> TT {
            if (s == "keyword")     return TT::KEYWORD;
            if (s == "identifier")  return TT::IDENTIFIER;
            if (s == "number")      return TT::NUMBER;
            if (s == "string")      return TT::STRING;
            if (s == "literal")     return TT::LITERAL;
            if (s == "operator")    return TT::OPERATOR;
            if (s == "punctuation") return TT::PUNCTUATION;
            if (s == "dvar")        return TT::DVAR;
            if (s == "url")         return TT::URL;
            if (s == "rc")          return TT::RC;
            return TT::IDENTIFIER;
        };
        try {
            Parser p(*srcPtr, *fnamePtr);
            return nova_bool(p.check(strToTT(typeStr), valueStr));
        } catch (...) {
            return nova_bool(false);
        }
    }, "check"));

    return pobj; }, "parser");
        std_obj->obj->set("parser", fn_parser);

        // ── asteval ───────────────────────────────────────────────────────────────────
        // Takes the object returned by Std.parser(...).parse() — which has __getRealAst__
        // on the PARSER object, not the AST object — so asteval takes the parser itself.
        // Signature: Std.asteval(parserObj, scope?) runs the stashed real AST.
        auto fn_asteval = NovaValue::makeNative([this](ValVec args, std::shared_ptr<Scope> cs) -> Val
                                                {
    if (args.empty() || !args[0].isObject())
        _error("Std.asteval: expected a parser object as first argument");

    Val getRealAstFn = args[0]->obj->get("__getRealAst__");
    if (!getRealAstFn || !getRealAstFn.isFunction())
        _error("Std.asteval: argument is not a parser object with a parsed AST (call .parse() first)");

    Val handle = callFunction(getRealAstFn, {}, cs);
    if (!handle || !handle.isObject())
        _error("Std.asteval: no AST available — call parser.parse() first");

    Val ptrFn = handle->obj->get("__ptr__");
    if (!ptrFn || !ptrFn.isFunction())
        _error("Std.asteval: malformed AST handle");

    Val ptrNum = callFunction(ptrFn, {}, cs);
    Node* ast = reinterpret_cast<Node*>((uintptr_t)ptrNum.asNumber());
    if (!ast)
        _error("Std.asteval: null AST pointer");

    // run in current scope by default, or caller-supplied scope
    auto runScope = cs;
    return run(ast, runScope); }, "asteval");
        std_obj->obj->set("asteval", fn_asteval);
        // ── executor ──────────────────────────────────────────────────────────────────
        auto fn_executor = NovaValue::makeNative([this](ValVec args, std::shared_ptr<Scope> cs) -> Val
                                                 {
    // Creates a Nova object wrapping a new Executor (or `this` if no src given).
    // Exposes every public method and field of Executor.
    std::string src   = args.empty()      ? source   : args[0].asString();
    std::string fname = args.size() > 1   ? args[1].asString() : filename;

    auto exe = std::make_shared<Executor>(src, fname);
    exe->globalScope = globalScope; // share global scope by default

    auto eobj = nova_obj();

    // ── identity / source ─────────────────────────────────────────────────────
    eobj->obj->set("source",   nova_str(src));
    eobj->obj->set("filename", nova_str(fname));

    // ── run(ast?) ─────────────────────────────────────────────────────────────
    eobj->obj->set("run", NovaValue::makeNative([exe](ValVec a, std::shared_ptr<Scope> sc) -> Val {
        if (a.empty()) {
            // re-parse own source and run
            Parser p(exe->source, exe->filename);
            auto ast = p.parse();
            return exe->run(ast.get(), exe->globalScope);
        }
        // accept parser object with stashed AST
        if (a[0].isObject()) {
            Val getRealAstFn = a[0]->obj->get("__getRealAst__");
            if (getRealAstFn && getRealAstFn.isFunction()) {
                Val handle = exe->callFunction(getRealAstFn, {}, sc);
                if (handle && handle.isObject()) {
                    Val ptrFn = handle->obj->get("__ptr__");
                    if (ptrFn && ptrFn.isFunction()) {
                        Val ptrNum = exe->callFunction(ptrFn, {}, sc);
                        Node* ast = reinterpret_cast<Node*>((uintptr_t)ptrNum.asNumber());
                        if (ast) return exe->run(ast, sc);
                    }
                }
            }
        }
        return nova_null();
    }, "run"));

    // ── execute(node via parser, scope?) ──────────────────────────────────────
    eobj->obj->set("execute", NovaValue::makeNative([exe](ValVec a, std::shared_ptr<Scope> sc) -> Val {
        if (a.empty() || !a[0].isObject()) return nova_null();
        Val getRealAstFn = a[0]->obj->get("__getRealAst__");
        if (!getRealAstFn || !getRealAstFn.isFunction()) return nova_null();
        Val handle = exe->callFunction(getRealAstFn, {}, sc);
        if (!handle || !handle.isObject()) return nova_null();
        Val ptrFn = handle->obj->get("__ptr__");
        if (!ptrFn || !ptrFn.isFunction()) return nova_null();
        Val ptrNum = exe->callFunction(ptrFn, {}, sc);
        Node* node = reinterpret_cast<Node*>((uintptr_t)ptrNum.asNumber());
        if (!node) return nova_null();
        auto runScope = a.size() > 1 && a[1].isObject() ? sc : exe->globalScope;
        return exe->execute(node, runScope).value;
    }, "execute"));

    // ── evaluate(src) ─────────────────────────────────────────────────────────
    eobj->obj->set("evaluate", NovaValue::makeNative([exe](ValVec a, std::shared_ptr<Scope> sc) -> Val {
        if (a.empty()) return nova_null();
        std::string exprSrc = a[0].asString();
        Parser p(exprSrc, exe->filename);
        auto ast = p.parse();
        if (!ast || ast->body.empty()) return nova_null();
        const Node* n = ast->body[0].get();
        if (n && n->kind == Node::Kind::Exec && n->value)
            return exe->evaluate(n->value.get(), exe->globalScope);
        return exe->evaluate(n, exe->globalScope);
    }, "evaluate"));

    // ── callFunction(fn, args[]) ──────────────────────────────────────────────
    eobj->obj->set("callFunction", NovaValue::makeNative([exe](ValVec a, std::shared_ptr<Scope> sc) -> Val {
        if (a.size() < 1 || !a[0].isFunction()) return nova_null();
        ValVec callArgs;
        if (a.size() > 1 && a[1].isArray())
            callArgs = a[1]->arr->inner;
        return exe->callFunction(a[0], callArgs, sc);
    }, "callFunction"));

    // ── globalScope access ────────────────────────────────────────────────────
    eobj->obj->set("getGlobal", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        Val v = exe->globalScope->get(a[0].asString());
        return v ? v : nova_null();
    }, "getGlobal"));

    eobj->obj->set("setGlobal", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_null();
        exe->globalScope->setOwn(a[0].asString(), a[1]);
        return a[1];
    }, "setGlobal"));

    eobj->obj->set("hasGlobal", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_bool(false);
        Val v = exe->globalScope->get(a[0].asString());
        return nova_bool(v != nullptr);
    }, "hasGlobal"));

    eobj->obj->set("delGlobal", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_bool(false);
        exe->globalScope->del(a[0].asString());
        return nova_bool(true);
    }, "delGlobal"));

    eobj->obj->set("snapshotGlobals", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        auto snap = exe->globalScope->snapshot();
        auto obj = nova_obj();
        for (auto& [k, v] : snap)
            obj->obj->set(k, v);
        return obj;
    }, "snapshotGlobals"));

    // ── scope factory ─────────────────────────────────────────────────────────
    eobj->obj->set("newScope", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        Scope::Kind kind = Scope::Kind::Block;
        if (!a.empty()) {
            std::string k = a[0].asString();
            if      (k == "function")  kind = Scope::Kind::Function;
            else if (k == "module")    kind = Scope::Kind::Module;
            else if (k == "namespace") kind = Scope::Kind::Namespace;
            else if (k == "catch")     kind = Scope::Kind::Catch;
            else if (k == "with")      kind = Scope::Kind::With;
        }
        auto sc = std::make_shared<Scope>(kind, exe->globalScope, exe->globalScope);
        // wrap scope as a Nova object with get/set/del/snapshot
        auto sobj = nova_obj();
        sobj->obj->set("get", NovaValue::makeNative([sc](ValVec a, auto) -> Val {
            if (a.empty()) return nova_null();
            Val v = sc->get(a[0].asString());
            return v ? v : nova_null();
        }, "get"));
        sobj->obj->set("set", NovaValue::makeNative([sc](ValVec a, auto) -> Val {
            if (a.size() < 2) return nova_null();
            sc->setOwn(a[0].asString(), a[1]);
            return a[1];
        }, "set"));
        sobj->obj->set("del", NovaValue::makeNative([sc](ValVec a, auto) -> Val {
            if (a.empty()) return nova_bool(false);
            sc->del(a[0].asString());
            return nova_bool(true);
        }, "del"));
        sobj->obj->set("snapshot", NovaValue::makeNative([sc](ValVec, auto) -> Val {
            auto snap = sc->snapshot();
            auto obj = nova_obj();
            for (auto& [k, v] : snap) obj->obj->set(k, v);
            return obj;
        }, "snapshot"));
        sobj->obj->set("child", NovaValue::makeNative([exe, sc](ValVec a, auto) -> Val {
            // returns a new scope object whose parent is this scope
            Scope::Kind ck = Scope::Kind::Block;
            if (!a.empty()) {
                std::string k = a[0].asString();
                if      (k == "function")  ck = Scope::Kind::Function;
                else if (k == "module")    ck = Scope::Kind::Module;
                else if (k == "catch")     ck = Scope::Kind::Catch;
            }
            auto child = sc->child(ck);
            auto cobj = nova_obj();
            cobj->obj->set("get", NovaValue::makeNative([child](ValVec a, auto) -> Val {
                if (a.empty()) return nova_null();
                Val v = child->get(a[0].asString());
                return v ? v : nova_null();
            }, "get"));
            cobj->obj->set("set", NovaValue::makeNative([child](ValVec a, auto) -> Val {
                if (a.size() < 2) return nova_null();
                child->setOwn(a[0].asString(), a[1]);
                return a[1];
            }, "set"));
            cobj->obj->set("snapshot", NovaValue::makeNative([child](ValVec, auto) -> Val {
                auto snap = child->snapshot();
                auto obj = nova_obj();
                for (auto& [k, v] : snap) obj->obj->set(k, v);
                return obj;
            }, "snapshot"));
            return cobj;
        }, "child"));
        return sobj;
    }, "newScope"));

    // ── module cache ──────────────────────────────────────────────────────────
    eobj->obj->set("getModuleCache", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        auto obj = nova_obj();
        for (auto& [k, v] : exe->moduleCache)
            obj->obj->set(k, v);
        return obj;
    }, "getModuleCache"));

    eobj->obj->set("setModuleCache", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_null();
        exe->moduleCache[a[0].asString()] = a[1];
        return a[1];
    }, "setModuleCache"));

    eobj->obj->set("clearModuleCache", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        exe->moduleCache.clear();
        return nova_null();
    }, "clearModuleCache"));

    // ── options (with option FLAG {}) ─────────────────────────────────────────
    eobj->obj->set("getOption", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        auto it = exe->options.find(a[0].asString());
        return it != exe->options.end() ? it->second : nova_null();
    }, "getOption"));

    eobj->obj->set("setOption", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_null();
        exe->options[a[0].asString()] = a[1];
        return a[1];
    }, "setOption"));

    eobj->obj->set("getOptions", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        auto obj = nova_obj();
        for (auto& [k, v] : exe->options)
            obj->obj->set(k, v);
        return obj;
    }, "getOptions"));

    // ── event listeners ───────────────────────────────────────────────────────
    eobj->obj->set("getListeners", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        std::string evName = a.empty() ? "" : a[0].asString();
        if (evName.empty()) {
            auto obj = nova_obj();
            for (auto& [k, listeners] : exe->eventListeners)
                obj->obj->set(k, nova_num((double)listeners.size()));
            return obj;
        }
        auto it = exe->eventListeners.find(evName);
        return nova_num(it != exe->eventListeners.end() ? (double)it->second.size() : 0);
    }, "getListeners"));

    eobj->obj->set("clearListeners", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) exe->eventListeners.clear();
        else exe->eventListeners.erase(a[0].asString());
        return nova_null();
    }, "clearListeners"));

    // ── perform handlers (algebraic effects) ──────────────────────────────────
    eobj->obj->set("getPerformHandler", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        auto it = exe->_performHandlers.find(a[0].asString());
        return it != exe->_performHandlers.end() ? it->second : nova_null();
    }, "getPerformHandler"));

    eobj->obj->set("setPerformHandler", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.size() < 2 || !a[1].isFunction()) return nova_null();
        exe->_performHandlers[a[0].asString()] = a[1];
        return a[1];
    }, "setPerformHandler"));

    eobj->obj->set("clearPerformHandlers", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        exe->_performHandlers.clear();
        return nova_null();
    }, "clearPerformHandlers"));

    // ── class registry ────────────────────────────────────────────────────────
    eobj->obj->set("getClass", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        auto it = exe->classRegistry.find(a[0].asString());
        return it != exe->classRegistry.end() ? it->second : nova_null();
    }, "getClass"));

    eobj->obj->set("registerClass", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_null();
        exe->classRegistry[a[0].asString()] = a[1];
        return a[1];
    }, "registerClass"));

    eobj->obj->set("getClassRegistry", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        auto obj = nova_obj();
        for (auto& [k, v] : exe->classRegistry)
            obj->obj->set(k, v);
        return obj;
    }, "getClassRegistry"));

    // ── namespaces ────────────────────────────────────────────────────────────
    eobj->obj->set("getNamespace", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        auto it = exe->namespaces.find(a[0].asString());
        if (it == exe->namespaces.end()) return nova_null();
        auto obj = nova_obj();
        for (auto& [k, desc] : it->second->variables) {
            Val v = (desc.hasHooks && desc.read) ? desc.read() : desc.raw;
            obj->obj->set(k, v);
        }
        return obj;
    }, "getNamespace"));

    eobj->obj->set("listNamespaces", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        ValVec out;
        for (auto& [k, _] : exe->namespaces)
            out.push_back(nova_str(k));
        return nova_arr(std::move(out));
    }, "listNamespaces"));

    // ── type system ───────────────────────────────────────────────────────────
    eobj->obj->set("typeOf", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_str("null");
        return nova_str(exe->_typeOf(a[0]));
    }, "typeOf"));

    eobj->obj->set("checkType", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_bool(false);
        // a[1] should be a type name string; build a simple Named TypeExpr
        TypeExpr te;
        te.kind = TypeExpr::Kind::Named;
        te.name = a[1].asString();
        return nova_bool(exe->types.check(a[0], te));
    }, "checkType"));

    eobj->obj->set("satisfiesInterface", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_bool(false);
        return nova_bool(exe->types.satisfies(a[0], a[1].asString(), *exe));
    }, "satisfiesInterface"));

    // ── stringify ─────────────────────────────────────────────────────────────
    eobj->obj->set("stringify", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_str("null");
        int indent = a.size() > 1 ? (int)a[1].asNumber() : 0;
        return nova_str(exe->stringify(a[0], indent));
    }, "stringify"));

    // ── isTruthy ──────────────────────────────────────────────────────────────
    eobj->obj->set("isTruthy", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        return nova_bool(!a.empty() && exe->_isTruthy(a[0]));
    }, "isTruthy"));

    // ── current position ──────────────────────────────────────────────────────
    eobj->obj->set("currentLine", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        return nova_num(exe->_currentLine);
    }, "currentLine"));

    eobj->obj->set("currentCol", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        return nova_num(exe->_currentCol);
    }, "currentCol"));

    // ── loadModule (force-load a file as a module) ────────────────────────────
    eobj->obj->set("loadModule", NovaValue::makeNative([exe](ValVec a, std::shared_ptr<Scope> sc) -> Val {
        if (a.empty()) return nova_null();
        return exe->_loadModule(a[0].asString(), sc);
    }, "loadModule"));

    // ── syncFetch ─────────────────────────────────────────────────────────────
    eobj->obj->set("syncFetch", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        Val opts = a.size() > 1 ? a[1] : nullptr;
        return exe->_syncFetch(a[0].asString(), opts);
    }, "syncFetch"));

    // ── native type helpers ───────────────────────────────────────────────────
    eobj->obj->set("toNative", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        std::string t = a.size() > 1 ? a[1].asString() : "";
        return exe->_toNative(a[0], t);
    }, "toNative"));

    eobj->obj->set("fromNative", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        return a.empty() ? nova_null() : exe->_fromNative(a[0]);
    }, "fromNative"));

    eobj->obj->set("toRawPtr", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        std::string t = a.size() > 1 ? a[1].asString() : "";
        return exe->_toRawPtr(a[0], t);
    }, "toRawPtr"));

    // ── binary op (direct _novaBinaryOp) ─────────────────────────────────────
    eobj->obj->set("binaryOp", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.size() < 3) return nova_null();
        return exe->_novaBinaryOp(a[0].asString(), a[1], a[2], nullptr);
    }, "binaryOp"));

    // ── instantiateClass ─────────────────────────────────────────────────────
    eobj->obj->set("instantiateClass", NovaValue::makeNative([exe](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        ValVec args;
        if (a.size() > 1 && a[1].isArray()) args = a[1]->arr->inner;
        return exe->_instantiateClass(a[0], args);
    }, "instantiateClass"));

    // ── shareGlobalScope / isolateGlobalScope ─────────────────────────────────
    eobj->obj->set("shareGlobalScope", NovaValue::makeNative([exe, this](ValVec, auto) -> Val {
        exe->globalScope = globalScope;
        return nova_null();
    }, "shareGlobalScope"));

    eobj->obj->set("isolateGlobalScope", NovaValue::makeNative([exe](ValVec, auto) -> Val {
        auto iso = std::make_shared<Scope>(Scope::Kind::Global, nullptr, nullptr);
        iso->globalScope = iso;
        exe->globalScope = iso;
        exe->_setupGlobalScope(); // re-init builtins on isolated scope
        return nova_null();
    }, "isolateGlobalScope"));

    // ── self-reference ────────────────────────────────────────────────────────
    // expose `this` executor (the outer one) as a nested executor object too
    eobj->obj->set("parent", NovaValue::makeNative([this](ValVec, std::shared_ptr<Scope> cs) -> Val {
        // just return Std.executor() of the parent — but avoid infinite recursion
        // by returning a minimal proxy instead
        auto proxy = nova_obj();
        proxy->obj->set("source",   nova_str(source));
        proxy->obj->set("filename", nova_str(filename));
        proxy->obj->set("getGlobal", NovaValue::makeNative([this](ValVec a, auto) -> Val {
            if (a.empty()) return nova_null();
            Val v = globalScope->get(a[0].asString());
            return v ? v : nova_null();
        }, "getGlobal"));
        proxy->obj->set("setGlobal", NovaValue::makeNative([this](ValVec a, auto) -> Val {
            if (a.size() < 2) return nova_null();
            globalScope->setOwn(a[0].asString(), a[1]);
            return a[1];
        }, "setGlobal"));
        return proxy;
    }, "parent"));

    return eobj; }, "executor");
        std_obj->obj->set("executor", fn_executor);
        // ── srand ────────────────────────────────────────────────────────────────
        auto fn_srand = NovaValue::makeNative([](ValVec a, auto)
                                              { std::srand(int(a[0].asNumber())); return nova_null(); }, "srand");
        std_obj->obj->set("srand", fn_srand);

        // ── type conversion ───────────────────────────────────────────────────────
        auto fn_number = NovaValue::makeNative([](ValVec a, auto)
                                               { return a.empty() ? nova_num(0) : nova_num(a[0].asNumber()); }, "number");
        auto fn_string = NovaValue::makeNative([this](ValVec a, auto)
                                               { return a.empty() ? nova_str("") : nova_str(stringify(a[0])); }, "string");
        auto fn_bool = NovaValue::makeNative([](ValVec a, auto)
                                             { return a.empty() ? nova_bool(false) : nova_bool(a[0].asBool()); }, "bool");
        auto fn_int = NovaValue::makeNative([](ValVec a, auto)
                                            { return a.empty() ? nova_num(0) : nova_num(std::floor(a[0].asNumber())); }, "int");
        auto fn_float = NovaValue::makeNative([](ValVec a, auto)
                                              { return a.empty() ? nova_num(0) : nova_num(a[0].asNumber()); }, "float");
        std_obj->obj->set("number", fn_number);
        std_obj->obj->set("string", fn_string);
        std_obj->obj->set("bool", fn_bool);
        std_obj->obj->set("int", fn_int);
        std_obj->obj->set("float", fn_float);

        // ── typeof / null ─────────────────────────────────────────────────────────
        auto fn_typeof = NovaValue::makeNative([this](ValVec a, auto)
                                               { return a.empty() ? nova_str("null") : nova_str(_typeOf(a[0])); }, "typeof");
        std_obj->obj->set("typeof", fn_typeof);
        std_obj->obj->set("typeOf", fn_typeof);
        std_obj->obj->set("null", nova_null());

        // ── isNull / isDefined / isEmpty ──────────────────────────────────────────
        auto fn_isNull = NovaValue::makeNative([](ValVec a, auto)
                                               { return nova_bool(!a.empty() && a[0].isNull()); }, "isNull");
        auto fn_isDefined = NovaValue::makeNative([](ValVec a, auto)
                                                  { return nova_bool(!a.empty() && !a[0].isNull()); }, "isDefined");
        auto fn_isEmpty = NovaValue::makeNative([this](ValVec a, auto) -> Val
                                                {
            if (a.empty() || a[0].isNull()) return nova_bool(true);
            if (a[0].isString()) return nova_bool(a[0]->sval.empty());
            if (a[0].isArray())  return nova_bool(a[0]->arr->inner.empty());
            if (a[0].isObject()) return nova_bool(a[0]->obj->inner.empty());
            return nova_bool(false); }, "isEmpty");
        std_obj->obj->set("isNull", fn_isNull);
        std_obj->obj->set("isDefined", fn_isDefined);
        std_obj->obj->set("isEmpty", fn_isEmpty);

        // ── assert ────────────────────────────────────────────────────────────────
        auto fn_assert = NovaValue::makeNative([this](ValVec a, auto) -> Val
                                               {
            if (a.empty() || !a[0].asBool()) {
                std::string msg = a.size() > 1 ? a[1].asString() : "Assertion failed";
                _error(msg);
            }
            return nova_null(); }, "assert");
        std_obj->obj->set("assert", fn_assert);

        // ── error / throw helper ──────────────────────────────────────────────────
        auto fn_error = NovaValue::makeNative([](ValVec a, auto) -> Val
                                              { throw ThrowSignal{a.empty() ? nova_str("Error") : a[0]}; return nova_null(); }, "error");
        std_obj->obj->set("error", fn_error);

        // ── range ─────────────────────────────────────────────────────────────────
        auto fn_range = NovaValue::makeNative([](ValVec a, auto) -> Val
                                              {
            double start = 0, end = 0, step = 1;
            if (a.size() >= 2) { start = a[0].asNumber(); end = a[1].asNumber(); }
            else if (a.size() == 1) { end = a[0].asNumber(); }
            if (a.size() >= 3) step = a[2].asNumber();
            return NovaValue::makeRange(start, end, step); }, "range");
        std_obj->obj->set("range", fn_range);

        // ── Array helpers ─────────────────────────────────────────────────────────
        auto arrayObj = nova_obj();
        arrayObj->obj->set("from", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                         {
            if (a.empty()) return nova_arr();
            if (a[0].isArray()) return a[0];
            if (a[0].isRange()) return nova_arr(a[0]->range->toArray());
            if (a[0].isString()) { ValVec out; for (auto &cp : utf8::codepoints(a[0]->sval)) out.push_back(nova_str(cp)); return nova_arr(out); }
            return nova_arr({a[0]}); }, "from"));
        arrayObj->obj->set("isArray", NovaValue::makeNative([](ValVec a, auto)
                                                            { return nova_bool(!a.empty() && a[0].isArray()); }, "isArray"));
        arrayObj->obj->set("of", NovaValue::makeNative([](ValVec a, auto)
                                                       { return nova_arr(a); }, "of"));
        std_obj->obj->set("Array", arrayObj);

        // ── Object helpers ────────────────────────────────────────────────────────
        auto objectObj = nova_obj();
        objectObj->obj->set("keys", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.empty()) return nova_arr();
            if (a[0].isObject()) { ValVec out; for (auto& k : a[0]->obj->keys()) out.push_back(nova_str(k)); return nova_arr(out); }
            if (a[0].isStruct()) { ValVec out; for (auto& [k,_] : a[0]->strct->inner) out.push_back(nova_str(k)); return nova_arr(out); }
            return nova_arr(); }, "keys"));
        objectObj->obj->set("values", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                            {
            if (a.empty()) return nova_arr();
            if (a[0].isObject()) return nova_arr(a[0]->obj->values());
            return nova_arr(); }, "values"));
        objectObj->obj->set("entries", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
            if (a.empty()) return nova_arr();
            ValVec out;
            if (a[0].isObject()) for (auto& [k,v] : a[0]->obj->inner) out.push_back(nova_arr({nova_str(k),v}));
            return nova_arr(out); }, "entries"));
        objectObj->obj->set("assign", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                            {
            if (a.size() < 2) return a.empty() ? nova_obj() : a[0];
            if (a[0].isObject() && a[1].isObject())
                for (auto& [k,v] : a[1]->obj->inner) a[0]->obj->set(k,v);
            return a[0]; }, "assign"));
        objectObj->obj->set("fromEntries", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
            if (a.empty() || !a[0].isArray()) return nova_obj();
            auto obj = nova_obj();
            for (auto& e : a[0]->arr->inner)
                if (e.isArray() && e->arr->length() >= 2) obj->obj->set(e->arr->get(0).asString(), e->arr->get(1));
            return obj; }, "fromEntries"));
        std_obj->obj->set("Object", objectObj);

        // ── String ───────────────────────────────────────────────────────────────
        auto stringObj = nova_obj();
        stringObj->obj->set("fromCharCode", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
            std::string out;
            for (auto& v : a) {
                uint32_t code = (uint32_t)(int)v.asNumber();
                if (code < 0x80)       { out += (char)code; }
                else if (code < 0x800) { out += (char)(0xC0|(code>>6)); out += (char)(0x80|(code&0x3F)); }
                else if (code < 0x10000) { out += (char)(0xE0|(code>>12)); out += (char)(0x80|((code>>6)&0x3F)); out += (char)(0x80|(code&0x3F)); }
                else { out += (char)(0xF0|(code>>18)); out += (char)(0x80|((code>>12)&0x3F)); out += (char)(0x80|((code>>6)&0x3F)); out += (char)(0x80|(code&0x3F)); }
            }
            return nova_str(out); }, "fromCharCode"));
        stringObj->obj->set("fromBytes", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               { return nova_str(BytesToChar(a)); }, "fromBytes"));
        std_obj->obj->set("String", stringObj);

        // ── Number ────────────────────────────────────────────────────────────────
        auto numObj = nova_obj();
        numObj->obj->set("isNaN", NovaValue::makeNative([](ValVec a, auto)
                                                        { return nova_bool(!a.empty() && std::isnan(a[0].asNumber())); }, "isNaN"));
        numObj->obj->set("isFinite", NovaValue::makeNative([](ValVec a, auto)
                                                           { return nova_bool(!a.empty() && std::isfinite(a[0].asNumber())); }, "isFinite"));
        numObj->obj->set("isInteger", NovaValue::makeNative([](ValVec a, auto)
                                                            { return nova_bool(!a.empty() && a[0].isNumber() && a[0]->nval == std::floor(a[0]->nval)); }, "isInteger"));
        numObj->obj->set("parseInt", NovaValue::makeNative([](ValVec a, auto)
                                                           { if(a.empty())return nova_num(0); try{return nova_num(std::stod(a[0].asString()));}catch(...){return nova_num(std::numeric_limits<double>::quiet_NaN());} }, "parseInt"));
        numObj->obj->set("parseFloat", NovaValue::makeNative([](ValVec a, auto)
                                                             { if(a.empty())return nova_num(0); try{return nova_num(std::stod(a[0].asString()));}catch(...){return nova_num(std::numeric_limits<double>::quiet_NaN());} }, "parseFloat"));
        numObj->obj->set("MAX_VALUE", nova_num(std::numeric_limits<double>::max()));
        numObj->obj->set("MIN_VALUE", nova_num(std::numeric_limits<double>::min()));
        numObj->obj->set("INFINITY", nova_num(std::numeric_limits<double>::infinity()));
        numObj->obj->set("NaN", nova_num(std::numeric_limits<double>::quiet_NaN()));
        std_obj->obj->set("Number", numObj);

        std_obj->obj->set("Inspect", NovaValue::makeNative([this](ValVec a, auto) -> Val
                                                           {
    if (a.empty())
        return nova_obj({{"kind", nova_str("null")}});

    Val v = a[0];
    auto out = nova_obj();
    out->obj->set("kind", nova_str(_typeOf(v)));

    // refCount reflects the underlying NovaValue's shared_ptr use_count,
    // available via TVal::asShared() — note this counts the temporary
    // shared_ptr created by asShared() too for primitive (boxed) tags,
    // so treat refCount as approximate for Null/Bool/Number.
    auto shared = v.asShared();
    out->obj->set("refCount", nova_num(shared ? (double)shared.use_count() : 0));

    if (v.isNumber())
        out->obj->set("value", v);
    else if (v.isString())
        out->obj->set("length", nova_num((double)utf8::length(v->sval)));
    else if (v.isArray())
        out->obj->set("length", nova_num((double)v->arr->inner.size()));
    else if (v.isObject())
    {
        out->obj->set("keyCount", nova_num((double)v->obj->inner.size()));
        out->obj->set("hasMeta", nova_bool(v->obj->meta != nullptr));
    }
    else if (v.isFunction())
    {
        out->obj->set("name", nova_str(v->fn ? v->fn->name : ""));
        out->obj->set("isNative", nova_bool(v->fn && v->fn->isNative));
        out->obj->set("isAsync", nova_bool(v->fn && v->fn->isAsync));
        out->obj->set("isGenerator", nova_bool(v->fn && v->fn->isGenerator));
        out->obj->set("isMemoized", nova_bool(v->fn && v->fn->memoize));
    }
    else if (v.isPointer() && v->ptr)
    {
        out->obj->set("isRaw", nova_bool(v->ptr->isRaw));
        out->obj->set("address", nova_str(v->ptr->address));
        if (v->ptr->isRaw)
        {
            out->obj->set("rawType", nova_str(nativeTypeName(v->ptr->rawType)));
            out->obj->set("rawSize", nova_num((double)v->ptr->rawSize));
        }
    }
    else if (v.isStruct() && v->strct)
        out->obj->set("typeName", nova_str(v->strct->typeName));
    else if (v.isEnum() && v->enm)
    {
        out->obj->set("typeName", nova_str(v->enm->typeName));
        out->obj->set("variant", nova_str(v->enm->variant));
    }

    out->obj->set("hasOverloads", nova_bool(v && v->overloads != nullptr));
    return out; }, "Inspect"));

        // ── Std.Buffer ──────────────────────────────────────────────────────────
        // ── Std.Buffer ──────────────────────────────────────────────────────────
        {
            auto bufferNs = nova_obj();

            // declared first so the lambda body can safely capture it BY VALUE
            // (shared_ptr copy) instead of by reference to a local that dies
            // when _setupGlobalScope() returns.
            auto wrapBufShared = std::make_shared<std::function<Val(std::shared_ptr<std::vector<uint8_t>>)>>();

            *wrapBufShared = [this, wrapBufShared](std::shared_ptr<std::vector<uint8_t>> backing) -> Val
            {
                auto ptr = std::make_shared<NovaPointer>();
                ptr->isRaw = true;
                ptr->rawType = NativeType::U8;
                ptr->rawBuffer = backing;
                ptr->rawAddr = backing->data();
                ptr->rawSize = backing->size();
                std::ostringstream ss;
                ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(ptr->rawAddr);
                ptr->address = ss.str();

                auto rawVal = std::make_shared<NovaValue>();
                rawVal->kind = VK::Pointer;
                rawVal->ptr = ptr;
                Val rawPtrVal = TVal(rawVal);

                auto self = nova_obj();
                self->obj->set("__type__", nova_str("buffer"));
                self->obj->set("size", nova_num((double)backing->size()));

                auto exe = this;
                auto checkRange = [exe, backing](size_t offset, size_t width, const char *who)
                {
                    if (offset + width > backing->size())
                        exe->_error(std::string(who) + ": offset " + std::to_string(offset) +
                                    " + width " + std::to_string(width) +
                                    " out of bounds for buffer of size " + std::to_string(backing->size()));
                };

                auto makeReader = [backing, checkRange](NativeType t, const char *name)
                {
                    return NovaValue::makeNative([backing, checkRange, t, name](ValVec a, auto) -> Val
                                                 {
                        size_t off = a.empty() ? 0 : (size_t)a[0].asNumber();
                        size_t w = nativeTypeSize(t);
                        checkRange(off, w, name);
                        return nativeDecode(backing->data() + off, w, t); }, name);
                };
                self->obj->set("readU8", makeReader(NativeType::U8, "readU8"));
                self->obj->set("readI8", makeReader(NativeType::I8, "readI8"));
                self->obj->set("readU16", makeReader(NativeType::U16, "readU16"));
                self->obj->set("readI16", makeReader(NativeType::I16, "readI16"));
                self->obj->set("readU32", makeReader(NativeType::U32, "readU32"));
                self->obj->set("readI32", makeReader(NativeType::I32, "readI32"));
                self->obj->set("readU64", makeReader(NativeType::U64, "readU64"));
                self->obj->set("readI64", makeReader(NativeType::I64, "readI64"));
                self->obj->set("readF32", makeReader(NativeType::F32, "readF32"));
                self->obj->set("readF64", makeReader(NativeType::F64, "readF64"));

                auto makeWriter = [backing, checkRange](NativeType t, const char *name)
                {
                    return NovaValue::makeNative([backing, checkRange, t, name](ValVec a, auto) -> Val
                                                 {
                        if (a.size() < 2)
                            return nova_null();
                        size_t off = (size_t)a[0].asNumber();
                        size_t w = nativeTypeSize(t);
                        checkRange(off, w, name);
                        auto bytes = nativeEncode(a[1], t);
                        std::memcpy(backing->data() + off, bytes.data(), std::min(bytes.size(), w));
                        return nova_null(); }, name);
                };
                self->obj->set("writeU8", makeWriter(NativeType::U8, "writeU8"));
                self->obj->set("writeI8", makeWriter(NativeType::I8, "writeI8"));
                self->obj->set("writeU16", makeWriter(NativeType::U16, "writeU16"));
                self->obj->set("writeI16", makeWriter(NativeType::I16, "writeI16"));
                self->obj->set("writeU32", makeWriter(NativeType::U32, "writeU32"));
                self->obj->set("writeI32", makeWriter(NativeType::I32, "writeI32"));
                self->obj->set("writeU64", makeWriter(NativeType::U64, "writeU64"));
                self->obj->set("writeI64", makeWriter(NativeType::I64, "writeI64"));
                self->obj->set("writeF32", makeWriter(NativeType::F32, "writeF32"));
                self->obj->set("writeF64", makeWriter(NativeType::F64, "writeF64"));

                self->obj->set("fill", NovaValue::makeNative([backing](ValVec a, auto) -> Val
                                                             {
                    uint8_t v = a.empty() ? 0 : (uint8_t)(int)a[0].asNumber();
                    std::fill(backing->begin(), backing->end(), v);
                    return nova_null(); }, "fill"));

                self->obj->set("toArray", NovaValue::makeNative([backing](ValVec, auto) -> Val
                                                                {
                    ValVec out;
                    out.reserve(backing->size());
                    for (uint8_t byte : *backing)
                        out.push_back(nova_num((double)byte));
                    return nova_arr(std::move(out)); }, "toArray"));

                self->obj->set("toString", NovaValue::makeNative([backing](ValVec, auto) -> Val
                                                                 { return nova_str(std::string(backing->begin(), backing->end())); }, "toString"));

                // FIX: capture wrapBufShared BY VALUE (shared_ptr copy) — keeps
                // the wrapping function alive for as long as any Buffer object
                // holding this closure is alive, instead of dangling.
                self->obj->set("slice", NovaValue::makeNative([exe, backing, wrapBufShared](ValVec a, auto) -> Val
                                                              {
                    size_t start = a.size() > 0 ? (size_t)a[0].asNumber() : 0;
                    size_t end = a.size() > 1 ? (size_t)a[1].asNumber() : backing->size();
                    if (start > backing->size() || end > backing->size() || start > end)
                        exe->_error("slice: invalid range [" + std::to_string(start) + ", " + std::to_string(end) + ")");
                    auto newBacking = std::make_shared<std::vector<uint8_t>>(
                        backing->begin() + start, backing->begin() + end);
                    return (*wrapBufShared)(newBacking); }, "slice"));

                self->obj->set("copyFrom", NovaValue::makeNative([exe, backing, checkRange](ValVec a, auto) -> Val
                                                                 {
                    if (a.empty() || !a[0].isObject())
                        exe->_error("copyFrom: expected a Buffer argument");
                    Val srcRaw = a[0]->obj->get("__rawptr__");
                    if (!srcRaw || !srcRaw.isPointer() || !srcRaw->ptr || !srcRaw->ptr->rawBuffer)
                        exe->_error("copyFrom: argument is not a Buffer");
                    size_t destOffset = a.size() > 1 ? (size_t)a[1].asNumber() : 0;
                    auto &srcData = *srcRaw->ptr->rawBuffer;
                    checkRange(destOffset, srcData.size(), "copyFrom");
                    std::memcpy(backing->data() + destOffset, srcData.data(), srcData.size());
                    return nova_null(); }, "copyFrom"));

                self->obj->set("toRawPtr", NovaValue::makeNative([rawPtrVal](ValVec, auto) -> Val
                                                                 { return rawPtrVal; }, "toRawPtr"));

                self->obj->set("__rawptr__", rawPtrVal);

                return self;
            };

            bufferNs->obj->set("new", NovaValue::makeNative([this, wrapBufShared](ValVec a, auto) -> Val
                                                            {
                size_t size = a.empty() ? 0 : (size_t)a[0].asNumber();
                return (*wrapBufShared)(std::make_shared<std::vector<uint8_t>>(size, 0)); }, "new"));

            bufferNs->obj->set("from", NovaValue::makeNative([this, wrapBufShared](ValVec a, auto) -> Val
                                                             {
                if (a.empty())
                    return (*wrapBufShared)(std::make_shared<std::vector<uint8_t>>());
                if (a[0].isString())
                {
                    auto &s = a[0]->sval;
                    return (*wrapBufShared)(std::make_shared<std::vector<uint8_t>>(s.begin(), s.end()));
                }
                if (a[0].isArray())
                {
                    auto backing = std::make_shared<std::vector<uint8_t>>();
                    backing->reserve(a[0]->arr->inner.size());
                    for (auto &v : a[0]->arr->inner)
                        backing->push_back((uint8_t)(int)v.asNumber());
                    return (*wrapBufShared)(backing);
                }
                _error("Buffer.from: expected a string or array");
                return nova_null(); }, "from"));

            std_obj->obj->set("Buffer", bufferNs);
        }
        // ── Std.Symbol ──────────────────────────────────────────────────────────
        {
            auto symbolNs = nova_obj();
            auto counter = std::make_shared<uint64_t>(0);

            symbolNs->obj->set("new", NovaValue::makeNative([counter](ValVec a, auto) -> Val
                                                            {
                std::string desc = a.empty() ? "" : a[0].asString();
                uint64_t id = (*counter)++;

                auto sym = nova_obj();
                sym->obj->set("__type__", nova_str("symbol"));
                sym->obj->set("__id__", nova_num((double)id));
                sym->obj->set("description", nova_str(desc));

                sym->obj->set("toString", NovaValue::makeNative([desc, id](ValVec, auto) -> Val
                {
                    return nova_str("Symbol(" + desc + ")#" + std::to_string(id));
                }, "toString"));

                if (!sym->overloads)
                    sym->overloads = std::make_shared<ValMap>();
                (*sym->overloads)["binary:=="] = NovaValue::makeNative(
                    [id](ValVec args, auto) -> Val
                    {
                        if (args.size() < 2 || !args[1] || !args[1].isObject())
                            return nova_bool(false);
                        Val otherId = args[1]->obj->get("__id__");
                        Val otherType = args[1]->obj->get("__type__");
                        if (!otherId || !otherType || otherType->sval != "symbol")
                            return nova_bool(false);
                        return nova_bool((uint64_t)otherId->nval == id);
                    }, "binary:==");

                return sym; }, "new"));

            std_obj->obj->set("Symbol", symbolNs);
        }
        // ── BigInt / BigFloat constructors ──────────────────────────────────────────
        std_obj->obj->set("BigInt", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
    if (a.empty()) return NovaValue::makeBigInt(0LL);
    if (a[0].isBigInt())   return a[0];
    if (a[0].isBigFloat()) return NovaValue::makeBigInt(a[0]->bigfloat->toString());
    if (a[0].isString())   return NovaValue::makeBigInt(a[0]->sval);
    return NovaValue::makeBigInt((int64_t)a[0].asNumber()); }, "BigInt"));

        std_obj->obj->set("BigFloat", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                            {
    if (a.empty()) return NovaValue::makeBigFloat(0.0);
    int prec = a.size() > 1 ? (int)a[1].asNumber() : NovaBigFloat::DEFAULT_PREC;
    if (a[0].isBigFloat()) return a[0];
    if (a[0].isBigInt())   return NovaValue::makeBigFloat(NovaBigFloat(*a[0]->bigint, prec));
    if (a[0].isString())   return NovaValue::makeBigFloat(a[0]->sval, prec);
    return NovaValue::makeBigFloat(a[0].asNumber(), prec); }, "BigFloat"));

        // also expose top-level
        s->setOwn("BigInt", std_obj->obj->get("BigInt"));
        s->setOwn("BigFloat", std_obj->obj->get("BigFloat"));

        // ── register sub-objects onto std_obj ─────────────────────────────────────
        _registerJSON(std_obj);
        _registerMath(std_obj);
        _registerCCType(std_obj);
        _registerAlgorithm(std_obj);
        _registerBit(std_obj);
        _registerComplex(std_obj);
        _registerValarray(std_obj);
        _registerCharconv(std_obj);
        _registerProcess(std_obj);
        _registerIO(std_obj);
        _registerTimers(std_obj);
        _registerRandom(std_obj);
        _registerChrono(std_obj);
        _registerNative(std_obj);
        _registerStdioControl(std_obj);
        _registerMemory(std_obj);
        _registerCFile(std_obj);
        _registerSystemControl(std_obj);
        _registerErrnoControl(std_obj);
        _registerSignalControl(std_obj);
        _registerFS(std_obj);
        _registerSocket(std_obj);
        _registerFiber(std_obj);
        _registerExperimental(std_obj);
        _registerFFI(std_obj);

        std_obj->obj->set("includeKit", NovaValue::makeNative([this](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                              {
                                                                  if (a.empty() || a[0].isNull())
                                                                      _error("Std.includeKit: expected a kit name or path");

                                                                  std::string kitName = a[0].asString();
                                                                  Val alias = a.size() > 1 && a[1] && !a[1].isNull() ? a[1] : nullptr;
                                                                  Val optsVal = a.size() > 2 ? a[2] : nullptr;

                                                                  // ── parse opts ────────────────────────────────────────────────────
                                                                  bool opt_silent = false;
                                                                  bool opt_nocache = false;
                                                                  std::string opt_customDllPath;

                                                                  if (optsVal && optsVal.isObject())
                                                                  {
                                                                      auto getBool = [&](const std::string &k) -> bool
                                                                      {
                                                                          Val v = optsVal->obj->get(k);
                                                                          return v && v.asBool();
                                                                      };
                                                                      auto getStr = [&](const std::string &k) -> std::string
                                                                      {
                                                                          Val v = optsVal->obj->get(k);
                                                                          return v && v.isString() ? v->sval : "";
                                                                      };
                                                                      opt_silent = getBool("silent");
                                                                      opt_nocache = getBool("nocache");
                                                                      opt_customDllPath = getStr("customDllPath");
                                                                  }

                                                                  // ── resolve DLL path ──────────────────────────────────────────────
                                                                  std::string binPath;
                                                                  if (!opt_customDllPath.empty())
                                                                  {
                                                                      binPath = opt_customDllPath;
                                                                  }
                                                                  else
                                                                  {
                                                                      // 1. nova_modules/<kitName>/out/main.dll
                                                                      fs::path nmKit = fs::current_path() / "nova_modules" / kitName / "out";
#ifdef _WIN32
                                                                      fs::path candidate = nmKit / "main.dll";
#else
                                                                      fs::path candidate = nmKit / "main.so";
#endif
                                                                      if (fs::exists(candidate))
                                                                      {
                                                                          binPath = fs::absolute(candidate).string();
                                                                      }
                                                                      else
                                                                      {
                                                                          // 2. kits/<kitName>/out/main.dll (legacy path)
                                                                          fs::path base = fs::path(__FILE__).parent_path();
                                                                          fs::path legacy = base / ".." / ".." / "kits" / kitName / "out";
#ifdef _WIN32
                                                                          legacy /= "main.dll";
#else
                                                                          legacy /= "main.so";
#endif
                                                                          try
                                                                          {
                                                                              binPath = fs::absolute(legacy).string();
                                                                          }
                                                                          catch (...)
                                                                          {
                                                                              binPath = legacy.string();
                                                                          }
                                                                      }
                                                                  }

                                                                  std::string cacheKey = "dinclude:" + binPath;
                                                                  if (!opt_nocache)
                                                                  {
                                                                      auto it = moduleCache.find(cacheKey);
                                                                      if (it != moduleCache.end())
                                                                      {
                                                                          Val cached = globalScope->get(kitName);
                                                                          if (alias && alias.isString() && cached)
                                                                              cs->setOwn(alias->sval, cached);
                                                                          return cached ? cached : nova_null();
                                                                      }
                                                                  }

            // ── load the DLL ──────────────────────────────────────────────────
#if !NOVA_FFI_PLATFORM_SUPPORTED
                                                                  _error("Std.includeKit: dynamic loading not supported on this platform");
                                                                  return nova_null();
#else
#ifdef _WIN32
                                                                  HMODULE handle = LoadLibraryA(binPath.c_str());
                                                                  if (!handle)
                                                                      _error("Std.includeKit: failed to load '" + binPath +
                                                                             "' (error " + std::to_string(GetLastError()) + ")");
                                                                  using KitRegFn = void (*)(Executor *);
                                                                  auto regFn = reinterpret_cast<KitRegFn>(GetProcAddress(handle, "nova_kit_register"));
                                                                  if (!regFn)
                                                                  {
                                                                      FreeLibrary(handle);
                                                                      _error("Std.includeKit: no nova_kit_register in '" + binPath + "'");
                                                                  }
#else
                                                                  ::dlerror();
                                                                  void *handle = ::dlopen(binPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
                                                                  if (!handle)
                                                                      _error("Std.includeKit: " + std::string(::dlerror() ? ::dlerror() : "unknown"));
                                                                  ::dlerror();
                                                                  using KitRegFn = void (*)(Executor *);
                                                                  auto regFn = reinterpret_cast<KitRegFn>(::dlsym(handle, "nova_kit_register"));
                                                                  if (!regFn)
                                                                  {
                                                                      ::dlclose(handle);
                                                                      _error("Std.includeKit: no nova_kit_register in '" + binPath + "'");
                                                                  }
#endif

                                                                  // ── silence IO if requested ───────────────────────────────────────
                                                                  Val savedPrint = globalScope->get("print");
                                                                  Val savedPrintln = globalScope->get("println");
                                                                  if (opt_silent)
                                                                  {
                                                                      auto noop = NovaValue::makeNative([](ValVec, auto) -> Val
                                                                                                        { return nova_null(); }, "noop");
                                                                      globalScope->setOwn("print", noop);
                                                                      globalScope->setOwn("println", noop);
                                                                  }

                                                                  try
                                                                  {
                                                                      regFn(this);
                                                                  }
                                                                  catch (std::exception &e)
                                                                  {
                                                                      _error("Std.includeKit: registration threw: " + std::string(e.what()));
                                                                  }
                                                                  catch (...)
                                                                  {
                                                                      _error("Std.includeKit: registration threw unknown exception");
                                                                  }

                                                                  if (opt_silent)
                                                                  {
                                                                      globalScope->setOwn("print", savedPrint);
                                                                      globalScope->setOwn("println", savedPrintln);
                                                                  }

                                                                  moduleCache[cacheKey] = nova_bool(true);

                                                                  // kit registers itself under its own name on globalScope
                                                                  Val kit = globalScope->get(kitName);
                                                                  if (!kit)
                                                                      kit = nova_null();

                                                                  if (alias && alias.isString())
                                                                      cs->setOwn(alias->sval, kit);

                                                                  return kit;
#endif
                                                              },
                                                              "includeKit"));

        // ── Std.include(path, alias?, opts?) — function-level module import ─────
        // opts object keys:
        //   silent   : bool  — suppress all IO (print/println/input/stdio/streams)
        //   sandbox  : bool  — hide Std from the module's scope entirely
        //   readonly : bool  — module runs against a snapshot of globals; mutations
        //                      do not propagate back to the real global scope
        //   strict   : bool  — reserved; passed as __strict__ into module scope
        //   timeout  : num   — reserved; max ms (not yet enforced, stored for tooling)
        //   nocache  : bool  — skip the module cache (re-execute every call)
        std_obj->obj->set("include", NovaValue::makeNative([this](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                           {
            if (a.empty() || a[0].isNull())
                _error("Std.include: expected a module path string");

            std::string path = a[0].asString();

            // ── parse opts ────────────────────────────────────────────────────
            Val optsVal = a.size() > 2 ? a[2] : nullptr;
            bool opt_silent   = false;
            bool opt_sandbox  = false;
            bool opt_readonly = false;
            bool opt_strict   = false;
            bool opt_nocache  = false;
            int  opt_timeout  = 0;

            if (optsVal && optsVal.isObject())
            {
                auto getBool = [&](const std::string &k) -> bool {
                    Val v = optsVal->obj->get(k);
                    return v && v.asBool();
                };
                opt_silent   = getBool("silent");
                opt_sandbox  = getBool("sandbox");
                opt_readonly = getBool("readonly");
                opt_strict   = getBool("strict");
                opt_nocache  = getBool("nocache");
                Val tv = optsVal->obj->get("timeout");
                if (tv && tv.isNumber()) opt_timeout = (int)tv.asNumber();
            }

            // ── cache lookup (opts that change execution are part of key) ─────
            std::string cacheKey = path
                + (opt_silent   ? "|silent"   : "")
                + (opt_sandbox  ? "|sandbox"  : "")
                + (opt_readonly ? "|readonly" : "");

            if (!opt_nocache)
            {
                auto cit = moduleCache.find(cacheKey);
                if (cit != moduleCache.end())
                {
                    if (a.size() > 1 && a[1] && a[1].isString())
                        cs->setOwn(a[1].asString(), cit->second);
                    return cit->second;
                }
            }

            // ── resolve & read source ─────────────────────────────────────────
            std::string content, fullPath = path;
            try
            {
                fs::path base = fs::path(filename).parent_path();
                fs::path resolved;

                // 1. exact path relative to current file (must be a file, not dir)
                {
                    fs::path candidate = base / path;
                    if (fs::exists(candidate) && !fs::is_directory(candidate))
                        resolved = candidate;
                }

                // 2. with extensions
                if (resolved.empty()) {
                    for (auto& ext : {".nv", ".nova", ".novac"}) {
                        fs::path candidate = base / (path + ext);
                        if (fs::exists(candidate) && !fs::is_directory(candidate)) {
                            resolved = candidate;
                            break;
                        }
                    }
                }

                // 3. nova_modules/<path>/novapkg.json → read "main"
                if (resolved.empty()) {
                    fs::path nmPath = fs::current_path() / "nova_modules" / path;
                    fs::path pkgJson = nmPath / "novapkg.json";
if (fs::exists(pkgJson) && !fs::is_directory(pkgJson)) {
    std::string mainFile;
    try {
        std::ifstream f(pkgJson);
        std::string s((std::istreambuf_iterator<char>(f)), {});
        Val pkg = nova_json::parse(s);
        mainFile = nova_json::getString(pkg, "main");
    } catch (...) {}
    if (mainFile.empty()) {
        for (auto& stem : {"main", "index"}) {
            for (auto& ext : {".nv", ".nova", ".novac"}) {
                fs::path c = nmPath / (std::string(stem) + ext);
                if (fs::exists(c) && !fs::is_directory(c)) { resolved = c; break; }
            }
            if (!resolved.empty()) break;
        }
    } else {
        resolved = nmPath / mainFile;
    }
}
                }

                // 4. nova_modules/<path> without novapkg.json → (main|index).(nv|nova|novac)
                if (resolved.empty()) {
                    fs::path nmPath = fs::current_path() / "nova_modules" / path;
                    if (fs::exists(nmPath) && fs::is_directory(nmPath)) {
                        for (auto& stem : {"main", "index"}) {
                            for (auto& ext : {".nv", ".nova", ".novac"}) {
                                fs::path c = nmPath / (std::string(stem) + ext);
                                if (fs::exists(c) && !fs::is_directory(c)) {
                                    resolved = c;
                                    break;
                                }
                            }
                            if (!resolved.empty()) break;
                        }
                    }
                }

                if (resolved.empty() || !fs::exists(resolved))
                    _error("Std.include: module not found: " + path);

                fullPath = fs::absolute(resolved).string();
                std::ifstream f(fullPath);
                if (!f.is_open()) _error("Std.include: cannot open: " + fullPath);
                content = std::string(std::istreambuf_iterator<char>(f), {});
            }
            catch (RuntimeError &) { throw; }
            catch (std::exception &e) { _error(std::string("Std.include: ") + e.what()); }

            // ── build parent scope (readonly = isolated snapshot) ─────────────
            std::shared_ptr<Scope> parentScope;
            if (opt_readonly)
            {
                parentScope = std::make_shared<Scope>(Scope::Kind::Module, nullptr, nullptr);
                for (auto &[k, desc] : globalScope->variables)
                    parentScope->variables[k] = desc;
                parentScope->globalScope = parentScope;
            }
            else
            {
                parentScope = globalScope;
            }

            auto modScope = std::make_shared<Scope>(
                Scope::Kind::Module, parentScope,
                opt_readonly ? parentScope : globalScope);
            auto exportsObj = nova_obj();
            modScope->setOwn("__exports__", exportsObj);
            if (opt_strict)
                modScope->setOwn("__strict__", nova_bool(true));
            if (opt_timeout > 0)
                modScope->setOwn("__timeout__", nova_num(opt_timeout));

            // ── sandbox: hide Std ─────────────────────────────────────────────
            if (opt_sandbox)
                modScope->setOwn("Std", nova_null());

            // ── silent: replace all IO with no-ops ───────────────────────────
            if (opt_silent)
            {
                auto noop_v = NovaValue::makeNative([](ValVec, auto) -> Val { return nova_null(); }, "noop");
                auto noop_s = NovaValue::makeNative([](ValVec, auto) -> Val { return nova_str(""); },  "noop_s");

                modScope->setOwn("print",   noop_v);
                modScope->setOwn("println", noop_v);
                modScope->setOwn("input",   noop_s);

                Val stdOrig = globalScope->get("Std");
                if (stdOrig && stdOrig.isObject())
                {
                    auto silentStd = nova_obj();
                    for (auto &[k, v] : stdOrig->obj->inner)
                        silentStd->obj->set(k, v);

                    silentStd->obj->set("print",   noop_v);
                    silentStd->obj->set("println", noop_v);
                    silentStd->obj->set("input",   noop_s);

                    auto silentStdio = nova_obj();
                    silentStdio->obj->set("write", noop_v);
                    silentStdio->obj->set("read",  noop_s);
                    silentStd->obj->set("stdio",  silentStdio);

                    // replace streams with null so stream-write calls silently fail
                    silentStd->obj->set("stdout", nova_null());
                    silentStd->obj->set("stderr", nova_null());
                    silentStd->obj->set("stdin",  nova_null());
                    silentStd->obj->set("stdlog", nova_null());

                    modScope->setOwn("Std", silentStd);
                }
            }

            // ── parse & execute ───────────────────────────────────────────────
            Parser p(content, fullPath);
            auto ast = p.parse();
            Executor modExe(content, fullPath);
            modExe.globalScope = opt_readonly ? parentScope : globalScope;
            modExe.run(ast.get(), modScope);

            // ── collect exports ───────────────────────────────────────────────
            Val exports = modScope->get("__exports__");
            if (!exports || (exports.isObject() && exports->obj->inner.empty()))
            {
                exports = nova_obj();
                for (auto &[k, desc] : modScope->variables)
                {
                    if (k.size() >= 2 && k[0] == '_' && k[1] == '_') continue;
                    Val v = (desc.hasHooks && desc.read) ? desc.read() : desc.raw;
                    exports->obj->set(k, v);
                }
            }

            if (!opt_nocache)
                moduleCache[cacheKey] = exports;

            if (a.size() > 1 && a[1] && a[1].isString())
                cs->setOwn(a[1].asString(), exports);

            return exports; }, "include"));

        std_obj->obj->set("instantiate", NovaValue::makeNative([this](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                               {
    if (a.empty() || a[0].isNull())
        _error("Std.instantiate: expected a function, module path string, or code string");

    // ── function: call directly in caller's scope ─────────────────────────
    if (a[0].isFunction())
        return callFunction(a[0], {}, cs);

    if (!a[0].isString())
        _error("Std.instantiate: argument must be a function or string");

    std::string input = a[0].asString();

    // ── string: try to resolve as a file path (same logic as Std.include) ─
    std::string content, fullPath;
    bool foundFile = false;

    try {
        fs::path base = fs::path(filename).parent_path();

        auto tryResolve = [&](fs::path candidate) -> bool {
            if (fs::exists(candidate) && !fs::is_directory(candidate)) {
                fullPath = fs::absolute(candidate).string();
                std::ifstream f(fullPath);
                if (!f.is_open()) return false;
                content = std::string(std::istreambuf_iterator<char>(f), {});
                return true;
            }
            return false;
        };

        // 1. exact relative path
        if (!foundFile) foundFile = tryResolve(base / input);

        // 2. with extensions
        if (!foundFile) {
            for (auto& ext : {".nv", ".nova", ".novac"}) {
                if (tryResolve(base / (input + ext))) { foundFile = true; break; }
            }
        }

        // 3. nova_modules/<input>/novapkg.json -> "main"
        if (!foundFile) {
            fs::path nmPath = fs::current_path() / "nova_modules" / input;
            fs::path pkgJson = nmPath / "novapkg.json";
            if (fs::exists(pkgJson) && !fs::is_directory(pkgJson)) {
                std::string mainFile;
                try {
                    std::ifstream f(pkgJson);
                    std::string s((std::istreambuf_iterator<char>(f)), {});
                    Val pkg = nova_json::parse(s);
                    mainFile = nova_json::getString(pkg, "main");
                } catch (...) {}
                if (!mainFile.empty()) {
                    foundFile = tryResolve(nmPath / mainFile);
                } else {
                    for (auto& stem : {"main", "index"}) {
                        for (auto& ext : {".nv", ".nova", ".novac"}) {
                            if (tryResolve(nmPath / (std::string(stem) + ext))) { foundFile = true; break; }
                        }
                        if (foundFile) break;
                    }
                }
            }
        }

        // 4. nova_modules/<input> without novapkg.json
        if (!foundFile) {
            fs::path nmPath = fs::current_path() / "nova_modules" / input;
            if (fs::exists(nmPath) && fs::is_directory(nmPath)) {
                for (auto& stem : {"main", "index"}) {
                    for (auto& ext : {".nv", ".nova", ".novac"}) {
                        if (tryResolve(nmPath / (std::string(stem) + ext))) { foundFile = true; break; }
                    }
                    if (foundFile) break;
                }
            }
        }
    } catch (...) {}

    if (foundFile) {
        // run file contents in caller's scope
        Parser p(content, fullPath);
        auto ast = p.parse();
        Executor runExe(content, fullPath);
        runExe.globalScope = globalScope;
        return runExe.run(ast.get(), cs);
    }

    // ── fallback: treat the string itself as Nova source code ─────────────
    try {
        Parser p(input, filename);
        auto ast = p.parse();
        Executor runExe(input, filename);
        runExe.globalScope = globalScope;
        return runExe.run(ast.get(), cs);
    } catch (std::exception& e) {
        _error(std::string("Std.instantiate: failed to parse/run as code: ") + e.what());
    }

    return nova_null(); }, "instantiate"));

        // ── Std.dinclude(path) — load a compiled native kit (.so / .dll) ──────
        // Opens a shared library that exports the C symbol:
        //
        //   extern "C" void nova_kit_register(novac::Executor* exe);
        //
        // That function is responsible for calling exe->globalScope->setOwn(...)
        // (or the equivalent kit-registration helpers) to install its builtins.
        // Returns true on success, false/null on failure.
        //
        // Example kit entry point (kit_mykit.cpp):
        //   extern "C" void nova_kit_register(novac::Executor* exe) {
        //       auto obj = nova_obj();
        //       obj->obj->set("hello", NovaValue::makeNative([](ValVec, auto)->Val{
        //           std::cout << "hello from native kit\n";
        //           return nova_null();
        //       }, "hello"));
        //       exe->globalScope->setOwn("MyKit", obj);
        //   }
        std_obj->obj->set("dinclude", NovaValue::makeNative([this](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                            {
#if !NOVA_FFI_PLATFORM_SUPPORTED
                                                                _error("Std.dinclude: dynamic loading is not supported on this platform");
                                                                return nova_bool(false);
#else
                                                                if (a.empty() || a[0].isNull())
                                                                    _error("Std.dinclude: expected a shared-library path string");

                                                                std::string path = a[0].asString();

                                                                // De-duplicate: if this path was already loaded, skip silently.
                                                                // We reuse the moduleCache key space with a "dinclude:" prefix so
                                                                // it never collides with normal Nova module paths.
                                                                std::string cacheKey = "dinclude:" + path;
                                                                {
                                                                    auto it = moduleCache.find(cacheKey);
                                                                    if (it != moduleCache.end())
                                                                        return nova_bool(true); // already registered
                                                                }

#ifdef _WIN32
                                                                HMODULE handle = LoadLibraryA(path.c_str());
                                                                if (!handle)
                                                                {
                                                                    std::string err = "Std.dinclude: failed to load '" + path + "' (error " +
                                                                                      std::to_string(GetLastError()) + ")";
                                                                    _error(err);
                                                                    return nova_bool(false);
                                                                }
                                                                using KitRegFn = void (*)(Executor *);
                                                                auto regFn = reinterpret_cast<KitRegFn>(
                                                                    GetProcAddress(handle, "nova_kit_register"));
                                                                if (!regFn)
                                                                {
                                                                    FreeLibrary(handle);
                                                                    _error("Std.dinclude: '" + path +
                                                                           "' does not export 'nova_kit_register'");
                                                                    return nova_bool(false);
                                                                }
#else
                                                                ::dlerror(); // clear
                                                                void *handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
                                                                if (!handle)
                                                                {
                                                                    const char *e = ::dlerror();
                                                                    _error("Std.dinclude: failed to load '" + path + "': " +
                                                                           (e ? e : "unknown error"));
                                                                    return nova_bool(false);
                                                                }
                                                                ::dlerror(); // clear again before dlsym
                                                                using KitRegFn = void (*)(Executor *);
                                                                auto regFn = reinterpret_cast<KitRegFn>(
                                                                    ::dlsym(handle, "nova_kit_register"));
                                                                const char *symErr = ::dlerror();
                                                                if (symErr || !regFn)
                                                                {
                                                                    ::dlclose(handle);
                                                                    _error("Std.dinclude: '" + path +
                                                                           "' does not export 'nova_kit_register': " +
                                                                           (symErr ? symErr : "symbol not found"));
                                                                    return nova_bool(false);
                                                                }
#endif
                                                                // Call the kit's registration entry point.
                                                                // Any exception it throws propagates as a Nova runtime error.
                                                                try
                                                                {
                                                                    regFn(this);
                                                                }
                                                                catch (std::exception &e)
                                                                {
                                                                    _error(std::string("Std.dinclude: kit registration threw: ") + e.what());
                                                                    return nova_bool(false);
                                                                }
                                                                catch (...)
                                                                {
                                                                    _error("Std.dinclude: kit registration threw an unknown exception");
                                                                    return nova_bool(false);
                                                                }

                                                                // Mark as loaded so repeated calls are no-ops.
                                                                moduleCache[cacheKey] = nova_bool(true);
                                                                return nova_bool(true);
#endif
                                                            },
                                                            "dinclude"));

        {
            using namespace nova_email_detail;
            auto email = nova_obj();

            // ── Std.Email.ParseSMTP(rawString) → emailObject ──────────────────────
            email->obj->set("ParseSMTP", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
        if (a.empty()) return nova_null();
        ParsedEmail e = parseRaw(a[0].asString());
        return parsedToVal(e); }, "ParseSMTP"));

            // ── Std.Email.StringifySMTP(emailObj) → rawString ─────────────────────
            email->obj->set("StringifySMTP", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
        if (a.empty()) return nova_str("");
        EmailSpec spec = specFromVal(a[0]);
        return nova_str(buildRaw(spec)); }, "StringifySMTP"));

            // ── Std.Email.Send(smtpCfg, emailObj) → {ok, log, response} ──────────
            // smtpCfg: { host, port?, username?, password?, timeout? }
            // emailObj: { from, to, subject, body, htmlBody?, cc?, attachments? }
            email->obj->set("Send", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
        if (a.size() < 2 || !a[0].isObject() || !a[1].isObject())
            return nova_null();

        Val cfg  = a[0];
        Val eobj = a[1];

        SmtpSession sess;
        sess.host     = cfg->obj->get("host")     ? cfg->obj->get("host").asString()     : "localhost";
        sess.port     = cfg->obj->get("port")     ? (int)cfg->obj->get("port").asNumber(): 587;
        sess.username = cfg->obj->get("username") ? cfg->obj->get("username").asString() : "";
        sess.password = cfg->obj->get("password") ? cfg->obj->get("password").asString() : "";
        sess.timeoutMs= cfg->obj->get("timeout")  ? (int)cfg->obj->get("timeout").asNumber(): 10000;

        EmailSpec spec = specFromVal(eobj);
        std::string raw = buildRaw(spec);

        // collect all rcpt addresses
        std::vector<std::string> rcpts;
        for (auto& [n,a] : spec.to)  if (!a.empty()) rcpts.push_back(a);
        for (auto& [n,a] : spec.cc)  if (!a.empty()) rcpts.push_back(a);
        for (auto& [n,a] : spec.bcc) if (!a.empty()) rcpts.push_back(a);

        SmtpResult r = smtpSend(sess, spec.fromAddr, rcpts, raw);

        auto res = nova_obj();
        res->obj->set("ok",       nova_bool(r.ok));
        res->obj->set("log",      nova_str(r.log));
        res->obj->set("response", nova_str(r.lastResponse));
        res->obj->set("raw",      nova_str(raw));
        return res; }, "Send"));

            // ── Std.Email.SendRawSMTP(smtpCfg, from, rcpts[], rawMsg) → result ───
            email->obj->set("SendRawSMTP", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
        if (a.size() < 4) return nova_null();
        Val cfg = a[0];
        SmtpSession sess;
        sess.host     = cfg->obj->get("host")     ? cfg->obj->get("host").asString()     : "localhost";
        sess.port     = cfg->obj->get("port")     ? (int)cfg->obj->get("port").asNumber(): 25;
        sess.username = cfg->obj->get("username") ? cfg->obj->get("username").asString() : "";
        sess.password = cfg->obj->get("password") ? cfg->obj->get("password").asString() : "";

        std::string from = a[1].asString();
        std::vector<std::string> rcpts;
        if (a[2].isArray())
            for (auto& v : a[2]->arr->inner) rcpts.push_back(v.asString());
        else
            rcpts.push_back(a[2].asString());
        std::string raw = a[3].asString();

        SmtpResult r = smtpSend(sess, from, rcpts, raw);
        auto res = nova_obj();
        res->obj->set("ok",       nova_bool(r.ok));
        res->obj->set("log",      nova_str(r.log));
        res->obj->set("response", nova_str(r.lastResponse));
        return res; }, "SendRawSMTP"));

            // ── Std.Email.Build(emailObj) → rawString (alias for StringifySMTP) ───
            email->obj->set("Build", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                           {
        if (a.empty()) return nova_str("");
        return nova_str(buildRaw(specFromVal(a[0]))); }, "Build"));

            // ── Std.Email.Validate(address) → {ok, local, domain, error} ─────────
            email->obj->set("Validate", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                              {
        if (a.empty()) return nova_null();
        std::string addr = a[0].asString();
        auto [name, bare] = parseAddress(addr);
        auto res = nova_obj();
        size_t atPos = bare.find('@');
        if (atPos == std::string::npos || atPos == 0 || atPos == bare.size()-1) {
            res->obj->set("ok",    nova_bool(false));
            res->obj->set("error", nova_str("missing or misplaced '@'"));
            return res;
        }
        std::string local  = bare.substr(0, atPos);
        std::string domain = bare.substr(atPos+1);
        bool ok = !local.empty() && !domain.empty() && domain.find('.') != std::string::npos;
        res->obj->set("ok",     nova_bool(ok));
        res->obj->set("local",  nova_str(local));
        res->obj->set("domain", nova_str(domain));
        res->obj->set("addr",   nova_str(bare));
        res->obj->set("name",   nova_str(name));
        if (!ok) res->obj->set("error", nova_str("domain has no dot"));
        return res; }, "Validate"));

            // ── Std.Email.ParseAddress(str) → {name, addr, full} ─────────────────
            email->obj->set("ParseAddress", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
        if (a.empty()) return nova_null();
        auto [n,addr] = parseAddress(a[0].asString());
        auto res = nova_obj();
        res->obj->set("name", nova_str(n));
        res->obj->set("addr", nova_str(addr));
        res->obj->set("full", nova_str(formatAddress(n, addr)));
        return res; }, "ParseAddress"));

            // ── Std.Email.FormatAddress(name, addr) → string ──────────────────────
            email->obj->set("FormatAddress", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
        std::string name = a.size()>0 ? a[0].asString() : "";
        std::string addr = a.size()>1 ? a[1].asString() : "";
        return nova_str(formatAddress(name, addr)); }, "FormatAddress"));

            // ── Std.Email.ParseHeaders(rawHeaders) → object ───────────────────────
            email->obj->set("ParseHeaders", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
        if (a.empty()) return nova_obj();
        ParsedEmail e = parseRaw(a[0].asString() + "\r\n\r\n");
        auto hdrs = nova_obj();
        for (auto& [k,v] : e.headers) hdrs->obj->set(k, nova_str(v));
        return hdrs; }, "ParseHeaders"));

            // ── Std.Email.BuildHeaders(obj) → string ──────────────────────────────
            email->obj->set("BuildHeaders", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
        if (a.empty() || !a[0].isObject()) return nova_str("");
        std::string out;
        for (auto& [k,v] : a[0]->obj->inner)
            out += foldHeader(k, v.asString());
        return nova_str(out); }, "BuildHeaders"));

            // ── Std.Email.Base64Encode(str) → string ─────────────────────────────
            email->obj->set("Base64Encode", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  { return nova_str(base64Encode(a.empty() ? "" : a[0].asString())); }, "Base64Encode"));

            // ── Std.Email.Base64Decode(str) → string ─────────────────────────────
            email->obj->set("Base64Decode", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  { return nova_str(base64Decode(a.empty() ? "" : a[0].asString())); }, "Base64Decode"));

            // ── Std.Email.QuotedPrintableEncode(str) → string ────────────────────
            email->obj->set("QuotedPrintableEncode", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                           { return nova_str(quotedPrintableEncode(a.empty() ? "" : a[0].asString())); }, "QuotedPrintableEncode"));

            // ── Std.Email.RFC2047Encode(str) → string (header word encode) ───────
            email->obj->set("RFC2047Encode", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   { return nova_str(rfc2047Encode(a.empty() ? "" : a[0].asString())); }, "RFC2047Encode"));

            // ── Std.Email.MakeBoundary() → string ────────────────────────────────
            email->obj->set("MakeBoundary", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                  { return nova_str(makeBoundary()); }, "MakeBoundary"));

            // ── Std.Email.MakeMessageId(domain?) → string ─────────────────────────
            email->obj->set("MakeMessageId", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
        std::string domain = (a.empty() || a[0].isNull()) ? "novac" : a[0].asString();
        static std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<uint64_t> dist;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "<%llx.%llx@%s>",
                      (unsigned long long)dist(rng), (unsigned long long)dist(rng), domain.c_str());
        return nova_str(buf); }, "MakeMessageId"));

            // ── Std.Email.FoldHeader(name, value) → string ───────────────────────
            email->obj->set("FoldHeader", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                {
        std::string name = a.size()>0 ? a[0].asString() : "";
        std::string val  = a.size()>1 ? a[1].asString() : "";
        return nova_str(foldHeader(name, val)); }, "FoldHeader"));

            // ── Std.Email.ExtractBody(emailObj) → {text, html} ───────────────────
            email->obj->set("ExtractBody", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
        if (a.empty() || !a[0].isObject()) return nova_null();
        auto res = nova_obj();
        Val body = a[0]->obj->get("body");
        Val html = a[0]->obj->get("htmlBody");
        res->obj->set("text", body ? body : nova_str(""));
        res->obj->set("html", html ? html : nova_str(""));
        return res; }, "ExtractBody"));

            // ── Std.Email.ExtractAttachments(emailObj) → array ───────────────────
            email->obj->set("ExtractAttachments", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                        {
        if (a.empty() || !a[0].isObject()) return nova_arr();
        Val atts = a[0]->obj->get("attachments");
        return atts ? atts : nova_arr(); }, "ExtractAttachments"));

            // ── Std.Email.AddAttachment(emailObj, {filename,contentType,data}) ────
            email->obj->set("AddAttachment", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
        if (a.size() < 2 || !a[0].isObject()) return a.empty() ? nova_null() : a[0];
        Val atts = a[0]->obj->get("attachments");
        if (!atts || !atts.isArray()) {
            atts = nova_arr();
            a[0]->obj->set("attachments", atts);
        }
        atts->arr->push(a[1]);
        return a[0]; }, "AddAttachment"));

            // ── Std.Email.StripHtml(html) → plaintext ─────────────────────────────
            email->obj->set("StripHtml", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
        if (a.empty()) return nova_str("");
        std::string html = a[0].asString();
        std::string out;
        bool inTag = false;
        for (char c : html) {
            if      (c == '<') inTag = true;
            else if (c == '>') { inTag = false; out += ' '; }
            else if (!inTag)   out += c;
        }
        // collapse whitespace
        std::string collapsed;
        bool lastSpace = true;
        for (char c : out) {
            if (c==' '||c=='\t'||c=='\r'||c=='\n') {
                if (!lastSpace) { collapsed += ' '; lastSpace=true; }
            } else { collapsed += c; lastSpace=false; }
        }
        return nova_str(collapsed); }, "StripHtml"));

            // ── Std.Email.TextToHtml(text) → basic HTML ───────────────────────────
            email->obj->set("TextToHtml", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                {
        if (a.empty()) return nova_str("");
        std::string txt = a[0].asString();
        std::string out = "<html><body><p>";
        for (char c : txt) {
            if      (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else if (c == '\n') out += "<br>\n";
            else out += c;
        }
        out += "</p></body></html>";
        return nova_str(out); }, "TextToHtml"));

            // ── Std.Email.Template(templateStr, vars) → string ────────────────────
            // Simple {{varName}} substitution
            email->obj->set("Template", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                              {
        if (a.size() < 2 || !a[1].isObject()) return a.empty() ? nova_str("") : a[0];
        std::string tmpl = a[0].asString();
        for (auto& [k,v] : a[1]->obj->inner) {
            std::string token = "{{" + k + "}}";
            std::string val   = v.asString();
            size_t pos = 0;
            while ((pos = tmpl.find(token, pos)) != std::string::npos) {
                tmpl.replace(pos, token.size(), val);
                pos += val.size();
            }
        }
        return nova_str(tmpl); }, "Template"));

            // ── Std.Email.Reply(originalEmail, replySpec) → emailObj ──────────────
            // Constructs a reply object pre-filled with Re:, In-Reply-To, References
            email->obj->set("Reply", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                           {
        if (a.size() < 2 || !a[0].isObject()) return nova_null();
        Val orig = a[0];
        Val spec = a[1];

        auto get = [](Val v, const std::string& k) -> std::string {
            Val x = v->obj->get(k);
            return x ? x.asString() : "";
        };

        auto reply = nova_obj();
        // reply-to goes to: original from
        std::string origFrom = get(orig, "from");
        if (origFrom.empty()) {
            std::string fn = get(orig, "fromName"), fa = get(orig, "fromAddr");
            origFrom = formatAddress(fn, fa);
        }
        reply->obj->set("to", nova_str(origFrom));

        // subject
        std::string subj = get(orig, "subject");
        if (subj.substr(0,3) != "Re:") subj = "Re: " + subj;
        reply->obj->set("subject", nova_str(subj));

        // In-Reply-To / References
        std::string msgId = get(orig, "messageId");
        reply->obj->set("inReplyTo", nova_str(msgId));
        std::string refs = get(orig, "references");
        reply->obj->set("references", nova_str(refs.empty() ? msgId : refs + " " + msgId));

        // quoted body
        std::string origBody = get(orig, "body");
        std::string quoted;
        {
            std::istringstream ss(origBody);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back()=='\r') line.pop_back();
                quoted += "> " + line + "\n";
            }
        }
        // merge from spec
        if (spec && spec.isObject()) {
            for (auto& [k,v] : spec->obj->inner)
                reply->obj->set(k, v);
        }
        // prepend quoted body if body not overridden
        Val specBody = reply->obj->get("body");
        std::string newBody = specBody ? specBody.asString() + "\n\n" + quoted : quoted;
        reply->obj->set("body", nova_str(newBody));

        return reply; }, "Reply"));

            // ── Std.Email.Forward(originalEmail, forwardSpec) → emailObj ──────────
            email->obj->set("Forward", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
        if (a.size() < 2 || !a[0].isObject()) return nova_null();
        Val orig = a[0];
        Val spec = a[1];
        auto get = [](Val v, const std::string& k) -> std::string {
            Val x = v->obj->get(k);
            return x ? x.asString() : "";
        };

        auto fwd = nova_obj();
        std::string subj = get(orig, "subject");
        if (subj.substr(0,4) != "Fwd:") subj = "Fwd: " + subj;
        fwd->obj->set("subject", nova_str(subj));

        // build forwarded body header
        std::string fwdHeader =
            "---------- Forwarded message ---------\n"
            "From: "    + get(orig,"from")    + "\n"
            "Date: "    + get(orig,"date")    + "\n"
            "Subject: " + get(orig,"subject") + "\n\n";
        fwd->obj->set("body", nova_str(fwdHeader + get(orig, "body")));

        // copy attachments
        Val atts = orig->obj->get("attachments");
        if (atts) fwd->obj->set("attachments", atts);

        // merge spec
        if (spec && spec.isObject())
            for (auto& [k,v] : spec->obj->inner) fwd->obj->set(k, v);

        return fwd; }, "Forward"));

            // ── Std.Email.SMTP — class-like object: new SMTP session ──────────────
            // Usage:
            //   let smtp = Std.Email.SMTP({ host, port, username, password })
            //   smtp.send({ from, to, subject, body })
            //   smtp.verify()  → {ok, banner}
            email->obj->set("SMTP", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
        Val cfg = a.empty() ? nova_obj() : a[0];

        auto sessPtr = std::make_shared<SmtpSession>();
        sessPtr->host     = cfg->obj->get("host")     ? cfg->obj->get("host").asString()     : "localhost";
        sessPtr->port     = cfg->obj->get("port")     ? (int)cfg->obj->get("port").asNumber(): 587;
        sessPtr->username = cfg->obj->get("username") ? cfg->obj->get("username").asString() : "";
        sessPtr->password = cfg->obj->get("password") ? cfg->obj->get("password").asString() : "";
        sessPtr->timeoutMs= cfg->obj->get("timeout")  ? (int)cfg->obj->get("timeout").asNumber(): 10000;

        auto obj = nova_obj();
        obj->obj->set("__type__", nova_str("smtp_session"));
        obj->obj->set("host",    nova_str(sessPtr->host));
        obj->obj->set("port",    nova_num(sessPtr->port));
        obj->obj->set("username",nova_str(sessPtr->username));

        // .send(emailObj) → result
        obj->obj->set("send", NovaValue::makeNative([sessPtr](ValVec a, auto) -> Val {
            if (a.empty() || !a[0].isObject()) return nova_null();
            EmailSpec spec = specFromVal(a[0]);
            std::string raw = buildRaw(spec);
            std::vector<std::string> rcpts;
            for (auto& [n,addr] : spec.to)  if (!addr.empty()) rcpts.push_back(addr);
            for (auto& [n,addr] : spec.cc)  if (!addr.empty()) rcpts.push_back(addr);
            for (auto& [n,addr] : spec.bcc) if (!addr.empty()) rcpts.push_back(addr);
            SmtpResult r = smtpSend(*sessPtr, spec.fromAddr, rcpts, raw);
            auto res = nova_obj();
            res->obj->set("ok",       nova_bool(r.ok));
            res->obj->set("log",      nova_str(r.log));
            res->obj->set("response", nova_str(r.lastResponse));
            return res;
        }, "send"));

        // .sendRaw(from, rcpts[], rawMsg) → result
        obj->obj->set("sendRaw", NovaValue::makeNative([sessPtr](ValVec a, auto) -> Val {
            if (a.size() < 3) return nova_null();
            std::string from = a[0].asString();
            std::vector<std::string> rcpts;
            if (a[1].isArray())
                for (auto& v : a[1]->arr->inner) rcpts.push_back(v.asString());
            else
                rcpts.push_back(a[1].asString());
            SmtpResult r = smtpSend(*sessPtr, from, rcpts, a[2].asString());
            auto res = nova_obj();
            res->obj->set("ok",       nova_bool(r.ok));
            res->obj->set("log",      nova_str(r.log));
            res->obj->set("response", nova_str(r.lastResponse));
            return res;
        }, "sendRaw"));

        // .verify() → {ok, banner} — just connect & check greeting
        obj->obj->set("verify", NovaValue::makeNative([sessPtr](ValVec, auto) -> Val {
            novac::sock_platform_init();
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
            std::string portStr = std::to_string(sessPtr->port);
            auto result = nova_obj();
            if (getaddrinfo(sessPtr->host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
                result->obj->set("ok",     nova_bool(false));
                result->obj->set("banner", nova_str("DNS failed"));
                return result;
            }
            sock_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd == SOCK_INVALID) { freeaddrinfo(res); result->obj->set("ok",nova_bool(false)); return result; }
            bool connected = (::connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) == 0);
            freeaddrinfo(res);
            std::string banner;
            if (connected) {
                char buf[512]; int n = ::recv(fd, buf, sizeof(buf)-1, 0);
                if (n > 0) { buf[n]='\0'; banner = buf; }
                sock_close(fd);
            }
            result->obj->set("ok",     nova_bool(connected));
            result->obj->set("banner", nova_str(banner));
            return result;
        }, "verify"));

        // .configure(newCfg) → self (update settings)
        obj->obj->set("configure", NovaValue::makeNative([sessPtr, obj](ValVec a, auto) -> Val {
            if (a.empty() || !a[0].isObject()) return obj;
            Val c = a[0];
            if (c->obj->get("host"))     sessPtr->host     = c->obj->get("host").asString();
            if (c->obj->get("port"))     sessPtr->port     = (int)c->obj->get("port").asNumber();
            if (c->obj->get("username")) sessPtr->username = c->obj->get("username").asString();
            if (c->obj->get("password")) sessPtr->password = c->obj->get("password").asString();
            if (c->obj->get("timeout"))  sessPtr->timeoutMs= (int)c->obj->get("timeout").asNumber();
            obj->obj->set("host",    nova_str(sessPtr->host));
            obj->obj->set("port",    nova_num(sessPtr->port));
            obj->obj->set("username",nova_str(sessPtr->username));
            return obj;
        }, "configure"));

        return obj; }, "SMTP"));

            // ── Std.Email.Receive — POP3 client ───────────────────────────────────
            // Std.Email.Receive({ host, port?, username, password, count?, delete? })
            // Returns array of parsed email objects
            email->obj->set("Receive", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
        if (a.empty() || !a[0].isObject()) return nova_arr();
        Val cfg = a[0];

        std::string host = cfg->obj->get("host")     ? cfg->obj->get("host").asString()     : "localhost";
        int         port = cfg->obj->get("port")     ? (int)cfg->obj->get("port").asNumber(): 110;
        std::string user = cfg->obj->get("username") ? cfg->obj->get("username").asString() : "";
        std::string pass = cfg->obj->get("password") ? cfg->obj->get("password").asString() : "";
        int         count= cfg->obj->get("count")    ? (int)cfg->obj->get("count").asNumber(): 10;
        bool        del  = cfg->obj->get("delete")   ? cfg->obj->get("delete").asBool()      : false;

        novac::sock_platform_init();
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
        std::string portStr = std::to_string(port);
        if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
            return nova_arr();

        sock_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        freeaddrinfo(res);
        if (fd == SOCK_INVALID) return nova_arr();
        if (::connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
            sock_close(fd); return nova_arr();
        }

        novac::NovaSocket sock(fd);

        auto recvLine = [&fd]() -> std::string {
            std::string line;
            char c;
            while (::recv(fd, &c, 1, 0) == 1 && c != '\n') {
                if (c != '\r') line += c;
            }
            return line;
        };
        auto sendLine = [&sock](const std::string& line) {
            std::string l = line + "\r\n";
            sock.sendAll(l.c_str(), (int)l.size());
        };

        // greeting
        recvLine();
        sendLine("USER " + user);
        std::string r = recvLine();
        if (r.empty() || r[0] != '+') { sock.close(); return nova_arr(); }
        sendLine("PASS " + pass);
        r = recvLine();
        if (r.empty() || r[0] != '+') { sock.close(); return nova_arr(); }

        // STAT
        sendLine("STAT");
        r = recvLine();
        int total = 0;
        { std::istringstream ss(r.substr(4)); ss >> total; }
        int fetch = std::min(count, total);

        ValVec emails;
        for (int i = total - fetch + 1; i <= total; i++) {
            sendLine("RETR " + std::to_string(i));
            std::string resp = recvLine();
            if (resp.empty() || resp[0] != '+') continue;
            std::string raw;
            while (true) {
                std::string line = recvLine();
                if (line == ".") break;
                // un-dot-stuff
                if (!line.empty() && line[0] == '.') line = line.substr(1);
                raw += line + "\r\n";
            }
            emails.push_back(parsedToVal(parseRaw(raw)));
            if (del) {
                sendLine("DELE " + std::to_string(i));
                recvLine();
            }
        }

        sendLine("QUIT");
        sock.close();
        return nova_arr(emails); }, "Receive"));

            // ── Std.Email.POP3 — stateful POP3 session object ─────────────────────
            email->obj->set("POP3", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
        Val cfg = a.empty() ? nova_obj() : a[0];
        auto host = std::make_shared<std::string>(cfg->obj->get("host") ? cfg->obj->get("host").asString() : "localhost");
        auto port = std::make_shared<int>(cfg->obj->get("port") ? (int)cfg->obj->get("port").asNumber() : 110);
        auto user = std::make_shared<std::string>(cfg->obj->get("username") ? cfg->obj->get("username").asString() : "");
        auto pass = std::make_shared<std::string>(cfg->obj->get("password") ? cfg->obj->get("password").asString() : "");

        auto obj = nova_obj();
        obj->obj->set("__type__", nova_str("pop3_session"));

        // .list() → array of {id, size}
        obj->obj->set("list", NovaValue::makeNative([host, port, user, pass](ValVec, auto) -> Val {
            novac::sock_platform_init();
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
            std::string ps = std::to_string(*port);
            if (getaddrinfo(host->c_str(), ps.c_str(), &hints, &res) != 0 || !res)
                return nova_arr();
            sock_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd==SOCK_INVALID){freeaddrinfo(res);return nova_arr();}
            ::connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen);
            freeaddrinfo(res);
            novac::NovaSocket sock(fd);
            auto rl=[&fd]()->std::string{std::string l;char c;while(::recv(fd,&c,1,0)==1&&c!='\n'){if(c!='\r')l+=c;}return l;};
            auto sl=[&sock](const std::string& l){std::string f=l+"\r\n";sock.sendAll(f.c_str(),(int)f.size());};
            rl(); sl("USER "+*user); rl(); sl("PASS "+*pass); rl();
            sl("LIST");
            std::string r = rl();
            ValVec out;
            while (true) {
                std::string line = rl();
                if (line == ".") break;
                std::istringstream ss(line);
                int id, sz; ss >> id >> sz;
                auto o = nova_obj();
                o->obj->set("id", nova_num(id));
                o->obj->set("size", nova_num(sz));
                out.push_back(o);
            }
            sl("QUIT"); sock.close();
            return nova_arr(out);
        }, "list"));

        // .count() → number
        obj->obj->set("count", NovaValue::makeNative([host, port, user, pass](ValVec, auto) -> Val {
            novac::sock_platform_init();
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
            std::string ps = std::to_string(*port);
            if (getaddrinfo(host->c_str(), ps.c_str(), &hints, &res) != 0 || !res)
                return nova_num(0);
            sock_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd==SOCK_INVALID){freeaddrinfo(res);return nova_num(0);}
            ::connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen);
            freeaddrinfo(res);
            novac::NovaSocket sock(fd);
            auto rl=[&fd]()->std::string{std::string l;char c;while(::recv(fd,&c,1,0)==1&&c!='\n'){if(c!='\r')l+=c;}return l;};
            auto sl=[&sock](const std::string& l){std::string f=l+"\r\n";sock.sendAll(f.c_str(),(int)f.size());};
            rl(); sl("USER "+*user); rl(); sl("PASS "+*pass); rl();
            sl("STAT"); std::string r = rl();
            int total=0; std::istringstream ss(r.substr(r.size()>4?4:0)); ss >> total;
            sl("QUIT"); sock.close();
            return nova_num(total);
        }, "count"));

        return obj; }, "POP3"));

            // ── Std.Email.IsSpam(emailObj, opts?) → {score, reasons[]} ───────────
            // Heuristic-based spam scorer (no external API needed)
            email->obj->set("IsSpam", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                            {
        if (a.empty() || !a[0].isObject()) return nova_null();
        Val e = a[0];
        double score = 0.0;
        ValVec reasons;

        auto get = [&](const std::string& k) -> std::string {
            Val v = e->obj->get(k); return v ? v.asString() : "";
        };
        std::string subj = get("subject");
        std::string body = get("body");
        std::string from = get("fromAddr");

        // subject checks
        auto subjLow = subj; std::transform(subjLow.begin(),subjLow.end(),subjLow.begin(),::tolower);
        for (auto& kw : {"free","winner","prize","click here","urgent","limited","offer","buy now","cash","earn"}) {
            if (subjLow.find(kw) != std::string::npos) {
                score += 1.5;
                reasons.push_back(nova_str(std::string("subject contains: ") + kw));
            }
        }
        // ALL CAPS subject
        int upper=0, alpha=0;
        for (char c:subj){if(std::isupper(c))upper++;if(std::isalpha(c))alpha++;}
        if (alpha>5 && upper > alpha*0.7) { score+=2; reasons.push_back(nova_str("subject is mostly uppercase")); }

        // body checks
        auto bodyLow=body; std::transform(bodyLow.begin(),bodyLow.end(),bodyLow.begin(),::tolower);
        for (auto& kw : {"unsubscribe","click here","buy now","free money","guaranteed","no obligation","order now"}) {
            if (bodyLow.find(kw) != std::string::npos) {
                score += 1.0;
                reasons.push_back(nova_str(std::string("body contains: ") + kw));
            }
        }
        // excessive exclamation
        int excl = (int)std::count(body.begin(),body.end(),'!');
        if (excl > 3) { score += (double)excl * 0.3; reasons.push_back(nova_str("excessive exclamation marks")); }

        // from checks — missing domain
        if (from.find('@') == std::string::npos) { score+=3; reasons.push_back(nova_str("malformed from address")); }

        auto res = nova_obj();
        res->obj->set("score",   nova_num(score));
        res->obj->set("isSpam",  nova_bool(score >= 5.0));
        res->obj->set("reasons", nova_arr(reasons));
        return res; }, "IsSpam"));

            // ── Std.Email.Sanitize(emailObj) → emailObj (in-place clean) ──────────
            email->obj->set("Sanitize", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                              {
        if (a.empty() || !a[0].isObject()) return a.empty() ? nova_null() : a[0];
        Val e = a[0];
        // strip scripts from htmlBody
        Val html = e->obj->get("htmlBody");
        if (html && html.isString()) {
            std::string h = html.asString();
            // remove <script>...</script>
            while (true) {
                size_t s = h.find("<script"); if (s==std::string::npos) break;
                size_t end = h.find("</script>", s);
                if (end==std::string::npos) { h.erase(s); break; }
                h.erase(s, end+9-s);
            }
            // remove on* attributes  (simplified)
            std::regex onAttr(R"( on\w+="[^"]*")", std::regex::icase);
            h = std::regex_replace(h, onAttr, "");
            e->obj->set("htmlBody", nova_str(h));
        }
        return e; }, "Sanitize"));

            // ── Std.Email.ExtractLinks(emailObj) → array of URLs ──────────────────
            email->obj->set("ExtractLinks", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
        if (a.empty() || !a[0].isObject()) return nova_arr();
        std::string text;
        Val body = a[0]->obj->get("body");
        Val html = a[0]->obj->get("htmlBody");
        if (body) text += body.asString() + " ";
        if (html) text += html.asString();

        ValVec links;
        std::regex urlRe(R"((https?://[^\s"'<>]+))", std::regex::icase);
        auto it  = std::sregex_iterator(text.begin(), text.end(), urlRe);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) links.push_back(nova_str((*it)[1].str()));
        return nova_arr(links); }, "ExtractLinks"));

            // ── Std.Email.ExtractEmails(text) → array of address strings ──────────
            email->obj->set("ExtractEmails", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
        if (a.empty()) return nova_arr();
        std::string text = a[0].asString();
        ValVec out;
        std::regex addrRe(R"([a-zA-Z0-9._%+\-]+@[a-zA-Z0-9.\-]+\.[a-zA-Z]{2,})");
        auto it  = std::sregex_iterator(text.begin(), text.end(), addrRe);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) out.push_back(nova_str((*it)[0].str()));
        return nova_arr(out); }, "ExtractEmails"));

            // ── Std.Email.Thread(emails[]) → threads object ───────────────────────
            // Groups emails by message-id / in-reply-to into conversation threads
            email->obj->set("Thread", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                            {
        if (a.empty() || !a[0].isArray()) return nova_arr();
        auto& msgs = a[0]->arr->inner;

        // build id→msg map
        std::unordered_map<std::string,Val> byId;
        for (auto& m : msgs) {
            if (!m.isObject()) continue;
            Val mid = m->obj->get("messageId");
            if (mid) byId[mid.asString()] = m;
        }

        // build thread roots
        std::unordered_map<std::string,ValVec> threads;
        std::unordered_map<std::string,std::string> rootOf;

        for (auto& m : msgs) {
            if (!m.isObject()) continue;
            Val mid  = m->obj->get("messageId");
            Val irt  = m->obj->get("inReplyTo");
            std::string id  = mid ? mid.asString() : "";
            std::string irt_ = irt ? irt.asString() : "";

            if (irt_.empty() || byId.find(irt_) == byId.end()) {
                // root message
                threads[id].push_back(m);
                rootOf[id] = id;
            } else {
                // find root
                std::string root = irt_;
                while (rootOf.count(root) && rootOf[root] != root) root = rootOf[root];
                rootOf[id] = root;
                threads[root].push_back(m);
            }
        }

        ValVec result;
        for (auto& [root, msgs_] : threads) {
            auto t = nova_obj();
            t->obj->set("root",     byId.count(root) ? byId[root] : nova_null());
            t->obj->set("messages", nova_arr(msgs_));
            t->obj->set("count",    nova_num((double)msgs_.size()));
            result.push_back(t);
        }
        return nova_arr(result); }, "Thread"));

            // ── Std.Email.Mime — low-level MIME part builder ──────────────────────
            {
                auto mime = nova_obj();

                // Mime.Part(contentType, body, encoding?) → string (MIME part)
                mime->obj->set("Part", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
            std::string ct  = a.size()>0 ? a[0].asString() : "text/plain";
            std::string body= a.size()>1 ? a[1].asString() : "";
            std::string enc = a.size()>2 ? a[2].asString() : "quoted-printable";
            std::string part;
            part += "Content-Type: " + ct + "\r\n";
            part += "Content-Transfer-Encoding: " + enc + "\r\n\r\n";
            if      (enc == "base64")           part += base64Encode(body);
            else if (enc == "quoted-printable")  part += quotedPrintableEncode(body);
            else                                 part += body;
            return nova_str(part); }, "Part"));

                // Mime.Multipart(parts[], subtype?) → {boundary, raw}
                mime->obj->set("Multipart", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
            if (a.empty() || !a[0].isArray()) return nova_null();
            std::string subtype = a.size()>1 ? a[1].asString() : "mixed";
            std::string boundary = makeBoundary();
            std::string raw;
            raw += "Content-Type: multipart/" + subtype + "; boundary=\"" + boundary + "\"\r\n\r\n";
            for (auto& p : a[0]->arr->inner) {
                raw += "--" + boundary + "\r\n";
                raw += p.asString() + "\r\n";
            }
            raw += "--" + boundary + "--\r\n";
            auto res = nova_obj();
            res->obj->set("boundary", nova_str(boundary));
            res->obj->set("raw",      nova_str(raw));
            return res; }, "Multipart"));

                // Mime.Encode(str, encoding) → string
                mime->obj->set("Encode", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
            if (a.size() < 2) return a.empty() ? nova_str("") : a[0];
            std::string enc = a[1].asString();
            if      (enc == "base64")          return nova_str(base64Encode(a[0].asString()));
            else if (enc == "quoted-printable") return nova_str(quotedPrintableEncode(a[0].asString()));
            return a[0]; }, "Encode"));

                // Mime.Decode(str, encoding) → string
                mime->obj->set("Decode", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
                                                                   if (a.size() < 2)
                                                                       return a.empty() ? nova_str("") : a[0];
                                                                   std::string enc = a[1].asString();
                                                                   if (enc == "base64")
                                                                       return nova_str(base64Decode(a[0].asString()));
                                                                   return a[0]; // qp decode already done in parseRaw
                                                               },
                                                               "Decode"));

                email->obj->set("Mime", mime);
            }

            std_obj->obj->set("Email", email);
        }
        // ── Std.StringScanner ────────────────────────────────────────────────────
        {
            auto wrapScanner = std::make_shared<std::function<Val(std::shared_ptr<std::string>, size_t)>>();
            auto exe = this;

            *wrapScanner = [exe, wrapScanner](std::shared_ptr<std::string> str, size_t startPos) -> Val
            {
                auto pos = std::make_shared<size_t>(startPos);
                auto self = nova_obj();
                self->obj->set("__type__", nova_str("string_scanner"));

                self->obj->set("peek", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                             {
            size_t off = a.empty() ? 0 : (size_t)a[0].asNumber();
            size_t p = *pos + off;
            if (p >= str->size()) return nova_null();
            return nova_str(std::string(1, (*str)[p])); }, "peek"));

                self->obj->set("peekStr", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                                {
            size_t n = a.empty() ? 1 : (size_t)a[0].asNumber();
            if (*pos >= str->size()) return nova_str("");
            return nova_str(str->substr(*pos, n)); }, "peekStr"));

                self->obj->set("next", NovaValue::makeNative([str, pos](ValVec, auto) -> Val
                                                             {
            if (*pos >= str->size()) return nova_null();
            char c = (*str)[(*pos)++];
            return nova_str(std::string(1, c)); }, "next"));

                self->obj->set("advance", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                                {
            size_t n = a.empty() ? 1 : (size_t)a[0].asNumber();
            *pos = std::min(*pos + n, str->size());
            return nova_null(); }, "advance"));

                self->obj->set("eof", NovaValue::makeNative([str, pos](ValVec, auto) -> Val
                                                            { return nova_bool(*pos >= str->size()); }, "eof"));

                self->obj->set("pos", NovaValue::makeNative([pos](ValVec, auto) -> Val
                                                            { return nova_num((double)*pos); }, "pos"));

                self->obj->set("setPos", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                               {
            if (a.empty()) return nova_null();
            *pos = (size_t)std::max(0.0, std::min(a[0].asNumber(), (double)str->size()));
            return nova_null(); }, "setPos"));

                self->obj->set("length", NovaValue::makeNative([str](ValVec, auto) -> Val
                                                               { return nova_num((double)str->size()); }, "length"));

                self->obj->set("remaining", NovaValue::makeNative([str, pos](ValVec, auto) -> Val
                                                                  { return nova_num((double)(str->size() - std::min(*pos, str->size()))); }, "remaining"));

                self->obj->set("rest", NovaValue::makeNative([str, pos](ValVec, auto) -> Val
                                                             {
            if (*pos >= str->size()) return nova_str("");
            return nova_str(str->substr(*pos)); }, "rest"));

                self->obj->set("consumed", NovaValue::makeNative([str, pos](ValVec, auto) -> Val
                                                                 { return nova_str(str->substr(0, std::min(*pos, str->size()))); }, "consumed"));

                // match(literal) → bool, consumes on success
                self->obj->set("match", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                              {
            if (a.empty()) return nova_bool(false);
            std::string lit = a[0].asString();
            if (str->compare(*pos, lit.size(), lit) == 0) { *pos += lit.size(); return nova_bool(true); }
            return nova_bool(false); }, "match"));

                // matchCI(literal) → bool, case-insensitive
                self->obj->set("matchCI", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                                {
            if (a.empty()) return nova_bool(false);
            std::string lit = a[0].asString();
            if (*pos + lit.size() > str->size()) return nova_bool(false);
            for (size_t i = 0; i < lit.size(); i++)
                if (std::tolower((unsigned char)(*str)[*pos + i]) != std::tolower((unsigned char)lit[i]))
                    return nova_bool(false);
            *pos += lit.size();
            return nova_bool(true); }, "matchCI"));

                // expect(literal) → throws if not matched
                self->obj->set("expect", NovaValue::makeNative([exe, str, pos](ValVec a, auto) -> Val
                                                               {
            if (a.empty()) return nova_bool(false);
            std::string lit = a[0].asString();
            if (str->compare(*pos, lit.size(), lit) == 0) { *pos += lit.size(); return nova_bool(true); }
            exe->_error("StringScanner.expect: expected '" + lit + "' at position " + std::to_string(*pos));
            return nova_bool(false); }, "expect"));

                // matchRegex(pattern, flags?) → matched string or null, anchored at pos, consumes on match
                self->obj->set("matchRegex", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                                   {
            if (a.empty()) return nova_null();
            try {
                auto flags = std::regex_constants::ECMAScript;
                if (a.size() > 1 && a[1].isString() && a[1]->sval.find('i') != std::string::npos)
                    flags |= std::regex_constants::icase;
                std::regex re(a[0].asString(), flags);
                std::smatch m;
                auto begin = str->cbegin() + *pos;
                if (std::regex_search(begin, str->cend(), m, re, std::regex_constants::match_continuous)
                    && m.position(0) == 0 && m.length(0) > 0) {
                    std::string matched = m[0].str();
                    *pos += matched.size();
                    return nova_str(matched);
                }
            } catch (...) {}
            return nova_null(); }, "matchRegex"));

                self->obj->set("skipWhitespace", NovaValue::makeNative([str, pos](ValVec, auto) -> Val
                                                                       {
            size_t start = *pos;
            while (*pos < str->size() && std::isspace((unsigned char)(*str)[*pos])) (*pos)++;
            return nova_num((double)(*pos - start)); }, "skipWhitespace"));

                self->obj->set("skipWhile", NovaValue::makeNative([exe, str, pos](ValVec a, auto cs) -> Val
                                                                  {
            if (a.empty() || !a[0].isFunction()) return nova_num(0);
            size_t start = *pos;
            while (*pos < str->size()) {
                if (!exe->callFunction(a[0], {nova_str(std::string(1, (*str)[*pos]))}, cs).asBool()) break;
                (*pos)++;
            }
            return nova_num((double)(*pos - start)); }, "skipWhile"));

                self->obj->set("takeWhile", NovaValue::makeNative([exe, str, pos](ValVec a, auto cs) -> Val
                                                                  {
            if (a.empty() || !a[0].isFunction()) return nova_str("");
            size_t start = *pos;
            while (*pos < str->size()) {
                if (!exe->callFunction(a[0], {nova_str(std::string(1, (*str)[*pos]))}, cs).asBool()) break;
                (*pos)++;
            }
            return nova_str(str->substr(start, *pos - start)); }, "takeWhile"));

                // readUntil(literal) → consumes/returns text up to (not incl.) literal; to end if absent
                self->obj->set("readUntil", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                                  {
            if (a.empty()) return nova_str("");
            std::string lit = a[0].asString();
            size_t found = str->find(lit, *pos);
            std::string out;
            if (found == std::string::npos) { out = str->substr(*pos); *pos = str->size(); }
            else { out = str->substr(*pos, found - *pos); *pos = found; }
            return nova_str(out); }, "readUntil"));

                // readWhile(literal) → consumes/returns text while matching (not incl.) literal
                self->obj->set("readWhile", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                                  {
            if (a.empty()) return nova_str("");
            std::string lit = a[0].asString();
            size_t found = str->find(lit, *pos);
            std::string out;
            if (!(found == std::string::npos)) { out = str->substr(*pos); *pos = str->size(); }
            else { out = str->substr(*pos, found - *pos); *pos = found; }
            return nova_str(out); }, "readUntil"));

                self->obj->set("readLine", NovaValue::makeNative([str, pos](ValVec, auto) -> Val
                                                                 {
            size_t nl = str->find('\n', *pos);
            std::string out;
            if (nl == std::string::npos) { out = str->substr(*pos); *pos = str->size(); }
            else {
                out = str->substr(*pos, nl - *pos);
                if (!out.empty() && out.back() == '\r') out.pop_back();
                *pos = nl + 1;
            }
            return nova_str(out); }, "readLine"));

                self->obj->set("readN", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                              {
            size_t n = a.empty() ? 0 : (size_t)a[0].asNumber();
            std::string out = str->substr(*pos, n);
            *pos = std::min(*pos + n, str->size());
            return nova_str(out); }, "readN"));

                self->obj->set("lineCol", NovaValue::makeNative([str, pos](ValVec, auto) -> Val
                                                                {
            int line = 1, col = 1;
            for (size_t i = 0; i < *pos && i < str->size(); i++) {
                if ((*str)[i] == '\n') { line++; col = 1; } else col++;
            }
            auto res = nova_obj();
            res->obj->set("line", nova_num(line));
            res->obj->set("col", nova_num(col));
            return res; }, "lineCol"));

                self->obj->set("save", NovaValue::makeNative([pos](ValVec, auto) -> Val
                                                             { return nova_num((double)*pos); }, "save"));

                self->obj->set("restore", NovaValue::makeNative([str, pos](ValVec a, auto) -> Val
                                                                {
            if (!a.empty()) *pos = (size_t)std::max(0.0, std::min(a[0].asNumber(), (double)str->size()));
            return nova_null(); }, "restore"));

                self->obj->set("clone", NovaValue::makeNative([str, pos, wrapScanner](ValVec, auto) -> Val
                                                              { return (*wrapScanner)(str, *pos); }, "clone"));

                self->obj->set("toString", NovaValue::makeNative([str](ValVec, auto) -> Val
                                                                 { return nova_str(*str); }, "toString"));

                return self;
            };

            auto scannerNs = nova_obj();
            scannerNs->obj->set("new", NovaValue::makeNative([wrapScanner](ValVec a, auto) -> Val
                                                             {
        std::string s = a.empty() ? "" : a[0].asString();
        return (*wrapScanner)(std::make_shared<std::string>(s), 0); }, "new"));
            std_obj->obj->set("StringScanner", scannerNs);
        }
        // ── Std.StringBuilder ────────────────────────────────────────────────────
        {
            auto wrapBuilder = std::make_shared<std::function<Val(std::shared_ptr<std::string>)>>();
            auto exe = this;

            *wrapBuilder = [exe](std::shared_ptr<std::string> buf) -> Val
            {
                auto self = nova_obj();
                self->obj->set("__type__", nova_str("string_builder"));

                self->obj->set("append", NovaValue::makeNative([exe, buf, self](ValVec a, auto) -> Val
                                                               {
            for (auto& v : a) *buf += exe->stringify(v);
            return self; }, "append"));

                self->obj->set("appendLine", NovaValue::makeNative([exe, buf, self](ValVec a, auto) -> Val
                                                                   {
            for (auto& v : a) *buf += exe->stringify(v);
            *buf += '\n';
            return self; }, "appendLine"));

                self->obj->set("appendChar", NovaValue::makeNative([buf, self](ValVec a, auto) -> Val
                                                                   {
            if (!a.empty()) {
                uint32_t code = (uint32_t)(int)a[0].asNumber();
                if      (code < 0x80)    *buf += (char)code;
                else if (code < 0x800)   { *buf += (char)(0xC0|(code>>6)); *buf += (char)(0x80|(code&0x3F)); }
                else if (code < 0x10000) { *buf += (char)(0xE0|(code>>12)); *buf += (char)(0x80|((code>>6)&0x3F)); *buf += (char)(0x80|(code&0x3F)); }
                else                     { *buf += (char)(0xF0|(code>>18)); *buf += (char)(0x80|((code>>12)&0x3F)); *buf += (char)(0x80|((code>>6)&0x3F)); *buf += (char)(0x80|(code&0x3F)); }
            }
            return self; }, "appendChar"));

                self->obj->set("prepend", NovaValue::makeNative([exe, buf, self](ValVec a, auto) -> Val
                                                                {
            if (!a.empty()) *buf = exe->stringify(a[0]) + *buf;
            return self; }, "prepend"));

                self->obj->set("insert", NovaValue::makeNative([exe, buf, self](ValVec a, auto) -> Val
                                                               {
            if (a.size() < 2) return self;
            size_t p = (size_t)std::max(0.0, std::min(a[0].asNumber(), (double)buf->size()));
            buf->insert(p, exe->stringify(a[1]));
            return self; }, "insert"));

                self->obj->set("remove", NovaValue::makeNative([buf, self](ValVec a, auto) -> Val
                                                               {
            if (a.empty()) return self;
            size_t start = (size_t)std::max(0.0, a[0].asNumber());
            size_t end   = a.size() > 1 ? (size_t)a[1].asNumber() : buf->size();
            start = std::min(start, buf->size()); end = std::min(end, buf->size());
            if (start < end) buf->erase(start, end - start);
            return self; }, "remove"));

                self->obj->set("clear", NovaValue::makeNative([buf, self](ValVec, auto) -> Val
                                                              {
            buf->clear(); return self; }, "clear"));

                self->obj->set("length", NovaValue::makeNative([buf](ValVec, auto) -> Val
                                                               { return nova_num((double)utf8::length(*buf)); }, "length"));

                self->obj->set("byteLength", NovaValue::makeNative([buf](ValVec, auto) -> Val
                                                                   { return nova_num((double)buf->size()); }, "byteLength"));

                self->obj->set("isEmpty", NovaValue::makeNative([buf](ValVec, auto) -> Val
                                                                { return nova_bool(buf->empty()); }, "isEmpty"));

                self->obj->set("charAt", NovaValue::makeNative([buf](ValVec a, auto) -> Val
                                                               {
            if (a.empty()) return nova_str("");
            return nova_str(utf8::at(*buf, (int)a[0].asNumber())); }, "charAt"));

                self->obj->set("indexOf", NovaValue::makeNative([buf](ValVec a, auto) -> Val
                                                                {
            if (a.empty()) return nova_num(-1);
            auto p = buf->find(a[0].asString());
            return nova_num(p == std::string::npos ? -1.0 : (double)p); }, "indexOf"));

                self->obj->set("contains", NovaValue::makeNative([buf](ValVec a, auto) -> Val
                                                                 { return nova_bool(!a.empty() && buf->find(a[0].asString()) != std::string::npos); }, "contains"));

                self->obj->set("replace", NovaValue::makeNative([buf, self](ValVec a, auto) -> Val
                                                                {
            if (a.size() < 2) return self;
            std::string from = a[0].asString(), to = a[1].asString();
            if (from.empty()) return self;
            size_t pos = 0;
            while ((pos = buf->find(from, pos)) != std::string::npos) { buf->replace(pos, from.size(), to); pos += to.size(); }
            return self; }, "replace"));

                self->obj->set("reverse", NovaValue::makeNative([buf, self](ValVec, auto) -> Val
                                                                {
            auto cps = utf8::codepoints(*buf);
            std::reverse(cps.begin(), cps.end());
            std::string out; for (auto& cp : cps) out += cp;
            *buf = out;
            return self; }, "reverse"));

                self->obj->set("repeatAppend", NovaValue::makeNative([buf, self](ValVec a, auto) -> Val
                                                                     {
            if (a.size() < 2) return self;
            std::string s = a[0].asString();
            int n = (int)a[1].asNumber();
            for (int i = 0; i < n; i++) *buf += s;
            return self; }, "repeatAppend"));

                self->obj->set("padStart", NovaValue::makeNative([buf, self](ValVec a, auto) -> Val
                                                                 {
            if (a.empty()) return self;
            int w = (int)a[0].asNumber();
            std::string p = a.size() > 1 ? a[1].asString() : " ";
            *buf = utf8::padStart(*buf, w, p);
            return self; }, "padStart"));

                self->obj->set("padEnd", NovaValue::makeNative([buf, self](ValVec a, auto) -> Val
                                                               {
            if (a.empty()) return self;
            int w = (int)a[0].asNumber();
            std::string p = a.size() > 1 ? a[1].asString() : " ";
            *buf = utf8::padEnd(*buf, w, p);
            return self; }, "padEnd"));

                self->obj->set("join", NovaValue::makeNative([exe, buf, self](ValVec a, auto) -> Val
                                                             {
            if (a.empty() || !a[0].isArray()) return self;
            std::string sep = a.size() > 1 ? a[1].asString() : "";
            auto& inner = a[0]->arr->inner;
            for (size_t i = 0; i < inner.size(); i++) {
                if (i) *buf += sep;
                *buf += exe->stringify(inner[i]);
            }
            return self; }, "join"));

                self->obj->set("trim", NovaValue::makeNative([buf, self](ValVec, auto) -> Val
                                                             {
            size_t l = buf->find_first_not_of(" \t\r\n");
            size_t r = buf->find_last_not_of(" \t\r\n");
            *buf = (l == std::string::npos) ? "" : buf->substr(l, r - l + 1);
            return self; }, "trim"));

                self->obj->set("slice", NovaValue::makeNative([buf](ValVec a, auto) -> Val
                                                              {
            int sz = (int)utf8::length(*buf);
            int start = a.size() > 0 ? (int)a[0].asNumber() : 0;
            int end   = a.size() > 1 ? (int)a[1].asNumber() : sz;
            if (start < 0) start = std::max(0, sz + start);
            if (end < 0)   end   = std::max(0, sz + end);
            start = std::min(start, sz); end = std::min(end, sz);
            return nova_str(utf8::substr(*buf, start, std::max(start, end))); }, "slice"));

                self->obj->set("toString", NovaValue::makeNative([buf](ValVec, auto) -> Val
                                                                 { return nova_str(*buf); }, "toString"));

                self->obj->set("toArray", NovaValue::makeNative([buf](ValVec, auto) -> Val
                                                                {
            ValVec out;
            for (auto& cp : utf8::codepoints(*buf)) out.push_back(nova_str(cp));
            return nova_arr(std::move(out)); }, "toArray"));

                return self;
            };

            auto sbNs = nova_obj();
            sbNs->obj->set("new", NovaValue::makeNative([wrapBuilder](ValVec a, auto) -> Val
                                                        {
        std::string init = a.empty() ? "" : a[0].asString();
        return (*wrapBuilder)(std::make_shared<std::string>(init)); }, "new"));
            std_obj->obj->set("StringBuilder", sbNs);
        }
        // ── Std.Lexer ────────────────────────────────────────────────────────────
        {
            struct LexQuoteRule
            {
                std::string type, open, close, escape;
            };
            struct LexTokenRule
            {
                std::string type;
                std::regex re;
                bool skip;
            };
            struct LexBlockComment
            {
                std::string open, close;
            };

            auto wrapLexer = std::make_shared<std::function<Val(Val)>>();
            auto exe = this;

            *wrapLexer = [exe](Val initialCfg) -> Val
            {
                auto quotes = std::make_shared<std::vector<LexQuoteRule>>();
                auto rules = std::make_shared<std::vector<LexTokenRule>>();
                auto blockComments = std::make_shared<std::vector<LexBlockComment>>();
                auto lineComments = std::make_shared<std::vector<std::string>>();
                auto keywords = std::make_shared<std::unordered_map<std::string, bool>>();
                auto keywordType = std::make_shared<std::string>("keyword");
                auto wsPattern = std::make_shared<std::string>("\\s+");
                auto wsEnabled = std::make_shared<bool>(true);
                auto unknownAsError = std::make_shared<bool>(false);

                auto applyConfig = std::make_shared<std::function<void(Val)>>();
                *applyConfig = [=](Val cfg)
                {
                    quotes->clear();
                    rules->clear();
                    blockComments->clear();
                    lineComments->clear();
                    keywords->clear();
                    *wsEnabled = true;
                    *wsPattern = "\\s+";
                    *keywordType = "keyword";
                    *unknownAsError = false;
                    if (!cfg || !cfg.isObject())
                        return;

                    Val ws = cfg->obj->get("whitespace");
                    if (ws)
                    {
                        if (ws.isBool())
                            *wsEnabled = ws.asBool();
                        else if (ws.isString())
                            *wsPattern = ws->sval;
                    }

                    Val lc = cfg->obj->get("lineComment");
                    if (lc)
                    {
                        if (lc.isString())
                            lineComments->push_back(lc->sval);
                        else if (lc.isArray())
                            for (auto &v : lc->arr->inner)
                                if (v.isString())
                                    lineComments->push_back(v->sval);
                    }

                    auto addBC = [&](Val v)
                    {
                        if (!v || !v.isObject())
                            return;
                        Val o = v->obj->get("open");
                        Val c = v->obj->get("close");
                        blockComments->push_back({o ? o.asString() : "", c ? c.asString() : ""});
                    };
                    Val bc = cfg->obj->get("blockComment");
                    if (bc)
                    {
                        if (bc.isArray())
                            for (auto &v : bc->arr->inner)
                                addBC(v);
                        else
                            addBC(bc);
                    }

                    Val qs = cfg->obj->get("quotes");
                    if (qs && qs.isArray())
                    {
                        for (auto &v : qs->arr->inner)
                        {
                            if (!v.isObject())
                                continue;
                            LexQuoteRule qr;
                            Val t = v->obj->get("type");
                            qr.type = t ? t.asString() : "string";
                            Val o = v->obj->get("open");
                            qr.open = o ? o.asString() : "\"";
                            Val c = v->obj->get("close");
                            qr.close = c ? c.asString() : qr.open;
                            Val e = v->obj->get("escape");
                            qr.escape = e ? e.asString() : "\\";
                            quotes->push_back(qr);
                        }
                    }

                    Val rs = cfg->obj->get("rules");
                    if (rs && rs.isArray())
                    {
                        for (auto &v : rs->arr->inner)
                        {
                            if (!v.isObject())
                                continue;
                            Val t = v->obj->get("type");
                            Val p = v->obj->get("pattern");
                            if (!p || !p.isString())
                                continue;
                            try
                            {
                                auto flags = std::regex_constants::ECMAScript;
                                Val fl = v->obj->get("flags");
                                if (fl && fl.isString() && fl->sval.find('i') != std::string::npos)
                                    flags |= std::regex_constants::icase;
                                LexTokenRule tr;
                                tr.type = t ? t.asString() : "token";
                                tr.re = std::regex(p->sval, flags);
                                Val sk = v->obj->get("skip");
                                tr.skip = sk && sk.asBool();
                                rules->push_back(std::move(tr));
                            }
                            catch (...)
                            {
                            }
                        }
                    }

                    Val kw = cfg->obj->get("keywords");
                    if (kw && kw.isArray())
                        for (auto &v : kw->arr->inner)
                            if (v.isString())
                                (*keywords)[v->sval] = true;

                    Val kt = cfg->obj->get("keywordType");
                    if (kt && kt.isString())
                        *keywordType = kt->sval;

                    Val ue = cfg->obj->get("unknownAsError");
                    if (ue)
                        *unknownAsError = ue.asBool();
                };
                (*applyConfig)(initialCfg);

                auto tokenizeFn = std::make_shared<std::function<Val(const std::string &)>>();
                *tokenizeFn = [=](const std::string &src) -> Val
                {
                    ValVec tokens;
                    size_t pos = 0, len = src.size();
                    int line = 1, col = 1;

                    auto advanceTracking = [&](size_t n)
                    {
                        for (size_t i = 0; i < n && pos + i < len; i++)
                        {
                            if (src[pos + i] == '\n')
                            {
                                line++;
                                col = 1;
                            }
                            else
                                col++;
                        }
                        pos += n;
                    };

                    std::regex wsRe;
                    bool wsOk = false;
                    if (*wsEnabled)
                    {
                        try
                        {
                            wsRe = std::regex(*wsPattern);
                            wsOk = true;
                        }
                        catch (...)
                        {
                        }
                    }

                    while (pos < len)
                    {
                        // whitespace
                        if (wsOk)
                        {
                            std::smatch m;
                            auto begin = src.cbegin() + pos;
                            if (std::regex_search(begin, src.cend(), m, wsRe, std::regex_constants::match_continuous) && m.position(0) == 0 && m.length(0) > 0)
                            {
                                advanceTracking(m.length(0));
                                continue;
                            }
                        }

                        // line comments
                        {
                            bool didLC = false;
                            for (auto &lc : *lineComments)
                            {
                                if (!lc.empty() && src.compare(pos, lc.size(), lc) == 0)
                                {
                                    size_t nl = src.find('\n', pos);
                                    advanceTracking((nl == std::string::npos ? len : nl) - pos);
                                    didLC = true;
                                    break;
                                }
                            }
                            if (didLC)
                                continue;
                        }

                        // block comments
                        {
                            bool didBC = false;
                            for (auto &bc : *blockComments)
                            {
                                if (!bc.open.empty() && src.compare(pos, bc.open.size(), bc.open) == 0)
                                {
                                    size_t closePos = src.find(bc.close, pos + bc.open.size());
                                    size_t end = (closePos == std::string::npos) ? len : closePos + bc.close.size();
                                    advanceTracking(end - pos);
                                    didBC = true;
                                    break;
                                }
                            }
                            if (didBC)
                                continue;
                        }

                        if (pos >= len)
                            break;

                        // quotes
                        {
                            bool didQuote = false;
                            for (auto &q : *quotes)
                            {
                                if (!q.open.empty() && src.compare(pos, q.open.size(), q.open) == 0)
                                {
                                    size_t startPos = pos;
                                    int startLine = line, startCol = col;
                                    advanceTracking(q.open.size());
                                    std::string content;
                                    bool closed = false;
                                    while (pos < len)
                                    {
                                        if (!q.escape.empty() && src.compare(pos, q.escape.size(), q.escape) == 0 && pos + q.escape.size() < len)
                                        {
                                            char c = src[pos + q.escape.size()];
                                            char decoded;
                                            switch (c)
                                            {
                                            case 'n':
                                                decoded = '\n';
                                                break;
                                            case 't':
                                                decoded = '\t';
                                                break;
                                            case 'r':
                                                decoded = '\r';
                                                break;
                                            case '0':
                                                decoded = '\0';
                                                break;
                                            default:
                                                decoded = c;
                                                break;
                                            }
                                            content += decoded;
                                            advanceTracking(q.escape.size() + 1);
                                            continue;
                                        }
                                        if (src.compare(pos, q.close.size(), q.close) == 0)
                                        {
                                            advanceTracking(q.close.size());
                                            closed = true;
                                            break;
                                        }
                                        content += src[pos];
                                        advanceTracking(1);
                                    }
                                    auto tok = nova_obj();
                                    tok->obj->set("type", nova_str(q.type));
                                    tok->obj->set("value", nova_str(content));
                                    tok->obj->set("raw", nova_str(src.substr(startPos, pos - startPos)));
                                    tok->obj->set("pos", nova_num((double)startPos));
                                    tok->obj->set("line", nova_num(startLine));
                                    tok->obj->set("col", nova_num(startCol));
                                    tok->obj->set("closed", nova_bool(closed));
                                    tokens.push_back(tok);
                                    didQuote = true;
                                    break;
                                }
                            }
                            if (didQuote)
                                continue;
                        }

                        // ordered token rules
                        {
                            bool didRule = false;
                            for (auto &r : *rules)
                            {
                                std::smatch m;
                                auto begin = src.cbegin() + pos;
                                if (std::regex_search(begin, src.cend(), m, r.re, std::regex_constants::match_continuous) && m.position(0) == 0 && m.length(0) > 0)
                                {
                                    std::string val = m[0].str();
                                    size_t startPos = pos;
                                    int startLine = line, startCol = col;
                                    advanceTracking(val.size());
                                    if (!r.skip)
                                    {
                                        std::string type = keywords->count(val) ? *keywordType : r.type;
                                        auto tok = nova_obj();
                                        tok->obj->set("type", nova_str(type));
                                        tok->obj->set("value", nova_str(val));
                                        tok->obj->set("pos", nova_num((double)startPos));
                                        tok->obj->set("line", nova_num(startLine));
                                        tok->obj->set("col", nova_num(startCol));
                                        tokens.push_back(tok);
                                    }
                                    didRule = true;
                                    break;
                                }
                            }
                            if (didRule)
                                continue;
                        }

                        // nothing matched
                        if (*unknownAsError)
                            exe->_error("Lexer: unexpected character '" + std::string(1, src[pos]) +
                                        "' at line " + std::to_string(line) + ", col " + std::to_string(col));
                        {
                            int startLine = line, startCol = col;
                            size_t startPos = pos;
                            std::string val(1, src[pos]);
                            advanceTracking(1);
                            auto tok = nova_obj();
                            tok->obj->set("type", nova_str("unknown"));
                            tok->obj->set("value", nova_str(val));
                            tok->obj->set("pos", nova_num((double)startPos));
                            tok->obj->set("line", nova_num(startLine));
                            tok->obj->set("col", nova_num(startCol));
                            tokens.push_back(tok);
                        }
                    }

                    auto eofTok = nova_obj();
                    eofTok->obj->set("type", nova_str("eof"));
                    eofTok->obj->set("value", nova_str(""));
                    eofTok->obj->set("pos", nova_num((double)pos));
                    eofTok->obj->set("line", nova_num(line));
                    eofTok->obj->set("col", nova_num(col));
                    tokens.push_back(eofTok);

                    return nova_arr(std::move(tokens));
                };

                auto self = nova_obj();
                self->obj->set("__type__", nova_str("lexer"));

                self->obj->set("tokenize", NovaValue::makeNative([tokenizeFn](ValVec a, auto) -> Val
                                                                 { return (*tokenizeFn)(a.empty() ? "" : a[0].asString()); }, "tokenize"));

                self->obj->set("setConfig", NovaValue::makeNative([applyConfig, self](ValVec a, auto) -> Val
                                                                  {
            (*applyConfig)(a.empty() ? nova_null() : a[0]);
            return self; }, "setConfig"));

                return self;
            };

            auto lexerNs = nova_obj();
            lexerNs->obj->set("new", NovaValue::makeNative([wrapLexer](ValVec a, auto) -> Val
                                                           { return (*wrapLexer)(a.empty() ? nova_null() : a[0]); }, "new"));

            // one-shot: Std.Lexer.tokenize(config, source)
            lexerNs->obj->set("tokenize", NovaValue::makeNative([wrapLexer, exe](ValVec a, auto cs) -> Val
                                                                {
        if (a.size() < 2) return nova_arr();
        Val lex = (*wrapLexer)(a[0]);
        Val tokFn = lex->obj->get("tokenize");
        return exe->callFunction(tokFn, {a[1]}, cs); }, "tokenize"));

            std_obj->obj->set("Lexer", lexerNs);
        }

        // ── Std.Reflect / Std.Proxy / Std.Set / Std.Map / Std.WeakMap / Std.WeakSet ──
        {
            auto exe = this;

            // ── Std.Reflect ───────────────────────────────────────────────────────
            {
                auto refl = nova_obj();

                refl->obj->set("get", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                            {
                    if (a.size() < 2) return nova_null();
                    return exe->_getProp(a[0], a[1].asString(), cs, true); }, "get"));

                refl->obj->set("set", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                            {
                    if (a.size() < 3) return nova_bool(false);
                    exe->_setProp(a[0], a[1].asString(), a[2]);
                    return nova_bool(true); }, "set"));

                refl->obj->set("has", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                            {
                    if (a.size() < 2 || !a[0]) return nova_bool(false);
                    if (a[0].isObject()) return nova_bool(a[0]->obj->has(a[1].asString()));
                    if (a[0].isStruct()) return nova_bool(a[0]->strct->get(a[1].asString()) != nullptr);
                    return nova_bool(false); }, "has"));

                refl->obj->set("deleteProperty", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    if (a.size() < 2 || !a[0] || !a[0].isObject()) return nova_bool(false);
                    a[0]->obj->del(a[1].asString());
                    return nova_bool(true); }, "deleteProperty"));

                refl->obj->set("ownKeys", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                {
                    if (a.empty() || !a[0]) return nova_arr();
                    ValVec out;
                    if (a[0].isObject())      for (auto& k : a[0]->obj->keys()) out.push_back(nova_str(k));
                    else if (a[0].isStruct()) for (auto& [k,_] : a[0]->strct->inner) out.push_back(nova_str(k));
                    return nova_arr(std::move(out)); }, "ownKeys"));

                refl->obj->set("construct", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                                  {
                    if (a.empty() || !a[0].isClass()) return nova_null();
                    ValVec args;
                    if (a.size() > 1 && a[1].isArray()) args = a[1]->arr->inner;
                    return exe->_instantiateClass(a[0], args); }, "construct"));

                refl->obj->set("apply", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                              {
                    if (a.empty() || !a[0].isFunction()) return nova_null();
                    Val thisArg = a.size() > 1 ? a[1] : nova_null();
                    ValVec args;
                    if (a.size() > 2 && a[2].isArray()) args = a[2]->arr->inner;
                    return exe->callFunction(a[0], args, cs, thisArg); }, "apply"));

                refl->obj->set("defineProperty", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    if (a.size() < 3 || !a[0] || !a[0].isObject() || !a[2].isObject()) return nova_bool(false);
                    Val v = a[2]->obj->get("value");
                    a[0]->obj->set(a[1].asString(), v ? v : nova_null());
                    return nova_bool(true); }, "defineProperty"));

                refl->obj->set("getPrototypeOf", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    if (a.empty() || !a[0] || !a[0].isObject()) return nova_null();
                    Val cls = a[0]->obj->get("__class__");
                    return cls ? cls : nova_null(); }, "getPrototypeOf"));

                refl->obj->set("isExtensible", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                     { return nova_bool(true); }, "isExtensible"));

                std_obj->obj->set("Reflect", refl);
            }

            // ── Std.Proxy ─────────────────────────────────────────────────────────
            // Traps both proxy["key"] (subscript:get/set) and proxy.key (prop:get/set,
            // requires the _getProp/_setProp patches above) so dot access is fully
            // interceptable, not just bracket access.
            {
                auto proxyNs = nova_obj();

                proxyNs->obj->set("new", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                               {
                    if (a.empty())
                        exe->_error("Proxy.new: expected a target object");
                    Val target  = a[0];
                    Val handler = (a.size() > 1 && a[1].isObject()) ? a[1] : nova_obj();

                    Val getTrap = handler->obj->get("get");
                    Val setTrap = handler->obj->get("set");
                    Val hasTrap = handler->obj->get("has");
                    Val delTrap = handler->obj->get("deleteProperty");

                    auto self = nova_obj();
                    self->obj->set("__type__",    nova_str("proxy"));
                    self->obj->set("__target__",  target);
                    self->obj->set("__handler__", handler);

                    self->obj->set("get", NovaValue::makeNative([exe, target, getTrap](ValVec a, auto cs) -> Val {
                        std::string key = a.empty() ? "" : a[0].asString();
                        if (getTrap && getTrap.isFunction())
                            return exe->callFunction(getTrap, {target, nova_str(key)}, cs);
                        return exe->_getProp(target, key, cs, true);
                    }, "get"));

                    self->obj->set("set", NovaValue::makeNative([exe, target, setTrap](ValVec a, auto cs) -> Val {
                        if (a.size() < 2) return nova_bool(false);
                        std::string key = a[0].asString();
                        if (setTrap && setTrap.isFunction())
                            exe->callFunction(setTrap, {target, nova_str(key), a[1]}, cs);
                        else
                            exe->_setProp(target, key, a[1]);
                        return nova_bool(true);
                    }, "set"));

                    self->obj->set("has", NovaValue::makeNative([exe, target, hasTrap](ValVec a, auto cs) -> Val {
                        std::string key = a.empty() ? "" : a[0].asString();
                        if (hasTrap && hasTrap.isFunction())
                            return exe->callFunction(hasTrap, {target, nova_str(key)}, cs);
                        return nova_bool(target.isObject() && target->obj->has(key));
                    }, "has"));

                    self->obj->set("deleteProperty", NovaValue::makeNative([exe, target, delTrap](ValVec a, auto cs) -> Val {
                        std::string key = a.empty() ? "" : a[0].asString();
                        if (delTrap && delTrap.isFunction())
                            return exe->callFunction(delTrap, {target, nova_str(key)}, cs);
                        if (target.isObject()) { target->obj->del(key); return nova_bool(true); }
                        return nova_bool(false);
                    }, "deleteProperty"));

                    if (!self->overloads) self->overloads = std::make_shared<ValMap>();

                    // bracket-notation: proxy["key"], proxy["key"] = v
                    (*self->overloads)["subscript:get"] = NovaValue::makeNative(
                        [exe, target, getTrap](ValVec a, auto cs) -> Val {
                            std::string key = a.size() > 1 ? a[1].asString() : "";
                            if (getTrap && getTrap.isFunction())
                                return exe->callFunction(getTrap, {target, nova_str(key)}, cs);
                            return exe->_getProp(target, key, cs, true);
                        }, "subscript:get");

                    (*self->overloads)["subscript:set"] = NovaValue::makeNative(
                        [exe, target, setTrap](ValVec a, auto cs) -> Val {
                            if (a.size() < 3) return nova_null();
                            std::string key = a[1].asString();
                            if (setTrap && setTrap.isFunction())
                                exe->callFunction(setTrap, {target, nova_str(key), a[2]}, cs);
                            else
                                exe->_setProp(target, key, a[2]);
                            return a[2];
                        }, "subscript:set");

                    // dot-notation: proxy.key, proxy.key = v  (needs the
                    // _getProp / _setProp patches checking prop:get/prop:set)
                    (*self->overloads)["prop:get"] = NovaValue::makeNative(
                        [exe, target, getTrap](ValVec a, auto cs) -> Val {
                            std::string key = a.size() > 1 ? a[1].asString() : "";
                            if (getTrap && getTrap.isFunction())
                                return exe->callFunction(getTrap, {target, nova_str(key)}, cs);
                            return exe->_getProp(target, key, cs, true);
                        }, "prop:get");

                    (*self->overloads)["prop:set"] = NovaValue::makeNative(
                        [exe, target, setTrap](ValVec a, auto cs) -> Val {
                            if (a.size() < 3) return nova_null();
                            std::string key = a[1].asString();
                            if (setTrap && setTrap.isFunction())
                                exe->callFunction(setTrap, {target, nova_str(key), a[2]}, cs);
                            else
                                exe->_setProp(target, key, a[2]);
                            return a[2];
                        }, "prop:set");

                    return self; }, "new"));

                std_obj->obj->set("Proxy", proxyNs);
            }

            // ── Std.Set ───────────────────────────────────────────────────────────
            {
                auto wrapSet = std::make_shared<std::function<Val(std::shared_ptr<ValVec>)>>();

                *wrapSet = [exe, wrapSet](std::shared_ptr<ValVec> data) -> Val
                {
                    auto self = nova_obj();
                    self->obj->set("__type__", nova_str("set"));

                    self->obj->set("add", NovaValue::makeNative([data, self](ValVec a, auto) -> Val
                                                                {
                        if (a.empty()) return self;
                        for (auto& v : *data) if (*v == *a[0]) return self;
                        data->push_back(a[0]);
                        return self; }, "add"));

                    self->obj->set("has", NovaValue::makeNative([data](ValVec a, auto) -> Val
                                                                {
                        if (a.empty()) return nova_bool(false);
                        for (auto& v : *data) if (*v == *a[0]) return nova_bool(true);
                        return nova_bool(false); }, "has"));

                    self->obj->set("delete", NovaValue::makeNative([data](ValVec a, auto) -> Val
                                                                   {
                        if (a.empty()) return nova_bool(false);
                        for (size_t i = 0; i < data->size(); i++)
                            if (*(*data)[i] == *a[0]) { data->erase(data->begin() + i); return nova_bool(true); }
                        return nova_bool(false); }, "delete"));

                    self->obj->set("clear", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                  { data->clear(); return nova_null(); }, "clear"));
                    self->obj->set("size", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                 { return nova_num((double)data->size()); }, "size"));
                    self->obj->set("values", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                   { return nova_arr(*data); }, "values"));
                    self->obj->set("keys", self->obj->get("values"));
                    self->obj->set("toArray", self->obj->get("values"));
                    self->obj->set("__iter__", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                     { return nova_arr(*data); }, "__iter__"));

                    self->obj->set("forEach", NovaValue::makeNative([data, exe](ValVec a, auto cs) -> Val
                                                                    {
                        if (a.empty() || !a[0].isFunction()) return nova_null();
                        for (auto& v : *data) exe->callFunction(a[0], {v, v}, cs);
                        return nova_null(); }, "forEach"));

                    return self;
                };

                auto setNs = nova_obj();
                setNs->obj->set("new", NovaValue::makeNative([wrapSet](ValVec a, auto) -> Val
                                                             {
                    auto data = std::make_shared<ValVec>();
                    if (!a.empty() && a[0].isArray())
                        for (auto& v : a[0]->arr->inner) {
                            bool found = false;
                            for (auto& e : *data) if (*e == *v) { found = true; break; }
                            if (!found) data->push_back(v);
                        }
                    return (*wrapSet)(data); }, "new"));

                std_obj->obj->set("Set", setNs);
            }

            // ── Std.Map ───────────────────────────────────────────────────────────
            {
                using Entry = std::pair<Val, Val>;
                auto wrapMap = std::make_shared<std::function<Val(std::shared_ptr<std::vector<Entry>>)>>();

                *wrapMap = [exe, wrapMap](std::shared_ptr<std::vector<Entry>> data) -> Val
                {
                    auto self = nova_obj();
                    self->obj->set("__type__", nova_str("map"));

                    self->obj->set("set", NovaValue::makeNative([data, self](ValVec a, auto) -> Val
                                                                {
                        if (a.size() < 2) return self;
                        for (auto& e : *data) if (*e.first == *a[0]) { e.second = a[1]; return self; }
                        data->push_back({a[0], a[1]});
                        return self; }, "set"));

                    self->obj->set("get", NovaValue::makeNative([data](ValVec a, auto) -> Val
                                                                {
                        if (a.empty()) return nova_null();
                        for (auto& e : *data) if (*e.first == *a[0]) return e.second;
                        return nova_null(); }, "get"));

                    self->obj->set("has", NovaValue::makeNative([data](ValVec a, auto) -> Val
                                                                {
                        if (a.empty()) return nova_bool(false);
                        for (auto& e : *data) if (*e.first == *a[0]) return nova_bool(true);
                        return nova_bool(false); }, "has"));

                    self->obj->set("delete", NovaValue::makeNative([data](ValVec a, auto) -> Val
                                                                   {
                        if (a.empty()) return nova_bool(false);
                        for (size_t i = 0; i < data->size(); i++)
                            if (*(*data)[i].first == *a[0]) { data->erase(data->begin() + i); return nova_bool(true); }
                        return nova_bool(false); }, "delete"));

                    self->obj->set("clear", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                  { data->clear(); return nova_null(); }, "clear"));
                    self->obj->set("size", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                 { return nova_num((double)data->size()); }, "size"));

                    self->obj->set("keys", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                 {
                        ValVec out; for (auto& e : *data) out.push_back(e.first);
                        return nova_arr(std::move(out)); }, "keys"));
                    self->obj->set("values", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                   {
                        ValVec out; for (auto& e : *data) out.push_back(e.second);
                        return nova_arr(std::move(out)); }, "values"));
                    self->obj->set("entries", NovaValue::makeNative([data](ValVec, auto) -> Val
                                                                    {
                        ValVec out; for (auto& e : *data) out.push_back(nova_arr({e.first, e.second}));
                        return nova_arr(std::move(out)); }, "entries"));
                    self->obj->set("__iter__", self->obj->get("entries"));

                    self->obj->set("forEach", NovaValue::makeNative([data, exe](ValVec a, auto cs) -> Val
                                                                    {
                        if (a.empty() || !a[0].isFunction()) return nova_null();
                        for (auto& e : *data) exe->callFunction(a[0], {e.second, e.first}, cs);
                        return nova_null(); }, "forEach"));

                    return self;
                };

                auto mapNs = nova_obj();
                mapNs->obj->set("new", NovaValue::makeNative([wrapMap](ValVec a, auto) -> Val
                                                             {
                    auto data = std::make_shared<std::vector<Entry>>();
                    if (!a.empty() && a[0].isArray())
                        for (auto& pair : a[0]->arr->inner)
                            if (pair.isArray() && pair->arr->length() >= 2)
                                data->push_back({pair->arr->get(0), pair->arr->get(1)});
                    return (*wrapMap)(data); }, "new"));

                std_obj->obj->set("Map", mapNs);
            }

            // ── Std.WeakMap / Std.WeakSet ────────────────────────────────────────
            {
                // WeakMap
                using WEntry = std::pair<std::weak_ptr<NovaValue>, Val>;
                auto wrapWeakMap = std::make_shared<std::function<Val(std::shared_ptr<std::vector<WEntry>>)>>();

                *wrapWeakMap = [exe, wrapWeakMap](std::shared_ptr<std::vector<WEntry>> data) -> Val
                {
                    auto self = nova_obj();
                    self->obj->set("__type__", nova_str("weakmap"));

                    auto keyShared = [exe](Val v) -> std::shared_ptr<NovaValue>
                    {
                        if (!v || !(v.isObject() || v.isArray() || v.isFunction() || v.isClass() || v.isStruct()))
                            exe->_error("WeakMap: key must be an object, array, function, class, or struct");
                        return v.asShared();
                    };
                    auto prune = [data]()
                    {
                        data->erase(std::remove_if(data->begin(), data->end(),
                                                   [](const WEntry &e)
                                                   { return e.first.expired(); }),
                                    data->end());
                    };

                    self->obj->set("set", NovaValue::makeNative([data, self, keyShared, prune](ValVec a, auto) -> Val
                                                                {
                        if (a.size() < 2) return self;
                        prune();
                        auto ks = keyShared(a[0]);
                        for (auto& e : *data) if (e.first.lock() == ks) { e.second = a[1]; return self; }
                        data->push_back({ks, a[1]});
                        return self; }, "set"));

                    self->obj->set("get", NovaValue::makeNative([data, keyShared, prune](ValVec a, auto) -> Val
                                                                {
                        if (a.empty()) return nova_null();
                        prune();
                        auto ks = keyShared(a[0]);
                        for (auto& e : *data) if (e.first.lock() == ks) return e.second;
                        return nova_null(); }, "get"));

                    self->obj->set("has", NovaValue::makeNative([data, keyShared, prune](ValVec a, auto) -> Val
                                                                {
                        if (a.empty()) return nova_bool(false);
                        prune();
                        auto ks = keyShared(a[0]);
                        for (auto& e : *data) if (e.first.lock() == ks) return nova_bool(true);
                        return nova_bool(false); }, "has"));

                    self->obj->set("delete", NovaValue::makeNative([data, keyShared, prune](ValVec a, auto) -> Val
                                                                   {
                        if (a.empty()) return nova_bool(false);
                        prune();
                        auto ks = keyShared(a[0]);
                        for (size_t i = 0; i < data->size(); i++)
                            if ((*data)[i].first.lock() == ks) { data->erase(data->begin() + i); return nova_bool(true); }
                        return nova_bool(false); }, "delete"));

                    return self;
                };

                auto weakMapNs = nova_obj();
                weakMapNs->obj->set("new", NovaValue::makeNative([wrapWeakMap](ValVec, auto) -> Val
                                                                 { return (*wrapWeakMap)(std::make_shared<std::vector<WEntry>>()); }, "new"));
                std_obj->obj->set("WeakMap", weakMapNs);

                // WeakSet
                using WPtr = std::weak_ptr<NovaValue>;
                auto wrapWeakSet = std::make_shared<std::function<Val(std::shared_ptr<std::vector<WPtr>>)>>();

                *wrapWeakSet = [exe, wrapWeakSet](std::shared_ptr<std::vector<WPtr>> data) -> Val
                {
                    auto self = nova_obj();
                    self->obj->set("__type__", nova_str("weakset"));

                    auto keyShared = [exe](Val v) -> std::shared_ptr<NovaValue>
                    {
                        if (!v || !(v.isObject() || v.isArray() || v.isFunction() || v.isClass() || v.isStruct()))
                            exe->_error("WeakSet: value must be an object, array, function, class, or struct");
                        return v.asShared();
                    };
                    auto prune = [data]()
                    {
                        data->erase(std::remove_if(data->begin(), data->end(),
                                                   [](const WPtr &w)
                                                   { return w.expired(); }),
                                    data->end());
                    };

                    self->obj->set("add", NovaValue::makeNative([data, self, keyShared, prune](ValVec a, auto) -> Val
                                                                {
                        if (a.empty()) return self;
                        prune();
                        auto ks = keyShared(a[0]);
                        for (auto& w : *data) if (w.lock() == ks) return self;
                        data->push_back(ks);
                        return self; }, "add"));

                    self->obj->set("has", NovaValue::makeNative([data, keyShared, prune](ValVec a, auto) -> Val
                                                                {
                        if (a.empty()) return nova_bool(false);
                        prune();
                        auto ks = keyShared(a[0]);
                        for (auto& w : *data) if (w.lock() == ks) return nova_bool(true);
                        return nova_bool(false); }, "has"));

                    self->obj->set("delete", NovaValue::makeNative([data, keyShared, prune](ValVec a, auto) -> Val
                                                                   {
                        if (a.empty()) return nova_bool(false);
                        prune();
                        auto ks = keyShared(a[0]);
                        for (size_t i = 0; i < data->size(); i++)
                            if ((*data)[i].lock() == ks) { data->erase(data->begin() + i); return nova_bool(true); }
                        return nova_bool(false); }, "delete"));

                    return self;
                };

                auto weakSetNs = nova_obj();
                weakSetNs->obj->set("new", NovaValue::makeNative([wrapWeakSet](ValVec, auto) -> Val
                                                                 { return (*wrapWeakSet)(std::make_shared<std::vector<WPtr>>()); }, "new"));
                std_obj->obj->set("WeakSet", weakSetNs);
            }
            // ── Std.Regex ─────────────────────────────────────────────────────────
            {
                auto wrapRegex = std::make_shared<std::function<Val(std::string, std::string)>>();

                *wrapRegex = [exe](std::string pat, std::string flags) -> Val
                {
                    auto self = nova_obj();
                    self->obj->set("source", nova_str(pat));
                    self->obj->set("flags", nova_str(flags));
                    self->obj->set("_kind", nova_str("regex"));

                    bool global = flags.find('g') != std::string::npos;
                    bool icase = flags.find('i') != std::string::npos;
                    bool multi = flags.find('m') != std::string::npos;

                    self->obj->set("global", nova_bool(global));
                    self->obj->set("ignoreCase", nova_bool(icase));
                    self->obj->set("multiline", nova_bool(multi));

                    auto rxFlags = std::regex_constants::ECMAScript;
                    if (icase)
                        rxFlags |= std::regex_constants::icase;
                    if (multi)
                        rxFlags |= std::regex_constants::multiline;

                    auto lastIndex = std::make_shared<size_t>(0);

                    // .test(str) → bool  (advances lastIndex when global, like JS)
                    self->obj->set("test", NovaValue::makeNative([pat, rxFlags, global, lastIndex](ValVec a, auto) -> Val
                                                                 {
                        if (a.empty()) return nova_bool(false);
                        std::string subject = a[0].asString();
                        try {
                            std::regex re(pat, rxFlags);
                            size_t start = global ? *lastIndex : 0;
                            if (start > subject.size()) { *lastIndex = 0; return nova_bool(false); }
                            std::smatch m;
                            std::string rest = subject.substr(start);
                            bool found = std::regex_search(rest, m, re);
                            if (global)
                                *lastIndex = found ? start + m.position(0) + m.length(0) : 0;
                            return nova_bool(found);
                        } catch (...) { return nova_bool(false); } }, "test"));

                    // .exec(str) → match array with .index/.input props, or null
                    // stateful via lastIndex when global, mirrors JS RegExp.exec
                    self->obj->set("exec", NovaValue::makeNative([pat, rxFlags, global, lastIndex](ValVec a, auto) -> Val
                                                                 {
                        if (a.empty()) return nova_null();
                        std::string subject = a[0].asString();
                        try {
                            std::regex re(pat, rxFlags);
                            size_t start = global ? *lastIndex : 0;
                            if (start > subject.size()) { *lastIndex = 0; return nova_null(); }
                            std::string rest = subject.substr(start);
                            std::smatch m;
                            if (!std::regex_search(rest, m, re)) {
                                if (global) *lastIndex = 0;
                                return nova_null();
                            }
                            auto arr = nova_obj(); // array-like with extra props
                            ValVec groups;
                            for (size_t i = 0; i < m.size(); i++)
                                groups.push_back(m[i].matched ? nova_str(m[i].str()) : nova_null());
                            Val arrVal = nova_arr(groups);
                            arrVal->obj = nullptr; // not actually used; keep as array
                            // attach index/input on the array object directly
                            if (arrVal.isArray()) {
                                // arrays don't carry arbitrary props in this VM, so
                                // wrap in an object exposing both array access and props
                            }
                            auto res = nova_obj();
                            res->obj->set("0", groups.empty() ? nova_str("") : groups[0]);
                            for (size_t i = 0; i < groups.size(); i++)
                                res->obj->set(std::to_string(i), groups[i]);
                            res->obj->set("length", nova_num((double)groups.size()));
                            res->obj->set("index", nova_num((double)(start + m.position(0))));
                            res->obj->set("input", nova_str(subject));
                            res->obj->set("array", nova_arr(groups)); // convenient real array form
                            if (global)
                                *lastIndex = start + m.position(0) + m.length(0);
                            return res;
                        } catch (...) { return nova_null(); } }, "exec"));

                    // .lastIndex getter/setter as methods (no property hooks on natives here)
                    self->obj->set("getLastIndex", NovaValue::makeNative([lastIndex](ValVec, auto) -> Val
                                                                         { return nova_num((double)*lastIndex); }, "getLastIndex"));
                    self->obj->set("setLastIndex", NovaValue::makeNative([lastIndex](ValVec a, auto) -> Val
                                                                         {
                        if (!a.empty()) *lastIndex = (size_t)std::max(0.0, a[0].asNumber());
                        return nova_null(); }, "setLastIndex"));

                    // .match(str) → array of matches (global) or single match array (non-global)
                    self->obj->set("match", NovaValue::makeNative([pat, rxFlags, global](ValVec a, auto) -> Val
                                                                  {
                        if (a.empty()) return nova_null();
                        try {
                            std::string subject = a[0].asString();
                            std::regex re(pat, rxFlags);
                            if (global) {
                                auto arr = nova_arr();
                                auto it = std::sregex_iterator(subject.begin(), subject.end(), re);
                                for (; it != std::sregex_iterator(); ++it)
                                    arr->arr->push(nova_str((*it)[0].str()));
                                return arr->arr->length() == 0 ? nova_null() : arr;
                            } else {
                                std::smatch m;
                                if (!std::regex_search(subject, m, re)) return nova_null();
                                auto arr = nova_arr();
                                for (size_t i = 0; i < m.size(); i++)
                                    arr->arr->push(m[i].matched ? nova_str(m[i].str()) : nova_null());
                                return arr;
                            }
                        } catch (...) { return nova_null(); } }, "match"));

                    // .matchAll(str) → array of match-result objects (each shaped like .exec's result)
                    self->obj->set("matchAll", NovaValue::makeNative([pat, rxFlags](ValVec a, auto) -> Val
                                                                     {
                        if (a.empty()) return nova_arr();
                        try {
                            std::string subject = a[0].asString();
                            std::regex re(pat, rxFlags);
                            auto results = nova_arr();
                            auto it = std::sregex_iterator(subject.begin(), subject.end(), re);
                            for (; it != std::sregex_iterator(); ++it) {
                                auto& m = *it;
                                auto res = nova_obj();
                                ValVec groups;
                                for (size_t i = 0; i < m.size(); i++)
                                    groups.push_back(m[i].matched ? nova_str(m[i].str()) : nova_null());
                                for (size_t i = 0; i < groups.size(); i++)
                                    res->obj->set(std::to_string(i), groups[i]);
                                res->obj->set("length", nova_num((double)groups.size()));
                                res->obj->set("index", nova_num((double)m.position(0)));
                                res->obj->set("input", nova_str(subject));
                                res->obj->set("array", nova_arr(groups));
                                results->arr->push(res);
                            }
                            return results;
                        } catch (...) { return nova_arr(); } }, "matchAll"));

                    // .replace(str, repl) → string. repl may be a string or a function(match, ...groups, index, input)
                    self->obj->set("replace", NovaValue::makeNative([exe, pat, rxFlags, global](ValVec a, auto cs) -> Val
                                                                    {
                        if (a.size() < 2) return a.empty() ? nova_str("") : a[0];
                        std::string subject = a[0].asString();
                        try {
                            std::regex re(pat, rxFlags);
                            if (a[1].isFunction()) {
                                Val fn = a[1];
                                std::string out;
                                auto begin = std::sregex_iterator(subject.begin(), subject.end(), re);
                                auto end   = std::sregex_iterator();
                                size_t lastPos = 0;
                                bool didOne = false;
                                for (auto it = begin; it != end; ++it) {
                                    if (!global && didOne) break;
                                    auto& m = *it;
                                    out += subject.substr(lastPos, m.position(0) - lastPos);
                                    ValVec args;
                                    for (size_t i = 0; i < m.size(); i++)
                                        args.push_back(m[i].matched ? nova_str(m[i].str()) : nova_null());
                                    args.push_back(nova_num((double)m.position(0)));
                                    args.push_back(nova_str(subject));
                                    out += exe->callFunction(fn, args, cs).asString();
                                    lastPos = m.position(0) + m.length(0);
                                    didOne = true;
                                }
                                out += subject.substr(lastPos);
                                return nova_str(out);
                            }
                            std::string repl = a[1].asString();
                            if (global)
                                return nova_str(std::regex_replace(subject, re, repl));
                            return nova_str(std::regex_replace(subject, re, repl,
                                std::regex_constants::format_first_only));
                        } catch (...) { return a[0]; } }, "replace"));

                    // .split(str, limit?) → array
                    self->obj->set("split", NovaValue::makeNative([pat, rxFlags](ValVec a, auto) -> Val
                                                                  {
                        if (a.empty()) return nova_arr();
                        try {
                            std::string subject = a[0].asString();
                            int limit = a.size() > 1 ? (int)a[1].asNumber() : -1;
                            std::regex re(pat, rxFlags);
                            std::sregex_token_iterator it(subject.begin(), subject.end(), re, -1), end;
                            auto arr = nova_arr();
                            for (; it != end; ++it) {
                                if (limit >= 0 && (int)arr->arr->length() >= limit) break;
                                arr->arr->push(nova_str(it->str()));
                            }
                            return arr;
                        } catch (...) { return nova_arr(); } }, "split"));

                    self->obj->set("toString", NovaValue::makeNative([pat, flags](ValVec, auto) -> Val
                                                                     { return nova_str("/" + pat + "/" + flags); }, "toString"));

                    return self;
                };

                auto regexNs = nova_obj();

                // Std.Regex.new(pattern, flags?) — mirrors `new RegExp(pattern, flags)`
                regexNs->obj->set("new", NovaValue::makeNative([wrapRegex](ValVec a, auto) -> Val
                                                               {
                    std::string pat   = a.empty() ? "" : a[0].asString();
                    std::string flags = a.size() > 1 ? a[1].asString() : "";
                    return (*wrapRegex)(pat, flags); }, "new"));

                // Std.Regex.escape(str) — escape regex metacharacters for literal matching
                regexNs->obj->set("escape", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
                    if (a.empty()) return nova_str("");
                    static const std::string special = "\\^$.|?*+()[]{}";
                    std::string out;
                    for (char c : a[0]->sval) {
                        if (special.find(c) != std::string::npos) out += '\\';
                        out += c;
                    }
                    return nova_str(out); }, "escape"));

                // Std.Regex.isRegex(val) — duck-type check via _kind marker
                regexNs->obj->set("isRegex", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
                    if (a.empty() || !a[0] || !a[0].isObject()) return nova_bool(false);
                    Val k = a[0]->obj->get("_kind");
                    return nova_bool(k && k.isString() && k->sval == "regex"); }, "isRegex"));

                std_obj->obj->set("Regex", regexNs);
            }
        }

        // ── Std.Getch / Std.Kbhit / Std.Putch — raw, unbuffered console I/O ──────────
        {
#ifdef _WIN32
            std_obj->obj->set("Getch", NovaValue::makeNative([](ValVec, auto) -> Val
                                                             { return nova_num((double)_getch()); }, "Getch"));

            std_obj->obj->set("Kbhit", NovaValue::makeNative([](ValVec, auto) -> Val
                                                             { return nova_bool(_kbhit() != 0); }, "Kbhit"));

            std_obj->obj->set("Putch", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
            if (a.empty()) return nova_null();
            _putch((int)a[0].asNumber());
            return nova_null(); }, "Putch"));
#else
            // Getch — blocking, unbuffered, no echo (equivalent to conio's _getch)
            std_obj->obj->set("Getch", NovaValue::makeNative([](ValVec, auto) -> Val
                                                             {
            termios oldt{}, newt{};
            tcgetattr(STDIN_FILENO, &oldt);
            newt = oldt;
            newt.c_lflag &= ~(ICANON | ECHO);
            newt.c_cc[VMIN]  = 1;
            newt.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);

            int c = std::getchar();

            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return nova_num((double)c); }, "Getch"));

            // Kbhit — non-blocking "is a key waiting?" (equivalent to conio's _kbhit)
            // Peeks a byte via a temporarily non-blocking raw-mode read, then
            // pushes it back with ungetc so a following Getch() still sees it.
            std_obj->obj->set("Kbhit", NovaValue::makeNative([](ValVec, auto) -> Val
                                                             {
            termios oldt{}, newt{};
            tcgetattr(STDIN_FILENO, &oldt);
            newt = oldt;
            newt.c_lflag &= ~(ICANON | ECHO);
            newt.c_cc[VMIN]  = 0;
            newt.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);

            int oldFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
            fcntl(STDIN_FILENO, F_SETFL, oldFlags | O_NONBLOCK);

            int ch = std::getchar();

            fcntl(STDIN_FILENO, F_SETFL, oldFlags);
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

            if (ch != EOF) {
                std::ungetc(ch, stdin);
                return nova_bool(true);
            }
            return nova_bool(false); }, "Kbhit"));

            // Putch — write one char immediately, unbuffered (equivalent to conio's _putch)
            std_obj->obj->set("Putch", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
            if (a.empty()) return nova_null();
            std::putchar((int)a[0].asNumber());
            std::fflush(stdout);
            return nova_null(); }, "Putch"));
#endif
        }

#ifdef _WIN32
        {
            auto winObj = nova_obj();
            auto comObj = nova_obj();
            auto exe = this;

            // ────────────────────────────────────────────────────────────────────────
            //  string helpers
            // ────────────────────────────────────────────────────────────────────────
            auto utf8ToWide = [](const std::string &s) -> std::wstring
            {
                if (s.empty())
                    return L"";
                int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
                std::wstring w(wlen, 0);
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
                if (!w.empty() && w.back() == L'\0')
                    w.pop_back();
                return w;
            };
            auto wideToUtf8 = [](const wchar_t *w, int len = -1) -> std::string
            {
                if (!w)
                    return "";
                int u8len = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
                std::string out(u8len > 0 ? u8len : 0, '\0');
                if (u8len > 0)
                    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), u8len, nullptr, nullptr);
                if (len < 0 && !out.empty() && out.back() == '\0')
                    out.pop_back();
                return out;
            };
            auto bstrToUtf8Shared = std::make_shared<std::function<std::string(BSTR)>>(
                [](BSTR b) -> std::string
                {
                    if (!b)
                        return "";
                    return std::string(); // placeholder, replaced below once wideToUtf8 captured
                });
            // real bstr->utf8 (needs wideToUtf8 in scope, defined via lambda capture below)
            auto bstrToUtf8 = [wideToUtf8](BSTR b) -> std::string
            {
                if (!b)
                    return "";
                return wideToUtf8(b, SysStringLen(b));
            };

            // (COM.MakeVTable's supporting types/functions live at file scope —
            // see the NovaVTableObject / NovaVT_* / novaVTableFfiTypeForName /
            // novaVTableClosureHandler block near the top of this file,
            // alongside makeRawPtrFromAddress.)

            // ────────────────────────────────────────────────────────────────────────
            //  forward decls (shared_ptrs so lambdas below can mutually reference)
            // ────────────────────────────────────────────────────────────────────────
            auto wrapComPtr = std::make_shared<std::function<Val(IUnknown *, bool)>>();
            auto variantToVal = std::make_shared<std::function<Val(VARIANT &)>>();
            auto valToVariant = std::make_shared<std::function<void(Val, VARIANT &)>>();
            auto safeArrayToVal = std::make_shared<std::function<Val(SAFEARRAY *, VARTYPE)>>();
            auto valToSafeArray = std::make_shared<std::function<SAFEARRAY *(Val)>>();

            // ────────────────────────────────────────────────────────────────────────
            //  SAFEARRAY <-> Nova array  (1-D only; see file header notes)
            // ────────────────────────────────────────────────────────────────────────
            *safeArrayToVal = [variantToVal, bstrToUtf8](SAFEARRAY *sa, VARTYPE vt) -> Val
            {
                if (!sa)
                    return nova_arr();
                long lBound = 0, uBound = -1;
                SafeArrayGetLBound(sa, 1, &lBound);
                SafeArrayGetUBound(sa, 1, &uBound);
                ValVec out;
                for (long i = lBound; i <= uBound; i++)
                {
                    if (vt == VT_VARIANT)
                    {
                        VARIANT v;
                        VariantInit(&v);
                        if (SUCCEEDED(SafeArrayGetElement(sa, &i, &v)))
                        {
                            out.push_back((*variantToVal)(v));
                            VariantClear(&v);
                        }
                        else
                            out.push_back(nova_null());
                    }
                    else if (vt == VT_BSTR)
                    {
                        BSTR b = nullptr;
                        if (SUCCEEDED(SafeArrayGetElement(sa, &i, &b)))
                        {
                            out.push_back(nova_str(bstrToUtf8(b)));
                            SysFreeString(b);
                        }
                        else
                            out.push_back(nova_str(""));
                    }
                    else if (vt == VT_R8)
                    {
                        double d = 0;
                        SafeArrayGetElement(sa, &i, &d);
                        out.push_back(nova_num(d));
                    }
                    else if (vt == VT_I4)
                    {
                        long v = 0;
                        SafeArrayGetElement(sa, &i, &v);
                        out.push_back(nova_num((double)v));
                    }
                    else
                    {
                        out.push_back(nova_null());
                    }
                }
                return nova_arr(std::move(out));
            };

            *valToSafeArray = [valToVariant](Val val) -> SAFEARRAY *
            {
                if (!val || !val.isArray())
                    return nullptr;
                auto &inner = val->arr->inner;
                SAFEARRAYBOUND bound{(ULONG)inner.size(), 0};
                SAFEARRAY *sa = SafeArrayCreate(VT_VARIANT, 1, &bound);
                if (!sa)
                    return nullptr;
                for (long i = 0; i < (long)inner.size(); i++)
                {
                    VARIANT v;
                    (*valToVariant)(inner[i], v);
                    SafeArrayPutElement(sa, &i, &v);
                    VariantClear(&v);
                }
                return sa;
            };

            // ────────────────────────────────────────────────────────────────────────
            //  Variant <-> Nova  (now with VT_DATE, VT_CY, SAFEARRAY, IEnumVARIANT)
            // ────────────────────────────────────────────────────────────────────────
            *variantToVal = [wrapComPtr, bstrToUtf8, safeArrayToVal](VARIANT &v) -> Val
            {
                VARTYPE baseType = v.vt & VT_TYPEMASK;
                bool isArray = (v.vt & VT_ARRAY) != 0;

                if (isArray && v.parray)
                {
                    return (*safeArrayToVal)(v.parray, baseType);
                }

                switch (baseType)
                {
                case VT_EMPTY:
                case VT_NULL:
                    return nova_null();
                case VT_BOOL:
                    return nova_bool(v.boolVal != VARIANT_FALSE);
                case VT_I1:
                    return nova_num(v.cVal);
                case VT_UI1:
                    return nova_num(v.bVal);
                case VT_I2:
                    return nova_num(v.iVal);
                case VT_UI2:
                    return nova_num(v.uiVal);
                case VT_I4:
                    return nova_num(v.lVal);
                case VT_UI4:
                    return nova_num(v.ulVal);
                case VT_I8:
                    return nova_num((double)v.llVal);
                case VT_UI8:
                    return nova_num((double)v.ullVal);
                case VT_INT:
                    return nova_num(v.intVal);
                case VT_UINT:
                    return nova_num(v.uintVal);
                case VT_R4:
                    return nova_num(v.fltVal);
                case VT_R8:
                    return nova_num(v.dblVal);
                case VT_CY:
                {
                    // currency: 4 implied decimal digits, stored as int64 * 10000
                    double d = 0;
                    VarR8FromCy(v.cyVal, &d);
                    return nova_num(d);
                }
                case VT_DATE:
                {
                    // VARIANT DATE -> ms since Unix epoch, for interop with Nova's
                    // existing __timestamp__/Chrono millisecond convention.
                    SYSTEMTIME st{};
                    if (VariantTimeToSystemTime(v.date, &st))
                    {
                        FILETIME ft{};
                        SystemTimeToFileTime(&st, &ft);
                        ULARGE_INTEGER uli{};
                        uli.LowPart = ft.dwLowDateTime;
                        uli.HighPart = ft.dwHighDateTime;
                        // FILETIME epoch (1601) -> Unix epoch (1970) offset in 100ns units
                        const uint64_t EPOCH_DIFF = 116444736000000000ULL;
                        uint64_t ms = (uli.QuadPart - EPOCH_DIFF) / 10000ULL;
                        return nova_num((double)ms);
                    }
                    return nova_num(v.date); // fallback: raw OLE date (days since 1899-12-30)
                }
                case VT_BSTR:
                    return nova_str(bstrToUtf8(v.bstrVal));
                case VT_DISPATCH:
                    return v.pdispVal ? (*wrapComPtr)(v.pdispVal, true) : nova_null();
                case VT_UNKNOWN:
                    return v.punkVal ? (*wrapComPtr)(v.punkVal, true) : nova_null();
                default:
                    return nova_null();
                }
            };

            *valToVariant = [utf8ToWide, valToSafeArray](Val val, VARIANT &v)
            {
                VariantInit(&v);
                if (!val || val.isNull())
                {
                    v.vt = VT_NULL;
                    return;
                }
                if (val.isBool())
                {
                    v.vt = VT_BOOL;
                    v.boolVal = val.asBool() ? VARIANT_TRUE : VARIANT_FALSE;
                    return;
                }
                if (val.isNumber())
                {
                    v.vt = VT_R8;
                    v.dblVal = val.asNumber();
                    return;
                }
                if (val.isString())
                {
                    std::wstring w = utf8ToWide(val.asString());
                    v.vt = VT_BSTR;
                    v.bstrVal = SysAllocString(w.c_str());
                    return;
                }
                if (val.isArray())
                {
                    SAFEARRAY *sa = (*valToSafeArray)(val);
                    if (sa)
                    {
                        v.vt = VT_ARRAY | VT_VARIANT;
                        v.parray = sa;
                        return;
                    }
                }
                v.vt = VT_NULL;
            };

            // ────────────────────────────────────────────────────────────────────────
            //  EXCEPINFO -> readable string
            // ────────────────────────────────────────────────────────────────────────
            auto excepInfoToString = [wideToUtf8](EXCEPINFO &ei, HRESULT hr) -> std::string
            {
                std::string msg = "hr=0x";
                {
                    std::ostringstream ss;
                    ss << std::hex << (unsigned long)hr;
                    msg += ss.str();
                }
                if (ei.bstrSource)
                    msg += " source=" + wideToUtf8(ei.bstrSource, SysStringLen(ei.bstrSource));
                if (ei.bstrDescription)
                    msg += " desc=\"" + wideToUtf8(ei.bstrDescription, SysStringLen(ei.bstrDescription)) + "\"";
                if (ei.scode)
                {
                    std::ostringstream ss;
                    ss << " scode=0x" << std::hex << (unsigned long)ei.scode;
                    msg += ss.str();
                }
                if (ei.bstrSource)
                    SysFreeString(ei.bstrSource);
                if (ei.bstrDescription)
                    SysFreeString(ei.bstrDescription);
                if (ei.bstrHelpFile)
                    SysFreeString(ei.bstrHelpFile);
                return msg;
            };

            // ────────────────────────────────────────────────────────────────────────
            //  ITypeInfo introspection: list members of a dispatch interface
            // ────────────────────────────────────────────────────────────────────────
            auto describeTypeInfo = [wideToUtf8](ITypeInfo *ti) -> Val
            {
                auto out = nova_arr();
                if (!ti)
                    return out;
                TYPEATTR *attr = nullptr;
                if (FAILED(ti->GetTypeAttr(&attr)) || !attr)
                    return out;

                for (UINT i = 0; i < attr->cFuncs; i++)
                {
                    FUNCDESC *fd = nullptr;
                    if (FAILED(ti->GetFuncDesc(i, &fd)) || !fd)
                        continue;

                    BSTR nameBstr = nullptr;
                    UINT namesFetched = 0;
                    std::vector<BSTR> paramNames(fd->cParams + 1, nullptr);
                    ti->GetNames(fd->memid, paramNames.data(), fd->cParams + 1, &namesFetched);

                    auto entry = nova_obj();
                    entry->obj->set("name", nova_str(namesFetched > 0 ? wideToUtf8(paramNames[0], SysStringLen(paramNames[0])) : ""));
                    entry->obj->set("dispid", nova_num((double)fd->memid));
                    entry->obj->set("paramCount", nova_num((double)fd->cParams));
                    std::string kind = (fd->invkind & INVOKE_FUNC) ? "method" : (fd->invkind & INVOKE_PROPERTYGET) ? "get"
                                                                            : (fd->invkind & INVOKE_PROPERTYPUT)   ? "put"
                                                                                                                   : "unknown";
                    entry->obj->set("kind", nova_str(kind));

                    ValVec params;
                    for (UINT p = 1; p < namesFetched; p++)
                        params.push_back(nova_str(wideToUtf8(paramNames[p], SysStringLen(paramNames[p]))));
                    entry->obj->set("params", nova_arr(std::move(params)));

                    for (UINT n = 0; n < namesFetched; n++)
                        if (paramNames[n])
                            SysFreeString(paramNames[n]);
                    out->arr->push(entry);
                    ti->ReleaseFuncDesc(fd);
                }

                for (UINT i = 0; i < attr->cVars; i++)
                {
                    VARDESC *vd = nullptr;
                    if (FAILED(ti->GetVarDesc(i, &vd)) || !vd)
                        continue;
                    BSTR nameBstr = nullptr;
                    UINT namesFetched = 0;
                    ti->GetNames(vd->memid, &nameBstr, 1, &namesFetched);
                    auto entry = nova_obj();
                    entry->obj->set("name", nova_str(namesFetched > 0 ? wideToUtf8(nameBstr, SysStringLen(nameBstr)) : ""));
                    entry->obj->set("dispid", nova_num((double)vd->memid));
                    entry->obj->set("kind", nova_str("property"));
                    if (nameBstr)
                        SysFreeString(nameBstr);
                    out->arr->push(entry);
                    ti->ReleaseVarDesc(vd);
                }

                ti->ReleaseTypeAttr(attr);
                return out;
            };

            // ────────────────────────────────────────────────────────────────────────
            //  Custom in-process IDispatch sink for connection-point events
            //  (the risky part — see file header notes)
            // ────────────────────────────────────────────────────────────────────────
            // NovaEventSink: a minimal hand-rolled IDispatch. Standard COM vtable
            // layout, refcounted, forwards every Invoke() call to a Nova function
            // keyed by DISPID. GetIDsOfNames/GetTypeInfo* are stubs since event
            // sinks are invoked purely by DISPID (this is how VB6/VBA-style event
            // sinks have always worked; source objects call Invoke(dispid) directly,
            // they don't look the name up first).
            struct NovaEventSink : public IDispatch
            {
                std::atomic<ULONG> refCount{1};
                Executor *exe;
                std::shared_ptr<Scope> scope;
                // dispid -> Nova function to call with the event's args
                std::unordered_map<DISPID, Val> handlers;
                std::function<Val(VARIANT &)> variantToVal;

                NovaEventSink(Executor *e, std::shared_ptr<Scope> s, std::function<Val(VARIANT &)> v2v)
                    : exe(e), scope(s), variantToVal(v2v) {}

                // IUnknown
                HRESULT __stdcall QueryInterface(REFIID riid, void **ppv) override
                {
                    if (!ppv)
                        return E_POINTER;
                    if (riid == IID_IUnknown || riid == IID_IDispatch)
                    {
                        *ppv = static_cast<IDispatch *>(this);
                        AddRef();
                        return S_OK;
                    }
                    // Event sinks are commonly QI'd for their specific dispinterface
                    // IID too; since we don't know the caller's arbitrary event IID
                    // in this generic bridge, we deliberately answer IUnknown/IDispatch
                    // only. Some sources require exact-IID QI to succeed to hand out
                    // the sink — if that happens, Advise() below will fail and we
                    // report it rather than silently pretending success.
                    *ppv = nullptr;
                    return E_NOINTERFACE;
                }
                ULONG __stdcall AddRef() override { return ++refCount; }
                ULONG __stdcall Release() override
                {
                    ULONG r = --refCount;
                    if (r == 0)
                        delete this;
                    return r;
                }

                // IDispatch
                HRESULT __stdcall GetTypeInfoCount(UINT *pctinfo) override
                {
                    *pctinfo = 0;
                    return S_OK;
                }
                HRESULT __stdcall GetTypeInfo(UINT, LCID, ITypeInfo **ppti) override
                {
                    *ppti = nullptr;
                    return E_NOTIMPL;
                }
                HRESULT __stdcall GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return E_NOTIMPL; }

                HRESULT __stdcall Invoke(DISPID dispIdMember, REFIID, LCID, WORD,
                                         DISPPARAMS *pDispParams, VARIANT *pVarResult,
                                         EXCEPINFO *, UINT *) override
                {
                    auto it = handlers.find(dispIdMember);
                    if (it == handlers.end())
                    {
                        if (pVarResult)
                            VariantInit(pVarResult);
                        return S_OK; // unhandled event id — not an error, just ignored
                    }
                    ValVec args;
                    if (pDispParams)
                    {
                        // COM event args arrive in reverse order, same as regular Invoke
                        for (LONG i = (LONG)pDispParams->cArgs - 1; i >= 0; i--)
                            args.push_back(variantToVal(pDispParams->rgvarg[i]));
                    }
                    try
                    {
                        exe->callFunction(it->second, args, scope);
                    }
                    catch (...)
                    {
                        // swallow — an exception crossing back into the COM caller's
                        // stack would be undefined behavior for most hosts
                    }
                    if (pVarResult)
                        VariantInit(pVarResult);
                    return S_OK;
                }
            };

            // ── active sink registry, so Unadvise can find the cookie + sink ────────
            struct AdviseHandle
            {
                IConnectionPoint *cp;
                DWORD cookie;
                NovaEventSink *sink;
            };
            auto activeAdvises = std::make_shared<std::vector<std::shared_ptr<AdviseHandle>>>();

            // ────────────────────────────────────────────────────────────────────────
            //  wrap a raw COM pointer as a Nova object
            // ────────────────────────────────────────────────────────────────────────
            *wrapComPtr = [exe, variantToVal, valToVariant, wrapComPtr, describeTypeInfo,
                           excepInfoToString, activeAdvises, utf8ToWide](IUnknown *punk, bool addRef) -> Val
            {
                if (!punk)
                    return nova_null();
                if (addRef)
                    punk->AddRef();

                auto ptr = std::shared_ptr<IUnknown>(punk, [](IUnknown *p)
                                                     { if (p) p->Release(); });
                auto self = nova_obj();
                self->obj->set("__type__", nova_str("com_object"));

                self->obj->set("addr", NovaValue::makeNative([ptr](ValVec, auto) -> Val
                                                             {
            std::ostringstream ss; ss << "0x" << std::hex << (uintptr_t)ptr.get();
            return nova_str(ss.str()); }, "addr"));

                self->obj->set("QueryInterface", NovaValue::makeNative([ptr, wrapComPtr](ValVec a, auto) -> Val
                                                                       {
            if (a.empty()) return nova_null();
            std::string iidStr = a[0].asString();
            int wlen = MultiByteToWideChar(CP_UTF8, 0, iidStr.c_str(), -1, nullptr, 0);
            std::wstring wbuf(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, iidStr.c_str(), -1, wbuf.data(), wlen);
            IID iid;
            if (FAILED(IIDFromString(wbuf.c_str(), &iid))) return nova_null();
            void* result = nullptr;
            if (FAILED(ptr->QueryInterface(iid, &result)) || !result) return nova_null();
            return (*wrapComPtr)((IUnknown*)result, false); }, "QueryInterface"));

                self->obj->set("AddRef", NovaValue::makeNative([ptr](ValVec, auto) -> Val
                                                               { return nova_num((double)ptr->AddRef()); }, "AddRef"));
                self->obj->set("Release", NovaValue::makeNative([ptr](ValVec, auto) -> Val
                                                                { return nova_num((double)ptr->Release()); }, "Release"));

                // ── IDispatch automation ─────────────────────────────────────────
                auto invokeHelper = [exe, ptr, variantToVal, valToVariant, excepInfoToString](
                                        const std::string &name, ValVec args, WORD flags) -> Val
                {
                    IDispatch *d = nullptr;
                    if (FAILED(ptr->QueryInterface(IID_IDispatch, (void **)&d)) || !d)
                        exe->_error("COM object does not support IDispatch (use VTableCall instead)");

                    int wlen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
                    std::wstring wname(wlen, 0);
                    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname.data(), wlen);
                    LPOLESTR nameArr[1] = {(LPOLESTR)wname.c_str()};
                    DISPID dispid;
                    HRESULT hr = d->GetIDsOfNames(IID_NULL, nameArr, 1, LOCALE_USER_DEFAULT, &dispid);
                    if (FAILED(hr))
                    {
                        d->Release();
                        exe->_error("COM member not found: " + name);
                    }

                    std::vector<VARIANT> variants(args.size());
                    for (size_t i = 0; i < args.size(); i++)
                        (*valToVariant)(args[args.size() - 1 - i], variants[i]);

                    DISPPARAMS params{};
                    params.cArgs = (UINT)variants.size();
                    params.rgvarg = variants.empty() ? nullptr : variants.data();
                    DISPID propPutId = DISPID_PROPERTYPUT;
                    if (flags == DISPATCH_PROPERTYPUT)
                    {
                        params.cNamedArgs = 1;
                        params.rgdispidNamedArgs = &propPutId;
                    }

                    VARIANT result;
                    VariantInit(&result);
                    EXCEPINFO excep{};
                    hr = d->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, &result, &excep, nullptr);

                    for (auto &v : variants)
                        VariantClear(&v);
                    d->Release();

                    if (FAILED(hr))
                        exe->_error("COM Invoke failed for '" + name + "': " + excepInfoToString(excep, hr));

                    Val out = (*variantToVal)(result);
                    VariantClear(&result);
                    return out;
                };

                self->obj->set("Invoke", NovaValue::makeNative([invokeHelper](ValVec a, auto) -> Val
                                                               {
            if (a.empty()) return nova_null();
            std::string name = a[0].asString();
            ValVec rest(a.begin() + 1, a.end());
            return invokeHelper(name, rest, DISPATCH_METHOD | DISPATCH_PROPERTYGET); }, "Invoke"));

                self->obj->set("Get", NovaValue::makeNative([invokeHelper](ValVec a, auto) -> Val
                                                            {
            if (a.empty()) return nova_null();
            return invokeHelper(a[0].asString(), {}, DISPATCH_PROPERTYGET); }, "Get"));

                self->obj->set("Set", NovaValue::makeNative([invokeHelper](ValVec a, auto) -> Val
                                                            {
            if (a.size() < 2) return nova_null();
            return invokeHelper(a[0].asString(), {a[1]}, DISPATCH_PROPERTYPUT); }, "Set"));

                // ── ITypeInfo introspection ──────────────────────────────────────
                self->obj->set("Describe", NovaValue::makeNative([ptr, describeTypeInfo](ValVec, auto) -> Val
                                                                 {
            IDispatch* d = nullptr;
            if (FAILED(ptr->QueryInterface(IID_IDispatch, (void**)&d)) || !d) return nova_arr();
            UINT tiCount = 0;
            d->GetTypeInfoCount(&tiCount);
            if (tiCount == 0) { d->Release(); return nova_arr(); }
            ITypeInfo* ti = nullptr;
            HRESULT hr = d->GetTypeInfo(0, LOCALE_USER_DEFAULT, &ti);
            d->Release();
            if (FAILED(hr) || !ti) return nova_arr();
            Val result = describeTypeInfo(ti);
            ti->Release();
            return result; }, "Describe"));

                // ── IEnumVARIANT iteration → Nova array ──────────────────────────
                // For collection-like COM objects exposing _NewEnum (DISPID -4) or
                // a direct IEnumVARIANT via QueryInterface.
                self->obj->set("ToArray", NovaValue::makeNative([exe, ptr, variantToVal](ValVec, auto) -> Val
                                                                {
            IEnumVARIANT* penum = nullptr;
            IUnknown* punkEnum = nullptr;

            IDispatch* d = nullptr;
            if (SUCCEEDED(ptr->QueryInterface(IID_IDispatch, (void**)&d)) && d) {
                DISPPARAMS noParams{};
                VARIANT result; VariantInit(&result);
                EXCEPINFO excep{};
                HRESULT hr = d->Invoke(DISPID_NEWENUM, IID_NULL, LOCALE_USER_DEFAULT,
                                       DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                                       &noParams, &result, &excep, nullptr);
                if (SUCCEEDED(hr) && result.vt == VT_UNKNOWN) punkEnum = result.punkVal;
                else if (SUCCEEDED(hr) && result.vt == VT_DISPATCH) punkEnum = result.pdispVal;
                d->Release();
            }
            if (!punkEnum) ptr->QueryInterface(IID_IUnknown, (void**)&punkEnum);
            if (punkEnum) punkEnum->QueryInterface(IID_IEnumVARIANT, (void**)&penum);
            if (!penum) { if (punkEnum) punkEnum->Release(); return nova_arr(); }

            ValVec out;
            VARIANT v; VariantInit(&v);
            ULONG fetched = 0;
            penum->Reset();
            while (SUCCEEDED(penum->Next(1, &v, &fetched)) && fetched == 1) {
                out.push_back((*variantToVal)(v));
                VariantClear(&v);
                VariantInit(&v);
            }
            penum->Release();
            if (punkEnum) punkEnum->Release();
            return nova_arr(std::move(out)); }, "ToArray"));

                // ── connection-point events ──────────────────────────────────────
                // Advise(eventIidString, {dispid: fn, ...}) -> handle object, or throws
                self->obj->set("Advise", NovaValue::makeNative(
                                             [exe, ptr, variantToVal, activeAdvises](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                             {
                                                 if (a.size() < 2 || !a[1].isObject())
                                                     exe->_error("COM.Advise: expected (eventIidString, {dispid: handlerFn, ...})");

                                                 std::string iidStr = a[0].asString();
                                                 int wlen = MultiByteToWideChar(CP_UTF8, 0, iidStr.c_str(), -1, nullptr, 0);
                                                 std::wstring wiid(wlen, 0);
                                                 MultiByteToWideChar(CP_UTF8, 0, iidStr.c_str(), -1, wiid.data(), wlen);
                                                 IID eventIid;
                                                 if (FAILED(IIDFromString(wiid.c_str(), &eventIid)))
                                                     exe->_error("COM.Advise: invalid event IID '" + iidStr + "'");

                                                 IConnectionPointContainer *cpc = nullptr;
                                                 if (FAILED(ptr->QueryInterface(IID_IConnectionPointContainer, (void **)&cpc)) || !cpc)
                                                     exe->_error("COM.Advise: object does not support IConnectionPointContainer");

                                                 IConnectionPoint *cp = nullptr;
                                                 HRESULT hr = cpc->FindConnectionPoint(eventIid, &cp);
                                                 cpc->Release();
                                                 if (FAILED(hr) || !cp)
                                                     exe->_error("COM.Advise: no connection point for that event IID");

                                                 auto *sink = new NovaEventSink(exe, cs, *variantToVal);
                                                 for (auto &[dispidStr, fn] : a[1]->obj->inner)
                                                 {
                                                     if (!fn.isFunction())
                                                         continue;
                                                     DISPID id = (DISPID)std::stol(dispidStr);
                                                     sink->handlers[id] = fn;
                                                 }

                                                 DWORD cookie = 0;
                                                 hr = cp->Advise(static_cast<IDispatch *>(sink), &cookie);
                                                 if (FAILED(hr))
                                                 {
                                                     cp->Release();
                                                     sink->Release();
                                                     exe->_error("COM.Advise: cp->Advise failed (hr=0x" +
                                                                 [&]
                                                                 { std::ostringstream ss; ss << std::hex << (unsigned long)hr; return ss.str(); }() + ")"
                                                                         " — source object likely QI'd the sink for the specific event"
                                                                         " dispinterface IID, which this generic sink doesn't answer to"
                                                                         " (see NovaEventSink::QueryInterface comment)");
                                                 }

                                                 auto handle = std::make_shared<AdviseHandle>(AdviseHandle{cp, cookie, sink});
                                                 activeAdvises->push_back(handle);

                                                 auto handleObj = nova_obj();
                                                 handleObj->obj->set("__type__", nova_str("com_advise_handle"));
                                                 handleObj->obj->set("cookie", nova_num((double)cookie));
                                                 handleObj->obj->set("Unadvise", NovaValue::makeNative([handle, activeAdvises](ValVec, auto) -> Val
                                                                                                       {
                if (handle->cp) {
                    handle->cp->Unadvise(handle->cookie);
                    handle->cp->Release();
                    handle->cp = nullptr;
                }
                if (handle->sink) {
                    handle->sink->Release();
                    handle->sink = nullptr;
                }
                activeAdvises->erase(
                    std::remove(activeAdvises->begin(), activeAdvises->end(), handle),
                    activeAdvises->end());
                return nova_null(); }, "Unadvise"));
                                                 return handleObj;
                                             },
                                             "Advise"));

                // ── raw vtable call, now with a real libffi path for float/double ──
                self->obj->set("VTableCall", NovaValue::makeNative([exe, ptr](ValVec a, auto) -> Val
                                                                   {
                                                                       if (a.size() < 4)
                                                                           exe->_error("VTableCall: expected (index, argTypes[], args[], retType)");
                                                                       int index = (int)a[0].asNumber();
                                                                       if (!a[1].isArray() || !a[2].isArray())
                                                                           exe->_error("VTableCall: argTypes and args must be arrays");
                                                                       auto &argTypeVals = a[1]->arr->inner;
                                                                       auto &argVals = a[2]->arr->inner;
                                                                       std::string retType = a[3].asString();

                                                                       void **vtbl = *(void ***)ptr.get();
                                                                       void *fn = vtbl[index];

#if defined(NOVA_HAVE_LIBFFI) && NOVA_FFI_PLATFORM_SUPPORTED
                                                                       // ── real path: build an ffi_cif including the implicit `this` ──
                                                                       auto ffiTypeFor = [exe](const std::string &t) -> ffi_type *
                                                                       {
                                                                           if (t == "void")
                                                                               return &ffi_type_void;
                                                                           if (t == "i8")
                                                                               return &ffi_type_sint8;
                                                                           if (t == "u8")
                                                                               return &ffi_type_uint8;
                                                                           if (t == "i16")
                                                                               return &ffi_type_sint16;
                                                                           if (t == "u16")
                                                                               return &ffi_type_uint16;
                                                                           if (t == "i32")
                                                                               return &ffi_type_sint32;
                                                                           if (t == "hresult")
                                                                               return &ffi_type_sint32;
                                                                           if (t == "u32")
                                                                               return &ffi_type_uint32;
                                                                           if (t == "i64")
                                                                               return &ffi_type_sint64;
                                                                           if (t == "u64")
                                                                               return &ffi_type_uint64;
                                                                           if (t == "f32")
                                                                               return &ffi_type_float;
                                                                           if (t == "f64")
                                                                               return &ffi_type_double;
                                                                           if (t == "bool")
                                                                               return &ffi_type_uint8;
                                                                           if (t == "ptr")
                                                                               return &ffi_type_pointer;
                                                                           if (t == "cstr")
                                                                               return &ffi_type_pointer;
                                                                           exe->_error("VTableCall: unknown type name '" + t + "'");
                                                                           return &ffi_type_void;
                                                                       };

                                                                       size_t n = argVals.size();
                                                                       std::vector<ffi_type *> argTypes(n + 1);
                                                                       argTypes[0] = &ffi_type_pointer; // this
                                                                       for (size_t i = 0; i < n; i++)
                                                                           argTypes[i + 1] = ffiTypeFor(i < argTypeVals.size() ? argTypeVals[i].asString() : "i32");
                                                                       ffi_type *retFfi = ffiTypeFor(retType.empty() ? "i32" : retType);

                                                                       ffi_cif cif;
                                                                       if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)(n + 1), retFfi, argTypes.data()) != FFI_OK)
                                                                           exe->_error("VTableCall: ffi_prep_cif failed");

                                                                       void *thisPtr = ptr.get();
                                                                       std::vector<int64_t> intStorage(n, 0);
                                                                       std::vector<float> f32Storage(n, 0.0f);
                                                                       std::vector<double> f64Storage(n, 0.0);
                                                                       std::vector<void *> ptrStorage(n, nullptr);
                                                                       std::vector<std::string> cstrKeep;
                                                                       cstrKeep.reserve(n);
                                                                       std::vector<void *> argPtrs(n + 1);
                                                                       argPtrs[0] = &thisPtr;

                                                                       for (size_t i = 0; i < n; i++)
                                                                       {
                                                                           std::string t = i < argTypeVals.size() ? argTypeVals[i].asString() : "i32";
                                                                           Val v = argVals[i];
                                                                           if (t == "f32")
                                                                           {
                                                                               f32Storage[i] = (float)v.asNumber();
                                                                               argPtrs[i + 1] = &f32Storage[i];
                                                                           }
                                                                           else if (t == "f64")
                                                                           {
                                                                               f64Storage[i] = v.asNumber();
                                                                               argPtrs[i + 1] = &f64Storage[i];
                                                                           }
                                                                           else if (t == "cstr")
                                                                           {
                                                                               cstrKeep.push_back(v.asString());
                                                                               ptrStorage[i] = (void *)cstrKeep.back().c_str();
                                                                               argPtrs[i + 1] = &ptrStorage[i];
                                                                           }
                                                                           else if (t == "ptr")
                                                                           {
                                                                               ptrStorage[i] = (v.isPointer() && v->ptr) ? v->ptr->rawAddr : nullptr;
                                                                               argPtrs[i + 1] = &ptrStorage[i];
                                                                           }
                                                                           else if (t == "bool")
                                                                           {
                                                                               intStorage[i] = v.asBool() ? 1 : 0;
                                                                               argPtrs[i + 1] = &intStorage[i];
                                                                           }
                                                                           else
                                                                           {
                                                                               intStorage[i] = (int64_t)v.asNumber();
                                                                               argPtrs[i + 1] = &intStorage[i];
                                                                           }
                                                                       }

                                                                       union
                                                                       {
                                                                           ffi_arg i;
                                                                           double d;
                                                                           float f;
                                                                           void *p;
                                                                       } retStorage;
                                                                       std::memset(&retStorage, 0, sizeof(retStorage));
                                                                       ffi_call(&cif, FFI_FN(fn), &retStorage, argPtrs.data());

                                                                       if (retType == "void")
                                                                           return nova_null();
                                                                       if (retType == "f32")
                                                                       {
                                                                           float fv;
                                                                           std::memcpy(&fv, &retStorage, sizeof(float));
                                                                           return nova_num((double)fv);
                                                                       }
                                                                       if (retType == "f64")
                                                                           return nova_num(retStorage.d);
                                                                       if (retType == "bool")
                                                                           return nova_bool(retStorage.i != 0);
                                                                       if (retType == "cstr")
                                                                           return retStorage.p ? nova_str(std::string((const char *)retStorage.p)) : nova_null();
                                                                       if (retType == "ptr")
                                                                           return makeRawPtrFromAddress(retStorage.p, NativeType::Ptr, 0);
                                                                       return nova_num((double)(int64_t)retStorage.i);
#else
                                                                       // ── fallback: integer-register path. Correct for int/ptr/bool/
                                                                       // cstr args and returns; float/double args here are NOT
                                                                       // guaranteed to hit the FPU registers the callee expects. Build
                                                                       // with NOVA_HAVE_LIBFFI + libffi linked to get the real path.
                                                                       std::vector<uintptr_t> callArgs;
                                                                       std::vector<std::string> cstrKeep;
                                                                       callArgs.push_back((uintptr_t)ptr.get());
                                                                       for (size_t i = 0; i < argVals.size(); i++)
                                                                       {
                                                                           std::string t = i < argTypeVals.size() ? argTypeVals[i].asString() : "i32";
                                                                           Val v = argVals[i];
                                                                           if (t == "cstr")
                                                                           {
                                                                               cstrKeep.push_back(v.asString());
                                                                               callArgs.push_back((uintptr_t)cstrKeep.back().c_str());
                                                                           }
                                                                           else if (t == "ptr")
                                                                           {
                                                                               callArgs.push_back((uintptr_t)(v.isPointer() && v->ptr ? v->ptr->rawAddr : nullptr));
                                                                           }
                                                                           else if (t == "bool")
                                                                           {
                                                                               callArgs.push_back(v.asBool() ? 1 : 0);
                                                                           }
                                                                           else if (t == "f64" || t == "f32")
                                                                           {
                                                                               double d = v.asNumber();
                                                                               uintptr_t bits;
                                                                               std::memcpy(&bits, &d, sizeof(bits) < sizeof(d) ? sizeof(bits) : sizeof(bits));
                                                                               callArgs.push_back(bits); // NOT calling-convention correct — see note above
                                                                           }
                                                                           else
                                                                           {
                                                                               callArgs.push_back((uintptr_t)(int64_t)v.asNumber());
                                                                           }
                                                                       }
                                                                       while (callArgs.size() < 9)
                                                                           callArgs.push_back(0);
                                                                       using Fn9 = uintptr_t(__stdcall *)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
                                                                       uintptr_t result = reinterpret_cast<Fn9>(fn)(
                                                                           callArgs[0], callArgs[1], callArgs[2], callArgs[3],
                                                                           callArgs[4], callArgs[5], callArgs[6], callArgs[7], callArgs[8]);
                                                                       if (retType == "void")
                                                                           return nova_null();
                                                                       if (retType == "bool")
                                                                           return nova_bool(result != 0);
                                                                       if (retType == "cstr")
                                                                           return result ? nova_str((const char *)result) : nova_null();
                                                                       if (retType == "ptr")
                                                                           return makeRawPtrFromAddress((void *)result, NativeType::Ptr, 0);
                                                                       return nova_num((double)(int64_t)result);
#endif
                                                                   },
                                                                   "VTableCall"));

                // ── IPersistFile: Load/Save common pattern ───────────────────────
                self->obj->set("PersistLoad", NovaValue::makeNative([exe, ptr, utf8ToWide](ValVec a, auto) -> Val
                                                                    {
            if (a.empty()) exe->_error("PersistLoad: expected a file path");
            IPersistFile* pf = nullptr;
            if (FAILED(ptr->QueryInterface(IID_IPersistFile, (void**)&pf)) || !pf)
                exe->_error("PersistLoad: object does not support IPersistFile");
            std::wstring w = utf8ToWide(a[0].asString());
            bool readonly = a.size() > 1 && a[1].asBool();
            HRESULT hr = pf->Load(w.c_str(), readonly ? STGM_READ : STGM_READWRITE);
            pf->Release();
            return nova_bool(SUCCEEDED(hr)); }, "PersistLoad"));

                self->obj->set("PersistSave", NovaValue::makeNative([exe, ptr, utf8ToWide](ValVec a, auto) -> Val
                                                                    {
            if (a.empty()) exe->_error("PersistSave: expected a file path");
            IPersistFile* pf = nullptr;
            if (FAILED(ptr->QueryInterface(IID_IPersistFile, (void**)&pf)) || !pf)
                exe->_error("PersistSave: object does not support IPersistFile");
            std::wstring w = utf8ToWide(a[0].asString());
            HRESULT hr = pf->Save(w.c_str(), TRUE);
            if (SUCCEEDED(hr)) pf->SaveCompleted(w.c_str());
            pf->Release();
            return nova_bool(SUCCEEDED(hr)); }, "PersistSave"));

                return self;
            };

            // ────────────────────────────────────────────────────────────────────────
            //  Std.Windows.COM.* top-level functions
            // ────────────────────────────────────────────────────────────────────────
            comObj->obj->set("Initialize", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
        bool mt = !a.empty() && a[0].asBool();
        HRESULT hr = CoInitializeEx(nullptr, mt ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
        return nova_bool(SUCCEEDED(hr)); }, "Initialize"));

            comObj->obj->set("Uninitialize", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   {
        CoUninitialize();
        return nova_null(); }, "Uninitialize"));

            // Pump the Windows message queue for `ms` milliseconds (or until empty
            // if ms <= 0 for a single pass). REQUIRED for connection-point events to
            // fire on an STA-created source object. Call this periodically (e.g. in
            // a loop, or dedicate a thread to it) while events are active.
            comObj->obj->set("PumpMessages", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
        long long ms = a.empty() ? 0 : (long long)a[0].asNumber();
        MSG msg;
        auto start = std::chrono::steady_clock::now();
        do {
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (ms <= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start).count() < ms);
        return nova_null(); }, "PumpMessages"));

            comObj->obj->set("CreateInstance", NovaValue::makeNative([exe, wrapComPtr](ValVec a, auto) -> Val
                                                                     {
        if (a.empty()) exe->_error("COM.CreateInstance: expected a ProgID or CLSID string");
        std::string idStr = a[0].asString();
        int wlen = MultiByteToWideChar(CP_UTF8, 0, idStr.c_str(), -1, nullptr, 0);
        std::wstring wid(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, idStr.c_str(), -1, wid.data(), wlen);

        CLSID clsid;
        HRESULT hr = (idStr.size() > 1 && idStr.front() == '{')
            ? CLSIDFromString(wid.c_str(), &clsid)
            : CLSIDFromProgID(wid.c_str(), &clsid);
        if (FAILED(hr)) exe->_error("COM.CreateInstance: could not resolve '" + idStr + "'");

        IUnknown* punk = nullptr;
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IUnknown, (void**)&punk);
        if (FAILED(hr) || !punk)
            exe->_error("COM.CreateInstance: CoCreateInstance failed (hr=" + std::to_string(hr) + ")");

        return (*wrapComPtr)(punk, false); }, "CreateInstance"));

            // ── IClassFactory / CoGetClassObject ──────────────────────────────────
            // GetClassObject(progIdOrClsid) -> factory object exposing
            //   .CreateInstance() -> com_object
            //   .LockServer(bool)
            comObj->obj->set("GetClassObject", NovaValue::makeNative([exe, wrapComPtr](ValVec a, auto) -> Val
                                                                     {
        if (a.empty()) exe->_error("COM.GetClassObject: expected a ProgID or CLSID string");
        std::string idStr = a[0].asString();
        int wlen = MultiByteToWideChar(CP_UTF8, 0, idStr.c_str(), -1, nullptr, 0);
        std::wstring wid(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, idStr.c_str(), -1, wid.data(), wlen);

        CLSID clsid;
        HRESULT hr = (idStr.size() > 1 && idStr.front() == '{')
            ? CLSIDFromString(wid.c_str(), &clsid)
            : CLSIDFromProgID(wid.c_str(), &clsid);
        if (FAILED(hr)) exe->_error("COM.GetClassObject: could not resolve '" + idStr + "'");

        IClassFactory* cf = nullptr;
        hr = CoGetClassObject(clsid, CLSCTX_ALL, nullptr, IID_IClassFactory, (void**)&cf);
        if (FAILED(hr) || !cf)
            exe->_error("COM.GetClassObject: CoGetClassObject failed (hr=" + std::to_string(hr) + ")");

        auto cfPtr = std::shared_ptr<IClassFactory>(cf, [](IClassFactory* p) { if (p) p->Release(); });
        auto out = nova_obj();
        out->obj->set("__type__", nova_str("com_class_factory"));

        out->obj->set("CreateInstance", NovaValue::makeNative([exe, cfPtr, wrapComPtr](ValVec, auto) -> Val {
            IUnknown* punk = nullptr;
            HRESULT hr = cfPtr->CreateInstance(nullptr, IID_IUnknown, (void**)&punk);
            if (FAILED(hr) || !punk)
                exe->_error("ClassFactory.CreateInstance failed (hr=" + std::to_string(hr) + ")");
            return (*wrapComPtr)(punk, false);
        }, "CreateInstance"));

        out->obj->set("LockServer", NovaValue::makeNative([cfPtr](ValVec a, auto) -> Val {
            BOOL lock = a.empty() || a[0].asBool();
            return nova_bool(SUCCEEDED(cfPtr->LockServer(lock)));
        }, "LockServer"));

        return out; }, "GetClassObject"));

            comObj->obj->set("GetActiveObject", NovaValue::makeNative([exe, wrapComPtr](ValVec a, auto) -> Val
                                                                      {
        if (a.empty()) exe->_error("COM.GetActiveObject: expected a ProgID or CLSID string");
        std::string idStr = a[0].asString();
        int wlen = MultiByteToWideChar(CP_UTF8, 0, idStr.c_str(), -1, nullptr, 0);
        std::wstring wid(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, idStr.c_str(), -1, wid.data(), wlen);

        CLSID clsid;
        HRESULT hr = (idStr.size() > 1 && idStr.front() == '{')
            ? CLSIDFromString(wid.c_str(), &clsid)
            : CLSIDFromProgID(wid.c_str(), &clsid);
        if (FAILED(hr)) exe->_error("COM.GetActiveObject: could not resolve '" + idStr + "'");

        IUnknown* punk = nullptr;
        hr = GetActiveObject(clsid, nullptr, &punk);
        if (FAILED(hr) || !punk) return nova_null();
        return (*wrapComPtr)(punk, false); }, "GetActiveObject"));

            // ── moniker binding: BindToObject(displayName) -> com_object ──────────
            // e.g. "winmgmts:" style or file monikers. Uses the default bind context.
            comObj->obj->set("BindToObject", NovaValue::makeNative([exe, wrapComPtr, utf8ToWide](ValVec a, auto) -> Val
                                                                   {
        if (a.empty()) exe->_error("COM.BindToObject: expected a display name string");
        std::wstring wname = utf8ToWide(a[0].asString());

        IBindCtx* bindCtx = nullptr;
        HRESULT hr = CreateBindCtx(0, &bindCtx);
        if (FAILED(hr)) exe->_error("COM.BindToObject: CreateBindCtx failed");

        IMoniker* moniker = nullptr;
        ULONG eaten = 0;
        hr = MkParseDisplayName(bindCtx, wname.c_str(), &eaten, &moniker);
        if (FAILED(hr) || !moniker) {
            bindCtx->Release();
            exe->_error("COM.BindToObject: could not parse display name '" + a[0].asString() + "'");
        }

        IUnknown* punk = nullptr;
        hr = moniker->BindToObject(bindCtx, nullptr, IID_IUnknown, (void**)&punk);
        moniker->Release();
        bindCtx->Release();
        if (FAILED(hr) || !punk)
            exe->_error("COM.BindToObject: BindToObject failed (hr=" + std::to_string(hr) + ")");

        return (*wrapComPtr)(punk, false); }, "BindToObject"));

            comObj->obj->set("FromRawPtr", NovaValue::makeNative([exe, wrapComPtr](ValVec a, auto) -> Val
                                                                 {
        if (a.empty() || !a[0].isPointer() || !a[0]->ptr) return nova_null();
        return (*wrapComPtr)((IUnknown*)a[0]->ptr->rawAddr, true); }, "FromRawPtr"));

            // ────────────────────────────────────────────────────────────────────
            //  COM.MakeVTable — build a real, callable COM object (vtable +
            //  QueryInterface/AddRef/Release + N user methods) at runtime, so
            //  native code (WebView2, custom connection points, any interface
            //  that calls INTO you) can invoke Nova functions as if they were
            //  compiled C++ vtable slots.
            //
            //  Requires libffi (NOVA_CCALL_AVAILABLE) because it uses
            //  ffi_closure to synthesize real executable trampolines — one per
            //  declared method — that marshal native args to Nova values,
            //  call the Nova handler, and marshal the return value back.
            //
            //  Usage:
            //    let vt = Std.Windows.COM.MakeVTable([
            //        { retType: "hresult", argTypes: ["hresult","ptr"],
            //          handler: (hr, env) => { ...; return 0; } }
            //    ]);
            //    Std.Ccall(someFn, "hresult", ["ptr"], [vt.ptr()]);
            //    vt.release();
            //
            //  Honest limitations:
            //   - QueryInterface always succeeds and hands back the same
            //     object/vtable regardless of requested IID. Correct for
            //     single-interface callback objects (the common WebView2 /
            //     connection-point pattern) but not for objects that must
            //     answer distinct IIDs with distinct vtables.
            //   - Supported marshaled types: i32, u32, i64, f64, f32, bool,
            //     ptr, cstr, hresult (alias of i32). No struct-by-value args.
            //   - The object must be kept alive (don't call .release() early)
            //     for as long as native code may still call into it.
            // ────────────────────────────────────────────────────────────────────
#if NOVA_CCALL_AVAILABLE
            comObj->obj->set("MakeVTable", NovaValue::makeNative(
                                               [exe](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                               {
                                                   if (a.empty() || !a[0].isArray())
                                                       exe->_error("COM.MakeVTable: expected an array of {retType, argTypes, handler}");

                                                   auto *self = new NovaVTableObject();
                                                   self->exe = exe;
                                                   self->scope = cs;

                                                   auto &methods = a[0]->arr->inner;
                                                   self->handlers.resize(methods.size());
                                                   self->argTypeStorage.resize(methods.size());
                                                   self->cifs.resize(methods.size());
                                                   self->closures.resize(methods.size());
                                                   self->closureCodePtrs.resize(methods.size());

                                                   // slots 0-2 are the fixed IUnknown methods; one slot per user method follows
                                                   self->vtableSlots.push_back((void *)NovaVT_QueryInterface);
                                                   self->vtableSlots.push_back((void *)NovaVT_AddRef);
                                                   self->vtableSlots.push_back((void *)NovaVT_Release);

                                                   for (size_t i = 0; i < methods.size(); i++)
                                                   {
                                                       Val m = methods[i];
                                                       if (!m || !m.isObject())
                                                           exe->_error("COM.MakeVTable: method " + std::to_string(i) + " must be an object");

                                                       std::string retType = m->obj->get("retType") ? m->obj->get("retType").asString() : "hresult";
                                                       Val handler = m->obj->get("handler");
                                                       if (!handler || !handler.isFunction())
                                                           exe->_error("COM.MakeVTable: method " + std::to_string(i) + " missing a handler function");
                                                       self->handlers[i] = handler;

                                                       std::vector<std::string> argTypeNames;
                                                       Val argTypesVal = m->obj->get("argTypes");
                                                       if (argTypesVal && argTypesVal.isArray())
                                                           for (auto &t : argTypesVal->arr->inner)
                                                               argTypeNames.push_back(t.asString());

                                                       // ffi_cif: implicit "this" (ptr) + declared args
                                                       auto &ffiArgs = self->argTypeStorage[i];
                                                       ffiArgs.push_back(&ffi_type_pointer); // this
                                                       for (auto &t : argTypeNames)
                                                           ffiArgs.push_back(novaVTableFfiTypeForName(t));
                                                       ffi_type *retFfi = novaVTableFfiTypeForName(retType);

                                                       if (ffi_prep_cif(&self->cifs[i], FFI_DEFAULT_ABI, (unsigned)ffiArgs.size(),
                                                                        retFfi, ffiArgs.data()) != FFI_OK)
                                                       {
                                                           NovaVT_Release(self);
                                                           exe->_error("COM.MakeVTable: ffi_prep_cif failed for method " + std::to_string(i));
                                                       }

                                                       void *codePtr = nullptr;
                                                       ffi_closure *closure = (ffi_closure *)ffi_closure_alloc(sizeof(ffi_closure), &codePtr);
                                                       if (!closure)
                                                       {
                                                           NovaVT_Release(self);
                                                           exe->_error("COM.MakeVTable: ffi_closure_alloc failed for method " + std::to_string(i));
                                                       }

                                                       // Owned by the closure for its lifetime; freed alongside the
                                                       // closures vector when the object's refcount hits zero.
                                                       auto *ctx = new ClosureCtx{self, i, retType, argTypeNames};
                                                       self->closureContexts.push_back(ctx);

                                                       if (ffi_prep_closure_loc(closure, &self->cifs[i], novaVTableClosureHandler, ctx, codePtr) != FFI_OK)
                                                       {
                                                           NovaVT_Release(self);
                                                           exe->_error("COM.MakeVTable: ffi_prep_closure_loc failed for method " + std::to_string(i));
                                                       }

                                                       self->closures[i] = closure;
                                                       self->closureCodePtrs[i] = codePtr;
                                                       self->vtableSlots.push_back(codePtr);
                                                   }

                                                   self->vtbl = self->vtableSlots.data();

                                                   auto out = nova_obj();
                                                   out->obj->set("__type__", nova_str("com_vtable_object"));

                                                   out->obj->set("ptr", NovaValue::makeNative([self](ValVec, auto) -> Val
                                                                                              {
                    // The "object pointer" a COM caller sees is the address of
                    // the vtable-pointer field itself (self == &self->vtbl,
                    // since vtbl is the first member).
                    return makeRawPtrFromAddress(self, NativeType::Ptr, 0); }, "ptr"));

                                                   out->obj->set("release", NovaValue::makeNative([self](ValVec, auto) -> Val
                                                                                                  {
                    NovaVT_Release(self);
                    return nova_null(); }, "release"));

                                                   out->obj->set("addRef", NovaValue::makeNative([self](ValVec, auto) -> Val
                                                                                                 { return nova_num((double)NovaVT_AddRef(self)); }, "addRef"));

                                                   out->obj->set("refCount", NovaValue::makeNative([self](ValVec, auto) -> Val
                                                                                                   { return nova_num((double)self->refCount.load()); }, "refCount"));

                                                   return out;
                                               },
                                               "MakeVTable"));
#endif // NOVA_CCALL_AVAILABLE

            winObj->obj->set("COM", comObj);
            std_obj->obj->set("Windows", winObj);
        }
#else
        std_obj->obj->set("Windows", nova_null());
#endif
        // ════════════════════════════════════════════════════════════════════════════
        //  Std.Windows.GUI — Win32 window/controls/GDI bridge
        // ════════════════════════════════════════════════════════════════════════════
        //
        //  PASTE LOCATION: directly before  s->setOwn("Std", std_obj);  — can go
        //  before or after the Std.Windows.COM block; both fetch/reuse the same
        //  "Windows" namespace object off std_obj so they compose regardless of order.
        //
        //  REQUIRED INCLUDES (top of vm.cpp, alongside your existing _WIN32 block):
        //      #include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM etc.
        //      #include <commctrl.h>   // buttons, edits, listbox, combobox, common ctrls
        //      #include <commdlg.h>    // GetOpenFileName / GetSaveFileName / ChooseColor
        //
        //  REQUIRED LINK LIBS (Windows only): user32.lib gdi32.lib comctl32.lib comdlg32.lib
        //
        //  ── Honest scope notes (read before relying on this) ────────────────────────
        //
        //  1. Every top-level window shares ONE registered window class
        //     ("NovaGenericWindow") with ONE static WndProc. Per-window behavior is
        //     dispatched through a per-HWND handler table (GWLP_USERDATA -> a
        //     NovaWindowState -> map<UINT, Val> of Nova callbacks). You can't
        //     customize class styles/icon/cursor per-window, only the one set at
        //     RegisterClassExW time.
        //
        //  2. Controls (button/edit/static/checkbox/listbox/combobox) are real
        //     child HWNDs using the standard Windows control class names — not the
        //     full Common Controls v6 set. No ListView/TreeView/TabControl/
        //     ProgressBar/etc. here; each would need its own wrapper.
        //
        //  3. GDI drawing is classic GDI, not GDI+/Direct2D — no alpha blending or
        //     anti-aliasing.
        //
        //  4. The message loop is a genuine blocking GetMessage loop
        //     (GUI.RunMessageLoop()). GUI.PumpOnce(ms) is the non-blocking
        //     alternative (same underlying queue Std.Windows.COM.PumpMessages
        //     drains — GUI windows and COM connection-point events on an STA thread
        //     share one message queue, so you only need to pump once per thread).
        //
        //  5. Nova callbacks run synchronously ON the Win32 message-pump thread.
        //     A slow handler freezes repainting/input, same as any Win32 app.
        //     Don't block in handlers.
        //
        //  6. Windows are thread-affine, same as raw Win32 — this bridge adds no
        //     marshaling of its own. Create and manipulate a given window from one
        //     thread.
        //
        //  7. Still not covered (full list at the bottom of the file): richer common
        //     controls, GDI+, Direct2D/3D, drag-and-drop, accessibility (UI
        //     Automation/MSAA), per-monitor-v2 DPI handling, owner-drawn controls,
        //     custom window classes/styles, tray icons, global hotkeys, raw input,
        //     DirectWrite, printing.
        //
        // ════════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
        {
            // reuse the "Windows" namespace object if Std.Windows.COM already created it
            Val existingWin = std_obj->obj->get("Windows");
            auto winObj = (existingWin && existingWin.isObject()) ? existingWin : nova_obj();

            auto guiObj = nova_obj();
            auto exe = this;

            static bool s_wndClassRegistered = false;
            static const wchar_t *s_wndClassName = L"NovaGenericWindow";
            static std::atomic<int> s_nextCtrlId{1000};

            auto utf8ToWide = [](const std::string &s) -> std::wstring
            {
                if (s.empty())
                    return L"";
                int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
                std::wstring w(wlen, 0);
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
                if (!w.empty() && w.back() == L'\0')
                    w.pop_back();
                return w;
            };
            auto wideToUtf8 = [](const wchar_t *w, int len = -1) -> std::string
            {
                if (!w)
                    return "";
                int u8len = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
                std::string out(u8len > 0 ? u8len : 0, '\0');
                if (u8len > 0)
                    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), u8len, nullptr, nullptr);
                if (len < 0 && !out.empty() && out.back() == '\0')
                    out.pop_back();
                return out;
            };

            // ────────────────────────────────────────────────────────────────────────
            //  per-top-level-window state: created for windows made via
            //  GUI.CreateWindow, NOT for plain child controls (those route
            //  WM_COMMAND back to their parent's state instead).
            // ────────────────────────────────────────────────────────────────────────
            struct NovaWindowState
            {
                Executor *exe;
                std::shared_ptr<Scope> scope;
                std::unordered_map<UINT, Val> handlers;       // raw message id -> handler
                std::unordered_map<int, Val> commandHandlers; // child control id -> handler
                std::unordered_map<int, Val> timerHandlers;   // timer id -> handler
                HWND hwnd = nullptr;
            };

            static auto novaWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT
            {
                auto *state = reinterpret_cast<NovaWindowState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

                if (msg == WM_NCCREATE)
                {
                    auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
                }
                if (!state)
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
                state->hwnd = hwnd;

                if (msg == WM_COMMAND)
                {
                    int ctrlId = LOWORD(wParam);
                    auto it = state->commandHandlers.find(ctrlId);
                    if (it != state->commandHandlers.end())
                    {
                        try
                        {
                            state->exe->callFunction(it->second,
                                                     {nova_num((double)ctrlId), nova_num((double)HIWORD(wParam))}, state->scope);
                        }
                        catch (...)
                        {
                        }
                        return 0;
                    }
                }

                if (msg == WM_TIMER)
                {
                    auto it = state->timerHandlers.find((int)wParam);
                    if (it != state->timerHandlers.end())
                    {
                        try
                        {
                            state->exe->callFunction(it->second, {}, state->scope);
                        }
                        catch (...)
                        {
                        }
                        return 0;
                    }
                }

                auto it = state->handlers.find(msg);
                if (it != state->handlers.end())
                {
                    Val result;
                    try
                    {
                        result = state->exe->callFunction(it->second,
                                                          {nova_num((double)wParam), nova_num((double)lParam)}, state->scope);
                    }
                    catch (...)
                    {
                        result = nova_null();
                    }
                    if (result && result.isNumber())
                        return (LRESULT)result->nval;
                }

                if (msg == WM_NCDESTROY)
                {
                    delete state;
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
                }

                return DefWindowProcW(hwnd, msg, wParam, lParam);
            };

            auto ensureWndClassRegistered = [exe]()
            {
                if (s_wndClassRegistered)
                    return;
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(WNDCLASSEXW);
                wc.style = CS_HREDRAW | CS_VREDRAW;
                wc.lpfnWndProc = novaWndProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
                wc.lpszClassName = s_wndClassName;
                wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
                if (!RegisterClassExW(&wc))
                    exe->_error("GUI: RegisterClassExW failed");

                INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
                InitCommonControlsEx(&icc);
                s_wndClassRegistered = true;
            };

            // ────────────────────────────────────────────────────────────────────────
            //  wrap ANY hwnd (top-level window or child control) as a Nova object.
            //  The raw HWND is stashed as a plain (non-function) numeric property
            //  "__hwndAddr__" so other builtins (CreateButton etc.) can retrieve it
            //  directly without round-tripping through a native-function call.
            // ────────────────────────────────────────────────────────────────────────
            auto wrapHwnd = std::make_shared<std::function<Val(HWND, NovaWindowState *)>>();
            *wrapHwnd = [exe, utf8ToWide, wideToUtf8](HWND hwnd, NovaWindowState *state) -> Val
            {
                auto self = nova_obj();
                self->obj->set("__type__", nova_str(state ? "gui_window" : "gui_control"));
                self->obj->set("__hwndAddr__", nova_num((double)(uintptr_t)hwnd));

                self->obj->set("handle", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                               {
            std::ostringstream ss; ss << "0x" << std::hex << (uintptr_t)hwnd;
            return nova_str(ss.str()); }, "handle"));

                self->obj->set("Show", NovaValue::makeNative([hwnd](ValVec a, auto) -> Val
                                                             {
            ShowWindow(hwnd, a.empty() ? SW_SHOW : (int)a[0].asNumber());
            UpdateWindow(hwnd);
            return nova_null(); }, "Show"));

                self->obj->set("Hide", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                             {
            ShowWindow(hwnd, SW_HIDE);
            return nova_null(); }, "Hide"));

                self->obj->set("Close", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                              {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return nova_null(); }, "Close"));

                self->obj->set("Destroy", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                                {
            DestroyWindow(hwnd);
            return nova_null(); }, "Destroy"));

                self->obj->set("SetTitle", NovaValue::makeNative([hwnd, utf8ToWide](ValVec a, auto) -> Val
                                                                 {
            if (!a.empty()) SetWindowTextW(hwnd, utf8ToWide(a[0].asString()).c_str());
            return nova_null(); }, "SetTitle"));

                self->obj->set("GetTitle", NovaValue::makeNative([hwnd, wideToUtf8](ValVec, auto) -> Val
                                                                 {
            wchar_t buf[1024];
            int n = GetWindowTextW(hwnd, buf, 1024);
            return nova_str(wideToUtf8(buf, n)); }, "GetTitle"));

                self->obj->set("SetText", self->obj->get("SetTitle")); // alias, natural on edit/static/button
                self->obj->set("GetText", self->obj->get("GetTitle"));

                self->obj->set("Move", NovaValue::makeNative([hwnd](ValVec a, auto) -> Val
                                                             {
            if (a.size() < 4) return nova_null();
            MoveWindow(hwnd, (int)a[0].asNumber(), (int)a[1].asNumber(),
                       (int)a[2].asNumber(), (int)a[3].asNumber(), TRUE);
            return nova_null(); }, "Move"));

                self->obj->set("GetRect", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                                {
            RECT r{}; GetWindowRect(hwnd, &r);
            auto out = nova_obj();
            out->obj->set("left", nova_num(r.left));
            out->obj->set("top", nova_num(r.top));
            out->obj->set("right", nova_num(r.right));
            out->obj->set("bottom", nova_num(r.bottom));
            out->obj->set("width", nova_num(r.right - r.left));
            out->obj->set("height", nova_num(r.bottom - r.top));
            return out; }, "GetRect"));

                self->obj->set("Invalidate", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                                   {
            InvalidateRect(hwnd, nullptr, TRUE);
            return nova_null(); }, "Invalidate"));

                self->obj->set("Enable", NovaValue::makeNative([hwnd](ValVec a, auto) -> Val
                                                               {
            EnableWindow(hwnd, a.empty() || a[0].asBool());
            return nova_null(); }, "Enable"));

                self->obj->set("Focus", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                              {
            SetFocus(hwnd);
            return nova_null(); }, "Focus"));

                // ── checkbox helpers (harmless no-ops on other control types) ──────
                self->obj->set("SetChecked", NovaValue::makeNative([hwnd](ValVec a, auto) -> Val
                                                                   {
            bool checked = a.empty() || a[0].asBool();
            SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
            return nova_null(); }, "SetChecked"));
                self->obj->set("IsChecked", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                                  { return nova_bool(SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED); }, "IsChecked"));

                // ── listbox / combobox helpers ──────────────────────────────────
                self->obj->set("AddItem", NovaValue::makeNative([hwnd, utf8ToWide](ValVec a, auto) -> Val
                                                                {
            if (a.empty()) return nova_num(-1);
            std::wstring w = utf8ToWide(a[0].asString());
            LRESULT idx = SendMessageW(hwnd, CB_ADDSTRING, 0, (LPARAM)w.c_str());
            if (idx == CB_ERR) idx = SendMessageW(hwnd, LB_ADDSTRING, 0, (LPARAM)w.c_str());
            return nova_num((double)idx); }, "AddItem"));

                self->obj->set("ClearItems", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                                   {
            SendMessageW(hwnd, CB_RESETCONTENT, 0, 0);
            SendMessageW(hwnd, LB_RESETCONTENT, 0, 0);
            return nova_null(); }, "ClearItems"));

                self->obj->set("GetSelectedIndex", NovaValue::makeNative([hwnd](ValVec, auto) -> Val
                                                                         {
            LRESULT idx = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
            if (idx == CB_ERR) idx = SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
            return nova_num((double)idx); }, "GetSelectedIndex"));

                self->obj->set("SetSelectedIndex", NovaValue::makeNative([hwnd](ValVec a, auto) -> Val
                                                                         {
            int idx = a.empty() ? 0 : (int)a[0].asNumber();
            SendMessageW(hwnd, CB_SETCURSEL, idx, 0);
            SendMessageW(hwnd, LB_SETCURSEL, idx, 0);
            return nova_null(); }, "SetSelectedIndex"));

                self->obj->set("GetSelectedText", NovaValue::makeNative([hwnd, wideToUtf8](ValVec, auto) -> Val
                                                                        {
            wchar_t buf[1024];
            GetWindowTextW(hwnd, buf, 1024); // works for combobox display text
            return nova_str(wideToUtf8(buf)); }, "GetSelectedText"));

                // ── raw-message handlers: only meaningful on top-level windows ──
                if (state)
                {
                    self->obj->set("On", NovaValue::makeNative([state](ValVec a, auto) -> Val
                                                               {
                if (a.size() < 2 || !a[1].isFunction()) return nova_bool(false);
                state->handlers[(UINT)a[0].asNumber()] = a[1];
                return nova_bool(true); }, "On"));

                    self->obj->set("Off", NovaValue::makeNative([state](ValVec a, auto) -> Val
                                                                {
                if (a.empty()) return nova_bool(false);
                return nova_bool(state->handlers.erase((UINT)a[0].asNumber()) > 0); }, "Off"));

                    // ── timers ────────────────────────────────────────────────────
                    self->obj->set("SetTimer", NovaValue::makeNative(
                                                   [hwnd, state](ValVec a, auto) -> Val
                                                   {
                                                       if (a.size() < 2 || !a[1].isFunction())
                                                           return nova_num(0);
                                                       UINT ms = (UINT)a[0].asNumber();
                                                       static std::atomic<int> s_nextTimerId{1};
                                                       int id = s_nextTimerId++;
                                                       state->timerHandlers[id] = a[1];
                                                       SetTimer(hwnd, (UINT_PTR)id, ms, nullptr);
                                                       return nova_num((double)id);
                                                   },
                                                   "SetTimer"));

                    self->obj->set("KillTimer", NovaValue::makeNative(
                                                    [hwnd, state](ValVec a, auto) -> Val
                                                    {
                                                        if (a.empty())
                                                            return nova_bool(false);
                                                        int id = (int)a[0].asNumber();
                                                        KillTimer(hwnd, (UINT_PTR)id);
                                                        state->timerHandlers.erase(id);
                                                        return nova_bool(true);
                                                    },
                                                    "KillTimer"));

                    // ── menu: SetMenu({label: [{label, id}, ...], ...}) ─────────
                    // Builds a simple top-level menu bar with one level of submenus.
                    // Selecting an item sends WM_COMMAND with the given numeric id,
                    // handle it via win.On(WM_COMMAND, fn) or the simpler OnMenu below.
                    self->obj->set("SetMenu", NovaValue::makeNative(
                                                  [hwnd, utf8ToWide](ValVec a, auto) -> Val
                                                  {
                                                      if (a.empty() || !a[0].isObject())
                                                          return nova_bool(false);
                                                      HMENU menuBar = CreateMenu();
                                                      for (auto &[topLabel, itemsVal] : a[0]->obj->inner)
                                                      {
                                                          HMENU sub = CreatePopupMenu();
                                                          if (itemsVal.isArray())
                                                          {
                                                              for (auto &item : itemsVal->arr->inner)
                                                              {
                                                                  if (!item.isObject())
                                                                      continue;
                                                                  Val labelV = item->obj->get("label");
                                                                  Val idV = item->obj->get("id");
                                                                  std::wstring wlabel = utf8ToWide(labelV ? labelV.asString() : "");
                                                                  UINT id = idV ? (UINT)idV.asNumber() : 0;
                                                                  AppendMenuW(sub, MF_STRING, id, wlabel.c_str());
                                                              }
                                                          }
                                                          AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)sub, utf8ToWide(topLabel).c_str());
                                                      }
                                                      ::SetMenu(hwnd, menuBar);
                                                      return nova_bool(true);
                                                  },
                                                  "SetMenu"));

                    self->obj->set("OnMenu", NovaValue::makeNative(
                                                 [state](ValVec a, auto) -> Val
                                                 {
                                                     if (a.size() < 2 || !a[1].isFunction())
                                                         return nova_bool(false);
                                                     state->commandHandlers[(int)a[0].asNumber()] = a[1];
                                                     return nova_bool(true);
                                                 },
                                                 "OnMenu"));

                    // ── GDI paint convenience wrapper ─────────────────────────────
                    self->obj->set("OnPaint", NovaValue::makeNative(
                                                  [exe, hwnd, state](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                  {
                                                      if (a.empty() || !a[0].isFunction())
                                                          return nova_bool(false);
                                                      Val drawFn = a[0];
                                                      state->handlers[WM_PAINT] = NovaValue::makeNative(
                                                          [exe, hwnd, drawFn](ValVec, std::shared_ptr<Scope> cs2) -> Val
                                                          {
                                                              PAINTSTRUCT ps;
                                                              HDC hdc = BeginPaint(hwnd, &ps);

                                                              auto dc = nova_obj();
                                                              dc->obj->set("__type__", nova_str("gui_dc"));

                                                              dc->obj->set("Line", NovaValue::makeNative([hdc](ValVec la, auto) -> Val
                                                                                                         {
                        if (la.size() < 4) return nova_null();
                        MoveToEx(hdc, (int)la[0].asNumber(), (int)la[1].asNumber(), nullptr);
                        LineTo(hdc, (int)la[2].asNumber(), (int)la[3].asNumber());
                        return nova_null(); }, "Line"));

                                                              dc->obj->set("Rect", NovaValue::makeNative([hdc](ValVec la, auto) -> Val
                                                                                                         {
                        if (la.size() < 4) return nova_null();
                        Rectangle(hdc, (int)la[0].asNumber(), (int)la[1].asNumber(),
                                       (int)la[2].asNumber(), (int)la[3].asNumber());
                        return nova_null(); }, "Rect"));

                                                              dc->obj->set("Ellipse", NovaValue::makeNative([hdc](ValVec la, auto) -> Val
                                                                                                            {
                        if (la.size() < 4) return nova_null();
                        Ellipse(hdc, (int)la[0].asNumber(), (int)la[1].asNumber(),
                                     (int)la[2].asNumber(), (int)la[3].asNumber());
                        return nova_null(); }, "Ellipse"));

                                                              dc->obj->set("Text", NovaValue::makeNative([hdc](ValVec la, auto) -> Val
                                                                                                         {
                        if (la.size() < 3) return nova_null();
                        std::string s = la[2].asString();
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
                        std::wstring w(wlen, 0);
                        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
                        TextOutW(hdc, (int)la[0].asNumber(), (int)la[1].asNumber(), w.c_str(), (int)w.size() - 1);
                        return nova_null(); }, "Text"));

                                                              dc->obj->set("SetTextColor", NovaValue::makeNative([hdc](ValVec la, auto) -> Val
                                                                                                                 {
                        if (la.size() < 3) return nova_null();
                        ::SetTextColor(hdc, RGB((int)la[0].asNumber(), (int)la[1].asNumber(), (int)la[2].asNumber()));
                        return nova_null(); }, "SetTextColor"));

                                                              dc->obj->set("SetBkColor", NovaValue::makeNative([hdc](ValVec la, auto) -> Val
                                                                                                               {
                        if (la.size() < 3) return nova_null();
                        ::SetBkColor(hdc, RGB((int)la[0].asNumber(), (int)la[1].asNumber(), (int)la[2].asNumber()));
                        return nova_null(); }, "SetBkColor"));

                                                              dc->obj->set("SetBkTransparent", NovaValue::makeNative([hdc](ValVec, auto) -> Val
                                                                                                                     {
                        ::SetBkMode(hdc, TRANSPARENT);
                        return nova_null(); }, "SetBkTransparent"));

                                                              // Selects a new pen/brush into the DC; the previously
                                                              // selected object is deleted only if it's one we created
                                                              // (tracked via a per-call local, not stock-object-safe
                                                              // across repaints — acceptable for the common "set once
                                                              // near the top of your paint handler" usage pattern).
                                                              dc->obj->set("SetPen", NovaValue::makeNative([hdc](ValVec la, auto) -> Val
                                                                                                           {
                        int width = la.size() > 0 ? (int)la[0].asNumber() : 1;
                        int r = la.size() > 1 ? (int)la[1].asNumber() : 0;
                        int g = la.size() > 2 ? (int)la[2].asNumber() : 0;
                        int b = la.size() > 3 ? (int)la[3].asNumber() : 0;
                        HPEN pen = CreatePen(PS_SOLID, width, RGB(r, g, b));
                        SelectObject(hdc, pen);
                        return nova_null(); }, "SetPen"));

                                                              dc->obj->set("SetBrush", NovaValue::makeNative([hdc](ValVec la, auto) -> Val
                                                                                                             {
                        int r = la.size() > 0 ? (int)la[0].asNumber() : 255;
                        int g = la.size() > 1 ? (int)la[1].asNumber() : 255;
                        int b = la.size() > 2 ? (int)la[2].asNumber() : 255;
                        HBRUSH brush = CreateSolidBrush(RGB(r, g, b));
                        SelectObject(hdc, brush);
                        return nova_null(); }, "SetBrush"));

                                                              dc->obj->set("width", nova_num(ps.rcPaint.right - ps.rcPaint.left));
                                                              dc->obj->set("height", nova_num(ps.rcPaint.bottom - ps.rcPaint.top));

                                                              try
                                                              {
                                                                  exe->callFunction(drawFn, {dc}, cs2);
                                                              }
                                                              catch (...)
                                                              {
                                                              }

                                                              EndPaint(hwnd, &ps);
                                                              return nova_num(0);
                                                          },
                                                          "__paint_thunk__");
                                                      return nova_bool(true);
                                                  },
                                                  "OnPaint"));
                }

                return self;
            };

            // ────────────────────────────────────────────────────────────────────────
            //  GUI.CreateWindow(title, width, height, opts?) -> gui_window
            // ────────────────────────────────────────────────────────────────────────
            guiObj->obj->set("CreateWindow", NovaValue::makeNative(
                                                 [exe, utf8ToWide, ensureWndClassRegistered, wrapHwnd](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                 {
                                                     ensureWndClassRegistered();

                                                     std::string title = a.empty() ? "Nova Window" : a[0].asString();
                                                     int width = a.size() > 1 ? (int)a[1].asNumber() : 800;
                                                     int height = a.size() > 2 ? (int)a[2].asNumber() : 600;

                                                     DWORD style = WS_OVERLAPPEDWINDOW;
                                                     int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
                                                     bool visible = true;

                                                     if (a.size() > 3 && a[3].isObject())
                                                     {
                                                         Val v;
                                                         if ((v = a[3]->obj->get("x")))
                                                             x = (int)v.asNumber();
                                                         if ((v = a[3]->obj->get("y")))
                                                             y = (int)v.asNumber();
                                                         if ((v = a[3]->obj->get("resizable")) && !v.asBool())
                                                             style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
                                                         if ((v = a[3]->obj->get("visible")))
                                                             visible = v.asBool();
                                                     }

                                                     auto *state = new NovaWindowState{exe, cs, {}, {}, {}, nullptr};

                                                     HWND hwnd = CreateWindowExW(
                                                         0, s_wndClassName, utf8ToWide(title).c_str(), style,
                                                         x, y, width, height,
                                                         nullptr, nullptr, GetModuleHandleW(nullptr), state);

                                                     if (!hwnd)
                                                     {
                                                         delete state;
                                                         exe->_error("GUI.CreateWindow: CreateWindowExW failed");
                                                     }
                                                     state->hwnd = hwnd;

                                                     if (visible)
                                                     {
                                                         ShowWindow(hwnd, SW_SHOW);
                                                         UpdateWindow(hwnd);
                                                     }
                                                     return (*wrapHwnd)(hwnd, state);
                                                 },
                                                 "CreateWindow"));

            // ────────────────────────────────────────────────────────────────────────
            //  child control constructors
            //  signature: Create<X>(parentWindow, text, x, y, w, h, opts?) -> gui_control
            // ────────────────────────────────────────────────────────────────────────
            auto makeCreateControl = [exe, utf8ToWide, wrapHwnd](
                                         const wchar_t *className, DWORD baseStyle, bool useAsInitialText)
            {
                return NovaValue::makeNative(
                    [exe, utf8ToWide, wrapHwnd, className, baseStyle, useAsInitialText](ValVec a, auto) -> Val
                    {
                        if (a.size() < 6 || !a[0].isObject())
                            exe->_error("GUI control constructor: expected (parentWindow, text, x, y, w, h, opts?)");

                        Val hwndAddrV = a[0]->obj->get("__hwndAddr__");
                        if (!hwndAddrV)
                            exe->_error("GUI control constructor: first argument is not a gui_window/control");
                        HWND parentHwnd = (HWND)(uintptr_t)hwndAddrV.asNumber();

                        std::string text = a[1].asString();
                        int x = (int)a[2].asNumber();
                        int y = (int)a[3].asNumber();
                        int w = (int)a[4].asNumber();
                        int h = (int)a[5].asNumber();

                        DWORD style = WS_CHILD | WS_VISIBLE | baseStyle;
                        if (a.size() > 6 && a[6].isObject())
                        {
                            Val v;
                            if ((v = a[6]->obj->get("multiline")) && v.asBool())
                                style |= ES_MULTILINE | WS_VSCROLL;
                            if ((v = a[6]->obj->get("readonly")) && v.asBool())
                                style |= ES_READONLY;
                            if ((v = a[6]->obj->get("password")) && v.asBool())
                                style |= ES_PASSWORD;
                            if ((v = a[6]->obj->get("sorted")) && v.asBool())
                                style |= LBS_SORT | CBS_SORT;
                        }

                        int ctrlId = s_nextCtrlId++;
                        HWND ctrl = CreateWindowExW(0, className, utf8ToWide(useAsInitialText ? text : text).c_str(),
                                                    style, x, y, w, h, parentHwnd, (HMENU)(INT_PTR)ctrlId, GetModuleHandleW(nullptr), nullptr);
                        if (!ctrl)
                            exe->_error("GUI: failed to create control");

                        Val self = (*wrapHwnd)(ctrl, nullptr);
                        self->obj->set("controlId", nova_num((double)ctrlId));

                        // OnClick/OnChange register into the PARENT window's
                        // commandHandlers table keyed by this control's id, since
                        // WM_COMMAND notifications always go to the parent.
                        self->obj->set("OnCommand", NovaValue::makeNative(
                                                        [parentHwnd, ctrlId](ValVec ha, auto) -> Val
                                                        {
                                                            if (ha.empty() || !ha[0].isFunction())
                                                                return nova_bool(false);
                                                            auto *pstate = reinterpret_cast<NovaWindowState *>(GetWindowLongPtrW(parentHwnd, GWLP_USERDATA));
                                                            if (!pstate)
                                                                return nova_bool(false);
                                                            pstate->commandHandlers[ctrlId] = ha[0];
                                                            return nova_bool(true);
                                                        },
                                                        "OnCommand"));
                        self->obj->set("OnClick", self->obj->get("OnCommand"));
                        self->obj->set("OnChange", self->obj->get("OnCommand"));

                        return self;
                    },
                    "CreateControl");
            };

            guiObj->obj->set("CreateButton", makeCreateControl(L"BUTTON", BS_PUSHBUTTON, true));
            guiObj->obj->set("CreateCheckbox", makeCreateControl(L"BUTTON", BS_AUTOCHECKBOX, true));
            guiObj->obj->set("CreateRadio", makeCreateControl(L"BUTTON", BS_AUTORADIOBUTTON, true));
            guiObj->obj->set("CreateLabel", makeCreateControl(L"STATIC", 0, true));
            guiObj->obj->set("CreateEdit", makeCreateControl(L"EDIT", WS_BORDER | ES_AUTOHSCROLL, true));
            guiObj->obj->set("CreateListBox", makeCreateControl(L"LISTBOX", WS_BORDER | LBS_NOTIFY, false));
            guiObj->obj->set("CreateComboBox", makeCreateControl(L"COMBOBOX", CBS_DROPDOWN | WS_VSCROLL, false));

            // ────────────────────────────────────────────────────────────────────────
            //  message loop
            // ────────────────────────────────────────────────────────────────────────
            guiObj->obj->set("RunMessageLoop", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                     {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return nova_num((double)msg.wParam); }, "RunMessageLoop"));

            // non-blocking pump — same underlying queue Std.Windows.COM.PumpMessages
            // drains; use one or the other per thread, not both busy-looping.
            guiObj->obj->set("PumpOnce", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
        long long ms = a.empty() ? 0 : (long long)a[0].asNumber();
        MSG msg;
        auto start = std::chrono::steady_clock::now();
        do {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return nova_bool(false);
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (ms <= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start).count() < ms);
        return nova_bool(true); }, "PumpOnce"));

            guiObj->obj->set("PostQuit", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
        PostQuitMessage(a.empty() ? 0 : (int)a[0].asNumber());
        return nova_null(); }, "PostQuit"));

            // ────────────────────────────────────────────────────────────────────────
            //  common dialogs
            // ────────────────────────────────────────────────────────────────────────
            guiObj->obj->set("MessageBox", NovaValue::makeNative(
                                               [utf8ToWide](ValVec a, auto) -> Val
                                               {
                                                   std::string text = a.empty() ? "" : a[0].asString();
                                                   std::string title = a.size() > 1 ? a[1].asString() : "Nova";
                                                   UINT flags = MB_OK;
                                                   if (a.size() > 2 && a[2].isString())
                                                   {
                                                       std::string t = a[2]->sval;
                                                       if (t == "yesno")
                                                           flags = MB_YESNO | MB_ICONQUESTION;
                                                       else if (t == "okcancel")
                                                           flags = MB_OKCANCEL;
                                                       else if (t == "error")
                                                           flags = MB_OK | MB_ICONERROR;
                                                       else if (t == "warning")
                                                           flags = MB_OK | MB_ICONWARNING;
                                                       else if (t == "info")
                                                           flags = MB_OK | MB_ICONINFORMATION;
                                                   }
                                                   int result = MessageBoxW(nullptr, utf8ToWide(text).c_str(), utf8ToWide(title).c_str(), flags);
                                                   if (result == IDYES)
                                                       return nova_str("yes");
                                                   if (result == IDNO)
                                                       return nova_str("no");
                                                   if (result == IDOK)
                                                       return nova_str("ok");
                                                   if (result == IDCANCEL)
                                                       return nova_str("cancel");
                                                   return nova_num(result);
                                               },
                                               "MessageBox"));

            guiObj->obj->set("OpenFileDialog", NovaValue::makeNative(
                                                   [utf8ToWide, wideToUtf8](ValVec a, auto) -> Val
                                                   {
                                                       wchar_t fileBuf[MAX_PATH] = L"";
                                                       std::wstring filter = L"All Files (*.*)\0*.*\0";
                                                       if (!a.empty() && a[0].isString())
                                                       {
                                                           // caller passes "Text Files|*.txt|All Files|*.*" — convert pipes to NULs
                                                           std::string f = a[0]->sval;
                                                           std::wstring wf = utf8ToWide(f);
                                                           for (auto &c : wf)
                                                               if (c == L'|')
                                                                   c = L'\0';
                                                           wf += L'\0';
                                                           filter = wf;
                                                       }
                                                       OPENFILENAMEW ofn{};
                                                       ofn.lStructSize = sizeof(ofn);
                                                       ofn.lpstrFilter = filter.c_str();
                                                       ofn.lpstrFile = fileBuf;
                                                       ofn.nMaxFile = MAX_PATH;
                                                       ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                                                       if (GetOpenFileNameW(&ofn))
                                                           return nova_str(wideToUtf8(fileBuf));
                                                       return nova_null();
                                                   },
                                                   "OpenFileDialog"));

            guiObj->obj->set("SaveFileDialog", NovaValue::makeNative(
                                                   [utf8ToWide, wideToUtf8](ValVec a, auto) -> Val
                                                   {
                                                       wchar_t fileBuf[MAX_PATH] = L"";
                                                       std::wstring filter = L"All Files (*.*)\0*.*\0";
                                                       if (!a.empty() && a[0].isString())
                                                       {
                                                           std::string f = a[0]->sval;
                                                           std::wstring wf = utf8ToWide(f);
                                                           for (auto &c : wf)
                                                               if (c == L'|')
                                                                   c = L'\0';
                                                           wf += L'\0';
                                                           filter = wf;
                                                       }
                                                       OPENFILENAMEW ofn{};
                                                       ofn.lStructSize = sizeof(ofn);
                                                       ofn.lpstrFilter = filter.c_str();
                                                       ofn.lpstrFile = fileBuf;
                                                       ofn.nMaxFile = MAX_PATH;
                                                       ofn.Flags = OFN_OVERWRITEPROMPT;
                                                       if (GetSaveFileNameW(&ofn))
                                                           return nova_str(wideToUtf8(fileBuf));
                                                       return nova_null();
                                                   },
                                                   "SaveFileDialog"));

            guiObj->obj->set("ColorDialog", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
        static COLORREF customColors[16] = {0};
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(cc);
        cc.rgbResult = (a.size() >= 3)
            ? RGB((int)a[0].asNumber(), (int)a[1].asNumber(), (int)a[2].asNumber())
            : RGB(255, 255, 255);
        cc.lpCustColors = customColors;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            auto out = nova_obj();
            out->obj->set("r", nova_num(GetRValue(cc.rgbResult)));
            out->obj->set("g", nova_num(GetGValue(cc.rgbResult)));
            out->obj->set("b", nova_num(GetBValue(cc.rgbResult)));
            return out;
        }
        return nova_null(); }, "ColorDialog"));

            // ────────────────────────────────────────────────────────────────────────
            //  screen metrics
            // ────────────────────────────────────────────────────────────────────────
            guiObj->obj->set("GetScreenSize", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                    {
        auto out = nova_obj();
        out->obj->set("width", nova_num(GetSystemMetrics(SM_CXSCREEN)));
        out->obj->set("height", nova_num(GetSystemMetrics(SM_CYSCREEN)));
        return out; }, "GetScreenSize"));

            winObj->obj->set("GUI", guiObj);
            std_obj->obj->set("Windows", winObj);
        }
#else
        if (!std_obj->obj->get("Windows"))
            std_obj->obj->set("Windows", nova_null());
#endif

        {
            auto procObj = nova_obj();

#if defined(_WIN32)

            procObj->obj->set("Processes", NovaValue::makeNative([this](ValVec, auto) -> Val
                                                                 {
        ValVec result;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return nova_arr();
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                auto obj = nova_obj();
                int wlen = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
                std::string name(wlen > 0 ? wlen - 1 : 0, '\0');
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name.data(), wlen, nullptr, nullptr);
                obj->obj->set("name", nova_str(name));
                obj->obj->set("pid",  nova_num((double)pe.th32ProcessID));
                obj->obj->set("ppid", nova_num((double)pe.th32ParentProcessID));

                DWORD pid = pe.th32ProcessID;
                obj->obj->set("AtAddress", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
                    if (a.empty()) return nova_null();
                    uintptr_t addr = (uintptr_t)a[0].asNumber();
                    std::string typeStr = a.size() > 1 ? a[1].asString() : "u8";
                    NativeType t = parseNativeType(typeStr);
                    size_t sz = nativeTypeSize(t);

                    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
                    if (!h) return nova_null();

                    auto backing = std::make_shared<std::vector<uint8_t>>(sz, 0);
                    SIZE_T bytesRead = 0;
                    bool ok = ReadProcessMemory(h, (LPCVOID)addr, backing->data(), sz, &bytesRead);
                    CloseHandle(h);
                    if (!ok) return nova_null();

                    auto ptr = std::make_shared<NovaPointer>();
                    ptr->isRaw   = true;
                    ptr->rawType = t;
                    ptr->rawBuffer = backing;
                    ptr->rawAddr = backing->data();
                    ptr->rawSize = sz;
                    std::ostringstream ss; ss << "0x" << std::hex << addr;
                    ptr->address = ss.str();

                    // read/write go through RPM/WPM
                    ptr->readFn = [pid, addr, t, sz]() -> Val {
                        std::vector<uint8_t> buf(sz);
                        HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, pid);
                        if (!h) return nova_null();
                        SIZE_T n = 0;
                        ReadProcessMemory(h, (LPCVOID)addr, buf.data(), sz, &n);
                        CloseHandle(h);
                        return nativeDecode(buf.data(), sz, t);
                    };
                    ptr->writeFn = [pid, addr, t, sz](Val v) {
                        auto bytes = nativeEncode(v, t);
                        HANDLE h = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
                        if (!h) return;
                        SIZE_T n = 0;
                        WriteProcessMemory(h, (LPVOID)addr, bytes.data(), std::min(bytes.size(), sz), &n);
                        CloseHandle(h);
                    };

                    auto out = std::make_shared<NovaValue>();
                    out->kind = VK::Pointer;
                    out->ptr  = ptr;
                    return TVal(out);
                }, "AtAddress"));

                obj->obj->set("ReadBytes", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
                    if (a.size() < 2) return nova_arr();
                    uintptr_t addr = (uintptr_t)a[0].asNumber();
                    size_t count   = (size_t)a[1].asNumber();
                    HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, pid);
                    if (!h) return nova_arr();
                    std::vector<uint8_t> buf(count, 0);
                    SIZE_T n = 0;
                    ReadProcessMemory(h, (LPCVOID)addr, buf.data(), count, &n);
                    CloseHandle(h);
                    ValVec out; out.reserve(n);
                    for (size_t i = 0; i < n; i++) out.push_back(nova_num((double)buf[i]));
                    return nova_arr(std::move(out));
                }, "ReadBytes"));

                obj->obj->set("WriteBytes", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
                    if (a.size() < 2 || !a[1].isArray()) return nova_bool(false);
                    uintptr_t addr = (uintptr_t)a[0].asNumber();
                    std::vector<uint8_t> bytes;
                    for (auto& v : a[1]->arr->inner) bytes.push_back((uint8_t)(int)v.asNumber());
                    HANDLE h = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
                    if (!h) return nova_bool(false);
                    SIZE_T n = 0;
                    bool ok = WriteProcessMemory(h, (LPVOID)addr, bytes.data(), bytes.size(), &n);
                    CloseHandle(h);
                    return nova_bool(ok);
                }, "WriteBytes"));

                obj->obj->set("ExecNativeF64", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
                    if (a.empty() || !a[0].isArray()) return nova_null();
                    auto& inner = a[0]->arr->inner;
                    std::vector<uint8_t> code(inner.size());
                    for (size_t i = 0; i < inner.size(); i++) code[i] = (uint8_t)(int)inner[i].asNumber();

                    HANDLE h = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD, FALSE, pid);
                    if (!h) return nova_null();

                    LPVOID remote = VirtualAllocEx(h, nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                    if (!remote) { CloseHandle(h); return nova_null(); }

                    SIZE_T written = 0;
                    WriteProcessMemory(h, remote, code.data(), code.size(), &written);

                    HANDLE thread = CreateRemoteThread(h, nullptr, 0, (LPTHREAD_START_ROUTINE)remote, nullptr, 0, nullptr);
                    double result = 0.0;
                    if (thread) {
                        WaitForSingleObject(thread, 5000);
                        DWORD exitCode = 0;
                        GetExitCodeThread(thread, &exitCode);
                        result = (double)exitCode;
                        CloseHandle(thread);
                    }
                    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
                    CloseHandle(h);
                    return nova_num(result);
                }, "ExecNativeF64"));

                obj->obj->set("Attach", NovaValue::makeNative([this, pid](ValVec a, std::shared_ptr<Scope> cs) -> Val {
                    if (a.size() < 2 || !a[0].isFunction() || !a[1].isObject())
                        _error("proc.Attach: expected (fn, {varname: ptr, ...})");
                    Val fn      = a[0];
                    Val varmap  = a[1];

                    // build a child scope where each varname is backed by RPM/WPM
                    auto attached = cs->child(Scope::Kind::Block);
                    for (auto& [name, ptrVal] : varmap->obj->inner) {
                        if (!ptrVal.isPointer() || !ptrVal->ptr) continue;
                        uintptr_t addr = (uintptr_t)(ptrVal->ptr->isRaw
                            ? reinterpret_cast<uintptr_t>(ptrVal->ptr->rawAddr)
                            : 0);
                        NativeType t = ptrVal->ptr->isRaw ? ptrVal->ptr->rawType : NativeType::U8;
                        size_t sz = nativeTypeSize(t);

                        VarDesc desc;
                        desc.raw = nova_null();
                        desc.hasHooks = true;
                        desc.read = [pid, addr, t, sz]() -> Val {
                            std::vector<uint8_t> buf(sz, 0);
                            HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, pid);
                            if (!h) return nova_null();
                            SIZE_T n = 0;
                            ReadProcessMemory(h, (LPCVOID)addr, buf.data(), sz, &n);
                            CloseHandle(h);
                            return nativeDecode(buf.data(), sz, t);
                        };
                        desc.write = [pid, addr, t, sz](Val v) {
                            auto bytes = nativeEncode(v, t);
                            HANDLE h = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
                            if (!h) return;
                            SIZE_T n = 0;
                            WriteProcessMemory(h, (LPVOID)addr, bytes.data(), std::min(bytes.size(), sz), &n);
                            CloseHandle(h);
                        };
                        attached->setDescriptor(name, std::move(desc));
                    }
                    return callFunction(fn, {}, attached);
                }, "Attach"));
                obj->obj->set("Suspend", NovaValue::makeNative([pid](ValVec, auto) -> Val {
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) return nova_bool(false);
    auto NtSuspendProcess = (LONG(WINAPI*)(HANDLE))GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSuspendProcess");
    bool ok = NtSuspendProcess && NtSuspendProcess(h) == 0;
    CloseHandle(h);
    return nova_bool(ok);
}, "Suspend"));

obj->obj->set("Resume", NovaValue::makeNative([pid](ValVec, auto) -> Val {
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) return nova_bool(false);
    auto NtResumeProcess = (LONG(WINAPI*)(HANDLE))GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtResumeProcess");
    bool ok = NtResumeProcess && NtResumeProcess(h) == 0;
    CloseHandle(h);
    return nova_bool(ok);
}, "Resume"));

obj->obj->set("Kill", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
    UINT code = a.empty() ? 0 : (UINT)a[0].asNumber();
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return nova_bool(false);
    bool ok = TerminateProcess(h, code);
    CloseHandle(h);
    return nova_bool(ok);
}, "Kill"));

obj->obj->set("SetPriority", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
    if (a.empty()) return nova_bool(false);
    std::string p = a[0].asString();
    DWORD cls = NORMAL_PRIORITY_CLASS;
    if      (p == "idle")     cls = IDLE_PRIORITY_CLASS;
    else if (p == "below")    cls = BELOW_NORMAL_PRIORITY_CLASS;
    else if (p == "above")    cls = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (p == "high")     cls = HIGH_PRIORITY_CLASS;
    else if (p == "realtime") cls = REALTIME_PRIORITY_CLASS;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return nova_bool(false);
    bool ok = SetPriorityClass(h, cls);
    CloseHandle(h);
    return nova_bool(ok);
}, "SetPriority"));

// MemoryEntries() — enumerate committed, accessible regions; each entry
// carries a raw_ptr whose readFn does a live ReadProcessMemory on demand.
obj->obj->set("MemoryEntries", NovaValue::makeNative([pid](ValVec, auto) -> Val {
    ValVec out;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return nova_arr();
    MEMORY_BASIC_INFORMATION mbi{};
    uint8_t* addr = nullptr;
    while (VirtualQueryEx(h, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS)) {
            auto entry = nova_obj();
            entry->obj->set("base",    nova_num((double)(uintptr_t)mbi.BaseAddress));
            entry->obj->set("size",    nova_num((double)mbi.RegionSize));
            entry->obj->set("protect", nova_num((double)mbi.Protect));
            entry->obj->set("state",   nova_num((double)mbi.State));

            auto ptr = std::make_shared<NovaPointer>();
            ptr->isRaw = true;
            ptr->rawType = NativeType::U8;
            ptr->rawSize = mbi.RegionSize;
            ptr->rawAddr = mbi.BaseAddress;
            std::ostringstream ss; ss << "0x" << std::hex << (uintptr_t)mbi.BaseAddress;
            ptr->address = ss.str();

            uintptr_t base = (uintptr_t)mbi.BaseAddress;
            SIZE_T regionSize = mbi.RegionSize;
            ptr->readFn = [pid, base, regionSize]() -> Val {
                std::vector<uint8_t> buf(regionSize);
                HANDLE hh = OpenProcess(PROCESS_VM_READ, FALSE, pid);
                if (!hh) return nova_null();
                SIZE_T n = 0;
                bool ok = ReadProcessMemory(hh, (LPCVOID)base, buf.data(), regionSize, &n);
                CloseHandle(hh);
                return ok ? nova_str(std::string((char*)buf.data(), n)) : nova_null();
            };
            ptr->writeFn = [pid, base, regionSize](Val v) {
                if (!v || !v.isString()) return;
                const std::string& data = v->sval;
                size_t toWrite = std::min(data.size(), (size_t)regionSize);
                if (toWrite == 0) return;
                HANDLE hh = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
                if (!hh) return;
                SIZE_T n = 0;
                WriteProcessMemory(hh, (LPVOID)base, data.data(), toWrite, &n);
                CloseHandle(hh);
            };
            auto wrapped = std::make_shared<NovaValue>();
            wrapped->kind = VK::Pointer;
            wrapped->ptr = ptr;
            entry->obj->set("raw", TVal(wrapped));
            out.push_back(entry);
        }
        addr += mbi.RegionSize;
    }
    CloseHandle(h);
    return nova_arr(std::move(out));
}, "MemoryEntries"));

                result.push_back(obj);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return nova_arr(std::move(result)); }, "Processes"));

#elif defined(__linux__)
            procObj->obj->set("Processes", NovaValue::makeNative([this](ValVec, auto) -> Val
                                                                 {
        ValVec result;
        for (auto& entry : fs::directory_iterator("/proc")) {
            std::string fname = entry.path().filename().string();
            if (!std::all_of(fname.begin(), fname.end(), ::isdigit)) continue;
            pid_t pid = std::stoi(fname);

            std::string name;
            { std::ifstream f("/proc/" + fname + "/comm"); std::getline(f, name); }

            pid_t ppid = 0;
            {
                std::ifstream f("/proc/" + fname + "/status");
                std::string line;
                while (std::getline(f, line))
                    if (line.rfind("PPid:", 0) == 0) { ppid = std::stoi(line.substr(5)); break; }
            }

            auto obj = nova_obj();
            obj->obj->set("name", nova_str(name));
            obj->obj->set("pid",  nova_num((double)pid));
            obj->obj->set("ppid", nova_num((double)ppid));

            obj->obj->set("AtAddress", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
                if (a.empty()) return nova_null();
                uintptr_t addr = (uintptr_t)a[0].asNumber();
                std::string typeStr = a.size() > 1 ? a[1].asString() : "u8";
                NativeType t = parseNativeType(typeStr);
                size_t sz = nativeTypeSize(t);

                auto ptr = std::make_shared<NovaPointer>();
                ptr->isRaw   = true;
                ptr->rawType = t;
                ptr->rawSize = sz;
                ptr->rawBuffer = std::make_shared<std::vector<uint8_t>>(sz, 0);
                ptr->rawAddr = ptr->rawBuffer->data();
                std::ostringstream ss; ss << "0x" << std::hex << addr;
                ptr->address = ss.str();

                ptr->readFn = [pid, addr, t, sz]() -> Val {
                    std::vector<uint8_t> buf(sz, 0);
                    std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
                    int fd = ::open(memPath.c_str(), O_RDONLY);
                    if (fd < 0) return nova_null();
                    ssize_t n = ::pread(fd, buf.data(), sz, (off_t)addr);
                    ::close(fd);
                    if (n <= 0) return nova_null();
                    return nativeDecode(buf.data(), sz, t);
                };
                ptr->writeFn = [pid, addr, t, sz](Val v) {
                    auto bytes = nativeEncode(v, t);
                    std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
                    int fd = ::open(memPath.c_str(), O_WRONLY);
                    if (fd < 0) return;
                    ::pwrite(fd, bytes.data(), std::min(bytes.size(), sz), (off_t)addr);
                    ::close(fd);
                };

                auto out = std::make_shared<NovaValue>();
                out->kind = VK::Pointer;
                out->ptr  = ptr;
                return TVal(out);
            }, "AtAddress"));

            obj->obj->set("ReadBytes", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
                if (a.size() < 2) return nova_arr();
                uintptr_t addr = (uintptr_t)a[0].asNumber();
                size_t count   = (size_t)a[1].asNumber();
                std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
                int fd = ::open(memPath.c_str(), O_RDONLY);
                if (fd < 0) return nova_arr();
                std::vector<uint8_t> buf(count, 0);
                ssize_t n = ::pread(fd, buf.data(), count, (off_t)addr);
                ::close(fd);
                if (n <= 0) return nova_arr();
                ValVec out; out.reserve(n);
                for (ssize_t i = 0; i < n; i++) out.push_back(nova_num((double)buf[i]));
                return nova_arr(std::move(out));
            }, "ReadBytes"));

            obj->obj->set("WriteBytes", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
                if (a.size() < 2 || !a[1].isArray()) return nova_bool(false);
                uintptr_t addr = (uintptr_t)a[0].asNumber();
                std::vector<uint8_t> bytes;
                for (auto& v : a[1]->arr->inner) bytes.push_back((uint8_t)(int)v.asNumber());
                std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
                int fd = ::open(memPath.c_str(), O_WRONLY);
                if (fd < 0) return nova_bool(false);
                ssize_t n = ::pwrite(fd, bytes.data(), bytes.size(), (off_t)addr);
                ::close(fd);
                return nova_bool(n == (ssize_t)bytes.size());
            }, "WriteBytes"));

            obj->obj->set("Suspend", NovaValue::makeNative([pid](ValVec, auto) -> Val {
    return nova_bool(::kill(pid, SIGSTOP) == 0);
}, "Suspend"));

obj->obj->set("Resume", NovaValue::makeNative([pid](ValVec, auto) -> Val {
    return nova_bool(::kill(pid, SIGCONT) == 0);
}, "Resume"));

obj->obj->set("Kill", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
    int sig = a.empty() ? SIGTERM : (int)a[0].asNumber();
    return nova_bool(::kill(pid, sig) == 0);
}, "Kill"));

obj->obj->set("SetPriority", NovaValue::makeNative([pid](ValVec a, auto) -> Val {
    int niceVal = a.empty() ? 0 : (int)a[0].asNumber();
    return nova_bool(::setpriority(PRIO_PROCESS, pid, niceVal) == 0);
}, "SetPriority"));



// MemoryEntries() — parse /proc/pid/maps; each entry's raw_ptr reads live
// via pread on /proc/pid/mem.
obj->obj->set("MemoryEntries", NovaValue::makeNative([pid](ValVec, auto) -> Val {
    ValVec out;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {0};
        if (std::sscanf(line.c_str(), "%lx-%lx %7s", &start, &end, perms) < 2) continue;
        if (perms[0] != 'r') continue; // skip unreadable regions
        size_t size = end - start;

        auto entry = nova_obj();
        entry->obj->set("base",  nova_num((double)start));
        entry->obj->set("size",  nova_num((double)size));
        entry->obj->set("perms", nova_str(perms));

        auto ptr = std::make_shared<NovaPointer>();
        ptr->isRaw = true;
        ptr->rawType = NativeType::U8;
        ptr->rawSize = size;
        ptr->rawAddr = (void*)start;
        std::ostringstream ss; ss << "0x" << std::hex << start;
        ptr->address = ss.str();
        ptr->readFn = [pid, start, size]() -> Val {
            std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
            int fd = ::open(memPath.c_str(), O_RDONLY);
            if (fd < 0) return nova_null();
            std::vector<uint8_t> buf(size);
            ssize_t n = ::pread(fd, buf.data(), size, (off_t)start);
            ::close(fd);
            return n > 0 ? nova_str(std::string((char*)buf.data(), n)) : nova_null();
        };
        ptr->writeFn = [pid, start, size](Val v) {
            if (!v || !v.isString()) return;
            const std::string& data = v->sval;
            size_t toWrite = std::min(data.size(), size);
            if (toWrite == 0) return;
            std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
            int fd = ::open(memPath.c_str(), O_WRONLY);
            if (fd < 0) return;
            ::pwrite(fd, data.data(), toWrite, (off_t)start);
            ::close(fd);
        };
        auto wrapped = std::make_shared<NovaValue>();
        wrapped->kind = VK::Pointer;
        wrapped->ptr = ptr;
        entry->obj->set("raw", TVal(wrapped));
        out.push_back(entry);
    }
    return nova_arr(std::move(out));
}, "MemoryEntries"));

            obj->obj->set("Attach", NovaValue::makeNative([this, pid](ValVec a, std::shared_ptr<Scope> cs) -> Val {
                if (a.size() < 2 || !a[0].isFunction() || !a[1].isObject())
                    this->_error("proc.Attach: expected (fn, {varname: ptr, ...})");
                Val fn     = a[0];
                Val varmap = a[1];
                auto attached = cs->child(Scope::Kind::Block);
                for (auto& [name, ptrVal] : varmap->obj->inner) {
                    if (!ptrVal.isPointer() || !ptrVal->ptr) continue;
                    uintptr_t addr = (uintptr_t)reinterpret_cast<uintptr_t>(ptrVal->ptr->rawAddr);
                    NativeType t = ptrVal->ptr->isRaw ? ptrVal->ptr->rawType : NativeType::U8;
                    size_t sz = nativeTypeSize(t);
                    VarDesc desc;
                    desc.raw = nova_null();
                    desc.hasHooks = true;
                    desc.read = [pid, addr, t, sz]() -> Val {
                        std::vector<uint8_t> buf(sz, 0);
                        std::string mp = "/proc/" + std::to_string(pid) + "/mem";
                        int fd = ::open(mp.c_str(), O_RDONLY);
                        if (fd < 0) return nova_null();
                        ::pread(fd, buf.data(), sz, (off_t)addr);
                        ::close(fd);
                        return nativeDecode(buf.data(), sz, t);
                    };
                    desc.write = [pid, addr, t, sz](Val v) {
                        auto bytes = nativeEncode(v, t);
                        std::string mp = "/proc/" + std::to_string(pid) + "/mem";
                        int fd = ::open(mp.c_str(), O_WRONLY);
                        if (fd < 0) return;
                        ::pwrite(fd, bytes.data(), std::min(bytes.size(), sz), (off_t)addr);
                        ::close(fd);
                    };
                    attached->setDescriptor(name, std::move(desc));
                }
                return this->callFunction(fn, {}, attached);
            }, "Attach"));

            result.push_back(obj);
        }
        return nova_arr(std::move(result)); }, "Processes"));

#else
            procObj->obj->set("Processes", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                 {
        std::cerr << "[novac] Std.Processes: not supported on this platform\n";
        return nova_arr(); }, "Processes"));
#endif

            std_obj->obj->set("Processes", procObj->obj->get("Processes"));
        }

        // ════════════════════════════════════════════════════════════════════════
        //  Std.Windows — extra kernel32/user32/winmm additions
        // ════════════════════════════════════════════════════════════════════════
#if defined(_WIN32)
        {
            Val winObj = std_obj->obj->get("Windows");
            if (!winObj || winObj.isNull())
            {
                winObj = nova_obj();
                std_obj->obj->set("Windows", winObj);
            }

            // ── Beep ──────────────────────────────────────────────────────────
            // Std.Windows.Beep(freq, ms)  — wraps kernel32 Beep()
            winObj->obj->set("Beep", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                           {
                DWORD freq = a.size() > 0 ? (DWORD)a[0].asNumber() : 750;
                DWORD ms   = a.size() > 1 ? (DWORD)a[1].asNumber() : 300;
                return nova_bool(::Beep(freq, ms) != 0); }, "Beep"));

            // ── MessageBox ────────────────────────────────────────────────────
            winObj->obj->set("MessageBox", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                std::string text  = a.size() > 0 ? a[0].asString() : "";
                std::string title = a.size() > 1 ? a[1].asString() : "Nova";
                UINT type         = a.size() > 2 ? (UINT)a[2].asNumber() : MB_OK;
                return nova_num((double)MessageBoxA(nullptr, text.c_str(), title.c_str(), type)); }, "MessageBox"));

            // ── MessageBox type constants ─────────────────────────────────────
            winObj->obj->set("MB_OK", nova_num(MB_OK));
            winObj->obj->set("MB_OKCANCEL", nova_num(MB_OKCANCEL));
            winObj->obj->set("MB_YESNO", nova_num(MB_YESNO));
            winObj->obj->set("MB_YESNOCANCEL", nova_num(MB_YESNOCANCEL));
            winObj->obj->set("MB_RETRYCANCEL", nova_num(MB_RETRYCANCEL));
            winObj->obj->set("MB_ICONERROR", nova_num(MB_ICONERROR));
            winObj->obj->set("MB_ICONWARNING", nova_num(MB_ICONWARNING));
            winObj->obj->set("MB_ICONINFORMATION", nova_num(MB_ICONINFORMATION));
            winObj->obj->set("MB_ICONQUESTION", nova_num(MB_ICONQUESTION));
            winObj->obj->set("IDOK", nova_num(IDOK));
            winObj->obj->set("IDCANCEL", nova_num(IDCANCEL));
            winObj->obj->set("IDYES", nova_num(IDYES));
            winObj->obj->set("IDNO", nova_num(IDNO));
            winObj->obj->set("IDRETRY", nova_num(IDRETRY));

            // ── Clipboard ─────────────────────────────────────────────────────
            winObj->obj->set("GetClipboardText", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                       {
                if (!OpenClipboard(nullptr)) return nova_null();
                HANDLE h = GetClipboardData(CF_TEXT);
                if (!h) { CloseClipboard(); return nova_null(); }
                char *p = (char *)GlobalLock(h);
                std::string s = p ? p : "";
                GlobalUnlock(h); CloseClipboard();
                return nova_str(s); }, "GetClipboardText"));

            winObj->obj->set("SetClipboardText", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                if (a.empty() || !OpenClipboard(nullptr)) return nova_bool(false);
                std::string s = a[0].asString();
                EmptyClipboard();
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
                if (!h) { CloseClipboard(); return nova_bool(false); }
                char *p = (char *)GlobalLock(h);
                memcpy(p, s.c_str(), s.size() + 1);
                GlobalUnlock(h);
                SetClipboardData(CF_TEXT, h);
                CloseClipboard();
                return nova_bool(true); }, "SetClipboardText"));

            // ── Registry ──────────────────────────────────────────────────────
            auto regObj = nova_obj();

            auto resolveHive = [](const std::string &name) -> HKEY
            {
                if (name == "HKLM" || name == "HKEY_LOCAL_MACHINE")
                    return HKEY_LOCAL_MACHINE;
                if (name == "HKCU" || name == "HKEY_CURRENT_USER")
                    return HKEY_CURRENT_USER;
                if (name == "HKCR" || name == "HKEY_CLASSES_ROOT")
                    return HKEY_CLASSES_ROOT;
                if (name == "HKU" || name == "HKEY_USERS")
                    return HKEY_USERS;
                return HKEY_CURRENT_USER;
            };

            regObj->obj->set("GetString", NovaValue::makeNative([resolveHive](ValVec a, auto) -> Val
                                                                {
                if (a.size() < 3) return nova_null();
                HKEY hive = resolveHive(a[0].asString());
                HKEY key; if (RegOpenKeyExA(hive, a[1].asString().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return nova_null();
                char buf[4096]; DWORD sz = sizeof(buf), type;
                LSTATUS st = RegQueryValueExA(key, a[2].asString().c_str(), nullptr, &type, (LPBYTE)buf, &sz);
                RegCloseKey(key);
                return (st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) ? nova_str(std::string(buf, sz > 0 ? sz - 1 : 0)) : nova_null(); }, "GetString"));

            regObj->obj->set("SetString", NovaValue::makeNative([resolveHive](ValVec a, auto) -> Val
                                                                {
                if (a.size() < 4) return nova_bool(false);
                HKEY hive = resolveHive(a[0].asString()); HKEY key;
                if (RegCreateKeyExA(hive, a[1].asString().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return nova_bool(false);
                std::string val = a[3].asString();
                LSTATUS st = RegSetValueExA(key, a[2].asString().c_str(), 0, REG_SZ, (LPBYTE)val.c_str(), (DWORD)(val.size() + 1));
                RegCloseKey(key);
                return nova_bool(st == ERROR_SUCCESS); }, "SetString"));

            regObj->obj->set("GetDWORD", NovaValue::makeNative([resolveHive](ValVec a, auto) -> Val
                                                               {
                if (a.size() < 3) return nova_null();
                HKEY hive = resolveHive(a[0].asString()); HKEY key;
                if (RegOpenKeyExA(hive, a[1].asString().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return nova_null();
                DWORD val, sz = sizeof(DWORD), type;
                LSTATUS st = RegQueryValueExA(key, a[2].asString().c_str(), nullptr, &type, (LPBYTE)&val, &sz);
                RegCloseKey(key);
                return (st == ERROR_SUCCESS && type == REG_DWORD) ? nova_num((double)val) : nova_null(); }, "GetDWORD"));

            regObj->obj->set("DeleteValue", NovaValue::makeNative([resolveHive](ValVec a, auto) -> Val
                                                                  {
                if (a.size() < 3) return nova_bool(false);
                HKEY hive = resolveHive(a[0].asString()); HKEY key;
                if (RegOpenKeyExA(hive, a[1].asString().c_str(), 0, KEY_WRITE, &key) != ERROR_SUCCESS) return nova_bool(false);
                LSTATUS st = RegDeleteValueA(key, a[2].asString().c_str());
                RegCloseKey(key); return nova_bool(st == ERROR_SUCCESS); }, "DeleteValue"));

            winObj->obj->set("Registry", regObj);

            // ── Shell ─────────────────────────────────────────────────────────
            auto shellObj = nova_obj();

            shellObj->obj->set("Execute", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                {
                if (a.empty()) return nova_bool(false);
                std::string op   = a.size() > 1 ? a[1].asString() : "open";
                std::string file = a[0].asString();
                HINSTANCE r = ShellExecuteA(nullptr, op.c_str(), file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return nova_bool((intptr_t)r > 32); }, "Execute"));

            shellObj->obj->set("GetSpecialFolder", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                         {
                int csidl = a.empty() ? CSIDL_DESKTOPDIRECTORY : (int)a[0].asNumber();
                char path[MAX_PATH] = {};
                SHGetSpecialFolderPathA(nullptr, path, csidl, FALSE);
                return nova_str(path); }, "GetSpecialFolder"));

            // Common CSIDL constants
            shellObj->obj->set("DESKTOP", nova_num(CSIDL_DESKTOPDIRECTORY));
            shellObj->obj->set("DOCUMENTS", nova_num(CSIDL_PERSONAL));
            shellObj->obj->set("APPDATA", nova_num(CSIDL_APPDATA));
            shellObj->obj->set("LOCALAPPDATA", nova_num(CSIDL_LOCAL_APPDATA));
            shellObj->obj->set("TEMP", nova_num(CSIDL_INTERNET_CACHE));
            shellObj->obj->set("STARTUP", nova_num(CSIDL_STARTUP));
            shellObj->obj->set("PROGRAMS", nova_num(CSIDL_PROGRAMS));

            winObj->obj->set("Shell", shellObj);

            // ── System info ───────────────────────────────────────────────────
            winObj->obj->set("GetSystemInfo", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                    {
                SYSTEM_INFO si; GetSystemInfo(&si);
                auto obj = nova_obj();
                obj->obj->set("processorCount",      nova_num((double)si.dwNumberOfProcessors));
                obj->obj->set("pageSize",            nova_num((double)si.dwPageSize));
                obj->obj->set("processorType",       nova_num((double)si.dwProcessorType));
                obj->obj->set("processorArchitecture", nova_num((double)si.wProcessorArchitecture));
                return obj; }, "GetSystemInfo"));

            winObj->obj->set("GetMemoryStatus", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                      {
                MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
                auto obj = nova_obj();
                obj->obj->set("memoryLoad",         nova_num((double)ms.dwMemoryLoad));
                obj->obj->set("totalPhys",          nova_num((double)ms.ullTotalPhys));
                obj->obj->set("availPhys",          nova_num((double)ms.ullAvailPhys));
                obj->obj->set("totalPageFile",      nova_num((double)ms.ullTotalPageFile));
                obj->obj->set("availPageFile",      nova_num((double)ms.ullAvailPageFile));
                obj->obj->set("totalVirtual",       nova_num((double)ms.ullTotalVirtual));
                obj->obj->set("availVirtual",       nova_num((double)ms.ullAvailVirtual));
                return obj; }, "GetMemoryStatus"));

            winObj->obj->set("GetComputerName", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                      {
                char buf[MAX_COMPUTERNAME_LENGTH + 1] = {}; DWORD sz = sizeof(buf);
                GetComputerNameA(buf, &sz);
                return nova_str(buf); }, "GetComputerName"));

            winObj->obj->set("GetUserName", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                  {
                char buf[256] = {}; DWORD sz = sizeof(buf);
                GetUserNameA(buf, &sz);
                return nova_str(buf); }, "GetUserName"));

            winObj->obj->set("GetWindowsVersion", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                        {
                OSVERSIONINFOEXA osi; ZeroMemory(&osi, sizeof(osi)); osi.dwOSVersionInfoSize = sizeof(osi);
#pragma warning(suppress : 4996)
                GetVersionExA((LPOSVERSIONINFOA)&osi);
                auto obj = nova_obj();
                obj->obj->set("major",       nova_num((double)osi.dwMajorVersion));
                obj->obj->set("minor",       nova_num((double)osi.dwMinorVersion));
                obj->obj->set("build",       nova_num((double)osi.dwBuildNumber));
                obj->obj->set("servicePack", nova_str(osi.szCSDVersion));
                return obj; }, "GetWindowsVersion"));

            // ── Timing (multimedia) ────────────────────────────────────────────
            winObj->obj->set("QueryPerformanceCounter", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                              {
                LARGE_INTEGER li; QueryPerformanceCounter(&li);
                return nova_num((double)li.QuadPart); }, "QueryPerformanceCounter"));

            winObj->obj->set("QueryPerformanceFrequency", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                                {
                LARGE_INTEGER li; QueryPerformanceFrequency(&li);
                return nova_num((double)li.QuadPart); }, "QueryPerformanceFrequency"));

            winObj->obj->set("GetTickCount", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   { return nova_num((double)GetTickCount64()); }, "GetTickCount"));

            // ── Power ─────────────────────────────────────────────────────────
            winObj->obj->set("GetBatteryStatus", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                       {
                SYSTEM_POWER_STATUS ps; GetSystemPowerStatus(&ps);
                auto obj = nova_obj();
                obj->obj->set("acStatus",     nova_num((double)ps.ACLineStatus));
                obj->obj->set("batteryFlag",  nova_num((double)ps.BatteryFlag));
                obj->obj->set("lifePercent",  nova_num((double)ps.BatteryLifePercent));
                obj->obj->set("lifeTime",     nova_num((double)ps.BatteryLifeTime));
                obj->obj->set("fullLifeTime", nova_num((double)ps.BatteryFullLifeTime));
                return obj; }, "GetBatteryStatus"));

            // ── Mutex / named synchronization ─────────────────────────────────
            winObj->obj->set("CreateMutex", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
                std::string name = a.empty() ? "" : a[0].asString();
                BOOL initialOwner = a.size() > 1 ? (BOOL)a[1].asBool() : FALSE;
                HANDLE h = CreateMutexA(nullptr, initialOwner, name.empty() ? nullptr : name.c_str());
                if (!h) return nova_null();
                auto obj = nova_obj();
                auto hShared = std::make_shared<HANDLE>(h);
                obj->obj->set("handle",   nova_num((double)(uintptr_t)h));
                obj->obj->set("wait",     NovaValue::makeNative([hShared](ValVec a, auto) -> Val {
                    DWORD ms = a.empty() ? INFINITE : (DWORD)a[0].asNumber();
                    return nova_num((double)WaitForSingleObject(*hShared, ms));
                }, "wait"));
                obj->obj->set("release",  NovaValue::makeNative([hShared](ValVec, auto) -> Val {
                    return nova_bool(ReleaseMutex(*hShared) != 0);
                }, "release"));
                obj->obj->set("close",    NovaValue::makeNative([hShared](ValVec, auto) -> Val {
                    return nova_bool(CloseHandle(*hShared) != 0);
                }, "close"));
                return obj; }, "CreateMutex"));

            // ── Pipe (anonymous) ──────────────────────────────────────────────
            winObj->obj->set("CreatePipe", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                 {
                HANDLE r, w;
                if (!CreatePipe(&r, &w, nullptr, 0)) return nova_null();
                auto obj = nova_obj();
                auto rh = std::make_shared<HANDLE>(r);
                auto wh = std::make_shared<HANDLE>(w);
                obj->obj->set("read", NovaValue::makeNative([rh](ValVec a, auto) -> Val {
                    DWORD sz = a.empty() ? 4096 : (DWORD)a[0].asNumber();
                    std::vector<char> buf(sz); DWORD got = 0;
                    if (!ReadFile(*rh, buf.data(), sz, &got, nullptr)) return nova_null();
                    return nova_str(std::string(buf.data(), got));
                }, "read"));
                obj->obj->set("write", NovaValue::makeNative([wh](ValVec a, auto) -> Val {
                    if (a.empty()) return nova_num(0);
                    std::string s = a[0].asString(); DWORD written = 0;
                    WriteFile(*wh, s.c_str(), (DWORD)s.size(), &written, nullptr);
                    return nova_num((double)written);
                }, "write"));
                obj->obj->set("closeRead",  NovaValue::makeNative([rh](ValVec, auto) -> Val { CloseHandle(*rh); return nova_null(); }, "closeRead"));
                obj->obj->set("closeWrite", NovaValue::makeNative([wh](ValVec, auto) -> Val { CloseHandle(*wh); return nova_null(); }, "closeWrite"));
                return obj; }, "CreatePipe"));

            // ── Virtual memory ────────────────────────────────────────────────
            winObj->obj->set("VirtualAlloc", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
                SIZE_T sz    = a.empty() ? 4096 : (SIZE_T)a[0].asNumber();
                DWORD  type  = a.size() > 1 ? (DWORD)a[1].asNumber() : MEM_COMMIT | MEM_RESERVE;
                DWORD  prot  = a.size() > 2 ? (DWORD)a[2].asNumber() : PAGE_READWRITE;
                void *p = VirtualAlloc(nullptr, sz, type, prot);
                if (!p) return nova_null();
                auto ptr = std::make_shared<NovaPointer>();
                ptr->isRaw = true; ptr->rawType = parseNativeType("u8");
                ptr->rawAddr = p; ptr->rawSize = sz;
                std::ostringstream ss; ss << "0x" << std::hex << (uintptr_t)p; ptr->address = ss.str();
                auto out = std::make_shared<NovaValue>(); out->kind = VK::Pointer; out->ptr = ptr;
                return out; }, "VirtualAlloc"));

            winObj->obj->set("VirtualFree", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
                if (a.empty() || !a[0].isPointer() || !a[0]->ptr || !a[0]->ptr->isRaw) return nova_bool(false);
                return nova_bool(VirtualFree(a[0]->ptr->rawAddr, 0, MEM_RELEASE) != 0); }, "VirtualFree"));

            winObj->obj->set("VirtualProtect", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                     {
                if (a.size() < 3 || !a[0].isPointer() || !a[0]->ptr || !a[0]->ptr->isRaw) return nova_bool(false);
                DWORD oldProt;
                return nova_bool(VirtualProtect(a[0]->ptr->rawAddr, (SIZE_T)a[1].asNumber(), (DWORD)a[2].asNumber(), &oldProt) != 0); }, "VirtualProtect"));

            // PAGE_* constants
            winObj->obj->set("PAGE_READONLY", nova_num(PAGE_READONLY));
            winObj->obj->set("PAGE_READWRITE", nova_num(PAGE_READWRITE));
            winObj->obj->set("PAGE_EXECUTE_READ", nova_num(PAGE_EXECUTE_READ));
            winObj->obj->set("PAGE_EXECUTE_READWRITE", nova_num(PAGE_EXECUTE_READWRITE));
            winObj->obj->set("MEM_COMMIT", nova_num(MEM_COMMIT));
            winObj->obj->set("MEM_RESERVE", nova_num(MEM_RESERVE));

            // ── Sound ─────────────────────────────────────────────────────────
            {
                auto soundObj = nova_obj();
                soundObj->obj->set("Beep", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                    DWORD freq = a.size() > 0 ? (DWORD)a[0].asNumber() : 750;
                    DWORD ms   = a.size() > 1 ? (DWORD)a[1].asNumber() : 300;
                    return nova_bool(::Beep(freq, ms) != 0); }, "Beep"));
                soundObj->obj->set("PlaySound", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                      {
                    if (a.empty()) { PlaySoundA(nullptr, nullptr, 0); return nova_bool(true); }
                    DWORD flags = a.size() > 1 ? (DWORD)a[1].asNumber() : (SND_FILENAME | SND_ASYNC);
                    return nova_bool(PlaySoundA(a[0].asString().c_str(), nullptr, flags) != 0); }, "PlaySound"));
                auto mciObj = nova_obj();
                mciObj->obj->set("SendString", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                     {
                    if (a.empty()) return nova_str("");
                    char ret[512] = {};
                    MCIERROR err = mciSendStringA(a[0].asString().c_str(), ret, sizeof(ret) - 1, nullptr);
                    return err == 0 ? nova_str(ret) : nova_null(); }, "SendString"));
                soundObj->obj->set("MCI", mciObj);
                soundObj->obj->set("SND_SYNC", nova_num(SND_SYNC));
                soundObj->obj->set("SND_ASYNC", nova_num(SND_ASYNC));
                soundObj->obj->set("SND_FILENAME", nova_num(SND_FILENAME));
                soundObj->obj->set("SND_LOOP", nova_num(SND_LOOP));
                soundObj->obj->set("SND_MEMORY", nova_num(SND_MEMORY));
                soundObj->obj->set("SND_NOSTOP", nova_num(SND_NOSTOP));
                soundObj->obj->set("SND_NOWAIT", nova_num(SND_NOWAIT));
                soundObj->obj->set("SND_PURGE", nova_num(SND_PURGE));
                winObj->obj->set("Sound", soundObj);
            }

            // ── Mouse ─────────────────────────────────────────────────────────
            {
                auto mouseObj = nova_obj();
                mouseObj->obj->set("GetPosition", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                        {
                    POINT p; GetCursorPos(&p);
                    auto obj = nova_obj();
                    obj->obj->set("x", nova_num((double)p.x));
                    obj->obj->set("y", nova_num((double)p.y));
                    return obj; }, "GetPosition"));
                mouseObj->obj->set("SetPosition", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                        {
                    int x = a.size() > 0 ? (int)a[0].asNumber() : 0;
                    int y = a.size() > 1 ? (int)a[1].asNumber() : 0;
                    return nova_bool(SetCursorPos(x, y) != 0); }, "SetPosition"));
                mouseObj->obj->set("ShowCursor", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    BOOL show = a.empty() ? TRUE : (a[0].asBool() ? TRUE : FALSE);
                    return nova_num((double)::ShowCursor(show)); }, "ShowCursor"));
                mouseObj->obj->set("ClipCursor", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    if (a.empty() || a[0].isNull()) return nova_bool(::ClipCursor(nullptr) != 0);
                    if (a[0].isObject()) {
                        RECT r;
                        Val left   = a[0]->obj->get("left");
                        Val top    = a[0]->obj->get("top");
                        Val right  = a[0]->obj->get("right");
                        Val bottom = a[0]->obj->get("bottom");
                        r.left   = left   ? (LONG)left->nval   : 0;
                        r.top    = top    ? (LONG)top->nval    : 0;
                        r.right  = right  ? (LONG)right->nval  : 1920;
                        r.bottom = bottom ? (LONG)bottom->nval : 1080;
                        return nova_bool(::ClipCursor(&r) != 0);
                    }
                    return nova_bool(false); }, "ClipCursor"));
                mouseObj->obj->set("BlockInput", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    BOOL block = a.empty() ? TRUE : (a[0].asBool() ? TRUE : FALSE);
                    return nova_bool(BlockInput(block) != 0); }, "BlockInput"));
                winObj->obj->set("Mouse", mouseObj);
            }

            // ── Keyboard ──────────────────────────────────────────────────────
            {
                auto kbObj = nova_obj();
                kbObj->obj->set("GetAsyncKeyState", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                          {
                    int vk = a.empty() ? 0 : (int)a[0].asNumber();
                    SHORT s = GetAsyncKeyState(vk);
                    auto obj = nova_obj();
                    obj->obj->set("pressed",  nova_bool((s & 0x8000) != 0));
                    obj->obj->set("toggled",  nova_bool((s & 0x0001) != 0));
                    obj->obj->set("raw",      nova_num((double)(uint16_t)s));
                    return obj; }, "GetAsyncKeyState"));
                kbObj->obj->set("GetKeyboardState", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                          {
                    BYTE keys[256];
                    if (!GetKeyboardState(keys)) return nova_null();
                    ValVec arr;
                    for (int i = 0; i < 256; i++) arr.push_back(nova_num((double)keys[i]));
                    return nova_arr(arr); }, "GetKeyboardState"));
                kbObj->obj->set("RegisterHotKey", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                        {
                    int  id  = a.size() > 0 ? (int)a[0].asNumber()  : 1;
                    UINT mod = a.size() > 1 ? (UINT)a[1].asNumber() : 0;
                    UINT vk  = a.size() > 2 ? (UINT)a[2].asNumber() : 0;
                    return nova_bool(RegisterHotKey(nullptr, id, mod, vk) != 0); }, "RegisterHotKey"));
                kbObj->obj->set("UnregisterHotKey", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                          {
                    int id = a.empty() ? 1 : (int)a[0].asNumber();
                    return nova_bool(UnregisterHotKey(nullptr, id) != 0); }, "UnregisterHotKey"));
                kbObj->obj->set("MOD_ALT", nova_num(MOD_ALT));
                kbObj->obj->set("MOD_CONTROL", nova_num(MOD_CONTROL));
                kbObj->obj->set("MOD_SHIFT", nova_num(MOD_SHIFT));
                kbObj->obj->set("MOD_WIN", nova_num(MOD_WIN));
                kbObj->obj->set("VK_BACK", nova_num(VK_BACK));
                kbObj->obj->set("VK_TAB", nova_num(VK_TAB));
                kbObj->obj->set("VK_RETURN", nova_num(VK_RETURN));
                kbObj->obj->set("VK_SHIFT", nova_num(VK_SHIFT));
                kbObj->obj->set("VK_CONTROL", nova_num(VK_CONTROL));
                kbObj->obj->set("VK_MENU", nova_num(VK_MENU));
                kbObj->obj->set("VK_ESCAPE", nova_num(VK_ESCAPE));
                kbObj->obj->set("VK_SPACE", nova_num(VK_SPACE));
                kbObj->obj->set("VK_LEFT", nova_num(VK_LEFT));
                kbObj->obj->set("VK_UP", nova_num(VK_UP));
                kbObj->obj->set("VK_RIGHT", nova_num(VK_RIGHT));
                kbObj->obj->set("VK_DOWN", nova_num(VK_DOWN));
                kbObj->obj->set("VK_DELETE", nova_num(VK_DELETE));
                kbObj->obj->set("VK_INSERT", nova_num(VK_INSERT));
                kbObj->obj->set("VK_HOME", nova_num(VK_HOME));
                kbObj->obj->set("VK_END", nova_num(VK_END));
                kbObj->obj->set("VK_F1", nova_num(VK_F1));
                kbObj->obj->set("VK_F2", nova_num(VK_F2));
                kbObj->obj->set("VK_F4", nova_num(VK_F4));
                kbObj->obj->set("VK_F5", nova_num(VK_F5));
                kbObj->obj->set("VK_F12", nova_num(VK_F12));
                kbObj->obj->set("VK_LBUTTON", nova_num(VK_LBUTTON));
                kbObj->obj->set("VK_RBUTTON", nova_num(VK_RBUTTON));
                kbObj->obj->set("VK_MBUTTON", nova_num(VK_MBUTTON));
                winObj->obj->set("Keyboard", kbObj);
            }

            // ── Clipboard ─────────────────────────────────────────────────────
            {
                auto cbObj = nova_obj();
                cbObj->obj->set("Open", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                              {
                    HWND owner = a.empty() ? nullptr : (HWND)(uintptr_t)(size_t)a[0].asNumber();
                    return nova_bool(OpenClipboard(owner) != 0); }, "Open"));
                cbObj->obj->set("Close", NovaValue::makeNative([](ValVec, auto) -> Val
                                                               { return nova_bool(CloseClipboard() != 0); }, "Close"));
                cbObj->obj->set("GetText", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                 {
                    if (!OpenClipboard(nullptr)) return nova_null();
                    HANDLE h = GetClipboardData(CF_TEXT);
                    if (!h) { CloseClipboard(); return nova_null(); }
                    char *p = (char *)GlobalLock(h);
                    std::string s = p ? p : "";
                    GlobalUnlock(h); CloseClipboard();
                    return nova_str(s); }, "GetText"));
                cbObj->obj->set("SetText", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                    if (a.empty() || !OpenClipboard(nullptr)) return nova_bool(false);
                    std::string s = a[0].asString(); EmptyClipboard();
                    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
                    if (!h) { CloseClipboard(); return nova_bool(false); }
                    memcpy(GlobalLock(h), s.c_str(), s.size() + 1);
                    GlobalUnlock(h); SetClipboardData(CF_TEXT, h); CloseClipboard();
                    return nova_bool(true); }, "SetText"));
                cbObj->obj->set("GetData", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                    UINT fmt = a.empty() ? CF_TEXT : (UINT)a[0].asNumber();
                    if (!OpenClipboard(nullptr)) return nova_null();
                    HANDLE h = GetClipboardData(fmt);
                    if (!h) { CloseClipboard(); return nova_null(); }
                    SIZE_T sz = GlobalSize(h);
                    void *p = GlobalLock(h);
                    std::string out((char *)p, sz);
                    GlobalUnlock(h); CloseClipboard();
                    return nova_str(out); }, "GetData"));
                cbObj->obj->set("SetData", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                    if (a.size() < 2 || !OpenClipboard(nullptr)) return nova_bool(false);
                    UINT fmt = (UINT)a[0].asNumber(); std::string data = a[1].asString();
                    EmptyClipboard();
                    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, data.size());
                    if (!h) { CloseClipboard(); return nova_bool(false); }
                    memcpy(GlobalLock(h), data.data(), data.size());
                    GlobalUnlock(h); SetClipboardData(fmt, h); CloseClipboard();
                    return nova_bool(true); }, "SetData"));
                cbObj->obj->set("Empty", NovaValue::makeNative([](ValVec, auto) -> Val
                                                               {
                    if (!OpenClipboard(nullptr)) return nova_bool(false);
                    bool r = EmptyClipboard() != 0; CloseClipboard(); return nova_bool(r); }, "Empty"));
                cbObj->obj->set("IsAvailable", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                     {
                    UINT fmt = a.empty() ? CF_TEXT : (UINT)a[0].asNumber();
                    return nova_bool(IsClipboardFormatAvailable(fmt) != 0); }, "IsAvailable"));
                cbObj->obj->set("CF_TEXT", nova_num(CF_TEXT));
                cbObj->obj->set("CF_UNICODETEXT", nova_num(CF_UNICODETEXT));
                cbObj->obj->set("CF_BITMAP", nova_num(CF_BITMAP));
                cbObj->obj->set("CF_DIB", nova_num(CF_DIB));
                cbObj->obj->set("CF_HDROP", nova_num(CF_HDROP));
                winObj->obj->set("Clipboard", cbObj);
            }

            // ── Console ───────────────────────────────────────────────────────
            {
                auto conObj = nova_obj();
                conObj->obj->set("Alloc", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                { return nova_bool(AllocConsole() != 0); }, "Alloc"));
                conObj->obj->set("Free", NovaValue::makeNative([](ValVec, auto) -> Val
                                                               { return nova_bool(FreeConsole() != 0); }, "Free"));
                conObj->obj->set("SetTitle", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                   {
                    if (a.empty()) return nova_bool(false);
                    return nova_bool(SetConsoleTitleA(a[0].asString().c_str()) != 0); }, "SetTitle"));
                conObj->obj->set("SetTextColor", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    WORD attr = a.empty() ? (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
                                          : (WORD)a[0].asNumber();
                    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
                    return nova_bool(SetConsoleTextAttribute(h, attr) != 0); }, "SetTextColor"));
                conObj->obj->set("GetSize", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                  {
                    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
                    CONSOLE_SCREEN_BUFFER_INFO info;
                    if (!GetConsoleScreenBufferInfo(h, &info)) return nova_null();
                    auto obj = nova_obj();
                    obj->obj->set("width",   nova_num(info.dwSize.X));
                    obj->obj->set("height",  nova_num(info.dwSize.Y));
                    obj->obj->set("cursorX", nova_num(info.dwCursorPosition.X));
                    obj->obj->set("cursorY", nova_num(info.dwCursorPosition.Y));
                    return obj; }, "GetSize"));
                conObj->obj->set("SetCursorPos", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
                    COORD c;
                    c.X = a.size() > 0 ? (SHORT)a[0].asNumber() : 0;
                    c.Y = a.size() > 1 ? (SHORT)a[1].asNumber() : 0;
                    return nova_bool(SetConsoleCursorPosition(h, c) != 0); }, "SetCursorPos"));
                conObj->obj->set("FG_BLACK", nova_num(0));
                conObj->obj->set("FG_BLUE", nova_num(FOREGROUND_BLUE));
                conObj->obj->set("FG_GREEN", nova_num(FOREGROUND_GREEN));
                conObj->obj->set("FG_CYAN", nova_num(FOREGROUND_BLUE | FOREGROUND_GREEN));
                conObj->obj->set("FG_RED", nova_num(FOREGROUND_RED));
                conObj->obj->set("FG_MAGENTA", nova_num(FOREGROUND_RED | FOREGROUND_BLUE));
                conObj->obj->set("FG_YELLOW", nova_num(FOREGROUND_RED | FOREGROUND_GREEN));
                conObj->obj->set("FG_WHITE", nova_num(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE));
                conObj->obj->set("FG_INTENSE", nova_num(FOREGROUND_INTENSITY));
                conObj->obj->set("BG_BLUE", nova_num(BACKGROUND_BLUE));
                conObj->obj->set("BG_GREEN", nova_num(BACKGROUND_GREEN));
                conObj->obj->set("BG_RED", nova_num(BACKGROUND_RED));
                conObj->obj->set("BG_WHITE", nova_num(BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE));
                conObj->obj->set("BG_INTENSE", nova_num(BACKGROUND_INTENSITY));
                winObj->obj->set("Console", conObj);
            }

            // ── Registry (structured sub-object) ──────────────────────────────
            {
                auto rObj = nova_obj();
                auto resolveHiveEx = [](const std::string &name) -> HKEY
                {
                    if (name == "HKLM" || name == "HKEY_LOCAL_MACHINE")
                        return HKEY_LOCAL_MACHINE;
                    if (name == "HKCU" || name == "HKEY_CURRENT_USER")
                        return HKEY_CURRENT_USER;
                    if (name == "HKCR" || name == "HKEY_CLASSES_ROOT")
                        return HKEY_CLASSES_ROOT;
                    if (name == "HKU" || name == "HKEY_USERS")
                        return HKEY_USERS;
                    return HKEY_CURRENT_USER;
                };
                rObj->obj->set("OpenKey", NovaValue::makeNative([resolveHiveEx](ValVec a, auto) -> Val
                                                                {
                    if (a.size() < 2) return nova_null();
                    HKEY hive = resolveHiveEx(a[0].asString());
                    std::string acc = a.size() > 2 ? a[2].asString() : "read";
                    REGSAM sam = KEY_READ;
                    if (acc == "write") sam = KEY_WRITE;
                    if (acc == "all")   sam = KEY_ALL_ACCESS;
                    HKEY key;
                    if (RegOpenKeyExA(hive, a[1].asString().c_str(), 0, sam, &key) != ERROR_SUCCESS) return nova_null();
                    auto obj = nova_obj();
                    auto hs = std::make_shared<HKEY>(key);
                    obj->obj->set("handle", nova_num((double)(uintptr_t)key));
                    obj->obj->set("QueryValue", NovaValue::makeNative([hs](ValVec a2, auto) -> Val {
                        std::string nm = a2.empty() ? "" : a2[0].asString();
                        char buf[4096]; DWORD sz = sizeof(buf), type;
                        if (RegQueryValueExA(*hs, nm.c_str(), nullptr, &type, (LPBYTE)buf, &sz) != ERROR_SUCCESS) return nova_null();
                        if (type == REG_SZ || type == REG_EXPAND_SZ) return nova_str(std::string(buf, sz > 0 ? sz - 1 : 0));
                        if (type == REG_DWORD) { DWORD d; memcpy(&d, buf, 4); return nova_num((double)d); }
                        return nova_str(std::string(buf, sz));
                    }, "QueryValue"));
                    obj->obj->set("SetValue", NovaValue::makeNative([hs](ValVec a2, auto) -> Val {
                        if (a2.size() < 2) return nova_bool(false);
                        std::string nm = a2[0].asString();
                        if (a2[1].isNumber()) { DWORD d = (DWORD)a2[1].asNumber(); return nova_bool(RegSetValueExA(*hs, nm.c_str(), 0, REG_DWORD, (LPBYTE)&d, sizeof(d)) == ERROR_SUCCESS); }
                        std::string v = a2[1].asString();
                        return nova_bool(RegSetValueExA(*hs, nm.c_str(), 0, REG_SZ, (LPBYTE)v.c_str(), (DWORD)(v.size()+1)) == ERROR_SUCCESS);
                    }, "SetValue"));
                    obj->obj->set("DeleteValue", NovaValue::makeNative([hs](ValVec a2, auto) -> Val {
                        if (a2.empty()) return nova_bool(false);
                        return nova_bool(RegDeleteValueA(*hs, a2[0].asString().c_str()) == ERROR_SUCCESS);
                    }, "DeleteValue"));
                    obj->obj->set("Close", NovaValue::makeNative([hs](ValVec, auto) -> Val {
                        RegCloseKey(*hs); return nova_null();
                    }, "Close"));
                    return obj; }, "OpenKey"));
                rObj->obj->set("QueryValue", NovaValue::makeNative([resolveHiveEx](ValVec a, auto) -> Val
                                                                   {
                    if (a.size() < 3) return nova_null();
                    HKEY hive = resolveHiveEx(a[0].asString()); HKEY key;
                    if (RegOpenKeyExA(hive, a[1].asString().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return nova_null();
                    char buf[4096]; DWORD sz = sizeof(buf), type;
                    LSTATUS st = RegQueryValueExA(key, a[2].asString().c_str(), nullptr, &type, (LPBYTE)buf, &sz);
                    RegCloseKey(key);
                    if (st != ERROR_SUCCESS) return nova_null();
                    if (type == REG_SZ || type == REG_EXPAND_SZ) return nova_str(std::string(buf, sz > 0 ? sz - 1 : 0));
                    if (type == REG_DWORD) { DWORD d; memcpy(&d, buf, 4); return nova_num((double)d); }
                    return nova_str(std::string(buf, sz)); }, "QueryValue"));
                rObj->obj->set("SetValue", NovaValue::makeNative([resolveHiveEx](ValVec a, auto) -> Val
                                                                 {
                    if (a.size() < 4) return nova_bool(false);
                    HKEY hive = resolveHiveEx(a[0].asString()); HKEY key;
                    if (RegCreateKeyExA(hive, a[1].asString().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return nova_bool(false);
                    LSTATUS st;
                    if (a[3].isNumber()) { DWORD d = (DWORD)a[3].asNumber(); st = RegSetValueExA(key, a[2].asString().c_str(), 0, REG_DWORD, (LPBYTE)&d, sizeof(d)); }
                    else { std::string v = a[3].asString(); st = RegSetValueExA(key, a[2].asString().c_str(), 0, REG_SZ, (LPBYTE)v.c_str(), (DWORD)(v.size()+1)); }
                    RegCloseKey(key); return nova_bool(st == ERROR_SUCCESS); }, "SetValue"));
                rObj->obj->set("DeleteKey", NovaValue::makeNative([resolveHiveEx](ValVec a, auto) -> Val
                                                                  {
                    if (a.size() < 3) return nova_bool(false);
                    HKEY hive = resolveHiveEx(a[0].asString()); HKEY key;
                    if (RegOpenKeyExA(hive, a[1].asString().c_str(), 0, KEY_WRITE, &key) != ERROR_SUCCESS) return nova_bool(false);
                    LSTATUS st = RegDeleteKeyA(key, a[2].asString().c_str());
                    RegCloseKey(key); return nova_bool(st == ERROR_SUCCESS); }, "DeleteKey"));
                rObj->obj->set("DeleteValue", NovaValue::makeNative([resolveHiveEx](ValVec a, auto) -> Val
                                                                    {
                    if (a.size() < 3) return nova_bool(false);
                    HKEY hive = resolveHiveEx(a[0].asString()); HKEY key;
                    if (RegOpenKeyExA(hive, a[1].asString().c_str(), 0, KEY_WRITE, &key) != ERROR_SUCCESS) return nova_bool(false);
                    LSTATUS st = RegDeleteValueA(key, a[2].asString().c_str());
                    RegCloseKey(key); return nova_bool(st == ERROR_SUCCESS); }, "DeleteValue"));
                winObj->obj->set("Registry", rObj);
            }

            // ── System (structured sub-object) ────────────────────────────────
            {
                auto sysObj = nova_obj();
                sysObj->obj->set("Sleep", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                {
                    ::Sleep(a.empty() ? 0 : (DWORD)a[0].asNumber()); return nova_null(); }, "Sleep"));
                sysObj->obj->set("GetTickCount", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                       { return nova_num((double)GetTickCount64()); }, "GetTickCount"));
                sysObj->obj->set("QueryPerformanceCounter", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                                  {
                    LARGE_INTEGER li; QueryPerformanceCounter(&li); return nova_num((double)li.QuadPart); }, "QueryPerformanceCounter"));
                sysObj->obj->set("QueryPerformanceFrequency", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                                    {
                    LARGE_INTEGER li; QueryPerformanceFrequency(&li); return nova_num((double)li.QuadPart); }, "QueryPerformanceFrequency"));
                sysObj->obj->set("GetComputerName", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                          {
                    char buf[MAX_COMPUTERNAME_LENGTH + 1] = {}; DWORD sz = sizeof(buf);
                    GetComputerNameA(buf, &sz); return nova_str(buf); }, "GetComputerName"));
                sysObj->obj->set("GetUserName", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                      {
                    char buf[256] = {}; DWORD sz = sizeof(buf);
                    GetUserNameA(buf, &sz); return nova_str(buf); }, "GetUserName"));
                sysObj->obj->set("GetNativeSystemInfo", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                              {
                    SYSTEM_INFO si; GetNativeSystemInfo(&si);
                    auto obj = nova_obj();
                    obj->obj->set("processorCount",        nova_num((double)si.dwNumberOfProcessors));
                    obj->obj->set("pageSize",              nova_num((double)si.dwPageSize));
                    obj->obj->set("processorType",         nova_num((double)si.dwProcessorType));
                    obj->obj->set("processorArchitecture", nova_num((double)si.wProcessorArchitecture));
                    obj->obj->set("allocationGranularity", nova_num((double)si.dwAllocationGranularity));
                    obj->obj->set("processorLevel",        nova_num((double)si.wProcessorLevel));
                    obj->obj->set("processorRevision",     nova_num((double)si.wProcessorRevision));
                    return obj; }, "GetNativeSystemInfo"));
                sysObj->obj->set("GetSystemMetrics", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                           { return nova_num((double)GetSystemMetrics(a.empty() ? SM_CXSCREEN : (int)a[0].asNumber())); }, "GetSystemMetrics"));
                sysObj->obj->set("SM_CXSCREEN", nova_num(SM_CXSCREEN));
                sysObj->obj->set("SM_CYSCREEN", nova_num(SM_CYSCREEN));
                sysObj->obj->set("SM_CXFULLSCREEN", nova_num(SM_CXFULLSCREEN));
                sysObj->obj->set("SM_CYFULLSCREEN", nova_num(SM_CYFULLSCREEN));
                sysObj->obj->set("SM_CMONITORS", nova_num(SM_CMONITORS));
                sysObj->obj->set("SM_CXVIRTUALSCREEN", nova_num(SM_CXVIRTUALSCREEN));
                sysObj->obj->set("SM_CYVIRTUALSCREEN", nova_num(SM_CYVIRTUALSCREEN));
                sysObj->obj->set("GetLastInputInfo", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                           {
                    LASTINPUTINFO lii; lii.cbSize = sizeof(lii);
                    if (!GetLastInputInfo(&lii)) return nova_null();
                    auto obj = nova_obj();
                    obj->obj->set("dwTime", nova_num((double)lii.dwTime));
                    obj->obj->set("idleMs", nova_num((double)(GetTickCount64() - lii.dwTime)));
                    return obj; }, "GetLastInputInfo"));
                sysObj->obj->set("EnumDisplayMonitors", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                              {
                    struct MonCtx { ValVec arr; };
                    auto ctx = std::make_shared<MonCtx>();
                    ::EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT rc, LPARAM lp) -> BOOL {
                        auto *mc = reinterpret_cast<MonCtx *>(lp);
                        MONITORINFO mi; mi.cbSize = sizeof(mi); GetMonitorInfoA(hm, &mi);
                        auto obj = nova_obj();
                        obj->obj->set("x",       nova_num((double)rc->left));
                        obj->obj->set("y",       nova_num((double)rc->top));
                        obj->obj->set("width",   nova_num((double)(rc->right - rc->left)));
                        obj->obj->set("height",  nova_num((double)(rc->bottom - rc->top)));
                        obj->obj->set("primary", nova_bool((mi.dwFlags & MONITORINFOF_PRIMARY) != 0));
                        mc->arr.push_back(obj); return TRUE;
                    }, (LPARAM)ctx.get());
                    return nova_arr(ctx->arr); }, "EnumDisplayMonitors"));
                sysObj->obj->set("ChangeDisplaySettings", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                                {
                    DEVMODEA dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
                    EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm);
                    if (a.size() > 0) dm.dmPelsWidth        = (DWORD)a[0].asNumber();
                    if (a.size() > 1) dm.dmPelsHeight       = (DWORD)a[1].asNumber();
                    if (a.size() > 2) dm.dmBitsPerPel       = (DWORD)a[2].asNumber();
                    if (a.size() > 3) dm.dmDisplayFrequency = (DWORD)a[3].asNumber();
                    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
                    return nova_bool(ChangeDisplaySettingsA(&dm, 0) == DISP_CHANGE_SUCCESSFUL); }, "ChangeDisplaySettings"));
                sysObj->obj->set("SetWallpaper", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                       {
                    if (a.empty()) return nova_bool(false);
                    std::string path = a[0].asString();
                    return nova_bool(SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0,
                        (PVOID)path.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != 0); }, "SetWallpaper"));
                sysObj->obj->set("GetVersion", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                     {
                    OSVERSIONINFOEXA osi; ZeroMemory(&osi, sizeof(osi)); osi.dwOSVersionInfoSize = sizeof(osi);
#pragma warning(suppress : 4996)
                    GetVersionExA((LPOSVERSIONINFOA)&osi);
                    auto obj = nova_obj();
                    obj->obj->set("major",       nova_num((double)osi.dwMajorVersion));
                    obj->obj->set("minor",       nova_num((double)osi.dwMinorVersion));
                    obj->obj->set("build",       nova_num((double)osi.dwBuildNumber));
                    obj->obj->set("servicePack", nova_str(osi.szCSDVersion));
                    return obj; }, "GetVersion"));
                winObj->obj->set("System", sysObj);
            }

            // ── Power ─────────────────────────────────────────────────────────
            {
                auto pwrObj = nova_obj();
                pwrObj->obj->set("LockWorkStation", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                          { return nova_bool(LockWorkStation() != 0); }, "LockWorkStation"));
                pwrObj->obj->set("ExitWindows", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                      {
                    UINT flags = a.empty() ? EWX_LOGOFF : (UINT)a[0].asNumber();
                    HANDLE tok; LUID luid; TOKEN_PRIVILEGES tp;
                    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
                        LookupPrivilegeValueA(nullptr, "SeShutdownPrivilege", &luid);
                        tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid;
                        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
                        CloseHandle(tok);
                    }
                    return nova_bool(ExitWindowsEx(flags, SHTDN_REASON_MINOR_OTHER) != 0); }, "ExitWindows"));
                pwrObj->obj->set("SetThreadExecutionState", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                                  {
                    EXECUTION_STATE flags = a.empty() ? ES_CONTINUOUS : (EXECUTION_STATE)(DWORD)a[0].asNumber();
                    return nova_num((double)SetThreadExecutionState(flags)); }, "SetThreadExecutionState"));
                pwrObj->obj->set("EWX_LOGOFF", nova_num(EWX_LOGOFF));
                pwrObj->obj->set("EWX_SHUTDOWN", nova_num(EWX_SHUTDOWN));
                pwrObj->obj->set("EWX_REBOOT", nova_num(EWX_REBOOT));
                pwrObj->obj->set("EWX_POWEROFF", nova_num(EWX_POWEROFF));
                pwrObj->obj->set("EWX_FORCE", nova_num(EWX_FORCE));
                pwrObj->obj->set("ES_CONTINUOUS", nova_num(ES_CONTINUOUS));
                pwrObj->obj->set("ES_DISPLAY_REQUIRED", nova_num(ES_DISPLAY_REQUIRED));
                pwrObj->obj->set("ES_SYSTEM_REQUIRED", nova_num(ES_SYSTEM_REQUIRED));
                pwrObj->obj->set("ES_AWAYMODE_REQUIRED", nova_num(ES_AWAYMODE_REQUIRED));
                winObj->obj->set("Power", pwrObj);
            }

            // ── Shell (structured sub-object) ─────────────────────────────────
            {
                auto shObj = nova_obj();
                shObj->obj->set("Execute", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                    if (a.empty()) return nova_bool(false);
                    std::string file = a[0].asString();
                    std::string op   = a.size() > 1 ? a[1].asString() : "open";
                    std::string args = a.size() > 2 ? a[2].asString() : "";
                    HINSTANCE r = ShellExecuteA(nullptr, op.c_str(), file.c_str(),
                        args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
                    return nova_bool((intptr_t)r > 32); }, "Execute"));
                shObj->obj->set("GetKnownFolderPath", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                            {
                    auto nameToCSIDL = [](const std::string &n) -> int {
                        if (n == "Desktop")      return CSIDL_DESKTOP;
                        if (n == "Documents")    return CSIDL_PERSONAL;
                        if (n == "AppData")      return CSIDL_APPDATA;
                        if (n == "LocalAppData") return CSIDL_LOCAL_APPDATA;
                        if (n == "Music")        return CSIDL_MYMUSIC;
                        if (n == "Pictures")     return CSIDL_MYPICTURES;
                        if (n == "Videos")       return CSIDL_MYVIDEO;
                        if (n == "Startup")      return CSIDL_STARTUP;
                        if (n == "Programs")     return CSIDL_PROGRAMS;
                        if (n == "SystemRoot")   return CSIDL_WINDOWS;
                        if (n == "System")       return CSIDL_SYSTEM;
                        if (n == "ProgramFiles") return CSIDL_PROGRAM_FILES;
                        return CSIDL_PERSONAL;
                    };
                    int csidl = a.empty() ? CSIDL_PERSONAL : nameToCSIDL(a[0].asString());
                    char path[MAX_PATH] = {};
                    SHGetSpecialFolderPathA(nullptr, path, csidl, FALSE);
                    return nova_str(path); }, "GetKnownFolderPath"));
                shObj->obj->set("OpenURL", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                    if (a.empty()) return nova_bool(false);
                    HINSTANCE r = ShellExecuteA(nullptr, "open", a[0].asString().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return nova_bool((intptr_t)r > 32); }, "OpenURL"));
                winObj->obj->set("Shell", shObj);
            }

            // ── Graphics ──────────────────────────────────────────────────────
            {
                auto gfxObj = nova_obj();
                gfxObj->obj->set("CaptureScreen", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                        {
                    int x = a.size() > 0 ? (int)a[0].asNumber() : 0;
                    int y = a.size() > 1 ? (int)a[1].asNumber() : 0;
                    int w = a.size() > 2 ? (int)a[2].asNumber() : GetSystemMetrics(SM_CXSCREEN);
                    int h = a.size() > 3 ? (int)a[3].asNumber() : GetSystemMetrics(SM_CYSCREEN);
                    HDC screenDC = GetDC(nullptr);
                    HDC memDC    = CreateCompatibleDC(screenDC);
                    HBITMAP bmp  = ::CreateCompatibleBitmap(screenDC, w, h);
                    HGDIOBJ old  = SelectObject(memDC, bmp);
                    ::BitBlt(memDC, 0, 0, w, h, screenDC, x, y, SRCCOPY);
                    BITMAPINFOHEADER bi; ZeroMemory(&bi, sizeof(bi));
                    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h;
                    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
                    std::vector<uint8_t> pixels((size_t)(w * h * 4));
                    ::GetDIBits(memDC, bmp, 0, (UINT)h, pixels.data(), (BITMAPINFO *)&bi, DIB_RGB_COLORS);
                    SelectObject(memDC, old); DeleteObject(bmp);
                    DeleteDC(memDC); ReleaseDC(nullptr, screenDC);
                    auto obj = nova_obj();
                    obj->obj->set("width",  nova_num((double)w));
                    obj->obj->set("height", nova_num((double)h));
                    obj->obj->set("pixels", nova_str(std::string((char *)pixels.data(), pixels.size())));
                    obj->obj->set("bpp",    nova_num(32));
                    return obj; }, "CaptureScreen"));
                gfxObj->obj->set("CreateCompatibleBitmap", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                                 {
                    int w = a.size() > 0 ? (int)a[0].asNumber() : 100;
                    int h = a.size() > 1 ? (int)a[1].asNumber() : 100;
                    HDC dc = GetDC(nullptr);
                    HBITMAP bmp = ::CreateCompatibleBitmap(dc, w, h);
                    ReleaseDC(nullptr, dc);
                    if (!bmp) return nova_null();
                    auto bs = std::make_shared<HBITMAP>(bmp);
                    auto obj = nova_obj();
                    obj->obj->set("handle", nova_num((double)(uintptr_t)bmp));
                    obj->obj->set("width",  nova_num((double)w));
                    obj->obj->set("height", nova_num((double)h));
                    obj->obj->set("delete", NovaValue::makeNative([bs](ValVec, auto) -> Val {
                        if (*bs) { DeleteObject(*bs); *bs = nullptr; } return nova_null();
                    }, "delete"));
                    return obj; }, "CreateCompatibleBitmap"));
                gfxObj->obj->set("BitBlt", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                    if (a.size() < 5) return nova_bool(false);
                    HDC  dst = (HDC)(uintptr_t)(size_t)a[0].asNumber();
                    int  dx  = (int)a[1].asNumber(), dy = (int)a[2].asNumber();
                    int  w   = (int)a[3].asNumber(), h  = (int)a[4].asNumber();
                    HDC  src = a.size() > 5 ? (HDC)(uintptr_t)(size_t)a[5].asNumber() : nullptr;
                    int  sx  = a.size() > 6 ? (int)a[6].asNumber() : 0;
                    int  sy  = a.size() > 7 ? (int)a[7].asNumber() : 0;
                    DWORD rop = a.size() > 8 ? (DWORD)a[8].asNumber() : SRCCOPY;
                    return nova_bool(::BitBlt(dst, dx, dy, w, h, src, sx, sy, rop) != 0); }, "BitBlt"));
                gfxObj->obj->set("GetDIBits", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                    {
                    if (a.empty()) return nova_null();
                    HBITMAP bmp = (HBITMAP)(uintptr_t)(size_t)a[0].asNumber();
                    int w = a.size() > 1 ? (int)a[1].asNumber() : 0;
                    int h = a.size() > 2 ? (int)a[2].asNumber() : 0;
                    if (!w || !h) { BITMAP bm; GetObjectA(bmp, sizeof(bm), &bm); w = bm.bmWidth; h = bm.bmHeight; }
                    HDC dc = GetDC(nullptr);
                    BITMAPINFOHEADER bi; ZeroMemory(&bi, sizeof(bi));
                    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h;
                    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
                    std::vector<uint8_t> pixels((size_t)(w * h * 4));
                    ::GetDIBits(dc, bmp, 0, (UINT)h, pixels.data(), (BITMAPINFO *)&bi, DIB_RGB_COLORS);
                    ReleaseDC(nullptr, dc);
                    auto obj = nova_obj();
                    obj->obj->set("width",  nova_num((double)w));
                    obj->obj->set("height", nova_num((double)h));
                    obj->obj->set("pixels", nova_str(std::string((char *)pixels.data(), pixels.size())));
                    return obj; }, "GetDIBits"));
                gfxObj->obj->set("SRCCOPY", nova_num(SRCCOPY));
                gfxObj->obj->set("SRCAND", nova_num(SRCAND));
                gfxObj->obj->set("SRCINVERT", nova_num(SRCINVERT));
                gfxObj->obj->set("SRCPAINT", nova_num(SRCPAINT));
                gfxObj->obj->set("BLACKNESS", nova_num(BLACKNESS));
                gfxObj->obj->set("WHITENESS", nova_num(WHITENESS));
                winObj->obj->set("Graphics", gfxObj);
            }

            // ── Resources ─────────────────────────────────────────────────────
            {
                auto resObj = nova_obj();
                resObj->obj->set("Find", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
                    HMODULE mod = a.size() > 0 && a[0].isNumber() ? (HMODULE)(uintptr_t)(size_t)a[0].asNumber() : GetModuleHandleA(nullptr);
                    std::string name = a.size() > 1 ? a[1].asString() : "";
                    std::string type = a.size() > 2 ? a[2].asString() : "RT_RCDATA";
                    LPCSTR resType = MAKEINTRESOURCEA(10); // RT_RCDATA
if (type == "RT_STRING")   resType = MAKEINTRESOURCEA(6);
if (type == "RT_BITMAP")   resType = MAKEINTRESOURCEA(2);
if (type == "RT_ICON")     resType = MAKEINTRESOURCEA(3);
if (type == "RT_MANIFEST") resType = MAKEINTRESOURCEA(24);
HRSRC h = FindResourceA(mod, name.c_str(), resType);
                    return h ? nova_num((double)(uintptr_t)h) : nova_null(); }, "Find"));
                resObj->obj->set("Load", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
                    if (a.size() < 2) return nova_null();
                    HMODULE mod  = a[0].isNumber() ? (HMODULE)(uintptr_t)(size_t)a[0].asNumber() : GetModuleHandleA(nullptr);
                    HRSRC   hres = (HRSRC)(uintptr_t)(size_t)a[1].asNumber();
                    HGLOBAL h = LoadResource(mod, hres);
                    return h ? nova_num((double)(uintptr_t)h) : nova_null(); }, "Load"));
                resObj->obj->set("Lock", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
                    if (a.empty()) return nova_null();
                    HGLOBAL h = (HGLOBAL)(uintptr_t)(size_t)a[0].asNumber();
                    DWORD   sz = a.size() > 1 ? (DWORD)a[1].asNumber() : 0;
                    void   *p  = LockResource(h);
                    if (!p) return nova_null();
                    return nova_str(std::string((char *)p, sz)); }, "Lock"));
                resObj->obj->set("Update", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                    if (a.size() < 4) return nova_bool(false);
                    std::string path = a[0].asString(), type = a[1].asString();
                    std::string name = a[2].asString(), data = a[3].asString();
                    HANDLE h = BeginUpdateResourceA(path.c_str(), FALSE);
                    if (!h) return nova_bool(false);
                    BOOL r = UpdateResourceA(h, type.c_str(), name.c_str(),
                        MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                        (LPVOID)data.data(), (DWORD)data.size());
                    EndUpdateResourceA(h, !r);
                    return nova_bool(r != 0); }, "Update"));
                winObj->obj->set("Resources", resObj);
            }

            // ── Device ────────────────────────────────────────────────────────
            {
                auto devObj = nova_obj();
                devObj->obj->set("RegisterDeviceNotification", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                                     {
                    HWND hwnd = a.size() > 0 ? (HWND)(uintptr_t)(size_t)a[0].asNumber() : nullptr;
                    DEV_BROADCAST_DEVICEINTERFACE dbd;
                    ZeroMemory(&dbd, sizeof(dbd));
                    dbd.dbcc_size       = sizeof(dbd);
                    dbd.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
                    dbd.dbcc_classguid  = GUID_NULL;
                    HDEVNOTIFY h = ::RegisterDeviceNotificationA(hwnd, &dbd,
                        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
                    return h ? nova_num((double)(uintptr_t)h) : nova_null(); }, "RegisterDeviceNotification"));
                devObj->obj->set("EnumDevices", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                      {
                    ValVec arr;
                    HKEY enumKey;
                    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                        "SYSTEM\\CurrentControlSet\\Enum", 0, KEY_READ, &enumKey) != ERROR_SUCCESS)
                        return nova_arr(arr);
                    char bus[256];
                    for (DWORD bi = 0; RegEnumKeyA(enumKey, bi, bus, sizeof(bus)) == ERROR_SUCCESS && arr.size() < 512; bi++) {
                        HKEY busKey;
                        if (RegOpenKeyExA(enumKey, bus, 0, KEY_READ, &busKey) != ERROR_SUCCESS) continue;
                        char dev[256];
                        for (DWORD di = 0; RegEnumKeyA(busKey, di, dev, sizeof(dev)) == ERROR_SUCCESS && arr.size() < 512; di++) {
                            std::string devPath = std::string(bus) + "\\" + dev;
                            HKEY devKey;
                            if (RegOpenKeyExA(enumKey, devPath.c_str(), 0, KEY_READ, &devKey) != ERROR_SUCCESS) continue;
                            char inst[256];
                            for (DWORD ii = 0; RegEnumKeyA(devKey, ii, inst, sizeof(inst)) == ERROR_SUCCESS && arr.size() < 512; ii++) {
                                std::string instPath = devPath + "\\" + inst;
                                HKEY instKey;
                                if (RegOpenKeyExA(enumKey, instPath.c_str(), 0, KEY_READ, &instKey) != ERROR_SUCCESS) continue;
                                auto entry = nova_obj(); char buf[512]; DWORD sz, type;
                                sz = sizeof(buf); type = 0;
                                if (RegQueryValueExA(instKey, "FriendlyName", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
                                    entry->obj->set("name", nova_str(std::string(buf, sz > 0 ? sz - 1 : 0)));
                                else
                                    entry->obj->set("name", nova_str(devPath + "/" + inst));
                                entry->obj->set("instanceId", nova_str(instPath));
                                sz = sizeof(buf); type = 0;
                                if (RegQueryValueExA(instKey, "Mfg", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
                                    entry->obj->set("manufacturer", nova_str(std::string(buf, sz > 0 ? sz - 1 : 0)));
                                sz = sizeof(buf); type = 0;
                                if (RegQueryValueExA(instKey, "Service", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
                                    entry->obj->set("service", nova_str(std::string(buf, sz > 0 ? sz - 1 : 0)));
                                RegCloseKey(instKey);
                                arr.push_back(entry);
                            }
                            RegCloseKey(devKey);
                        }
                        RegCloseKey(busKey);
                    }
                    RegCloseKey(enumKey);
                    return nova_arr(arr); }, "EnumDevices"));
                winObj->obj->set("Device", devObj);
            }

            std_obj->obj->set("Windows", winObj);
        }
#endif // _WIN32

        // ════════════════════════════════════════════════════════════════════════
        //  Std.Apple — macOS/Apple-specific APIs
        // ════════════════════════════════════════════════════════════════════════
#if defined(__APPLE__)
        {
            auto appleObj = nova_obj();

            // ── Beep (AudioToolbox sine wave) ─────────────────────────────────
            // Std.Apple.Beep(freq, ms)
            appleObj->obj->set("Beep", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
                int freq = a.size() > 0 ? (int)a[0].asNumber() : 750;
                int ms   = a.size() > 1 ? (int)a[1].asNumber() : 300;
                // Synthesise a sine wave and play it via AudioQueue
                const int sampleRate = 44100;
                int numSamples = sampleRate * ms / 1000;
                if (numSamples <= 0) return nova_bool(false);

                AudioStreamBasicDescription fmt = {};
                fmt.mSampleRate       = sampleRate;
                fmt.mFormatID         = kAudioFormatLinearPCM;
                fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                fmt.mFramesPerPacket  = 1;
                fmt.mChannelsPerFrame = 1;
                fmt.mBitsPerChannel   = 32;
                fmt.mBytesPerPacket   = 4;
                fmt.mBytesPerFrame    = 4;

                std::vector<float> buf(numSamples);
                int fade = sampleRate * 10 / 1000;
                for (int i = 0; i < numSamples; i++) {
                    float t   = (float)i / sampleRate;
                    float env = 1.0f;
                    if (i < fade)              env = (float)i / fade;
                    else if (i > numSamples - fade) env = (float)(numSamples - i) / fade;
                    buf[i] = env * 0.5f * sinf(2.0f * (float)M_PI * freq * t);
                }

                AudioQueueRef queue = nullptr;
                struct Ctx { bool done; };
                auto ctx = std::make_shared<Ctx>(); ctx->done = false;
                auto ctxRaw = ctx.get();
                AudioQueueNewOutput(&fmt, [](void *ud, AudioQueueRef q, AudioQueueBufferRef) {
                    static_cast<Ctx*>(ud)->done = true;
                    AudioQueueStop(q, false);
                }, ctxRaw, nullptr, nullptr, 0, &queue);

                if (!queue) return nova_bool(false);
                AudioQueueBufferRef qbuf;
                AudioQueueAllocateBuffer(queue, numSamples * 4, &qbuf);
                memcpy(qbuf->mAudioData, buf.data(), numSamples * 4);
                qbuf->mAudioDataByteSize = numSamples * 4;
                AudioQueueEnqueueBuffer(queue, qbuf, 0, nullptr);
                AudioQueueStart(queue, nullptr);
                usleep((ms + 50) * 1000);
                AudioQueueDispose(queue, true);
                return nova_bool(true); }, "Beep"));

            // ── NSBeep (system alert sound) ────────────────────────────────────
            appleObj->obj->set("SystemBeep", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   {
                std::system("afplay /System/Library/Sounds/Ping.aiff &");
                return nova_null(); }, "SystemBeep"));

            // ── Notification (macOS notification center via osascript) ─────────
            appleObj->obj->set("Notify", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
                std::string title   = a.size() > 0 ? a[0].asString() : "Nova";
                std::string message = a.size() > 1 ? a[1].asString() : "";
                std::string subtitle= a.size() > 2 ? a[2].asString() : "";
                std::string cmd = "osascript -e 'display notification \"" + message +
                                  "\" with title \"" + title + "\"";
                if (!subtitle.empty()) cmd += " subtitle \"" + subtitle + "\"";
                cmd += "' &";
                std::system(cmd.c_str());
                return nova_null(); }, "Notify"));

            // ── SpeakText (text-to-speech via say) ────────────────────────────
            appleObj->obj->set("Speak", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                              {
                if (a.empty()) return nova_null();
                std::string text  = a[0].asString();
                std::string voice = a.size() > 1 ? a[1].asString() : "";
                std::string cmd = "say";
                if (!voice.empty()) cmd += " -v \"" + voice + "\"";
                cmd += " \"" + text + "\" &";
                std::system(cmd.c_str());
                return nova_null(); }, "Speak"));

            // ── Pasteboard (clipboard) ─────────────────────────────────────────
            auto pbObj = nova_obj();
            pbObj->obj->set("GetText", NovaValue::makeNative([](ValVec, auto) -> Val
                                                             {
                FILE *fp = popen("pbpaste", "r");
                if (!fp) return nova_null();
                std::string out; char buf[256];
                while (fgets(buf, sizeof(buf), fp)) out += buf;
                pclose(fp);
                return nova_str(out); }, "GetText"));
            pbObj->obj->set("SetText", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
                if (a.empty()) return nova_bool(false);
                std::string s = a[0].asString();
                FILE *fp = popen("pbcopy", "w");
                if (!fp) return nova_bool(false);
                fwrite(s.c_str(), 1, s.size(), fp);
                pclose(fp);
                return nova_bool(true); }, "SetText"));
            appleObj->obj->set("Pasteboard", pbObj);

            // ── sysctl wrappers ────────────────────────────────────────────────
            appleObj->obj->set("GetCPUBrand", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                    {
                char buf[256] = {}; size_t sz = sizeof(buf);
                sysctlbyname("machdep.cpu.brand_string", buf, &sz, nullptr, 0);
                return nova_str(buf); }, "GetCPUBrand"));

            appleObj->obj->set("GetPhysicalMemory", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                          {
                uint64_t mem = 0; size_t sz = sizeof(mem);
                sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0);
                return nova_num((double)mem); }, "GetPhysicalMemory"));

            appleObj->obj->set("GetCPUCount", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                    {
                int n = 0; size_t sz = sizeof(n);
                sysctlbyname("hw.logicalcpu", &n, &sz, nullptr, 0);
                return nova_num((double)n); }, "GetCPUCount"));

            appleObj->obj->set("GetOSVersion", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                     {
                char buf[64] = {}; size_t sz = sizeof(buf);
                sysctlbyname("kern.osrelease", buf, &sz, nullptr, 0);
                return nova_str(buf); }, "GetOSVersion"));

            // ── open URL / file with default app ──────────────────────────────
            appleObj->obj->set("Open", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
                if (a.empty()) return nova_bool(false);
                std::string cmd = "open \"" + a[0].asString() + "\" &";
                return nova_bool(std::system(cmd.c_str()) == 0); }, "Open"));

            // ── Dark mode check ────────────────────────────────────────────────
            appleObj->obj->set("IsDarkMode", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   {
                FILE *fp = popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r");
                if (!fp) return nova_bool(false);
                char buf[64] = {}; fgets(buf, sizeof(buf), fp); pclose(fp);
                std::string s(buf);
                std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                return nova_bool(s.find("dark") != std::string::npos); }, "IsDarkMode"));

            // ── Keychain (simple password store via security CLI) ──────────────
            auto kcObj = nova_obj();
            kcObj->obj->set("FindPassword", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
                if (a.size() < 2) return nova_null();
                std::string cmd = "security find-generic-password -a \"" + a[0].asString() +
                                  "\" -s \"" + a[1].asString() + "\" -w 2>/dev/null";
                FILE *fp = popen(cmd.c_str(), "r");
                if (!fp) return nova_null();
                char buf[512] = {}; fgets(buf, sizeof(buf), fp); pclose(fp);
                std::string s(buf);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                return s.empty() ? nova_null() : nova_str(s); }, "FindPassword"));
            kcObj->obj->set("AddPassword", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                if (a.size() < 3) return nova_bool(false);
                std::string cmd = "security add-generic-password -a \"" + a[0].asString() +
                                  "\" -s \"" + a[1].asString() + "\" -w \"" + a[2].asString() + "\" 2>/dev/null";
                return nova_bool(std::system(cmd.c_str()) == 0); }, "AddPassword"));
            appleObj->obj->set("Keychain", kcObj);

            // ── mach_absolute_time high-res timer ─────────────────────────────
            appleObj->obj->set("MachTime", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                 { return nova_num((double)mach_absolute_time()); }, "MachTime"));

            // ── process info ──────────────────────────────────────────────────
            appleObj->obj->set("GetPID", NovaValue::makeNative([](ValVec, auto) -> Val
                                                               { return nova_num((double)getpid()); }, "GetPID"));

            appleObj->obj->set("GetParentPID", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                     { return nova_num((double)getppid()); }, "GetParentPID"));

            std_obj->obj->set("Apple", appleObj);
        }
#else
        std_obj->obj->set("Apple", nova_null());
#endif // __APPLE__

        // ════════════════════════════════════════════════════════════════════════
        //  Std.Linux — Linux-specific APIs
        // ════════════════════════════════════════════════════════════════════════
#if defined(__linux__)
        {
            auto linuxObj = nova_obj();

            // ── Beep (ALSA sine wave) ──────────────────────────────────────────
            // Std.Linux.Beep(freq, ms)
            linuxObj->obj->set("Beep", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
                                                                 int freq = a.size() > 0 ? (int)a[0].asNumber() : 750;
                                                                 int ms = a.size() > 1 ? (int)a[1].asNumber() : 300;
                                                                 const int sampleRate = 44100;
                                                                 int numSamples = sampleRate * ms / 1000;
                                                                 if (numSamples <= 0)
                                                                     return nova_bool(false);

                                                                 std::vector<int16_t> buf(numSamples);
                                                                 int fade = sampleRate * 10 / 1000;
                                                                 for (int i = 0; i < numSamples; i++)
                                                                 {
                                                                     float t = (float)i / sampleRate;
                                                                     float env = 1.0f;
                                                                     if (i < fade)
                                                                         env = (float)i / fade;
                                                                     else if (i > numSamples - fade)
                                                                         env = (float)(numSamples - i) / fade;
                                                                     buf[i] = (int16_t)(env * 0.5f * 32767 * sinf(2.0f * (float)M_PI * freq * t));
                                                                 }

#if __has_include(<alsa/asoundlib.h>)
                                                                 snd_pcm_t *pcm = nullptr;
                                                                 if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
                                                                     return nova_bool(false);
                                                                 snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                                                                    1, sampleRate, 1, ms * 1000);
                                                                 snd_pcm_writei(pcm, buf.data(), numSamples);
                                                                 snd_pcm_drain(pcm);
                                                                 snd_pcm_close(pcm);
                                                                 return nova_bool(true);
#else
                                                                 // fallback: terminal bell
                                                                 std::system("echo -e '\\a'");
                                                                 return nova_bool(false);
#endif
                                                             },
                                                             "Beep"));

            // ── /proc/cpuinfo ─────────────────────────────────────────────────
            linuxObj->obj->set("GetCPUInfo", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   {
                auto obj = nova_obj();
                std::ifstream f("/proc/cpuinfo");
                if (!f) return obj;
                std::string line;
                while (std::getline(f, line)) {
                    size_t c = line.find(':');
                    if (c == std::string::npos) continue;
                    std::string key = line.substr(0, c);
                    std::string val = line.substr(c + 2);
                    // trim key
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                    // first occurrence wins
                    if (!obj->obj->get(key)) obj->obj->set(key, nova_str(val));
                }
                return obj; }, "GetCPUInfo"));

            // ── /proc/meminfo ─────────────────────────────────────────────────
            linuxObj->obj->set("GetMemInfo", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   {
                auto obj = nova_obj();
                std::ifstream f("/proc/meminfo");
                if (!f) return obj;
                std::string line;
                while (std::getline(f, line)) {
                    size_t c = line.find(':');
                    if (c == std::string::npos) continue;
                    std::string key = line.substr(0, c);
                    std::string val = line.substr(c + 1);
                    size_t s = val.find_first_not_of(" \t");
                    val = (s == std::string::npos) ? "" : val.substr(s);
                    obj->obj->set(key, nova_str(val));
                }
                return obj; }, "GetMemInfo"));

            // ── /proc/stat (CPU usage snapshot) ──────────────────────────────
            linuxObj->obj->set("GetCPUStat", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   {
                auto arr = nova_arr();
                std::ifstream f("/proc/stat");
                if (!f) return arr;
                std::string line;
                while (std::getline(f, line)) {
                    if (line.rfind("cpu", 0) != 0) continue;
                    auto entry = nova_obj();
                    std::istringstream ss(line);
                    std::string name; ss >> name; entry->obj->set("name", nova_str(name));
                    double user,nice,sys,idle,iowait,irq,softirq,steal;
                    ss >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
                    entry->obj->set("user",    nova_num(user));
                    entry->obj->set("nice",    nova_num(nice));
                    entry->obj->set("system",  nova_num(sys));
                    entry->obj->set("idle",    nova_num(idle));
                    entry->obj->set("iowait",  nova_num(iowait));
                    entry->obj->set("irq",     nova_num(irq));
                    entry->obj->set("softirq", nova_num(softirq));
                    entry->obj->set("steal",   nova_num(steal));
                    arr->arr->push(entry);
                }
                return arr; }, "GetCPUStat"));

            // ── /proc/loadavg ─────────────────────────────────────────────────
            linuxObj->obj->set("GetLoadAvg", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                   {
                double load[3] = {};
                getloadavg(load, 3);
                auto obj = nova_obj();
                obj->obj->set("1min",  nova_num(load[0]));
                obj->obj->set("5min",  nova_num(load[1]));
                obj->obj->set("15min", nova_num(load[2]));
                return obj; }, "GetLoadAvg"));

            // ── /proc/net/dev ─────────────────────────────────────────────────
            linuxObj->obj->set("GetNetworkStats", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                        {
                auto obj = nova_obj();
                std::ifstream f("/proc/net/dev");
                if (!f) return obj;
                std::string line;
                std::getline(f, line); std::getline(f, line); // skip headers
                while (std::getline(f, line)) {
                    size_t colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    std::string iface = line.substr(0, colon);
                    size_t s = iface.find_first_not_of(" \t");
                    iface = (s == std::string::npos) ? iface : iface.substr(s);
                    while (!iface.empty() && iface.back() == ' ') iface.pop_back();
                    std::istringstream ss(line.substr(colon + 1));
                    uint64_t rxBytes, rxPkts, rxErr, rxDrop, x1, x2, x3, x4;
                    uint64_t txBytes, txPkts, txErr, txDrop;
                    ss >> rxBytes >> rxPkts >> rxErr >> rxDrop >> x1 >> x2 >> x3 >> x4
                       >> txBytes >> txPkts >> txErr >> txDrop;
                    auto entry = nova_obj();
                    entry->obj->set("rxBytes", nova_num((double)rxBytes));
                    entry->obj->set("rxPackets",nova_num((double)rxPkts));
                    entry->obj->set("rxErrors", nova_num((double)rxErr));
                    entry->obj->set("txBytes",  nova_num((double)txBytes));
                    entry->obj->set("txPackets",nova_num((double)txPkts));
                    entry->obj->set("txErrors", nova_num((double)txErr));
                    obj->obj->set(iface, entry);
                }
                return obj; }, "GetNetworkStats"));

            // ── /proc/self/status ─────────────────────────────────────────────
            linuxObj->obj->set("GetSelfStatus", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                      {
                auto obj = nova_obj();
                std::ifstream f("/proc/self/status");
                if (!f) return obj;
                std::string line;
                while (std::getline(f, line)) {
                    size_t c = line.find(':');
                    if (c == std::string::npos) continue;
                    std::string key = line.substr(0, c);
                    std::string val = line.substr(c + 1);
                    size_t s = val.find_first_not_of(" \t");
                    val = (s == std::string::npos) ? "" : val.substr(s);
                    obj->obj->set(key, nova_str(val));
                }
                return obj; }, "GetSelfStatus"));

            // ── inotify (file watch) ──────────────────────────────────────────
            linuxObj->obj->set("WatchFile", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
                if (a.empty()) return nova_null();
                std::string path = a[0].asString();
                int fd = inotify_init1(IN_NONBLOCK);
                if (fd < 0) return nova_null();
                uint32_t mask = a.size() > 1 ? (uint32_t)a[1].asNumber()
                                             : IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
                int wd = inotify_add_watch(fd, path.c_str(), mask);
                if (wd < 0) { close(fd); return nova_null(); }
                auto obj = nova_obj();
                auto fdShared = std::make_shared<int>(fd);
                obj->obj->set("fd", nova_num((double)fd));
                obj->obj->set("wd", nova_num((double)wd));
                obj->obj->set("read", NovaValue::makeNative([fdShared](ValVec, auto) -> Val {
                    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
                    ssize_t n = read(*fdShared, buf, sizeof(buf));
                    if (n <= 0) return nova_arr();
                    auto arr = nova_arr();
                    for (char *p = buf; p < buf + n; ) {
                        auto *ev = (struct inotify_event *)p;
                        auto entry = nova_obj();
                        entry->obj->set("mask", nova_num((double)ev->mask));
                        entry->obj->set("name", nova_str(ev->len ? ev->name : ""));
                        arr->arr->push(entry);
                        p += sizeof(struct inotify_event) + ev->len;
                    }
                    return arr;
                }, "read"));
                obj->obj->set("close", NovaValue::makeNative([fdShared](ValVec, auto) -> Val {
                    close(*fdShared); return nova_null();
                }, "close"));
                // inotify mask constants
                obj->obj->set("IN_MODIFY",      nova_num(IN_MODIFY));
                obj->obj->set("IN_CREATE",      nova_num(IN_CREATE));
                obj->obj->set("IN_DELETE",      nova_num(IN_DELETE));
                obj->obj->set("IN_MOVED_FROM",  nova_num(IN_MOVED_FROM));
                obj->obj->set("IN_MOVED_TO",    nova_num(IN_MOVED_TO));
                obj->obj->set("IN_CLOSE_WRITE", nova_num(IN_CLOSE_WRITE));
                return obj; }, "WatchFile"));

            // inotify constants at top level too
            linuxObj->obj->set("IN_MODIFY", nova_num(IN_MODIFY));
            linuxObj->obj->set("IN_CREATE", nova_num(IN_CREATE));
            linuxObj->obj->set("IN_DELETE", nova_num(IN_DELETE));
            linuxObj->obj->set("IN_MOVED_FROM", nova_num(IN_MOVED_FROM));
            linuxObj->obj->set("IN_MOVED_TO", nova_num(IN_MOVED_TO));
            linuxObj->obj->set("IN_CLOSE_WRITE", nova_num(IN_CLOSE_WRITE));

            // ── epoll ─────────────────────────────────────────────────────────
            linuxObj->obj->set("EpollCreate", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                    {
                int fd = epoll_create1(0);
                if (fd < 0) return nova_null();
                auto obj = nova_obj();
                auto fdShared = std::make_shared<int>(fd);
                obj->obj->set("fd", nova_num((double)fd));
                obj->obj->set("add", NovaValue::makeNative([fdShared](ValVec a, auto) -> Val {
                    if (a.empty()) return nova_bool(false);
                    int targetFd = (int)a[0].asNumber();
                    uint32_t events = a.size() > 1 ? (uint32_t)a[1].asNumber() : EPOLLIN;
                    struct epoll_event ev; ev.events = events; ev.data.fd = targetFd;
                    return nova_bool(epoll_ctl(*fdShared, EPOLL_CTL_ADD, targetFd, &ev) == 0);
                }, "add"));
                obj->obj->set("del", NovaValue::makeNative([fdShared](ValVec a, auto) -> Val {
                    if (a.empty()) return nova_bool(false);
                    int targetFd = (int)a[0].asNumber();
                    return nova_bool(epoll_ctl(*fdShared, EPOLL_CTL_DEL, targetFd, nullptr) == 0);
                }, "del"));
                obj->obj->set("wait", NovaValue::makeNative([fdShared](ValVec a, auto) -> Val {
                    int timeout = a.empty() ? -1 : (int)a[0].asNumber();
                    int maxEvents = a.size() > 1 ? (int)a[1].asNumber() : 64;
                    std::vector<struct epoll_event> evts(maxEvents);
                    int n = epoll_wait(*fdShared, evts.data(), maxEvents, timeout);
                    if (n < 0) return nova_arr();
                    auto arr = nova_arr();
                    for (int i = 0; i < n; i++) {
                        auto entry = nova_obj();
                        entry->obj->set("fd",     nova_num((double)evts[i].data.fd));
                        entry->obj->set("events", nova_num((double)evts[i].events));
                        arr->arr->push(entry);
                    }
                    return arr;
                }, "wait"));
                obj->obj->set("close", NovaValue::makeNative([fdShared](ValVec, auto) -> Val {
                    close(*fdShared); return nova_null();
                }, "close"));
                return obj; }, "EpollCreate"));

            linuxObj->obj->set("EPOLLIN", nova_num(EPOLLIN));
            linuxObj->obj->set("EPOLLOUT", nova_num(EPOLLOUT));
            linuxObj->obj->set("EPOLLERR", nova_num(EPOLLERR));
            linuxObj->obj->set("EPOLLHUP", nova_num(EPOLLHUP));
            linuxObj->obj->set("EPOLLET", nova_num(EPOLLET));

            // ── sendfile ──────────────────────────────────────────────────────
            linuxObj->obj->set("Sendfile", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                 {
                if (a.size() < 3) return nova_num(-1);
                int outFd  = (int)a[0].asNumber();
                int inFd   = (int)a[1].asNumber();
                size_t cnt = (size_t)a[2].asNumber();
                off_t offset = a.size() > 3 ? (off_t)a[3].asNumber() : 0;
                ssize_t r = sendfile(outFd, inFd, &offset, cnt);
                return nova_num((double)r); }, "Sendfile"));

            // ── mmap ──────────────────────────────────────────────────────────
            linuxObj->obj->set("Mmap", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
                size_t length = a.size() > 0 ? (size_t)a[0].asNumber() : 4096;
                int prot      = a.size() > 1 ? (int)a[1].asNumber() : PROT_READ | PROT_WRITE;
                int flags     = a.size() > 2 ? (int)a[2].asNumber() : MAP_PRIVATE | MAP_ANONYMOUS;
                int fd        = a.size() > 3 ? (int)a[3].asNumber() : -1;
                off_t off     = a.size() > 4 ? (off_t)a[4].asNumber() : 0;
                void *p = mmap(nullptr, length, prot, flags, fd, off);
                if (p == MAP_FAILED) return nova_null();
                auto ptr = std::make_shared<NovaPointer>();
                ptr->isRaw = true; ptr->rawType = parseNativeType("u8");
                ptr->rawAddr = p; ptr->rawSize = length;
                std::ostringstream ss; ss << "0x" << std::hex << (uintptr_t)p; ptr->address = ss.str();
                auto out = std::make_shared<NovaValue>(); out->kind = VK::Pointer; out->ptr = ptr;
                return out; }, "Mmap"));

            linuxObj->obj->set("Munmap", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
                if (a.empty() || !a[0].isPointer() || !a[0]->ptr || !a[0]->ptr->isRaw) return nova_bool(false);
                size_t len = a.size() > 1 ? (size_t)a[1].asNumber() : a[0]->ptr->rawSize;
                return nova_bool(munmap(a[0]->ptr->rawAddr, len) == 0); }, "Munmap"));

            linuxObj->obj->set("PROT_READ", nova_num(PROT_READ));
            linuxObj->obj->set("PROT_WRITE", nova_num(PROT_WRITE));
            linuxObj->obj->set("PROT_EXEC", nova_num(PROT_EXEC));
            linuxObj->obj->set("MAP_PRIVATE", nova_num(MAP_PRIVATE));
            linuxObj->obj->set("MAP_SHARED", nova_num(MAP_SHARED));
            linuxObj->obj->set("MAP_ANONYMOUS", nova_num(MAP_ANONYMOUS));

            // ── prctl (process control) ────────────────────────────────────────
            linuxObj->obj->set("Prctl", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                              {
                if (a.empty()) return nova_num(-1);
                int op  = (int)a[0].asNumber();
                long a1 = a.size() > 1 ? (long)a[1].asNumber() : 0;
                long a2 = a.size() > 2 ? (long)a[2].asNumber() : 0;
                long a3 = a.size() > 3 ? (long)a[3].asNumber() : 0;
                long a4 = a.size() > 4 ? (long)a[4].asNumber() : 0;
                return nova_num((double)prctl(op, a1, a2, a3, a4)); }, "Prctl"));

            linuxObj->obj->set("PR_SET_NAME", nova_num(PR_SET_NAME));
            linuxObj->obj->set("PR_GET_NAME", nova_num(PR_GET_NAME));
            linuxObj->obj->set("PR_SET_DUMPABLE", nova_num(PR_SET_DUMPABLE));

            // ── setrlimit / getrlimit ─────────────────────────────────────────
            linuxObj->obj->set("Getrlimit", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
                if (a.empty()) return nova_null();
                struct rlimit rl;
                if (getrlimit((int)a[0].asNumber(), &rl) != 0) return nova_null();
                auto obj = nova_obj();
                obj->obj->set("cur", nova_num((double)rl.rlim_cur));
                obj->obj->set("max", nova_num((double)rl.rlim_max));
                return obj; }, "Getrlimit"));

            linuxObj->obj->set("Setrlimit", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                  {
                if (a.size() < 3) return nova_bool(false);
                struct rlimit rl;
                rl.rlim_cur = (rlim_t)a[1].asNumber();
                rl.rlim_max = (rlim_t)a[2].asNumber();
                return nova_bool(setrlimit((int)a[0].asNumber(), &rl) == 0); }, "Setrlimit"));

            linuxObj->obj->set("RLIMIT_NOFILE", nova_num(RLIMIT_NOFILE));
            linuxObj->obj->set("RLIMIT_NPROC", nova_num(RLIMIT_NPROC));
            linuxObj->obj->set("RLIMIT_STACK", nova_num(RLIMIT_STACK));
            linuxObj->obj->set("RLIMIT_AS", nova_num(RLIMIT_AS));
            linuxObj->obj->set("RLIM_INFINITY", nova_num((double)RLIM_INFINITY));

            // ── getpid / getppid / getuid / getgid ────────────────────────────
            linuxObj->obj->set("GetPID", NovaValue::makeNative([](ValVec, auto) -> Val
                                                               { return nova_num((double)getpid()); }, "GetPID"));
            linuxObj->obj->set("GetPPID", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                { return nova_num((double)getppid()); }, "GetPPID"));
            linuxObj->obj->set("GetUID", NovaValue::makeNative([](ValVec, auto) -> Val
                                                               { return nova_num((double)getuid()); }, "GetUID"));
            linuxObj->obj->set("GetGID", NovaValue::makeNative([](ValVec, auto) -> Val
                                                               { return nova_num((double)getgid()); }, "GetGID"));
            linuxObj->obj->set("GetEUID", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                { return nova_num((double)geteuid()); }, "GetEUID"));
            linuxObj->obj->set("GetEGID", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                { return nova_num((double)getegid()); }, "GetEGID"));

            // ── hostname ──────────────────────────────────────────────────────
            linuxObj->obj->set("GetHostname", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                    {
                char buf[256] = {}; gethostname(buf, sizeof(buf));
                return nova_str(buf); }, "GetHostname"));

            linuxObj->obj->set("SetHostname", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                    {
                if (a.empty()) return nova_bool(false);
                std::string s = a[0].asString();
                return nova_bool(sethostname(s.c_str(), s.size()) == 0); }, "SetHostname"));

            // ── sysinfo ───────────────────────────────────────────────────────
            linuxObj->obj->set("Sysinfo", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                {
                struct sysinfo si; sysinfo(&si);
                auto obj = nova_obj();
                obj->obj->set("uptime",    nova_num((double)si.uptime));
                obj->obj->set("totalram",  nova_num((double)si.totalram));
                obj->obj->set("freeram",   nova_num((double)si.freeram));
                obj->obj->set("sharedram", nova_num((double)si.sharedram));
                obj->obj->set("bufferram", nova_num((double)si.bufferram));
                obj->obj->set("totalswap", nova_num((double)si.totalswap));
                obj->obj->set("freeswap",  nova_num((double)si.freeswap));
                obj->obj->set("procs",     nova_num((double)si.procs));
                obj->obj->set("mem_unit",  nova_num((double)si.mem_unit));
                return obj; }, "Sysinfo"));

            // ── uname ─────────────────────────────────────────────────────────
            linuxObj->obj->set("Uname", NovaValue::makeNative([](ValVec, auto) -> Val
                                                              {
                struct utsname u; uname(&u);
                auto obj = nova_obj();
                obj->obj->set("sysname",  nova_str(u.sysname));
                obj->obj->set("nodename", nova_str(u.nodename));
                obj->obj->set("release",  nova_str(u.release));
                obj->obj->set("version",  nova_str(u.version));
                obj->obj->set("machine",  nova_str(u.machine));
                return obj; }, "Uname"));

            // ── /proc/uptime ──────────────────────────────────────────────────
            linuxObj->obj->set("GetUptime", NovaValue::makeNative([](ValVec, auto) -> Val
                                                                  {
                std::ifstream f("/proc/uptime");
                if (!f) return nova_null();
                double up, idle; f >> up >> idle;
                auto obj = nova_obj();
                obj->obj->set("uptime", nova_num(up));
                obj->obj->set("idle",   nova_num(idle));
                return obj; }, "GetUptime"));

            // ── clipboard (xclip/xsel/wl-clipboard) ───────────────────────────
            auto cbObj = nova_obj();
            cbObj->obj->set("GetText", NovaValue::makeNative([](ValVec, auto) -> Val
                                                             {
                // Try wayland first, then X11
                FILE *fp = popen("wl-paste 2>/dev/null || xclip -selection clipboard -o 2>/dev/null || xsel --clipboard --output 2>/dev/null", "r");
                if (!fp) return nova_null();
                std::string out; char buf[256];
                while (fgets(buf, sizeof(buf), fp)) out += buf;
                pclose(fp);
                return nova_str(out); }, "GetText"));
            cbObj->obj->set("SetText", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
                if (a.empty()) return nova_bool(false);
                std::string s = a[0].asString();
                // try wl-copy, then xclip, then xsel
                std::string cmd = "echo -n '" + s + "' | wl-copy 2>/dev/null || echo -n '" + s + "' | xclip -selection clipboard 2>/dev/null || echo -n '" + s + "' | xsel --clipboard --input 2>/dev/null";
                return nova_bool(std::system(cmd.c_str()) == 0); }, "SetText"));
            linuxObj->obj->set("Clipboard", cbObj);

            // ── notify-send (desktop notification) ────────────────────────────
            linuxObj->obj->set("Notify", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
                std::string title   = a.size() > 0 ? a[0].asString() : "Nova";
                std::string message = a.size() > 1 ? a[1].asString() : "";
                std::string urgency = a.size() > 2 ? a[2].asString() : "normal";
                std::string cmd = "notify-send -u " + urgency + " \"" + title + "\" \"" + message + "\" 2>/dev/null &";
                std::system(cmd.c_str());
                return nova_null(); }, "Notify"));

            // ── espeak / festival TTS ──────────────────────────────────────────
            linuxObj->obj->set("Speak", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                              {
                if (a.empty()) return nova_null();
                std::string text = a[0].asString();
                std::string cmd = "espeak \"" + text + "\" 2>/dev/null || festival --tts <<< \"" + text + "\" 2>/dev/null &";
                std::system(cmd.c_str());
                return nova_null(); }, "Speak"));

            // ── open URL/file with xdg-open ───────────────────────────────────
            linuxObj->obj->set("Open", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
                if (a.empty()) return nova_bool(false);
                std::string cmd = "xdg-open \"" + a[0].asString() + "\" 2>/dev/null &";
                return nova_bool(std::system(cmd.c_str()) == 0); }, "Open"));

            // ── raw socket helpers ────────────────────────────────────────────
            linuxObj->obj->set("CreateRawSocket", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                        {
                int domain   = a.size() > 0 ? (int)a[0].asNumber() : AF_INET;
                int type     = a.size() > 1 ? (int)a[1].asNumber() : SOCK_RAW;
                int protocol = a.size() > 2 ? (int)a[2].asNumber() : IPPROTO_RAW;
                int fd = socket(domain, type, protocol);
                return fd < 0 ? nova_num(-1) : nova_num((double)fd); }, "CreateRawSocket"));

            linuxObj->obj->set("AF_INET", nova_num(AF_INET));
            linuxObj->obj->set("AF_INET6", nova_num(AF_INET6));
            linuxObj->obj->set("AF_PACKET", nova_num(AF_PACKET));
            linuxObj->obj->set("SOCK_RAW", nova_num(SOCK_RAW));
            linuxObj->obj->set("IPPROTO_RAW", nova_num(IPPROTO_RAW));
            linuxObj->obj->set("IPPROTO_TCP", nova_num(IPPROTO_TCP));
            linuxObj->obj->set("IPPROTO_UDP", nova_num(IPPROTO_UDP));
            linuxObj->obj->set("IPPROTO_ICMP", nova_num(IPPROTO_ICMP));

            std_obj->obj->set("Linux", linuxObj);
        }
#else
        std_obj->obj->set("Linux", nova_null());
#endif // __linux__

        // ── expose Std on global scope ────────────────────────────────────────────
        s->setOwn("Std", std_obj);

        // ── bstd ─────────────────────────────────────────────────────────────────
        _loadBstd(s);
    }

    // ffi

    void Executor::_registerFFI(Val obj)
    {
        auto exe = this;

#if !NOVA_FFI_PLATFORM_SUPPORTED
        obj->obj->set("Dlopen", NovaValue::makeNative([](ValVec, auto) -> Val
                                                      {
        std::cout << "[novac] Std.Experimental.Dlopen: dynamic loading is not supported on this platform\n";
        return nova_null(); }, "Dlopen"));

        obj->obj->set("Dlsym", NovaValue::makeNative([](ValVec, auto) -> Val
                                                     {
        std::cout << "[novac] Std.Experimental.Dlsym: dynamic loading is not supported on this platform\n";
        return nova_null(); }, "Dlsym"));

        obj->obj->set("Mprotect", NovaValue::makeNative([](ValVec, auto) -> Val
                                                        {
        std::cout << "[novac] Std.Experimental.Mprotect: not supported on this platform\n";
        return nova_bool(false); }, "Mprotect"));

        obj->obj->set("Ccall", NovaValue::makeNative([](ValVec, auto) -> Val
                                                     {
        std::cout << "[novac] Std.Experimental.Ccall: not supported on this platform\n";
        return nova_null(); }, "Ccall"));

        return;
#else
        obj->obj->set("Dlopen", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
        std::string path = a.empty() ? "" : a[0].asString();

#ifdef _WIN32
        HMODULE h = path.empty() ? GetModuleHandle(nullptr) : LoadLibraryA(path.c_str());
        if (!h) return nova_null();
        auto handle = std::shared_ptr<void>(h, [](void *) { /* not freed — symbols may still be in use */ });
#else
        std::string flagStr = a.size() > 1 ? a[1].asString() : "now";
        int flags = (flagStr == "lazy") ? RTLD_LAZY : RTLD_NOW;
        void *h = ::dlopen(path.empty() ? nullptr : path.c_str(), flags);
        if (!h)
            return nova_null();
        auto handle = std::shared_ptr<void>(h, [](void *p)
                                             { if (p) ::dlclose(p); });
#endif
        auto out = nova_obj();
        out->obj->set("__type__", nova_str("dlhandle"));
        out->obj->set("path", nova_str(path));

        out->obj->set("sym", NovaValue::makeNative(
            [handle](ValVec sa, auto) -> Val
            {
                if (sa.empty())
                    return nova_null();
                return dlsymImpl(handle.get(), sa[0].asString());
            },
            "sym"));

        return out; }, "Dlopen"));

        obj->obj->set("Dlsym", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                     {
        if (a.size() < 2 || !a[0] || !a[0].isObject())
            return nova_null();
        Val symFn = a[0]->obj->get("sym");
        if (!symFn || !symFn.isFunction())
            return nova_null();
        return exe->callFunction(symFn, {a[1]}, cs); }, "Dlsym"));

#ifndef _WIN32
        obj->obj->set("Dlerror", NovaValue::makeNative([](ValVec, auto) -> Val
                                                       {
        const char *e = ::dlerror();
        return e ? nova_str(e) : nova_null(); }, "Dlerror"));
#endif
        obj->obj->set("Mprotect", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                        {
                                                            if (a.size() < 3)
                                                                return nova_bool(false);
                                                            void *addr = rawAddrOf(a[0]);
                                                            if (!addr)
                                                                exe->_error("Mprotect: argument is not a raw_ptr");
                                                            size_t size = (size_t)a[1].asNumber();
                                                            std::string flagStr = a[2].asString();

                                                            bool r = flagStr.find('r') != std::string::npos;
                                                            bool w = flagStr.find('w') != std::string::npos;
                                                            bool x = flagStr.find('x') != std::string::npos;

#ifdef _WIN32
                                                            DWORD prot = PAGE_NOACCESS;
                                                            if (x && w)
                                                                prot = PAGE_EXECUTE_READWRITE;
                                                            else if (x && r)
                                                                prot = PAGE_EXECUTE_READ;
                                                            else if (x)
                                                                prot = PAGE_EXECUTE;
                                                            else if (w)
                                                                prot = PAGE_READWRITE;
                                                            else if (r)
                                                                prot = PAGE_READONLY;
                                                            DWORD oldProt;
                                                            return nova_bool(VirtualProtect(addr, size, prot, &oldProt) != 0);
#else
                                                            int prot = PROT_NONE;
                                                            if (r)
                                                                prot |= PROT_READ;
                                                            if (w)
                                                                prot |= PROT_WRITE;
                                                            if (x)
                                                                prot |= PROT_EXEC;

                                                            long pageSize = sysconf(_SC_PAGESIZE);
                                                            uintptr_t base = reinterpret_cast<uintptr_t>(addr);
                                                            uintptr_t pageStart = base & ~(uintptr_t)(pageSize - 1);
                                                            size_t adjustedSize = size + (base - pageStart);

                                                            return nova_bool(::mprotect(reinterpret_cast<void *>(pageStart),
                                                                                        adjustedSize, prot) == 0);
#endif
                                                        },
                                                        "Mprotect"));
#if NOVA_CCALL_AVAILABLE
        obj->obj->set("Ccall", NovaValue::makeNative([exe](ValVec a, auto) -> Val
                                                     {
        if (a.size() < 4)
            exe->_error("Ccall: expected (fnPtr, retType, argTypes[], args[])");

        void *fnAddr = rawAddrOf(a[0]);
        if (!fnAddr)
            exe->_error("Ccall: fnPtr is not a raw_ptr (use Dlsym to obtain one)");

        std::string retTypeName = a[1].asString();
        if (!a[2].isArray() || !a[3].isArray())
            exe->_error("Ccall: argTypes and args must be arrays");

        auto &argTypeVals = a[2]->arr->inner;
        auto &argVals = a[3]->arr->inner;
        if (argTypeVals.size() != argVals.size())
            exe->_error("Ccall: argTypes and args length mismatch");

        auto ffiTypeFor = [exe](const std::string &t) -> ffi_type *
        {
            if (t == "void")  return &ffi_type_void;
            if (t == "i8")    return &ffi_type_sint8;
            if (t == "u8")    return &ffi_type_uint8;
            if (t == "i16")   return &ffi_type_sint16;
            if (t == "u16")   return &ffi_type_uint16;
            if (t == "i32")     return &ffi_type_sint32;
            if (t == "hresult") return &ffi_type_sint32;
            if (t == "u32")   return &ffi_type_uint32;
            if (t == "i64")   return &ffi_type_sint64;
            if (t == "u64")   return &ffi_type_uint64;
            if (t == "f32")   return &ffi_type_float;
            if (t == "f64")   return &ffi_type_double;
            if (t == "bool")  return &ffi_type_uint8;
            if (t == "ptr")   return &ffi_type_pointer;
            if (t == "cstr")  return &ffi_type_pointer;
            exe->_error("Ccall: unknown type name '" + t + "'");
            return &ffi_type_void; // unreachable, _error throws
        };

        size_t n = argVals.size();
        std::vector<ffi_type *> argTypes(n);
        for (size_t i = 0; i < n; i++)
            argTypes[i] = ffiTypeFor(argTypeVals[i].asString());
        ffi_type *retType = ffiTypeFor(retTypeName);

        ffi_cif cif;
        if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)n, retType, argTypes.data()) != FFI_OK)
            exe->_error("Ccall: ffi_prep_cif failed (bad type combination)");

        std::vector<int64_t> intStorage(n, 0);
        std::vector<float> f32Storage(n, 0.0f);
        std::vector<double> fltStorage(n, 0);
        std::vector<void *> ptrStorage(n, nullptr);
        std::vector<std::string> cstrKeepalive;
        cstrKeepalive.reserve(n);
        std::vector<void *> argPtrs(n);

        for (size_t i = 0; i < n; i++)
        {
            std::string t = argTypeVals[i].asString();
            Val v = argVals[i];

            if (t == "f32")
            {
                f32Storage[i] = (float)v.asNumber();
                argPtrs[i] = &f32Storage[i];
            }
            else if (t == "f64")
            {
                fltStorage[i] = v.asNumber();
                argPtrs[i] = &fltStorage[i];
            }
            else if (t == "cstr")
            {
                cstrKeepalive.push_back(v.asString());
                ptrStorage[i] = (void *)cstrKeepalive.back().c_str();
                argPtrs[i] = &ptrStorage[i];
            }
            else if (t == "ptr")
            {
                ptrStorage[i] = rawAddrOf(v);
                argPtrs[i] = &ptrStorage[i];
            }
            else if (t == "bool")
            {
                intStorage[i] = v.asBool() ? 1 : 0;
                argPtrs[i] = &intStorage[i];
            }
            else
            {
                intStorage[i] = (int64_t)v.asNumber();
                argPtrs[i] = &intStorage[i];
            }
        }

        union { ffi_arg i; double d; void *p; } retStorage;
        std::memset(&retStorage, 0, sizeof(retStorage));

        ffi_call(&cif, FFI_FN(fnAddr), &retStorage, argPtrs.data());

        if (retTypeName == "void")
            return nova_null();
        if (retTypeName == "f32")
        {
            float fv;
            std::memcpy(&fv, &retStorage, sizeof(float));
            return nova_num((double)fv);
        }
        if (retTypeName == "f64")
            return nova_num(retStorage.d);
        if (retTypeName == "bool")
            return nova_bool(retStorage.i != 0);
        if (retTypeName == "cstr")
            return retStorage.p ? nova_str(std::string((const char *)retStorage.p)) : nova_null();
        if (retTypeName == "ptr")
            return makeRawPtrFromAddress(retStorage.p, NativeType::Ptr, 0);
        return nova_num((double)(int64_t)retStorage.i); }, "Ccall"));
#else
        obj->obj->set("Ccall", NovaValue::makeNative([](ValVec, auto) -> Val
                                                     {
        std::cout << "[novac] Std.Experimental.Ccall: built without libffi support\n";
        return nova_null(); }, "Ccall"));
#endif
#endif
    }

    // experimental
    void Executor::_registerExperimental(Val obj)
    {
        auto exp = nova_obj();

        // Helper lambda to eliminate the massive code duplication
        auto execNativeInternal = [](ValVec a, auto invoke_and_get_result) -> Val
        {
            if (a.empty() || !a[0].isArray())
                return nova_null();

            auto &inner = a[0]->arr->inner;
            size_t size = inner.size();
            if (size == 0)
                return nova_null();

            // Build byte buffer
            std::vector<uint8_t> bytes(size);
            for (size_t i = 0; i < size; i++)
                bytes[i] = (uint8_t)(int)inner[i].asNumber();

            // Allocate executable memory
#if defined(_WIN32)
            void *mem = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!mem)
                return nova_null();
#else
            void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mem == MAP_FAILED)
                return nova_null();
#endif

            // Copy bytes in
            std::memcpy(mem, bytes.data(), size);

            using CallbackResult = decltype(invoke_and_get_result(mem));
            Val resultVal = nullptr;
            double resultNum = 0.0;
            bool isValResult = false;

            if constexpr (std::is_same_v<CallbackResult, Val>)
            {
                try
                {
                    resultVal = invoke_and_get_result(mem);
                    isValResult = true;
                }
                catch (...)
                {
                    // Swallow C++ exceptions (Note: Won't stop raw OS signals like SIGSEGV)
                }
            }
            else
            {
                try
                {
                    resultNum = static_cast<double>(invoke_and_get_result(mem));
                }
                catch (...)
                {
                    // Swallow C++ exceptions (Note: Won't stop raw OS signals like SIGSEGV)
                }
            }

            // Free memory
#if defined(_WIN32)
            VirtualFree(mem, 0, MEM_RELEASE);
#else
            munmap(mem, size);
#endif

            if constexpr (std::is_same_v<CallbackResult, Val>)
            {
                return resultVal ? resultVal : nova_null();
            }
            else
            {
                return nova_num(resultNum);
            }
        };

        // --- Register the variants using the helper ---

        // ExecNative (double)
        exp->obj->set("ExecNative", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                          { return execNativeInternal(a, [](void *mem)
                                                                                      { return reinterpret_cast<double (*)()>(mem)(); }); }, "ExecNative"));

        // ExecNativeInt (int)
        exp->obj->set("ExecNativeInt", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                             { return execNativeInternal(a, [](void *mem)
                                                                                         { return static_cast<double>(reinterpret_cast<int (*)()>(mem)()); }); }, "ExecNativeInt"));

        // ExecNativeVoid (void)
        exp->obj->set("ExecNativeVoid", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                              { return execNativeInternal(a, [](void *mem)
                                                                                          {
            reinterpret_cast<void(*)()>(mem)();
            return 0.0; }); }, "ExecNativeVoid"));

        // ExecNativeBool (bool)
        exp->obj->set("ExecNativeBool", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                              { return execNativeInternal(a, [](void *mem)
                                                                                          { return static_cast<double>(reinterpret_cast<bool (*)()>(mem)()); }); }, "ExecNativeBool"));

        // ExecNativeFloat (float)
        exp->obj->set("ExecNativeFloat", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                               { return execNativeInternal(a, [](void *mem)
                                                                                           { return static_cast<double>(reinterpret_cast<float (*)()>(mem)()); }); }, "ExecNativeFloat"));
        exp->obj->set("ExecNativeInt64", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                               { return execNativeInternal(a, [](void *mem)
                                                                                           { return static_cast<double>(reinterpret_cast<int64_t (*)()>(mem)()); }); }, "ExecNativeInt64"));
        exp->obj->set("ExecNativeUInt32", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                                { return execNativeInternal(a, [](void *mem)
                                                                                            { return static_cast<double>(reinterpret_cast<uint32_t (*)()>(mem)()); }); }, "ExecNativeUInt32"));
        // ExecNativePtr (void*) -> returns a Nova Pointer object (VK::Pointer)
        exp->obj->set("ExecNativePtr", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                             {
        // We reuse our execution helper but change the callback implementation
        return execNativeInternal(a, [](void* mem) {
            // Cast and execute the function as a pointer-returning function signature
            using JitPtrFn = void*(*)();
            JitPtrFn fn = reinterpret_cast<JitPtrFn>(mem);
            void* raw_address = fn();

            // Wrap the raw address return into Nova's genuine pointer object type
            return makeRawPtrFromAddress(raw_address, NativeType::Ptr, 0);
        }); }, "ExecNativePtr"));
        exp->obj->set("ExecNativeString", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                                { return execNativeInternal(a, [](void *mem)
                                                                                            {
        const char* str = reinterpret_cast<const char*(*)()>(mem)();
        return str ? nova_str(str) : nova_null(); }); }, "ExecNativeString"));
        exp->obj->set("ExecNativeInt64", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                               { return execNativeInternal(a, [](void *mem)
                                                                                           { return nova_num(static_cast<double>(reinterpret_cast<int64_t (*)()>(mem)())); }); }, "ExecNativeInt64"));
        exp->obj->set("ExecNativeUInt32", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                                { return execNativeInternal(a, [](void *mem)
                                                                                            { return nova_num(reinterpret_cast<uint32_t (*)()>(mem)()); }); }, "ExecNativeUInt32"));
        // ExecNativeArgs(types, opcodes, args) -> returns Val (variant matching return type)
        // types[0] is the return type, types[1..N] are parameter types.
        exp->obj->set("ExecNativeArgs", NovaValue::makeNative([execNativeInternal](ValVec a, auto) -> Val
                                                              {
        if (a.size() < 2 || !a[0].isArray() || !a[1].isArray())
            return nova_null();

        auto& typeArr = a[0]->arr->inner;
        auto& opcodesVal = a[1]; // passed into execNativeInternal as a[0] inside the wrapper
        ValVec argsVec = (a.size() > 2 && a[2].isArray()) ? a[2]->arr->inner : ValVec{};

        if (typeArr.empty()) 
            return nova_null();

        // 1. Parse signature types
        std::string retType = typeArr[0].asString();
        std::vector<std::string> paramTypes;
        for (size_t i = 1; i < typeArr.size(); ++i) {
            paramTypes.push_back(typeArr[i].asString());
        }

        // 2. Marshall up to 6 registers/arguments into standard uintptr_t array (x86_64 Calling Convention)
        uintptr_t nativeArgs[6] = {0, 0, 0, 0, 0, 0};
        size_t argLimit = std::min<size_t>(paramTypes.size(), 6);

        for (size_t i = 0; i < argLimit; ++i) {
            if (i >= argsVec.size()) break;
            const auto& val = argsVec[i];
            const std::string& tStr = paramTypes[i];

            if (tStr == "int" || tStr == "int32" || tStr == "int32_t") {
                nativeArgs[i] = static_cast<uintptr_t>((int)val.asNumber());
            } 
            else if (tStr == "bool") {
                nativeArgs[i] = static_cast<uintptr_t>(val.asBool() ? 1 : 0);
            } 
            else if (tStr == "void*" || tStr == "ptr" || tStr == "pointer") {
                nativeArgs[i] = reinterpret_cast<uintptr_t>(rawAddrOf(val));
            } 
            else if (tStr == "char*" || tStr == "string" || tStr == "const char*") {
                nativeArgs[i] = reinterpret_cast<uintptr_t>(val.asString().c_str());
            } 
            else if (tStr == "uint32" || tStr == "uint32_t") {
                nativeArgs[i] = static_cast<uintptr_t>(static_cast<uint32_t>(val.asNumber()));
            } 
            else if (tStr == "int64" || tStr == "int64_t") {
                nativeArgs[i] = static_cast<uintptr_t>(static_cast<int64_t>(val.asNumber()));
            } 
            else {
                // Fallback direct double numeric casting raw bitwise/value conversion
                nativeArgs[i] = static_cast<uintptr_t>((int64_t)val.asNumber());
            }
        }

        // We wrap the opcodes in a vector to fit the expected inner layout of execNativeInternal
        ValVec internalWrapper = { opcodesVal };

        // 3. Invoke internal executor block and process return type
        return execNativeInternal(internalWrapper, [retType, nativeArgs](void* mem) -> Val {
            using GenericFn = uintptr_t(*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
            GenericFn fn = reinterpret_cast<GenericFn>(mem);

            // Invoke with the extracted register configurations
            uintptr_t rawResult = fn(nativeArgs[0], nativeArgs[1], nativeArgs[2], nativeArgs[3], nativeArgs[4], nativeArgs[5]);

            // Map standard C-layout outputs back into Nova Value representations
            if (retType == "void") {
                return nova_null();
            }
            if (retType == "int" || retType == "int32" || retType == "int32_t") {
                return nova_num(static_cast<double>(static_cast<int32_t>(rawResult)));
            }
            if (retType == "bool") {
                return nova_bool(rawResult != 0);
            }
            if (retType == "void*" || retType == "ptr" || retType == "pointer") {
                return makeRawPtrFromAddress(reinterpret_cast<void*>(rawResult), NativeType::Ptr, 0);
            }
            if (retType == "char*" || retType == "string" || retType == "const char*") {
                const char* str = reinterpret_cast<const char*>(rawResult);
                return str ? nova_str(str) : nova_null();
            }
            if (retType == "int64" || retType == "int64_t") {
                return nova_num(static_cast<double>(static_cast<int64_t>(rawResult)));
            }

            // Universal numerical numeric fallback
            return nova_num(static_cast<double>(rawResult));
        }); }, "ExecNativeArgs"));

        obj->obj->set("Experimental", exp);
    } // sockets

    void Executor::_registerSocket(Val obj)
    {
        novac::registerSocketBuiltins(*obj->obj);
    }

    // ── math ─────────────────────────────────────────────────────────────────────

    void Executor::_registerCCType(Val obj)
    {
        auto cc = nova_obj();

        // ── classification ────────────────────────────────────────────────────────
        auto charOf = [](Val v) -> int
        {
            if (v.isNumber())
                return (unsigned char)(int)v.asNumber();
            if (v.isString() && !v->sval.empty())
                return (unsigned char)v->sval[0];
            return 0;
        };

#define CC_PRED(name, fn) \
    cc->obj->set(#name, NovaValue::makeNative([charOf](ValVec a, auto) -> Val { return nova_bool(!a.empty() && fn(charOf(a[0]))); }, #name))

        CC_PRED(isalpha, std::isalpha);
        CC_PRED(isdigit, std::isdigit);
        CC_PRED(isalnum, std::isalnum);
        CC_PRED(isspace, std::isspace);
        CC_PRED(isupper, std::isupper);
        CC_PRED(islower, std::islower);
        CC_PRED(ispunct, std::ispunct);
        CC_PRED(isprint, std::isprint);
        CC_PRED(isgraph, std::isgraph);
        CC_PRED(iscntrl, std::iscntrl);
        CC_PRED(isxdigit, std::isxdigit);
        CC_PRED(isblank, std::isblank);
#undef CC_PRED

        // ── conversion ────────────────────────────────────────────────────────────
        cc->obj->set("toupper", NovaValue::makeNative([charOf](ValVec a, auto) -> Val
                                                      {
            if (a.empty()) return nova_null();
            if (a[0].isString()) {
                std::string s = a[0]->sval;
                std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return (char)std::toupper(c); });
                return nova_str(s);
            }
            return nova_num((double)std::toupper(charOf(a[0]))); }, "toupper"));

        cc->obj->set("tolower", NovaValue::makeNative([charOf](ValVec a, auto) -> Val
                                                      {
            if (a.empty()) return nova_null();
            if (a[0].isString()) {
                std::string s = a[0]->sval;
                std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return (char)std::tolower(c); });
                return nova_str(s);
            }
            return nova_num((double)std::tolower(charOf(a[0]))); }, "tolower"));

        obj->obj->set("CCType", cc);
    }

    // ── <algorithm> ───────────────────────────────────────────────────────────────

    void Executor::_registerAlgorithm(Val obj)
    {
        auto alg = nova_obj();
        auto exe = this;

        // next_permutation(array) → { array, done }
        alg->obj->set("nextPermutation", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
            if (a.empty() || !a[0].isArray()) return nova_null();
            ValVec v = a[0]->arr->inner;
            bool more = std::next_permutation(v.begin(), v.end(),
                [](Val x, Val y) { return *x < *y; });
            auto res = nova_obj();
            res->obj->set("array", nova_arr(v));
            res->obj->set("done", nova_bool(!more));
            return res; }, "nextPermutation"));

        // prevPermutation(array) → { array, done }
        alg->obj->set("prevPermutation", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
            if (a.empty() || !a[0].isArray()) return nova_null();
            ValVec v = a[0]->arr->inner;
            bool more = std::prev_permutation(v.begin(), v.end(),
                [](Val x, Val y) { return *x < *y; });
            auto res = nova_obj();
            res->obj->set("array", nova_arr(v));
            res->obj->set("done", nova_bool(!more));
            return res; }, "prevPermutation"));

        // allPermutations(array) → array of all permutations
        alg->obj->set("allPermutations", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
            if (a.empty() || !a[0].isArray()) return nova_arr();
            ValVec v = a[0]->arr->inner;
            std::sort(v.begin(), v.end(), [](Val x, Val y) { return *x < *y; });
            ValVec all;
            do {
                all.push_back(nova_arr(v));
            } while (std::next_permutation(v.begin(), v.end(),
                [](Val x, Val y) { return *x < *y; }));
            return nova_arr(std::move(all)); }, "allPermutations"));

        // nthElement(array, n) → array with nth element in sorted position
        alg->obj->set("nthElement", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.size() < 2 || !a[0].isArray()) return nova_null();
            ValVec v = a[0]->arr->inner;
            int n = (int)a[1].asNumber();
            if (n < 0 || n >= (int)v.size()) return nova_null();
            std::nth_element(v.begin(), v.begin() + n, v.end(),
                [](Val x, Val y) { return *x < *y; });
            return nova_arr(std::move(v)); }, "nthElement"));

        // rotate(array, n) → rotated array (n positions left)
        alg->obj->set("rotate", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.size() < 2 || !a[0].isArray()) return nova_null();
            ValVec v = a[0]->arr->inner;
            int n = (int)a[1].asNumber() % (int)v.size();
            if (n < 0) n += (int)v.size();
            std::rotate(v.begin(), v.begin() + n, v.end());
            return nova_arr(std::move(v)); }, "rotate"));

        // partition(array, predFn) → { matched, unmatched }
        alg->obj->set("partition", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                         {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isFunction()) return nova_null();
            ValVec matched, unmatched;
            for (auto& v : a[0]->arr->inner) {
                if (exe->callFunction(a[1], {v}, cs).asBool()) matched.push_back(v);
                else unmatched.push_back(v);
            }
            auto res = nova_obj();
            res->obj->set("matched",   nova_arr(std::move(matched)));
            res->obj->set("unmatched", nova_arr(std::move(unmatched)));
            return res; }, "partition"));

        // stablePartition(array, predFn) → { matched, unmatched } (order preserved)
        alg->obj->set("stablePartition", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                               {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isFunction()) return nova_null();
            ValVec matched, unmatched;
            for (auto& v : a[0]->arr->inner) {
                if (exe->callFunction(a[1], {v}, cs).asBool()) matched.push_back(v);
                else unmatched.push_back(v);
            }
            auto res = nova_obj();
            res->obj->set("matched",   nova_arr(std::move(matched)));
            res->obj->set("unmatched", nova_arr(std::move(unmatched)));
            return res; }, "stablePartition"));

        // setUnion(a, b) — both must be sorted
        alg->obj->set("setUnion", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_arr();
            ValVec out;
            std::set_union(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                           a[1]->arr->inner.begin(), a[1]->arr->inner.end(),
                           std::back_inserter(out), [](Val x, Val y) { return *x < *y; });
            return nova_arr(std::move(out)); }, "setUnion"));

        // setIntersection(a, b)
        alg->obj->set("setIntersection", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                               {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_arr();
            ValVec out;
            std::set_intersection(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                                  a[1]->arr->inner.begin(), a[1]->arr->inner.end(),
                                  std::back_inserter(out), [](Val x, Val y) { return *x < *y; });
            return nova_arr(std::move(out)); }, "setIntersection"));

        // setDifference(a, b)
        alg->obj->set("setDifference", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_arr();
            ValVec out;
            std::set_difference(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                                a[1]->arr->inner.begin(), a[1]->arr->inner.end(),
                                std::back_inserter(out), [](Val x, Val y) { return *x < *y; });
            return nova_arr(std::move(out)); }, "setDifference"));

        // setSymmetricDifference(a, b)
        alg->obj->set("setSymmetricDifference", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                                      {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_arr();
            ValVec out;
            std::set_symmetric_difference(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                                          a[1]->arr->inner.begin(), a[1]->arr->inner.end(),
                                          std::back_inserter(out), [](Val x, Val y) { return *x < *y; });
            return nova_arr(std::move(out)); }, "setSymmetricDifference"));

        // isSorted(array) → bool
        alg->obj->set("isSorted", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.empty() || !a[0].isArray()) return nova_bool(true);
            return nova_bool(std::is_sorted(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                [](Val x, Val y) { return *x < *y; })); }, "isSorted"));

        // isPermutation(a, b) → bool
        alg->obj->set("isPermutation", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_bool(false);
            return nova_bool(std::is_permutation(
                a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                a[1]->arr->inner.begin(), a[1]->arr->inner.end(),
                [](Val x, Val y) { return *x == *y; })); }, "isPermutation"));

        // countIf(array, predFn) → number
        alg->obj->set("countIf", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                       {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isFunction()) return nova_num(0);
            int count = 0;
            for (auto& v : a[0]->arr->inner)
                if (exe->callFunction(a[1], {v}, cs).asBool()) count++;
            return nova_num((double)count); }, "countIf"));

        // minElement(array) / maxElement(array)
        alg->obj->set("minElement", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.empty() || !a[0].isArray() || a[0]->arr->inner.empty()) return nova_null();
            return *std::min_element(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                [](Val x, Val y) { return *x < *y; }); }, "minElement"));

        alg->obj->set("maxElement", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.empty() || !a[0].isArray() || a[0]->arr->inner.empty()) return nova_null();
            return *std::max_element(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                [](Val x, Val y) { return *x < *y; }); }, "maxElement"));

        // minmaxElement(array) → { min, max }
        alg->obj->set("minmaxElement", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                             {
            if (a.empty() || !a[0].isArray() || a[0]->arr->inner.empty()) return nova_null();
            auto [mn, mx] = std::minmax_element(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                [](Val x, Val y) { return *x < *y; });
            auto res = nova_obj();
            res->obj->set("min", *mn);
            res->obj->set("max", *mx);
            return res; }, "minmaxElement"));

        // fill(n, value) → array of n copies of value
        alg->obj->set("fill", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                    {
            if (a.size() < 2) return nova_arr();
            int n = (int)a[0].asNumber();
            ValVec out(n, a[1]);
            return nova_arr(std::move(out)); }, "fill"));

        // generate(n, fn) → array of n values from fn(i)
        alg->obj->set("generate", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                        {
            if (a.size() < 2 || !a[1].isFunction()) return nova_arr();
            int n = (int)a[0].asNumber();
            ValVec out;
            out.reserve(n);
            for (int i = 0; i < n; i++)
                out.push_back(exe->callFunction(a[1], {nova_num(i)}, cs));
            return nova_arr(std::move(out)); }, "generate"));

        // binarySearch(sortedArray, value) → bool
        alg->obj->set("binarySearch", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                            {
            if (a.size() < 2 || !a[0].isArray()) return nova_bool(false);
            return nova_bool(std::binary_search(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                a[1], [](Val x, Val y) { return *x < *y; })); }, "binarySearch"));

        // lowerBound(sortedArray, value) → index
        alg->obj->set("lowerBound", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.size() < 2 || !a[0].isArray()) return nova_num(0);
            auto it = std::lower_bound(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                a[1], [](Val x, Val y) { return *x < *y; });
            return nova_num((double)std::distance(a[0]->arr->inner.begin(), it)); }, "lowerBound"));

        // upperBound(sortedArray, value) → index
        alg->obj->set("upperBound", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.size() < 2 || !a[0].isArray()) return nova_num(0);
            auto it = std::upper_bound(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                a[1], [](Val x, Val y) { return *x < *y; });
            return nova_num((double)std::distance(a[0]->arr->inner.begin(), it)); }, "upperBound"));

        // transform(array, fn) → mapped array (alias of map, but stdlib name)
        alg->obj->set("transform", NovaValue::makeNative([exe](ValVec a, auto cs) -> Val
                                                         {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isFunction()) return nova_arr();
            ValVec out;
            out.reserve(a[0]->arr->inner.size());
            for (int i = 0; i < (int)a[0]->arr->inner.size(); i++)
                out.push_back(exe->callFunction(a[1], {a[0]->arr->inner[i], nova_num(i)}, cs));
            return nova_arr(std::move(out)); }, "transform"));

        // unique(array) → deduplicated array (adjacent duplicates removed, like STL)
        alg->obj->set("unique", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.empty() || !a[0].isArray()) return nova_arr();
            ValVec v = a[0]->arr->inner;
            auto it = std::unique(v.begin(), v.end(), [](Val x, Val y) { return *x == *y; });
            v.erase(it, v.end());
            return nova_arr(std::move(v)); }, "unique"));

        // mergeSort(a, b) → merged sorted array from two sorted arrays
        alg->obj->set("merge", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_arr();
            ValVec out;
            std::merge(a[0]->arr->inner.begin(), a[0]->arr->inner.end(),
                       a[1]->arr->inner.begin(), a[1]->arr->inner.end(),
                       std::back_inserter(out), [](Val x, Val y) { return *x < *y; });
            return nova_arr(std::move(out)); }, "merge"));

        obj->obj->set("Algorithm", alg);
    }

    // ── <bit> (C++20) ─────────────────────────────────────────────────────────────

    void Executor::_registerBit(Val obj)
    {
        auto b = nova_obj();

        b->obj->set("popcount", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.empty()) return nova_num(0);
            return nova_num((double)std::popcount((uint64_t)a[0].asNumber())); }, "popcount"));

        b->obj->set("countlZero", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.empty()) return nova_num(64);
            uint64_t v = (uint64_t)a[0].asNumber();
            return nova_num(v == 0 ? 64.0 : (double)std::countl_zero(v)); }, "countlZero"));

        b->obj->set("countrZero", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.empty()) return nova_num(64);
            uint64_t v = (uint64_t)a[0].asNumber();
            return nova_num(v == 0 ? 64.0 : (double)std::countr_zero(v)); }, "countrZero"));

        b->obj->set("countlOne", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                       {
            if (a.empty()) return nova_num(0);
            return nova_num((double)std::countl_one((uint64_t)a[0].asNumber())); }, "countlOne"));

        b->obj->set("countrOne", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                       {
            if (a.empty()) return nova_num(0);
            return nova_num((double)std::countr_one((uint64_t)a[0].asNumber())); }, "countrOne"));

        b->obj->set("rotl", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_num(0);
            return nova_num((double)std::rotl((uint64_t)a[0].asNumber(), (int)a[1].asNumber())); }, "rotl"));

        b->obj->set("rotr", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_num(0);
            return nova_num((double)std::rotr((uint64_t)a[0].asNumber(), (int)a[1].asNumber())); }, "rotr"));

        b->obj->set("hasSingleBit", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.empty()) return nova_bool(false);
            uint64_t v = (uint64_t)a[0].asNumber();
            return nova_bool(v != 0 && std::has_single_bit(v)); }, "hasSingleBit"));

        b->obj->set("bitCeil", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     {
            if (a.empty()) return nova_num(1);
            uint64_t v = (uint64_t)a[0].asNumber();
            return nova_num(v == 0 ? 1.0 : (double)std::bit_ceil(v)); }, "bitCeil"));

        b->obj->set("bitFloor", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.empty()) return nova_num(0);
            return nova_num((double)std::bit_floor((uint64_t)a[0].asNumber())); }, "bitFloor"));

        b->obj->set("bitWidth", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.empty()) return nova_num(0);
            return nova_num((double)std::bit_width((uint64_t)a[0].asNumber())); }, "bitWidth"));

        // byteswap — manual since std::byteswap is C++23
        b->obj->set("byteswap", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.empty()) return nova_num(0);
            uint64_t v = (uint64_t)a[0].asNumber();
            v = ((v & 0xFF00000000000000ULL) >> 56) | ((v & 0x00FF000000000000ULL) >> 40) |
                ((v & 0x0000FF0000000000ULL) >> 24) | ((v & 0x000000FF00000000ULL) >>  8) |
                ((v & 0x00000000FF000000ULL) <<  8) | ((v & 0x0000000000FF0000ULL) << 24) |
                ((v & 0x000000000000FF00ULL) << 40) | ((v & 0x00000000000000FFULL) << 56);
            return nova_num((double)v); }, "byteswap"));

        // Bitset — fixed-size bit array (64-bit backed)
        b->obj->set("Bitset", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                    {
            uint64_t bits = a.empty() ? 0ULL : (uint64_t)a[0].asNumber();
            auto store = std::make_shared<uint64_t>(bits);
            auto self = nova_obj();

            self->obj->set("set", NovaValue::makeNative([store](ValVec a, auto) -> Val {
                if (a.empty()) { *store = ~0ULL; return nova_null(); }
                int i = (int)a[0].asNumber();
                bool val = a.size() > 1 ? a[1].asBool() : true;
                if (val) *store |=  (1ULL << i);
                else     *store &= ~(1ULL << i);
                return nova_null();
            }, "set"));

            self->obj->set("reset", NovaValue::makeNative([store](ValVec a, auto) -> Val {
                if (a.empty()) { *store = 0ULL; return nova_null(); }
                *store &= ~(1ULL << (int)a[0].asNumber());
                return nova_null();
            }, "reset"));

            self->obj->set("flip", NovaValue::makeNative([store](ValVec a, auto) -> Val {
                if (a.empty()) { *store = ~*store; return nova_null(); }
                *store ^= (1ULL << (int)a[0].asNumber());
                return nova_null();
            }, "flip"));

            self->obj->set("test", NovaValue::makeNative([store](ValVec a, auto) -> Val {
                if (a.empty()) return nova_bool(false);
                return nova_bool((*store >> (int)a[0].asNumber()) & 1ULL);
            }, "test"));

            self->obj->set("count", NovaValue::makeNative([store](ValVec, auto) -> Val {
                return nova_num((double)std::popcount(*store));
            }, "count"));

            self->obj->set("any", NovaValue::makeNative([store](ValVec, auto) -> Val {
                return nova_bool(*store != 0);
            }, "any"));

            self->obj->set("none", NovaValue::makeNative([store](ValVec, auto) -> Val {
                return nova_bool(*store == 0);
            }, "none"));

            self->obj->set("all", NovaValue::makeNative([store](ValVec, auto) -> Val {
                return nova_bool(*store == ~0ULL);
            }, "all"));

            self->obj->set("toNumber", NovaValue::makeNative([store](ValVec, auto) -> Val {
                return nova_num((double)*store);
            }, "toNumber"));

            self->obj->set("toString", NovaValue::makeNative([store](ValVec, auto) -> Val {
                std::string s;
                for (int i = 63; i >= 0; i--) s += ((*store >> i) & 1) ? '1' : '0';
                return nova_str(s);
            }, "toString"));

            return self; }, "Bitset"));

        obj->obj->set("Bit", b);
    }

    // ── <complex> ─────────────────────────────────────────────────────────────────

    void Executor::_registerComplex(Val obj)
    {
        using Cx = std::complex<double>;

        auto makeComplex = [](Cx c) -> Val
        {
            auto o = nova_obj();
            o->obj->set("re", nova_num(c.real()));
            o->obj->set("im", nova_num(c.imag()));
            o->obj->set("_kind", nova_str("complex"));
            return o;
        };

        auto getCx = [](Val v) -> Cx
        {
            if (!v || !v.isObject())
                return {v.asNumber(), 0};
            Val re = v->obj->get("re");
            Val im = v->obj->get("im");
            return {re ? re.asNumber() : 0.0, im ? im.asNumber() : 0.0};
        };

        auto cx = nova_obj();

        // Complex(re, im) → complex object
        cx->obj->set("new", NovaValue::makeNative([makeComplex](ValVec a, auto) -> Val
                                                  {
            double re = a.size() > 0 ? a[0].asNumber() : 0;
            double im = a.size() > 1 ? a[1].asNumber() : 0;
            return makeComplex({re, im}); }, "new"));

        // fromPolar(r, theta) → complex
        cx->obj->set("fromPolar", NovaValue::makeNative([makeComplex](ValVec a, auto) -> Val
                                                        {
            double r     = a.size() > 0 ? a[0].asNumber() : 0;
            double theta = a.size() > 1 ? a[1].asNumber() : 0;
            return makeComplex(std::polar(r, theta)); }, "fromPolar"));

#define CX_UNARY(name, fn) \
    cx->obj->set(#name, NovaValue::makeNative([makeComplex, getCx](ValVec a, auto) -> Val { \
            if (a.empty()) return nova_null(); \
            return makeComplex(fn(getCx(a[0]))); }, #name))

        CX_UNARY(abs, std::abs);
        CX_UNARY(arg, std::arg);
        CX_UNARY(norm, std::norm);
        CX_UNARY(conj, std::conj);
        CX_UNARY(proj, std::proj);
        CX_UNARY(exp, std::exp);
        CX_UNARY(log, std::log);
        CX_UNARY(log10, std::log10);
        CX_UNARY(sqrt, std::sqrt);
        CX_UNARY(sin, std::sin);
        CX_UNARY(cos, std::cos);
        CX_UNARY(tan, std::tan);
        CX_UNARY(asin, std::asin);
        CX_UNARY(acos, std::acos);
        CX_UNARY(atan, std::atan);
        CX_UNARY(sinh, std::sinh);
        CX_UNARY(cosh, std::cosh);
        CX_UNARY(tanh, std::tanh);
        CX_UNARY(asinh, std::asinh);
        CX_UNARY(acosh, std::acosh);
        CX_UNARY(atanh, std::atanh);
#undef CX_UNARY

        // abs returns a real number not complex
        cx->obj->set("abs", NovaValue::makeNative([getCx](ValVec a, auto) -> Val
                                                  {
            if (a.empty()) return nova_num(0);
            return nova_num(std::abs(getCx(a[0]))); }, "abs"));

        cx->obj->set("arg", NovaValue::makeNative([getCx](ValVec a, auto) -> Val
                                                  {
            if (a.empty()) return nova_num(0);
            return nova_num(std::arg(getCx(a[0]))); }, "arg"));

        cx->obj->set("norm", NovaValue::makeNative([getCx](ValVec a, auto) -> Val
                                                   {
            if (a.empty()) return nova_num(0);
            return nova_num(std::norm(getCx(a[0]))); }, "norm"));

        cx->obj->set("pow", NovaValue::makeNative([makeComplex, getCx](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_null();
            return makeComplex(std::pow(getCx(a[0]), getCx(a[1]))); }, "pow"));

        // arithmetic: add, sub, mul, div
        cx->obj->set("add", NovaValue::makeNative([makeComplex, getCx](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_null();
            return makeComplex(getCx(a[0]) + getCx(a[1])); }, "add"));
        cx->obj->set("sub", NovaValue::makeNative([makeComplex, getCx](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_null();
            return makeComplex(getCx(a[0]) - getCx(a[1])); }, "sub"));
        cx->obj->set("mul", NovaValue::makeNative([makeComplex, getCx](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_null();
            return makeComplex(getCx(a[0]) * getCx(a[1])); }, "mul"));
        cx->obj->set("div", NovaValue::makeNative([makeComplex, getCx](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_null();
            return makeComplex(getCx(a[0]) / getCx(a[1])); }, "div"));

        obj->obj->set("Complex", cx);
    }

    // ── <valarray> ────────────────────────────────────────────────────────────────

    void Executor::_registerValarray(Val obj)
    {
        auto va = nova_obj();

        auto getVA = [](Val v) -> std::valarray<double>
        {
            if (!v || !v.isArray())
                return {};
            std::valarray<double> out(v->arr->inner.size());
            for (size_t i = 0; i < v->arr->inner.size(); i++)
                out[i] = v->arr->inner[i].asNumber();
            return out;
        };

        auto fromVA = [](const std::valarray<double> &va) -> Val
        {
            ValVec out(va.size());
            for (size_t i = 0; i < va.size(); i++)
                out[i] = nova_num(va[i]);
            return nova_arr(std::move(out));
        };

#define VA_UNARY(name, fn) \
    va->obj->set(#name, NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val { \
            if (a.empty()) return nova_arr(); \
            return fromVA(fn(getVA(a[0]))); }, #name))

        VA_UNARY(abs, std::abs);
        VA_UNARY(exp, std::exp);
        VA_UNARY(log, std::log);
        VA_UNARY(log10, std::log10);
        VA_UNARY(sqrt, std::sqrt);
        VA_UNARY(sin, std::sin);
        VA_UNARY(cos, std::cos);
        VA_UNARY(tan, std::tan);
        VA_UNARY(asin, std::asin);
        VA_UNARY(acos, std::acos);
        VA_UNARY(atan, std::atan);
        VA_UNARY(sinh, std::sinh);
        VA_UNARY(cosh, std::cosh);
        VA_UNARY(tanh, std::tanh);
#undef VA_UNARY

        // element-wise arithmetic between two arrays
        va->obj->set("add", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_arr();
            return fromVA(getVA(a[0]) + getVA(a[1])); }, "add"));
        va->obj->set("sub", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_arr();
            return fromVA(getVA(a[0]) - getVA(a[1])); }, "sub"));
        va->obj->set("mul", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_arr();
            return fromVA(getVA(a[0]) * getVA(a[1])); }, "mul"));
        va->obj->set("div", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_arr();
            return fromVA(getVA(a[0]) / getVA(a[1])); }, "div"));

        // scalar ops
        va->obj->set("scale", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                    {
            if (a.size() < 2) return nova_arr();
            return fromVA(getVA(a[0]) * a[1].asNumber()); }, "scale"));
        va->obj->set("shift", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                    {
            if (a.size() < 2) return nova_arr();
            return fromVA(getVA(a[0]) + a[1].asNumber()); }, "shift"));

        // reductions
        va->obj->set("sum", NovaValue::makeNative([getVA](ValVec a, auto) -> Val
                                                  { return a.empty() ? nova_num(0) : nova_num(getVA(a[0]).sum()); }, "sum"));
        va->obj->set("min", NovaValue::makeNative([getVA](ValVec a, auto) -> Val
                                                  { return a.empty() ? nova_num(0) : nova_num(getVA(a[0]).min()); }, "min"));
        va->obj->set("max", NovaValue::makeNative([getVA](ValVec a, auto) -> Val
                                                  { return a.empty() ? nova_num(0) : nova_num(getVA(a[0]).max()); }, "max"));

        // slice(array, start, size, stride)
        va->obj->set("slice", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                    {
            if (a.size() < 4) return nova_arr();
            auto v = getVA(a[0]);
            size_t start  = (size_t)a[1].asNumber();
            size_t size   = (size_t)a[2].asNumber();
            size_t stride = (size_t)a[3].asNumber();
            std::valarray<double> sliced = v[std::slice(start, size, stride)];
            return fromVA(sliced); }, "slice"));

        // pow(array, exponent)
        va->obj->set("pow", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_arr();
            return fromVA(std::pow(getVA(a[0]), a[1].asNumber())); }, "pow"));

        // atan2(y, x) — element-wise
        va->obj->set("atan2", NovaValue::makeNative([getVA, fromVA](ValVec a, auto) -> Val
                                                    {
            if (a.size() < 2) return nova_arr();
            return fromVA(std::atan2(getVA(a[0]), getVA(a[1]))); }, "atan2"));

        obj->obj->set("Valarray", va);
    }

    // ── <charconv> ────────────────────────────────────────────────────────────────

    void Executor::_registerCharconv(Val obj)
    {
        auto cc = nova_obj();

        // fromChars(str, base?) → { value, ok }
        cc->obj->set("fromChars", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.empty()) return nova_null();
            std::string s = a[0].asString();
            int base = a.size() > 1 ? (int)a[1].asNumber() : 10;
            auto res = nova_obj();
            // try integer first
            long long ival = 0;
            auto [iptr, iec] = std::from_chars(s.data(), s.data() + s.size(), ival, base);
            if (iec == std::errc{} && iptr == s.data() + s.size()) {
                res->obj->set("value", nova_num((double)ival));
                res->obj->set("ok", nova_bool(true));
                return res;
            }
            // try float (base 10 only)
            double fval = 0;
            auto [fptr, fec] = portable_from_chars_double(s.data(), s.data() + s.size(), fval);
            if (fec == std::errc{}) {
                res->obj->set("value", nova_num(fval));
                res->obj->set("ok", nova_bool(true));
                return res;
            }
            res->obj->set("value", nova_null());
            res->obj->set("ok", nova_bool(false));
            return res; }, "fromChars"));

        // toChars(number, base?) → string
        cc->obj->set("toChars", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.empty()) return nova_str("");
            int base = a.size() > 1 ? (int)a[1].asNumber() : 10;
            char buf[64];
            long long v = (long long)a[0].asNumber();
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v, base);
            if (ec == std::errc{}) return nova_str(std::string(buf, ptr));
            return nova_str(""); }, "toChars"));

        // toCharsFloat(number, format?, precision?) → string
        // format: "fixed" | "scientific" | "hex" | "general" (default)
        cc->obj->set("toCharsFloat", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                           {
            if (a.empty()) return nova_str("");
            double v = a[0].asNumber();
            std::string fmt = a.size() > 1 ? a[1].asString() : "general";
            int prec = a.size() > 2 ? (int)a[2].asNumber() : 6;
            char buf[64];
            std::chars_format cf = std::chars_format::general;
            if      (fmt == "fixed")      cf = std::chars_format::fixed;
            else if (fmt == "scientific")  cf = std::chars_format::scientific;
            else if (fmt == "hex")         cf = std::chars_format::hex;
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v, cf, prec);
            if (ec == std::errc{}) return nova_str(std::string(buf, ptr));
            return nova_str(""); }, "toCharsFloat"));

        // parseInt(str, base?) → number | null  (fast, no locale)
        cc->obj->set("parseInt", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                       {
            if (a.empty()) return nova_null();
            std::string s = a[0].asString();
            int base = a.size() > 1 ? (int)a[1].asNumber() : 10;
            long long v = 0;
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
            return ec == std::errc{} ? nova_num((double)v) : nova_null(); }, "parseInt"));

        // parseFloat(str) → number | null  (fast, no locale)
        cc->obj->set("parseFloat", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                         {
            if (a.empty()) return nova_null();
            std::string s = a[0].asString();
            double v = 0;
            auto [ptr, ec] = portable_from_chars_double(s.data(), s.data() + s.size(), v);
            return ec == std::errc{} ? nova_num(v) : nova_null(); }, "parseFloat"));

        obj->obj->set("Charconv", cc);
    }

    void Executor::_registerMath(Val obj)
    {
        auto m = nova_obj();
#define MF1(name, fn) m->obj->set(#name, NovaValue::makeNative([](ValVec a, auto) { return a.empty() ? nova_num(0) : nova_num(fn(a[0].asNumber())); }, #name))
        MF1(abs, std::abs);
        MF1(ceil, std::ceil);
        MF1(floor, std::floor);
        MF1(round, std::round);
        MF1(sqrt, std::sqrt);
        MF1(cbrt, std::cbrt);
        MF1(log, std::log);
        MF1(log2, std::log2);
        MF1(log10, std::log10);
        MF1(sin, std::sin);
        MF1(cos, std::cos);
        MF1(tan, std::tan);
        MF1(asin, std::asin);
        MF1(acos, std::acos);
        MF1(atan, std::atan);
        MF1(trunc, std::trunc);
        MF1(exp, std::exp);
#undef MF1
        m->obj->set("pow", NovaValue::makeNative([](ValVec a, auto)
                                                 { return a.size() < 2 ? nova_num(0) : nova_num(std::pow(a[0].asNumber(), a[1].asNumber())); }, "pow"));
        m->obj->set("max", NovaValue::makeNative([](ValVec a, auto)
                                                 { double r=-1e300; for(auto&v:a) r=std::max(r,v.asNumber()); return nova_num(r); }, "max"));
        m->obj->set("min", NovaValue::makeNative([](ValVec a, auto)
                                                 { double r=1e300;  for(auto&v:a) r=std::min(r,v.asNumber()); return nova_num(r); }, "min"));
        m->obj->set("atan2", NovaValue::makeNative([](ValVec a, auto)
                                                   { return a.size() < 2 ? nova_num(0) : nova_num(std::atan2(a[0].asNumber(), a[1].asNumber())); }, "atan2"));
        m->obj->set("hypot", NovaValue::makeNative([](ValVec a, auto)
                                                   { double s=0; for(auto&v:a){double n=v.asNumber();s+=n*n;} return nova_num(std::sqrt(s)); }, "hypot"));
        m->obj->set("sign", NovaValue::makeNative([](ValVec a, auto)
                                                  { if(a.empty())return nova_num(0); double n=a[0].asNumber(); return nova_num(n>0?1:n<0?-1:0); }, "sign"));
        m->obj->set("random", NovaValue::makeNative([](ValVec, auto)
                                                    { return nova_num((double)std::rand() / RAND_MAX); }, "random"));
        m->obj->set("clamp", NovaValue::makeNative([](ValVec a, auto)
                                                   { if(a.size()<3)return a.empty()?nova_num(0):a[0]; return nova_num(std::max(a[1].asNumber(),std::min(a[0].asNumber(),a[2].asNumber()))); }, "clamp"));
        m->obj->set("PI", nova_num(M_PI));
        m->obj->set("E", nova_num(M_E));
        m->obj->set("LN2", nova_num(M_LN2));
        m->obj->set("LN10", nova_num(M_LN10));
        m->obj->set("SQRT2", nova_num(M_SQRT2));
        m->obj->set("TAU", nova_num(2 * M_PI));
        m->obj->set("NaN", nova_num(std::numeric_limits<double>::quiet_NaN()));
        m->obj->set("INFINITY", nova_num(std::numeric_limits<double>::infinity()));
        // ── extra <cmath> functions ──────────────────────────────────────────
#define MF1(name, fn) m->obj->set(#name, NovaValue::makeNative([](ValVec a, auto) { return a.empty() ? nova_num(0) : nova_num(fn(a[0].asNumber())); }, #name))
        MF1(sinh, std::sinh);
        MF1(cosh, std::cosh);
        MF1(tanh, std::tanh);
        MF1(asinh, std::asinh);
        MF1(acosh, std::acosh);
        MF1(atanh, std::atanh);
        MF1(exp2, std::exp2);
        MF1(expm1, std::expm1);
        MF1(log1p, std::log1p);
        MF1(erf, std::erf);
        MF1(erfc, std::erfc);
        MF1(lgamma, std::lgamma);
        MF1(tgamma, std::tgamma);
        MF1(nearbyint, std::nearbyint);
#undef MF1

        // ── <cmath> two-arg / misc ────────────────────────────────────────────
        m->obj->set("fma", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                 {
            if (a.size() < 3) return nova_num(0);
            return nova_num(std::fma(a[0].asNumber(), a[1].asNumber(), a[2].asNumber())); }, "fma"));
        m->obj->set("copysign", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                      {
            if (a.size() < 2) return nova_num(0);
            return nova_num(std::copysign(a[0].asNumber(), a[1].asNumber())); }, "copysign"));
        m->obj->set("remainder", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                       {
            if (a.size() < 2) return nova_num(0);
            return nova_num(std::remainder(a[0].asNumber(), a[1].asNumber())); }, "remainder"));
        m->obj->set("fdim", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                  {
            if (a.size() < 2) return nova_num(0);
            return nova_num(std::fdim(a[0].asNumber(), a[1].asNumber())); }, "fdim"));
        m->obj->set("isNaN", NovaValue::makeNative([](ValVec a, auto)
                                                   { return nova_bool(!a.empty() && std::isnan(a[0].asNumber())); }, "isNaN"));
        m->obj->set("isFinite", NovaValue::makeNative([](ValVec a, auto)
                                                      { return nova_bool(!a.empty() && std::isfinite(a[0].asNumber())); }, "isFinite"));
        m->obj->set("isInf", NovaValue::makeNative([](ValVec a, auto)
                                                   { return nova_bool(!a.empty() && std::isinf(a[0].asNumber())); }, "isInf"));

        // ── <numeric> algorithms ──────────────────────────────────────────────
        m->obj->set("gcd", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                 {
            if (a.size() < 2) return nova_num(0);
            return nova_num((double)std::gcd((long long)a[0].asNumber(), (long long)a[1].asNumber())); }, "gcd"));
        m->obj->set("lcm", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                 {
            if (a.size() < 2) return nova_num(0);
            return nova_num((double)std::lcm((long long)a[0].asNumber(), (long long)a[1].asNumber())); }, "lcm"));
        // iota(n, start=0, step=1) -> array of n values
        m->obj->set("iota", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                  {
            if (a.empty()) return nova_arr();
            int n = (int)a[0].asNumber();
            double start = a.size() > 1 ? a[1].asNumber() : 0.0;
            double step  = a.size() > 2 ? a[2].asNumber() : 1.0;
            ValVec out; out.reserve(n);
            for (int i = 0; i < n; i++) out.push_back(nova_num(start + i * step));
            return nova_arr(std::move(out)); }, "iota"));
        // accumulate(array, init=0) -> sum
        m->obj->set("accumulate", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.empty() || !a[0].isArray()) return nova_num(0);
            double acc = a.size() > 1 ? a[1].asNumber() : 0.0;
            for (auto& v : a[0]->arr->inner) acc += v.asNumber();
            return nova_num(acc); }, "accumulate"));
        // innerProduct(a, b, init=0)
        m->obj->set("innerProduct", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_num(0);
            double acc = a.size() > 2 ? a[2].asNumber() : 0.0;
            auto& l = a[0]->arr->inner; auto& r = a[1]->arr->inner;
            size_t n = std::min(l.size(), r.size());
            for (size_t i = 0; i < n; i++) acc += l[i].asNumber() * r[i].asNumber();
            return nova_num(acc); }, "innerProduct"));
        // partialSum(array) -> running totals array
        m->obj->set("partialSum", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                        {
            if (a.empty() || !a[0].isArray()) return nova_arr();
            ValVec out; double acc = 0;
            for (auto& v : a[0]->arr->inner) { acc += v.asNumber(); out.push_back(nova_num(acc)); }
            return nova_arr(std::move(out)); }, "partialSum"));
        // adjacentDiff(array) -> [a[0], a[1]-a[0], a[2]-a[1], ...]
        m->obj->set("adjacentDiff", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                          {
            if (a.empty() || !a[0].isArray() || a[0]->arr->inner.empty()) return nova_arr();
            auto& inner = a[0]->arr->inner;
            ValVec out; out.push_back(inner[0]);
            for (size_t i = 1; i < inner.size(); i++)
                out.push_back(nova_num(inner[i].asNumber() - inner[i-1].asNumber()));
            return nova_arr(std::move(out)); }, "adjacentDiff"));

        // ── <numbers> constants (C++20) ───────────────────────────────────────
        m->obj->set("PHI", nova_num(std::numbers::phi));       // golden ratio
        m->obj->set("EGAMMA", nova_num(std::numbers::egamma)); // Euler-Mascheroni
        m->obj->set("LOG2E", nova_num(std::numbers::log2e));
        m->obj->set("LOG10E", nova_num(std::numbers::log10e));
        m->obj->set("INV_PI", nova_num(std::numbers::inv_pi));
        m->obj->set("INV_SQRTPI", nova_num(std::numbers::inv_sqrtpi));
        m->obj->set("SQRT3", nova_num(std::numbers::sqrt3));
        m->obj->set("INV_SQRT3", nova_num(std::numbers::inv_sqrt3));

        // ── <ratio> SI prefixes ───────────────────────────────────────────────
        auto ratio = nova_obj();
        ratio->obj->set("atto", nova_num(1e-18));
        ratio->obj->set("femto", nova_num(1e-15));
        ratio->obj->set("pico", nova_num(1e-12));
        ratio->obj->set("nano", nova_num(1e-9));
        ratio->obj->set("micro", nova_num(1e-6));
        ratio->obj->set("milli", nova_num(1e-3));
        ratio->obj->set("centi", nova_num(1e-2));
        ratio->obj->set("deci", nova_num(1e-1));
        ratio->obj->set("deca", nova_num(1e1));
        ratio->obj->set("hecto", nova_num(1e2));
        ratio->obj->set("kilo", nova_num(1e3));
        ratio->obj->set("mega", nova_num(1e6));
        ratio->obj->set("giga", nova_num(1e9));
        ratio->obj->set("tera", nova_num(1e12));
        ratio->obj->set("peta", nova_num(1e15));
        ratio->obj->set("exa", nova_num(1e18));
        m->obj->set("Ratio", ratio);
        obj->obj->set("Math", m);
    }

    // ── JSON ──────────────────────────────────────────────────────────────────────
    void Executor::_registerJSON(Val obj)
    {
        auto j = nova_obj();
        j->obj->set("stringify", NovaValue::makeNative([this](ValVec a, auto) -> Val
                                                       {
        if (a.empty()) return nova_str("null");
        int indent = a.size() > 1 ? (int)a[1].asNumber() : 0;
        return nova_str(_stringify(a[0], indent, 0)); }, "stringify"));
        j->obj->set("parse", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                   {
        if (a.empty()) return nova_null();
        return nova_json::parse(a[0].asString()); }, "parse"));
        obj->obj->set("JSON", j);
    }

    void Executor::_registerFiber(Val obj)
    {
        auto exe = this;

        // Std.Fiber(bodyFn) -> fiber object
        obj->obj->set("Fiber", NovaValue::makeNative([exe](ValVec args, std::shared_ptr<Scope> cs) -> Val
                                                     {
        auto fiber = std::make_shared<Fiber>();

        auto fiberObj = nova_obj();

        // ── body ──────────────────────────────────────────────────────────────
        // Can be set at construction time or via .setBody() later
        if (!args.empty() && args[0] && args[0].isFunction()) {
            Val bodyFn = args[0];
            fiber->body = [exe, bodyFn, cs](Fiber& f) {
                // Inject suspend as a callable inside the fiber body
                auto fiberRef = &f;
                cs->setOwn("__fiber_suspend__", NovaValue::makeNative(
                    [fiberRef](ValVec a, auto) -> Val {
                        fiberRef->yieldedValue = a.empty() ? nova_null() : a[0];
                        fiberRef->suspend();
                        return fiberRef->sentValue ? fiberRef->sentValue : nova_null();
                    }, "__fiber_suspend__"));

                Val result = nova_null();
                try {
                    result = exe->callFunction(bodyFn, {}, cs);
                } catch (...) {
                    f.thrownException = std::current_exception();
                }
                f.yieldedValue = result;
            };
            fiberInit(*fiber);
        }

        // ── .setBody(fn) — set/replace body before first resume ───────────────
        fiberObj->obj->set("setBody", NovaValue::makeNative([exe, fiber, cs](ValVec a, auto) -> Val {
            if (a.empty() || !a[0].isFunction()) return nova_null();
            if (fiber->started)
                exe->_error("Cannot setBody on a started fiber");
            Val bodyFn = a[0];
            fiber->body = [exe, bodyFn, cs](Fiber& f) {
                Val result = nova_null();
                try {
                    result = exe->callFunction(bodyFn, {}, cs);
                } catch (...) {
                    f.thrownException = std::current_exception();
                }
                f.yieldedValue = result;
            };
            fiberInit(*fiber);
            return nova_null();
        }, "setBody"));

        // ── .resume(sentVal?) — resume the fiber, returns yielded value ───────
        fiberObj->obj->set("resume", NovaValue::makeNative([fiber](ValVec a, auto) -> Val {
            if (fiber->done)
                return nova_null();
            fiber->sentValue = a.empty() ? nova_null() : a[0];
            try {
                fiber->resume();
            } catch (ThrowSignal& e) {
                throw;
            } catch (std::exception& e) {
                throw ThrowSignal{nova_str(e.what())};
            }
            return fiber->yieldedValue ? fiber->yieldedValue : nova_null();
        }, "resume"));

        // ── .send(val) — alias for resume(val), matches generator .next(val) ──
        fiberObj->obj->set("send", NovaValue::makeNative([fiber](ValVec a, auto) -> Val {
            if (fiber->done) return nova_null();
            fiber->sentValue = a.empty() ? nova_null() : a[0];
            fiber->resume();
            return fiber->yieldedValue ? fiber->yieldedValue : nova_null();
        }, "send"));

        // ── .throw(val) — inject an exception into the fiber ─────────────────
        fiberObj->obj->set("throw", NovaValue::makeNative([fiber](ValVec a, auto) -> Val {
            if (fiber->done) return nova_null();
            Val errVal = a.empty() ? nova_str("FiberError") : a[0];
            fiber->thrownException = std::make_exception_ptr(ThrowSignal{errVal});
            fiber->resume();
            return fiber->yieldedValue ? fiber->yieldedValue : nova_null();
        }, "throw"));

        // ── .yieldedValue — last value the fiber suspended with ───────────────
        fiberObj->obj->set("yieldedValue", NovaValue::makeNative([fiber](ValVec, auto) -> Val {
            return fiber->yieldedValue ? fiber->yieldedValue : nova_null();
        }, "yieldedValue"));

        // ── .sentValue — last value sent INTO the fiber via resume/send ───────
        fiberObj->obj->set("sentValue", NovaValue::makeNative([fiber](ValVec, auto) -> Val {
            return fiber->sentValue ? fiber->sentValue : nova_null();
        }, "sentValue"));

        // ── .setSentValue(val) — write sentValue before manual resume ─────────
        fiberObj->obj->set("setSentValue", NovaValue::makeNative([fiber](ValVec a, auto) -> Val {
            fiber->sentValue = a.empty() ? nova_null() : a[0];
            return nova_null();
        }, "setSentValue"));

        // ── .done — is the fiber finished? ───────────────────────────────────
        fiberObj->obj->set("isDone", NovaValue::makeNative([fiber](ValVec, auto) -> Val {
            return nova_bool(fiber->done);
        }, "isDone"));

        // ── .started — has resume() been called at least once? ───────────────
        fiberObj->obj->set("isStarted", NovaValue::makeNative([fiber](ValVec, auto) -> Val {
            return nova_bool(fiber->started);
        }, "isStarted"));

        // ── .reset(newBodyFn?) — kill fiber, reinit with optional new body ────
        // NOTE: only safe when fiber is done or not yet started
        fiberObj->obj->set("reset", NovaValue::makeNative([exe, fiber, fiberObj, cs](ValVec a, auto) -> Val {
            if (!fiber->done && fiber->started)
                exe->_error("Cannot reset a running fiber — resume to completion first");

            fiber->done    = false;
            fiber->started = false;
            fiber->yieldedValue = nova_null();
            fiber->sentValue    = nova_null();
            fiber->thrownException = nullptr;

#if defined(NOVA_FIBER_WINDOWS)
            if (fiber->handle) { DeleteFiber(fiber->handle); fiber->handle = nullptr; }
            fiber->callerHandle = nullptr;
#else
                // stack stays allocated — just re-init context
#endif
            if (!a.empty() && a[0] && a[0].isFunction()) {
                Val bodyFn = a[0];
                fiber->body = [exe, bodyFn, cs](Fiber& f) {
                    Val result = nova_null();
                    try {
                        result = exe->callFunction(bodyFn, {}, cs);
                    } catch (...) {
                        f.thrownException = std::current_exception();
                    }
                    f.yieldedValue = result;
                };
            }

            if (fiber->body) fiberInit(*fiber);
            return nova_null();
        }, "reset"));

        // ── .driveToCompletion() — run to done, collect all yielded values ────
        fiberObj->obj->set("driveToCompletion", NovaValue::makeNative([fiber](ValVec a, auto) -> Val {
            Val seed = a.empty() ? nova_null() : a[0];
            ValVec yielded;
            fiber->sentValue = seed;
            while (!fiber->done) {
                fiber->resume();
                if (fiber->yieldedValue)
                    yielded.push_back(fiber->yieldedValue);
            }
            return nova_arr(std::move(yielded));
        }, "driveToCompletion"));

        // ── .stackSize — always 1MB per fiber.h constant ─────────────────────
        fiberObj->obj->set("stackSize", nova_num((double)Fiber::STACK_SIZE));

        return fiberObj; }, "Fiber"));
    }

    // ── process ───────────────────────────────────────────────────────────────────

    void Executor::_registerProcess(Val obj)
    {
        auto p = nova_obj();
        p->obj->set("exit", NovaValue::makeNative([](ValVec a, auto)
                                                  { std::exit(a.empty()?0:(int)a[0].asNumber()); return nova_null(); }, "exit"));
        p->obj->set("cwd", NovaValue::makeNative([](ValVec, auto)
                                                 { return nova_str(fs::current_path().string()); }, "cwd"));
        p->obj->set("argv", nova_arr());
        p->obj->set("platform", nova_str(
#if defined(_WIN32)
                                    "win32"
#elif defined(__APPLE__)
                                    "darwin"
#else
                                    "linux"
#endif
                                    ));
        auto env = nova_obj();
        if (const char *path = std::getenv("PATH"))
            env->obj->set("PATH", nova_str(path));
        if (const char *home = std::getenv("HOME"))
            env->obj->set("HOME", nova_str(home));
        p->obj->set("env", env);
        obj->obj->set("process", p);
    }

    // ── I/O ───────────────────────────────────────────────────────────────────────

    void Executor::_registerIO(Val stdobj)
    {
        auto obj = nova_obj();
        auto fn_read = NovaValue::makeNative([](ValVec a, auto) -> Val
                                             {
            if (!a.empty()) std::cout << a[0].asString();
            std::string line; std::getline(std::cin, line);
            return nova_str(line); }, "read");
        obj->obj->set("read", fn_read);
        obj->obj->set("write", NovaValue::makeNative([this](ValVec a, auto) -> Val
                                                     {
            for (auto& v : a) std::cout << stringify(v);
            return nova_null(); }, "write"));
        stdobj->obj->set("stdio", obj);
    }

    // ── timers (sync stubs kept + async additions folded in directly) ─────────────

    void Executor::_registerTimers(Val obj)
    {
        // sync, unchanged
        obj->obj->set("sleep", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     {
        if (a.empty()) return nova_null();
        int ms = (int)a[0].asNumber();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return nova_null(); }, "sleep"));
        obj->obj->set("time", NovaValue::makeNative([](ValVec, auto) -> Val
                                                    {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        return nova_num((double)std::chrono::duration_cast<std::chrono::milliseconds>(now).count()); }, "time"));

        // ── Std.Timers.{after,every,soon,cancel} — deferred/repeating callbacks ───
        // One detached std::thread per timer; sleeps, then (if not cancelled)
        // calls back via callFunction on the scope captured at registration time.
        auto exe = this;
        auto timersNs = nova_obj();

        auto makeHandle = [](std::shared_ptr<std::atomic<bool>> cancelled) -> Val
        {
            auto h = nova_obj();
            h->obj->set("__type__", nova_str("timer_handle"));
            h->obj->set("cancel", NovaValue::makeNative([cancelled](ValVec, auto) -> Val
                                                        { cancelled->store(true); return nova_null(); }, "cancel"));
            h->obj->set("cancelled", NovaValue::makeNative([cancelled](ValVec, auto) -> Val
                                                           { return nova_bool(cancelled->load()); }, "cancelled"));
            return h;
        };

        timersNs->obj->set("after", NovaValue::makeNative([exe, makeHandle](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                          {
        if (a.empty() || !a[0].isFunction())
            exe->_error("Timers.after: expected a function as the first argument");
        Val fn = a[0];
        long long ms = a.size() > 1 ? (long long)a[1].asNumber() : 0;
        if (ms < 0) ms = 0;

        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        std::thread([exe, fn, cs, ms, cancelled]()
                    {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            if (cancelled->load()) return;
            try { exe->callFunction(fn, {}, cs); } catch (...) {} })
            .detach();

        return makeHandle(cancelled); }, "after"));

        timersNs->obj->set("every", NovaValue::makeNative([exe, makeHandle](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                          {
        if (a.empty() || !a[0].isFunction())
            exe->_error("Timers.every: expected a function as the first argument");
        Val fn = a[0];
        long long ms = a.size() > 1 ? (long long)a[1].asNumber() : 0;
        if (ms < 1) ms = 1; // guard against a 0/negative-interval busy loop

        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        std::thread([exe, fn, cs, ms, cancelled]()
                    {
            while (!cancelled->load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                if (cancelled->load()) break;
                try { exe->callFunction(fn, {}, cs); } catch (...) {}
            } })
            .detach();

        return makeHandle(cancelled); }, "every"));

        timersNs->obj->set("soon", NovaValue::makeNative([exe, makeHandle](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                         {
        if (a.empty() || !a[0].isFunction())
            exe->_error("Timers.soon: expected a function as the first argument");
        Val fn = a[0];

        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        std::thread([exe, fn, cs, cancelled]()
                    {
            if (cancelled->load()) return;
            try { exe->callFunction(fn, {}, cs); } catch (...) {} })
            .detach();

        return makeHandle(cancelled); }, "soon"));

        timersNs->obj->set("cancel", NovaValue::makeNative([exe](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                           {
        if (a.empty() || !a[0] || !a[0].isObject())
            return nova_bool(false);
        Val cancelFn = a[0]->obj->get("cancel");
        if (!cancelFn || !cancelFn.isFunction())
            return nova_bool(false);
        exe->callFunction(cancelFn, {}, cs);
        return nova_bool(true); }, "cancel"));

        obj->obj->set("Timers", timersNs);
    }
    void Executor::_registerRandom(Val obj)
    {
        auto r = nova_obj();

        // ── shared RNG state ──────────────────────────────────────────────────────
        auto rng = std::make_shared<std::mt19937_64>(std::random_device{}());

        // ── seeding ───────────────────────────────────────────────────────────────
        r->obj->set("seed", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                  {
        if (a.empty()) *rng = std::mt19937_64(std::random_device{}());
        else           *rng = std::mt19937_64((uint64_t)a[0].asNumber());
        return nova_null(); }, "seed"));

        r->obj->set("randomDevice", NovaValue::makeNative([](ValVec, auto) -> Val
                                                          { return nova_num((double)std::random_device{}()); }, "randomDevice"));

        // ── raw engine output ─────────────────────────────────────────────────────
        r->obj->set("raw", NovaValue::makeNative([rng](ValVec, auto) -> Val
                                                 { return nova_num((double)(*rng)()); }, "raw"));

        r->obj->set("min", NovaValue::makeNative([](ValVec, auto) -> Val
                                                 { return nova_num((double)std::mt19937_64::min()); }, "min"));

        r->obj->set("max", NovaValue::makeNative([](ValVec, auto) -> Val
                                                 { return nova_num((double)std::mt19937_64::max()); }, "max"));

        // ── uniform distributions ─────────────────────────────────────────────────
        // int(lo, hi) — inclusive both ends
        r->obj->set("int", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                 {
        int lo = a.size() > 0 ? (int)a[0].asNumber() : 0;
        int hi = a.size() > 1 ? (int)a[1].asNumber() : 1;
        return nova_num((double)std::uniform_int_distribution<long long>(lo, hi)(*rng)); }, "int"));

        // float(lo=0.0, hi=1.0) — [lo, hi)
        r->obj->set("float", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                   {
        double lo = a.size() > 0 ? a[0].asNumber() : 0.0;
        double hi = a.size() > 1 ? a[1].asNumber() : 1.0;
        return nova_num(std::uniform_real_distribution<double>(lo, hi)(*rng)); }, "float"));

        // ── bernoulli ─────────────────────────────────────────────────────────────
        // bool(p=0.5) — true with probability p
        r->obj->set("bool", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                  {
        double p = a.empty() ? 0.5 : a[0].asNumber();
        return nova_bool(std::bernoulli_distribution(p)(*rng)); }, "bool"));

        // ── binomial family ───────────────────────────────────────────────────────
        // binomial(n, p) — number of successes in n trials
        r->obj->set("binomial", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                      {
        int    n = a.size() > 0 ? (int)a[0].asNumber() : 1;
        double p = a.size() > 1 ? a[1].asNumber()      : 0.5;
        return nova_num((double)std::binomial_distribution<int>(n, p)(*rng)); }, "binomial"));

        // negative binomial(k, p) — trials until k successes
        r->obj->set("negativeBinomial", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                              {
        int    k = a.size() > 0 ? (int)a[0].asNumber() : 1;
        double p = a.size() > 1 ? a[1].asNumber()      : 0.5;
        return nova_num((double)std::negative_binomial_distribution<int>(k, p)(*rng)); }, "negativeBinomial"));

        // geometric(p) — trials until first success
        r->obj->set("geometric", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                       {
        double p = a.empty() ? 0.5 : a[0].asNumber();
        return nova_num((double)std::geometric_distribution<int>(p)(*rng)); }, "geometric"));

        // ── poisson family ────────────────────────────────────────────────────────
        // poisson(mean)
        r->obj->set("poisson", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                     {
        double mean = a.empty() ? 1.0 : a[0].asNumber();
        return nova_num((double)std::poisson_distribution<int>(mean)(*rng)); }, "poisson"));

        // exponential(lambda) — time between poisson events
        r->obj->set("exponential", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                         {
        double lambda = a.empty() ? 1.0 : a[0].asNumber();
        return nova_num(std::exponential_distribution<double>(lambda)(*rng)); }, "exponential"));

        // gamma(alpha, beta)
        r->obj->set("gamma", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                   {
        double alpha = a.size() > 0 ? a[0].asNumber() : 1.0;
        double beta  = a.size() > 1 ? a[1].asNumber() : 1.0;
        return nova_num(std::gamma_distribution<double>(alpha, beta)(*rng)); }, "gamma"));

        // weibull(a, b)
        r->obj->set("weibull", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                     {
        double pa = a.size() > 0 ? a[0].asNumber() : 1.0;
        double pb = a.size() > 1 ? a[1].asNumber() : 1.0;
        return nova_num(std::weibull_distribution<double>(pa, pb)(*rng)); }, "weibull"));

        // extremeValue(a, b) — Gumbel distribution
        r->obj->set("extremeValue", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                          {
        double pa = a.size() > 0 ? a[0].asNumber() : 0.0;
        double pb = a.size() > 1 ? a[1].asNumber() : 1.0;
        return nova_num(std::extreme_value_distribution<double>(pa, pb)(*rng)); }, "extremeValue"));

        // ── normal family ─────────────────────────────────────────────────────────
        // normal(mean=0, stddev=1)
        r->obj->set("normal", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                    {
        double mean   = a.size() > 0 ? a[0].asNumber() : 0.0;
        double stddev = a.size() > 1 ? a[1].asNumber() : 1.0;
        return nova_num(std::normal_distribution<double>(mean, stddev)(*rng)); }, "normal"));

        // lognormal(mean, stddev)
        r->obj->set("lognormal", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                       {
        double mean   = a.size() > 0 ? a[0].asNumber() : 0.0;
        double stddev = a.size() > 1 ? a[1].asNumber() : 1.0;
        return nova_num(std::lognormal_distribution<double>(mean, stddev)(*rng)); }, "lognormal"));

        // chiSquared(n) — sum of squares of n standard normals
        r->obj->set("chiSquared", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                        {
        double n = a.empty() ? 1.0 : a[0].asNumber();
        return nova_num(std::chi_squared_distribution<double>(n)(*rng)); }, "chiSquared"));

        // cauchy(location, scale)
        r->obj->set("cauchy", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                    {
        double loc   = a.size() > 0 ? a[0].asNumber() : 0.0;
        double scale = a.size() > 1 ? a[1].asNumber() : 1.0;
        return nova_num(std::cauchy_distribution<double>(loc, scale)(*rng)); }, "cauchy"));

        // fisherF(m, n)
        r->obj->set("fisherF", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                     {
        double m = a.size() > 0 ? a[0].asNumber() : 1.0;
        double n = a.size() > 1 ? a[1].asNumber() : 1.0;
        return nova_num(std::fisher_f_distribution<double>(m, n)(*rng)); }, "fisherF"));

        // studentT(n) — t-distribution
        r->obj->set("studentT", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                      {
        double n = a.empty() ? 1.0 : a[0].asNumber();
        return nova_num(std::student_t_distribution<double>(n)(*rng)); }, "studentT"));

        // ── sampling / discrete ───────────────────────────────────────────────────
        // discrete(weights[]) — pick index by weight
        r->obj->set("discrete", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                      {
        if (a.empty() || !a[0].isArray()) return nova_num(0);
        std::vector<double> weights;
        for (auto& v : a[0]->arr->inner) weights.push_back(v.asNumber());
        return nova_num((double)std::discrete_distribution<int>(weights.begin(), weights.end())(*rng)); }, "discrete"));

        // piecewiseConstant(intervals[], weights[]) — uniform within weighted intervals
        r->obj->set("piecewiseConstant", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                               {
        if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_num(0);
        std::vector<double> intervals, weights;
        for (auto& v : a[0]->arr->inner) intervals.push_back(v.asNumber());
        for (auto& v : a[1]->arr->inner) weights.push_back(v.asNumber());
        return nova_num(std::piecewise_constant_distribution<double>(
            intervals.begin(), intervals.end(), weights.begin())(*rng)); }, "piecewiseConstant"));

        // piecewiseLinear(intervals[], weights[]) — linearly interpolated between weights
        r->obj->set("piecewiseLinear", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                             {
        if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_num(0);
        std::vector<double> intervals, weights;
        for (auto& v : a[0]->arr->inner) intervals.push_back(v.asNumber());
        for (auto& v : a[1]->arr->inner) weights.push_back(v.asNumber());
        return nova_num(std::piecewise_linear_distribution<double>(
            intervals.begin(), intervals.end(), weights.begin())(*rng)); }, "piecewiseLinear"));

        // ── array utilities ───────────────────────────────────────────────────────
        // shuffle(array) -> shuffled copy
        r->obj->set("shuffle", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                     {
        if (a.empty() || !a[0].isArray()) return nova_arr();
        ValVec out = a[0]->arr->inner;
        std::shuffle(out.begin(), out.end(), *rng);
        return nova_arr(std::move(out)); }, "shuffle"));

        // sample(array, n) -> n random elements without replacement
        r->obj->set("sample", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                    {
        if (a.size() < 2 || !a[0].isArray()) return nova_arr();
        auto& src = a[0]->arr->inner;
        int n = std::min((int)a[1].asNumber(), (int)src.size());
        ValVec out(n);
        std::sample(src.begin(), src.end(), out.begin(), n, *rng);
        return nova_arr(std::move(out)); }, "sample"));

        // choice(array) -> single random element
        r->obj->set("choice", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                    {
        if (a.empty() || !a[0].isArray() || a[0]->arr->inner.empty()) return nova_null();
        std::uniform_int_distribution<size_t> dist(0, a[0]->arr->inner.size() - 1);
        return a[0]->arr->inner[dist(*rng)]; }, "choice"));

        // weightedChoice(array, weights[]) -> single element by weight
        r->obj->set("weightedChoice", NovaValue::makeNative([rng](ValVec a, auto) -> Val
                                                            {
        if (a.size() < 2 || !a[0].isArray() || !a[1].isArray()) return nova_null();
        auto& items = a[0]->arr->inner;
        std::vector<double> weights;
        for (auto& v : a[1]->arr->inner) weights.push_back(v.asNumber());
        int idx = std::discrete_distribution<int>(weights.begin(), weights.end())(*rng);
        return idx < (int)items.size() ? items[idx] : nova_null(); }, "weightedChoice"));

        // ── engine factory — lets Nova code hold independent RNG states ───────────
        r->obj->set("createRng", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                       {
        uint64_t seed = a.empty()
            ? (uint64_t)std::random_device{}()
            : (uint64_t)a[0].asNumber();
        auto eng = std::make_shared<std::mt19937_64>(seed);
        auto obj = nova_obj();

        obj->obj->set("seed", NovaValue::makeNative([eng](ValVec a, auto) -> Val {
            *eng = std::mt19937_64(a.empty() ? (uint64_t)std::random_device{}() : (uint64_t)a[0].asNumber());
            return nova_null();
        }, "seed"));
        obj->obj->set("raw",  NovaValue::makeNative([eng](ValVec, auto) -> Val { return nova_num((double)(*eng)()); }, "raw"));
        obj->obj->set("int",  NovaValue::makeNative([eng](ValVec a, auto) -> Val {
            long long lo = a.size()>0?(long long)a[0].asNumber():0;
            long long hi = a.size()>1?(long long)a[1].asNumber():1;
            return nova_num((double)std::uniform_int_distribution<long long>(lo,hi)(*eng));
        }, "int"));
        obj->obj->set("float", NovaValue::makeNative([eng](ValVec a, auto) -> Val {
            double lo = a.size()>0?a[0].asNumber():0.0;
            double hi = a.size()>1?a[1].asNumber():1.0;
            return nova_num(std::uniform_real_distribution<double>(lo,hi)(*eng));
        }, "float"));
        obj->obj->set("normal", NovaValue::makeNative([eng](ValVec a, auto) -> Val {
            double mean=a.size()>0?a[0].asNumber():0.0, sd=a.size()>1?a[1].asNumber():1.0;
            return nova_num(std::normal_distribution<double>(mean,sd)(*eng));
        }, "normal"));
        obj->obj->set("bool", NovaValue::makeNative([eng](ValVec a, auto) -> Val {
            return nova_bool(std::bernoulli_distribution(a.empty()?0.5:a[0].asNumber())(*eng));
        }, "bool"));
        obj->obj->set("shuffle", NovaValue::makeNative([eng](ValVec a, auto) -> Val {
            if (a.empty()||!a[0].isArray()) return nova_arr();
            ValVec out=a[0]->arr->inner;
            std::shuffle(out.begin(),out.end(),*eng);
            return nova_arr(std::move(out));
        }, "shuffle"));
        obj->obj->set("choice", NovaValue::makeNative([eng](ValVec a, auto) -> Val {
            if (a.empty()||!a[0].isArray()||a[0]->arr->inner.empty()) return nova_null();
            std::uniform_int_distribution<size_t> d(0, a[0]->arr->inner.size()-1);
            return a[0]->arr->inner[d(*eng)];
        }, "choice"));
        return obj; }, "createRng"));

        obj->obj->set("Random", r);
    }
    void Executor::_registerChrono(Val obj)
    {
        auto c = nova_obj();

        // ── clocks ────────────────────────────────────────────────────────────────
        c->obj->set("now", NovaValue::makeNative([](ValVec, auto) -> Val
                                                 {
        auto t = std::chrono::system_clock::now().time_since_epoch();
        return nova_num((double)std::chrono::duration_cast<std::chrono::milliseconds>(t).count()); }, "now"));
        c->obj->set("nowSteady", NovaValue::makeNative([](ValVec, auto) -> Val
                                                       {
        auto t = std::chrono::steady_clock::now().time_since_epoch();
        return nova_num((double)std::chrono::duration_cast<std::chrono::nanoseconds>(t).count()); }, "nowSteady"));
        c->obj->set("nowHR", NovaValue::makeNative([](ValVec, auto) -> Val
                                                   {
        auto t = std::chrono::high_resolution_clock::now().time_since_epoch();
        return nova_num((double)std::chrono::duration_cast<std::chrono::nanoseconds>(t).count()); }, "nowHR"));

        // ── measure(fn) → elapsed ms (f64) ───────────────────────────────────────
        c->obj->set("measure", NovaValue::makeNative([this](ValVec a, auto cs) -> Val
                                                     {
        if (a.empty() || !a[0].isFunction()) return nova_num(0);
        auto start = std::chrono::high_resolution_clock::now();
        callFunction(a[0], {}, cs);
        auto end = std::chrono::high_resolution_clock::now();
        return nova_num((double)std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0); }, "measure"));

        // ── sleep with explicit units (sync, unchanged) ──────────────────────────
        c->obj->set("sleepMs", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     { std::this_thread::sleep_for(std::chrono::milliseconds (a.empty()?0:(long long)a[0].asNumber())); return nova_null(); }, "sleepMs"));
        c->obj->set("sleepSecs", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                       { std::this_thread::sleep_for(std::chrono::seconds      (a.empty()?0:(long long)a[0].asNumber())); return nova_null(); }, "sleepSecs"));
        c->obj->set("sleepUs", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     { std::this_thread::sleep_for(std::chrono::microseconds  (a.empty()?0:(long long)a[0].asNumber())); return nova_null(); }, "sleepUs"));
        c->obj->set("sleepNs", NovaValue::makeNative([](ValVec a, auto) -> Val
                                                     { std::this_thread::sleep_for(std::chrono::nanoseconds   (a.empty()?0:(long long)a[0].asNumber())); return nova_null(); }, "sleepNs"));

        // ── unit converters: toMs(value, fromUnit) ────────────────────────────────
        auto makeConv = [](double toFactor)
        {
            return NovaValue::makeNative([toFactor](ValVec a, auto) -> Val
                                         {
            if (a.empty()) return nova_num(0);
            double v = a[0].asNumber();
            if (a.size() > 1) {
                std::string u = a[1].asString();
                double fromFactor = 1.0;
                if      (u == "ns")  fromFactor = 1e-6;
                else if (u == "us")  fromFactor = 1e-3;
                else if (u == "ms")  fromFactor = 1.0;
                else if (u == "s")   fromFactor = 1e3;
                else if (u == "min") fromFactor = 60e3;
                else if (u == "h")   fromFactor = 3600e3;
                v = v * fromFactor;
            }
            return nova_num(v / toFactor); }, "conv");
        };
        c->obj->set("toNs", makeConv(1e-6));
        c->obj->set("toUs", makeConv(1e-3));
        c->obj->set("toMs", makeConv(1.0));
        c->obj->set("toSecs", makeConv(1e3));
        c->obj->set("toMins", makeConv(60e3));
        c->obj->set("toHours", makeConv(3600e3));

        // ── duration constants (in ms, for arithmetic) ────────────────────────────
        c->obj->set("NS", nova_num(1e-6));
        c->obj->set("US", nova_num(1e-3));
        c->obj->set("MS", nova_num(1.0));
        c->obj->set("SEC", nova_num(1000.0));
        c->obj->set("MIN", nova_num(60000.0));
        c->obj->set("HOUR", nova_num(3600000.0));
        c->obj->set("DAY", nova_num(86400000.0));

        // ── Chrono.asleep(ms) — async, awaitable ───────────────────────────────────
        // Settled by a background thread after `ms` elapses. Deliberately does
        // NOT use a Fiber as the synchronization primitive: a fiber's suspend()
        // doesn't block on anything by itself — resume() just continues from
        // wherever it last suspended, on whichever thread calls it, whenever
        // that happens. The old implementation let the awaiting thread call
        // __drive__ -> fiber->resume() immediately, which ran the fiber's body
        // straight past its suspend point with nothing to wait on — so the
        // sleep was skipped outright. It also raced the timer thread, which
        // would *also* call fiber->resume() later, risking two threads
        // resuming the same fiber concurrently (undefined behavior for the
        // underlying stack-switch primitive). A mutex + condition_variable,
        // settled exclusively by the timer thread, fixes both: __drive__ now
        // genuinely blocks until the timer fires, and only one thread ever
        // touches the settle state.
        {
            auto exe = this;
            c->obj->set("asleep", NovaValue::makeNative([exe](ValVec a, std::shared_ptr<Scope> cs) -> Val
                                                        {
            long long ms = a.empty() ? 0 : (long long)a[0].asNumber();
            if (ms < 0) ms = 0;

            auto done   = std::make_shared<std::atomic<bool>>(false);
            auto mu     = std::make_shared<std::mutex>();
            auto cv     = std::make_shared<std::condition_variable>();

            auto taskObj = nova_obj();
            taskObj->obj->set("__async__", nova_bool(true));
            taskObj->obj->set("__result__", nova_null());
            taskObj->obj->set("__done__", nova_bool(false));

            taskObj->obj->set("__isDone__", NovaValue::makeNative([done](ValVec, auto) -> Val
                                                                  { return nova_bool(done->load()); }, "__isDone__"));

            // __drive__ — blocks the calling thread until the timer settles
            // this task, then returns the (always-null) result. Since asleep
            // never yields an intermediate nested task, one call is enough —
            // driveAsyncTask's outer loop will see __isDone__() flip true
            // right after this returns and stop.
            taskObj->obj->set("__drive__", NovaValue::makeNative([done, mu, cv](ValVec, auto) -> Val
                                                                 {
                std::unique_lock<std::mutex> lk(*mu);
                cv->wait(lk, [&]{ return done->load(); });
                return nova_null(); }, "__drive__"));

            taskObj->obj->set("then", NovaValue::makeNative([done, mu, cv, exe](ValVec a, auto cs) -> Val
                                                            {
                {
                    std::unique_lock<std::mutex> lk(*mu);
                    cv->wait(lk, [&]{ return done->load(); });
                }
                Val result = nova_null();
                if (!a.empty() && a[0] && a[0].isFunction())
                    return exe->callFunction(a[0], {result}, cs);
                return result; }, "then"));

            taskObj->obj->set("catch", NovaValue::makeNative([](ValVec, auto) -> Val
                                                             { return nova_null(); /* asleep never rejects */ }, "catch"));

            std::thread([done, mu, cv, ms]()
                        {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                {
                    std::lock_guard<std::mutex> lk(*mu);
                    done->store(true);
                }
                cv->notify_all(); })
                .detach();

            return taskObj; }, "asleep"));
        }

        obj->obj->set("Chrono", c);
    }
    // ── load bstd ────────────────────────────────────────────────────────────────

    void Executor::_loadBstd(std::shared_ptr<Scope> s)
    {
        // bstd is implemented as native C++ registrations in bstd.h/cpp
        // and also includes the Nova-source stdlib from kits/std/
        // For now we register the basic bstd functions that bstd.cpp exports
        // The full bstd registration is done in bstd.cpp via Executor::registerBstd()
        extern void registerBstd(Executor &);
        registerBstd(*this);
    }

    // ── error ─────────────────────────────────────────────────────────────────────

    void Executor::_error(const std::string &msg, const Node *n)
    {
        int line = n ? n->line : _currentLine;
        int col = n ? n->column : _currentCol;
        SourceLoc loc{line, col, filename};
        throw RuntimeError("Runtime", msg, loc, source);
    }

} // namespace novac