#pragma once
#include "./value.h"

// ── platform detection ────────────────────────────────────────────────────────
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using sock_t = SOCKET;
#  define SOCK_INVALID INVALID_SOCKET
#  define SOCK_ERR     SOCKET_ERROR
#  define sock_close   closesocket
#  define sock_errno   WSAGetLastError()
#  define SOCK_WOULDBLOCK WSAEWOULDBLOCK
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <errno.h>
   using sock_t = int;
#  define SOCK_INVALID (-1)
#  define SOCK_ERR     (-1)
#  define sock_close   ::close
#  define sock_errno   errno
#  define SOCK_WOULDBLOCK EWOULDBLOCK
#endif

#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <sstream>

namespace novac
{

// ════════════════════════════════════════════════════════════════════════════
//  Platform init/cleanup (idempotent)
// ════════════════════════════════════════════════════════════════════════════

inline void sock_platform_init()
{
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2,2), &w);
        done = true;
    }
#endif
}

inline void sock_platform_cleanup()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

// ════════════════════════════════════════════════════════════════════════════
//  NovaSocket  — shared state behind every socket object
// ════════════════════════════════════════════════════════════════════════════

struct NovaSocket
{
    sock_t fd      = SOCK_INVALID;
    bool   tcp     = true;   // false = UDP
    bool   server  = false;
    bool   open    = false;
    bool   nonblocking = false;

    std::string peerHost;
    int         peerPort = 0;
    int         localPort = 0;

    explicit NovaSocket(sock_t f = SOCK_INVALID) : fd(f), open(f != SOCK_INVALID) {}
    ~NovaSocket() { close(); }

    // non-copyable
    NovaSocket(const NovaSocket&) = delete;
    NovaSocket& operator=(const NovaSocket&) = delete;

    void close()
    {
        if (fd != SOCK_INVALID) {
            sock_close(fd);
            fd   = SOCK_INVALID;
            open = false;
        }
    }

    // ── set non-blocking mode ────────────────────────────────────────────────
    void setNonBlocking(bool nb)
    {
#ifdef _WIN32
        u_long mode = nb ? 1 : 0;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        if (nb) flags |= O_NONBLOCK;
        else    flags &= ~O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
#endif
        nonblocking = nb;
    }

    // ── low-level send ───────────────────────────────────────────────────────
    int sendAll(const char* buf, int len)
    {
        int sent = 0;
        while (sent < len) {
            int n = ::send(fd, buf + sent, len - sent, 0);
            if (n <= 0) return n;
            sent += n;
        }
        return sent;
    }

    // ── recv into string ─────────────────────────────────────────────────────
    std::string recvBytes(int maxBytes)
    {
        std::string out;
        out.resize(maxBytes);
        int n = ::recv(fd, &out[0], maxBytes, 0);
        if (n <= 0) { out.clear(); return out; }
        out.resize(n);
        return out;
    }

    // ── recv exactly n bytes ─────────────────────────────────────────────────
    std::string recvExact(int n)
    {
        std::string out;
        out.resize(n);
        int got = 0;
        while (got < n) {
            int r = ::recv(fd, &out[got], n - got, 0);
            if (r <= 0) { out.resize(got); break; }
            got += r;
        }
        return out;
    }

    // ── recv until delimiter ─────────────────────────────────────────────────
    std::string recvUntil(const std::string& delim, int maxBytes = 65536)
    {
        std::string buf;
        buf.reserve(256);
        while ((int)buf.size() < maxBytes) {
            char c;
            int r = ::recv(fd, &c, 1, 0);
            if (r <= 0) break;
            buf += c;
            if (buf.size() >= delim.size() &&
                buf.compare(buf.size() - delim.size(), delim.size(), delim) == 0)
                break;
        }
        return buf;
    }

    // ── wait for readability (milliseconds; -1 = forever) ───────────────────
    bool waitReadable(int ms)
    {
#ifdef _WIN32
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        timeval tv{ ms / 1000, (ms % 1000) * 1000 };
        timeval* tvp = ms < 0 ? nullptr : &tv;
        return select(0, &fds, nullptr, nullptr, tvp) > 0;
#else
        pollfd pfd{ fd, POLLIN, 0 };
        return poll(&pfd, 1, ms) > 0;
#endif
    }

    std::string info() const
    {
        std::ostringstream ss;
        ss << (tcp ? "tcp" : "udp")
           << (server ? ":server" : ":client")
           << "@" << peerHost << ":" << peerPort;
        return ss.str();
    }
};

// ════════════════════════════════════════════════════════════════════════════
//  nova_socket()  — returns a Nova object with the full socket API
//
//  Nova-side usage (TCP client):
//    let s = socket()
//    s.connect("example.com", 80)
//    s.send("GET / HTTP/1.0\r\n\r\n")
//    let resp = s.recvAll()
//    s.close()
//
//  Nova-side usage (TCP server):
//    let srv = socket()
//    srv.bind(8080)
//    srv.listen(10)
//    let client = srv.accept()   // returns a new socket object
//    let data   = client.recv(1024)
//    client.send("pong\n")
//    client.close()
//    srv.close()
//
//  UDP:
//    let s = socket("udp")
//    s.bind(9000)
//    let pkt = s.recvFrom(1024)   // { data, host, port }
//    s.sendTo("hi", "127.0.0.1", 9001)
// ════════════════════════════════════════════════════════════════════════════

inline Val nova_socket_from(std::shared_ptr<NovaSocket> sock);

inline Val nova_socket(const std::string& proto = "tcp")
{
    sock_platform_init();

    bool tcp = (proto != "udp");
    int  type = tcp ? SOCK_STREAM : SOCK_DGRAM;
    sock_t fd = ::socket(AF_INET, type, 0);
    if (fd == SOCK_INVALID)
        throw std::runtime_error("socket(): failed to create socket");

    auto ns = std::make_shared<NovaSocket>(fd);
    ns->tcp = tcp;
    return nova_socket_from(ns);
}

inline Val nova_socket_from(std::shared_ptr<NovaSocket> ns)
{
    Val obj = NovaValue::makeObject();

    // keep the socket alive as a NativeVal inside the object
    Val handle = NovaValue::makeNativeVal(ns);
    obj->obj->set("__handle__", handle);
    obj->obj->set("__type__",   NovaValue::makeString("socket"));

    auto get = [ns]() -> std::shared_ptr<NovaSocket>& { return const_cast<std::shared_ptr<NovaSocket>&>(ns); };

    // ── connect(host, port) ──────────────────────────────────────────────────
    obj->obj->set("connect", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        if (a.size() < 2) throw std::runtime_error("connect(host, port)");
        std::string host = a[0]->asString();
        int         port = (int)a[1]->asNumber();

        addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = ns->tcp ? SOCK_STREAM : SOCK_DGRAM;
        std::string portStr = std::to_string(port);

        int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
        if (rc != 0)
            throw std::runtime_error(std::string("connect: DNS failed: ") + gai_strerror(rc));

        bool ok = false;
        for (auto* p = res; p; p = p->ai_next) {
            if (::connect(ns->fd, p->ai_addr, (int)p->ai_addrlen) == 0) { ok = true; break; }
        }
        freeaddrinfo(res);
        if (!ok) throw std::runtime_error("connect: connection refused");

        ns->peerHost = host;
        ns->peerPort = port;
        ns->open     = true;
        return NovaValue::makeBool(true);
    }, "connect"));

    // ── bind(port [, host]) ──────────────────────────────────────────────────
    obj->obj->set("bind", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        if (a.empty()) throw std::runtime_error("bind(port)");
        int port = (int)a[0]->asNumber();
        std::string host = (a.size() > 1) ? a[1]->asString() : "0.0.0.0";

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((uint16_t)port);
        addr.sin_addr.s_addr = (host == "0.0.0.0") ? INADDR_ANY : inet_addr(host.c_str());

        int yes = 1;
        setsockopt(ns->fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

        if (::bind(ns->fd, (sockaddr*)&addr, sizeof(addr)) == SOCK_ERR)
            throw std::runtime_error("bind: port " + std::to_string(port) + " unavailable");

        ns->localPort = port;
        ns->open = true;
        return NovaValue::makeBool(true);
    }, "bind"));

    // ── listen([backlog]) ────────────────────────────────────────────────────
    obj->obj->set("listen", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        int backlog = a.empty() ? 128 : (int)a[0]->asNumber();
        if (::listen(ns->fd, backlog) == SOCK_ERR)
            throw std::runtime_error("listen: failed");
        ns->server = true;
        return NovaValue::makeBool(true);
    }, "listen"));

    // ── accept() → new socket object ─────────────────────────────────────────
    obj->obj->set("accept", NovaValue::makeNative([ns](ValVec, auto) -> Val {
        sockaddr_in cli{};
        socklen_t   len = sizeof(cli);
        sock_t cfd = ::accept(ns->fd, (sockaddr*)&cli, &len);
        if (cfd == SOCK_INVALID) return NovaValue::makeNull();

        auto cs = std::make_shared<NovaSocket>(cfd);
        cs->tcp      = ns->tcp;
        cs->peerPort = ntohs(cli.sin_port);
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, buf, sizeof(buf));
        cs->peerHost = buf;
        return nova_socket_from(cs);
    }, "accept"));

    // ── send(data) → bytesSent ───────────────────────────────────────────────
    obj->obj->set("send", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        if (a.empty()) return NovaValue::makeNumber(0);
        std::string data = a[0]->asString();
        int n = ns->sendAll(data.c_str(), (int)data.size());
        return NovaValue::makeNumber(n);
    }, "send"));

    // ── sendBytes(array) → bytesSent ────────────────────────────────────────
    obj->obj->set("sendBytes", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        if (a.empty() || !a[0]->isArray()) return NovaValue::makeNumber(0);
        auto& inner = a[0]->arr->inner;
        std::vector<char> buf;
        buf.reserve(inner.size());
        for (auto& v : inner) buf.push_back((char)(int)v->asNumber());
        int n = ns->sendAll(buf.data(), (int)buf.size());
        return NovaValue::makeNumber(n);
    }, "sendBytes"));

    // ── recv(maxBytes) → string ──────────────────────────────────────────────
    obj->obj->set("recv", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        int max = a.empty() ? 4096 : (int)a[0]->asNumber();
        return NovaValue::makeString(ns->recvBytes(max));
    }, "recv"));

    // ── recvExact(n) → string ────────────────────────────────────────────────
    obj->obj->set("recvExact", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        int n = a.empty() ? 1 : (int)a[0]->asNumber();
        return NovaValue::makeString(ns->recvExact(n));
    }, "recvExact"));

    // ── recvUntil(delim [, maxBytes]) → string ───────────────────────────────
    obj->obj->set("recvUntil", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        if (a.empty()) throw std::runtime_error("recvUntil(delim)");
        std::string delim = a[0]->asString();
        int max = (a.size() > 1) ? (int)a[1]->asNumber() : 65536;
        return NovaValue::makeString(ns->recvUntil(delim, max));
    }, "recvUntil"));

    // ── recvLine([maxBytes]) → string  (recvUntil "\n") ─────────────────────
    obj->obj->set("recvLine", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        int max = a.empty() ? 65536 : (int)a[0]->asNumber();
        std::string line = ns->recvUntil("\n", max);
        // strip trailing \r\n
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return NovaValue::makeString(line);
    }, "recvLine"));

    // ── recvAll([chunkSize]) → string ────────────────────────────────────────
    obj->obj->set("recvAll", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        int chunk = a.empty() ? 4096 : (int)a[0]->asNumber();
        std::string out;
        while (true) {
            std::string part = ns->recvBytes(chunk);
            if (part.empty()) break;
            out += part;
        }
        return NovaValue::makeString(out);
    }, "recvAll"));

    // ── recvBytes(maxBytes) → array of numbers ───────────────────────────────
    obj->obj->set("recvBytes", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        int max = a.empty() ? 4096 : (int)a[0]->asNumber();
        std::string raw = ns->recvBytes(max);
        ValVec out;
        out.reserve(raw.size());
        for (unsigned char c : raw)
            out.push_back(NovaValue::makeNumber((double)c));
        return NovaValue::makeArray(std::move(out));
    }, "recvBytes"));

    // ── sendTo(data, host, port) → n  [UDP] ─────────────────────────────────
    obj->obj->set("sendTo", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        if (a.size() < 3) throw std::runtime_error("sendTo(data, host, port)");
        std::string data = a[0]->asString();
        std::string host = a[1]->asString();
        int         port = (int)a[2]->asNumber();

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((uint16_t)port);
        addr.sin_addr.s_addr = inet_addr(host.c_str());

        int n = (int)::sendto(ns->fd, data.c_str(), (int)data.size(), 0,
                              (sockaddr*)&addr, sizeof(addr));
        return NovaValue::makeNumber(n);
    }, "sendTo"));

    // ── recvFrom(maxBytes) → { data, host, port }  [UDP] ────────────────────
    obj->obj->set("recvFrom", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        int max = a.empty() ? 4096 : (int)a[0]->asNumber();
        std::string buf(max, '\0');
        sockaddr_in src{};
        socklen_t   len = sizeof(src);
        int n = (int)::recvfrom(ns->fd, &buf[0], max, 0,
                                (sockaddr*)&src, &len);
        if (n < 0) return NovaValue::makeNull();
        buf.resize(n);

        char hostbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, hostbuf, sizeof(hostbuf));

        ValMap res;
        res["data"] = NovaValue::makeString(buf);
        res["host"] = NovaValue::makeString(hostbuf);
        res["port"] = NovaValue::makeNumber(ntohs(src.sin_port));
        return NovaValue::makeObject(std::move(res));
    }, "recvFrom"));

    // ── wait([ms]) → bool  — wait for data (default: block forever) ─────────
    obj->obj->set("wait", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        int ms = a.empty() ? -1 : (int)a[0]->asNumber();
        return NovaValue::makeBool(ns->waitReadable(ms));
    }, "wait"));

    // ── setNonBlocking(bool) ─────────────────────────────────────────────────
    obj->obj->set("setNonBlocking", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        bool nb = a.empty() ? true : a[0]->asBool();
        ns->setNonBlocking(nb);
        return NovaValue::makeNull();
    }, "setNonBlocking"));

    // ── setOption(opt, value) ────────────────────────────────────────────────
    //   supported: "nodelay", "keepalive", "reuseaddr", "broadcast",
    //              "rcvbuf", "sndbuf", "timeout" (ms, applies to both)
    obj->obj->set("setOption", NovaValue::makeNative([ns](ValVec a, auto) -> Val {
        if (a.size() < 2) throw std::runtime_error("setOption(name, value)");
        std::string opt = a[0]->asString();
        int         val = (int)a[1]->asNumber();

        if (opt == "nodelay") {
            setsockopt(ns->fd, IPPROTO_TCP, TCP_NODELAY, (char*)&val, sizeof(val));
        } else if (opt == "keepalive") {
            setsockopt(ns->fd, SOL_SOCKET, SO_KEEPALIVE, (char*)&val, sizeof(val));
        } else if (opt == "reuseaddr") {
            setsockopt(ns->fd, SOL_SOCKET, SO_REUSEADDR, (char*)&val, sizeof(val));
        } else if (opt == "broadcast") {
            setsockopt(ns->fd, SOL_SOCKET, SO_BROADCAST, (char*)&val, sizeof(val));
        } else if (opt == "rcvbuf") {
            setsockopt(ns->fd, SOL_SOCKET, SO_RCVBUF, (char*)&val, sizeof(val));
        } else if (opt == "sndbuf") {
            setsockopt(ns->fd, SOL_SOCKET, SO_SNDBUF, (char*)&val, sizeof(val));
        } else if (opt == "timeout") {
#ifdef _WIN32
            DWORD ms = val;
            setsockopt(ns->fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&ms, sizeof(ms));
            setsockopt(ns->fd, SOL_SOCKET, SO_SNDTIMEO, (char*)&ms, sizeof(ms));
#else
            timeval tv{ val / 1000, (val % 1000) * 1000 };
            setsockopt(ns->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(ns->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        } else {
            throw std::runtime_error("setOption: unknown option '" + opt + "'");
        }
        return NovaValue::makeNull();
    }, "setOption"));

    // ── resolve(host) → string (IP) ──────────────────────────────────────────
    obj->obj->set("resolve", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty()) throw std::runtime_error("resolve(host)");
        std::string host = a[0]->asString();
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
            return NovaValue::makeNull();
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)res->ai_addr)->sin_addr, buf, sizeof(buf));
        freeaddrinfo(res);
        return NovaValue::makeString(buf);
    }, "resolve"));

    // ── close() ──────────────────────────────────────────────────────────────
    obj->obj->set("close", NovaValue::makeNative([ns](ValVec, auto) -> Val {
        ns->close();
        return NovaValue::makeNull();
    }, "close"));

    // ── isOpen → bool ────────────────────────────────────────────────────────
    obj->obj->set("isOpen", NovaValue::makeNative([ns](ValVec, auto) -> Val {
        return NovaValue::makeBool(ns->open);
    }, "isOpen"));

    // ── info() → string ──────────────────────────────────────────────────────
    obj->obj->set("info", NovaValue::makeNative([ns](ValVec, auto) -> Val {
        return NovaValue::makeString(ns->info());
    }, "info"));

    // ── peer → { host, port } ────────────────────────────────────────────────
    obj->obj->set("peer", NovaValue::makeNative([ns](ValVec, auto) -> Val {
        ValMap m;
        m["host"] = NovaValue::makeString(ns->peerHost);
        m["port"] = NovaValue::makeNumber(ns->peerPort);
        return NovaValue::makeObject(std::move(m));
    }, "peer"));

    // ── localPort → number ───────────────────────────────────────────────────
    obj->obj->set("localPort", NovaValue::makeNative([ns](ValVec, auto) -> Val {
        return NovaValue::makeNumber(ns->localPort);
    }, "localPort"));

    return obj;
}

// ════════════════════════════════════════════════════════════════════════════
//  Register into a scope / object
//
//  In your interpreter setup:
//    #include "socket.h"
//    novac::registerSocketBuiltins(globalScope->vars);
//
//  Nova code can then do:
//    let s = socket()         // TCP
//    let u = socket("udp")    // UDP
// ════════════════════════════════════════════════════════════════════════════

inline void registerSocketBuiltins(NovaObject& obj)
{
    sock_platform_init();
    obj.set("Socket", NovaValue::makeNative([](ValVec a, auto) -> Val {
        std::string proto = a.empty() ? "tcp" : a[0]->asString();
        return nova_socket(proto);
    }, "Socket"));
};
} // namespace novac