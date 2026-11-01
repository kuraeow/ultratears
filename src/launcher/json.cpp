// A small JSON reader for setup.py's event lines: objects, arrays, strings (with \u escapes), numbers, true / false / null.
#include "launcher.h"
#include <cstdlib>

static const Json NONE;

const Json &Json::operator[](const char *key) const {
    if (kind == Obj)
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == key)
                return items[i];
    return NONE;
}

const Json &Json::operator[](size_t i) const { return kind == Arr && i < items.size() ? items[i] : NONE; }

std::string Json::str(const char *def) const {
    if (kind == Str)
        return s;
    if (kind == Num) {
        char buf[32];
        if (n == (long long)n)
            SDL_snprintf(buf, sizeof buf, "%lld", (long long)n);
        else
            SDL_snprintf(buf, sizeof buf, "%g", n);
        return buf;
    }
    if (kind == Bool)
        return b ? "true" : "false";
    return def;
}

double Json::num(double def) const { return kind == Num ? n : kind == Str ? SDL_atof(s.c_str()) : def; }

namespace {
struct Reader {
    const char *p, *end;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            p++;
    }
    bool lit(const char *w) {
        size_t n = SDL_strlen(w);
        if ((size_t)(end - p) < n || SDL_memcmp(p, w, n) != 0)
            return false;
        p += n;
        return true;
    }
    static void utf8(std::string &out, unsigned c) {
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | c >> 6);
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | c >> 12);
            out += (char)(0x80 | (c >> 6 & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | c >> 18);
            out += (char)(0x80 | (c >> 12 & 0x3F));
            out += (char)(0x80 | (c >> 6 & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    bool hex4(unsigned &c) {
        if (end - p < 4)
            return false;
        c = 0;
        for (int i = 0; i < 4; i++) {
            char h = *p++;
            c = c << 4 | (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 99);
            if (c & ~0xFFFFu)
                return false;
        }
        return true;
    }
    bool string(std::string &out) {
        if (p >= end || *p != '"')
            return false;
        p++;
        while (p < end && *p != '"') {
            if (*p != '\\') {
                out += *p++;
                continue;
            }
            if (++p >= end)
                return false;
            char e = *p++;
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned c;
                if (!hex4(c))
                    return false;
                if (c >= 0xD800 && c < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {   // a surrogate pair
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo))
                        return false;
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, c);
                break;
            }
            default: out += e;
            }
        }
        if (p >= end)
            return false;
        p++;
        return true;
    }
    bool value(Json &v, int depth) {
        ws();
        if (p >= end || depth > 32)
            return false;
        if (*p == '{') {
            v.kind = Json::Obj;
            p++;
            ws();
            if (p < end && *p == '}')
                return p++, true;
            for (;;) {
                ws();
                std::string k;
                if (!string(k))
                    return false;
                ws();
                if (p >= end || *p++ != ':')
                    return false;
                v.keys.push_back(k);
                v.items.emplace_back();
                if (!value(v.items.back(), depth + 1))
                    return false;
                ws();
                if (p < end && *p == ',') {
                    p++;
                    continue;
                }
                return p < end && *p++ == '}';
            }
        }
        if (*p == '[') {
            v.kind = Json::Arr;
            p++;
            ws();
            if (p < end && *p == ']')
                return p++, true;
            for (;;) {
                v.items.emplace_back();
                if (!value(v.items.back(), depth + 1))
                    return false;
                ws();
                if (p < end && *p == ',') {
                    p++;
                    continue;
                }
                return p < end && *p++ == ']';
            }
        }
        if (*p == '"') {
            v.kind = Json::Str;
            return string(v.s);
        }
        if (lit("true")) {
            v.kind = Json::Bool, v.b = true;
            return true;
        }
        if (lit("false")) {
            v.kind = Json::Bool, v.b = false;
            return true;
        }
        if (lit("null"))
            return true;
        char *stop;
        std::string num(p, (size_t)SDL_min(end - p, 64));
        v.n = SDL_strtod(num.c_str(), &stop);
        if (stop == num.c_str())
            return false;
        v.kind = Json::Num;
        p += stop - num.c_str();
        return true;
    }
};
}   // namespace

bool json_parse(const std::string &text, Json &out) {
    out = Json();
    Reader r{text.data(), text.data() + text.size()};
    if (!r.value(out, 0))
        return false;
    r.ws();
    return r.p == r.end;
}
