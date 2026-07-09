#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <functional>
#include <vector>
#include <cstdint>

namespace novac {

// ════════════════════════════════════════════════════════════════════════════
//  TOKEN
// ════════════════════════════════════════════════════════════════════════════

enum class TT {
    // literals
    NUMBER, STRING, STRING_PART, FSTRING_START, FSTRING_END,
    INTERPOLATION_START, INTERPOLATION_END,
    LITERAL,      // true / false / null
    URL,
    // identifiers
    IDENTIFIER,
    KEYWORD,
    DVAR,         // __line__, __file__, etc.
    EXEC_COMMENT, // /?/ ... /
    // operators / punctuation
    OPERATOR,
    PUNCTUATION,
    // RC_* family (RC_REST, RC_stop, RC_<keyword>)
    RC,
    // sentinel
    EOF_TOKEN,
};

struct Token {
    TT          type;
    std::string value;      // string representation
    double      numval  = 0;
    int         line    = 1;
    int         column  = 1;
    // operator metadata
    int         precedence = 0;
    bool        isUnary    = false;
    bool        isPostfix  = false;
    bool        rightAssoc = false;
    // DVAR baked source location
    int         srcLine = 0;
    int         srcCol  = 0;
};

// ════════════════════════════════════════════════════════════════════════════
//  KEYWORDS
// ════════════════════════════════════════════════════════════════════════════

inline const std::unordered_set<std::string> KEYWORDS = {
    // core
    "var","let","const","class","if","else","for","repeat","unless","until",
    "throw","try","catch","finally","function","return","give","yield","goback",
    "async","await","do","while","block",
    // control
    "switch","case","default","break","continue","rate",
    "of","each","match","as",
    // meta
    "emit","on","new","extends","link","run","nat","unat","perform","partial","using",
    // types
    "type","struct","interface","enum","trait","impl",
    // accessors
    "get","set",
    // smart variable modifiers
    "setter","getter","frozen","lazy","tracked","nonull","once",
    // numeric constraint types
    "fnum","fint",
    // misc
    "from","export","namespace","with","handle","import","import_builtin","import_kit",
    // http / server
    "server","fetch","post","put","delete","patch","head","options",
};

// ════════════════════════════════════════════════════════════════════════════
//  DYNAMIC VARS
// ════════════════════════════════════════════════════════════════════════════

inline const std::unordered_set<std::string> DYNAMIC_VARS = {
    "default",
    "__line__","__col__","__file__","__date__","__time__","__timestamp__",
    "__datetime__","__version__","__pid__","__platform__","__arch__",
    "__argv__","__env__","__scope__","__caller__","__module__",
    "__namespace__","__random__","__uuid__","__iter__","__stack__",
};

// ════════════════════════════════════════════════════════════════════════════
//  OPERATORS
// ════════════════════════════════════════════════════════════════════════════

struct OpInfo {
    int  precedence = 0;
    bool isUnary    = false;
    bool isPostfix  = false;
    bool rightAssoc = false;
    bool custom     = false;
};

// Ordered map so longest-match scanning works (we scan by length, not order,
// but keeping it ordered aids debugging).
inline std::map<std::string, OpInfo>& operators() {
    static std::map<std::string, OpInfo> OPS = {
        // assignment (0)
        {"=",    {0}},
        {"=>",   {0}},
        {"|>",   {0}},
        {"+=",   {0}},
        {"-=",   {0}},
        {"*=",   {0}},
        {"/=",   {0}},
        {"%=",   {0}},
        {"**=",  {0}},
        {"&&=",  {0}},
        {"||=",  {0}},
        {"?\?=",  {0}},
        // logical (2-4)
        {"||",   {2}},
        {"??",   {3}},
        {"&&",   {4}},
        // bitwise (5-7)
        {"|",    {5}},
        {"^",    {6}},
        {"&",    {7}},
        // equality (8)
        {"===",  {8}},
        {"!==",  {8}},
        {"==",   {8}},
        {"!=",   {8}},
        // relational (9)
        {"<=",   {9}},
        {">=",   {9}},
        {"<",    {9}},
        {">",    {9}},
        // shift / range (10)
        {">>>",  {10}},
        {">>",   {10}},
        {"<<",   {10}},
        {"..",   {10}},
        // additive (11)
        {"+",    {11,true}},
        {"-",    {11,true}},
        // multiplicative (12)
        {"*",    {12,true}},
        {"/",    {12}},
        {"%",    {12}},
        // exponent right-assoc (13)
        {"**",   {13,false,false,true}},
        // unary (14)
        {"!",    {14,true}},
        {"~",    {14,true}},
        {"delete",{14,true}},
        // postfix (15)
        {"deg",  {15,false,true}},
        {"++",   {15,true,true}},
        {"--",   {15,true,true}},
        {"?.",   {15}},
        // spread / chain / namespace
        {"...",  {0,true}},
        {"::",   {15}},
        {"#:",   {15}},
        {"$:",   {15}},
        // keyword operators
        {"and",         {4}},
        {"or",          {2}},
        {"not",         {14,true}},
        {"typeof",      {14,true}},
        {"void",        {14,true}},
        {"instanceof",  {8}},
        {"in",          {8}},
        // Nova Classic
        {"pow",         {13,false,false,true}},
        {"bigger",      {9}},
        {"smaller",     {9}},
        {"equals",      {8}},
        {"xor",         {3}},
        {"nand",        {4}},
        {"nor",         {2}},
        {"xnor",        {3}},
        {"is",          {8}},
        {"isnt",        {8}},
        {"istypeof",    {8}},
        {"matches",     {8}},
        {"between",     {8}},
        {"step",        {10}},
        {"zip",         {10}},
        {"intersect",   {7}},
        {"diff_arr",    {7}},
        {"union",       {5}},
        {"avg",         {11}},
        {"diff",        {11}},
        {"ratio",       {12}},
        {"mult_of",     {12}},
        {"gcd",         {12}},
        {"lcm",         {12}},
        {"pad_start",   {11}},
        {"pad_end",     {11}},
        {"cmp",         {8}},
        {"equals_ignore",{8}},
        {"extend",      {5}},
        {"concat",      {11}},
    };
    return OPS;
}

// ════════════════════════════════════════════════════════════════════════════
//  PUNCTUATION
// ════════════════════════════════════════════════════════════════════════════

inline const std::unordered_set<char> PUNCTUATION = {
    '(',')','{','}','[',']',',','.',':', ';','?','@','\\',
};

// ════════════════════════════════════════════════════════════════════════════
//  H_UNIT  (numeric suffix literals: 0hbillion etc.)
// ════════════════════════════════════════════════════════════════════════════

inline const std::unordered_map<std::string, double> H_UNIT = {
    {"million",     1e6},  {"billion",     1e9},  {"trillion",    1e12},
    {"quadrillion", 1e15}, {"quintillion", 1e18}, {"sextillion",  1e21},
    {"septillion",  1e24}, {"octillion",   1e27}, {"nonillion",   1e30},
    {"decillion",   1e33}, {"googol",      1e100},
    {"yotta",1e24},{"zetta",1e21},{"exa",1e18},{"peta",1e15},{"tera",1e12},
    {"giga",1e9},  {"mega",1e6}, {"kilo",1e3}, {"hecto",1e2},{"deca",1e1},
    {"deci",1e-1}, {"centi",1e-2},{"milli",1e-3},{"micro",1e-6},{"nano",1e-9},
    {"pico",1e-12},{"femto",1e-15},{"atto",1e-18},{"zepto",1e-21},{"yocto",1e-24},
};

// ════════════════════════════════════════════════════════════════════════════
//  ESCAPE TABLES
// ════════════════════════════════════════════════════════════════════════════

// ── Foreground colors ────────────────────────────────────────────────────────
inline const std::unordered_map<std::string, std::string> FG = {
    {"black","\x1b[30m"},{"red","\x1b[31m"},{"green","\x1b[32m"},
    {"yellow","\x1b[33m"},{"blue","\x1b[34m"},{"magenta","\x1b[35m"},
    {"cyan","\x1b[36m"},{"white","\x1b[37m"},
    {"brightblack","\x1b[90m"},{"brightred","\x1b[91m"},{"brightgreen","\x1b[92m"},
    {"brightyellow","\x1b[93m"},{"brightblue","\x1b[94m"},{"brightmagenta","\x1b[95m"},
    {"brightcyan","\x1b[96m"},{"brightwhite","\x1b[97m"},
    {"grey","\x1b[90m"},{"gray","\x1b[90m"},{"orange","\x1b[33m"},
    {"pink","\x1b[95m"},{"purple","\x1b[35m"},{"violet","\x1b[35m"},
    {"lime","\x1b[92m"},{"teal","\x1b[36m"},{"navy","\x1b[34m"},
    {"maroon","\x1b[31m"},{"olive","\x1b[33m"},{"aqua","\x1b[36m"},
    {"silver","\x1b[37m"},{"gold","\x1b[33m"},{"indigo","\x1b[34m"},
    {"coral","\x1b[91m"},{"salmon","\x1b[91m"},{"khaki","\x1b[33m"},
    {"lavender","\x1b[95m"},{"turquoise","\x1b[96m"},{"crimson","\x1b[31m"},
    {"ivory","\x1b[97m"},{"beige","\x1b[97m"},{"tan","\x1b[33m"},
    {"sienna","\x1b[31m"},{"amber","\x1b[33m"},{"emerald","\x1b[32m"},
    {"ruby","\x1b[31m"},{"sapphire","\x1b[34m"},{"jade","\x1b[32m"},
    {"default","\x1b[39m"},
};

// ── Background colors ────────────────────────────────────────────────────────
inline const std::unordered_map<std::string, std::string> BG = {
    {"black","\x1b[40m"},{"red","\x1b[41m"},{"green","\x1b[42m"},
    {"yellow","\x1b[43m"},{"blue","\x1b[44m"},{"magenta","\x1b[45m"},
    {"cyan","\x1b[46m"},{"white","\x1b[47m"},
    {"brightblack","\x1b[100m"},{"brightred","\x1b[101m"},{"brightgreen","\x1b[102m"},
    {"brightyellow","\x1b[103m"},{"brightblue","\x1b[104m"},{"brightmagenta","\x1b[105m"},
    {"brightcyan","\x1b[106m"},{"brightwhite","\x1b[107m"},
    {"grey","\x1b[100m"},{"gray","\x1b[100m"},{"orange","\x1b[43m"},
    {"pink","\x1b[105m"},{"purple","\x1b[45m"},{"default","\x1b[49m"},
};

// ── SGR text styles ──────────────────────────────────────────────────────────
inline const std::unordered_map<std::string, std::string> TEXT_TYPES = {
    {"reset","\x1b[0m"},{"bold","\x1b[1m"},{"dim","\x1b[2m"},
    {"italic","\x1b[3m"},{"underline","\x1b[4m"},{"blink","\x1b[5m"},
    {"reverse","\x1b[7m"},{"hidden","\x1b[8m"},{"strikethrough","\x1b[9m"},
    {"resetbold","\x1b[21m"},{"resetdim","\x1b[22m"},{"resetitalic","\x1b[23m"},
    {"resetunderline","\x1b[24m"},{"resetblink","\x1b[25m"},
    {"resetreverse","\x1b[27m"},{"resethidden","\x1b[28m"},{"resetstrike","\x1b[29m"},
    {"overline","\x1b[53m"},{"resetoverline","\x1b[55m"},
    {"doubleunderline","\x1b[21m"},
    {"b","\x1b[1m"},{"i","\x1b[3m"},{"u","\x1b[4m"},
    {"s","\x1b[9m"},{"r","\x1b[0m"},{"inv","\x1b[7m"},
};

// ── No-arg named escapes — defined in lexer.cpp to avoid SIOF ───────────────
const std::unordered_map<std::string, std::string>& escapeSequences();

} // namespace novac