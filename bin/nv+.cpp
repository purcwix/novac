// ════════════════════════════════════════════════════════════════════════════
//  nv+ — Nova Package Manager  (cross-platform rewrite)
//  HTTP/S via WinINet (Windows) or OpenSSL sockets (Linux/macOS)
//  Process spawning via CreateProcess / fork+exec  (no system()/popen())
//  Commands: install | remove | list | update | publish | init | search | login | alias
// ════════════════════════════════════════════════════════════════════════════

// ── platform detection ───────────────────────────────────────────────────
#ifdef _WIN32
#define NVP_WIN 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#include <conio.h>
#include <fcntl.h> // _O_BINARY
#include <io.h>    // _setmode, _fileno
#pragma comment(lib, "wininet.lib")
#else
#define NVP_POSIX 1
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <termios.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <cassert>

namespace fs = std::filesystem;

// ════════════════════════════════════════════════════════════════════════════
//  ANSI colors
// ════════════════════════════════════════════════════════════════════════════

static bool colorEnabled()
{
    static int c = -1;
    if (c != -1)
        return c == 1;
#ifdef NVP_WIN
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
    {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        c = 1;
    }
    else
    {
        c = 0;
    }
#else
    c = std::getenv("NO_COLOR") ? 0 : 1;
#endif
    return c == 1;
}
static std::string clr(const std::string &code, const std::string &t)
{
    return colorEnabled() ? "\x1b[" + code + "m" + t + "\x1b[0m" : t;
}
static std::string red(const std::string &s) { return clr("31", s); }
static std::string green(const std::string &s) { return clr("32", s); }
static std::string yellow(const std::string &s) { return clr("33", s); }
static std::string blue(const std::string &s) { return clr("34", s); }
static std::string gray(const std::string &s) { return clr("90", s); }
static std::string bold(const std::string &s) { return clr("1", s); }
static std::string cyan(const std::string &s) { return clr("36", s); }

// ════════════════════════════════════════════════════════════════════════════
//  Minimal JSON  (flat/nested string maps)
// ════════════════════════════════════════════════════════════════════════════

struct Json
{
    enum class Type
    {
        Null,
        String,
        Number,
        Bool,
        Object,
        Array
    } type = Type::Null;
    std::string str;
    double num = 0;
    bool b = false;
    std::vector<std::pair<std::string, Json>> obj;
    std::vector<Json> arr;

    const Json *get(const std::string &key) const
    {
        for (auto &[k, v] : obj)
            if (k == key)
                return &v;
        return nullptr;
    }
    Json *get(const std::string &key)
    {
        for (auto &[k, v] : obj)
            if (k == key)
                return &v;
        return nullptr;
    }
    void set(const std::string &key, Json val)
    {
        for (auto &[k, v] : obj)
        {
            if (k == key)
            {
                v = std::move(val);
                return;
            }
        }
        obj.push_back({key, std::move(val)});
    }
    void erase(const std::string &key)
    {
        obj.erase(std::remove_if(obj.begin(), obj.end(),
                                 [&](auto &p)
                                 { return p.first == key; }),
                  obj.end());
    }
    bool has(const std::string &key) const { return get(key) != nullptr; }
    std::string asStr(const std::string &fb = "") const
    {
        return type == Type::String ? str : fb;
    }
    bool isObj() const { return type == Type::Object; }
    bool isArr() const { return type == Type::Array; }
    bool isNull() const { return type == Type::Null; }

    static Json string(const std::string &s)
    {
        Json j;
        j.type = Type::String;
        j.str = s;
        return j;
    }
    static Json object()
    {
        Json j;
        j.type = Type::Object;
        return j;
    }
    static Json null() { return {}; }
};

struct JsonParser
{
    const std::string &s;
    size_t pos = 0;
    void skip()
    {
        while (pos < s.size() && std::isspace((unsigned char)s[pos]))
            pos++;
    }
    std::string rawStr()
    {
        pos++;
        std::string out;
        while (pos < s.size() && s[pos] != '"')
        {
            if (s[pos] == '\\' && pos + 1 < s.size())
            {
                pos++;
                switch (s[pos])
                {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
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
                default:
                    out += s[pos];
                    break;
                }
            }
            else
            {
                out += s[pos];
            }
            pos++;
        }
        if (pos < s.size())
            pos++;
        return out;
    }
    Json parse()
    {
        skip();
        if (pos >= s.size())
            return {};
        char c = s[pos];
        if (c == '{')
        {
            Json j;
            j.type = Json::Type::Object;
            pos++;
            skip();
            while (pos < s.size() && s[pos] != '}')
            {
                if (s[pos] != '"')
                    break;
                std::string key = rawStr();
                skip();
                if (pos < s.size() && s[pos] == ':')
                    pos++;
                skip();
                j.obj.push_back({key, parse()});
                skip();
                if (pos < s.size() && s[pos] == ',')
                    pos++;
                skip();
            }
            if (pos < s.size())
                pos++;
            return j;
        }
        if (c == '[')
        {
            Json j;
            j.type = Json::Type::Array;
            pos++;
            skip();
            while (pos < s.size() && s[pos] != ']')
            {
                j.arr.push_back(parse());
                skip();
                if (pos < s.size() && s[pos] == ',')
                    pos++;
                skip();
            }
            if (pos < s.size())
                pos++;
            return j;
        }
        if (c == '"')
        {
            Json j;
            j.type = Json::Type::String;
            j.str = rawStr();
            return j;
        }
        if (c == 't')
        {
            pos += 4;
            Json j;
            j.type = Json::Type::Bool;
            j.b = true;
            return j;
        }
        if (c == 'f')
        {
            pos += 5;
            Json j;
            j.type = Json::Type::Bool;
            j.b = false;
            return j;
        }
        if (c == 'n')
        {
            pos += 4;
            return {};
        }
        {
            Json j;
            j.type = Json::Type::Number;
            size_t start = pos;
            while (pos < s.size() && (std::isdigit((unsigned char)s[pos]) || s[pos] == '-' || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' || s[pos] == '+'))
                pos++;
            try
            {
                j.num = std::stod(s.substr(start, pos - start));
            }
            catch (...)
            {
            }
            return j;
        }
    }
};

static Json parseJson(const std::string &s)
{
    JsonParser p{s};
    return p.parse();
}

static std::string stringifyJson(const Json &j, int indent = 2, int depth = 0)
{
    std::string pad(depth * indent, ' ');
    std::string inner((depth + 1) * indent, ' ');
    switch (j.type)
    {
    case Json::Type::Null:
        return "null";
    case Json::Type::Bool:
        return j.b ? "true" : "false";
    case Json::Type::Number:
    {
        if (j.num == (long long)j.num)
            return std::to_string((long long)j.num);
        std::ostringstream o;
        o << j.num;
        return o.str();
    }
    case Json::Type::String:
    {
        std::string out = "\"";
        for (char c : j.str)
        {
            if (c == '"')
                out += "\\\"";
            else if (c == '\\')
                out += "\\\\";
            else if (c == '\n')
                out += "\\n";
            else if (c == '\r')
                out += "\\r";
            else if (c == '\t')
                out += "\\t";
            else
                out += c;
        }
        return out + "\"";
    }
    case Json::Type::Array:
    {
        if (j.arr.empty())
            return "[]";
        std::string out = "[\n";
        for (size_t i = 0; i < j.arr.size(); i++)
        {
            out += inner + stringifyJson(j.arr[i], indent, depth + 1);
            if (i + 1 < j.arr.size())
                out += ",";
            out += "\n";
        }
        return out + pad + "]";
    }
    case Json::Type::Object:
    {
        if (j.obj.empty())
            return "{}";
        std::string out = "{\n";
        for (size_t i = 0; i < j.obj.size(); i++)
        {
            out += inner + "\"" + j.obj[i].first + "\": " + stringifyJson(j.obj[i].second, indent, depth + 1);
            if (i + 1 < j.obj.size())
                out += ",";
            out += "\n";
        }
        return out + pad + "}";
    }
    }
    return "null";
}

// ════════════════════════════════════════════════════════════════════════════
//  HTTP client — WinINet (Windows) or OpenSSL+sockets (POSIX)
// ════════════════════════════════════════════════════════════════════════════

struct HttpResponse
{
    int status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

// ── URL parser ───────────────────────────────────────────────────────────
struct Url
{
    std::string scheme, host, path;
    int port = 443;
    static Url parse(const std::string &url)
    {
        Url u;
        size_t ss = url.find("://");
        u.scheme = (ss != std::string::npos) ? url.substr(0, ss) : "https";
        size_t start = (ss != std::string::npos) ? ss + 3 : 0;
        size_t slash = url.find('/', start);
        std::string hostport = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        u.path = (slash != std::string::npos) ? url.substr(slash) : "/";
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos)
        {
            u.host = hostport.substr(0, colon);
            u.port = std::stoi(hostport.substr(colon + 1));
        }
        else
        {
            u.host = hostport;
            u.port = (u.scheme == "http") ? 80 : 443;
        }
        return u;
    }
};

// ── headers map ─────────────────────────────────────────────────────────
using Headers = std::vector<std::pair<std::string, std::string>>;

// ════════════════════════════════════════════════════════════════════════════
//  Windows  — WinINet implementation
// ════════════════════════════════════════════════════════════════════════════
#ifdef NVP_WIN

static HttpResponse httpRequest(const std::string &method,
                                const std::string &url,
                                const Headers &headers,
                                const std::string &body)
{
    HttpResponse resp;
    Url u = Url::parse(url);

    HINTERNET hInet = InternetOpenA("nv+/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet)
    {
        resp.status = -1;
        return resp;
    }

    HINTERNET hConn = InternetConnectA(hInet, u.host.c_str(), (INTERNET_PORT)u.port,
                                       nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn)
    {
        InternetCloseHandle(hInet);
        resp.status = -1;
        return resp;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (u.scheme == "https")
        flags |= INTERNET_FLAG_SECURE;

    HINTERNET hReq = HttpOpenRequestA(hConn, method.c_str(), u.path.c_str(),
                                      nullptr, nullptr, nullptr, flags, 0);
    if (!hReq)
    {
        InternetCloseHandle(hConn);
        InternetCloseHandle(hInet);
        resp.status = -1;
        return resp;
    }

    std::string hdrStr;
    for (auto &[k, v] : headers)
        hdrStr += k + ": " + v + "\r\n";

    const char *sendBuf = body.empty() ? nullptr : body.c_str();
    DWORD sendLen = body.empty() ? 0 : (DWORD)body.size();

    BOOL sent = HttpSendRequestA(hReq,
                                 hdrStr.empty() ? nullptr : hdrStr.c_str(),
                                 (DWORD)hdrStr.size(),
                                 (LPVOID)sendBuf, sendLen);

    if (sent)
    {
        DWORD statusCode = 0, size = sizeof(DWORD);
        HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &statusCode, &size, nullptr);
        resp.status = (int)statusCode;

        char buf[8192];
        DWORD read = 0;
        while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0)
            resp.body.append(buf, read);
    }
    else
    {
        resp.status = -1;
    }

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hInet);
    return resp;
}

#endif // NVP_WIN

// ════════════════════════════════════════════════════════════════════════════
//  POSIX  — OpenSSL + BSD sockets implementation
// ════════════════════════════════════════════════════════════════════════════
#ifdef NVP_POSIX

namespace detail
{

    struct TcpConn
    {
        int fd = -1;
        SSL_CTX *ctx = nullptr;
        SSL *ssl = nullptr;

        ~TcpConn()
        {
            if (ssl)
            {
                SSL_shutdown(ssl);
                SSL_free(ssl);
            }
            if (ctx)
                SSL_CTX_free(ctx);
            if (fd >= 0)
                ::close(fd);
        }

        bool connect(const std::string &host, int port, bool tls)
        {
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            std::string portStr = std::to_string(port);
            if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0)
                return false;

            for (auto *rp = res; rp; rp = rp->ai_next)
            {
                fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                if (fd < 0)
                    continue;
                if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
                    break;
                ::close(fd);
                fd = -1;
            }
            freeaddrinfo(res);
            if (fd < 0)
                return false;

            if (tls)
            {
                SSL_library_init();
                SSL_load_error_strings();
                ctx = SSL_CTX_new(TLS_client_method());
                if (!ctx)
                    return false;
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
                SSL_CTX_set_default_verify_paths(ctx);
                ssl = SSL_new(ctx);
                if (!ssl)
                    return false;
                SSL_set_fd(ssl, fd);
                SSL_set_tlsext_host_name(ssl, host.c_str());
                if (SSL_connect(ssl) != 1)
                    return false;
            }
            return true;
        }

        bool write(const std::string &data)
        {
            const char *p = data.data();
            int rem = (int)data.size();
            while (rem > 0)
            {
                int n = ssl ? SSL_write(ssl, p, rem) : ::write(fd, p, rem);
                if (n <= 0)
                    return false;
                p += n;
                rem -= n;
            }
            return true;
        }

        std::string readAll()
        {
            std::string out;
            char buf[8192];
            while (true)
            {
                int n = ssl ? SSL_read(ssl, buf, sizeof(buf))
                            : (int)::read(fd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                out.append(buf, n);
            }
            return out;
        }
    };

    static std::pair<int, std::string> parseHttpResponse(const std::string &raw)
    {
        int status = 0;
        size_t sp1 = raw.find(' ');
        if (sp1 != std::string::npos)
        {
            size_t sp2 = raw.find(' ', sp1 + 1);
            std::string code = raw.substr(sp1 + 1, sp2 - sp1 - 1);
            try
            {
                status = std::stoi(code);
            }
            catch (...)
            {
            }
        }
        size_t split = raw.find("\r\n\r\n");
        std::string body;
        if (split != std::string::npos)
            body = raw.substr(split + 4);
        std::string headers_part = raw.substr(0, split != std::string::npos ? split : 0);
        std::string lhdr = headers_part;
        std::transform(lhdr.begin(), lhdr.end(), lhdr.begin(), ::tolower);
        if (lhdr.find("transfer-encoding: chunked") != std::string::npos)
        {
            std::string decoded;
            size_t pos = 0;
            while (pos < body.size())
            {
                size_t crlf = body.find("\r\n", pos);
                if (crlf == std::string::npos)
                    break;
                std::string hexStr = body.substr(pos, crlf - pos);
                size_t chunkLen = 0;
                try
                {
                    chunkLen = std::stoul(hexStr, nullptr, 16);
                }
                catch (...)
                {
                    break;
                }
                if (chunkLen == 0)
                    break;
                pos = crlf + 2;
                if (pos + chunkLen > body.size())
                    break;
                decoded.append(body, pos, chunkLen);
                pos += chunkLen + 2;
            }
            body = std::move(decoded);
        }
        return {status, body};
    }

} // namespace detail

static HttpResponse httpRequest(const std::string &method,
                                const std::string &url,
                                const Headers &headers,
                                const std::string &body)
{
    HttpResponse resp;
    Url u = Url::parse(url);
    bool tls = (u.scheme == "https");

    detail::TcpConn conn;
    if (!conn.connect(u.host, u.port, tls))
    {
        resp.status = -1;
        return resp;
    }

    std::string req = method + " " + u.path + " HTTP/1.1\r\n";
    req += "Host: " + u.host + "\r\n";
    req += "Connection: close\r\n";
    req += "User-Agent: nv+/1.0\r\n";
    for (auto &[k, v] : headers)
        req += k + ": " + v + "\r\n";
    if (!body.empty())
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    if (!body.empty())
        req += body;

    if (!conn.write(req))
    {
        resp.status = -1;
        return resp;
    }

    std::string raw = conn.readAll();
    auto [status, rbody] = detail::parseHttpResponse(raw);
    resp.status = status;
    resp.body = std::move(rbody);
    return resp;
}

#endif // NVP_POSIX

// ── convenience wrappers ─────────────────────────────────────────────────

static HttpResponse httpGet(const std::string &url, const Headers &hdrs = {})
{
    return httpRequest("GET", url, hdrs, "");
}

static HttpResponse httpPost(const std::string &url, const Headers &hdrs,
                             const std::string &body)
{
    return httpRequest("POST", url, hdrs, body);
}

static HttpResponse httpPut(const std::string &url, const Headers &hdrs,
                            const std::string &body)
{
    return httpRequest("PUT", url, hdrs, body);
}

static HttpResponse httpGetFollow(const std::string &url, const Headers &hdrs = {})
{
    std::string cur = url;
    for (int i = 0; i < 5; i++)
    {
        auto r = httpGet(cur, hdrs);
        if (r.status == 301 || r.status == 302 || r.status == 307 || r.status == 308)
        {
            break;
        }
        return r;
    }
    return httpGet(url, hdrs);
}

// ════════════════════════════════════════════════════════════════════════════
//  Process spawning — cross-platform, no system()/popen()
// ════════════════════════════════════════════════════════════════════════════

struct ProcResult
{
    int exitCode = -1;
    std::string output;
};

#ifdef NVP_WIN
static std::string buildCmdLine(const std::vector<std::string> &args)
{
    std::string out;
    for (auto &a : args)
    {
        if (!out.empty())
            out += ' ';
        bool needQ = a.find(' ') != std::string::npos || a.find('"') != std::string::npos;
        if (needQ)
            out += '"';
        for (char c : a)
        {
            if (c == '"')
                out += "\\\"";
            else
                out += c;
        }
        if (needQ)
            out += '"';
    }
    return out;
}
#endif

static ProcResult runProc(const std::vector<std::string> &argv,
                          bool captureOutput = false)
{
    ProcResult res;
    if (argv.empty())
        return res;

#ifdef NVP_WIN
    HANDLE hReadOut = nullptr, hWriteOut = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

    if (captureOutput)
    {
        if (!CreatePipe(&hReadOut, &hWriteOut, &sa, 0))
            return res;
        SetHandleInformation(hReadOut, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (captureOutput)
    {
        si.hStdOutput = hWriteOut;
        si.hStdError = hWriteOut;
        si.dwFlags = STARTF_USESTDHANDLES;
    }

    PROCESS_INFORMATION pi{};
    std::string cmdLine = buildCmdLine(argv);

    BOOL ok = CreateProcessA(nullptr, cmdLine.data(),
                             nullptr, nullptr, captureOutput ? TRUE : FALSE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok)
    {
        if (captureOutput)
        {
            CloseHandle(hReadOut);
            CloseHandle(hWriteOut);
        }
        return res;
    }
    if (captureOutput)
        CloseHandle(hWriteOut);

    if (captureOutput)
    {
        char buf[4096];
        DWORD rd;
        while (ReadFile(hReadOut, buf, sizeof(buf), &rd, nullptr) && rd > 0)
            res.output.append(buf, rd);
        CloseHandle(hReadOut);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    res.exitCode = (int)ec;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

#else // POSIX
    int pipefd[2] = {-1, -1};
    if (captureOutput && pipe(pipefd) != 0)
        return res;

    pid_t pid = fork();
    if (pid < 0)
    {
        if (captureOutput)
        {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        return res;
    }

    if (pid == 0)
    {
        if (captureOutput)
        {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        }
        std::vector<const char *> cargv;
        for (auto &a : argv)
            cargv.push_back(a.c_str());
        cargv.push_back(nullptr);
        execvp(cargv[0], const_cast<char **>(cargv.data()));
        _exit(127);
    }

    if (captureOutput)
    {
        close(pipefd[1]);
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
            res.output.append(buf, n);
        close(pipefd[0]);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return res;
}

static bool runSilent(const std::vector<std::string> &argv)
{
    return runProc(argv, false).exitCode == 0;
}

static std::string runCapture(const std::vector<std::string> &argv)
{
    return runProc(argv, true).output;
}

// ════════════════════════════════════════════════════════════════════════════
//  Sleep (portable)
// ════════════════════════════════════════════════════════════════════════════

static void sleepSec(int s)
{
#ifdef NVP_WIN
    Sleep((DWORD)s * 1000);
#else
    sleep((unsigned)s);
#endif
}

// ════════════════════════════════════════════════════════════════════════════
//  Safe recursive remove
// ════════════════════════════════════════════════════════════════════════════

static void safeRemoveAll(const fs::path &p)
{
    if (!fs::exists(p))
        return;
#ifdef NVP_WIN
    std::error_code ec;
    for (auto &entry : fs::recursive_directory_iterator(p,
                                                        fs::directory_options::skip_permission_denied, ec))
    {
        try
        {
            fs::permissions(entry.path(),
                            fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                            fs::perm_options::add);
        }
        catch (...)
        {
        }
    }
#endif
    std::error_code ec2;
    fs::remove_all(p, ec2);
    if (ec2)
        throw std::runtime_error("cannot remove '" + p.string() + "': " + ec2.message());
}

// ════════════════════════════════════════════════════════════════════════════
//  Paths & manifest
// ════════════════════════════════════════════════════════════════════════════

static fs::path homeDir()
{
    const char *h = std::getenv("HOME");
    if (!h)
        h = std::getenv("USERPROFILE");
    return fs::path(h ? h : ".");
}

static fs::path novacDir() { return homeDir() / ".novac"; }
static fs::path globalModulesDir() { return novacDir() / "nova_modules"; }
static fs::path manifestPath() { return fs::current_path() / "novapkg.json"; }
static fs::path tokenPath() { return novacDir() / "token"; }
static fs::path aliasPath() { return novacDir() / "aliases.json"; }

static Json loadManifest()
{
    std::ifstream f(manifestPath());
    if (!f.is_open())
        return Json::object();
    std::string s((std::istreambuf_iterator<char>(f)), {});
    return parseJson(s);
}
static void saveManifest(const Json &j)
{
    std::ofstream f(manifestPath());
    f << stringifyJson(j, 2) << "\n";
}

// ════════════════════════════════════════════════════════════════════════════
//  Aliases
// ════════════════════════════════════════════════════════════════════════════

static const std::map<std::string, std::string> BUILTIN_ALIASES = {
    {"i", "install"},
    {"rm", "remove"},
    {"ls", "list"},
    {"up", "update"},
    {"s", "search"},
    {"pub", "publish"},
    {"lgi", "login"},
    {"lgo", "logout"},
    {"wa", "whoami"},
};

static Json loadAliases()
{
    std::ifstream f(aliasPath());
    if (!f.is_open())
        return Json::object();
    std::string s((std::istreambuf_iterator<char>(f)), {});
    Json j = parseJson(s);
    return j.isObj() ? j : Json::object();
}

static void saveAliases(const Json &j)
{
    fs::create_directories(novacDir());
    std::ofstream f(aliasPath());
    f << stringifyJson(j, 2) << "\n";
}

static std::string resolveAlias(const std::string &cmd)
{
    Json userAliases = loadAliases();
    if (userAliases.has(cmd))
        return userAliases.get(cmd)->asStr();
    auto it = BUILTIN_ALIASES.find(cmd);
    if (it != BUILTIN_ALIASES.end())
        return it->second;
    return cmd;
}

int cmdAlias(const std::vector<std::string> &args)
{
    Json userAliases = loadAliases();

    if (args.empty())
    {
        std::cout << bold("Built-in aliases:") << "\n";
        for (auto &[k, v] : BUILTIN_ALIASES)
            std::cout << "  " << cyan(k) << gray(" → ") << bold(v) << "\n";

        bool hasUser = !userAliases.obj.empty();
        std::cout << "\n"
                  << bold("User aliases:") << "\n";
        if (!hasUser)
        {
            std::cout << gray("  (none)\n");
        }
        else
        {
            for (auto &[k, v] : userAliases.obj)
                std::cout << "  " << cyan(k) << gray(" → ") << bold(v.asStr()) << "\n";
        }
        return 0;
    }

    if (args[0] == "--remove" || args[0] == "-r")
    {
        if (args.size() < 2)
        {
            std::cerr << red("Usage: nv+ alias --remove <name>") << "\n";
            return 1;
        }
        const std::string &name = args[1];
        if (!userAliases.has(name))
        {
            std::cerr << yellow("No user alias \"" + name + "\" found.") << "\n";
            return 1;
        }
        userAliases.erase(name);
        saveAliases(userAliases);
        std::cout << green("Removed alias: " + name) << "\n";
        return 0;
    }

    for (auto &arg : args)
    {
        size_t eq = arg.find('=');
        if (eq == std::string::npos)
        {
            std::string resolved = resolveAlias(arg);
            if (resolved == arg)
                std::cout << yellow("No alias found for \"" + arg + "\".") << "\n";
            else
                std::cout << cyan(arg) << gray(" → ") << bold(resolved) << "\n";
            continue;
        }
        std::string name = arg.substr(0, eq);
        std::string value = arg.substr(eq + 1);
        if (name.empty() || value.empty())
        {
            std::cerr << red("Invalid alias spec: \"" + arg + "\". Use name=command.") << "\n";
            return 1;
        }
        if (name == "alias")
        {
            std::cerr << red("Cannot alias 'alias' itself.") << "\n";
            return 1;
        }
        userAliases.set(name, Json::string(value));
        std::cout << green("Set alias: ") << cyan(name) << gray(" → ") << bold(value) << "\n";
    }
    saveAliases(userAliases);
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  Registry — per-file layout
//
//  nova-registry/
//    packages/
//      mypackage.json      ← one file per package
//      anotherpackage.json
//
//  Each packages/{name}.json:
//  {
//    "name":        "mypackage",
//    "repo":        "username/mypackage",
//    "description": "...",
//    "latest":      "1.0.0",
//    "author":      "username"   ← used by CI to verify ownership
//  }
// ════════════════════════════════════════════════════════════════════════════

static const std::string REGISTRY_OWNER = "purcwix";
static const std::string REGISTRY_REPO = "nova-registry";

// Raw content base — append "{name}.json" to fetch a single package
static const std::string REGISTRY_RAW_BASE =
    "https://raw.githubusercontent.com/purcwix/nova-registry/main/packages/";

// GitHub Contents API — used for directory listing (search) and SHA lookup
static const std::string REGISTRY_API_PACKAGES =
    "https://api.github.com/repos/purcwix/nova-registry/contents/packages";

// ── fetch a single package file (no auth needed) ─────────────────────────
//   Returns Json::null() if the package doesn't exist (404).
//   Throws on network/parse errors.
static Json fetchPackageJson(const std::string &name)
{
    auto r = httpGetFollow(REGISTRY_RAW_BASE + name + ".json");
    if (r.status == 404)
        return Json::null();
    if (!r.ok())
        throw std::runtime_error(
            "Failed to fetch package '" + name + "' (HTTP " + std::to_string(r.status) + "). "
                                                                                         "Check your connection.");
    Json j = parseJson(r.body);
    if (j.isNull())
        throw std::runtime_error("Package '" + name + "' returned invalid JSON.");
    return j;
}

struct PkgInfo
{
    std::string name, repo, description, latest, author;
};

// ── look up a single package by name ─────────────────────────────────────
static PkgInfo lookupPackage(const std::string &name)
{
    std::cout << gray("  fetching " + name + "...") << "\n";
    Json entry = fetchPackageJson(name);
    if (entry.isNull())
        throw std::runtime_error("Package not found in registry: " + name);

    PkgInfo info;
    info.name = name;
    info.repo = entry.has("repo") ? entry.get("repo")->asStr() : "";
    info.description = entry.has("description") ? entry.get("description")->asStr() : "";
    info.latest = entry.has("latest") ? entry.get("latest")->asStr() : "latest";
    info.author = entry.has("author") ? entry.get("author")->asStr() : "";
    if (info.repo.empty())
        throw std::runtime_error("Package '" + name + "' has no repo field.");
    return info;
}

// ════════════════════════════════════════════════════════════════════════════
//  Git helpers (via runProc)
// ════════════════════════════════════════════════════════════════════════════

static bool hasGit()
{
    auto out = runCapture({"git", "--version"});
    return out.find("git version") != std::string::npos;
}

static bool gitClone(const std::string &repoUrl, const fs::path &dest)
{
    return runSilent({"git", "clone", "--depth", "1", "--quiet",
                      repoUrl, dest.string()});
}

// ════════════════════════════════════════════════════════════════════════════
//  Token storage
// ════════════════════════════════════════════════════════════════════════════

static std::string loadToken()
{
    std::ifstream f(tokenPath());
    if (!f.is_open())
        return "";
    std::string t;
    std::getline(f, t);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' '))
        t.pop_back();
    return t;
}

static void saveToken(const std::string &token)
{
    fs::create_directories(novacDir());
    std::ofstream f(tokenPath());
    f << token << "\n";
#ifdef NVP_POSIX
    fs::permissions(tokenPath(),
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
#endif
}

// ════════════════════════════════════════════════════════════════════════════
//  GitHub API helpers
// ════════════════════════════════════════════════════════════════════════════

static Headers ghHeaders(const std::string &token)
{
    return {
        {"Authorization", "Bearer " + token},
        {"Accept", "application/vnd.github+json"},
        {"Content-Type", "application/json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
    };
}

static std::string ghGetUsername(const std::string &token)
{
    auto r = httpGet("https://api.github.com/user", ghHeaders(token));
    if (!r.ok())
        return "";
    Json j = parseJson(r.body);
    return j.has("login") ? j.get("login")->asStr() : "";
}

static bool ghFork(const std::string &token,
                   const std::string &owner, const std::string &repo)
{
    auto r = httpPost("https://api.github.com/repos/" + owner + "/" + repo + "/forks",
                      ghHeaders(token), "{}");
    Json j = parseJson(r.body);
    return j.has("full_name");
}

static std::string b64encode(const std::string &in)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in)
    {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0)
        {
            out.push_back(T[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        out.push_back(T[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4)
        out.push_back('=');
    return out;
}

// Get SHA of a file in a repo (needed for PUT to update existing files)
static std::string ghGetFileSha(const std::string &token,
                                const std::string &owner,
                                const std::string &repo,
                                const std::string &path,
                                const std::string &branch = "")
{
    std::string url = "https://api.github.com/repos/" + owner + "/" + repo + "/contents/" + path;
    if (!branch.empty())
        url += "?ref=" + branch;
    auto r = httpGet(url, ghHeaders(token));
    Json j = parseJson(r.body);
    return j.has("sha") ? j.get("sha")->asStr() : "";
}

static bool ghPutFile(const std::string &token,
                      const std::string &owner, const std::string &repo,
                      const std::string &path, const std::string &message,
                      const std::string &content, const std::string &branch,
                      const std::string &sha = "")
{
    std::string body = "{\"message\":\"" + message + "\","
                                                     "\"content\":\"" +
                       b64encode(content) + "\","
                                            "\"branch\":\"" +
                       branch + "\"";
    if (!sha.empty())
        body += ",\"sha\":\"" + sha + "\"";
    body += "}";
    auto r = httpPut("https://api.github.com/repos/" + owner + "/" + repo + "/contents/" + path,
                     ghHeaders(token), body);
    Json j = parseJson(r.body);
    return j.has("content");
}

static bool ghCreateBranch(const std::string &token,
                           const std::string &owner,
                           const std::string &repo,
                           const std::string &branch)
{
    // Check if branch already exists
    auto existing = httpGet("https://api.github.com/repos/" + owner + "/" + repo + "/git/ref/heads/" + branch,
                            ghHeaders(token));
    if (existing.ok())
    {
        return true; // already exists, fine
    }

    auto r = httpGet("https://api.github.com/repos/" + owner + "/" + repo + "/git/ref/heads/main",
                     ghHeaders(token));
    Json j = parseJson(r.body);
    if (!j.has("object"))
        return false;
    std::string sha = j.get("object")->get("sha")->asStr();
    std::string body = "{\"ref\":\"refs/heads/" + branch + "\",\"sha\":\"" + sha + "\"}";
    auto r2 = httpPost("https://api.github.com/repos/" + owner + "/" + repo + "/git/refs",
                       ghHeaders(token), body);
    Json j2 = parseJson(r2.body);
    return j2.has("ref");
}

static std::string ghOpenPR(const std::string &token,
                            const std::string &owner, const std::string &repo,
                            const std::string &title, const std::string &body,
                            const std::string &head, const std::string &base)
{
    std::string payload = "{\"title\":\"" + title + "\","
                                                    "\"body\":\"" +
                          body + "\","
                                 "\"head\":\"" +
                          head + "\","
                                 "\"base\":\"" +
                          base + "\"}";
    auto r = httpPost("https://api.github.com/repos/" + owner + "/" + repo + "/pulls",
                      ghHeaders(token), payload);
    Json j = parseJson(r.body);
    if (j.has("html_url"))
        return j.get("html_url")->asStr();

    // PR may already exist — search for it
    std::string searchUrl = "https://api.github.com/repos/" + owner + "/" + repo +
                            "/pulls?state=open&head=" + head + "&base=" + base;
    auto r2 = httpGet(searchUrl, ghHeaders(token));
    Json j2 = parseJson(r2.body);
    if (j2.isArr() && !j2.arr.empty() && j2.arr[0].has("html_url"))
        return j2.arr[0].get("html_url")->asStr();

    return "";
}

// ════════════════════════════════════════════════════════════════════════════
//  nv+ install
// ════════════════════════════════════════════════════════════════════════════

static int doInstallOne(const PkgInfo &pkg, const fs::path &modulesDir, bool updateManifest)
{
    fs::path dest = modulesDir / pkg.name;

    if (fs::exists(dest))
    {
        std::cout << yellow("  skip  " + pkg.name + " (already installed — run nv+ update to upgrade)") << "\n";
        return 0;
    }
    if (!hasGit())
    {
        std::cerr << red("  fail  git not found. Install git and retry.") << "\n";
        return 1;
    }

    std::string cloneUrl = "https://github.com/" + pkg.repo + ".git";
    std::cout << gray("  cloning " + pkg.repo + "...") << "\n";

    if (!gitClone(cloneUrl, dest))
    {
        safeRemoveAll(dest);
        std::cerr << red("  fail  " + pkg.name + ": git clone failed (" + cloneUrl + ")") << "\n";
        return 1;
    }

    safeRemoveAll(dest / ".git");
    std::cout << green("  ok    " + pkg.name + " @ " + pkg.latest) << "\n";
    if (!pkg.description.empty())
        std::cout << gray("         " + pkg.description) << "\n";

    if (updateManifest)
    {
        Json manifest = loadManifest();
        if (manifest.isNull())
            manifest = Json::object();
        if (!manifest.has("dependencies"))
            manifest.set("dependencies", Json::object());
        manifest.get("dependencies")->set(pkg.name, Json::string("^" + pkg.latest));
        saveManifest(manifest);
    }
    return 0;
}

int cmdInstall(const std::vector<std::string> &names, bool global)
{
    fs::path modulesDir = global ? globalModulesDir() : (fs::current_path() / "nova_modules");
    fs::create_directories(modulesDir);
    if (global)
        std::cout << blue("  installing globally → " + modulesDir.string()) << "\n";

    // No names → install from manifest dependencies
    if (names.empty())
    {
        Json manifest = loadManifest();
        if (manifest.isNull())
        {
            std::cerr << red("No novapkg.json found. Run nv+ init first.") << "\n";
            return 1;
        }
        const Json *deps = manifest.get("dependencies");
        if (!deps || !deps->isObj() || deps->obj.empty())
        {
            std::cout << yellow("No dependencies in novapkg.json.") << "\n";
            return 0;
        }

        std::cout << blue("Installing " + std::to_string(deps->obj.size()) + " dep(s)...\n");
        int ok = 0, fail = 0;
        for (auto &[name, _] : deps->obj)
        {
            try
            {
                auto pkg = lookupPackage(name);
                if (doInstallOne(pkg, modulesDir, false) == 0)
                    ok++;
                else
                    fail++;
            }
            catch (const std::exception &e)
            {
                std::cerr << red("  fail  " + name + ": " + e.what()) << "\n";
                fail++;
            }
        }
        std::cout << blue("\nDone. " + std::to_string(ok) + " installed, " + std::to_string(fail) + " failed.") << "\n";
        return fail > 0 ? 1 : 0;
    }

    // Named packages → look up each one individually (one HTTP request per package)
    int ok = 0, fail = 0;
    for (auto &name : names)
    {
        try
        {
            auto pkg = lookupPackage(name);
            if (doInstallOne(pkg, modulesDir, !global) == 0)
                ok++;
            else
                fail++;
        }
        catch (const std::exception &e)
        {
            std::cerr << red("  fail  " + name + ": " + e.what()) << "\n";
            fail++;
        }
    }
    if (names.size() > 1)
        std::cout << blue("\nDone. " + std::to_string(ok) + " installed, " + std::to_string(fail) + " failed.") << "\n";
    return (fail > 0 && ok == 0) ? 1 : 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  nv+ remove
// ════════════════════════════════════════════════════════════════════════════

int cmdRemove(const std::vector<std::string> &names, bool global, bool yes)
{
    if (names.empty())
    {
        std::cerr << red("Usage: nv+ remove <package...>") << "\n";
        return 1;
    }
    fs::path modulesDir = global ? globalModulesDir() : (fs::current_path() / "nova_modules");

    int ok = 0, fail = 0;
    for (auto &name : names)
    {
        fs::path dest = modulesDir / name;
        if (!fs::exists(dest))
        {
            std::cerr << yellow("  skip  " + name + " (not installed)") << "\n";
            fail++;
            continue;
        }
        if (!yes)
        {
            std::cout << "  Remove " << bold(name) << "? [y/N] ";
            std::string ans;
            std::getline(std::cin, ans);
            if (ans != "y" && ans != "Y")
            {
                std::cout << gray("  skip  " + name) << "\n";
                continue;
            }
        }
        safeRemoveAll(dest);
        std::cout << green("  ok    removed " + name) << "\n";
        if (!global)
        {
            Json manifest = loadManifest();
            if (!manifest.isNull() && manifest.has("dependencies"))
                manifest.get("dependencies")->erase(name);
            saveManifest(manifest);
        }
        ok++;
    }
    return (fail > 0 && ok == 0) ? 1 : 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  nv+ list
// ════════════════════════════════════════════════════════════════════════════

int cmdList(const std::string &pattern, bool global)
{
    fs::path modulesDir = global ? globalModulesDir() : (fs::current_path() / "nova_modules");
    std::string scope = global ? "global" : "local";

    if (!fs::exists(modulesDir))
    {
        std::cout << yellow("No " + scope + " nova_modules directory found.") << "\n";
        return 0;
    }
    std::vector<std::string> names;
    for (auto &e : fs::directory_iterator(modulesDir))
        if (e.is_directory())
            names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());

    if (!pattern.empty())
    {
        names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string &n)
                                   { return n.find(pattern) == std::string::npos; }),
                    names.end());
    }

    if (names.empty())
    {
        std::cout << yellow("No " + scope + " packages installed" + (pattern.empty() ? "." : " matching \"" + pattern + "\".")) << "\n";
        return 0;
    }

    std::cout << bold("Nova packages (" + scope + "):") << "\n";
    for (auto &name : names)
    {
        std::string version;
        fs::path pkgManifest = modulesDir / name / "novapkg.json";
        if (fs::exists(pkgManifest))
        {
            std::ifstream f(pkgManifest);
            std::string s((std::istreambuf_iterator<char>(f)), {});
            Json j = parseJson(s);
            if (j.has("version"))
                version = " @ " + j.get("version")->asStr();
        }
        std::cout << "  " << green("✓") << " " << bold(name) << gray(version) << "\n";
    }
    std::cout << gray("\n" + std::to_string(names.size()) + " package(s)") << "\n";
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  nv+ update
// ════════════════════════════════════════════════════════════════════════════

int cmdUpdate(const std::vector<std::string> &names, bool global)
{
    fs::path modulesDir = global ? globalModulesDir() : (fs::current_path() / "nova_modules");

    if (!fs::exists(modulesDir))
    {
        std::cerr << yellow("No nova_modules directory found. Nothing to update.") << "\n";
        return 0;
    }
    std::vector<std::string> targets = names;
    if (targets.empty())
    {
        for (auto &e : fs::directory_iterator(modulesDir))
            if (e.is_directory())
                targets.push_back(e.path().filename().string());
        std::sort(targets.begin(), targets.end());
    }
    if (targets.empty())
    {
        std::cout << yellow("No packages installed.") << "\n";
        return 0;
    }

    if (!hasGit())
    {
        std::cerr << red("git not found. Install git and retry.") << "\n";
        return 1;
    }

    std::cout << blue("Updating " + std::to_string(targets.size()) + " package(s)...\n");
    int ok = 0, fail = 0, skip = 0;

    for (auto &name : targets)
    {
        fs::path dest = modulesDir / name;
        PkgInfo pkg;
        try
        {
            pkg = lookupPackage(name);
        }
        catch (...)
        {
            std::cout << gray("  skip  " + name + " (not in registry)") << "\n";
            skip++;
            continue;
        }

        std::cout << gray("  updating " + name + "...") << "\n";
        safeRemoveAll(dest);

        std::string cloneUrl = "https://github.com/" + pkg.repo + ".git";
        if (!gitClone(cloneUrl, dest))
        {
            std::cerr << red("  fail  " + name + ": git clone failed") << "\n";
            fail++;
            continue;
        }
        safeRemoveAll(dest / ".git");
        std::cout << green("  ok    " + name + " @ " + pkg.latest) << "\n";

        if (!global)
        {
            Json manifest = loadManifest();
            if (!manifest.isNull() && manifest.has("dependencies"))
                manifest.get("dependencies")->set(name, Json::string("^" + pkg.latest));
            saveManifest(manifest);
        }
        ok++;
    }

    std::cout << blue("\nDone. " + std::to_string(ok) + " updated, " + std::to_string(skip) + " skipped, " + std::to_string(fail) + " failed.") << "\n";
    return fail > 0 ? 1 : 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  nv+ search
//  Uses GitHub Contents API to list packages/ directory, filters by name.
//  Fetches individual package files only for matches (avoids downloading
//  the entire registry for every search).
// ════════════════════════════════════════════════════════════════════════════

int cmdSearch(const std::string &query)
{
    // Use token if available to avoid GitHub API rate limits (60/hr unauthed)
    std::string token = loadToken();
    Headers hdrs = token.empty()
                       ? Headers{{"Accept", "application/vnd.github+json"},
                                 {"X-GitHub-Api-Version", "2022-11-28"}}
                       : ghHeaders(token);

    std::cout << gray("  fetching package list...") << "\n";
    auto r = httpGet(REGISTRY_API_PACKAGES, hdrs);
    if (!r.ok())
    {
        std::cerr << red("Failed to fetch package list (HTTP " + std::to_string(r.status) + ").") << "\n";
        if (r.status == 403)
            std::cerr << yellow("  Tip: run nv+ login to avoid API rate limits.") << "\n";
        return 1;
    }

    Json arr = parseJson(r.body);
    if (!arr.isArr())
    {
        // packages/ directory may not exist yet (empty registry)
        std::cout << yellow("No packages found matching \"" + query + "\".") << "\n";
        return 0;
    }

    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);

    // Filter filenames by query (O(1) per package — no content download yet)
    std::vector<std::string> matches;
    for (auto &item : arr.arr)
    {
        if (!item.has("name"))
            continue;
        std::string fname = item.get("name")->asStr();
        // Only consider *.json files
        if (fname.size() <= 5 || fname.substr(fname.size() - 5) != ".json")
            continue;
        std::string pkgName = fname.substr(0, fname.size() - 5);
        std::string lower = pkgName;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find(q) != std::string::npos)
            matches.push_back(pkgName);
    }

    if (matches.empty())
    {
        std::cout << yellow("No packages found matching \"" + query + "\".") << "\n";
        return 0;
    }

    std::cout << bold("Results for \"" + query + "\":") << "\n\n";
    for (auto &name : matches)
    {
        // Fetch individual package file for description/version
        try
        {
            Json pkg = fetchPackageJson(name);
            std::string version = pkg.has("latest") ? pkg.get("latest")->asStr() : "?";
            std::string desc = pkg.has("description") ? pkg.get("description")->asStr() : "";
            std::string author = pkg.has("author") ? pkg.get("author")->asStr() : "";
            std::cout << "  " << bold(name) << gray(" @ " + version);
            if (!author.empty())
                std::cout << gray(" by " + author);
            std::cout << "\n";
            if (!desc.empty())
                std::cout << "  " << gray(desc) << "\n";
            std::cout << "  " << gray("nv+ install " + name) << "\n\n";
        }
        catch (...)
        {
            // Package file temporarily unavailable — show name only
            std::cout << "  " << bold(name) << "\n";
            std::cout << "  " << gray("nv+ install " + name) << "\n\n";
        }
    }
    std::cout << gray(std::to_string(matches.size()) + " result(s)") << "\n";
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  nv+ login
// ════════════════════════════════════════════════════════════════════════════

static std::string readPasswordLine()
{
    std::string token;
#ifdef NVP_WIN
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode & ~(ENABLE_ECHO_INPUT));
    std::getline(std::cin, token);
    SetConsoleMode(h, mode);
    std::cout << "\n";
#else
    struct termios old{}, noecho{};
    tcgetattr(STDIN_FILENO, &old);
    noecho = old;
    noecho.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    std::getline(std::cin, token);
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    std::cout << "\n";
#endif
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n' || token.back() == ' '))
        token.pop_back();
    return token;
}

int cmdLogin()
{
    std::cout << bold("nv+ login") << " — GitHub Personal Access Token\n\n";
    std::cout << "Create a token at:\n";
    std::cout << cyan("  https://github.com/settings/tokens/new") << "\n\n";
    std::cout << "Required scopes: " << bold("repo") << gray(" (needed to create repos and publish)") << "\n\n";

    std::string existing = loadToken();
    if (!existing.empty())
        std::cout << yellow("A token is already saved. Enter a new one to replace it, or press Enter to keep it.\n\n");

    std::cout << "Paste token: " << std::flush;
    std::string token = readPasswordLine();

    if (token.empty())
    {
        if (!existing.empty())
        {
            std::cout << gray("Keeping existing token.\n");
            return 0;
        }
        std::cerr << red("No token entered.") << "\n";
        return 1;
    }

    std::cout << gray("  verifying...") << "\n";
    std::string username = ghGetUsername(token);
    if (username.empty())
    {
        std::cerr << red("Token verification failed. Make sure it has the 'repo' scope.") << "\n";
        return 1;
    }

    saveToken(token);
    std::cout << green("Logged in as ") << bold(username) << "\n";
    std::cout << gray("Token saved to " + tokenPath().string()) << "\n";
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  nv+ publish
//
//  Registry layout: packages/{name}.json (one file per package)
//
//  Flow:
//    1. Validate novapkg.json
//    2. Authenticate with GitHub
//    3. Create/push source repo
//    4. Fork nova-registry (idempotent)
//    5. Create publish branch on fork
//    6. PUT packages/{name}.json to fork  ← single file, no full registry fetch
//    7. Open PR → CI validates and auto-merges
// ════════════════════════════════════════════════════════════════════════════

int cmdPublish()
{
    if (!fs::exists(manifestPath()))
    {
        std::cerr << red("No novapkg.json found. Run nv+ init first.") << "\n";
        return 1;
    }
    Json manifest = loadManifest();
    if (manifest.isNull())
    {
        std::cerr << red("novapkg.json is empty or invalid.") << "\n";
        return 1;
    }

    // Validate required manifest fields
    std::vector<std::string> required = {"name", "version", "description", "main"};
    std::vector<std::string> missing;
    for (auto &f : required)
        if (!manifest.has(f) || manifest.get(f)->asStr().empty())
            missing.push_back(f);
    if (!missing.empty())
    {
        std::cerr << red("novapkg.json is missing required fields:") << "\n";
        for (auto &f : missing)
            std::cerr << red("  - " + f) << "\n";
        return 1;
    }

    std::string pkgName = manifest.get("name")->asStr();
    std::string version = manifest.get("version")->asStr();
    std::string desc = manifest.get("description")->asStr();

    std::string token = loadToken();
    if (token.empty())
    {
        std::cerr << red("Not logged in. Run: nv+ login") << "\n";
        return 1;
    }

    std::cout << gray("  authenticating...") << "\n";
    std::string username = ghGetUsername(token);
    if (username.empty())
    {
        std::cerr << red("Token invalid or expired. Run: nv+ login") << "\n";
        return 1;
    }
    std::cout << gray("  logged in as " + username) << "\n";

    // ── 1. Create the package's GitHub repo ───────────────────────────────
    std::cout << gray("  creating repo " + username + "/" + pkgName + "...") << "\n";
    {
        std::string safeDesc;
        for (char c : desc)
        {
            if (c == '"')
                safeDesc += "\\\"";
            else if (c == '\\')
                safeDesc += "\\\\";
            else
                safeDesc += c;
        }
        std::string body = "{\"name\":\"" + pkgName + "\","
                                                      "\"description\":\"" +
                           safeDesc + "\","
                                      "\"private\":false,"
                                      "\"auto_init\":true}";
        auto r = httpPost("https://api.github.com/user/repos", ghHeaders(token), body);
        if (r.status != 201 && r.status != 422)
        {
            std::cerr << red("Failed to create GitHub repo '" + pkgName + "'.\n"
                                                                          "Make sure your token has the 'repo' scope.")
                      << "\n";
            return 1;
        }
    }
    std::cout << green("  repo ready: github.com/" + username + "/" + pkgName) << "\n";

    // ── 2. Push local source to the repo ──────────────────────────────────
    if (!hasGit())
    {
        std::cerr << red("git not found. Install git and retry.") << "\n";
        return 1;
    }

    std::string repoUrl = "https://" + token + "@github.com/" + username + "/" + pkgName + ".git";
    fs::path cwd = fs::current_path();

    if (!fs::exists(cwd / ".git"))
    {
        std::cout << gray("  initializing git repo...") << "\n";
        if (!runSilent({"git", "-C", cwd.string(), "init"}))
        {
            std::cerr << red("git init failed.") << "\n";
            return 1;
        }
    }

    // Always stage and commit current state so every publish pushes the right source
    runSilent({"git", "-C", cwd.string(), "add", "."});
    runSilent({"git", "-C", cwd.string(), "commit", "-m", "publish " + pkgName + " @ " + version});

    std::cout << gray("  pushing source to github.com/" + username + "/" + pkgName + "...") << "\n";
    runSilent({"git", "-C", cwd.string(), "remote", "remove", "origin"});
    if (!runSilent({"git", "-C", cwd.string(), "remote", "add", "origin", repoUrl}))
    {
        std::cerr << red("Failed to set git remote.") << "\n";
        return 1;
    }

    bool pushed = runSilent({"git", "-C", cwd.string(), "push", "-u", "origin", "HEAD:main", "--force"});
    if (!pushed)
        pushed = runSilent({"git", "-C", cwd.string(), "push", "-u", "origin", "HEAD:master", "--force"});
    if (!pushed)
    {
        std::cerr << red("Failed to push source code to GitHub.") << "\n";
        return 1;
    }
    std::cout << green("  source pushed.") << "\n";

    // ── 3. Fork the registry (idempotent) ─────────────────────────────────
    std::cout << gray("  forking registry...") << "\n";
    ghFork(token, REGISTRY_OWNER, REGISTRY_REPO);
    std::cout << gray("  waiting for fork to be ready...") << "\n";
    bool forkReady = false;
    for (int i = 0; i < 15; i++)
    {
        auto r = httpGet("https://api.github.com/repos/" + username + "/" + REGISTRY_REPO,
                         ghHeaders(token));
        if (r.ok())
        {
            forkReady = true;
            break;
        }
        std::cout << gray("  ...") << "\n";
        sleepSec(2);
    }
    if (!forkReady)
    {
        std::cerr << red("Fork not ready after 30s. Try nv+ publish again.") << "\n";
        return 1;
    }

    // ── 4. Create a publish branch on the fork ────────────────────────────
    std::string branch = "publish-" + pkgName + "-" + version;
    std::replace(branch.begin(), branch.end(), '.', '-');
    std::cout << gray("  creating branch " + branch + "...") << "\n";
    ghCreateBranch(token, username, REGISTRY_REPO, branch);

    // ── 5. Build packages/{name}.json content ─────────────────────────────
    //  Includes "author" field so the CI workflow can verify ownership.
    Json entry = Json::object();
    entry.set("name", Json::string(pkgName));
    entry.set("repo", Json::string(username + "/" + pkgName));
    entry.set("description", Json::string(desc));
    entry.set("latest", Json::string(version));
    entry.set("author", Json::string(username)); // ownership anchor for CI

    std::string pkgContent = stringifyJson(entry, 2) + "\n";
    std::string filePath = "packages/" + pkgName + ".json";

    // ── 6. PUT the single package file (get SHA if updating) ─────────────
    std::cout << gray("  writing packages/" + pkgName + ".json...") << "\n";
    std::string sha = ghGetFileSha(token, username, REGISTRY_REPO, filePath, branch);

    bool pushed2 = ghPutFile(token, username, REGISTRY_REPO,
                             filePath,
                             "publish: add " + pkgName + " @ " + version,
                             pkgContent, branch, sha);
    if (!pushed2)
    {
        std::cerr << red("Failed to write packages/" + pkgName + ".json to your fork.") << "\n";
        return 1;
    }

    // ── 7. Open PR ────────────────────────────────────────────────────────
    std::cout << gray("  opening pull request...") << "\n";
    std::string prTitle = "publish: " + pkgName + " @ " + version;
    std::string prBody = "Adding **" + pkgName + "** to the Nova registry.\\n\\n"
                                                 "- Repo: `" +
                         username + "/" + pkgName + "`\\n"
                                                    "- Version: `" +
                         version + "`\\n"
                                   "- Description: " +
                         desc;
    std::string prUrl = ghOpenPR(token, REGISTRY_OWNER, REGISTRY_REPO,
                                 prTitle, prBody,
                                 username + ":" + branch, "main");
    if (prUrl.empty())
    {
        std::cerr << red("Failed to open PR. Open it manually:\n");
        std::cerr << gray("  https://github.com/" + REGISTRY_OWNER + "/" + REGISTRY_REPO + "/pulls") << "\n";
        return 1;
    }

    std::cout << "\n"
              << green("Published successfully!") << "\n";
    std::cout << bold("  Repo:  https://github.com/" + username + "/" + pkgName) << "\n";
    std::cout << bold("  PR:    " + prUrl) << "\n\n";
    std::cout << gray("The PR will be validated and auto-merged by CI.\n");
    std::cout << gray("Once merged, your package will be installable via:\n");
    std::cout << bold("  nv+ install " + pkgName) << "\n\n";
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  nv+ init
// ════════════════════════════════════════════════════════════════════════════

int cmdInit()
{
    if (fs::exists(manifestPath()))
    {
        std::cerr << yellow("novapkg.json already exists.") << "\n";
        return 0;
    }
    Json j = Json::object();
    j.set("name", Json::string(fs::current_path().filename().string()));
    j.set("version", Json::string("1.0.0"));
    j.set("description", Json::string(""));
    j.set("author", Json::string(""));
    j.set("license", Json::string("MIT"));
    j.set("main", Json::string("src/main.nv"));
    j.set("dependencies", Json::object());
    saveManifest(j);
    std::cout << green("Created novapkg.json") << "\n";
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  Help / Version
// ════════════════════════════════════════════════════════════════════════════

static void printHelp()
{
    std::cout
        << bold("nv+") << " — Nova Package Manager\n\n"
        << bold("Usage:") << "\n"
        << "  nv+ <command> [args]\n\n"
        << bold("Commands:\n")
        << "  " << cyan("install") << " [pkg...]       Install package(s) or all deps from novapkg.json\n"
        << "  " << cyan("remove") << "  <pkg...>       Remove package(s)\n"
        << "  " << cyan("list") << "   [pattern]      List installed packages\n"
        << "  " << cyan("update") << "  [pkg...]       Update package(s) to latest\n"
        << "  " << cyan("search") << "  <query>        Search registry by name\n"
        << "  " << cyan("init") << "                 Create novapkg.json\n\n"
        << bold("Publishing:\n")
        << "  " << cyan("login") << "                 Save GitHub token (needs 'repo' scope)\n"
        << "  " << cyan("logout") << "                Remove saved token\n"
        << "  " << cyan("whoami") << "                Show logged-in GitHub user\n"
        << "  " << cyan("publish") << "               Create repo + auto-PR to nova-registry\n\n"
        << bold("Aliases:\n")
        << "  " << cyan("alias") << "                 List all aliases (built-in + user)\n"
        << "  " << cyan("alias") << " a=b              Set user alias  a → b\n"
        << "  " << cyan("alias") << " --remove a       Remove user alias\n\n"
        << bold("Built-in aliases:\n");
    for (auto &[k, v] : BUILTIN_ALIASES)
        std::cout << "  " << cyan(k) << gray(" → ") << v << "\n";
    std::cout
        << "\n"
        << bold("Flags:\n")
        << "  -g, --global   Install/remove/list globally (~/.novac/nova_modules)\n"
        << "  -y, --yes      Skip confirmation prompts\n\n"
        << bold("Build:\n")
        << "  Windows:  cl nv+.cpp /std:c++17 /EHsc /O2 /Fe:nv+.exe wininet.lib\n"
        << "  Linux:    g++ -std=c++17 -O2 nv+.cpp -lssl -lcrypto -o nv+\n"
        << "  macOS:    g++ -std=c++17 -O2 nv+.cpp -lssl -lcrypto -o nv+\n\n"
        << bold("Registry:\n")
        << "  " << gray("https://github.com/purcwix/nova-registry/tree/main/packages") << "\n\n";
}

static void printVersion() { std::cout << "nv+ 1.0.0\n"; }

// ════════════════════════════════════════════════════════════════════════════
//  main
// ════════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
#ifdef NVP_WIN
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty())
    {
        printHelp();
        return 0;
    }

    bool global = false, yes = false;
    std::vector<std::string> filtered;
    for (auto &a : args)
    {
        if (a == "-g" || a == "--global")
            global = true;
        else if (a == "-y" || a == "--yes")
            yes = true;
        else if (a == "--help" || a == "-h")
        {
            printHelp();
            return 0;
        }
        else if (a == "--version" || a == "-v")
        {
            printVersion();
            return 0;
        }
        else
            filtered.push_back(a);
    }
    if (filtered.empty())
    {
        printHelp();
        return 0;
    }

    const std::string rawCmd = filtered[0];
    const std::string cmd = resolveAlias(rawCmd);
    std::vector<std::string> rest(filtered.begin() + 1, filtered.end());

    try
    {
        if (cmd == "install")
            return cmdInstall(rest, global);
        if (cmd == "remove" || cmd == "uninstall")
            return cmdRemove(rest, global, yes);
        if (cmd == "list")
            return cmdList(rest.empty() ? "" : rest[0], global);
        if (cmd == "update")
            return cmdUpdate(rest, global);
        if (cmd == "publish")
            return cmdPublish();
        if (cmd == "search")
        {
            if (rest.empty())
            {
                std::cerr << red("Usage: nv+ search <query>") << "\n";
                return 1;
            }
            return cmdSearch(rest[0]);
        }
        if (cmd == "init")
            return cmdInit();
        if (cmd == "login")
            return cmdLogin();
        if (cmd == "alias")
            return cmdAlias(rest);
        if (cmd == "logout")
        {
            if (fs::exists(tokenPath()))
            {
                fs::remove(tokenPath());
                std::cout << green("Logged out.") << "\n";
            }
            else
            {
                std::cout << yellow("Not logged in.") << "\n";
            }
            return 0;
        }
        if (cmd == "whoami")
        {
            std::string token = loadToken();
            if (token.empty())
            {
                std::cerr << red("Not logged in. Run: nv+ login") << "\n";
                return 1;
            }
            std::string username = ghGetUsername(token);
            if (username.empty())
            {
                std::cerr << red("Token invalid. Run: nv+ login") << "\n";
                return 1;
            }
            std::cout << bold(username) << "\n";
            return 0;
        }

        std::cerr << red("Unknown command: " + rawCmd) << "\n";
        printHelp();
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << red(std::string("Error: ") + e.what()) << "\n";
        return 1;
    }
}
