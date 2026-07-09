#include "fetch.h"
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <algorithm>

// ── platform socket includes ──────────────────────────────────────────────────

#if defined(_WIN32)
#define SECURITY_WIN32
#endif

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using SocketFd = SOCKET;
    #define INVALID_SOCK INVALID_SOCKET
    #define SOCK_ERR     SOCKET_ERROR
    #define sock_close(s) closesocket(s)
    #define sock_errno()  WSAGetLastError()
#else
    #include <sys/socket.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    using SocketFd = int;
    #define INVALID_SOCK (-1)
    #define SOCK_ERR     (-1)
    #define sock_close(s) ::close(s)
    #define sock_errno()  errno
#endif

// ── TLS via platform APIs ────────────────────────────────────────────────────

#if defined(_WIN32)
    #define NOVA_TLS_SCHANNEL
    #include <schannel.h>
    #include <security.h>
    #pragma comment(lib, "secur32.lib")
#elif defined(__APPLE__)
    #define NOVA_TLS_SECTRANSPORT
    #include <Security/Security.h>
    #include <Security/SecureTransport.h>
#elif defined(__linux__)
    #define NOVA_TLS_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
#endif

namespace novac {

// ════════════════════════════════════════════════════════════════════════════
//  URL parser
// ════════════════════════════════════════════════════════════════════════════

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl p;

    // scheme
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        throw std::runtime_error("Invalid URL: no scheme");
    p.scheme = url.substr(0, schemeEnd);
    std::transform(p.scheme.begin(), p.scheme.end(), p.scheme.begin(), ::tolower);

    std::string rest = url.substr(schemeEnd + 3);

    // host + port
    auto pathStart = rest.find('/');
    std::string hostPort = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);
    p.path = pathStart == std::string::npos ? "/" : rest.substr(pathStart);

    auto colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        p.host = hostPort.substr(0, colon);
        p.port = hostPort.substr(colon + 1);
    } else {
        p.host = hostPort;
        p.port = (p.scheme == "https") ? "443" : "80";
    }

    return p;
}

// ════════════════════════════════════════════════════════════════════════════
//  Socket helpers
// ════════════════════════════════════════════════════════════════════════════

#if defined(_WIN32)
struct WsaInit {
    WsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); }
    ~WsaInit() { WSACleanup(); }
};
static WsaInit _wsaInit;
#endif

static SocketFd connectSocket(const std::string& host, const std::string& port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (err != 0)
        throw std::runtime_error("DNS resolution failed for: " + host);

    SocketFd fd = INVALID_SOCK;
    for (auto* rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == INVALID_SOCK) continue;
        if (connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        sock_close(fd);
        fd = INVALID_SOCK;
    }
    freeaddrinfo(res);

    if (fd == INVALID_SOCK)
        throw std::runtime_error("Could not connect to: " + host + ":" + port);
    return fd;
}

// ════════════════════════════════════════════════════════════════════════════
//  HTTP response parser
// ════════════════════════════════════════════════════════════════════════════

static FetchResponse parseHttpResponse(const std::string& raw) {
    FetchResponse resp;

    auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        throw std::runtime_error("Malformed HTTP response");

    std::string headerSection = raw.substr(0, headerEnd);
    resp.body = raw.substr(headerEnd + 4);

    // status line
    auto firstLine = headerSection.find("\r\n");
    std::string statusLine = headerSection.substr(0, firstLine);
    // "HTTP/1.1 200 OK"
    auto sp1 = statusLine.find(' ');
    if (sp1 != std::string::npos) {
        auto sp2 = statusLine.find(' ', sp1 + 1);
        resp.status = std::stoi(statusLine.substr(sp1 + 1, sp2 - sp1 - 1));
        if (sp2 != std::string::npos)
            resp.statusText = statusLine.substr(sp2 + 1);
    }
    resp.ok = resp.status >= 200 && resp.status < 300;

    // headers
    std::istringstream hss(headerSection.substr(firstLine + 2));
    std::string line;
    while (std::getline(hss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // trim
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        resp.headers[key] = val;
    }

    // handle chunked transfer encoding
    auto te = resp.headers.find("transfer-encoding");
    if (te != resp.headers.end() && te->second.find("chunked") != std::string::npos) {
        std::string decoded;
        std::istringstream css(resp.body);
        std::string sizeLine;
        while (std::getline(css, sizeLine)) {
            if (!sizeLine.empty() && sizeLine.back() == '\r') sizeLine.pop_back();
            if (sizeLine.empty()) continue;
            size_t chunkSize = std::stoul(sizeLine, nullptr, 16);
            if (chunkSize == 0) break;
            std::string chunk(chunkSize, '\0');
            css.read(&chunk[0], chunkSize);
            decoded += chunk;
            css.ignore(2); // \r\n
        }
        resp.body = decoded;
    }

    return resp;
}

// ════════════════════════════════════════════════════════════════════════════
//  HTTP (plain)
// ════════════════════════════════════════════════════════════════════════════

static FetchResponse doHttp(const ParsedUrl& url, const FetchRequest& req) {
    SocketFd fd = connectSocket(url.host, url.port);

    // build request
    std::ostringstream r;
    r << req.method << " " << url.path << " HTTP/1.1\r\n";
    r << "Host: " << url.host << "\r\n";
    r << "Connection: close\r\n";
    r << "User-Agent: novac/0.1\r\n";
    for (auto& [k, v] : req.headers)
        r << k << ": " << v << "\r\n";
    if (!req.body.empty()) {
        r << "Content-Length: " << req.body.size() << "\r\n";
        if (req.headers.find("content-type") == req.headers.end())
            r << "Content-Type: application/json\r\n";
    }
    r << "\r\n";
    if (!req.body.empty()) r << req.body;

    std::string raw = r.str();
    int sent = send(fd, raw.c_str(), (int)raw.size(), 0);
    if (sent == SOCK_ERR) {
        sock_close(fd);
        throw std::runtime_error("Failed to send HTTP request");
    }

    // receive
    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
        response.append(buf, n);
    sock_close(fd);

    return parseHttpResponse(response);
}

// ════════════════════════════════════════════════════════════════════════════
//  HTTPS
// ════════════════════════════════════════════════════════════════════════════

#if defined(NOVA_TLS_OPENSSL)

static FetchResponse doHttpsOpenSSL(const ParsedUrl& url, const FetchRequest& req) {
    static bool sslInit = false;
    if (!sslInit) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        sslInit = true;
    }

    SocketFd fd = connectSocket(url.host, url.port);

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) throw std::runtime_error("SSL_CTX_new failed");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ctx);

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, (int)fd);
    SSL_set_tlsext_host_name(ssl, url.host.c_str());

    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl); SSL_CTX_free(ctx); sock_close(fd);
        throw std::runtime_error("TLS handshake failed");
    }

    // build request
    std::ostringstream r;
    r << req.method << " " << url.path << " HTTP/1.1\r\n";
    r << "Host: " << url.host << "\r\n";
    r << "Connection: close\r\n";
    r << "User-Agent: novac/0.1\r\n";
    for (auto& [k, v] : req.headers)
        r << k << ": " << v << "\r\n";
    if (!req.body.empty()) {
        r << "Content-Length: " << req.body.size() << "\r\n";
        if (req.headers.find("content-type") == req.headers.end())
            r << "Content-Type: application/json\r\n";
    }
    r << "\r\n";
    if (!req.body.empty()) r << req.body;

    std::string raw = r.str();
    SSL_write(ssl, raw.c_str(), (int)raw.size());

    std::string response;
    char buf[4096];
    int n;
    while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0)
        response.append(buf, n);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    sock_close(fd);

    return parseHttpResponse(response);
}

#elif defined(NOVA_TLS_SCHANNEL)

static FetchResponse doHttpsSchannel(const ParsedUrl& url, const FetchRequest& req) {
    SocketFd fd = connectSocket(url.host, url.port);

    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags   = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

    CredHandle hCred;
    TimeStamp  tsExpiry;
    SECURITY_STATUS ss = AcquireCredentialsHandleA(
        nullptr, const_cast<char*>(UNISP_NAME_A),
        SECPKG_CRED_OUTBOUND, nullptr, &cred,
        nullptr, nullptr, &hCred, &tsExpiry);
    if (ss != SEC_E_OK) {
        sock_close(fd);
        throw std::runtime_error("AcquireCredentialsHandle failed");
    }

    CtxtHandle hCtx;
    bool hCtxInit = false;

    // TLS handshake loop
    SecBuffer outBufs[1];
    SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, outBufs };
    outBufs[0] = { 0, SECBUFFER_TOKEN, nullptr };

    DWORD reqFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                     ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
                     ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    std::string hostCopy = url.host;
    ss = InitializeSecurityContextA(
        &hCred, nullptr, const_cast<char*>(hostCopy.c_str()),
        reqFlags, 0, 0, nullptr, 0,
        &hCtx, &outDesc, &reqFlags, &tsExpiry);

    while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE ||
           ss == SEC_I_INCOMPLETE_CREDENTIALS) {

        if (outBufs[0].pvBuffer && outBufs[0].cbBuffer) {
            send(fd, (char*)outBufs[0].pvBuffer, outBufs[0].cbBuffer, 0);
            FreeContextBuffer(outBufs[0].pvBuffer);
            outBufs[0].pvBuffer = nullptr;
            outBufs[0].cbBuffer = 0;
        }

        char inData[16384];
        int  inLen = recv(fd, inData, sizeof(inData), 0);
        if (inLen <= 0) break;

        SecBuffer inBufs[2];
        SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, inBufs };
        inBufs[0] = { (ULONG)inLen, SECBUFFER_TOKEN, inData };
        inBufs[1] = { 0, SECBUFFER_EMPTY, nullptr };

        outBufs[0] = { 0, SECBUFFER_TOKEN, nullptr };
        outDesc    = { SECBUFFER_VERSION, 1, outBufs };

        ss = InitializeSecurityContextA(
            &hCred, &hCtx, nullptr,
            reqFlags, 0, 0, &inDesc, 0,
            nullptr, &outDesc, &reqFlags, &tsExpiry);
        hCtxInit = true;
    }

    if (ss != SEC_E_OK && ss != SEC_I_CONTEXT_EXPIRED) {
        if (hCtxInit) DeleteSecurityContext(&hCtx);
        FreeCredentialsHandle(&hCred);
        sock_close(fd);
        throw std::runtime_error("TLS handshake failed (SChannel)");
    }

    // get stream sizes
    SecPkgContext_StreamSizes sizes;
    QueryContextAttributes(&hCtx, SECPKG_ATTR_STREAM_SIZES, &sizes);

    // build and encrypt request
    std::ostringstream r;
    r << req.method << " " << url.path << " HTTP/1.1\r\n";
    r << "Host: " << url.host << "\r\n";
    r << "Connection: close\r\n";
    r << "User-Agent: novac/0.1\r\n";
    for (auto& [k, v] : req.headers)
        r << k << ": " << v << "\r\n";
    if (!req.body.empty()) {
        r << "Content-Length: " << req.body.size() << "\r\n";
        if (req.headers.find("content-type") == req.headers.end())
            r << "Content-Type: application/json\r\n";
    }
    r << "\r\n";
    if (!req.body.empty()) r << req.body;
    std::string plainReq = r.str();

    // encrypt in chunks
    size_t offset = 0;
    while (offset < plainReq.size()) {
        size_t chunkSize = std::min((size_t)sizes.cbMaximumMessage, plainReq.size() - offset);
        std::vector<char> msgBuf(sizes.cbHeader + chunkSize + sizes.cbTrailer);

        SecBuffer encBufs[4];
        encBufs[0] = { sizes.cbHeader,  SECBUFFER_STREAM_HEADER,  msgBuf.data() };
        encBufs[1] = { (ULONG)chunkSize, SECBUFFER_DATA, msgBuf.data() + sizes.cbHeader };
        encBufs[2] = { sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, msgBuf.data() + sizes.cbHeader + chunkSize };
        encBufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
        memcpy(encBufs[1].pvBuffer, plainReq.c_str() + offset, chunkSize);

        SecBufferDesc encDesc{ SECBUFFER_VERSION, 4, encBufs };
        EncryptMessage(&hCtx, 0, &encDesc, 0);

        size_t totalSize = encBufs[0].cbBuffer + encBufs[1].cbBuffer + encBufs[2].cbBuffer;
        send(fd, msgBuf.data(), (int)totalSize, 0);
        offset += chunkSize;
    }

    // receive and decrypt
    std::string response;
    std::vector<char> recvBuf;
    char tmp[16384];
    int n;

    while ((n = recv(fd, tmp, sizeof(tmp), 0)) > 0) {
        recvBuf.insert(recvBuf.end(), tmp, tmp + n);

        while (!recvBuf.empty()) {
            SecBuffer decBufs[4];
            decBufs[0] = { (ULONG)recvBuf.size(), SECBUFFER_DATA, recvBuf.data() };
            decBufs[1] = { 0, SECBUFFER_EMPTY, nullptr };
            decBufs[2] = { 0, SECBUFFER_EMPTY, nullptr };
            decBufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
            SecBufferDesc decDesc{ SECBUFFER_VERSION, 4, decBufs };

            ss = DecryptMessage(&hCtx, &decDesc, 0, nullptr);
            if (ss == SEC_E_INCOMPLETE_MESSAGE) break;
            if (ss == SEC_I_CONTEXT_EXPIRED)    { goto done; }
            if (ss != SEC_E_OK)                 break;

            for (int i = 0; i < 4; i++)
                if (decBufs[i].BufferType == SECBUFFER_DATA)
                    response.append((char*)decBufs[i].pvBuffer, decBufs[i].cbBuffer);

            // leftover bytes
            std::vector<char> leftover;
            for (int i = 0; i < 4; i++)
                if (decBufs[i].BufferType == SECBUFFER_EXTRA)
                    leftover.insert(leftover.end(),
                        (char*)decBufs[i].pvBuffer,
                        (char*)decBufs[i].pvBuffer + decBufs[i].cbBuffer);
            recvBuf = leftover;
        }
    }
    done:

    DeleteSecurityContext(&hCtx);
    FreeCredentialsHandle(&hCred);
    sock_close(fd);

    return parseHttpResponse(response);
}

#elif defined(NOVA_TLS_SECTRANSPORT)

static FetchResponse doHttpsSecTransport(const ParsedUrl& url, const FetchRequest& req) {
    SocketFd fd = connectSocket(url.host, url.port);

    SSLContextRef ctx = SSLCreateContext(nullptr, kSSLClientSide, kSSLStreamType);
    if (!ctx) { sock_close(fd); throw std::runtime_error("SSLCreateContext failed"); }

    SSLSetIOFuncs(ctx,
        [](SSLConnectionRef conn, void* data, size_t* len) -> OSStatus {
            int fd = *(int*)conn;
            ssize_t n = recv(fd, data, *len, 0);
            if (n > 0) { *len = n; return noErr; }
            *len = 0;
            return n == 0 ? errSSLClosedGraceful : errSSLWouldBlock;
        },
        [](SSLConnectionRef conn, const void* data, size_t* len) -> OSStatus {
            int fd = *(int*)conn;
            ssize_t n = send(fd, data, *len, 0);
            if (n > 0) { *len = n; return noErr; }
            *len = 0; return errSSLWouldBlock;
        });

    SSLSetConnection(ctx, &fd);
    SSLSetPeerDomainName(ctx, url.host.c_str(), url.host.size());

    OSStatus hs;
    do { hs = SSLHandshake(ctx); } while (hs == errSSLWouldBlock);
    if (hs != noErr) {
        CFRelease(ctx); sock_close(fd);
        throw std::runtime_error("TLS handshake failed (SecureTransport)");
    }

    // build request
    std::ostringstream r;
    r << req.method << " " << url.path << " HTTP/1.1\r\n";
    r << "Host: " << url.host << "\r\n";
    r << "Connection: close\r\n";
    r << "User-Agent: novac/0.1\r\n";
    for (auto& [k, v] : req.headers)
        r << k << ": " << v << "\r\n";
    if (!req.body.empty()) {
        r << "Content-Length: " << req.body.size() << "\r\n";
        if (req.headers.find("content-type") == req.headers.end())
            r << "Content-Type: application/json\r\n";
    }
    r << "\r\n";
    if (!req.body.empty()) r << req.body;
    std::string raw = r.str();

    size_t written = 0;
    SSLWrite(ctx, raw.c_str(), raw.size(), &written);

    std::string response;
    char buf[4096];
    size_t processed = 0;
    OSStatus readStatus;
    do {
        readStatus = SSLRead(ctx, buf, sizeof(buf), &processed);
        if (processed > 0) response.append(buf, processed);
    } while (readStatus == noErr || readStatus == errSSLWouldBlock);

    SSLClose(ctx);
    CFRelease(ctx);
    sock_close(fd);

    return parseHttpResponse(response);
}

#endif

// ════════════════════════════════════════════════════════════════════════════
//  Public entry point
// ════════════════════════════════════════════════════════════════════════════

FetchResponse syncFetch(const FetchRequest& req) {
    ParsedUrl url = parseUrl(req.url);

    if (url.scheme == "https") {
#if defined(NOVA_TLS_OPENSSL)
        return doHttpsOpenSSL(url, req);
#elif defined(NOVA_TLS_SCHANNEL)
        return doHttpsSchannel(url, req);
#elif defined(NOVA_TLS_SECTRANSPORT)
        return doHttpsSecTransport(url, req);
#else
        throw std::runtime_error("HTTPS not supported on this platform");
#endif
    }
    return doHttp(url, req);
}

} // namespace novac