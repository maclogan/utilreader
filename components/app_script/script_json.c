/*
 * The `json` module for script apps: json.decode(text) and json.encode(value).
 *
 * The decoder builds Lua values directly (rather than going through cJSON), so the memory
 * a big reply takes counts against the app's own Lua heap limit instead of the chip's
 * internal RAM.
 *
 *   objects -> tables with string keys      arrays -> tables indexed 1..n
 *   null    -> nil (so it leaves a hole in an array; check with `== nil`)
 *   numbers -> integers when they have no fraction or exponent, floats otherwise
 *
 * Encoding: a table whose keys are exactly 1..n (n >= 1) becomes an array, any other
 * table an object (keys must be strings or numbers); an empty table becomes {}.
 */
#include <math.h>                       // isfinite()
#include <stdio.h>
#include <string.h>
#include "lauxlib.h"
#include "script_internal.h"

#define MAX_DEPTH 64

// ---------- decoding ----------

typedef struct {
    lua_State *L;
    const char *s, *p, *end;
} parser_t;

static void fail_at(parser_t *ps, const char *what)
{
    luaL_error(ps->L, "json.decode: %s at character %d", what, (int)(ps->p - ps->s) + 1);
}

static void skip_ws(parser_t *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static int hex4(parser_t *ps)
{
    if (ps->end - ps->p < 4) {
        fail_at(ps, "short \\u escape");
    }
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *ps->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else fail_at(ps, "bad \\u escape");
    }
    return v;
}

static void add_utf8(luaL_Buffer *b, unsigned cp)
{
    if (cp < 0x80) {
        luaL_addchar(b, (char)cp);
    } else if (cp < 0x800) {
        luaL_addchar(b, (char)(0xC0 | (cp >> 6)));
        luaL_addchar(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        luaL_addchar(b, (char)(0xE0 | (cp >> 12)));
        luaL_addchar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        luaL_addchar(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        luaL_addchar(b, (char)(0xF0 | (cp >> 18)));
        luaL_addchar(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        luaL_addchar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        luaL_addchar(b, (char)(0x80 | (cp & 0x3F)));
    }
}

// Pushes the string starting after the opening quote
static void parse_string(parser_t *ps)
{
    luaL_Buffer b;
    luaL_buffinit(ps->L, &b);
    for (;;) {
        if (ps->p >= ps->end) {
            fail_at(ps, "unfinished string");
        }
        char c = *ps->p++;
        if (c == '"') {
            break;
        }
        if (c != '\\') {
            luaL_addchar(&b, c);
            continue;
        }
        if (ps->p >= ps->end) {
            fail_at(ps, "unfinished escape");
        }
        c = *ps->p++;
        switch (c) {
        case '"': case '\\': case '/': luaL_addchar(&b, c); break;
        case 'b': luaL_addchar(&b, '\b'); break;
        case 'f': luaL_addchar(&b, '\f'); break;
        case 'n': luaL_addchar(&b, '\n'); break;
        case 'r': luaL_addchar(&b, '\r'); break;
        case 't': luaL_addchar(&b, '\t'); break;
        case 'u': {
            unsigned cp = hex4(ps);
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                ps->p += 2;                                 // surrogate pair: one character
                unsigned lo = hex4(ps);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            add_utf8(&b, cp);
            break;
        }
        default:
            fail_at(ps, "bad escape");
        }
    }
    luaL_pushresult(&b);
}

static void parse_value(parser_t *ps, int depth);

static void parse_number(parser_t *ps)
{
    const char *start = ps->p;
    while (ps->p < ps->end && strchr("+-0123456789.eE", *ps->p)) {
        ps->p++;
    }
    char buf[64];
    size_t n = ps->p - start;
    if (n == 0 || n >= sizeof(buf)) {
        fail_at(ps, "bad number");
    }
    memcpy(buf, start, n);
    buf[n] = '\0';
    if (lua_stringtonumber(ps->L, buf) == 0) {              // pushes an integer or a float
        fail_at(ps, "bad number");
    }
}

static void parse_value(parser_t *ps, int depth)
{
    lua_State *L = ps->L;
    if (depth > MAX_DEPTH) {
        fail_at(ps, "nested too deep");
    }
    luaL_checkstack(L, 4, "json.decode: nested too deep");
    skip_ws(ps);
    if (ps->p >= ps->end) {
        fail_at(ps, "unexpected end");
    }
    char c = *ps->p;
    if (c == '{') {
        ps->p++;
        lua_newtable(L);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            return;
        }
        for (;;) {
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') {
                fail_at(ps, "expected a key");
            }
            ps->p++;
            parse_string(ps);
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                fail_at(ps, "expected ':'");
            }
            ps->p++;
            parse_value(ps, depth + 1);
            lua_rawset(L, -3);                              // a null value leaves the key unset
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
            } else if (ps->p < ps->end && *ps->p == '}') {
                ps->p++;
                return;
            } else {
                fail_at(ps, "expected ',' or '}'");
            }
        }
    } else if (c == '[') {
        ps->p++;
        lua_newtable(L);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return;
        }
        for (lua_Integer i = 1;; i++) {
            parse_value(ps, depth + 1);
            lua_rawseti(L, -2, i);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
            } else if (ps->p < ps->end && *ps->p == ']') {
                ps->p++;
                return;
            } else {
                fail_at(ps, "expected ',' or ']'");
            }
        }
    } else if (c == '"') {
        ps->p++;
        parse_string(ps);
    } else if (c == 't' && ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        lua_pushboolean(L, 1);
    } else if (c == 'f' && ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        lua_pushboolean(L, 0);
    } else if (c == 'n' && ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        lua_pushnil(L);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        parse_number(ps);
    } else {
        fail_at(ps, "unexpected character");
    }
}

// json.decode(text) -> value, or raises an error describing where the text is wrong
static int json_decode(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    parser_t ps = {L, s, s, s + len};
    parse_value(&ps, 0);
    skip_ws(&ps);
    if (ps.p != ps.end) {
        fail_at(&ps, "text after the value");
    }
    return 1;
}

// ---------- encoding ----------
// The text grows in a userdata kept at a fixed stack slot (replaced by a bigger one when
// full). Unlike luaL_Buffer, that leaves the stack free for walking tables, and its memory
// counts against the app's Lua heap and is collected even if encoding fails half way.

typedef struct {
    lua_State *L;
    int slot;                           // absolute stack index of the userdata
    char *data;
    size_t len, cap;
} sbuf_t;

static void sb_add(sbuf_t *sb, const char *s, size_t n)
{
    if (sb->len + n > sb->cap) {
        size_t cap = sb->cap * 2;
        while (cap < sb->len + n) {
            cap *= 2;
        }
        char *bigger = lua_newuserdatauv(sb->L, cap, 0);
        memcpy(bigger, sb->data, sb->len);
        lua_replace(sb->L, sb->slot);                       // the old one becomes garbage
        sb->data = bigger;
        sb->cap = cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
}

static void sb_str(sbuf_t *sb, const char *s)
{
    sb_add(sb, s, strlen(s));
}

static void add_escaped(sbuf_t *sb, const char *s, size_t len)
{
    sb_add(sb, "\"", 1);
    size_t run = 0;                                         // characters copied as they are
    for (size_t i = 0; i < len; i++) {
        unsigned char c = s[i];
        const char *esc = NULL;
        char hex[8];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20) {
                snprintf(hex, sizeof(hex), "\\u%04x", c);
                esc = hex;
            }
        }
        if (esc) {
            sb_add(sb, s + i - run, run);
            run = 0;
            sb_str(sb, esc);
        } else {
            run++;                                          // UTF-8 passes through
        }
    }
    sb_add(sb, s + len - run, run);
    sb_add(sb, "\"", 1);
}

// True if the table at idx has exactly the keys 1..n, n >= 1; *n gets n
static bool is_array(lua_State *L, int idx, lua_Integer *n)
{
    lua_Integer len = (lua_Integer)lua_rawlen(L, idx), keys = 0;
    if (len == 0) {
        return false;
    }
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1 || lua_tointeger(L, -1) > len) {
            lua_pop(L, 1);
            return false;
        }
        keys++;
    }
    *n = len;
    return keys == len;
}

static void encode(sbuf_t *sb, int idx, int depth)
{
    lua_State *L = sb->L;
    if (depth > MAX_DEPTH) {
        luaL_error(L, "json.encode: nested too deep (a table that contains itself?)");
    }
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        sb_str(sb, "null");
        break;
    case LUA_TBOOLEAN:
        sb_str(sb, lua_toboolean(L, idx) ? "true" : "false");
        break;
    case LUA_TNUMBER: {
        char num[32];
        if (lua_isinteger(L, idx)) {
            snprintf(num, sizeof(num), LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, idx));
        } else {
            double d = lua_tonumber(L, idx);
            if (isfinite(d)) {
                snprintf(num, sizeof(num), "%.14g", d);
            } else {
                snprintf(num, sizeof(num), "null");         // JSON has no NaN or infinity
            }
        }
        sb_str(sb, num);
        break;
    }
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        add_escaped(sb, s, len);
        break;
    }
    case LUA_TTABLE: {
        luaL_checkstack(L, 6, "json.encode: nested too deep");
        lua_Integer n;
        if (is_array(L, idx, &n)) {
            sb_str(sb, "[");
            for (lua_Integer i = 1; i <= n; i++) {
                if (i > 1) {
                    sb_str(sb, ",");
                }
                lua_rawgeti(L, idx, i);
                encode(sb, -1, depth + 1);
                lua_pop(L, 1);
            }
            sb_str(sb, "]");
        } else {
            sb_str(sb, "{");
            bool first = true;
            lua_pushnil(L);
            while (lua_next(L, idx)) {                      // key at -2, value at -1
                int kt = lua_type(L, -2);
                if (kt != LUA_TSTRING && kt != LUA_TNUMBER) {
                    luaL_error(L, "json.encode: object keys must be strings or numbers");
                }
                if (!first) {
                    sb_str(sb, ",");
                }
                first = false;
                lua_pushvalue(L, -2);                       // a copy: tolstring changes number keys in place
                size_t klen;
                const char *k = lua_tolstring(L, -1, &klen);
                add_escaped(sb, k, klen);
                lua_pop(L, 1);
                sb_str(sb, ":");
                encode(sb, -1, depth + 1);
                lua_pop(L, 1);                              // the value; the key stays for lua_next
            }
            sb_str(sb, "}");
        }
        break;
    }
    default:
        luaL_error(L, "json.encode: can't encode a %s", luaL_typename(L, idx));
    }
}

// json.encode(value) -> text
static int json_encode(lua_State *L)
{
    luaL_checkany(L, 1);
    lua_settop(L, 1);
    sbuf_t sb = {.L = L, .slot = 2, .cap = 256};
    sb.data = lua_newuserdatauv(L, sb.cap, 0);              // slot 2
    encode(&sb, 1, 0);
    lua_pushlstring(L, sb.data, sb.len);
    return 1;
}

void script_json_open(lua_State *L)
{
    static const luaL_Reg funcs[] = {{"decode", json_decode}, {"encode", json_encode}, {NULL, NULL}};
    luaL_newlib(L, funcs);
    lua_setglobal(L, "json");
}
