// src/cli/port.cpp
// novac port <file> — transpile JS to Nova
// Uses tree-sitter for parsing.
#include "port.h"
#include "../../tree-sitter-deps/tree-sitter/lib/include/tree_sitter/api.h"
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

extern "C"
{
    TSLanguage *tree_sitter_javascript();
    TSLanguage *tree_sitter_python();
}

// ── helpers ───────────────────────────────────────────────────────────────

static std::string readFile(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open file: " + path);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

static std::string nodeText(TSNode node, const std::string &src)
{
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    return src.substr(start, end - start);
}

static TSNode childByField(TSNode node, const char *fieldName)
{
    return ts_node_child_by_field_name(node, fieldName, (uint32_t)strlen(fieldName));
}

static bool nodeValid(TSNode node)
{
    return !ts_node_is_null(node);
}

static std::string indent(int depth)
{
    return std::string(depth * 2, ' ');
}

// ── forward declarations ──────────────────────────────────────────────────
static std::string genNode(TSNode node, const std::string &src, int depth);
static std::string genExpr(TSNode node, const std::string &src);

// ── strip outer parens from a node if it's parenthesized_expression ──────
static TSNode unwrapParens(TSNode node)
{
    if (!ts_node_is_null(node) &&
        std::string(ts_node_type(node)) == "parenthesized_expression")
    {
        return ts_node_child(node, 1); // skip opening (
    }
    return node;
}

// ── remap JS identifiers to Nova equivalents ─────────────────────────────
static std::string remapIdent(const std::string &name)
{
    if (name == "console")
        return "Std";
    if (name == "Math")
        return "Std.Math";
    if (name == "JSON")
        return "Std.JSON";
    if (name == "process")
        return "Std.process";
    if (name == "parseInt")
        return "Std.Number.parseInt";
    if (name == "parseFloat")
        return "Std.Number.parseFloat";
    if (name == "isNaN")
        return "Std.Number.isNaN";
    if (name == "isFinite")
        return "Std.Number.isFinite";
    if (name == "undefined")
        return "null";
    if (name == "Infinity")
        return "Std.Number.INFINITY";
    if (name == "NaN")
        return "Std.Number.NaN";
    return name;
}

// ── remap JS operators to Nova equivalents ────────────────────────────────
static std::string remapOp(const std::string &op)
{
    if (op == "===")
        return "==";
    if (op == "!==")
        return "!=";
    if (op == "&&")
        return "and";
    if (op == "||")
        return "or";
    if (op == "!")
        return "not ";
    return op;
}

// ── remap console.X calls ─────────────────────────────────────────────────
static std::string remapCallTarget(const std::string &s)
{
    if (s == "Std.log")
        return "Std.println";
    if (s == "Std.error")
        return "Std.stderr.writeln";
    if (s == "Std.warn")
        return "Std.stderr.writeln";
    if (s == "Std.info")
        return "Std.println";
    if (s == "console.log")
        return "Std.println";
    if (s == "console.error")
        return "Std.stderr.writeln";
    if (s == "console.warn")
        return "Std.stderr.writeln";
    return s;
}

// ── collect comma-separated child nodes (skipping punctuation) ────────────
static std::vector<TSNode> collectChildren(TSNode node)
{
    std::vector<TSNode> result;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode c = ts_node_child(node, i);
        std::string t = ts_node_type(c);
        if (t == "(" || t == ")" || t == "[" || t == "]" ||
            t == "{" || t == "}" || t == "," || t == ";")
            continue;
        result.push_back(c);
    }
    return result;
}

// ── generate a comma-separated argument list ─────────────────────────────
static std::string genArgList(TSNode argsNode, const std::string &src)
{
    std::string out = "(";
    auto children = collectChildren(argsNode);
    for (size_t i = 0; i < children.size(); i++)
    {
        if (i > 0)
            out += ", ";
        out += genExpr(children[i], src);
    }
    return out + ")";
}

// ── generate parameter list for function/arrow ───────────────────────────
static std::string genParams(TSNode paramsNode, const std::string &src)
{
    std::string out = "(";
    uint32_t count = ts_node_child_count(paramsNode);
    bool first = true;
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode p = ts_node_child(paramsNode, i);
        std::string pt = ts_node_type(p);
        if (pt == "(" || pt == ")" || pt == ",")
            continue;

        if (!first)
            out += ", ";
        first = false;

        if (pt == "assignment_pattern")
        {
            // default param: x = val
            TSNode left = childByField(p, "left");
            TSNode right = childByField(p, "right");
            out += nodeText(left, src) + " = " + genExpr(right, src);
        }
        else if (pt == "rest_pattern")
        {
            TSNode inner = ts_node_child(p, 1);
            out += "..." + nodeText(inner, src);
        }
        else if (pt == "identifier")
        {
            out += nodeText(p, src);
        }
        else
        {
            out += genExpr(p, src);
        }
    }
    return out + ")";
}

// ════════════════════════════════════════════════════════════════════════════
//  EXPRESSION CODEGEN
// ════════════════════════════════════════════════════════════════════════════

static std::string genExpr(TSNode node, const std::string &src)
{
    if (ts_node_is_null(node))
        return "";
    std::string type = ts_node_type(node);

    // ── this ──────────────────────────────────────────────────────────────
    if (type == "this")
        return "this";

    // ── super ─────────────────────────────────────────────────────────────
    if (type == "super")
        return "Super";

    // ── literals ──────────────────────────────────────────────────────────
    if (type == "number")
        return nodeText(node, src);
    if (type == "string")
        return nodeText(node, src);
    if (type == "true")
        return "true";
    if (type == "false")
        return "false";
    if (type == "null")
        return "null";

    // ── identifier ────────────────────────────────────────────────────────
    if (type == "identifier")
    {
        return remapIdent(nodeText(node, src));
    }

    // ── property identifier (inside member expressions) ───────────────────
    if (type == "property_identifier")
    {
        return nodeText(node, src);
    }

    // ── template literal → f-string ───────────────────────────────────────
    if (type == "template_string")
    {
        std::string out = "f\"";
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode c = ts_node_child(node, i);
            std::string ct = ts_node_type(c);
            if (ct == "`")
                continue;
            if (ct == "template_substitution")
            {
                // ${expr} → {expr}
                uint32_t cc = ts_node_child_count(c);
                for (uint32_t j = 0; j < cc; j++)
                {
                    TSNode ic = ts_node_child(c, j);
                    std::string ict = ts_node_type(ic);
                    if (ict == "${" || ict == "}")
                        continue;
                    out += "{" + genExpr(ic, src) + "}";
                }
            }
            else if (ct == "template_characters")
            {
                out += nodeText(c, src);
            }
        }
        return out + "\"";
    }

    // ── binary expression ─────────────────────────────────────────────────
    if (type == "binary_expression")
    {
        TSNode left = childByField(node, "left");
        TSNode op = childByField(node, "operator");
        TSNode right = childByField(node, "right");
        std::string opStr = remapOp(nodeText(op, src));
        return genExpr(left, src) + " " + opStr + " " + genExpr(right, src);
    }

    // ── unary expression ──────────────────────────────────────────────────
    if (type == "unary_expression")
    {
        TSNode op = childByField(node, "operator");
        if (nodeText(op, src) == "typeof")
        {
            TSNode arg = childByField(node, "argument");
            return "Std.typeOf(" + genExpr(arg, src) + ")";
        }
        TSNode operand = childByField(node, "argument");
        std::string opStr = remapOp(nodeText(op, src));
        return opStr + genExpr(operand, src);
    }

    // ── update expression (i++, i--) ──────────────────────────────────────
    if (type == "update_expression")
    {
        TSNode arg = childByField(node, "argument");
        TSNode opNode = childByField(node, "operator");
        // check prefix vs postfix
        bool prefix = ts_node_start_byte(opNode) < ts_node_start_byte(arg);
        std::string op = nodeText(opNode, src);
        return prefix ? op + genExpr(arg, src) : genExpr(arg, src) + op;
    }

    // ── assignment expression ─────────────────────────────────────────────
    // ── assignment expression ─────────────────────────────────────────────────
    if (type == "assignment_expression")
    {
        TSNode left = childByField(node, "left");
        TSNode right = childByField(node, "right");
        // '=' is an anonymous token in the TS grammar — NOT a named field.
        // augmented_assignment_expression handles +=, -=, etc. separately.
        return genExpr(left, src) + " = " + genExpr(right, src);
    }

    // ── augmented assignment (+=, -= etc) ─────────────────────────────────
    if (type == "augmented_assignment_expression")
    {
        TSNode left = childByField(node, "left");
        TSNode right = childByField(node, "right");
        TSNode op = childByField(node, "operator");
        return genExpr(left, src) + " " + nodeText(op, src) + " " + genExpr(right, src);
    }

    // ── ternary ───────────────────────────────────────────────────────────
    if (type == "ternary_expression")
    {
        TSNode cond = childByField(node, "condition");
        TSNode consequent = childByField(node, "consequence");
        TSNode alternate = childByField(node, "alternative");
        return genExpr(cond, src) + " ? " +
               genExpr(consequent, src) + " : " +
               genExpr(alternate, src);
    }

    // ── call expression ───────────────────────────────────────────────────
    if (type == "call_expression")
    {
        TSNode func = childByField(node, "function");
        TSNode args = childByField(node, "arguments");
        std::string funcStr = remapCallTarget(genExpr(func, src));
        return funcStr + genArgList(args, src);
    }

    // ── member expression obj.prop ────────────────────────────────────────
    if (type == "member_expression")
    {
        TSNode obj = childByField(node, "object");
        TSNode prop = childByField(node, "property");
        std::string objStr = genExpr(obj, src);
        // remap known objects
        objStr = remapCallTarget(objStr);
        return objStr + "." + nodeText(prop, src);
    }

    // ── subscript expression obj[key] ─────────────────────────────────────
    if (type == "subscript_expression")
    {
        TSNode obj = childByField(node, "object");
        TSNode index = childByField(node, "index");
        return genExpr(obj, src) + "[" + genExpr(index, src) + "]";
    }

    // ── new expression → Nova: just call without 'new' ────────────────────
    if (type == "new_expression")
    {
        TSNode ctor = childByField(node, "constructor");
        TSNode args = childByField(node, "arguments");
        std::string ctorStr = genExpr(ctor, src);
        if (nodeValid(args))
        {
            return ctorStr + genArgList(args, src);
        }
        return ctorStr + "()";
    }

    // ── array ─────────────────────────────────────────────────────────────
    if (type == "array")
    {
        std::string out = "[";
        auto children = collectChildren(node);
        for (size_t i = 0; i < children.size(); i++)
        {
            if (i > 0)
                out += ", ";
            out += genExpr(children[i], src);
        }
        return out + "]";
    }

    // ── object ────────────────────────────────────────────────────────────
    if (type == "object")
    {
        std::string out = "{ ";
        uint32_t count = ts_node_child_count(node);
        bool first = true;
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode c = ts_node_child(node, i);
            std::string ct = ts_node_type(c);
            if (ct == "{" || ct == "}" || ct == ",")
                continue;

            if (!first)
                out += ", ";
            first = false;

            if (ct == "pair")
            {
                TSNode key = childByField(c, "key");
                TSNode val = childByField(c, "value");
                // key may be string, identifier, or property_identifier
                std::string keyStr = nodeText(key, src);
                out += keyStr + ": " + genExpr(val, src);
            }
            else if (ct == "shorthand_property_identifier" ||
                     ct == "shorthand_property_identifier_pattern")
            {
                std::string name = nodeText(c, src);
                out += name + ": " + name;
            }
            else if (ct == "spread_element")
            {
                TSNode arg = ts_node_child(c, 1);
                out += "..." + genExpr(arg, src);
            }
            else if (ct == "method_definition")
            {
                // method shorthand in object: { foo() {} }
                TSNode mname = childByField(c, "name");
                TSNode params = childByField(c, "parameters");
                TSNode mbody = childByField(c, "body");
                out += nodeText(mname, src) + genParams(params, src) + " {\n";
                out += genNode(mbody, src, 1);
                out += "}";
            }
        }
        return out + " }";
    }

    // ── arrow function ────────────────────────────────────────────────────
    if (type == "arrow_function")
    {
        TSNode params = childByField(node, "parameters");
        if (ts_node_is_null(params))
            params = childByField(node, "parameter"); // singular fallback
        TSNode body = childByField(node, "body");

        std::string paramStr;
        if (ts_node_is_null(params))
        {
            // single param without parens — find the identifier child
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; i++)
            {
                TSNode c = ts_node_child(node, i);
                if (std::string(ts_node_type(c)) == "identifier")
                {
                    paramStr = nodeText(c, src);
                    break;
                }
            }
        }
        else
        {
            paramStr = genParams(params, src);
        }

        std::string bodyStr;
        std::string bt = ts_node_type(body);
        if (bt == "statement_block")
        {
            bodyStr = " {\n" + genNode(body, src, 1) + "}";
        }
        else
        {
            bodyStr = " " + genExpr(body, src);
        }
        return paramStr + " =>" + bodyStr;
    }

    // ── await ─────────────────────────────────────────────────────────────
    if (type == "await_expression")
    {
        TSNode arg = childByField(node, "expression");
        if (!nodeValid(arg))
            arg = ts_node_child(node, 1);
        return "await " + genExpr(arg, src);
    }

    // ── spread ────────────────────────────────────────────────────────────
    if (type == "spread_element")
    {
        TSNode arg = ts_node_child(node, 1);
        return "..." + genExpr(arg, src);
    }

    // ── parenthesized expression ──────────────────────────────────────────
    if (type == "parenthesized_expression")
    {
        TSNode inner = ts_node_child(node, 1);
        return "(" + genExpr(inner, src) + ")";
    }

    // ── sequence expression (a, b) ────────────────────────────────────────
    if (type == "sequence_expression")
    {
        TSNode left = childByField(node, "left");
        TSNode right = childByField(node, "right");
        return genExpr(left, src) + ", " + genExpr(right, src);
    }

    // ── regex literal → just emit as string comment ───────────────────────
    if (type == "regex")
    {
        return "// [regex] " + nodeText(node, src);
    }

    // ── fallback: emit raw source ─────────────────────────────────────────
    return nodeText(node, src);
}

// ════════════════════════════════════════════════════════════════════════════
//  STATEMENT / DECLARATION CODEGEN
// ════════════════════════════════════════════════════════════════════════════

static std::string genNode(TSNode node, const std::string &src, int depth)
{
    if (ts_node_is_null(node))
        return "";
    std::string type = ts_node_type(node);
    std::string ind = indent(depth);
    std::string out;

    // ── program / statement_block: iterate children ────────────────────────
    if (type == "program" || type == "statement_block")
    {
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode c = ts_node_child(node, i);
            std::string ct = ts_node_type(c);
            if (ct == "{" || ct == "}")
                continue;
            out += genNode(c, src, depth);
        }
        return out;
    }

    // ── comment ───────────────────────────────────────────────────────────
    if (type == "comment")
    {
        return ind + nodeText(node, src) + "\n";
    }

    // ── variable / lexical declaration ────────────────────────────────────
    if (type == "lexical_declaration" || type == "variable_declaration")
    {
        TSNode kindNode = ts_node_child(node, 0);
        std::string kind = nodeText(kindNode, src); // const / let / var
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 1; i < count; i++)
        {
            TSNode c = ts_node_child(node, i);
            if (std::string(ts_node_type(c)) == "variable_declarator")
            {
                TSNode name = childByField(c, "name");
                TSNode value = childByField(c, "value");
                out += ind + kind + " " + nodeText(name, src);
                if (nodeValid(value))
                {
                    out += " = " + genExpr(value, src);
                }
                out += ";\n";
            }
        }
        return out;
    }

    // ── function declaration ───────────────────────────────────────────────
    if (type == "function_declaration" || type == "generator_function_declaration")
    {
        TSNode nameNode = childByField(node, "name");
        TSNode paramsNode = childByField(node, "parameters");
        TSNode bodyNode = childByField(node, "body");

        // detect async: first child token is "async"
        bool isAsync = false;
        if (nodeValid(ts_node_child(node, 0)))
        {
            std::string ft = nodeText(ts_node_child(node, 0), src);
            if (ft == "async")
                isAsync = true;
        }
        bool isGen = (type == "generator_function_declaration");

        std::string prefix = "function";
        if (isAsync)
            prefix += " async";
        if (isGen)
            prefix += " generator";

        std::string funcName = nodeValid(nameNode) ? nodeText(nameNode, src) : "";
        out += ind + prefix + " " + funcName + genParams(paramsNode, src) + " {\n";
        out += genNode(bodyNode, src, depth + 1);
        out += ind + "}\n";
        return out;
    }

    // ── return statement ───────────────────────────────────────────────────
    if (type == "return_statement")
    {
        uint32_t count = ts_node_child_count(node);
        if (count > 1)
        {
            TSNode val = ts_node_child(node, 1);
            std::string vt = ts_node_type(val);
            if (vt != ";")
            {
                out += ind + "return " + genExpr(val, src) + ";\n";
                return out;
            }
        }
        out += ind + "return;\n";
        return out;
    }

    // ── expression statement ───────────────────────────────────────────────
    if (type == "expression_statement")
    {
        TSNode expr = ts_node_child(node, 0);
        out += ind + genExpr(expr, src) + ";\n";
        return out;
    }

    // ── if statement ───────────────────────────────────────────────────────
    if (type == "if_statement")
    {
        TSNode cond = childByField(node, "condition");
        TSNode consequent = childByField(node, "consequence");
        TSNode alternative = childByField(node, "alternative");

        std::string condStr = genExpr(unwrapParens(cond), src);
        out += ind + "if " + condStr + " {\n";
        out += genNode(consequent, src, depth + 1);
        out += ind + "}";

        if (nodeValid(alternative))
        {
            std::string altType = ts_node_type(alternative);
            if (altType == "else_clause")
            {
                TSNode altBody = ts_node_child(alternative, 1); // skip "else" keyword
                std::string altBodyType = ts_node_type(altBody);
                if (altBodyType == "if_statement")
                {
                    // else if
                    std::string altStr = genNode(altBody, src, depth);
                    out += " else " + altStr.substr(ind.size());
                    return out;
                }
                else
                {
                    out += " else {\n";
                    out += genNode(altBody, src, depth + 1);
                    out += ind + "}";
                }
            }
            else if (altType == "if_statement")
            {
                std::string altStr = genNode(alternative, src, depth);
                out += " else " + altStr.substr(ind.size());
                return out;
            }
            else
            {
                out += " else {\n";
                out += genNode(alternative, src, depth + 1);
                out += ind + "}";
            }
        }
        out += "\n";
        return out;
    }

    // ── while statement ────────────────────────────────────────────────────
    if (type == "while_statement")
    {
        TSNode cond = childByField(node, "condition");
        TSNode body = childByField(node, "body");
        out += ind + "while " + genExpr(unwrapParens(cond), src) + " {\n";
        out += genNode(body, src, depth + 1);
        out += ind + "}\n";
        return out;
    }

    // ── do-while statement ─────────────────────────────────────────────────
    if (type == "do_statement")
    {
        TSNode body = childByField(node, "body");
        TSNode cond = childByField(node, "condition");
        out += ind + "do {\n";
        out += genNode(body, src, depth + 1);
        out += ind + "} while " + genExpr(unwrapParens(cond), src) + ";\n";
        return out;
    }

    // ── for statement (C-style) ────────────────────────────────────────────
    if (type == "for_statement")
    {
        TSNode init = childByField(node, "initializer");
        TSNode cond = childByField(node, "condition");
        TSNode update = childByField(node, "increment");
        TSNode body = childByField(node, "body");

        std::string initStr;
        if (nodeValid(init))
        {
            initStr = genNode(init, src, 0);
            while (!initStr.empty() && (initStr.back() == '\n' || initStr.back() == ';'))
                initStr.pop_back();
        }
        std::string condStr = nodeValid(cond) ? genExpr(cond, src) : "true";
        std::string updStr = nodeValid(update) ? genExpr(update, src) : "";

        out += ind + "for " + initStr + "; " + condStr + "; " + updStr + " {\n";
        out += genNode(body, src, depth + 1);
        out += ind + "}\n";
        return out;
    }

    // ── for-of / for-in ───────────────────────────────────────────────────
    if (type == "for_in_statement")
    {
        TSNode left = childByField(node, "left");
        TSNode right = childByField(node, "right");
        TSNode body = childByField(node, "body");

        // detect of vs in by scanning for "of" keyword child
        bool isOf = false;
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode c = ts_node_child(node, i);
            if (!ts_node_is_named(c))
            {
                std::string tok = nodeText(c, src);
                if (tok == "of")
                {
                    isOf = true;
                    break;
                }
                if (tok == "in")
                {
                    isOf = false;
                    break;
                }
            }
        }

        // left side: strip const/let/var, emit as "let x"
        std::string leftStr;
        std::string leftType = ts_node_type(left);
        if (leftType == "identifier")
        {
            leftStr = "let " + nodeText(left, src);
        }
        else if (leftType == "lexical_declaration" || leftType == "variable_declaration")
        {
            // get the declarator name
            uint32_t count = ts_node_child_count(left);
            for (uint32_t i = 0; i < count; i++)
            {
                TSNode c = ts_node_child(left, i);
                if (std::string(ts_node_type(c)) == "variable_declarator")
                {
                    TSNode nm = childByField(c, "name");
                    leftStr = "let " + nodeText(nm, src);
                    break;
                }
            }
        }
        else
        {
            leftStr = genExpr(left, src);
        }

        std::string keyword = isOf ? "of" : "in";
        out += ind + "for " + leftStr + " " + keyword + " " + genExpr(right, src) + " {\n";
        out += genNode(body, src, depth + 1);
        out += ind + "}\n";
        return out;
    }

    // ── class declaration ──────────────────────────────────────────────────
    if (type == "class_declaration" || type == "class")
    {
        TSNode nameNode = childByField(node, "name");
        TSNode superNode = childByField(node, "superclass");
        TSNode bodyNode = childByField(node, "body");

        std::string className = nodeValid(nameNode) ? nodeText(nameNode, src) : "";
        out += ind + "class " + className;
        if (nodeValid(superNode))
            out += " extends " + nodeText(superNode, src);
        out += " {\n";

        // separate fields from methods
        uint32_t count = ts_node_child_count(bodyNode);
        std::vector<TSNode> methods;
        std::vector<TSNode> fields;

        for (uint32_t i = 0; i < count; i++)
        {
            TSNode m = ts_node_child(bodyNode, i);
            std::string mt = ts_node_type(m);
            if (mt == "{" || mt == "}")
                continue;
            if (mt == "method_definition")
                methods.push_back(m);
            else if (mt == "field_definition")
                fields.push_back(m);
        }

        // emit fields (Nova style: name: defaultValue)
        for (auto &f : fields)
        {
            TSNode fname = childByField(f, "name");
            TSNode fval = childByField(f, "value");
            out += indent(depth + 1) + nodeText(fname, src) + ": ";
            out += nodeValid(fval) ? genExpr(fval, src) : "null";
            out += "\n";
        }
        if (!fields.empty() && !methods.empty())
            out += "\n";

        // emit methods
        for (auto &m : methods)
        {
            TSNode mname = childByField(m, "name");
            TSNode params = childByField(m, "parameters");
            TSNode mbody = childByField(m, "body");

            std::string mnameStr = nodeValid(mname) ? nodeText(mname, src) : "";

            // detect static
            bool isStatic = false;
            uint32_t mc = ts_node_child_count(m);
            for (uint32_t i = 0; i < mc; i++)
            {
                if (nodeText(ts_node_child(m, i), src) == "static")
                {
                    isStatic = true;
                    break;
                }
            }

            std::string prefix = isStatic ? "static " : "";
            out += indent(depth + 1) + prefix + mnameStr + genParams(params, src) + " {\n";
            out += genNode(mbody, src, depth + 2);
            out += indent(depth + 1) + "}\n\n";
        }

        out += ind + "}\n";
        return out;
    }

    // ── throw statement ────────────────────────────────────────────────────
    if (type == "throw_statement")
    {
        TSNode val = ts_node_child(node, 1);
        out += ind + "throw " + genExpr(val, src) + ";\n";
        return out;
    }

    // ── try statement ──────────────────────────────────────────────────────
    if (type == "try_statement")
    {
        TSNode body = childByField(node, "body");
        TSNode handler = childByField(node, "handler");
        TSNode fin = childByField(node, "finalizer");

        out += ind + "try {\n";
        out += genNode(body, src, depth + 1);
        out += ind + "}";

        if (nodeValid(handler))
        {
            TSNode param = childByField(handler, "parameter");
            TSNode hbody = childByField(handler, "body");
            std::string pStr = nodeValid(param) ? nodeText(param, src) : "e";
            out += " catch " + pStr + " {\n";
            out += genNode(hbody, src, depth + 1);
            out += ind + "}";
        }
        if (nodeValid(fin))
        {
            TSNode fbody = childByField(fin, "body");
            if (ts_node_is_null(fbody))
                fbody = fin;
            out += " finally {\n";
            out += genNode(fbody, src, depth + 1);
            out += ind + "}";
        }
        out += "\n";
        return out;
    }

    // ── switch statement ───────────────────────────────────────────────────
    if (type == "switch_statement")
    {
        TSNode val = childByField(node, "value");
        TSNode body = childByField(node, "body");
        out += ind + "switch " + genExpr(unwrapParens(val), src) + " {\n";
        uint32_t count = ts_node_child_count(body);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode c = ts_node_child(body, i);
            std::string ct = ts_node_type(c);
            if (ct == "{" || ct == "}")
                continue;
            if (ct == "switch_case")
            {
                TSNode cval = childByField(c, "value");
                out += indent(depth + 1) + "case " + genExpr(cval, src) + ": ";
                uint32_t bc = ts_node_child_count(c);
                bool first = true;
                for (uint32_t j = 0; j < bc; j++)
                {
                    TSNode b = ts_node_child(c, j);
                    std::string bt = ts_node_type(b);
                    if (bt == "case" || bt == ":")
                        continue;
                    if (nodeValid(childByField(c, "value")) &&
                        ts_node_start_byte(b) <= ts_node_end_byte(childByField(c, "value")))
                        continue;
                    if (first)
                    {
                        out += "\n";
                        first = false;
                    }
                    out += genNode(b, src, depth + 2);
                }
            }
            else if (ct == "switch_default")
            {
                out += indent(depth + 1) + "default:\n";
                uint32_t bc = ts_node_child_count(c);
                for (uint32_t j = 0; j < bc; j++)
                {
                    TSNode b = ts_node_child(c, j);
                    std::string bt = ts_node_type(b);
                    if (bt == "default" || bt == ":")
                        continue;
                    out += genNode(b, src, depth + 2);
                }
            }
        }
        out += ind + "}\n";
        return out;
    }

    // ── break / continue ───────────────────────────────────────────────────
    if (type == "break_statement")
        return ind + "break;\n";
    if (type == "continue_statement")
        return ind + "continue;\n";

    // ── import → Std.include ──────────────────────────────────────────────
    if (type == "import_declaration")
    {
        TSNode source = childByField(node, "source");
        std::string path = nodeText(source, src);
        out += ind + "Std.include(" + path + ");\n";
        return out;
    }

    // ── export: unwrap inner declaration ──────────────────────────────────
    if (type == "export_statement")
    {
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode c = ts_node_child(node, i);
            std::string ct = ts_node_type(c);
            if (ct == "export" || ct == "default")
                continue;
            out += genNode(c, src, depth);
        }
        return out;
    }

    // ── empty statement ────────────────────────────────────────────────────
    if (type == "empty_statement")
        return "";

    // ── labeled statement (skip label, emit body) ──────────────────────────
    if (type == "labeled_statement")
    {
        TSNode body = childByField(node, "body");
        return genNode(body, src, depth);
    }

    // ── debugger statement ─────────────────────────────────────────────────
    if (type == "debugger_statement")
    {
        return ind + "// debugger\n";
    }

    // ── fallback: emit unported comment ───────────────────────────────────
    out += ind + "// [unported: " + type + "] " + nodeText(node, src) + "\n";
    return out;
}

// ── language detection ─────────────────────────────────────────────────────

static std::string detectLang(const std::string &file)
{
    auto ext = fs::path(file).extension().string();
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs")
        return "javascript";
    if (ext == ".ts")
        return "typescript";
    if (ext == ".py")
        return "python";
    return "";
}

// ── entry point ────────────────────────────────────────────────────────────

int cmdPort(const std::string &file, const std::string &targetLang)
{
    (void)targetLang;

    std::string lang = detectLang(file);
    if (lang.empty())
    {
        std::cerr << "port: cannot detect language for: " << file << "\n"
                  << "  supported: .js .mjs .cjs .py\n";
        return 1;
    }
    if (lang == "python")
    {
        std::cerr << "port: Python support coming soon\n";
        return 1;
    }

    std::string src;
    try
    {
        src = readFile(file);
    }
    catch (const std::exception &e)
    {
        std::cerr << "port: " << e.what() << "\n";
        return 1;
    }

    TSParser *parser = ts_parser_new();
    TSLanguage *tsLang = tree_sitter_javascript();

    if (!ts_parser_set_language(parser, tsLang))
    {
        std::cerr << "port: failed to init JS parser\n";
        ts_parser_delete(parser);
        return 1;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, src.c_str(), (uint32_t)src.size());
    TSNode root = ts_tree_root_node(tree);

    if (ts_node_has_error(root))
        std::cerr << "port: warning: parse errors in " << file << " — output may be incomplete\n";

    std::string outFile = fs::path(file).stem().string() + ".nv";
    std::string nova = "// Ported from " + file + " by novac port\n\n";
    nova += genNode(root, src, 0);

    std::ofstream out(outFile);
    if (!out.is_open())
    {
        std::cerr << "port: cannot write " << outFile << "\n";
        ts_tree_delete(tree);
        ts_parser_delete(parser);
        return 1;
    }
    out << nova;
    out.close();

    std::cout << "ported → " << outFile << "\n";

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return 0;
}