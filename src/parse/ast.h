#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <variant>
#include <unordered_map>

namespace novac {

// ── forward declaration ──────────────────────────────────────────────────────
struct Node;
using NodePtr  = std::shared_ptr<Node>;
using NodeList = std::vector<NodePtr>;

// ── type expression (used in annotations, declarations, interfaces) ──────────
struct TypeExpr {
    enum class Kind { Named, Union, Intersect, Shape, Array, Function, Value };
    Kind kind = Kind::Named;

    std::string             name;
    std::vector<TypeExpr>   args;
    bool                    optional = false;
    std::string             literalValue;

    // Shape fields use shared_ptr so TypeExpr remains copyable
    struct ShapeField {
        std::shared_ptr<TypeExpr> type;
        bool optional = false;
    };
    std::unordered_map<std::string, ShapeField> fields;

    // Array / Function element type
    std::shared_ptr<TypeExpr> of;

    TypeExpr() = default;
    TypeExpr(const TypeExpr&) = default;
    TypeExpr(TypeExpr&&)      = default;
    TypeExpr& operator=(const TypeExpr&) = default;
    TypeExpr& operator=(TypeExpr&&)      = default;
};

// ── function argument ────────────────────────────────────────────────────────
struct FuncArg {
    std::string            name;
    bool                   rest         = false;
    NodePtr                defaultValue;
    std::optional<TypeExpr> type;
};

// ── object/array destructure patterns ───────────────────────────────────────
struct ObjPatternProp {
    std::string key, alias;
    NodePtr     defaultValue;
};
struct ObjPattern { std::vector<ObjPatternProp> props; };

struct ArrPatternElem {
    std::string name;
    bool        rest = false;
    NodePtr     defaultValue;
};
struct ArrPattern { std::vector<ArrPatternElem> elements; };

using DestructurePattern = std::variant<std::monostate, ObjPattern, ArrPattern>;

// ── smart variable modifiers ─────────────────────────────────────────────────
struct VarModifiers {
    NodePtr     setter, getter;
    bool        frozen   = false;
    bool        lazy     = false;
    bool        tracked  = false;
    bool        nonull   = false;
    bool        once     = false;
    std::string clampType;   // "fnum" | "fint"
    NodePtr     clampExpr;
};

// ── enum/struct/interface helpers ────────────────────────────────────────────
struct EnumVariant {
    std::string name;
    NodePtr     value;   // null, tuple types, or expr
};

struct StructField {
    std::string             name;
    TypeExpr                type;
    NodePtr                 defaultValue;
    NodeList                decorators;
};

struct InterfaceMember {
    std::string             name;
    bool                    isMethod   = false;
    bool                    isOptional = false;
    std::vector<std::pair<std::string, std::optional<TypeExpr>>> params;
    std::optional<TypeExpr> returnType;
};

struct ClassMember {
    enum class Kind { Method, Field };
    Kind        kind;
    std::string name;
    std::string accessor;   // "get" | "set" | ""
    NodeList    decorators;
    // method
    std::vector<FuncArg>    args;
    NodeList                body;
    std::optional<TypeExpr> returnType;
    // field
    NodePtr                 value;
    std::optional<TypeExpr> fieldType;
};

struct ServerRoute {
    std::string             method;
    NodePtr                 path;
    std::vector<std::string> params;
    NodeList                body;
    int line = 0, column = 0;
};

struct HandleClause {
    std::string              name;
    std::vector<std::string> params;
    NodeList                 body;
};

struct CaseNode {
    enum class Kind { Case, Default };
    Kind     kind;
    NodePtr  value;   // null for default
    NodeList body;
    int line = 0, column = 0;
};

struct WhenNode {
    NodeList patterns;
    NodeList body;
    int line = 0, column = 0;
};

struct ObjectProp {
    enum class Kind { Normal, Computed, Spread, Accessor };
    Kind        kind    = Kind::Normal;
    std::string key;
    NodePtr     keyExpr;   // computed
    NodePtr     value;
    std::string accessor;  // "get" | "set"
    bool        boolProp = false;
};

// ════════════════════════════════════════════════════════════════════════════
//  NODE
// ════════════════════════════════════════════════════════════════════════════

struct Node {
    enum class Kind {
        // ── program ──
        Program,

        // ── declarations ──
        Declare, Function, Class,
        TypeDecl, StructDecl, InterfaceDecl, EnumDecl, TraitDecl, ImplDecl,
        Namespace,

        // ── statements ──
        Exec, Return, Break, Continue, Goback,
        Branch,       // if/while/for/repeat/until/unless/do/else
        ForOf, ForIn, Each,
        Switch, Match,
        Try,
        EmitEvent, OnEvent,
        WithCtx, WithOption,
        Import, ImportBuiltin, ImportKit, FromImport,
        Export, DefaultExport,
        Server,
        FetchStmt,
        ExecComment,
        Block,
        DotCmd,

        // ── expressions ──
        Value,        // literal number/string/bool/null
        Ref,          // identifier reference
        DVar,         // __line__ etc.
        Array,        // [elements]
        Object,       // {props}
        FString,      // f"..." parts
        Regex,        // /pattern/flags
        UrlLiteral,

        Unary, Prefix, Postfix, Binary,
        Ternary, IfTernary,
        Assign, CompoundAssign,
        Call, OptionalCall, StructCall,
        Prop, OptionalProp,
        Subscript, OptionalSubscript,
        Spread,
        ArrowFunc,
        Await,
        Yield,
        New,
        Cast,
        Deref,
        Link,
        RateCast,
        Classify,
        Partial,
        Native, Unative,
        Throw,
        Perform,
        Run,
        Ast,
        HasOpt, GetOpt, OptDecl,
        HttpRequest,
        FetchExpr,
    };

    Kind        kind;
    int         line   = 0;
    int         column = 0;

    // ── literal value ──
    std::string         strval;   // string/identifier/operator value
    double              numval = 0;
    // For Kind::Value — distinguishes the original token type so the VM
    // doesn't need to guess from content (e.g. string "1.0.0" vs number 1.0.0)
    enum class ValueKind { Number, String, Literal } valueKind = ValueKind::String;

    // ── generic children ──
    NodePtr             left, right, operand, value, callee;
    NodeList            body, args, elements, parts, decorators;
    NodePtr             cond, consequent, alternate;
    NodePtr             next;    // else branch

    // ── names ──
    std::string         name;    // var name, function name, prop name, etc.
    std::string         op;      // operator string

    // ── types ──
    std::optional<TypeExpr>    typeAnnotation;
    std::optional<TypeExpr>    returnType;
    std::optional<TypeExpr>    explicitType;

    // ── declaration ──
    bool                isConst   = false;
    bool                isPointer = false;
    DestructurePattern  destructure;
    std::shared_ptr<VarModifiers> modifiers;

    // ── function / arrow ──
    std::vector<FuncArg>  funcArgs;
    bool                  isAsync     = false;
    bool                  isGenerator = false;
    bool                  strictArgs  = false;
    bool                  memoize     = false;
    bool                  once_fn     = false;
    bool                  defer_fn    = false;
    NodePtr               deferStmt;
    NodePtr               timeout;

    // ── class ──
    NodePtr                  superClass;
    std::vector<std::string> impls;
    std::vector<ClassMember> members;

    // ── type decls ──
    std::vector<std::string>   typeParams;
    TypeExpr                   typeDef;
    std::vector<StructField>   structFields;
    std::vector<InterfaceMember> ifaceMembers;
    std::vector<std::string>   extendsNames;
    std::vector<EnumVariant>   enumVariants;
    std::string                traitName, forType;

    // ── branch / for / each ──
    std::string            branchType;   // "if","while","for","do",...
    NodePtr                init, update;
    std::string            varName, indexName;
    NodePtr                iterable, object;

    // ── switch / match ──
    NodePtr                subject;
    std::vector<CaseNode>  cases;
    std::vector<WhenNode>  whenCases;

    // ── try ──
    NodeList               tryBody, catchBody, elseBody, finallyBody;
    std::string            catchName, catchResume;
    std::vector<HandleClause> handleClauses;

    // ── object literal ──
    std::vector<ObjectProp> props;

    // ── call / subscript ──
    NodeList               callArgs;
    NodePtr                index;

    // ── dvar ──
    int                    srcLine = 0, srcCol = 0;

    // ── import / export ──
    NodePtr                source;
    std::vector<std::string> names;
    std::string              kitName;

    // ── server ──
    NodePtr                port;
    std::vector<ServerRoute> routes;

    // ── http ──
    std::string            method;
    NodePtr                url;

    // ── fetch ──
    NodePtr                options;
    std::string            varNameOut;   // fetch => varName

    // ── misc ──
    std::string            flags;        // regex flags
    bool                   terminate = false;  // give (hard return)
    bool                   isExpr    = false;  // throw-as-expression
    std::string            flag;               // with option flag
    std::string            castType;           // rate_cast target type

    // ── dot_cmd ──
    NodePtr                cmd;

    // ── with_ctx / namespace ──
    NodePtr                target;

    // ── emit / on ──
    NodePtr                event;
    std::string            param;
};

// ── factory helpers ──────────────────────────────────────────────────────────
inline NodePtr makeNode(Node::Kind k, int line = 0, int col = 0) {
    auto n    = std::make_shared<Node>();
    n->kind   = k;
    n->line   = line;
    n->column = col;
    return n;
}

} // namespace novac