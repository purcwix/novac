#include "bstd.h"
#include "../vm/vm.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <random>

#if defined(_WIN32)
#  include <windows.h>
#  include <conio.h>
#  include <io.h>
#  include <stdlib.h>  // _putenv_s
   // Windows doesn't have setenv — provide it
   static inline int setenv(const char* name, const char* value, int) {
       return _putenv_s(name, value);
   }
#  define isatty _isatty
#  ifndef STDIN_FILENO
#    define STDIN_FILENO _fileno(stdin)
#  endif
#else
#  include <termios.h>
#  include <unistd.h>
#  include <sys/select.h>
#endif

namespace novac {
namespace fs = std::filesystem;

// ════════════════════════════════════════════════════════════════════════════
//  History
// ════════════════════════════════════════════════════════════════════════════

static const std::string HISTORY_PREFIX = ".nova.history.";
static std::unordered_map<std::string, std::vector<std::string>> _histCache;

namespace History {

std::vector<std::string> load(const std::string& name) {
    auto it = _histCache.find(name);
    if (it != _histCache.end()) return it->second;

    std::string filepath = HISTORY_PREFIX + name;
    std::vector<std::string> arr;
    if (fs::exists(filepath)) {
        std::ifstream f(filepath);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) arr.push_back(line);
        }
    }
    _histCache[name] = arr;
    return arr;
}

void save(const std::string& name, const std::vector<std::string>& arr) {
    _histCache[name] = arr;
    std::string filepath = HISTORY_PREFIX + name;
    std::ofstream f(filepath);
    for (auto& e : arr) f << e << "\n";
}

void push(const std::string& name, const std::string& entry) {
    if (entry.empty() || std::all_of(entry.begin(), entry.end(), ::isspace)) return;
    auto arr = load(name);
    auto it = std::find(arr.rbegin(), arr.rend(), entry);
    if (it != arr.rend()) arr.erase((it+1).base());
    arr.push_back(entry);
    if (arr.size() > 500) arr.erase(arr.begin(), arr.begin() + (arr.size() - 500));
    save(name, arr);
}

void clear(const std::string& name) { save(name, {}); }

std::vector<std::string> keys() {
    std::vector<std::string> ks;
    try {
        for (auto& e : fs::directory_iterator(".")) {
            std::string fn = e.path().filename().string();
            if (fn.rfind(HISTORY_PREFIX, 0) == 0)
                ks.push_back(fn.substr(HISTORY_PREFIX.size()));
        }
    } catch (...) {}
    return ks;
}

bool exists(const std::string& name) { return fs::exists(HISTORY_PREFIX + name); }

Val makeNovaObj() {
    auto obj = nova_obj();
    obj->obj->set("load", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty()) return nova_arr();
        auto arr = load(a[0].asString());
        ValVec out; for (auto& s : arr) out.push_back(nova_str(s));
        return nova_arr(out);
    }, "load"));
    obj->obj->set("save", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_null();
        std::vector<std::string> arr;
        if (a[1].isArray()) for (auto& v : a[1]->arr->inner) arr.push_back(v.asString());
        save(a[0].asString(), arr);
        return nova_null();
    }, "save"));
    obj->obj->set("push", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() >= 2) push(a[0].asString(), a[1].asString());
        return nova_null();
    }, "push"));
    obj->obj->set("clear", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (!a.empty()) clear(a[0].asString());
        return nova_null();
    }, "clear"));
    obj->obj->set("keys", NovaValue::makeNative([](ValVec, auto) -> Val {
        ValVec out; for (auto& k : keys()) out.push_back(nova_str(k));
        return nova_arr(out);
    }, "keys"));
    return obj;
}

} // namespace History

// ════════════════════════════════════════════════════════════════════════════
//  Storage
// ════════════════════════════════════════════════════════════════════════════

namespace Storage {

static std::string storageDir() {
    std::string d = ".nova_storage";
    fs::create_directories(d);
    return d;
}

void setItem(const std::string& key, const std::string& value) {
    std::string path = storageDir() + "/" + key;
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path); f << value;
}

std::string getItem(const std::string& key) {
    std::string path = storageDir() + "/" + key;
    if (!fs::exists(path)) return "";
    std::ifstream f(path);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void removeItem(const std::string& key) {
    std::string path = storageDir() + "/" + key;
    if (fs::exists(path)) fs::remove(path);
}

void clearAll() {
    std::string d = storageDir();
    if (fs::exists(d)) fs::remove_all(d);
}

Val makeNovaObj() {
    auto obj = nova_obj();
    obj->obj->set("setItem", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() >= 2) setItem(a[0].asString(), a[1].asString());
        return nova_null();
    }, "setItem"));
    obj->obj->set("getItem", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        auto v = getItem(a[0].asString());
        return v.empty() ? nova_null() : nova_str(v);
    }, "getItem"));
    obj->obj->set("removeItem", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (!a.empty()) removeItem(a[0].asString());
        return nova_null();
    }, "removeItem"));
    obj->obj->set("clear", NovaValue::makeNative([](ValVec, auto) -> Val {
        clearAll(); return nova_null();
    }, "clear"));
    return obj;
}

} // namespace Storage

// ════════════════════════════════════════════════════════════════════════════
//  Crypto — simple hash via djb2 + platform SHA if available
// ════════════════════════════════════════════════════════════════════════════

namespace Crypto {

// djb2 — very fast non-cryptographic hash, used as fallback
static std::string djb2(const std::string& data) {
    unsigned long hash = 5381;
    for (unsigned char c : data) hash = ((hash << 5) + hash) + c;
    std::ostringstream ss;
    ss << std::hex << hash;
    return ss.str();
}

std::string hash(const std::string& data, const std::string& algo) {
    // Try platform sha256 via system command
    std::string cmd;
#if defined(_WIN32)
    cmd = "echo -n \"" + data + "\" | certutil -hashfile - SHA256 2>nul";
#elif defined(__APPLE__)
    cmd = "echo -n \"" + data + "\" | shasum -a 256 | cut -d' ' -f1";
#else
    cmd = "echo -n \"" + data + "\" | sha256sum | cut -d' ' -f1";
#endif
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[128]; std::string result;
        while (fgets(buf, sizeof(buf), pipe)) result += buf;
        pclose(pipe);
        // trim
        result.erase(result.find_last_not_of(" \t\r\n") + 1);
        if (!result.empty() && result.size() >= 32) return result.substr(0, 64);
    }
    return djb2(data);
}

std::string hmac(const std::string& key, const std::string& data, const std::string&) {
    // Simplified HMAC: hash(key + data)
    return hash(key + data);
}

Val makeNovaObj() {
    auto obj = nova_obj();
    obj->obj->set("hash", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty()) return nova_str("");
        std::string algo = a.size() > 1 ? a[1].asString() : "sha256";
        return nova_str(hash(a[0].asString(), algo));
    }, "hash"));
    obj->obj->set("hmac", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_str("");
        return nova_str(hmac(a[0].asString(), a[1].asString()));
    }, "hmac"));
    obj->obj->set("randomBytes", NovaValue::makeNative([](ValVec a, auto) -> Val {
        int n = a.empty() ? 16 : (int)a[0].asNumber();
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 255);
        std::string out;
        for (int i = 0; i < n; i++) {
            char buf[3]; snprintf(buf, sizeof(buf), "%02x", dist(rng));
            out += buf;
        }
        return nova_str(out);
    }, "randomBytes"));
    return obj;
}

} // namespace Crypto

// ════════════════════════════════════════════════════════════════════════════
//  Base64
// ════════════════════════════════════════════════════════════════════════════

namespace Base64 {

static const char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode(const std::string& in) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) { out += TABLE[(val >> valb) & 0x3F]; valb -= 6; }
    }
    if (valb > -6) out += TABLE[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

std::string decode(const std::string& in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(unsigned char)TABLE[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) { out += (char)((val >> valb) & 0xFF); valb -= 8; }
    }
    return out;
}

Val makeNovaObj() {
    auto obj = nova_obj();
    obj->obj->set("encode", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return a.empty() ? nova_str("") : nova_str(encode(a[0].asString()));
    }, "encode"));
    obj->obj->set("decode", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return a.empty() ? nova_str("") : nova_str(decode(a[0].asString()));
    }, "decode"));
    return obj;
}

} // namespace Base64

// ════════════════════════════════════════════════════════════════════════════
//  PathUtils
// ════════════════════════════════════════════════════════════════════════════

namespace PathUtils {

std::string join(const std::vector<std::string>& segs) {
    fs::path p;
    for (auto& s : segs) p /= s;
    return p.string();
}

std::string resolve(const std::vector<std::string>& segs) {
    fs::path p = fs::current_path();
    for (auto& s : segs) p /= s;
    return fs::weakly_canonical(p).string();
}

std::string basename(const std::string& p, const std::string& ext) {
    fs::path fp(p);
    if (!ext.empty() && fp.extension() == ext) return fp.stem().string();
    return fp.filename().string();
}

std::string dirname(const std::string& p) {
    return fs::path(p).parent_path().string();
}

std::string extname(const std::string& p) {
    return fs::path(p).extension().string();
}

Val makeNovaObj() {
    auto obj = nova_obj();
    obj->obj->set("join", NovaValue::makeNative([](ValVec a, auto) -> Val {
        std::vector<std::string> segs;
        for (auto& v : a) segs.push_back(v.asString());
        return nova_str(join(segs));
    }, "join"));
    obj->obj->set("resolve", NovaValue::makeNative([](ValVec a, auto) -> Val {
        std::vector<std::string> segs;
        for (auto& v : a) segs.push_back(v.asString());
        return nova_str(resolve(segs));
    }, "resolve"));
    obj->obj->set("basename", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty()) return nova_str("");
        std::string ext = a.size() > 1 ? a[1].asString() : "";
        return nova_str(basename(a[0].asString(), ext));
    }, "basename"));
    obj->obj->set("dirname", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return a.empty() ? nova_str("") : nova_str(dirname(a[0].asString()));
    }, "dirname"));
    obj->obj->set("extname", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return a.empty() ? nova_str("") : nova_str(extname(a[0].asString()));
    }, "extname"));
    obj->obj->set("sep", nova_str(
#if defined(_WIN32)
        "\\"
#else
        "/"
#endif
    ));
    obj->obj->set("starter", nova_str(
#if defined(_WIN32)
        "C:"
#else
        "/"
#endif
    ));
    return obj;
}

} // namespace PathUtils

// ════════════════════════════════════════════════════════════════════════════
//  OsUtils
// ════════════════════════════════════════════════════════════════════════════

namespace OsUtils {

std::string platform() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

std::string homedir() {
    const char* h = std::getenv("HOME");
    if (!h) h = std::getenv("USERPROFILE");
    return h ? h : ".";
}

std::string tmpdir() {
    const char* t = std::getenv("TMPDIR");
    if (!t) t = std::getenv("TEMP");
    if (!t) t = "/tmp";
    return t;
}

uint64_t totalmem() { return 0; } // platform-specific
uint64_t freemem()  { return 0; }

Val makeNovaObj() {
    auto obj = nova_obj();
    obj->obj->set("platform", nova_str(platform()));
    obj->obj->set("homedir",  NovaValue::makeNative([](ValVec, auto){ return nova_str(homedir()); }, "homedir"));
    obj->obj->set("tmpdir",   NovaValue::makeNative([](ValVec, auto){ return nova_str(tmpdir()); }, "tmpdir"));
    obj->obj->set("totalmem", NovaValue::makeNative([](ValVec, auto){ return nova_num(0); }, "totalmem"));
    obj->obj->set("freemem",  NovaValue::makeNative([](ValVec, auto){ return nova_num(0); }, "freemem"));
    obj->obj->set("uptime",   NovaValue::makeNative([](ValVec, auto) -> Val {
        // process uptime in seconds
        static auto start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        return nova_num(std::chrono::duration<double>(now - start).count());
    }, "uptime"));
    obj->obj->set("openFile", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        std::string path = a[0].asString();
#if defined(_WIN32)
        std::string cmd = "start \"\" \"" + path + "\"";
#elif defined(__APPLE__)
        std::string cmd = "open \"" + path + "\"";
#else
        std::string cmd = "xdg-open \"" + path + "\" &";
#endif
        system(cmd.c_str());
        return nova_null();
    }, "openFile"));
    return obj;
}

} // namespace OsUtils

// ════════════════════════════════════════════════════════════════════════════
//  UUID
// ════════════════════════════════════════════════════════════════════════════

namespace UUID {

std::string v4() {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> d(0, 15);
    std::uniform_int_distribution<int> d8(8, 11);
    std::ostringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8;  i++) ss << d(rng);  ss << '-';
    for (int i = 0; i < 4;  i++) ss << d(rng);  ss << '-';
    ss << '4';
    for (int i = 0; i < 3;  i++) ss << d(rng);  ss << '-';
    ss << d8(rng);
    for (int i = 0; i < 3;  i++) ss << d(rng);  ss << '-';
    for (int i = 0; i < 12; i++) ss << d(rng);
    return ss.str();
}

std::string shortId() {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> d(0, 15);
    std::ostringstream ss; ss << std::hex;
    for (int i = 0; i < 8; i++) ss << d(rng);
    return ss.str();
}

bool is(const std::string& s) {
    if (s.size() != 36) return false;
    static const std::string fmt = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (size_t i = 0; i < 36; i++) {
        if (fmt[i] == '-') { if (s[i] != '-') return false; }
        else if (fmt[i] == '4') { if (s[i] != '4') return false; }
        else if (fmt[i] == 'y') { if (std::string("89ab").find(s[i]) == std::string::npos) return false; }
        else { if (!std::isxdigit(s[i])) return false; }
    }
    return true;
}

Val makeNovaObj() {
    auto obj = nova_obj();
    obj->obj->set("v4",      NovaValue::makeNative([](ValVec, auto){ return nova_str(v4()); }, "v4"));
    obj->obj->set("short",   NovaValue::makeNative([](ValVec, auto){ return nova_str(shortId()); }, "short"));
    obj->obj->set("is",      NovaValue::makeNative([](ValVec a, auto){ return nova_bool(!a.empty() && is(a[0].asString())); }, "is"));
    return obj;
}

} // namespace UUID

// ════════════════════════════════════════════════════════════════════════════
//  Deep
// ════════════════════════════════════════════════════════════════════════════

namespace Deep {

Val clone(Val v) {
    if (!v) return nova_null();
    switch (v->kind) {
        case VK::Null:   return nova_null();
        case VK::Bool:   return nova_bool(v->bval);
        case VK::Number: return nova_num(v->nval);
        case VK::String: return nova_str(v->sval);
        case VK::Array: {
            ValVec out;
            for (auto& e : v->arr->inner) out.push_back(clone(e));
            return nova_arr(std::move(out));
        }
        case VK::Object: {
            ValMap out;
            for (auto& [k, val] : v->obj->inner) out[k] = clone(val);
            return nova_obj(std::move(out));
        }
        case VK::Struct: {
            ValMap out;
            for (auto& [k, val] : v->strct->inner) out[k] = clone(val);
            return NovaValue::makeStruct(v->strct->typeName, std::move(out));
        }
        default: return v;
    }
}

bool equal(Val a, Val b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case VK::Null:   return true;
        case VK::Bool:   return a->bval == b->bval;
        case VK::Number: return a->nval == b->nval;
        case VK::String: return a->sval == b->sval;
        case VK::Array: {
            if (a->arr->inner.size() != b->arr->inner.size()) return false;
            for (size_t i = 0; i < a->arr->inner.size(); i++)
                if (!equal(a->arr->inner[i], b->arr->inner[i])) return false;
            return true;
        }
        case VK::Object: {
            if (a->obj->inner.size() != b->obj->inner.size()) return false;
            for (auto& [k, v] : a->obj->inner) {
                auto it = b->obj->inner.find(k);
                if (it == b->obj->inner.end()) return false;
                if (!equal(v, it->second)) return false;
            }
            return true;
        }
        default: return a.get() == b.get();
    }
}

Val merge(Val target, Val source) {
    if (!target || !target.isObject()) return target;
    if (!source || !source.isObject()) return target;
    for (auto& [k, v] : source->obj->inner) {
        auto existing = target->obj->inner.find(k);
        if (existing != target->obj->inner.end() &&
            existing->second && existing->second.isObject() &&
            v && v.isObject()) {
            merge(existing->second, v);
        } else {
            target->obj->set(k, clone(v));
        }
    }
    return target;
}

Val makeNovaObj() {
    auto obj = nova_obj();
    obj->obj->set("clone", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return a.empty() ? nova_null() : clone(a[0]);
    }, "clone"));
    obj->obj->set("equal", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return nova_bool(a.size() >= 2 && equal(a[0], a[1]));
    }, "equal"));
    obj->obj->set("merge", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() < 2) return a.empty() ? nova_null() : a[0];
        Val target = a[0];
        for (size_t i = 1; i < a.size(); i++) target = merge(target, a[i]);
        return target;
    }, "merge"));
    return obj;
}

} // namespace Deep

// ════════════════════════════════════════════════════════════════════════════
//  Timers
// ════════════════════════════════════════════════════════════════════════════

namespace Timers {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

Val makeNovaObj(Executor& exe) {
    auto obj = nova_obj();

    obj->obj->set("delay", NovaValue::makeNative([](ValVec a, auto) -> Val {
        // Synchronous sleep — in async context the scheduler would yield instead
        if (!a.empty()) sleepMs((int)a[0].asNumber());
        return nova_null();
    }, "delay"));

    obj->obj->set("debounce", NovaValue::makeNative([&exe](ValVec a, auto cs) -> Val {
        if (a.size() < 2 || !a[0].isFunction()) return nova_null();
        Val fn = a[0];
        int ms = (int)a[1].asNumber();
        auto last  = std::make_shared<std::chrono::steady_clock::time_point>();
        auto timer = std::make_shared<bool>(false);
        return NovaValue::makeNative([fn, ms, last, &exe](ValVec args, auto cs2) -> Val {
            *last = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *last).count() >= ms)
                return exe.callFunction(fn, args, cs2);
            return nova_null();
        }, "debounced");
    }, "debounce"));

    obj->obj->set("throttle", NovaValue::makeNative([&exe](ValVec a, auto cs) -> Val {
        if (a.size() < 2 || !a[0].isFunction()) return nova_null();
        Val fn  = a[0];
        int ms  = (int)a[1].asNumber();
        auto last = std::make_shared<std::chrono::steady_clock::time_point>();
        return NovaValue::makeNative([fn, ms, last, &exe](ValVec args, auto cs2) -> Val {
            auto now = std::chrono::steady_clock::now();
            int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - *last).count();
            if (elapsed >= ms) { *last = now; return exe.callFunction(fn, args, cs2); }
            return nova_null();
        }, "throttled");
    }, "throttle"));

    return obj;
}

} // namespace Timers

// ════════════════════════════════════════════════════════════════════════════
//  Env
// ════════════════════════════════════════════════════════════════════════════

namespace Env {

static std::unordered_map<std::string,std::string> _envCache;
static bool _loaded = false;

std::unordered_map<std::string,std::string> load(const std::string& filepath) {
    std::string path = filepath.empty() ? ".env" : filepath;
    _envCache.clear();
    if (!fs::exists(path)) return {};
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        auto trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.empty() || trimmed[0] == '#') continue;
        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trimmed.substr(0, eq);
        std::string v = trimmed.substr(eq+1);
        k.erase(k.find_last_not_of(" \t") + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        if (v.size() >= 2 &&
            ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
            v = v.substr(1, v.size()-2);
        _envCache[k] = v;
        setenv(k.c_str(), v.c_str(), 1);
    }
    _loaded = true;
    return _envCache;
}

std::string get(const std::string& key) {
    if (!_loaded) load();
    auto it = _envCache.find(key);
    if (it != _envCache.end()) return it->second;
    const char* ev = std::getenv(key.c_str());
    return ev ? ev : "";
}

void set(const std::string& key, const std::string& value) {
    _envCache[key] = value;
    setenv(key.c_str(), value.c_str(), 1);
}

Val makeNovaObj() {
    auto fn = NovaValue::makeNative([](ValVec a, auto) -> Val {
        std::string path = a.empty() ? "" : a[0].asString();
        auto m = load(path);
        auto obj = nova_obj();
        for (auto& [k,v] : m) obj->obj->set(k, nova_str(v));
        return obj;
    }, "env");

    // Attach .get and .set directly
    fn->fn->native = [](ValVec a, std::shared_ptr<Scope>) -> Val {
        std::string path = a.empty() ? "" : a[0].asString();
        auto m = load(path);
        auto obj = nova_obj();
        for (auto& [k,v] : m) obj->obj->set(k, nova_str(v));
        return obj;
    };

    // Return a callable object with get/set/all
    auto obj = nova_obj();
    obj->obj->set("__call__", fn);
    obj->obj->set("get", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return a.empty() ? nova_null() : nova_str(get(a[0].asString()));
    }, "get"));
    obj->obj->set("set", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() >= 2) set(a[0].asString(), a[1].asString());
        return nova_null();
    }, "set"));
    obj->obj->set("all", NovaValue::makeNative([](ValVec, auto) -> Val {
        if (!_loaded) load();
        auto out = nova_obj();
        for (auto& [k,v] : _envCache) out->obj->set(k, nova_str(v));
        return out;
    }, "all"));
    obj->obj->set("load", NovaValue::makeNative([](ValVec a, auto) -> Val {
        std::string path = a.empty() ? "" : a[0].asString();
        auto m = load(path);
        auto out = nova_obj();
        for (auto& [k,v] : m) out->obj->set(k, nova_str(v));
        return out;
    }, "load"));
    return obj;
}

} // namespace Env

// ════════════════════════════════════════════════════════════════════════════
//  SyntaxHighlight
// ════════════════════════════════════════════════════════════════════════════

namespace SyntaxHighlight {

static std::unordered_map<std::string, Highlighter> _registry;
static bool _initialized = false;

static void _initBuiltins() {
    if (_initialized) return;
    _initialized = true;

    // Novac keywords
    static const std::unordered_set<std::string> KEYWORDS = {
        "var","let","const","class","if","else","for","repeat","unless","until",
        "throw","try","catch","finally","function","return","give","yield","goback",
        "async","await","do","while","block","switch","case","default","break","continue",
        "of","each","match","as","emit","on","new","extends","import","export","namespace",
        "type","struct","interface","enum","trait","impl","get","set","perform","handle",
        "server","fetch","post","put","delete","patch","head","options","from",
    };

    const char* KC = "\x1b[35m"; // keyword  = magenta
    const char* SC = "\x1b[32m"; // string   = green
    const char* NC = "\x1b[33m"; // number   = yellow
    const char* CC = "\x1b[90m"; // comment  = dark grey
    const char* IC = "\x1b[36m"; // ident    = cyan
    const char* OC = "\x1b[37m"; // op/punct = white
    const char* R  = "\x1b[0m";  // reset

    auto paint = [](const char* c, const std::string& s) {
        return std::string(c) + s + "\x1b[0m";
    };

    _registry["novac"] = _registry["nova"] = [paint, KC, SC, NC, CC, IC, OC, R](const std::string& line) {
        std::string out;
        size_t i = 0;
        while (i < line.size()) {
            // comment
            if (line[i] == '/' && i+1 < line.size() && (line[i+1]=='/'||line[i+1]=='!')) {
                out += paint(CC, line.substr(i)); break;
            }
            // string
            if (line[i] == '"' || line[i] == '\'') {
                char q = line[i]; size_t j = i+1;
                while (j < line.size() && line[j] != q) { if (line[j]=='\\') j++; j++; }
                out += paint(SC, line.substr(i, j-i+1)); i = j+1; continue;
            }
            // f-string prefix
            if (line[i] == 'f' && i+1 < line.size() && (line[i+1]=='"'||line[i+1]=='\'')) {
                char q = line[i+1]; size_t j = i+2;
                while (j < line.size() && line[j] != q) { if (line[j]=='\\') j++; j++; }
                out += paint(SC, line.substr(i, j-i+1)); i = j+1; continue;
            }
            // number
            if (std::isdigit(line[i])) {
                size_t j = i;
                while (j < line.size() && (std::isalnum(line[j])||line[j]=='.'||line[j]=='_'||line[j]=='+'||line[j]=='-')) j++;
                out += paint(NC, line.substr(i, j-i)); i = j; continue;
            }
            // identifier / keyword
            if (std::isalpha(line[i]) || line[i]=='_') {
                size_t j = i;
                while (j < line.size() && (std::isalnum(line[j])||line[j]=='_')) j++;
                std::string word = line.substr(i, j-i);
                out += KEYWORDS.count(word) ? paint(KC, word) : paint(IC, word);
                i = j; continue;
            }
            out += paint(OC, std::string(1, line[i])); i++;
        }
        return out;
    };

    _registry["json"] = [paint, SC, NC, KC](const std::string& line) {
        // Simple: strings green, numbers yellow, keywords magenta
        std::string out;
        size_t i = 0;
        while (i < line.size()) {
            if (line[i] == '"') {
                size_t j = i+1;
                while (j < line.size() && line[j] != '"') { if (line[j]=='\\') j++; j++; }
                out += paint(SC, line.substr(i, j-i+1)); i = j+1; continue;
            }
            if (std::isdigit(line[i]) || (line[i]=='-' && i+1<line.size() && std::isdigit(line[i+1]))) {
                size_t j = i; if (line[j]=='-') j++;
                while (j<line.size()&&(std::isdigit(line[j])||line[j]=='.'||line[j]=='e'||line[j]=='E'||line[j]=='+'||line[j]=='-')) j++;
                out += paint(NC, line.substr(i, j-i)); i = j; continue;
            }
            if (line.substr(i, 4) == "true"||line.substr(i,5)=="false"||line.substr(i,4)=="null") {
                size_t len = line.substr(i,4)=="null"||line.substr(i,4)=="true"?4:5;
                out += paint(KC, line.substr(i, len)); i += len; continue;
            }
            out += line[i++];
        }
        return out;
    };

    _registry["shell"] = _registry["bash"] = [paint, KC, SC, NC, CC, IC](const std::string& line) {
        if (!line.empty() && line[0] == '#') return paint(CC, line);
        return line; // simplified
    };
}

void registerHighlighter(const std::string& name, Highlighter fn) {
    _initBuiltins();
    _registry[name] = fn;
}

Highlighter get(const std::string& name) {
    _initBuiltins();
    auto it = _registry.find(name);
    return it != _registry.end() ? it->second : nullptr;
}

std::string highlight(const std::string& name, const std::string& line) {
    auto fn = get(name);
    return fn ? fn(line) : line;
}

std::vector<std::string> list() {
    _initBuiltins();
    std::vector<std::string> ks;
    for (auto& [k,_] : _registry) ks.push_back(k);
    return ks;
}

Val makeNovaObj() {
    _initBuiltins();
    auto obj = nova_obj();
    obj->obj->set("register", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() < 2 || !a[1].isFunction()) return nova_null();
        // Can't easily register a Nova fn as a C++ fn without executor ref — store placeholder
        return nova_null();
    }, "register"));
    obj->obj->set("highlight", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() < 2) return a.empty() ? nova_str("") : a[0];
        return nova_str(highlight(a[0].asString(), a[1].asString()));
    }, "highlight"));
    obj->obj->set("list", NovaValue::makeNative([](ValVec, auto) -> Val {
        ValVec out; for (auto& k : list()) out.push_back(nova_str(k));
        return nova_arr(out);
    }, "list"));
    obj->obj->set("get", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return a.empty() ? nova_null() : nova_bool((bool)get(a[0].asString()));
    }, "get"));
    return obj;
}

} // namespace SyntaxHighlight

// ════════════════════════════════════════════════════════════════════════════
//  InputMgr — synchronous terminal line reader
// ════════════════════════════════════════════════════════════════════════════

namespace InputMgr {

// Platform-agnostic synchronous character read
static int readChar() {
#if defined(_WIN32)
    return _getch();
#else
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    int c = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return c;
#endif
}

std::string prompt(const std::string& message, const PromptOpts& opts) {
    std::cout << message << std::flush;

    if (!opts.password && opts.historyKey.empty()) {
        // Simple readline
        std::string line;
        std::getline(std::cin, line);
        return line;
    }

    // Full raw-mode line editor
    std::string buf;
    std::vector<std::string> hist = opts.historyKey.empty() ? std::vector<std::string>{}
                                                             : History::load(opts.historyKey);
    int histIdx = (int)hist.size();
    std::string saved;

#if !defined(_WIN32)
    struct termios oldt, newt;
    bool isTTY = isatty(STDIN_FILENO);
    if (isTTY) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }
#endif

    auto redraw = [&]() {
        std::cout << '\r' << "\x1b[2K" << message;
        if (opts.password) std::cout << std::string(buf.size(), opts.mask[0]);
        else if (!opts.highlight.empty()) std::cout << SyntaxHighlight::highlight(opts.highlight, buf);
        else std::cout << buf;
        std::cout << std::flush;
    };

    redraw();

    while (true) {
        int c = readChar();
        if (c == '\r' || c == '\n') { std::cout << '\n'; break; }
        if (c == 127 || c == 8) {  // backspace
            if (!buf.empty()) { buf.pop_back(); redraw(); }
            continue;
        }
        if (c == 3) { std::cout << "^C\n"; std::exit(130); } // Ctrl-C
        if (c == 4) { std::cout << '\n'; break; }             // Ctrl-D
        if (c == 27) {  // escape sequence
            int c2 = readChar();
            if (c2 == '[') {
                int c3 = readChar();
                if (c3 == 'A' && !hist.empty()) { // up
                    if (histIdx == (int)hist.size()) saved = buf;
                    if (histIdx > 0) { histIdx--; buf = hist[histIdx]; redraw(); }
                } else if (c3 == 'B') { // down
                    if (histIdx < (int)hist.size()) {
                        histIdx++;
                        buf = histIdx == (int)hist.size() ? saved : hist[histIdx];
                        redraw();
                    }
                }
            }
            continue;
        }
        if (c >= 32) { buf += (char)c; redraw(); }
    }

#if !defined(_WIN32)
    if (isTTY) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif

    if (!opts.password && !opts.historyKey.empty() && !buf.empty())
        History::push(opts.historyKey, buf);

    return buf;
}

std::vector<ListItem> list(const std::vector<std::string>& items, const ListOpts& opts) {
    if (items.empty()) return {};

    if (!opts.label.empty()) std::cout << opts.label << "\n";

    int cursor = 0;
    std::string RESET = "\x1b[0m";

    auto draw = [&](bool first) {
        if (!first) {
            // move cursor up
            std::cout << "\x1b[" << items.size() << "A";
        }
        for (size_t i = 0; i < items.size(); i++) {
            bool hi = ((int)i == cursor);
            std::cout << "\r\x1b[2K";
            if (hi) std::cout << opts.highlightColor << opts.pointer;
            else    std::cout << opts.color << std::string(opts.pointer.size(), ' ');
            std::cout << items[i] << RESET << "\n";
        }
        std::cout << std::flush;
    };

    draw(true);

#if !defined(_WIN32)
    struct termios oldt, newt;
    bool isTTY = isatty(STDIN_FILENO);
    if (isTTY) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        newt.c_cc[VMIN] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }
#endif
    std::cout << "\x1b[?25l"; // hide cursor

    std::vector<int> selected;

    while (true) {
        int c = readChar();
        if (c == '\r' || c == '\n') break;
        if (c == 3) { std::cout << "\x1b[?25h\n"; std::exit(130); }
        if (c == 27) {
            int c2 = readChar();
            if (c2 == '[') {
                int c3 = readChar();
                if (c3 == 'A' && cursor > 0) { cursor--; draw(false); }
                if (c3 == 'B' && cursor < (int)items.size()-1) { cursor++; draw(false); }
            }
            continue;
        }
        if (c == ' ' && opts.multi) {
            auto it = std::find(selected.begin(), selected.end(), cursor);
            if (it != selected.end()) selected.erase(it);
            else selected.push_back(cursor);
            draw(false);
        }
    }

    std::cout << "\x1b[?25h\n";
#if !defined(_WIN32)
    if (isTTY) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif

    if (opts.multi) {
        std::vector<ListItem> out;
        for (int i : selected) out.push_back({items[i], i});
        if (out.empty()) out.push_back({items[cursor], cursor});
        return out;
    }
    return {{items[cursor], cursor}};
}

Val makeNovaObj(Executor& exe) {
    auto obj = nova_obj();

    obj->obj->set("prompt", NovaValue::makeNative([](ValVec a, auto) -> Val {
        std::string msg = a.empty() ? "> " : a[0].asString();
        PromptOpts opts;
        if (a.size() > 1 && a[1].isObject()) {
            Val o = a[1];
            Val pw = o->obj->get("password");
            if (pw && pw.asBool()) opts.password = true;
            Val hk = o->obj->get("historyKey");
            if (hk && hk.isString()) opts.historyKey = hk->sval;
            Val hl = o->obj->get("highlight");
            if (hl && hl.isString()) opts.highlight = hl->sval;
            Val ph = o->obj->get("placeholder");
            if (ph && ph.isString()) opts.placeholder = ph->sval;
        }
        return nova_str(prompt(msg, opts));
    }, "prompt"));

    obj->obj->set("promptPassword", NovaValue::makeNative([](ValVec a, auto) -> Val {
        std::string msg = a.empty() ? "Password: " : a[0].asString();
        PromptOpts opts; opts.password = true;
        return nova_str(prompt(msg, opts));
    }, "promptPassword"));

    obj->obj->set("repl", NovaValue::makeNative([&exe](ValVec a, auto cs) -> Val {
        std::string promptStr = "> ";
        std::string banner;
        std::string histKey = "repl";
        std::string hlTheme;
        Val evalFn;
        std::vector<std::string> exitWords = {"exit","quit",".q"};

        if (!a.empty() && a[0].isObject()) {
            Val o = a[0];
            Val p = o->obj->get("prompt"); if (p && p.isString()) promptStr = p->sval;
            Val b = o->obj->get("banner"); if (b && b.isString()) banner = b->sval;
            Val h = o->obj->get("historyKey"); if (h && h.isString()) histKey = h->sval;
            Val hl = o->obj->get("highlight"); if (hl && hl.isString()) hlTheme = hl->sval;
            Val ef = o->obj->get("eval"); if (ef && ef.isFunction()) evalFn = ef;
        }

        if (!banner.empty()) std::cout << banner << "\n";

        while (true) {
            PromptOpts opts; opts.historyKey = histKey; opts.highlight = hlTheme;
            std::string line = prompt(promptStr, opts);
            if (line.empty()) continue;
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));
            if (std::find(exitWords.begin(), exitWords.end(), trimmed) != exitWords.end()) break;
            if (evalFn) {
                try {
                    Val out = exe.callFunction(evalFn, {nova_str(trimmed)}, cs);
                    if (out && !out.isNull()) std::cout << exe.stringify(out) << "\n";
                } catch (std::exception& e) {
                    std::cout << "\x1b[31mError: " << e.what() << "\x1b[0m\n";
                }
            }
        }
        return nova_null();
    }, "repl"));

    obj->obj->set("list", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty() || !a[0].isArray()) return nova_null();
        std::vector<std::string> items;
        for (auto& v : a[0]->arr->inner) items.push_back(v.asString());
        ListOpts opts;
        if (a.size() > 1 && a[1].isObject()) {
            Val o = a[1];
            Val m = o->obj->get("multi"); if (m) opts.multi = m.asBool();
            Val l = o->obj->get("label"); if (l && l.isString()) opts.label = l->sval;
        }
        auto result = list(items, opts);
        if (result.empty()) return nova_null();
        if (!opts.multi) {
            auto out = nova_obj();
            out->obj->set("value", nova_str(result[0].value));
            out->obj->set("index", nova_num(result[0].index));
            return out;
        }
        ValVec out;
        for (auto& r : result) {
            auto o = nova_obj();
            o->obj->set("value", nova_str(r.value));
            o->obj->set("index", nova_num(r.index));
            out.push_back(o);
        }
        return nova_arr(out);
    }, "list"));

    obj->obj->set("SyntaxHighlight", SyntaxHighlight::makeNovaObj());
    obj->obj->set("highlight", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() < 2) return a.empty() ? nova_str("") : a[0];
        return nova_str(SyntaxHighlight::highlight(a[0].asString(), a[1].asString()));
    }, "highlight"));

    obj->obj->set("history", History::makeNovaObj());

    return obj;
}

} // namespace InputMgr

// ════════════════════════════════════════════════════════════════════════════
//  Stack / Queue
// ════════════════════════════════════════════════════════════════════════════

Val makeNovaStack() {
    auto data = std::make_shared<ValVec>();
    auto obj  = nova_obj();

    obj->obj->set("push", NovaValue::makeNative([data, obj](ValVec a, auto) -> Val {
        if (!a.empty()) data->push_back(a[0]);
        obj->obj->set("size", nova_num((double)data->size()));
        return obj;
    }, "push"));
    obj->obj->set("pop", NovaValue::makeNative([data, obj](ValVec, auto) -> Val {
        if (data->empty()) return nova_null();
        Val v = data->back(); data->pop_back();
        obj->obj->set("size", nova_num((double)data->size()));
        return v;
    }, "pop"));
    obj->obj->set("peek", NovaValue::makeNative([data](ValVec, auto) -> Val {
        return data->empty() ? nova_null() : data->back();
    }, "peek"));
    obj->obj->set("isEmpty", NovaValue::makeNative([data](ValVec, auto) -> Val {
        return nova_bool(data->empty());
    }, "isEmpty"));
    obj->obj->set("clear", NovaValue::makeNative([data, obj](ValVec, auto) -> Val {
        data->clear();
        obj->obj->set("size", nova_num(0));
        return obj;
    }, "clear"));
    obj->obj->set("toArray", NovaValue::makeNative([data](ValVec, auto) -> Val {
        ValVec out(data->rbegin(), data->rend());
        return nova_arr(out);
    }, "toArray"));
    obj->obj->set("size", nova_num(0));
    return obj;
}

Val makeNovaQueue() {
    auto data = std::make_shared<ValVec>();
    auto obj  = nova_obj();

    obj->obj->set("enqueue", NovaValue::makeNative([data, obj](ValVec a, auto) -> Val {
        if (!a.empty()) data->push_back(a[0]);
        obj->obj->set("size", nova_num((double)data->size()));
        return obj;
    }, "enqueue"));
    obj->obj->set("dequeue", NovaValue::makeNative([data, obj](ValVec, auto) -> Val {
        if (data->empty()) return nova_null();
        Val v = data->front(); data->erase(data->begin());
        obj->obj->set("size", nova_num((double)data->size()));
        return v;
    }, "dequeue"));
    obj->obj->set("front", NovaValue::makeNative([data](ValVec, auto) -> Val {
        return data->empty() ? nova_null() : data->front();
    }, "front"));
    obj->obj->set("isEmpty", NovaValue::makeNative([data](ValVec, auto) -> Val {
        return nova_bool(data->empty());
    }, "isEmpty"));
    obj->obj->set("clear", NovaValue::makeNative([data, obj](ValVec, auto) -> Val {
        data->clear();
        obj->obj->set("size", nova_num(0));
        return obj;
    }, "clear"));
    obj->obj->set("toArray", NovaValue::makeNative([data](ValVec, auto) -> Val {
        return nova_arr(*data);
    }, "toArray"));
    obj->obj->set("size", nova_num(0));
    return obj;
}

// ════════════════════════════════════════════════════════════════════════════
//  registerBstd — main entry point called by Executor::_loadBstd
// ════════════════════════════════════════════════════════════════════════════

void registerBstd(Executor& exe) {
    auto s = exe.globalScope;

    // ── History ──────────────────────────────────────────────────────────────
    s->setOwn("History", History::makeNovaObj());

    // ── Storage ───────────────────────────────────────────────────────────────
    s->setOwn("Storage", Storage::makeNovaObj());

    // ── Crypto ────────────────────────────────────────────────────────────────
    s->setOwn("Crypto", Crypto::makeNovaObj());

    // ── Base64 ────────────────────────────────────────────────────────────────
    s->setOwn("Base64", Base64::makeNovaObj());

    // ── PathUtils ─────────────────────────────────────────────────────────────
    s->setOwn("PathUtils", PathUtils::makeNovaObj());

    // ── OsUtils ───────────────────────────────────────────────────────────────
    s->setOwn("OsUtils", OsUtils::makeNovaObj());

    // ── UUID ──────────────────────────────────────────────────────────────────
    s->setOwn("UUID", UUID::makeNovaObj());

    // ── Deep ──────────────────────────────────────────────────────────────────
    s->setOwn("Deep", Deep::makeNovaObj());

    // ── Timers ────────────────────────────────────────────────────────────────
    s->setOwn("Timers", Timers::makeNovaObj(exe));

    // ── Env ───────────────────────────────────────────────────────────────────
    s->setOwn("Env", Env::makeNovaObj());

    // ── SyntaxHighlight ───────────────────────────────────────────────────────
    s->setOwn("SyntaxHighlight", SyntaxHighlight::makeNovaObj());

    // ── InputMgr ──────────────────────────────────────────────────────────────
    s->setOwn("InputMgr", InputMgr::makeNovaObj(exe));

    // ── Stack / Queue constructors ────────────────────────────────────────────
    s->setOwn("Stack", NovaValue::makeNative([](ValVec, auto) -> Val {
        return makeNovaStack();
    }, "Stack"));
    s->setOwn("Queue", NovaValue::makeNative([](ValVec, auto) -> Val {
        return makeNovaQueue();
    }, "Queue"));

    // ── Schema (lightweight) ──────────────────────────────────────────────────
    auto schemaObj = nova_obj();
    schemaObj->obj->set("validate", NovaValue::makeNative([](ValVec a, auto) -> Val {
        // Basic: validate(value, schemaObj) — check that all keys in schema exist in value
        if (a.size() < 2 || !a[1].isObject()) return nova_bool(true);
        if (!a[0].isObject()) return nova_bool(false);
        for (auto& k : a[1]->obj->keys())
            if (!a[0]->obj->has(k)) return nova_bool(false);
        return nova_bool(true);
    }, "validate"));
    s->setOwn("Schema", schemaObj);

    // ── fs (file system) ──────────────────────────────────────────────────────
    auto fsObj = nova_obj();
    fsObj->obj->set("readFileSync", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty()) return nova_null();
        std::ifstream f(a[0].asString());
        if (!f.is_open()) return nova_null();
        return nova_str(std::string(std::istreambuf_iterator<char>(f), {}));
    }, "readFileSync"));
    fsObj->obj->set("writeFileSync", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_null();
        std::ofstream f(a[0].asString());
        f << a[1].asString();
        return nova_null();
    }, "writeFileSync"));
    fsObj->obj->set("existsSync", NovaValue::makeNative([](ValVec a, auto) -> Val {
        return nova_bool(!a.empty() && fs::exists(a[0].asString()));
    }, "existsSync"));
    fsObj->obj->set("mkdirSync", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (!a.empty()) fs::create_directories(a[0].asString());
        return nova_null();
    }, "mkdirSync"));
    fsObj->obj->set("unlinkSync", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (!a.empty()) fs::remove(a[0].asString());
        return nova_null();
    }, "unlinkSync"));
    fsObj->obj->set("readdirSync", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty() || !fs::exists(a[0].asString())) return nova_arr();
        ValVec out;
        for (auto& e : fs::directory_iterator(a[0].asString()))
            out.push_back(nova_str(e.path().filename().string()));
        return nova_arr(out);
    }, "readdirSync"));
    fsObj->obj->set("appendFileSync", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.size() < 2) return nova_null();
        std::ofstream f(a[0].asString(), std::ios::app);
        f << a[1].asString();
        return nova_null();
    }, "appendFileSync"));
    fsObj->obj->set("statSync", NovaValue::makeNative([](ValVec a, auto) -> Val {
        if (a.empty() || !fs::exists(a[0].asString())) return nova_null();
        auto stat = nova_obj();
        auto p = a[0].asString();
        stat->obj->set("isFile",      nova_bool(fs::is_regular_file(p)));
        stat->obj->set("isDirectory", nova_bool(fs::is_directory(p)));
        stat->obj->set("size",        nova_num((double)fs::file_size(p)));
        return stat;
    }, "statSync"));
    s->setOwn("Fs", fsObj);
}

} // namespace novac
