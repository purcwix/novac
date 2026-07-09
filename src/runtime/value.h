#pragma once
#include <string>
#include <type_traits>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <variant>
#include <cmath>
#include <iomanip>
#include <cassert>
#include <stdexcept>
#include <sstream>
#include <optional>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <istream>
#include <ostream>
#include <iterator>

namespace novac
{

    // ── forward declarations ─────────────────────────────────────────────────────
    struct NovaValue;
    struct NovaArray;
    struct NovaNative;
    struct NovaObject;
    struct NovaFunction;
    struct NovaRange;
    struct NovaStruct;
    struct NovaEnum;
    struct NovaPointer;
    struct NovaBigInt;
    struct NovaBigFloat;
    class Scope;
    struct Node;

    // ── Phase 1 TVal (data layout only — methods defined at bottom after NovaValue complete)
    // Forward declaration of VK so TVal can use it
    enum class VK : int;

    struct TVal
    {
        // Tag constants
        enum Tag : uint8_t
        {
            TNul = 0,
            TBool,
            TNum,
            THeap,
            TUndef
        };

        std::shared_ptr<NovaValue> _heap;            // null for primitive tags
        mutable std::shared_ptr<NovaValue> _lazyBox; // cached box for -> access on primitives
        double _n = 0.0;
        uint8_t _tag = TNul;
        bool _b = false;

        // ── construction ─────────────────────────────────────────────────────
        TVal() noexcept : _tag(TNul) {}                 // Nova null value — truthy (like old non-null ptr)
        TVal(std::nullptr_t) noexcept : _tag(TUndef) {} // "not found" sentinel — falsy (like old nullptr)
        TVal(std::shared_ptr<NovaValue> v) noexcept
            : _heap(std::move(v)), _tag(_heap ? THeap : TUndef) {}

        // Factory statics (don't need NovaValue complete)
        static TVal null() noexcept { return TVal(); }
        static TVal boolean(bool b) noexcept
        {
            TVal v;
            v._tag = TBool;
            v._b = b;
            return v;
        }
        static TVal number(double n) noexcept
        {
            TVal v;
            v._tag = TNum;
            v._n = n;
            return v;
        }
        static TVal undef() noexcept { return TVal(nullptr); } // "variable not found"

        // ── pointer-like interface ────────────────────────────────────────────
        NovaValue *operator->() const noexcept
        {
            if (_heap)
                return _heap.get();
            return _ensureBox();
        }
        NovaValue &operator*() const noexcept { return *(_heap ? _heap.get() : _ensureBox()); }
        NovaValue *get() const noexcept { return _heap.get(); }

        // Implicit conversion to shared_ptr (backward compat for call sites)
        std::shared_ptr<NovaValue> asShared() const; // defined later
        operator std::shared_ptr<NovaValue>() const { return asShared(); }
        NovaValue *_ensureBox() const; // defined later — lazily boxes a primitive

        // ── bool/null checks (no NovaValue access needed) ─────────────────────
        explicit operator bool() const noexcept { return _tag != TUndef; } // false only for "not found"
        bool operator==(std::nullptr_t) const noexcept { return _tag == TUndef; }
        bool operator!=(std::nullptr_t) const noexcept { return _tag != TUndef; }

        // ── type checks (defined inline at bottom after VK/NovaValue complete) ──
        bool isNull() const noexcept { return _tag == TNul; } // Nova null (truthy in C++)
        bool isBool() const noexcept { return _tag == TBool; }
        bool isNumber() const noexcept { return _tag == TNum; }
        bool isString() const noexcept;
        bool isArray() const noexcept;
        bool isObject() const noexcept;
        bool isFunction() const noexcept;
        bool isRange() const noexcept;
        bool isStruct() const noexcept;
        bool isEnum() const noexcept;
        bool isPointer() const noexcept;
        bool isClass() const noexcept;
        bool isNativeVal() const noexcept;
        // In the class body, replace the inline definitions with declarations:
        bool isBigInt() const noexcept;
        bool isBigFloat() const noexcept;
        // ── coercions (defined at bottom) ─────────────────────────────────────
        bool asBool() const;
        double asNumber() const;
        std::string asString() const;
        VK kind() const;

        // ── equality ──────────────────────────────────────────────────────────
        bool operator==(const TVal &o) const;
        bool operator!=(const TVal &o) const { return !(*this == o); }
        bool operator<(const TVal &o) const;
    };

    using Val = TVal;
    using ValVec = std::vector<Val>;
    using ValMap = std::unordered_map<std::string, Val>;

    // ════════════════════════════════════════════════════════════════════════════
    //  NATIVE TYPES  (for nat() / unat() / raw_ptr())
    //  Declared here so NovaValue::makeNativeVal can reference NativeType
    // ════════════════════════════════════════════════════════════════════════════

    // ── NovaClass ────────────────────────────────────────────────────────────────
    struct NovaClass
    {
        std::string name;
        std::shared_ptr<Node> node;
        std::shared_ptr<Scope> closureScope;
        Val superClass;
        ValMap methods; // instance methods + constructor
        ValMap statics; // static members
    };

    enum class NativeType
    {
        I8,
        U8,
        I16,
        U16,
        I32,
        U32,
        I64,
        U64,
        F32,
        F64,
        Bool,
        CStr,
        Ptr,
        Stream
    };

    inline size_t nativeTypeSize(NativeType t)
    {
        if (t == NativeType::Stream)
            return 0;
        switch (t)
        {
        case NativeType::I8:
        case NativeType::U8:
        case NativeType::Bool:
            return 1;
        case NativeType::I16:
        case NativeType::U16:
            return 2;
        case NativeType::I32:
        case NativeType::U32:
        case NativeType::F32:
            return 4;
        case NativeType::I64:
        case NativeType::U64:
        case NativeType::F64:
        case NativeType::Ptr:
            return 8;
        case NativeType::CStr:
            return 0; // variable-length
        }
        return 0;
    }

    inline std::string nativeTypeName(NativeType t)
    {
        if (t == NativeType::Stream)
            return "stream";
        switch (t)
        {
        case NativeType::I8:
            return "i8";
        case NativeType::U8:
            return "u8";
        case NativeType::I16:
            return "i16";
        case NativeType::U16:
            return "u16";
        case NativeType::I32:
            return "i32";
        case NativeType::U32:
            return "u32";
        case NativeType::I64:
            return "i64";
        case NativeType::U64:
            return "u64";
        case NativeType::F32:
            return "f32";
        case NativeType::F64:
            return "f64";
        case NativeType::Bool:
            return "bool";
        case NativeType::CStr:
            return "cstr";
        case NativeType::Ptr:
            return "ptr";
        }
        return "u8";
    }

    inline NativeType parseNativeType(const std::string &s)
    {
        if (s == "stream")
            return NativeType::Stream;
        if (s == "i8")
            return NativeType::I8;
        if (s == "u8")
            return NativeType::U8;
        if (s == "i16")
            return NativeType::I16;
        if (s == "u16")
            return NativeType::U16;
        if (s == "i32" || s == "int")
            return NativeType::I32;
        if (s == "u32" || s == "uint")
            return NativeType::U32;
        if (s == "i64" || s == "long")
            return NativeType::I64;
        if (s == "u64" || s == "ulong")
            return NativeType::U64;
        if (s == "f32" || s == "float")
            return NativeType::F32;
        if (s == "f64" || s == "double")
            return NativeType::F64;
        if (s == "bool")
            return NativeType::Bool;
        if (s == "cstr" || s == "string" || s == "str")
            return NativeType::CStr;
        if (s == "ptr" || s == "pointer")
            return NativeType::Ptr;
        return NativeType::F64; // sensible default for unknown type names
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  VALUE KINDS
    // ════════════════════════════════════════════════════════════════════════════

    enum class VK
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
        Function,
        Range,
        Struct,
        Enum,
        Pointer,
        TemplateString,
        Native,    // raw C++ callable wrapped as a value
        NativeVal, // nat() — typed native scalar/byte buffer
        Class,
        BigInt,
        BigFloat,
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaValue  — base tagged union
    // ════════════════════════════════════════════════════════════════════════════

    struct NovaValue
    {
        VK kind = VK::Null;

        // ── primitive storage ──
        bool bval = false;
        double nval = 0.0;
        std::string sval;

        // ── compound storage (lazy-alloc) ──
        std::shared_ptr<NovaClass> cls;
        bool isClass() const { return kind == VK::Class; }

        static Val makeClass(std::shared_ptr<NovaClass> cls)
        {
            auto v = std::make_shared<NovaValue>();
            v->kind = VK::Class;
            v->cls = cls;
            return v;
        }
        std::shared_ptr<NovaArray> arr;
        std::shared_ptr<NovaObject> obj;
        std::shared_ptr<NovaFunction> fn;
        std::shared_ptr<NovaRange> range;
        std::shared_ptr<NovaStruct> strct;
        std::shared_ptr<NovaEnum> enm;
        std::shared_ptr<NovaPointer> ptr;
        std::shared_ptr<NovaBigInt> bigint;
        std::shared_ptr<NovaBigFloat> bigfloat;
        std::shared_ptr<NovaNative> natv; // NEW — for VK::NativeVal

        // ── operator overload table (optional) ──
        // { "binary:+", fn }, { "unary:-", fn }, ...
        std::shared_ptr<ValMap> overloads;

        // ── constructors ────────────────────────────────────────────────────────
        static Val makeNull();
        static Val makeBool(bool b);
        static Val makeNumber(double n);
        static Val makeString(const std::string &s);
        static Val makeString(std::string &&s);
        static Val makeArray(ValVec elems = {});
        static Val makeObject(ValMap props = {});
        static Val makeRange(double start, double end, double step = 1.0);
        static Val makeStruct(const std::string &typeName, ValMap fields = {});
        static Val makeEnum(const std::string &typeName, const std::string &variant, Val value = nullptr);
        static Val makePointer(Val inner,
                               std::function<Val()> readFn = nullptr,
                               std::function<void(Val)> writeFn = nullptr,
                               std::string address = "");
        static Val makeBigInt(const std::string &s);
        static Val makeBigInt(int64_t v);
        static Val makeBigInt(NovaBigInt bi);
        static Val makeBigFloat(const std::string &s, int prec = 50);
        static Val makeBigFloat(double v, int prec = 50);
        static Val makeBigFloat(NovaBigFloat bf);
        static Val makeNative(std::function<Val(ValVec, std::shared_ptr<Scope>)> fn,
                              std::string name = "<native>");
        static Val makeNativeVal(NativeType type, std::vector<uint8_t> data);

        // NEW — convenience overloads for registering raw C++ values directly
        template <typename StreamT>
        static Val makeNativeVal(std::shared_ptr<StreamT> stream);
        template <typename StreamT>
        static Val makeStream(std::shared_ptr<StreamT> stream);
        template <typename T>
        static Val makeNativeVal(T value);
        static Val makeNativeVal(const std::string &s);
        static Val makeNativeVal(const char *s) { return makeNativeVal(std::string(s)); }
        static Val makeFunction(std::shared_ptr<Node> node,
                                std::shared_ptr<Scope> closureScope = nullptr);

        // ── coercions ───────────────────────────────────────────────────────────
        bool asBool() const;
        double asNumber() const;
        std::string asString() const;

        // ── type helpers ─────────────────────────────────────────────────────────
        bool isNull() const { return kind == VK::Null; }
        bool isBool() const { return kind == VK::Bool; }
        bool isNumber() const { return kind == VK::Number; }
        bool isString() const { return kind == VK::String; }
        bool isArray() const { return kind == VK::Array; }
        bool isObject() const { return kind == VK::Object; }
        bool isFunction() const { return kind == VK::Function || kind == VK::Native; }
        bool isRange() const { return kind == VK::Range; }
        bool isStruct() const { return kind == VK::Struct; }
        bool isEnum() const { return kind == VK::Enum; }
        bool isPointer() const { return kind == VK::Pointer; }
        bool isBigInt() const { return kind == VK::BigInt; }
        bool isBigFloat() const { return kind == VK::BigFloat; }
        bool isNativeVal() const { return kind == VK::NativeVal; } // NEW

        std::string typeName() const;

        // ── equality ─────────────────────────────────────────────────────────────
        bool operator==(const NovaValue &o) const;
        bool operator!=(const NovaValue &o) const { return !(*this == o); }
        bool operator<(const NovaValue &o) const;
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaArray
    // ════════════════════════════════════════════════════════════════════════════

    struct NovaArray
    {
        ValVec inner;
        NovaArray() = default;
        explicit NovaArray(ValVec v) : inner(std::move(v)) {}

        Val get(int i) const;
        void set(int i, Val v);
        void push(Val v) { inner.push_back(v); }
        Val pop();
        Val shift();
        void unshift(Val v) { inner.insert(inner.begin(), v); }
        size_t length() const { return inner.size(); }

        std::string toString() const;
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaObject
    // ════════════════════════════════════════════════════════════════════════════

    // Meta protocol — optional hooks on get/set/missing
    struct NovaMeta
    {
        std::function<Val(const std::string &k, Val v)> get;
        std::function<void(const std::string &k, Val v, ValMap &inner)> set;
        std::function<Val(const std::string &k)> missing;
    };

    struct NovaObject
    {
        ValMap inner;
        std::shared_ptr<NovaMeta> meta;

        NovaObject() = default;
        explicit NovaObject(ValMap m) : inner(std::move(m)) {}

        Val get(const std::string &k) const;
        void set(const std::string &k, Val v);
        void del(const std::string &k) { inner.erase(k); }
        bool has(const std::string &k) const { return inner.count(k) > 0; }

        std::vector<std::string> keys() const;
        ValVec values() const;
        std::string toString() const;
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaFunction
    // ════════════════════════════════════════════════════════════════════════════

    struct NovaFunction
    {
        // AST node (nullptr for native functions)
        std::shared_ptr<Node> node;
        // Closure scope captured at definition time
        std::shared_ptr<Scope> closureScope;
        // Native C++ callable (set when node == nullptr)
        std::function<Val(ValVec, std::shared_ptr<Scope>)> native;

        std::string name;
        bool isNative = false;
        bool isAsync = false;
        bool isGenerator = false;
        bool strictArgs = false;
        bool memoize = false;
        bool once = false;

        // memoize cache
        std::shared_ptr<std::unordered_map<std::string, Val>> memoCache;
        int execCount = 0;

        bool isCallable() const { return node || native; }

    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaRange
    // ════════════════════════════════════════════════════════════════════════════

    struct NovaRange
    {
        double start, end, step;
        NovaRange(double s, double e, double st = 1.0) : start(s), end(e), step(st) {}

        ValVec toArray() const;
        bool includes(double v) const
        {
            return v >= std::min(start, end) && v <= std::max(start, end);
        }
        size_t length() const
        {
            if (step == 0)
                return 0;
            double span = (end - start) / step;
            if (span < 0)
                return 0;
            return (size_t)std::floor(std::abs(span)) + 1;
        }
        std::string toString() const
        {
            std::ostringstream ss;
            ss << "Range(" << start << ".." << end << ")";
            return ss.str();
        }
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaStruct
    // ════════════════════════════════════════════════════════════════════════════

    struct NovaStruct
    {
        std::string typeName;
        ValMap inner;

        NovaStruct() = default;
        NovaStruct(std::string t, ValMap f) : typeName(std::move(t)), inner(std::move(f)) {}

        Val get(const std::string &k) const
        {
            auto it = inner.find(k);
            return it != inner.end() ? it->second : nullptr;
        }
        void set(const std::string &k, Val v) { inner[k] = v; }
        void del(const std::string &k) { inner.erase(k); }

        std::string toString() const;
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaEnum
    // ════════════════════════════════════════════════════════════════════════════

    struct NovaEnum
    {
        std::string typeName;
        std::string variant;
        Val value; // payload (may be nullptr)

        NovaEnum() = default;
        NovaEnum(std::string t, std::string v, Val val = nullptr)
            : typeName(std::move(t)), variant(std::move(v)), value(val) {}

        std::string toString() const { return typeName + "::" + variant; }
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaPointer
    // ════════════════════════════════════════════════════════════════════════════
    struct NovaPointer
    {
        bool isLink = false;
        Val inner;
        Val *ptrAddr = nullptr;
        std::function<Val()> readFn;
        std::function<void(Val)> writeFn;
        std::string address;

        // ── raw / unsafe memory mode (raw_ptr) ──────────────────────────── // NEW
        bool isRaw = false;                  // NEW
        void *rawAddr = nullptr;             // NEW
        size_t rawSize = 0;                  // NEW
        NativeType rawType = NativeType::U8; // NEW
        // keeps the backing buffer alive (shared with a NativeVal, or owned) // NEW
        std::shared_ptr<std::vector<uint8_t>> rawBuffer; // NEW

        NovaPointer() = default;
        NovaPointer(Val v,
                    std::function<Val()> r = nullptr,
                    std::function<void(Val)> w = nullptr,
                    std::string a = "")
            : inner(v), ptrAddr(nullptr), readFn(r), writeFn(w), address(a)
        {
            allocate();
        }

        NovaPointer(NovaPointer &&o) noexcept
            : inner(std::move(o.inner)), ptrAddr(o.ptrAddr), readFn(std::move(o.readFn)),
              writeFn(std::move(o.writeFn)), address(std::move(o.address)),
              isRaw(o.isRaw), rawAddr(o.rawAddr), rawSize(o.rawSize), // NEW
              rawType(o.rawType), rawBuffer(std::move(o.rawBuffer))
        { // NEW
            o.ptrAddr = nullptr;
            o.rawAddr = nullptr; // NEW
        }

        NovaPointer &operator=(NovaPointer &&o) noexcept
        {
            if (this != &o)
            {
                deallocate();
                inner = std::move(o.inner);
                ptrAddr = o.ptrAddr;
                readFn = std::move(o.readFn);
                writeFn = std::move(o.writeFn);
                address = std::move(o.address);
                isRaw = o.isRaw;                    // NEW
                rawAddr = o.rawAddr;                // NEW
                rawSize = o.rawSize;                // NEW
                rawType = o.rawType;                // NEW
                rawBuffer = std::move(o.rawBuffer); // NEW
                o.ptrAddr = nullptr;
                o.rawAddr = nullptr; // NEW
            }
            return *this;
        }

        ~NovaPointer()
        {
            deallocate();
        }

        NovaPointer(const NovaPointer &o) = delete;
        NovaPointer &operator=(const NovaPointer &o) = delete;

        void allocate()
        {
            if (!ptrAddr)
            {
                ptrAddr = static_cast<Val *>(std::malloc(sizeof(Val)));
                if (!ptrAddr)
                    throw std::bad_alloc();
                new (ptrAddr) Val(inner);
                if (address.empty())
                {
                    std::ostringstream ss;
                    ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(ptrAddr);
                    address = ss.str();
                }
            }
        }

        void deallocate()
        {
            if (ptrAddr)
            {
                ptrAddr->~TVal();
                std::free(ptrAddr);
                ptrAddr = nullptr;
            }
        }

        Val read() const
        {
            if (isRaw)
                return readRaw(); // ← add this
            return readFn ? readFn() : (ptrAddr ? *ptrAddr : inner);
        }
        void write(Val v)
        {
            if (isRaw)
            {
                writeRaw(v);
                return;
            } // ← add this
            if (writeFn)
                writeFn(v);
            if (ptrAddr)
                *ptrAddr = v;
            else
                inner = v;
        }

        // ── raw read/write — UNSAFE: no bounds or lifetime checking ──────  // NEW
        uintptr_t addressNum() const { return reinterpret_cast<uintptr_t>(rawAddr); } // NEW
        Val readRaw(NativeType t) const;                                              // NEW
        Val readRaw() const { return readRaw(rawType); }                              // NEW
        void writeRaw(Val v, NativeType t);                                           // NEW
        void writeRaw(Val v) { writeRaw(v, rawType); }                                // NEW

        std::string toString() const
        {
            if (isRaw)
            {                                                   // NEW
                std::ostringstream ss;                          // NEW
                ss << "<raw_ptr@0x" << std::hex << addressNum() // NEW
                   << " (" << nativeTypeName(rawType) << ")>";  // NEW
                return ss.str();                                // NEW
            } // NEW
            return "<Pointer@" + address.substr(0, std::min(address.size(), (size_t)6)) + ">";
        }
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaBigInt — arbitrary-precision signed integer (base 10^9 limbs)
    // ════════════════════════════════════════════════════════════════════════════
    struct NovaBigInt
    {
        static constexpr uint32_t BASE = 1'000'000'000u;
        std::vector<uint32_t> limbs; // little-endian base-10^9 digits
        bool negative = false;

        NovaBigInt() : limbs{0} {}
        explicit NovaBigInt(int64_t v)
        {
            negative = v < 0;
            uint64_t u = negative ? (v == INT64_MIN ? (uint64_t)9223372036854775808ULL : (uint64_t)-v) : (uint64_t)v;
            if (u == 0)
            {
                limbs.push_back(0);
                return;
            }
            while (u)
            {
                limbs.push_back((uint32_t)(u % BASE));
                u /= BASE;
            }
        }
        explicit NovaBigInt(const std::string &s)
        {
            size_t i = 0;
            negative = false;
            if (i < s.size() && (s[i] == '-' || s[i] == '+'))
            {
                negative = (s[i++] == '-');
            }
            // skip leading zeros
            while (i < s.size() - 1 && s[i] == '0')
                i++;
            std::string digits = s.substr(i);
            if (digits.empty() || digits == "0")
            {
                limbs.push_back(0);
                negative = false;
                return;
            }
            // parse in chunks of 9 from the right
            int j = (int)digits.size();
            while (j > 0)
            {
                int start = std::max(0, j - 9);
                limbs.push_back((uint32_t)std::stoul(digits.substr(start, j - start)));
                j = start;
            }
            trim();
            if (isZero())
                negative = false;
        }

        bool isZero() const { return limbs.size() == 1 && limbs[0] == 0; }
        void trim()
        {
            while (limbs.size() > 1 && limbs.back() == 0)
                limbs.pop_back();
        }

        // ── comparison (magnitude only) ──
        static int cmpAbs(const NovaBigInt &a, const NovaBigInt &b)
        {
            if (a.limbs.size() != b.limbs.size())
                return a.limbs.size() < b.limbs.size() ? -1 : 1;
            for (int i = (int)a.limbs.size() - 1; i >= 0; i--)
                if (a.limbs[i] != b.limbs[i])
                    return a.limbs[i] < b.limbs[i] ? -1 : 1;
            return 0;
        }
        bool operator==(const NovaBigInt &o) const
        {
            return negative == o.negative && limbs == o.limbs;
        }
        bool operator<(const NovaBigInt &o) const
        {
            if (negative != o.negative)
                return negative;
            int c = cmpAbs(*this, o);
            return negative ? c > 0 : c < 0;
        }

        // ── add/sub unsigned ──
        static NovaBigInt addAbs(const NovaBigInt &a, const NovaBigInt &b)
        {
            NovaBigInt r;
            r.limbs.clear();
            uint64_t carry = 0;
            size_t n = std::max(a.limbs.size(), b.limbs.size());
            for (size_t i = 0; i < n || carry; i++)
            {
                uint64_t s = carry;
                if (i < a.limbs.size())
                    s += a.limbs[i];
                if (i < b.limbs.size())
                    s += b.limbs[i];
                r.limbs.push_back((uint32_t)(s % BASE));
                carry = s / BASE;
            }
            return r;
        }
        static NovaBigInt subAbs(const NovaBigInt &a, const NovaBigInt &b)
        {
            // assumes |a| >= |b|
            NovaBigInt r;
            r.limbs.clear();
            int64_t borrow = 0;
            for (size_t i = 0; i < a.limbs.size(); i++)
            {
                int64_t d = (int64_t)a.limbs[i] - borrow - (i < b.limbs.size() ? (int64_t)b.limbs[i] : 0);
                if (d < 0)
                {
                    d += BASE;
                    borrow = 1;
                }
                else
                    borrow = 0;
                r.limbs.push_back((uint32_t)d);
            }
            r.trim();
            return r;
        }

        NovaBigInt operator+(const NovaBigInt &o) const
        {
            if (negative == o.negative)
            {
                auto r = addAbs(*this, o);
                r.negative = negative;
                return r;
            }
            int c = cmpAbs(*this, o);
            if (c == 0)
                return NovaBigInt(0LL);
            if (c > 0)
            {
                auto r = subAbs(*this, o);
                r.negative = negative;
                return r;
            }
            auto r = subAbs(o, *this);
            r.negative = o.negative;
            return r;
        }
        NovaBigInt operator-(const NovaBigInt &o) const
        {
            NovaBigInt neg = o;
            neg.negative = !o.negative;
            return *this + neg;
        }
        NovaBigInt operator*(const NovaBigInt &o) const
        {
            NovaBigInt r;
            r.limbs.assign(limbs.size() + o.limbs.size(), 0);
            for (size_t i = 0; i < limbs.size(); i++)
            {
                uint64_t carry = 0;
                for (size_t j = 0; j < o.limbs.size() || carry; j++)
                {
                    uint64_t cur = (uint64_t)r.limbs[i + j] + (j < o.limbs.size() ? (uint64_t)limbs[i] * o.limbs[j] : 0) + carry;
                    r.limbs[i + j] = (uint32_t)(cur % BASE);
                    carry = cur / BASE;
                }
            }
            r.trim();
            r.negative = negative ^ o.negative;
            if (r.isZero())
                r.negative = false;
            return r;
        }
        // integer division (truncate toward zero)
        NovaBigInt operator/(const NovaBigInt &o) const
        {
            if (o.isZero())
                throw std::runtime_error("BigInt: division by zero");
            // simple long division via repeated subtraction for small divisors
            // full implementation using single-limb short division for speed
            if (o.limbs.size() == 1)
            {
                NovaBigInt r;
                r.limbs.resize(limbs.size());
                uint64_t rem = 0;
                for (int i = (int)limbs.size() - 1; i >= 0; i--)
                {
                    uint64_t cur = rem * (uint64_t)BASE + limbs[i];
                    r.limbs[i] = (uint32_t)(cur / o.limbs[0]);
                    rem = cur % o.limbs[0];
                }
                r.trim();
                r.negative = negative ^ o.negative;
                if (r.isZero())
                    r.negative = false;
                return r;
            }
            // multi-limb: naive O(n^2) long division
            NovaBigInt q(0LL), rem(0LL);
            for (int i = (int)limbs.size() - 1; i >= 0; i--)
            {
                // rem = rem * BASE + limbs[i]
                NovaBigInt base_val(0LL);
                base_val.limbs.clear();
                for (size_t k = 0; k < rem.limbs.size(); k++)
                    base_val.limbs.push_back(rem.limbs[k]);
                base_val.limbs.insert(base_val.limbs.begin(), 0); // *BASE shift
                rem = base_val;
                rem.limbs[0] = limbs[i];
                rem.trim();
                // find digit: how many times does |o| fit in rem?
                uint32_t lo = 0, hi = (uint32_t)BASE - 1;
                while (lo < hi)
                {
                    uint32_t mid = lo + (hi - lo + 1) / 2;
                    NovaBigInt t = o * NovaBigInt((int64_t)mid);
                    t.negative = false;
                    if (cmpAbs(t, rem) <= 0)
                        lo = mid;
                    else
                        hi = mid - 1;
                }
                q.limbs.insert(q.limbs.begin(), (uint32_t)lo);
                NovaBigInt sub = o * NovaBigInt((int64_t)lo);
                sub.negative = false;
                rem.negative = false;
                rem = subAbs(rem, sub);
            }
            q.trim();
            q.negative = negative ^ o.negative;
            if (q.isZero())
                q.negative = false;
            return q;
        }
        NovaBigInt operator%(const NovaBigInt &o) const
        {
            NovaBigInt q = *this / o;
            NovaBigInt r = *this - (q * o);
            return r;
        }
        NovaBigInt pow(uint64_t exp) const
        {
            NovaBigInt result(1LL), base = *this;
            while (exp > 0)
            {
                if (exp & 1)
                    result = result * base;
                base = base * base;
                exp >>= 1;
            }
            return result;
        }
        NovaBigInt abs() const
        {
            NovaBigInt r = *this;
            r.negative = false;
            return r;
        }
        NovaBigInt negate() const
        {
            NovaBigInt r = *this;
            r.negative = !r.negative && !r.isZero();
            return r;
        }

        double toDouble() const
        {
            double r = 0, base = 1;
            for (auto &l : limbs)
            {
                r += l * base;
                base *= BASE;
            }
            return negative ? -r : r;
        }
        std::string toString() const
        {
            if (isZero())
                return "0";
            std::string out;
            for (int i = (int)limbs.size() - 1; i >= 0; i--)
            {
                std::string chunk = std::to_string(limbs[i]);
                if (i != (int)limbs.size() - 1)
                    chunk = std::string(9 - chunk.size(), '0') + chunk;
                out += chunk;
            }
            return (negative ? "-" : "") + out;
        }
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaBigFloat — arbitrary-precision decimal float
    //  Stored as: value = mantissa * 10^exponent  (mantissa is a NovaBigInt)
    //  Default precision: 50 significant decimal digits
    // ════════════════════════════════════════════════════════════════════════════
    struct NovaBigFloat
    {
        NovaBigInt mantissa; // significand (integer, scaled)
        int64_t exponent;    // power of 10
        int precision;       // significant digits to keep

        static constexpr int DEFAULT_PREC = 50;

        NovaBigFloat() : mantissa(0LL), exponent(0), precision(DEFAULT_PREC) {}
        explicit NovaBigFloat(double d, int prec = DEFAULT_PREC) : precision(prec)
        {
            if (d == 0.0)
            {
                mantissa = NovaBigInt(0LL);
                exponent = 0;
                return;
            }
            bool neg = (d < 0);
            if (neg)
                d = -d;
            // represent as string with enough digits then parse
            std::ostringstream ss;
            ss << std::setprecision(prec) << std::fixed << d;
            fromString(ss.str());
            mantissa.negative = neg;
            normalize();
        }
        explicit NovaBigFloat(const NovaBigInt &i, int prec = DEFAULT_PREC)
            : mantissa(i), exponent(0), precision(prec) { normalize(); }
        explicit NovaBigFloat(const std::string &s, int prec = DEFAULT_PREC) : precision(prec)
        {
            fromString(s);
            normalize();
        }

        void fromString(const std::string &s)
        {
            size_t i = 0;
            bool neg = false;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                neg = (s[i++] == '-');
            // find decimal point and 'e'/'E'
            size_t dot = s.find('.', i), epos = s.find_first_of("eE", i);
            int64_t exp_shift = 0;
            std::string digits;
            std::string intPart = s.substr(i, std::min(dot, epos) - i);
            std::string fracPart = (dot != std::string::npos) ? s.substr(dot + 1, (epos != std::string::npos ? epos : s.size()) - dot - 1) : "";
            if (epos != std::string::npos)
                exp_shift = std::stoll(s.substr(epos + 1));
            digits = intPart + fracPart;
            // remove leading zeros
            size_t first = digits.find_first_not_of('0');
            if (first == std::string::npos)
            {
                mantissa = NovaBigInt(0LL);
                exponent = 0;
                return;
            }
            digits = digits.substr(first);
            mantissa = NovaBigInt(digits);
            mantissa.negative = neg;
            exponent = exp_shift - (int64_t)fracPart.size();
        }

        void normalize()
        {
            if (mantissa.isZero())
            {
                exponent = 0;
                return;
            }
            // trim trailing zeros from mantissa, adjust exponent
            std::string s = mantissa.toString();
            bool neg = (s[0] == '-');
            if (neg)
                s = s.substr(1);
            size_t trailing = 0;
            for (int j = (int)s.size() - 1; j >= 0 && s[j] == '0'; j--)
                trailing++;
            if (trailing > 0)
            {
                s = s.substr(0, s.size() - trailing);
                exponent += trailing;
                mantissa = NovaBigInt(s);
                mantissa.negative = neg;
            }
            // enforce precision: trim to `precision` significant digits
            std::string ms = mantissa.toString();
            bool mn = (ms[0] == '-');
            if (mn)
                ms = ms.substr(1);
            if ((int)ms.size() > precision)
            {
                int drop = (int)ms.size() - precision;
                ms = ms.substr(0, precision);
                exponent += drop;
                mantissa = NovaBigInt(ms);
                mantissa.negative = mn;
            }
        }

        // ── align two BigFloats to the same exponent ──
        static std::pair<NovaBigInt, NovaBigInt> align(const NovaBigFloat &a, const NovaBigFloat &b)
        {
            if (a.exponent == b.exponent)
                return {a.mantissa, b.mantissa};
            if (a.exponent > b.exponent)
            {
                // shift a.mantissa left by (a.exponent-b.exponent) decimal places
                int64_t diff = a.exponent - b.exponent;
                if (diff > 100)
                    diff = 100; // guard
                NovaBigInt ten(10LL);
                NovaBigInt scale = ten.pow((uint64_t)diff);
                return {a.mantissa * scale, b.mantissa};
            }
            else
            {
                int64_t diff = b.exponent - a.exponent;
                if (diff > 100)
                    diff = 100;
                NovaBigInt ten(10LL);
                NovaBigInt scale = ten.pow((uint64_t)diff);
                return {a.mantissa, b.mantissa * scale};
            }
        }

        NovaBigFloat operator+(const NovaBigFloat &o) const
        {
            auto [am, bm] = align(*this, o);
            NovaBigFloat r;
            r.precision = std::max(precision, o.precision);
            r.mantissa = am + bm;
            r.exponent = std::min(exponent, o.exponent);
            r.normalize();
            return r;
        }
        NovaBigFloat operator-(const NovaBigFloat &o) const
        {
            auto [am, bm] = align(*this, o);
            NovaBigFloat r;
            r.precision = std::max(precision, o.precision);
            r.mantissa = am - bm;
            r.exponent = std::min(exponent, o.exponent);
            r.normalize();
            return r;
        }
        NovaBigFloat operator*(const NovaBigFloat &o) const
        {
            NovaBigFloat r;
            r.precision = std::max(precision, o.precision);
            r.mantissa = mantissa * o.mantissa;
            r.exponent = exponent + o.exponent;
            r.normalize();
            return r;
        }
        NovaBigFloat operator/(const NovaBigFloat &o) const
        {
            if (o.mantissa.isZero())
                throw std::runtime_error("BigFloat: division by zero");
            // scale dividend up by 10^(precision+extra) then integer divide
            int prec = std::max(precision, o.precision);
            NovaBigInt ten(10LL);
            NovaBigInt scale = ten.pow((uint64_t)(prec + 10));
            NovaBigFloat r;
            r.precision = prec;
            r.mantissa = (mantissa * scale) / o.mantissa;
            r.exponent = exponent - o.exponent - (prec + 10);
            r.normalize();
            return r;
        }

        bool operator==(const NovaBigFloat &o) const
        {
            auto [am, bm] = align(*this, o);
            return am == bm;
        }
        bool operator<(const NovaBigFloat &o) const
        {
            auto [am, bm] = align(*this, o);
            return am < bm;
        }

        double toDouble() const
        {
            // fast path: shift mantissa by exponent
            double m = mantissa.toDouble();
            if (exponent == 0)
                return m;
            if (exponent > 0)
                return m * std::pow(10.0, (double)exponent);
            return m / std::pow(10.0, (double)-exponent);
        }
        std::string toString() const
        {
            if (mantissa.isZero())
                return "0";
            std::string m = mantissa.toString();
            bool neg = (m[0] == '-');
            if (neg)
                m = m.substr(1);
            // reconstruct decimal string
            int64_t exp = exponent;
            // insert decimal point
            std::string out;
            if (exp >= 0)
            {
                out = m + std::string((size_t)exp, '0');
            }
            else
            {
                int64_t decPos = (int64_t)m.size() + exp;
                if (decPos <= 0)
                {
                    out = "0." + std::string((size_t)-decPos, '0') + m;
                }
                else
                {
                    out = m.substr(0, (size_t)decPos) + "." + m.substr((size_t)decPos);
                }
            }
            return (neg ? "-" : "") + out;
        }
        std::string toSciString() const
        {
            if (mantissa.isZero())
                return "0e0";
            std::string m = mantissa.toString();
            bool neg = (m[0] == '-');
            if (neg)
                m = m.substr(1);
            int64_t exp = exponent + (int64_t)m.size() - 1;
            std::string frac = m.size() > 1 ? "." + m.substr(1) : "";
            return (neg ? "-" : "") + m.substr(0, 1) + frac + "e" + std::to_string(exp);
        }
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  NovaNative  — backing storage for nat()/unat()
    // ════════════════════════════════════════════════════════════════════════════

    struct NovaNative
    {
        NativeType type;
        std::shared_ptr<std::vector<uint8_t>> buffer;
        std::shared_ptr<void> handle; // for non-serializable types

        // existing — byte buffer
        NovaNative(NativeType t, std::vector<uint8_t> data)
            : type(t), buffer(std::make_shared<std::vector<uint8_t>>(std::move(data))) {}

        // NEW — handle (streams, file handles, etc.)
        NovaNative(NativeType t, std::shared_ptr<void> h)
            : type(t), handle(std::move(h)) {}

        void *data() const { return buffer ? buffer->data() : nullptr; }
        size_t size() const { return buffer ? buffer->size() : 0; }

        template <typename T>
        T *as() const { return static_cast<T *>(handle.get()); }
    };
    // ════════════════════════════════════════════════════════════════════════════
    //  nativeEncode / nativeDecode  — bytes <-> NovaValue
    // ════════════════════════════════════════════════════════════════════════════

    inline std::vector<uint8_t> nativeEncode(const Val &v, NativeType t)
    {
        if (t == NativeType::Stream)
            return {}; // streams can't be encoded to bytes
        std::vector<uint8_t> buf;
        switch (t)
        {
        case NativeType::I8:
        {
            int8_t x = (int8_t)(v ? v->asNumber() : 0);
            buf.resize(1);
            std::memcpy(buf.data(), &x, 1);
            break;
        }
        case NativeType::U8:
        {
            uint8_t x = (uint8_t)(v ? v->asNumber() : 0);
            buf.resize(1);
            std::memcpy(buf.data(), &x, 1);
            break;
        }
        case NativeType::Bool:
        {
            uint8_t x = (v && v->asBool()) ? 1 : 0;
            buf.resize(1);
            buf[0] = x;
            break;
        }
        case NativeType::I16:
        {
            int16_t x = (int16_t)(v ? v->asNumber() : 0);
            buf.resize(2);
            std::memcpy(buf.data(), &x, 2);
            break;
        }
        case NativeType::U16:
        {
            uint16_t x = (uint16_t)(v ? v->asNumber() : 0);
            buf.resize(2);
            std::memcpy(buf.data(), &x, 2);
            break;
        }
        case NativeType::I32:
        {
            int32_t x = (int32_t)(v ? v->asNumber() : 0);
            buf.resize(4);
            std::memcpy(buf.data(), &x, 4);
            break;
        }
        case NativeType::U32:
        {
            uint32_t x = (uint32_t)(v ? v->asNumber() : 0);
            buf.resize(4);
            std::memcpy(buf.data(), &x, 4);
            break;
        }
        case NativeType::I64:
        {
            int64_t x = (int64_t)(v ? v->asNumber() : 0);
            buf.resize(8);
            std::memcpy(buf.data(), &x, 8);
            break;
        }
        case NativeType::U64:
        {
            uint64_t x = (uint64_t)(v ? v->asNumber() : 0);
            buf.resize(8);
            std::memcpy(buf.data(), &x, 8);
            break;
        }
        case NativeType::F32:
        {
            float x = (float)(v ? v->asNumber() : 0);
            buf.resize(4);
            std::memcpy(buf.data(), &x, 4);
            break;
        }
        case NativeType::F64:
        {
            double x = (v ? v->asNumber() : 0);
            buf.resize(8);
            std::memcpy(buf.data(), &x, 8);
            break;
        }
        case NativeType::Ptr:
        {
            uintptr_t addr = 0;
            if (v && v->isPointer() && v->ptr)
            {
                if (v->ptr->rawAddr)
                    addr = reinterpret_cast<uintptr_t>(v->ptr->rawAddr);
                else if (v->ptr->ptrAddr)
                    addr = reinterpret_cast<uintptr_t>(v->ptr->ptrAddr);
            }
            buf.resize(sizeof(uintptr_t));
            std::memcpy(buf.data(), &addr, sizeof(uintptr_t));
            break;
        }
        case NativeType::CStr:
        {
            std::string s = v ? v->asString() : "";
            buf.assign(s.begin(), s.end());
            buf.push_back('\0');
            break;
        }
        }
        return buf;
    }

    inline Val nativeDecode(const uint8_t *p, size_t size, NativeType t)
    {
        if (t == NativeType::Stream)
            return NovaValue::makeNull();
        if (!p)
            return NovaValue::makeNull();
        switch (t)
        {
        case NativeType::I8:
        {
            int8_t x;
            std::memcpy(&x, p, 1);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::U8:
        {
            uint8_t x;
            std::memcpy(&x, p, 1);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::Bool:
            return NovaValue::makeBool(p[0] != 0);
        case NativeType::I16:
        {
            int16_t x;
            std::memcpy(&x, p, 2);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::U16:
        {
            uint16_t x;
            std::memcpy(&x, p, 2);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::I32:
        {
            int32_t x;
            std::memcpy(&x, p, 4);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::U32:
        {
            uint32_t x;
            std::memcpy(&x, p, 4);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::I64:
        {
            int64_t x;
            std::memcpy(&x, p, 8);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::U64:
        {
            uint64_t x;
            std::memcpy(&x, p, 8);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::F32:
        {
            float x;
            std::memcpy(&x, p, 4);
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::F64:
        {
            double x;
            std::memcpy(&x, p, 8);
            return NovaValue::makeNumber(x);
        }
        case NativeType::Ptr:
        {
            uintptr_t x;
            std::memcpy(&x, p, sizeof(uintptr_t));
            return NovaValue::makeNumber((double)x);
        }
        case NativeType::CStr:
        {
            size_t n = 0;
            while ((size == 0 || n < size) && p[n] != '\0')
                n++;
            return NovaValue::makeString(std::string(reinterpret_cast<const char *>(p), n));
        }
        }
        return NovaValue::makeNull();
    }

    // ── NovaPointer raw read/write (defined here — needs nativeEncode/Decode) ────
    inline Val NovaPointer::readRaw(NativeType t) const
    {
        if (!rawAddr)
            return NovaValue::makeNull();
        size_t sz = nativeTypeSize(t);
        size_t avail = rawSize ? rawSize : sz;
        return nativeDecode(reinterpret_cast<const uint8_t *>(rawAddr), sz == 0 ? avail : sz, t);
    }

    inline void NovaPointer::writeRaw(Val v, NativeType t)
    {
        if (!rawAddr)
            return;
        auto bytes = nativeEncode(v, t);
        size_t n = bytes.size();
        if (rawSize)
            n = std::min(n, rawSize);
        std::memcpy(rawAddr, bytes.data(), n);
    }

    // ── makeNativeVal convenience overloads ──────────────────────────────────────

    inline Val NovaValue::makeNativeVal(const std::string &s)
    {
        std::vector<uint8_t> buf(s.begin(), s.end());
        buf.push_back('\0');
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::NativeVal;
        v->natv = std::make_shared<NovaNative>(NativeType::CStr, std::move(buf));
        return v;
    }

    template <typename StreamT>
    inline Val NovaValue::makeNativeVal(std::shared_ptr<StreamT> stream)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::NativeVal;
        v->natv = std::make_shared<NovaNative>(
            NativeType::Stream,
            std::static_pointer_cast<void>(stream));
        return v;
    }
    template <typename StreamT>
    inline Val NovaValue::makeStream(std::shared_ptr<StreamT> stream)
    {
        static_assert(
            std::is_base_of_v<std::istream, StreamT> ||
                std::is_base_of_v<std::ostream, StreamT>,
            "makeStream<T>: T must derive from std::istream or std::ostream");

        constexpr bool readable = std::is_base_of_v<std::istream, StreamT>;
        constexpr bool writable = std::is_base_of_v<std::ostream, StreamT>;

        Val streamVal = makeNativeVal(stream);
        auto obj = makeObject();

        if constexpr (writable)
        {
            obj->obj->set("write", makeNative([streamVal](ValVec a, auto) -> Val
                                              {
            auto* s = nativeStream<StreamT>(streamVal);
            if (!s || a.empty()) return makeNull();
            *s << a[0]->asString();
            return makeNull(); }, "write"));

            obj->obj->set("writeln", makeNative([streamVal](ValVec a, auto) -> Val
                                                {
            auto* s = nativeStream<StreamT>(streamVal);
            if (!s) return makeNull();
            *s << (a.empty() ? "" : a[0]->asString()) << "\n";
            return makeNull(); }, "writeln"));

            obj->obj->set("flush", makeNative([streamVal](ValVec, auto) -> Val
                                              {
            auto* s = nativeStream<StreamT>(streamVal);
            if (s) s->flush();
            return makeNull(); }, "flush"));
        }

        if constexpr (readable)
        {
            obj->obj->set("readLine", makeNative([streamVal](ValVec, auto) -> Val
                                                 {
            auto* s = nativeStream<StreamT>(streamVal);
            if (!s) return makeNull();
            std::string line;
            std::getline(*s, line);
            return makeString(line); }, "readLine"));

            obj->obj->set("readAll", makeNative([streamVal](ValVec, auto) -> Val
                                                {
            auto* s = nativeStream<StreamT>(streamVal);
            if (!s) return makeNull();
            std::string content(
                (std::istreambuf_iterator<char>(*s)),
                std::istreambuf_iterator<char>{}
            );
            return makeString(content); }, "readAll"));

            obj->obj->set("eof", makeNative([streamVal](ValVec, auto) -> Val
                                            {
            auto* s = nativeStream<StreamT>(streamVal);
            return makeBool(!s || s->eof()); }, "eof"));
        }

        // type tag so Nova code can check: typeof(stdin) == "stream"
        obj->obj->set("__type__", makeString("stream"));
        obj->obj->set("__handle__", streamVal);
        return obj;
    }

    template <typename T>
    inline Val NovaValue::makeNativeVal(T value)
    {
        static_assert(std::is_arithmetic_v<T> || std::is_pointer_v<T>,
                      "makeNativeVal<T>: T must be arithmetic or a pointer "
                      "(use makeNativeVal(std::string)/makeNativeVal(const char*) for strings)");

        NativeType nt;
        if constexpr (std::is_pointer_v<T>)
        {
            nt = NativeType::Ptr;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            nt = NativeType::Bool;
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            nt = (sizeof(T) == 4) ? NativeType::F32 : NativeType::F64;
        }
        else
        { // integral
            constexpr bool sgn = std::is_signed_v<T>;
            if constexpr (sizeof(T) == 1)
                nt = sgn ? NativeType::I8 : NativeType::U8;
            else if constexpr (sizeof(T) == 2)
                nt = sgn ? NativeType::I16 : NativeType::U16;
            else if constexpr (sizeof(T) == 4)
                nt = sgn ? NativeType::I32 : NativeType::U32;
            else
                nt = sgn ? NativeType::I64 : NativeType::U64;
        }

        std::vector<uint8_t> buf;
        if constexpr (std::is_pointer_v<T>)
        {
            uintptr_t addr = reinterpret_cast<uintptr_t>(value);
            buf.resize(sizeof(uintptr_t));
            std::memcpy(buf.data(), &addr, sizeof(uintptr_t));
        }
        else
        {
            buf.resize(sizeof(T));
            std::memcpy(buf.data(), &value, sizeof(T));
        }

        auto v = std::make_shared<NovaValue>();
        v->kind = VK::NativeVal;
        v->natv = std::make_shared<NovaNative>(nt, std::move(buf));
        return v;
    }

    template <typename T>
    inline T *nativeStream(Val v)
    {
        if (!v || v->kind != VK::NativeVal || !v->natv ||
            v->natv->type != NativeType::Stream || !v->natv->handle)
            return nullptr;
        return static_cast<T *>(v->natv->handle.get());
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  ERROR LEVELS
    // ════════════════════════════════════════════════════════════════════════════

    enum class ErrorLevel
    {
        Moderate = 1,
        Critical = 2,
        Fatal = 3,
        Signal = 4
    };

    struct NovaCriticalError
    {
        std::string message;
        Val payload;
        bool resumable = false;
        NovaCriticalError(std::string m, Val p = nullptr)
            : message(std::move(m)), payload(p) {}
    };

    struct NovaFatalError
    {
        std::string message;
        Val payload;
        NovaFatalError(std::string m, Val p = nullptr)
            : message(std::move(m)), payload(p) {}
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  FACTORY IMPLEMENTATIONS (inline for header-only)
    // ════════════════════════════════════════════════════════════════════════════

    inline Val NovaValue::makeBigInt(int64_t v)
    {
        auto nv = std::make_shared<NovaValue>();
        nv->kind = VK::BigInt;
        nv->bigint = std::make_shared<NovaBigInt>(v);
        return nv;
    }
    inline Val NovaValue::makeBigInt(const std::string &s)
    {
        auto nv = std::make_shared<NovaValue>();
        nv->kind = VK::BigInt;
        nv->bigint = std::make_shared<NovaBigInt>(s);
        return nv;
    }
    inline Val NovaValue::makeBigInt(NovaBigInt bi)
    {
        auto nv = std::make_shared<NovaValue>();
        nv->kind = VK::BigInt;
        nv->bigint = std::make_shared<NovaBigInt>(std::move(bi));
        return nv;
    }
    inline Val NovaValue::makeBigFloat(double v, int prec)
    {
        auto nv = std::make_shared<NovaValue>();
        nv->kind = VK::BigFloat;
        nv->bigfloat = std::make_shared<NovaBigFloat>(v, prec);
        return nv;
    }
    inline Val NovaValue::makeBigFloat(const std::string &s, int prec)
    {
        auto nv = std::make_shared<NovaValue>();
        nv->kind = VK::BigFloat;
        nv->bigfloat = std::make_shared<NovaBigFloat>(s, prec);
        return nv;
    }
    inline Val NovaValue::makeBigFloat(NovaBigFloat bf)
    {
        auto nv = std::make_shared<NovaValue>();
        nv->kind = VK::BigFloat;
        nv->bigfloat = std::make_shared<NovaBigFloat>(std::move(bf));
        return nv;
    }

    inline Val NovaValue::makeNull()
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Null;
        return v;
    }
    inline Val NovaValue::makeBool(bool b)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Bool;
        v->bval = b;
        return v;
    }
    inline Val NovaValue::makeNumber(double n)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Number;
        v->nval = n;
        return v;
    }
    inline Val NovaValue::makeString(const std::string &s)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::String;
        v->sval = s;
        return v;
    }
    inline Val NovaValue::makeString(std::string &&s)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::String;
        v->sval = std::move(s);
        return v;
    }
    inline Val NovaValue::makeArray(ValVec elems)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Array;
        v->arr = std::make_shared<NovaArray>(std::move(elems));
        return v;
    }
    inline Val NovaValue::makeObject(ValMap props)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Object;
        v->obj = std::make_shared<NovaObject>(std::move(props));
        return v;
    }
    inline Val NovaValue::makeRange(double start, double end, double step)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Range;
        v->range = std::make_shared<NovaRange>(start, end, step);
        return v;
    }
    inline Val NovaValue::makeStruct(const std::string &typeName, ValMap fields)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Struct;
        v->strct = std::make_shared<NovaStruct>(typeName, std::move(fields));
        return v;
    }
    inline Val NovaValue::makeEnum(const std::string &typeName, const std::string &variant, Val value)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Enum;
        v->enm = std::make_shared<NovaEnum>(typeName, variant, value);
        return v;
    }
    inline Val NovaValue::makePointer(Val inner,
                                      std::function<Val()> readFn,
                                      std::function<void(Val)> writeFn,
                                      std::string address)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Pointer;
        v->ptr = std::make_shared<NovaPointer>(inner, readFn, writeFn, address);
        return v;
    }
    inline Val NovaValue::makeNative(std::function<Val(ValVec, std::shared_ptr<Scope>)> fn, std::string name)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Native;
        v->fn = std::make_shared<NovaFunction>();
        v->fn->isNative = true;
        v->fn->native = std::move(fn);
        v->fn->name = std::move(name);
        return v;
    }
    inline Val NovaValue::makeFunction(std::shared_ptr<Node> node, std::shared_ptr<Scope> closureScope)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::Function;
        v->fn = std::make_shared<NovaFunction>();
        v->fn->node = node;
        v->fn->closureScope = closureScope;
        v->fn->isNative = false;
        return v;
    }

    inline Val NovaValue::makeNativeVal(NativeType type, std::vector<uint8_t> data)
    {
        auto v = std::make_shared<NovaValue>();
        v->kind = VK::NativeVal;
        v->natv = std::make_shared<NovaNative>(type, std::move(data));
        return v;
    }

    // ── coercions ────────────────────────────────────────────────────────────────
    inline bool NovaValue::asBool() const
    {
        switch (kind)
        {
        case VK::Null:
            return false;
        case VK::Bool:
            return bval;
        case VK::Number:
            return nval != 0.0 && !std::isnan(nval);
        case VK::String:
            return !sval.empty();
        case VK::NativeVal: // NEW
            return natv ? nativeDecode((const uint8_t *)natv->data(), natv->size(), natv->type)->asBool() : false;
        case VK::BigInt:
            return bigint && !bigint->isZero();
        case VK::BigFloat:
            return bigfloat && !bigfloat->mantissa.isZero();
        default:
            return true;
        }
    }

    inline double NovaValue::asNumber() const
    {
        switch (kind)
        {
        case VK::Number:
            return nval;
        case VK::Bool:
            return bval ? 1.0 : 0.0;
        case VK::Null:
            return 0.0;
        case VK::NativeVal: // NEW
            return natv ? nativeDecode((const uint8_t *)natv->data(), natv->size(), natv->type)->asNumber()
                        : std::numeric_limits<double>::quiet_NaN();
        case VK::String:
        {
            try
            {
                size_t p;
                return std::stod(sval, &p);
            }
            catch (...)
            {
                return std::numeric_limits<double>::quiet_NaN();
            }
        }
        case VK::BigInt:
            return bigint ? bigint->toDouble() : 0.0;
        case VK::BigFloat:
            return bigfloat ? bigfloat->toDouble() : 0.0;
        default:
            return std::numeric_limits<double>::quiet_NaN();
        }
    }

    inline std::string NovaValue::asString() const
    {
        switch (kind)
        {
        case VK::Class:
            return "[Class " + (cls ? cls->name : "?") + "]";
        case VK::Null:
            return "null";
        case VK::Bool:
            return bval ? "true" : "false";
        case VK::Number:
        { /* unchanged */
            if (std::isnan(nval))
                return "NaN";
            if (std::isinf(nval))
                return nval > 0 ? "Infinity" : "-Infinity";
            if (nval == std::floor(nval) && std::abs(nval) < 1e15)
            {
                std::ostringstream ss;
                ss << (long long)nval;
                return ss.str();
            }
            std::ostringstream ss;
            ss << nval;
            return ss.str();
        }
        case VK::String:
            return sval;
        case VK::NativeVal:
        {
            if (!natv)
                return "<native>";
            if (natv->type == NativeType::Stream)
                return "<stream@0x" + [&]
                { std::ostringstream ss; ss << std::hex << (uintptr_t)natv->handle.get(); return ss.str(); }() + ">";
            Val decoded = nativeDecode((const uint8_t *)natv->data(), natv->size(), natv->type);
            return "<native " + nativeTypeName(natv->type) + ": " + decoded->asString() + ">";
        }
        case VK::Array:
            return arr ? arr->toString() : "[]";
        case VK::Object:
            return obj ? obj->toString() : "{}";
        case VK::Range:
            return range ? range->toString() : "Range()";
        case VK::Struct:
            return strct ? strct->toString() : "{}";
        case VK::Enum:
            return enm ? enm->toString() : "<enum>";
        case VK::Pointer:
            return ptr ? ptr->toString() : "<ptr>";
        case VK::Function:
        case VK::Native:
            return fn ? "[Function " + fn->name + "]" : "[Function]";
        case VK::BigInt:
            return bigint ? bigint->toString() : "0";
        case VK::BigFloat:
            return bigfloat ? bigfloat->toString() : "0";
        default:
            return "<value>";
        }
    }

    inline std::string NovaValue::typeName() const
    {
        switch (kind)
        {
        case VK::Class:
            return "class";
        case VK::Null:
            return "null";
        case VK::Bool:
            return "bool";
        case VK::Number:
            return "number";
        case VK::String:
            return "string";
        case VK::Array:
            return "array";
        case VK::Object:
            return "object";
        case VK::Function:
        case VK::Native:
            return "function";
        case VK::Range:
            return "range";
        case VK::Struct:
            return strct ? "struct:" + strct->typeName : "struct";
        case VK::Enum:
            return enm ? "enum:" + enm->typeName : "enum";
        case VK::Pointer:
            return "pointer";
        case VK::NativeVal:
            return natv ? "native:" + nativeTypeName(natv->type) : "native"; // NEW
        default:
        case VK::BigInt:
            return "bigint";
        case VK::BigFloat:
            return "bigfloat";
            return "unknown";
        }
    }
    inline bool NovaValue::operator==(const NovaValue &o) const
    {
        if (kind != o.kind)
            return false;
        switch (kind)
        {
        case VK::Null:
            return true;
        case VK::Bool:
            return bval == o.bval;
        case VK::Number:
            return nval == o.nval;
        case VK::String:
            return sval == o.sval;
        case VK::NativeVal: // NEW
            if (natv && o.natv)
                return natv->type == o.natv->type && *natv->buffer == *o.natv->buffer;
            return false;
        case VK::Enum:
            if (enm && o.enm)
                return enm->typeName == o.enm->typeName && enm->variant == o.enm->variant;
            return false;
        case VK::BigInt:
            return bigint && o.bigint && *bigint == *o.bigint;
        case VK::BigFloat:
            return bigfloat && o.bigfloat && *bigfloat == *o.bigfloat;
        default:
            return false;
        }
    }
    inline bool NovaValue::operator<(const NovaValue &o) const
    {
        if (isString() && o.isString())
            return sval < o.sval;
        if (kind == VK::BigInt && o.kind == VK::BigInt && bigint && o.bigint)
            return *bigint < *o.bigint;
        if (kind == VK::BigFloat && o.kind == VK::BigFloat && bigfloat && o.bigfloat)
            return *bigfloat < *o.bigfloat;
        return asNumber() < o.asNumber();
    }

    // ── NovaArray methods ────────────────────────────────────────────────────────
    inline Val NovaArray::get(int i) const
    {
        if (i < 0)
            i = (int)inner.size() + i;
        if (i < 0 || i >= (int)inner.size())
            return TVal::null();
        return inner[i];
    }
    inline void NovaArray::set(int i, Val v)
    {
        if (i < 0)
            i = (int)inner.size() + i;
        if (i >= (int)inner.size())
            inner.resize(i + 1, TVal::null());
        inner[i] = v;
    }
    inline Val NovaArray::pop()
    {
        if (inner.empty())
            return TVal::null();
        Val v = inner.back();
        inner.pop_back();
        return v;
    }
    inline Val NovaArray::shift()
    {
        if (inner.empty())
            return TVal::null();
        Val v = inner.front();
        inner.erase(inner.begin());
        return v;
    }
    inline std::string NovaArray::toString() const
    {
        std::string out = "[";
        for (size_t i = 0; i < inner.size(); i++)
        {
            if (i)
                out += ",";
            out += inner[i] ? inner[i]->asString() : "null";
        }
        return out + "]";
    }

    // ── NovaObject methods ───────────────────────────────────────────────────────
    inline Val NovaObject::get(const std::string &k) const
    {
        if (meta)
        {
            auto it = inner.find(k);
            if (it != inner.end())
            {
                return meta->get ? meta->get(k, it->second) : it->second;
            }
            return meta->missing ? meta->missing(k) : nullptr;
        }
        auto it = inner.find(k);
        return it != inner.end() ? it->second : nullptr;
    }
    inline void NovaObject::set(const std::string &k, Val v)
    {
        if (meta && meta->set)
        {
            meta->set(k, v, inner);
            return;
        }
        inner[k] = v;
    }
    inline std::vector<std::string> NovaObject::keys() const
    {
        std::vector<std::string> ks;
        ks.reserve(inner.size());
        for (auto &[k, _] : inner)
            ks.push_back(k);
        return ks;
    }
    inline ValVec NovaObject::values() const
    {
        ValVec vs;
        vs.reserve(inner.size());
        for (auto &[_, v] : inner)
            vs.push_back(v);
        return vs;
    }
    inline std::string NovaObject::toString() const
    {
        std::string out = "{";
        bool first = true;
        for (auto &[k, v] : inner)
        {
            if (!first)
                out += ",";
            out += k + ":" + (v ? v->asString() : "null");
            first = false;
        }
        return out + "}";
    }

    // ── NovaStruct::toString ─────────────────────────────────────────────────────
    inline std::string NovaStruct::toString() const
    {
        std::string out = typeName + " { ";
        bool first = true;
        for (auto &[k, v] : inner)
        {
            if (!first)
                out += ", ";
            out += k + ": " + (v ? v->asString() : "null");
            first = false;
        }
        return out + " }";
    }

    // ── NovaRange::toArray ───────────────────────────────────────────────────────
    inline ValVec NovaRange::toArray() const
    {
        ValVec out;
        if (step > 0)
        {
            for (double i = start; i <= end; i += step)
                out.push_back(TVal::number(i)); // inline — no heap allocation
        }
        else
        {
            for (double i = start; i >= end; i += step)
                out.push_back(TVal::number(i));
        }
        return out;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  TVal — 32-byte tagged value (Stage 3: eliminates heap allocation for
    //  null / bool / double — the hot path for numeric loops)
    //
    //  Layout (x86-64):
    //    offset  0: shared_ptr<NovaValue> _heap  (16 bytes)  — null for primitives
    //    offset 16: double _n                    ( 8 bytes)  — Number payload
    //    offset 24: uint8_t _tag                ( 1 byte )
    //    offset 25: bool    _b                  ( 1 byte )
    //    offset 26: padding                     ( 6 bytes)
    //  Total: 32 bytes
    //
    //  For Null/Bool/Num: _heap is empty, no allocation, no atomic ops.
    //  For everything else: _heap holds the shared_ptr as before.
    //  operator->() returns _heap.get() for heap types; asserts/crashes for
    //  primitives (callers must check .isNumber() etc. before field access).
    // ════════════════════════════════════════════════════════════════════════════

    // ── Val is now TVal ──────────────────────────────────────────────────────────

    // ════════════════════════════════════════════════════════════════════════════
    //  TVal method implementations (NovaValue is now complete)
    // ════════════════════════════════════════════════════════════════════════════

    inline std::shared_ptr<NovaValue> TVal::asShared() const
    {
        if (_tag == THeap)
            return _heap;
        if (_tag == TUndef)
            return nullptr; // "not found" → null shared_ptr
        // Lazy-box primitive Nova value into heap NovaValue
        auto v = std::make_shared<NovaValue>();
        switch (_tag)
        {
        case TNul:
            v->kind = VK::Null;
            break;
        case TBool:
            v->kind = VK::Bool;
            v->bval = _b;
            break;
        case TNum:
            v->kind = VK::Number;
            v->nval = _n;
            break;
        default:
            break;
        }
        return v;
    }

    inline NovaValue *TVal::_ensureBox() const
    {
        // operator->() fallback for primitive tags (Null/Bool/Num/Undef) — builds
        // and caches a real NovaValue* so existing v->field call sites across the
        // codebase keep working unmodified (e.g. v->overloads, v->kind, v->bval).
        if (_lazyBox)
            return _lazyBox.get();
        _lazyBox = std::make_shared<NovaValue>();
        switch (_tag)
        {
        case TNul:
            _lazyBox->kind = VK::Null;
            break;
        case TBool:
            _lazyBox->kind = VK::Bool;
            _lazyBox->bval = _b;
            break;
        case TNum:
            _lazyBox->kind = VK::Number;
            _lazyBox->nval = _n;
            break;
        case TUndef:
            _lazyBox->kind = VK::Null;
            break; // "not found" reads as null
        default:
            break;
        }
        return _lazyBox.get();
    }

    // Type checks
    inline bool TVal::isString() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::String; }
    inline bool TVal::isArray() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::Array; }
    inline bool TVal::isObject() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::Object; }
    inline bool TVal::isFunction() const noexcept { return _tag == THeap && _heap && (_heap->kind == VK::Function || _heap->kind == VK::Native); }
    inline bool TVal::isRange() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::Range; }
    inline bool TVal::isStruct() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::Struct; }
    inline bool TVal::isEnum() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::Enum; }
    inline bool TVal::isPointer() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::Pointer; }
    inline bool TVal::isClass() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::Class; }
    inline bool TVal::isBigInt() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::BigInt; }
    inline bool TVal::isBigFloat() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::BigFloat; }
    inline bool TVal::isNativeVal() const noexcept { return _tag == THeap && _heap && _heap->kind == VK::NativeVal; }

    // Coercions
    inline bool TVal::asBool() const
    {
        switch (_tag)
        {
        case TNul:
            return false;
        case TUndef:
            return false;
        case TBool:
            return _b;
        case TNum:
            return _n != 0.0 && !std::isnan(_n);
        case THeap:
            return _heap ? _heap->asBool() : false;
        }
        return false;
    }
    inline double TVal::asNumber() const
    {
        switch (_tag)
        {
        case TNul:
            return 0.0;
        case TBool:
            return _b ? 1.0 : 0.0;
        case TNum:
            return _n;
        case THeap:
            return _heap ? _heap->asNumber() : 0.0;
        }
        return 0.0;
    }
    inline std::string TVal::asString() const
    {
        switch (_tag)
        {
        case TNul:
            return "null";
        case TBool:
            return _b ? "true" : "false";
        case TNum:
        {
            if (std::isnan(_n))
                return "NaN";
            if (std::isinf(_n))
                return _n > 0 ? "Infinity" : "-Infinity";
            if (_n == std::floor(_n) && std::abs(_n) < 1e15)
            {
                std::ostringstream ss;
                ss << (long long)_n;
                return ss.str();
            }
            std::ostringstream ss;
            ss << _n;
            return ss.str();
        }
        case THeap:
            return _heap ? _heap->asString() : "null";
        }
        return "null";
    }
    inline VK TVal::kind() const
    {
        switch (_tag)
        {
        case TUndef:
            return VK::Null;
        case TNul:
            return VK::Null;
        case TBool:
            return VK::Bool;
        case TNum:
            return VK::Number;
        case THeap:
            return _heap ? _heap->kind : VK::Null;
        }
        return VK::Null;
    }

    // Equality / ordering
    inline bool TVal::operator==(const TVal &o) const
    {
        if (_tag == TUndef || o._tag == TUndef)
            return false;
        // Fast path: same tag
        if (_tag == TBool && o._tag == TBool)
            return _b == o._b;
        if (_tag == TNum && o._tag == TNum)
            return _n == o._n;
        if (_tag == TNul && o._tag == TNul)
            return true;
        // Heap comparison
        if (_tag == THeap && o._tag == THeap && _heap && o._heap)
            return *_heap == *o._heap;
        // Cross-type: delegate to asNumber for numeric coercion
        if ((_tag == TNum || _tag == TBool) && (o._tag == TNum || o._tag == TBool))
            return asNumber() == o.asNumber();
        return false;
    }
    inline bool TVal::operator<(const TVal &o) const
    {
        if (isString() && o.isString() && _heap && o._heap)
            return _heap->sval < o._heap->sval;
        return asNumber() < o.asNumber();
    }

    // ── convenience aliases (Stage 3: null/bool/num are now inline, no heap alloc)
    inline Val nova_null() { return TVal::null(); }
    inline Val nova_bool(bool b) { return TVal::boolean(b); }
    inline Val nova_num(double n) { return TVal::number(n); }
    inline Val nova_str(std::string s) { return TVal(NovaValue::makeString(std::move(s))); }
    inline Val nova_arr(ValVec v = {}) { return TVal(NovaValue::makeArray(std::move(v))); }
    inline Val nova_obj(ValMap m = {}) { return TVal(NovaValue::makeObject(std::move(m))); }

    // sentinel used when `default` is passed as an argument
    inline Val DEFAULT_SENTINEL()
    {
        static auto s = []()
        {
            auto v = std::make_shared<NovaValue>();
            v->kind = VK::String;
            v->sval = "__nova:default__";
            return v;
        }();
        return s;
    }
    inline bool isDefault(Val v)
    {
        return v && v->isString() && v->sval == "__nova:default__";
    }

} // namespace novac