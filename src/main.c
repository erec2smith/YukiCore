#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include "ui.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#ifdef _MSC_VER
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"shell32.lib")
#endif
typedef SOCKET sock_t;
typedef int slen_t;
#define CLOSESOCK closesocket
#define MKDIR(p) _mkdir(p)
#define HOMEVAR "USERPROFILE"
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int sock_t;
typedef socklen_t slen_t;
#define CLOSESOCK close
#define MKDIR(p) mkdir((p),0755)
#define INVALID_SOCKET (-1)
#define HOMEVAR "HOME"
#endif

#define MAXBODY (100u*1024u*1024u)
#define MAXPV 64

#ifdef _WIN32
static CRITICAL_SECTION pvlock;
#define LOCK() EnterCriticalSection(&pvlock)
#define UNLOCK() LeaveCriticalSection(&pvlock)
#else
static pthread_mutex_t pvlock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&pvlock)
#define UNLOCK() pthread_mutex_unlock(&pvlock)
#endif

typedef struct { char *p; size_t n, c; } Buf;

static void bp(Buf *b, const char *s, size_t l) {
    if (b->n + l + 1 > b->c) {
        size_t nc = b->c ? b->c : 512;
        char *q;
        while (nc < b->n + l + 1) nc *= 2;
        q = (char *)realloc(b->p, nc);
        if (!q) exit(1);
        b->p = q;
        b->c = nc;
    }
    memcpy(b->p + b->n, s, l);
    b->n += l;
    b->p[b->n] = 0;
}
static void bs(Buf *b, const char *s) { bp(b, s, strlen(s)); }
static void bc(Buf *b, char c) { bp(b, &c, 1); }

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JT;
typedef struct J { JT t; char *s; double n; struct J **v; char **k; int c; } J;

static J *jnew(JT t) { J *j = (J *)calloc(1, sizeof(J)); if (!j) exit(1); j->t = t; return j; }
static void jadd(J *o, char *k, J *v) {
    o->v = (J **)realloc(o->v, sizeof(J *) * (size_t)(o->c + 1));
    o->k = (char **)realloc(o->k, sizeof(char *) * (size_t)(o->c + 1));
    if (!o->v || !o->k) exit(1);
    o->k[o->c] = k;
    o->v[o->c] = v;
    o->c++;
}
static void jfree(J *j) {
    int i;
    if (!j) return;
    for (i = 0; i < j->c; i++) { free(j->k[i]); jfree(j->v[i]); }
    free(j->v); free(j->k); free(j->s); free(j);
}
static J *jget(J *o, const char *k) {
    int i;
    if (!o || o->t != J_OBJ) return NULL;
    for (i = 0; i < o->c; i++) if (!strcmp(o->k[i], k)) return o->v[i];
    return NULL;
}
static const char *jstr(J *o, const char *k) {
    J *v = jget(o, k);
    return v && v->t == J_STR ? v->s : "";
}

static void ws(const char **p) { while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t') (*p)++; }
static unsigned hex4(const char *p) {
    unsigned r = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        r <<= 4;
        if (c >= '0' && c <= '9') r |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') r |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') r |= (unsigned)(c - 'A' + 10);
        else return 0xFFFFFFFFu;
    }
    return r;
}
static void utf8(Buf *b, unsigned cp) {
    if (cp < 0x80) bc(b, (char)cp);
    else if (cp < 0x800) { bc(b, (char)(0xC0 | (cp >> 6))); bc(b, (char)(0x80 | (cp & 63))); }
    else if (cp < 0x10000) { bc(b, (char)(0xE0 | (cp >> 12))); bc(b, (char)(0x80 | ((cp >> 6) & 63))); bc(b, (char)(0x80 | (cp & 63))); }
    else { bc(b, (char)(0xF0 | (cp >> 18))); bc(b, (char)(0x80 | ((cp >> 12) & 63))); bc(b, (char)(0x80 | ((cp >> 6) & 63))); bc(b, (char)(0x80 | (cp & 63))); }
}
static char *pstr(const char **pp) {
    const char *p = *pp + 1;
    Buf b = {0, 0, 0};
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': bc(&b, '\n'); break;
                case 't': bc(&b, '\t'); break;
                case 'r': bc(&b, '\r'); break;
                case 'b': bc(&b, '\b'); break;
                case 'f': bc(&b, '\f'); break;
                case 'u': {
                    unsigned cp = hex4(p + 1);
                    if (cp == 0xFFFFFFFFu) { free(b.p); return NULL; }
                    p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                        unsigned lo = hex4(p + 3);
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                    }
                    utf8(&b, cp);
                    break;
                }
                case 0: free(b.p); return NULL;
                default: bc(&b, *p);
            }
            p++;
        } else bc(&b, *p++);
    }
    if (*p != '"') { free(b.p); return NULL; }
    *pp = p + 1;
    if (!b.p) { char *e = (char *)malloc(1); if (!e) exit(1); *e = 0; return e; }
    return b.p;
}
static J *pval(const char **pp, int d) {
    char c;
    if (d > 100) return NULL;
    ws(pp);
    c = **pp;
    if (c == '{') {
        J *o = jnew(J_OBJ);
        (*pp)++;
        ws(pp);
        if (**pp == '}') { (*pp)++; return o; }
        for (;;) {
            char *k;
            J *v;
            ws(pp);
            if (**pp != '"') { jfree(o); return NULL; }
            k = pstr(pp);
            if (!k) { jfree(o); return NULL; }
            ws(pp);
            if (**pp != ':') { free(k); jfree(o); return NULL; }
            (*pp)++;
            v = pval(pp, d + 1);
            if (!v) { free(k); jfree(o); return NULL; }
            jadd(o, k, v);
            ws(pp);
            if (**pp == ',') { (*pp)++; continue; }
            if (**pp == '}') { (*pp)++; return o; }
            jfree(o);
            return NULL;
        }
    }
    if (c == '[') {
        J *o = jnew(J_ARR);
        (*pp)++;
        ws(pp);
        if (**pp == ']') { (*pp)++; return o; }
        for (;;) {
            J *v = pval(pp, d + 1);
            if (!v) { jfree(o); return NULL; }
            jadd(o, NULL, v);
            ws(pp);
            if (**pp == ',') { (*pp)++; continue; }
            if (**pp == ']') { (*pp)++; return o; }
            jfree(o);
            return NULL;
        }
    }
    if (c == '"') {
        char *s = pstr(pp);
        J *j;
        if (!s) return NULL;
        j = jnew(J_STR);
        j->s = s;
        return j;
    }
    if (!strncmp(*pp, "true", 4)) { J *j = jnew(J_BOOL); j->n = 1; *pp += 4; return j; }
    if (!strncmp(*pp, "false", 5)) { J *j = jnew(J_BOOL); *pp += 5; return j; }
    if (!strncmp(*pp, "null", 4)) { *pp += 4; return jnew(J_NULL); }
    {
        char *e;
        double n = strtod(*pp, &e);
        J *j;
        if (e == *pp) return NULL;
        *pp = e;
        j = jnew(J_NUM);
        j->n = n;
        return j;
    }
}

static void escn(Buf *o, const char *s, size_t n, int nl) {
    size_t i;
    for (i = 0; i < n; i++) {
        switch (s[i]) {
            case '&': bs(o, "&amp;"); break;
            case '<': bs(o, "&lt;"); break;
            case '>': bs(o, "&gt;"); break;
            case '"': bs(o, "&quot;"); break;
            case '\r': break;
            case '\n': if (nl) bs(o, "<br>"); else bc(o, '\n'); break;
            default: bc(o, s[i]);
        }
    }
}
static void esc(Buf *o, const char *s, int nl) { escn(o, s, strlen(s), nl); }
static void jesc(Buf *o, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { bc(o, '\\'); bc(o, (char)c); }
        else if (c == '\n') bs(o, "\\n");
        else if (c < 32) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); bs(o, t); }
        else bc(o, (char)c);
    }
}
static void ident(const char *s, char *o, size_t m) {
    size_t k = 0;
    for (; *s && k + 1 < m; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '_' || c == '-') o[k++] = (char)c;
    }
    o[k] = 0;
}

static const char *homedir(void) {
    const char *h = getenv("YOKICORE_HOME");
    if (!h || !*h) h = getenv(HOMEVAR);
    return h;
}

typedef struct { int preview; int tab; char dir[1100]; int assets; } Ctx;
typedef struct { Buf d, t, m, h; int n; } Sty;

static const char *TAGS[] = {
    "header", "nav", "main", "section", "article", "aside", "footer", "div", "form",
    "h1", "h2", "h3", "h4", "h5", "h6", "p", "span", "a", "label", "blockquote",
    "strong", "em", "small", "img", "input", "textarea", "select", "button",
    "ul", "ol", "li", "hr", "figure", "figcaption", NULL
};
static const char *safe_tag(const char *t) {
    int i;
    for (i = 0; TAGS[i]; i++) if (!strcmp(TAGS[i], t)) return TAGS[i];
    return "div";
}
static int is_void(const char *t) { return !strcmp(t, "img") || !strcmp(t, "input") || !strcmp(t, "hr"); }

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
static int wf(const char *path, const char *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (n && fwrite(d, 1, n, f) != n) { fclose(f); return 0; }
    fclose(f);
    return 1;
}
static int save_asset(Ctx *x, const char *u, size_t len, char *name, size_t ns) {
    const char *m, *semi, *d, *ext = "bin";
    size_t ml, dl, i, bn = 0;
    unsigned char *buf;
    unsigned acc = 0;
    int bits = 0, ok;
    char path[1300];
    if (len < 20 || strncmp(u, "data:image/", 11)) return 0;
    m = u + 11;
    semi = (const char *)memchr(m, ';', len - 11);
    if (!semi) return 0;
    if ((size_t)(semi - u) + 8 > len || strncmp(semi, ";base64,", 8)) return 0;
    ml = (size_t)(semi - m);
    if (ml == 3 && !strncmp(m, "png", 3)) ext = "png";
    else if (ml == 4 && !strncmp(m, "jpeg", 4)) ext = "jpg";
    else if (ml == 3 && !strncmp(m, "jpg", 3)) ext = "jpg";
    else if (ml == 3 && !strncmp(m, "gif", 3)) ext = "gif";
    else if (ml == 4 && !strncmp(m, "webp", 4)) ext = "webp";
    else if (ml == 4 && !strncmp(m, "avif", 4)) ext = "avif";
    else if (ml == 3 && !strncmp(m, "bmp", 3)) ext = "bmp";
    else if (ml == 7 && !strncmp(m, "svg+xml", 7)) ext = "svg";
    d = semi + 8;
    dl = len - (size_t)(d - u);
    buf = (unsigned char *)malloc(dl / 4 * 3 + 4);
    if (!buf) return 0;
    for (i = 0; i < dl; i++) {
        int v = b64v((unsigned char)d[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; buf[bn++] = (unsigned char)((acc >> bits) & 255); }
    }
    snprintf(name, ns, "img-%d.%s", ++x->assets, ext);
    snprintf(path, sizeof path, "%s/assets/%s", x->dir, name);
    ok = wf(path, (const char *)buf, bn);
    free(buf);
    return ok;
}

static void css_val(Ctx *x, Buf *o, const char *v) {
    while (*v) {
        char c;
        if (!strncmp(v, "url(data:", 9)) {
            const char *e = strchr(v, ')');
            if (!e) break;
            if (x->preview) bp(o, v, (size_t)(e - v) + 1);
            else {
                char nm[64];
                if (save_asset(x, v + 4, (size_t)(e - (v + 4)), nm, sizeof nm)) {
                    bs(o, "url(../assets/");
                    bs(o, nm);
                    bc(o, ')');
                }
            }
            v = e + 1;
            continue;
        }
        c = *v++;
        if (c == '{' || c == '}' || c == ';' || c == '<' || c == '>') continue;
        bc(o, c);
    }
}

static void rule(Ctx *x, Buf *o, const char *sel, const char *ps, J *m, const char *pad) {
    Buf t = {0, 0, 0};
    int n = 0, i;
    if (!m || m->t != J_OBJ || !m->c) return;
    for (i = 0; i < m->c; i++) {
        J *v = m->v[i];
        char k[64];
        size_t kn = 0;
        const char *s;
        if (v->t != J_STR || !v->s[0]) continue;
        for (s = m->k[i]; *s && kn + 1 < sizeof k; s++) {
            unsigned char c = (unsigned char)tolower((unsigned char)*s);
            if (isalnum(c) || c == '-') k[kn++] = (char)c;
        }
        k[kn] = 0;
        if (!kn) continue;
        bs(&t, pad); bs(&t, "  "); bs(&t, k); bs(&t, ": ");
        css_val(x, &t, v->s);
        bs(&t, ";\n");
        n++;
    }
    if (n) {
        bs(o, pad); bs(o, sel); bs(o, ps); bs(o, " {\n");
        bs(o, t.p);
        bs(o, pad); bs(o, "}\n\n");
    }
    free(t.p);
}

static void ind(Buf *o, int d) { int i; for (i = 0; i < d; i++) bs(o, "  "); }
static void attr(Buf *o, const char *k, const char *v) {
    bc(o, ' '); bs(o, k); bs(o, "=\""); esc(o, v, 0); bc(o, '"');
}

static void gen_el(Ctx *x, Sty *sy, Buf *h, J *e, int d) {
    const char *tag = safe_tag(jstr(e, "tag"));
    const char *uc = jstr(e, "cls"), *txt = jstr(e, "txt"), *nm = jstr(e, "nm"), *rv = jstr(e, "rv");
    char id[160], autoc[24] = "", sel[220] = "";
    static const char *vk[] = {"d", "t", "m", "h"};
    J *v = jget(e, "v"), *a = jget(e, "a"), *ch = jget(e, "ch");
    int styled = 0, i, nch = (ch && ch->t == J_ARR) ? ch->c : 0;
    ident(jstr(e, "id"), id, sizeof id);
    for (i = 0; i < 4; i++) {
        J *m = jget(v, vk[i]);
        if (m && m->t == J_OBJ && m->c) styled = 1;
    }
    if (styled) {
        if (id[0]) {
            if (isalpha((unsigned char)id[0]) || id[0] == '_') snprintf(sel, sizeof sel, "#%s", id);
            else snprintf(sel, sizeof sel, "[id=\"%s\"]", id);
        } else {
            snprintf(autoc, sizeof autoc, "el-%d", ++sy->n);
            snprintf(sel, sizeof sel, ".%s", autoc);
        }
    }
    ind(h, d); bc(h, '<'); bs(h, tag);
    if (id[0]) attr(h, "id", id);
    if (uc[0] || autoc[0]) {
        bs(h, " class=\"");
        esc(h, uc, 0);
        if (uc[0] && autoc[0]) bc(h, ' ');
        bs(h, autoc);
        bc(h, '"');
    }
    if (nm[0]) attr(h, "name", nm);
    if (a && a->t == J_OBJ) {
        for (i = 0; i < a->c; i++) {
            J *val = a->v[i];
            char kk[64];
            size_t q;
            if (val->t != J_STR || !val->s[0]) continue;
            ident(a->k[i], kk, sizeof kk);
            for (q = 0; kk[q]; q++) kk[q] = (char)tolower((unsigned char)kk[q]);
            if (!kk[0] || !strcmp(kk, "id") || !strcmp(kk, "class") || !strcmp(kk, "name") || !strcmp(kk, "style")) continue;
            if (kk[0] == 'o' && kk[1] == 'n') continue;
            if (!strncmp(val->s, "javascript:", 11)) continue;
            if (!strcmp(kk, "src") && !strncmp(val->s, "data:image/", 11)) {
                if (x->preview) attr(h, kk, val->s);
                else {
                    char fn[64], pth[96];
                    if (save_asset(x, val->s, strlen(val->s), fn, sizeof fn)) {
                        snprintf(pth, sizeof pth, "assets/%s", fn);
                        attr(h, kk, pth);
                    }
                }
            } else attr(h, kk, val->s);
        }
    }
    if (rv[0]) { char r2[24]; ident(rv, r2, sizeof r2); attr(h, "data-reveal", r2); }
    if (styled) {
        rule(x, &sy->d, sel, "", jget(v, "d"), "");
        rule(x, &sy->h, sel, ":hover", jget(v, "h"), "  ");
        rule(x, &sy->t, sel, "", jget(v, "t"), "  ");
        rule(x, &sy->m, sel, "", jget(v, "m"), "  ");
    }
    if (is_void(tag)) {
        bs(h, ">\n");
    } else if (!strcmp(tag, "select")) {
        const char *s = txt;
        bc(h, '>');
        if (txt[0]) {
            bc(h, '\n');
            while (*s) {
                const char *nl = strchr(s, '\n');
                size_t l = nl ? (size_t)(nl - s) : strlen(s);
                if (l && !(l == 1 && s[0] == '\r')) {
                    ind(h, d + 1); bs(h, "<option>"); escn(h, s, l, 0); bs(h, "</option>\n");
                }
                s += l + (nl ? 1 : 0);
            }
            ind(h, d);
        }
        bs(h, "</select>\n");
    } else if (!strcmp(tag, "textarea")) {
        bc(h, '>'); esc(h, txt, 0); bs(h, "</textarea>\n");
    } else if (!nch) {
        bc(h, '>'); esc(h, txt, 1); bs(h, "</"); bs(h, tag); bs(h, ">\n");
    } else {
        bs(h, ">\n");
        if (txt[0]) { ind(h, d + 1); esc(h, txt, 1); bc(h, '\n'); }
        for (i = 0; i < ch->c; i++) gen_el(x, sy, h, ch->v[i], d + 1);
        ind(h, d); bs(h, "</"); bs(h, tag); bs(h, ">\n");
    }
}

static const char BASE_CSS[] =
"*, *::before, *::after { box-sizing: border-box; }\n"
"html { scroll-behavior: smooth; -webkit-text-size-adjust: 100%; }\n"
"body { margin: 0; font-family: system-ui, -apple-system, 'Segoe UI', Roboto, Tahoma, Arial, sans-serif; line-height: 1.5; color: #222; }\n"
"img, video { max-width: 100%; height: auto; }\n"
"a { color: inherit; }\n"
"input, textarea, select, button { font: inherit; }\n"
".js [data-reveal] { opacity: 0; transition: opacity .7s ease, transform .7s ease; }\n"
".js [data-reveal=up] { transform: translateY(36px); }\n"
".js [data-reveal=down] { transform: translateY(-36px); }\n"
".js [data-reveal=left] { transform: translateX(-36px); }\n"
".js [data-reveal=right] { transform: translateX(36px); }\n"
".js [data-reveal=zoom] { transform: scale(.9); }\n"
".js [data-reveal].in { opacity: 1; transform: none; }\n"
"[hidden] { display: none !important; }\n"
"@keyframes yc-fade { from { opacity: 0; } to { opacity: 1; } }\n"
"@keyframes yc-up { from { opacity: 0; transform: translateY(30px); } to { opacity: 1; transform: none; } }\n"
"@keyframes yc-down { from { opacity: 0; transform: translateY(-30px); } to { opacity: 1; transform: none; } }\n"
"@keyframes yc-left { from { opacity: 0; transform: translateX(-30px); } to { opacity: 1; transform: none; } }\n"
"@keyframes yc-right { from { opacity: 0; transform: translateX(30px); } to { opacity: 1; transform: none; } }\n"
"@keyframes yc-zoom { from { opacity: 0; transform: scale(.85); } to { opacity: 1; transform: none; } }\n"
"@keyframes yc-bounce { 0%, 100% { transform: translateY(0); } 50% { transform: translateY(-14px); } }\n"
"@keyframes yc-spin { to { transform: rotate(360deg); } }\n"
"@keyframes yc-pulse { 0%, 100% { transform: scale(1); } 50% { transform: scale(1.06); } }\n"
"@keyframes yc-shake { 0%, 100% { transform: translateX(0); } 25% { transform: translateX(-6px); } 75% { transform: translateX(6px); } }\n"
"@keyframes yc-flip { from { transform: perspective(600px) rotateY(90deg); opacity: 0; } to { transform: perspective(600px) rotateY(0); opacity: 1; } }\n"
"@media (prefers-reduced-motion: reduce) {\n"
"  .js [data-reveal] { opacity: 1; transform: none; transition: none; }\n"
"  * { animation-duration: .01ms !important; animation-iteration-count: 1 !important; }\n"
"}\n\n";

static const char BASE_JS[] =
"document.documentElement.classList.add('js');\n"
"document.addEventListener('DOMContentLoaded', function () {\n"
"  var items = document.querySelectorAll('[data-reveal]');\n"
"  if (!('IntersectionObserver' in window)) {\n"
"    items.forEach(function (el) { el.classList.add('in'); });\n"
"    return;\n"
"  }\n"
"  var io = new IntersectionObserver(function (entries) {\n"
"    entries.forEach(function (en) {\n"
"      if (en.isIntersecting) {\n"
"        en.target.classList.add('in');\n"
"        io.unobserve(en.target);\n"
"      }\n"
"    });\n"
"  }, { threshold: 0.15 });\n"
"  items.forEach(function (el) { io.observe(el); });\n"
"});\n";

static const char PREVIEW_JS[] =
"document.addEventListener('click', function (e) {\n"
"  var a = e.target.closest && e.target.closest('a');\n"
"  if (!a) return;\n"
"  var h = a.getAttribute('href') || '';\n"
"  if (/\\.html?$/.test(h)) { e.preventDefault(); parent.postMessage({ yc: h }, '*'); }\n"
"});\n";

static void build_page(Ctx *x, J *pg, Buf *body, Buf *css) {
    Sty sy;
    J *els = jget(pg, "els");
    int i;
    memset(&sy, 0, sizeof sy);
    if (els && els->t == J_ARR) for (i = 0; i < els->c; i++) gen_el(x, &sy, body, els->v[i], 1);
    if (sy.d.p) bs(css, sy.d.p);
    if (sy.h.p) { bs(css, "@media (hover: hover) {\n"); bs(css, sy.h.p); bs(css, "}\n\n"); }
    if (sy.t.p) { bs(css, "@media (max-width: 991px) {\n"); bs(css, sy.t.p); bs(css, "}\n\n"); }
    if (sy.m.p) { bs(css, "@media (max-width: 640px) {\n"); bs(css, sy.m.p); bs(css, "}\n\n"); }
    free(sy.d.p); free(sy.t.p); free(sy.m.p); free(sy.h.p);
}

static void safe_script(Buf *o, const char *s) {
    for (; *s; s++) {
        if (s[0] == '<' && s[1] == '/') { bs(o, "<\\/"); s++; }
        else bc(o, *s);
    }
}

static void page_html(Ctx *x, J *pr, J *pg, const char *slug, Buf *body, Buf *css, Buf *o) {
    char lang[16];
    const char *dr = strcmp(jstr(pr, "dir"), "rtl") ? "ltr" : "rtl";
    const char *title = jstr(pg, "title"), *desc = jstr(pg, "desc");
    ident(jstr(pr, "lang"), lang, sizeof lang);
    if (!lang[0]) strcpy(lang, "en");
    bs(o, "<!DOCTYPE html>\n<html lang=\""); bs(o, lang);
    bs(o, "\" dir=\""); bs(o, dr);
    bs(o, "\">\n<head>\n  <meta charset=\"UTF-8\">\n  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n  <title>");
    esc(o, title[0] ? title : "Page", 0);
    bs(o, "</title>\n");
    if (desc[0]) { bs(o, "  <meta name=\"description\" content=\""); esc(o, desc, 0); bs(o, "\">\n"); }
    if (x->preview) {
        bs(o, "  <style>\n"); bs(o, BASE_CSS); if (css->p) bs(o, css->p); bs(o, "  </style>\n");
    } else {
        bs(o, "  <link rel=\"stylesheet\" href=\"css/base.css\">\n  <link rel=\"stylesheet\" href=\"css/");
        bs(o, slug);
        bs(o, ".css\">\n");
    }
    bs(o, "</head>\n<body>\n");
    if (body->p) bs(o, body->p);
    if (x->preview) {
        bs(o, "<script>\n"); bs(o, BASE_JS);
        if (!x->tab) bs(o, PREVIEW_JS);
        safe_script(o, jstr(pr, "js"));
        bs(o, "\n");
        safe_script(o, jstr(pr, "evjs"));
        bs(o, "\n</script>\n");
    } else bs(o, "<script src=\"js/script.js\" defer></script>\n");
    bs(o, "</body>\n</html>\n");
}

static char last_dir[1100];

static int export_proj(J *pr, const char *raw, size_t rl, char *err, size_t es, char *outdir) {
    char pname[80], t1[800], path[1300];
    const char *home = homedir();
    J *pgs = jget(pr, "pages");
    Ctx x;
    Buf js = {0, 0, 0};
    int i;
    ident(jstr(pr, "name"), pname, sizeof pname);
    if (!pname[0]) strcpy(pname, "project");
    if (!home || !*home || strlen(home) > 700) home = ".";
    if (!pgs || pgs->t != J_ARR || !pgs->c) { snprintf(err, es, "No pages to export"); return 0; }
    memset(&x, 0, sizeof x);
    snprintf(t1, sizeof t1, "%s/YokiCore", home);
    MKDIR(t1);
    snprintf(x.dir, sizeof x.dir, "%s/%s", t1, pname);
    MKDIR(x.dir);
    snprintf(path, sizeof path, "%s/css", x.dir); MKDIR(path);
    snprintf(path, sizeof path, "%s/js", x.dir); MKDIR(path);
    snprintf(path, sizeof path, "%s/assets", x.dir); MKDIR(path);
    snprintf(path, sizeof path, "%s/css/base.css", x.dir);
    if (!wf(path, BASE_CSS, strlen(BASE_CSS))) { snprintf(err, es, "Cannot write to %.120s", x.dir); return 0; }
    for (i = 0; i < pgs->c; i++) {
        J *pg = pgs->v[i];
        char slug[80];
        size_t q;
        Buf body = {0, 0, 0}, css = {0, 0, 0}, html = {0, 0, 0};
        ident(jstr(pg, "name"), slug, sizeof slug);
        for (q = 0; slug[q]; q++) slug[q] = (char)tolower((unsigned char)slug[q]);
        if (!slug[0]) snprintf(slug, sizeof slug, "page%d", i + 1);
        build_page(&x, pg, &body, &css);
        page_html(&x, pr, pg, slug, &body, &css, &html);
        snprintf(path, sizeof path, "%s/%s.html", x.dir, slug);
        wf(path, html.p, html.n);
        snprintf(path, sizeof path, "%s/css/%s.css", x.dir, slug);
        wf(path, css.p ? css.p : "", css.n);
        free(body.p); free(css.p); free(html.p);
    }
    bs(&js, BASE_JS);
    bc(&js, '\n');
    bs(&js, jstr(pr, "js"));
    bc(&js, '\n');
    bs(&js, jstr(pr, "evjs"));
    bc(&js, '\n');
    snprintf(path, sizeof path, "%s/js/script.js", x.dir);
    wf(path, js.p, js.n);
    free(js.p);
    snprintf(path, sizeof path, "%s/project.json", x.dir);
    wf(path, raw, rl);
    snprintf(outdir, 1100, "%s", x.dir);
    return 1;
}

static void render_one(J *pr, J *pg, int tab, Buf *o) {
    Ctx x;
    Buf body = {0, 0, 0}, css = {0, 0, 0};
    memset(&x, 0, sizeof x);
    x.preview = 1;
    x.tab = tab;
    build_page(&x, pg, &body, &css);
    page_html(&x, pr, pg, "", &body, &css, o);
    free(body.p); free(css.p);
}

static void preview_proj(J *pr, Buf *o) {
    J *pgs = jget(pr, "pages"), *pv = jget(pr, "pv");
    int i;
    if (!pgs || pgs->t != J_ARR || !pgs->c) { bs(o, "<!DOCTYPE html><p>No pages</p>"); return; }
    i = (pv && pv->t == J_NUM) ? (int)pv->n : 0;
    if (i < 0 || i >= pgs->c) i = 0;
    render_one(pr, pgs->v[i], 0, o);
}

static char *pv_name[MAXPV];
static Buf pv_page[MAXPV];
static int pv_count = 0;

static void slug_of(J *pg, int i, char *slug, size_t n) {
    size_t q;
    ident(jstr(pg, "name"), slug, n);
    for (q = 0; slug[q]; q++) slug[q] = (char)tolower((unsigned char)slug[q]);
    if (!slug[0]) snprintf(slug, n, "page%d", i + 1);
}

static int store_tab_preview(J *pr, char *first, size_t fs) {
    J *pgs = jget(pr, "pages"), *pv = jget(pr, "pv");
    int i, n, start;
    Buf np[MAXPV];
    char *nn[MAXPV];
    if (!pgs || pgs->t != J_ARR || !pgs->c) return 0;
    n = pgs->c > MAXPV ? MAXPV : pgs->c;
    start = (pv && pv->t == J_NUM) ? (int)pv->n : 0;
    if (start < 0 || start >= n) start = 0;
    memset(np, 0, sizeof np);
    for (i = 0; i < n; i++) {
        char slug[80];
        slug_of(pgs->v[i], i, slug, sizeof slug);
        nn[i] = (char *)malloc(strlen(slug) + 1);
        if (!nn[i]) exit(1);
        strcpy(nn[i], slug);
        render_one(pr, pgs->v[i], 1, &np[i]);
        if (i == start) snprintf(first, fs, "%s", slug);
    }
    LOCK();
    for (i = 0; i < pv_count; i++) { free(pv_name[i]); free(pv_page[i].p); }
    for (i = 0; i < n; i++) { pv_name[i] = nn[i]; pv_page[i] = np[i]; }
    pv_count = n;
    UNLOCK();
    return 1;
}

static void reply(sock_t c, int code, const char *ct, const char *body, size_t len);

static int send_tab_page(sock_t c, const char *name, size_t nl) {
    int i, ok = 0;
    Buf copy = {0, 0, 0};
    LOCK();
    for (i = 0; i < pv_count; i++) {
        if (strlen(pv_name[i]) == nl && !strncmp(pv_name[i], name, nl)) {
            bp(&copy, pv_page[i].p ? pv_page[i].p : "", pv_page[i].n);
            ok = 1;
            break;
        }
    }
    UNLOCK();
    if (ok) reply(c, 200, "text/html; charset=utf-8", copy.p ? copy.p : "", copy.n);
    free(copy.p);
    return ok;
}

static void launch(const char *t) {
    if (getenv("YOKICORE_NO_BROWSER")) return;
#ifdef _WIN32
    ShellExecuteA(NULL, "open", t, NULL, NULL, SW_SHOWNORMAL);
#else
    pid_t p = fork();
    if (p == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
#ifdef __APPLE__
        execlp("open", "open", t, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", t, (char *)NULL);
#endif
        _exit(1);
    }
#endif
}

static char token[17];
static volatile int quit_flag = 0, seen_ping = 0;
static volatile time_t last_ping = 0, bye_at = 0;

static void mktoken(void) {
    uint64_t s = (uint64_t)time(NULL) * 6364136223846793005ULL ^ ((uint64_t)clock() << 20) ^ (uint64_t)(uintptr_t)&s;
    uint64_t r = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    int i;
    if (f) { if (fread(&r, sizeof r, 1, f) != 1) r = 0; fclose(f); }
    s ^= r;
    if (!s) s = 88172645463325252ULL;
    for (i = 0; i < 16; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        token[i] = "0123456789abcdef"[s & 15];
    }
    token[16] = 0;
}

static void sendall(sock_t c, const char *p, size_t n) {
    while (n) {
        int k = (int)(n > 1048576 ? 1048576 : n);
        int r = (int)send(c, p, k, 0);
        if (r <= 0) return;
        p += r;
        n -= (size_t)r;
    }
}
static void reply(sock_t c, int code, const char *ct, const char *body, size_t len) {
    char h[320];
    const char *st = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 413 ? "Payload Too Large" : "Error";
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nCache-Control: no-store\r\nX-Frame-Options: DENY\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        code, st, ct, (unsigned long)len);
    sendall(c, h, (size_t)n);
    if (len) sendall(c, body, len);
}

static int ci(const char *s, const char *p) {
    for (; *p; s++, p++) if (tolower((unsigned char)*s) != *p) return 0;
    return 1;
}

static void handle(sock_t c) {
    Buf rq = {0, 0, 0};
    char tmp[8192], method[8] = {0}, path[512] = {0}, *q, *body;
    char *he = NULL;
    size_t hl, cl = 0, pl;
    const char *cp;
    int auth;
#ifdef _WIN32
    DWORD to = 10000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
#else
    struct timeval to;
    to.tv_sec = 10; to.tv_usec = 0;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
#endif
    while (!he && rq.n < 65536) {
        int r = (int)recv(c, tmp, sizeof tmp, 0);
        if (r <= 0) break;
        bp(&rq, tmp, (size_t)r);
        he = strstr(rq.p, "\r\n\r\n");
    }
    if (!he) { free(rq.p); return; }
    hl = (size_t)(he - rq.p) + 4;
    sscanf(rq.p, "%7s %511s", method, path);
    cp = rq.p;
    while ((cp = strchr(cp, '\n')) != NULL) {
        cp++;
        if (cp > he) break;
        if (ci(cp, "content-length:")) cl = (size_t)strtoull(cp + 15, NULL, 10);
    }
    if (cl > MAXBODY) { reply(c, 413, "text/plain", "Too large", 9); free(rq.p); return; }
    while (rq.n < hl + cl) {
        int r = (int)recv(c, tmp, sizeof tmp, 0);
        if (r <= 0) break;
        bp(&rq, tmp, (size_t)r);
    }
    if (rq.n < hl + cl) { reply(c, 400, "text/plain", "Bad request", 11); free(rq.p); return; }
    body = rq.p + hl;
    body[cl] = 0;
    q = strchr(path, '?');
    pl = q ? (size_t)(q - path) : strlen(path);
    auth = q && !strncmp(q, "?t=", 3) && !strncmp(q + 3, token, 16);
#define ROUTE(m, p) (!strcmp(method, m) && pl == sizeof(p) - 1 && !strncmp(path, p, pl))
    if (ROUTE("GET", "/")) {
        const char *ph = "__YC_TOKEN__", *u = (const char *)UI_HTML, *f = strstr(u, ph);
        Buf o = {0, 0, 0};
        bye_at = 0;
        if (f) { bp(&o, u, (size_t)(f - u)); bp(&o, token, 16); bs(&o, f + strlen(ph)); }
        else bs(&o, u);
        reply(c, 200, "text/html; charset=utf-8", o.p, o.n);
        free(o.p);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/v/", 3) && !strncmp(path + 3, token, 16) && path[19] == '/') {
        const char *nm = path + 20;
        size_t nl = 0;
        while (nm[nl] && nm[nl] != '?' && nm[nl] != '.') nl++;
        if (!send_tab_page(c, nm, nl)) reply(c, 404, "text/plain", "Not found", 9);
    } else if (!strcmp(method, "POST") && !auth) {
        reply(c, 403, "text/plain", "Forbidden", 9);
    } else if (ROUTE("POST", "/ping")) {
        seen_ping = 1; last_ping = time(NULL); bye_at = 0;
        reply(c, 200, "text/plain", "ok", 2);
    } else if (ROUTE("POST", "/bye")) {
        bye_at = time(NULL);
        reply(c, 200, "text/plain", "ok", 2);
    } else if (ROUTE("POST", "/quit")) {
        reply(c, 200, "text/plain", "ok", 2);
        quit_flag = 1;
    } else if (ROUTE("POST", "/open")) {
        char d[1100];
        if (last_dir[0]) snprintf(d, sizeof d, "%s", last_dir);
        else {
            const char *home = homedir();
            if (!home || !*home || strlen(home) > 700) home = ".";
            snprintf(d, sizeof d, "%s/YokiCore", home);
            MKDIR(d);
        }
        launch(d);
        reply(c, 200, "application/json", "{\"ok\":true}", 11);
    } else if (ROUTE("POST", "/export")) {
        const char *pp = body;
        J *pr = pval(&pp, 0);
        Buf r = {0, 0, 0};
        if (!pr || pr->t != J_OBJ) bs(&r, "{\"ok\":false,\"error\":\"Invalid project data\"}");
        else {
            char err[200] = "", dir[1100] = "";
            if (export_proj(pr, body, cl, err, sizeof err, dir)) {
                snprintf(last_dir, sizeof last_dir, "%s", dir);
                bs(&r, "{\"ok\":true,\"path\":\""); jesc(&r, dir); bs(&r, "\"}");
            } else {
                bs(&r, "{\"ok\":false,\"error\":\""); jesc(&r, err); bs(&r, "\"}");
            }
        }
        reply(c, 200, "application/json", r.p, r.n);
        free(r.p);
        jfree(pr);
    } else if (ROUTE("POST", "/newtab")) {
        const char *pp = body;
        J *pr = pval(&pp, 0);
        Buf r = {0, 0, 0};
        char first[80] = "", u[160];
        if (pr && pr->t == J_OBJ && store_tab_preview(pr, first, sizeof first)) {
            snprintf(u, sizeof u, "/v/%s/%s.html", token, first);
            bs(&r, "{\"ok\":true,\"url\":\""); jesc(&r, u); bs(&r, "\"}");
        } else bs(&r, "{\"ok\":false,\"error\":\"Invalid project data\"}");
        reply(c, 200, "application/json", r.p, r.n);
        free(r.p);
        jfree(pr);
    } else if (ROUTE("POST", "/preview")) {
        const char *pp = body;
        J *pr = pval(&pp, 0);
        Buf o = {0, 0, 0};
        if (pr && pr->t == J_OBJ) preview_proj(pr, &o);
        else bs(&o, "<!DOCTYPE html><p>Invalid project</p>");
        reply(c, 200, "text/html; charset=utf-8", o.p, o.n);
        free(o.p);
        jfree(pr);
    } else {
        reply(c, 404, "text/plain", "Not found", 9);
    }
    free(rq.p);
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID a) {
    sock_t c = (sock_t)(uintptr_t)a;
    handle(c);
    CLOSESOCK(c);
    return 0;
}
#else
static void *worker(void *a) {
    sock_t c = (sock_t)(intptr_t)a;
    handle(c);
    CLOSESOCK(c);
    return NULL;
}
#endif

static void spawn(sock_t c) {
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)c, 0, NULL);
    if (h) CloseHandle(h);
    else { handle(c); CLOSESOCK(c); }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, worker, (void *)(intptr_t)c) == 0) pthread_detach(t);
    else { handle(c); CLOSESOCK(c); }
#endif
}

int main(void) {
    sock_t ls;
    struct sockaddr_in a;
    slen_t sl = (slen_t)sizeof a;
    int port;
    time_t start = time(NULL);
    char url[64];
#ifdef _WIN32
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    InitializeCriticalSection(&pvlock);
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
#endif
    mktoken();
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return 1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) return 1;
    if (listen(ls, 32) != 0) return 1;
    if (getsockname(ls, (struct sockaddr *)&a, &sl) != 0) return 1;
    port = (int)ntohs(a.sin_port);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);
    printf("YokiCore is running at %s\n", url);
    fflush(stdout);
    launch(url);
    for (;;) {
        fd_set fs;
        struct timeval tv;
        int r;
        time_t now;
        FD_ZERO(&fs);
        FD_SET(ls, &fs);
        tv.tv_sec = 1; tv.tv_usec = 0;
        r = select((int)ls + 1, &fs, NULL, NULL, &tv);
        if (r > 0) {
            sock_t c = accept(ls, NULL, NULL);
            if (c != INVALID_SOCKET) spawn(c);
        }
        now = time(NULL);
        if (quit_flag) { 
#ifdef _WIN32
            Sleep(300);
#else
            usleep(300000);
#endif
            break;
        }
        if (bye_at && now - bye_at >= 6) break;
        if (seen_ping && now - last_ping > 300) break;
        if (!seen_ping && now - start > 180) break;
    }
    CLOSESOCK(ls);
    return 0;
}
