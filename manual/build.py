#!/usr/bin/env python3
"""The manual: man-page style HTML for every public header, generated from the headers themselves.

    python3 manual/build.py            # writes manual/*.html next to this script

Each header under include/ioxd/ becomes one page in section 3 (ioxd_http(3), ioxd_json(3), ...):
NAME from the header's top comment, SYNOPSIS from its declarations, DESCRIPTION from the comment
above each declaration - in the header's own order and sections - EXAMPLES from the snippets in
this file, SEE ALSO from the rest. ioxd(7) is the overview, index.html the front page, and
functions.html every public name in one alphabetical list. Private names (ioxd__, IOXD__) are
left out, as are the macro internals that only serve a public one.
"""
import html
import os
import re
import sys
from datetime import date

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INCLUDE = os.path.join(ROOT, "include")

PAGES = [  # (header, page name, one-line subject used on the index)
    ("ioxd/config.h", "ioxd_config", "the runtime's knobs"),
    ("ioxd/http.h",   "ioxd_http",   "request, response, context, body, reply"),
    ("ioxd/router.h", "ioxd_router", "groups, endpoints, middleware; the script macros"),
    ("ioxd/slice.h",  "ioxd_slice",  "slices, conversions, key/value parsing"),
    ("ioxd/json.h",   "ioxd_json",   "JSON written as you go; structs described once"),
    ("ioxd/pipe.h",   "ioxd_pipe",   "a connection as a pipe, for other protocols"),
    ("ioxd/run.h",    "ioxd_run",    "bind the ports, run the workers"),
    ("ioxd/timer.h",  "ioxd_timer",  "a delay that parks the connection, not the worker"),
    ("ioxd/tls.h",    "ioxd_tls",    "a certificate store, for a TLS port"),
]


def version():
    with open(os.path.join(ROOT, "Makefile")) as f:
        m = re.search(r"^VERSION\s*:=\s*(\S+)", f.read(), re.M)
    return m.group(1) if m else "0"


# ── parsing a header ───────────────────────────────────────────────────────────────────────

class Entry:
    """One thing in a header: a section title, or a comment with the declarations under it."""
    def __init__(self, kind):
        self.kind = kind            # 'section' | 'entry'
        self.title = ""             # section
        self.paras = []             # entry: the comment above, as paragraphs (text or ('pre', code))
        self.decls = []             # entry: [(code, trailing comment, [names], public)]


def comment_text(lines):
    """The lines of a block comment, without their decoration, as paragraphs: plain text, or
    indented code kept as it is."""
    body = []
    for ln in lines:
        ln = re.sub(r"^\s*/\*\s?", "", ln)
        ln = re.sub(r"\s*\*/\s*$", "", ln)
        ln = re.sub(r"^\s*\*\s?", "", ln) if not ln.lstrip().startswith("*/") else ""
        body.append(ln.rstrip())
    paras, text, code = [], [], []

    def flush():
        nonlocal text, code
        if code:
            paras.append(("pre", "\n".join(code)))
            code = []
        if text:
            paras.append(" ".join(t.strip() for t in text))
            text = []

    for ln in body:
        if ln.startswith("    "):
            if text:
                paras.append(" ".join(t.strip() for t in text)); text = []
            code.append(ln[4:])
        elif ln.strip() == "":
            flush()
        else:
            if code:
                paras.append(("pre", "\n".join(code))); code = []
            text.append(ln)
    flush()
    return paras


def names_of(code):
    """The public identifiers a declaration defines: a function, a macro, a type, a constant."""
    first = code.split("\n")[0].strip()
    names = []
    m = re.match(r"#define\s+([A-Za-z_]\w*)", first)
    if m:
        return [m.group(1)]
    m = re.match(r"typedef\s+.*\(\*\s*([A-Za-z_]\w*)\s*\)", first)
    if m:
        return [m.group(1)]
    if first.startswith("typedef struct") or first.startswith("typedef union") or first.startswith("typedef enum"):
        last = code.strip().split("\n")[-1]
        m = re.search(r"}\s*([A-Za-z_]\w*)\s*;\s*$", last) or re.search(r"typedef\s+struct\s+\w+\s+([A-Za-z_]\w*)\s*;", first)
        if m:
            return [m.group(1)]
    m = re.match(r"struct\s+([A-Za-z_]\w*)\s*\{", first)
    if m:
        return ["struct " + m.group(1)]
    m = re.match(r"struct\s+([A-Za-z_]\w*)\s*;", first)
    if m:
        return ["struct " + m.group(1)]
    m = re.search(r"([A-Za-z_]\w*)\s*\(", first)
    if m and not first.startswith("#"):
        return [m.group(1)]
    return names


def is_public(names, code):
    for n in names:
        if n.startswith("ioxd__") or n.startswith("IOXD__") or n.endswith("_args"):
            return False
    if code.lstrip().startswith("#define IOXD__"):
        return False
    if re.match(r"struct\s+\w+\s*;", code.strip()):
        return False                                    # a forward declaration: the type is elsewhere
    return True


MACRO_USAGE = {   # the script macros, as they are written; the expansion is machinery
    "IOXD_GROUP":   "IOXD_GROUP(prefix, middleware...) { ... }",
    "IOXD_USE":     "IOXD_USE(middleware)",
    "IOXD_ROUTE":   "IOXD_ROUTE(method, path, handler, middleware...)",
    "IOXD_GET":     "IOXD_GET(path, handler, middleware...)",
    "IOXD_HEAD":    "IOXD_HEAD(path, handler, middleware...)",
    "IOXD_POST":    "IOXD_POST(path, handler, middleware...)",
    "IOXD_PUT":     "IOXD_PUT(path, handler, middleware...)",
    "IOXD_PATCH":   "IOXD_PATCH(path, handler, middleware...)",
    "IOXD_DELETE":  "IOXD_DELETE(path, handler, middleware...)",
    "IOXD_OPTIONS": "IOXD_OPTIONS(path, handler, middleware...)",
    "IOXD_DEFAULT": "IOXD_DEFAULT(handler)",
    "IOXD_JSON_VALUE":  "IOXD_JSON_VALUE(ioxd_json *j, x)",
    "IOXD_JSON_FIELD":  "IOXD_JSON_FIELD(ioxd_json *j, const char *name, x)",
    "IOXD_JSON_STRUCT": "IOXD_JSON_STRUCT(name, FIELDS)",
    "IOXD_JSON_WRITER": "IOXD_JSON_WRITER(name, FIELDS)",
}


def shown(code):
    """A declaration as the manual shows it: an inline function by its signature alone, a
    function-like macro by the way it is written rather than what it expands to."""
    if code.startswith("static inline") and "{" in code:
        return code[:code.find("{")].rstrip() + ";"
    m = re.match(r"#define\s+([A-Za-z_]\w*)\(", code)
    if m:
        name = m.group(1)
        if name in MACRO_USAGE:
            return MACRO_USAGE[name]
        head = re.match(r"#define\s+[A-Za-z_]\w*\([^)]*\)", code)
        return head.group(0) if head else code.split("\n")[0]
    return code


def decl_ends(line):
    core = strip_trailing_comment(line)[0].rstrip()
    return core.endswith(";") or core.endswith("}")


def braces(line):
    """The brace depth a line adds, comments and character literals not counted."""
    core = strip_trailing_comment(line)[0]
    core = re.sub(r"'.'|\"[^\"]*\"", "", core)
    return core.count("{") - core.count("}")


def strip_trailing_comment(line):
    m = re.match(r"^(.*?)\s*/\*\s*(.*?)\s*\*/\s*$", line)
    if m:
        return m.group(1), m.group(2)
    return line, ""


def parse_header(path):
    with open(path) as f:
        src = f.read().split("\n")
    top = []
    i = 0
    if src and src[0].startswith("/*"):
        while i < len(src):
            top.append(src[i])
            if "*/" in src[i]:
                i += 1
                break
            i += 1
    top_paras = comment_text(top)
    name_line = top_paras[0] if top_paras else ""
    m = re.match(r"(\S+)\s+-\s+(.*)", name_line)
    subject = m.group(2) if m else name_line

    entries = []
    pending = []                                        # comment paragraphs waiting for a declaration
    skipping_else = False
    while i < len(src):
        line = src[i]
        s = line.strip()
        if skipping_else:
            if s.startswith("#endif"):
                skipping_else = False
            i += 1
            continue
        if s == "" or s.startswith("#pragma") or s.startswith("#include"):
            if s == "" and pending and entries and entries[-1].kind == "entry" and not entries[-1].decls:
                pass
            i += 1
            continue
        if s.startswith("#else"):
            skipping_else = True
            i += 1
            continue
        if s.startswith("#endif") or s.startswith("#if") or s.startswith("#ifdef") or s.startswith("#ifndef"):
            i += 1
            continue
        if s.startswith("/*"):
            block = [line]
            while "*/" not in src[i]:
                i += 1
                block.append(src[i])
            i += 1
            text = " ".join(b.strip(" /*") for b in block)
            sec = re.match(r"\s*/\*\s*[─-]+\s*(.*?)\s*[─-]+\s*\*/\s*$", "".join(block))
            if sec:
                e = Entry("section"); e.title = sec.group(1); entries.append(e)
                pending = []
                continue
            pending = comment_text(block)
            e = Entry("entry"); e.paras = pending
            entries.append(e)
            continue
        # a declaration: collect until it is whole - a ';' or a closing brace at depth zero, with
        # any trailing comment out of the way; a macro continues while its lines end in '\\'
        decl = [line]
        depth = braces(line)
        if s.startswith("#define"):
            while src[i].rstrip().endswith("\\"):
                i += 1
                decl.append(src[i])
        else:
            while not (depth == 0 and decl_ends(decl[-1])):
                i += 1
                decl.append(src[i])
                depth += braces(src[i])
        i += 1
        last, trailing = strip_trailing_comment(decl[-1])
        if trailing and not last.strip().startswith("}"):
            decl[-1] = last                             # a prototype's trailing comment is its description
        else:
            trailing = ""                               # a struct's closing line: its members keep theirs
        code = "\n".join(decl)
        names = names_of(code)
        public = is_public(names, code)
        if entries and entries[-1].kind == "entry":
            entries[-1].decls.append((code.rstrip(), trailing, names, public))
        else:
            e = Entry("entry"); e.decls.append((code.rstrip(), trailing, names, public)); entries.append(e)
    # a section title with nothing public under it is dropped later
    return subject, top_paras, entries


# ── rendering ──────────────────────────────────────────────────────────────────────────────

def esc(t):
    return html.escape(t, quote=False)


# C, highlighted at build time: no script on the page, and the library's names still become links
# (the linker runs on the result, and matches nothing inside a tag). Each match is one span.
C_TOKENS = re.compile(r"""
    (?P<cm>/\*.*?\*/|//[^\n]*)
  | (?P<pp>^[ \t]*\#[^\n]*)
  | (?P<str>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])+')
  | (?P<num>\b(?:0[xX][0-9A-Fa-f]+|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)[uUlLfF]*\b)
  | (?P<kw>\b(?:if|else|for|while|do|return|break|continue|switch|case|default|goto|sizeof
        |static|inline|const|struct|union|enum|typedef|extern|volatile|register|restrict
        |_Atomic|_Generic|_Thread_local|thread_local|true|false|NULL|nullptr)\b)
  | (?P<ty>\b(?:void|char|short|int|long|unsigned|signed|float|double|bool|size_t|ssize_t
        |u?int(?:8|16|32|64)_t|uintptr_t|intptr_t|time_t)\b)
""", re.S | re.M | re.X)


SHELL_WORDS = ("make", "curl", "printf", "head", "sh ", "cc ", "$", "python3", "nc ", "openssl", "gcc")


def highlight(code):
    """C with its tokens in spans; a shell transcript (the lines a comment shows to run) as it is."""
    first = next((ln.strip() for ln in code.split("\n") if ln.strip()), "")
    if first.startswith(SHELL_WORDS):
        return esc(code)
    out, at = [], 0
    for m in C_TOKENS.finditer(code):
        out.append(esc(code[at:m.start()]))
        kind = m.lastgroup
        out.append(f'<span class="c-{kind}">{esc(m.group(0))}</span>')
        at = m.end()
    out.append(esc(code[at:]))
    return "".join(out)


def anchor_id(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def render_paras(paras, link):
    out = []
    for p in paras:
        if isinstance(p, tuple):
            out.append('<pre class="ex">' + link(highlight(p[1])) + "</pre>")
        else:
            out.append("<p>" + link(esc(p)) + "</p>")
    return "\n".join(out)


def make_linker(index, self_page):
    """Turn every public name mentioned in text into a link to its page and anchor."""
    names = sorted((n for n in index if not n.startswith("struct ")), key=len, reverse=True)
    pat = re.compile(r"(?<![\w#/])(" + "|".join(re.escape(n) for n in names) + r")\b") if names else None

    def link(text):
        if not pat:
            return text

        def repl(m):
            n = m.group(1)
            page, aid = index[n]
            href = ("" if page == self_page else page + ".html") + "#" + aid
            return f'<a href="{href}">{n}</a>'
        return pat.sub(repl, text)
    return link


def page_html(title, section, body, version_str, nav_extra=""):
    today = date.today().isoformat()
    upper = title.upper() + f"({section})"
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(title)}({section}) - libioxd manual</title>
<link rel="stylesheet" href="style.css">
</head>
<body>
<nav class="crumbs"><a href="index.html">libioxd manual</a> &rsaquo; <a href="ioxd.7.html">ioxd(7)</a> &rsaquo; <a href="ioxd_examples.html">examples</a> &rsaquo; <a href="functions.html">all names</a>{nav_extra}</nav>
<main>
<div class="hdr"><span>{upper}</span><span>libioxd Programmer's Manual</span><span>{upper}</span></div>
{body}
<div class="ftr"><span>libioxd {esc(version_str)}</span><span>{today}</span><span>{upper}</span></div>
</main>
</body>
</html>
"""


def synopsis_of(entries):
    lines = []
    for e in entries:
        if e.kind != "entry":
            continue
        for code, _t, names, public in e.decls:
            if not public:
                continue
            first = code.split("\n")[0]
            if code.startswith("typedef struct") and "\n" in code or code.startswith("struct") and "{" in code:
                nm = names[0] if names else "..."
                lines.append(f"typedef struct {{ ... }} {nm};" if code.startswith("typedef") else f"{nm} {{ ... }};")
            elif code.startswith("#define"):
                lines.append(shown(code) if "(" in first.split()[1] else strip_trailing_comment(first)[0].strip())
            elif code.startswith("static inline"):
                body_at = code.find("{")
                lines.append(code[:body_at].rstrip() + ";" if body_at > 0 else code)
            else:
                lines.append(re.sub(r"\s+", " ", code.replace("\n", " ")).strip() if "\n" in code else code.strip())
    return "\n".join(lines)


def render_page(header, page, subject, top_paras, entries, index, version_str, examples, see_also):
    link = make_linker(index, page)
    body = []
    body.append("<h2>NAME</h2>")
    body.append(f"<p>{esc(header)} - {link(esc(subject))}</p>")
    body.append("<h2>SYNOPSIS</h2>")
    body.append("<pre class=\"syn\">" + link(highlight("#include <ioxd.h>\n\n" + synopsis_of(entries))) + "</pre>")
    body.append("<h2>DESCRIPTION</h2>")
    if len(top_paras) > 1:
        body.append(render_paras(top_paras[1:], link))
    for e in entries:
        if e.kind == "section":
            body.append(f"<h3>{esc(e.title)}</h3>")
            continue
        pub = [d for d in e.decls if d[3]]
        if not pub and not e.paras:
            continue
        if not pub and e.paras and e.decls:
            continue                                    # a comment on private machinery
        body.append('<div class="entry">')
        if pub:
            ids = " ".join(anchor_id(n) for d in pub for n in d[2])
            first_id = anchor_id(pub[0][2][0]) if pub[0][2] else ""
            body.append(f'<pre class="decl" id="{first_id}">')
            for code, trailing, names, _p in pub:
                for n in names[1:]:
                    body.append(f'<span id="{anchor_id(n)}"></span>')
                body.append(highlight(shown(code)))
            body.append("</pre>")
        if e.paras:
            body.append('<div class="text">' + render_paras(e.paras, link) + "</div>")
        trail = [(code, t) for code, t, _n, _p in pub if t]
        if trail:
            body.append('<dl class="trail">')
            for code, t in trail:
                short = code.split("\n")[0].strip()
                m = re.search(r"([A-Za-z_]\w*)\s*\(", short)
                label = m.group(1) if m and not short.startswith("#") else (re.match(r"#define\s+(\S+)", short).group(1) if short.startswith("#define") else short)
                body.append(f"<dt>{esc(label)}</dt><dd>{link(esc(t))}</dd>")
            body.append("</dl>")
        body.append("</div>")
    if examples:
        body.append("<h2>EXAMPLES</h2>")
        for title, code in examples:
            body.append(f"<p>{link(esc(title))}</p>")
            body.append('<pre class="ex">' + link(highlight(code)) + "</pre>")
    body.append("<h2>SEE ALSO</h2>")
    body.append("<p>" + ", ".join(f'<a href="{p}.html">{p}({s})</a>' for p, s in see_also) + "</p>")
    return page_html(page, "3", "\n".join(body), version_str)


# ── the hand-written pages: the overview, the examples ─────────────────────────────────────

OVERVIEW = """
<h2>NAME</h2>
<p>ioxd - an HTTP/1.1 server library on io_uring, one worker per core, a stackful coroutine per connection</p>

<h2>SYNOPSIS</h2>
<pre class="syn">#include &lt;ioxd.h&gt;

cc main.c $(pkg-config --cflags --libs ioxd) -o server</pre>

<h2>DESCRIPTION</h2>
<p>libioxd serves HTTP/1.1, plain or over TLS 1.3, from a thread per core. Each worker owns an io_uring
ring, a ring of receive buffers the kernel delivers into, and its own sockets on every bound port
(SO_REUSEPORT); nothing is shared between workers while serving. Every connection runs on its own
coroutine, so a handler reads the body and writes the reply in straight-line code: a call that has to
wait for the wire suspends the coroutine, and the worker's loop resumes it on the completion.</p>

<p>A program registers its routes, sets the runtime's knobs if it wants to, binds its ports, then runs:</p>
<pre class="ex">static void hello(ioxd_ctx *ctx)
{
    ioxd_slice name = ctx-&gt;req.route_params[0].value;
    ioxd_printf(ctx, "hello %.*s\\n", (int)name.len, name.p);
}

int main(void)
{
    IOXD_GET("/hello/:name", hello);
    ioxd_bind(8080, NULL);                                  /* plain */
    ioxd_bind(8443, ioxd_certs_load("certs"));              /* TLS 1.3, from a directory of certificates */
    return ioxd_run(0);                                     /* one worker per core, until SIGINT or SIGTERM */
}</pre>

<h3>The request and the reply</h3>
<p>A handler receives an <a href="ioxd_http.html#ioxd_ctx">ioxd_ctx</a>: the request as plain data - method,
path, query, headers, parameters, all slices into the connection's buffers - and the response being
shaped. The body stays on the wire until asked for: <a href="ioxd_http.html#ioxd_body_all">ioxd_body_all</a>
reads it whole, <a href="ioxd_http.html#ioxd_body_read_until">ioxd_body_read_until</a> streams it. The reply
is written into a slab with <a href="ioxd_http.html#ioxd_write">ioxd_write</a>, <a href="ioxd_http.html#ioxd_printf">ioxd_printf</a>
or the <a href="ioxd_json.html">JSON writer</a>; when everything fits it goes out in one send with its head in
front, and when it does not it streams, chunked. What goes on the wire follows the protocol whatever the handler
did: a reply to HEAD carries no body, a declared length is held to, a request whose framing cannot be trusted is
refused before a handler sees it.</p>

<h3>Routes</h3>
<p>Endpoints live in <a href="ioxd_router.html">groups</a>: a prefix plus middleware, nesting. Everything is
resolved once, when the run starts, into a segment tree and one flat middleware chain per endpoint, so a request
costs one walk and no scan. The script macros (IOXD_GROUP, IOXD_GET, IOXD_USE) are the same registrations
written as a block.</p>

<h3>Threads and lifetimes</h3>
<p>Register routes, configure and bind from the main thread, before the run. Handlers run on worker threads,
one at a time per worker; a request's slices are valid until the handler returns, and anything a handler hands
the reply (a header, a content type) is copied. Nothing in the library is shared between workers except the
read-only route tree and a TLS store's certificate table, which is reference counted.</p>

<h3>Limits</h3>
<p>A request head, and a body read whole, must fit the reader's 16 KB; streamed bodies have no limit. At most
64 request headers, 32 query parameters, 8 route captures, 16 added reply headers within 3 KB. These size the
context, so <a href="ioxd_run.html#ioxd_run">ioxd_run</a> refuses an application built with different values.
The runtime's own sizes - the ring, the receive buffers, the coroutine stacks, the pools - are the
<a href="ioxd_config.html">configuration</a>, per worker.</p>

<h3>Building</h3>
<p>Linux 6.x on x86-64, gcc 14 or newer (the library is C23; the headers are usable from C11), OpenSSL 3 for
the TLS handshake (built by default; <code>make TLS=0</code> or <code>-DIOXD_TLS=OFF</code> leaves it out).
<code>make</code> produces libioxd.a and libioxd.so; <code>make install</code> the headers and a pkg-config file;
CMake exports <code>ioxd::ioxd</code>. Kernel TLS needs a kernel with SOCKET_URING_OP_SETSOCKOPT (6.7 or newer)
when the registered file table is on, which is the default.</p>

<h2>FILES</h2>
<dl class="files">
<dt>&lt;ioxd.h&gt;</dt><dd>the whole API: an umbrella over the headers below</dd>
<dt><a href="ioxd_config.html">&lt;ioxd/config.h&gt;</a></dt><dd>the runtime's knobs: ring, buffers, stacks, pools</dd>
<dt><a href="ioxd_http.html">&lt;ioxd/http.h&gt;</a></dt><dd>request, response, context, body, reply</dd>
<dt><a href="ioxd_router.html">&lt;ioxd/router.h&gt;</a></dt><dd>groups, endpoints, middleware, the script macros</dd>
<dt><a href="ioxd_slice.html">&lt;ioxd/slice.h&gt;</a></dt><dd>slices, conversions, key/value parsing</dd>
<dt><a href="ioxd_json.html">&lt;ioxd/json.h&gt;</a></dt><dd>the JSON writer and IOXD_JSON_STRUCT</dd>
<dt><a href="ioxd_pipe.html">&lt;ioxd/pipe.h&gt;</a></dt><dd>a connection as a pipe, for protocols other than HTTP</dd>
<dt><a href="ioxd_run.html">&lt;ioxd/run.h&gt;</a></dt><dd>bind the ports, plain or TLS, run the workers</dd>
<dt><a href="ioxd_timer.html">&lt;ioxd/timer.h&gt;</a></dt><dd>a delay that parks the connection, not the worker</dd>
<dt><a href="ioxd_tls.html">&lt;ioxd/tls.h&gt;</a></dt><dd>a certificate store, for a TLS port</dd>
</dl>

<h2>SEE ALSO</h2>
<p><a href="functions.html">every public name</a>, <a href="ioxd_examples.html">ioxd_examples(7)</a>, and the
repository at <a href="https://github.com/MDA2AV/libioxd">github.com/MDA2AV/libioxd</a>.</p>
"""

EXAMPLES = {
    "ioxd_config": [
        ("Twice the receive buffers, before the run; every other field keeps its default:",
         """ioxd_config config = { .recv_buffers = 8192 };
if (ioxd_configure(&config) < 0)
    return 1;                                   /* the reason is on stderr */
ioxd_bind(8080, NULL);
return ioxd_run(0);"""),
    ],
    "ioxd_http": [
        ("A route parameter, a query parameter converted, and a formatted reply:",
         """static void user(ioxd_ctx *ctx)
{
    int64_t id;
    if (!ioxd_to_i64(ctx->req.route_params[0].value, &id)) {
        ctx->res.status = 400;
        ioxd_text(ctx, "the id must be an integer\\n");
        return;
    }
    for (size_t i = 0; i < ctx->req.n_params; i++)
        if (ioxd_slice_eq(ctx->req.params[i].key, "fields"))
            ioxd_printf(ctx, "fields=%.*s\\n", (int)ctx->req.params[i].value.len, ctx->req.params[i].value.p);
    ioxd_header(ctx, "x-user", "42");                 /* copied: a temporary is fine */
    ioxd_printf(ctx, "user %lld\\n", (long long)id);
}"""),
        ("A body read whole, then a reply streamed with a flush every ten lines:",
         """static void repeat(ioxd_ctx *ctx)
{
    ioxd_slice body = ioxd_body_all(ctx);             /* over 16 KB: empty, and res.status is 413 */
    if (ctx->res.status != 200)
        return;
    for (int i = 1; i <= 25; i++) {
        if (ioxd_printf(ctx, "%d: %.*s\\n", i, (int)body.len, body.p) < 0)
            return;                                   /* the peer is gone */
        if (i % 10 == 0 && ioxd_flush(ctx) < 0)
            return;
    }
}"""),
        ("A large upload streamed through a fixed buffer:",
         """static void upload(ioxd_ctx *ctx)
{
    char   buf[4096];
    size_t total = 0;
    for (;;) {
        int n = ioxd_body_read_until(ctx, buf, sizeof buf);
        if (n < 0) return;                            /* malformed or gone: the engine answers */
        if (n == 0) break;
        total += (size_t)n;
    }
    ioxd_printf(ctx, "%zu bytes\\n", total);
}"""),
        ("Middleware around the handler, the onion way:",
         """static void timing(ioxd_ctx *ctx, ioxd_next *next)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ioxd_next_run(ctx, next);                         /* the rest of the chain, then the endpoint */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* the head may already be out (a streamed reply): ioxd_header then returns false */
}"""),
    ],
    "ioxd_router": [
        ("The same registrations as calls and as a script:",
         """ioxd_group *api = ioxd_group_new(NULL, "/api");
ioxd_group_use(api, auth);
ioxd_get(api, "/users/:id", user);                    /* GET /api/users/:id, behind auth */
ioxd_endpoint_use(ioxd_post(api, "/users", create), audit);

IOXD_USE(log);                                        /* root middleware: every request */
IOXD_GROUP("/api", auth) {
    IOXD_GET ("/users/:id", user);
    IOXD_POST("/users",     create, audit);
    IOXD_GROUP("/admin", require_token) {
        IOXD_GET("/stats", stats);
    }
}
IOXD_DEFAULT(not_found);"""),
    ],
    "ioxd_slice": [
        ("Strict conversions: the whole slice is the value, or the call fails and *out is untouched:",
         """int64_t id;
double  price;
bool    on;
if (!ioxd_to_i64(ctx->req.route_params[0].value, &id))     /* "42", "-7"; not "42x", not " 42" */
    ...
if (ioxd_to_double(v, &price) && ioxd_to_bool(w, &on))      /* "2.5", "1e-3"; "yes", "off" */
    ..."""),
        ("A form body parsed like a query string:",
         """ioxd_slice body = ioxd_body_all(ctx);
ioxd_kv    form[8];
char       arena[512];
bool       truncated;
size_t n = ioxd_kv_parse(body.p, body.len, form, 8, arena, sizeof arena, &truncated);
if (truncated) { ctx->res.status = 400; return; }        /* never act on part of it */
for (size_t i = 0; i < n; i++)
    if (ioxd_slice_eq(form[i].key, "name"))
        ioxd_printf(ctx, "hello %.*s\\n", (int)form[i].value.len, form[i].value.p);"""),
    ],
    "ioxd_json": [
        ("Written as you go, straight into the reply:",
         """ioxd_json j = ioxd_json_reply(ctx);
ioxd_json_object(&j);
    IOXD_JSON_FIELD(&j, "id", id);
    IOXD_JSON_FIELD(&j, "name", name);
    ioxd_json_key(&j, "tags"); ioxd_json_array(&j);
        ioxd_json_cstr(&j, "new");
    ioxd_json_end(&j);
ioxd_json_end(&j);"""),
        ("A struct described once, nested objects and arrays included:",
         """#define ORDER_FIELDS(X)        \\
    X(VALUE,   int,    number)  \\
    X(VALUE,   double, total)
IOXD_JSON_STRUCT(order, ORDER_FIELDS)

#define USER_FIELDS(X)                         \\
    X(VALUE,   int64_t,      id)               \\
    X(VALUE,   const char *, name)             /* NULL comes out as null */ \\
    X(ARRAY,   const char *, tags,   n_tags)   \\
    X(OBJECTS, order,        orders, n_orders)
IOXD_JSON_STRUCT(user, USER_FIELDS)

struct user u = { .id = 42, .name = "Zoe", .tags = tags, .n_tags = 2, .orders = orders, .n_orders = 1 };
ioxd_json j = ioxd_json_reply(ctx);
user_to_json(&j, &u);                                 /* {"id":42,"name":"Zoe","tags":[...],"orders":[{...}]} */"""),
    ],
    "ioxd_pipe": [
        ("A line echo server on raw TCP: read until a newline, answer, repeat:",
         """static void echo(ioxd_pipe *pipe)
{
    for (;;) {
        ioxd_slice live;
        int rc = ioxd_pipe_read(pipe, &live);         /* one contiguous span, or waits */
        if (rc <= 0)
            return;                                   /* 0: the peer is done; <0: gone or FULL */
        const char *nl = memchr(live.p, '\\n', live.len);
        if (!nl) {
            ioxd_pipe_examine(pipe, live.len);        /* seen it all: the next read waits for more */
            continue;
        }
        size_t n = (size_t)(nl - live.p) + 1;
        if (ioxd_pipe_send(pipe, live.p, n) < 0)
            return;
        ioxd_pipe_drop(pipe, n);
    }
}

int main(void)
{
    ioxd_bind(8100, NULL);
    return ioxd_run_pipes(0, echo);
}"""),
    ],
    "ioxd_timer": [
        ("A feed that ticks: a line every quarter second, the connection parked in between while the worker serves the rest:",
         """static void ticks(ioxd_ctx *ctx)
{
    for (int i = 1; i <= 20; i++) {
        if (ioxd_printf(ctx, "tick %d\\n", i) < 0 || ioxd_flush(ctx) < 0)
            return;                                   /* the peer is gone */
        if (ioxd_delay(250) != 0)
            return;                                   /* the server is stopping */
    }
}"""),
    ],
    "ioxd_tls": [
        ("A TLS port beside a plain one; the files rotated, then reloaded:",
         """ioxd_certs *certs = ioxd_certs_load("/etc/ioxd/certs");   /* <dir>/<host>/cert.pem and key.pem; `default` required */
if (!certs)
    return 1;
ioxd_bind(8080, NULL);
ioxd_bind(8443, certs);
/* ... later, after new files were written: */
ioxd_certs_reload(certs);                               /* a host that fails keeps its old certificate */"""),
    ],
}

STYLE = """/* The manual's look: a man page, as man7.org renders one - monospace, sections in capitals at the
 * margin, the text indented under them - with links, anchors and a light nav bar on top. */
:root {
  --paper: #ffffff; --ink: #111111; --dim: #555555; --rule: #d8d8d8; --link: #1a3fbf; --code: #f4f4f4;
}
@media (prefers-color-scheme: dark) {
  :root { --paper: #111213; --ink: #e6e6e6; --dim: #a0a0a0; --rule: #333; --link: #7da2ff; --code: #1c1e21; }
}
html { background: var(--paper); }
body {
  margin: 0; color: var(--ink); background: var(--paper);
  font: 14px/1.5 "DejaVu Sans Mono", "Liberation Mono", Menlo, Consolas, ui-monospace, monospace;
}
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
.crumbs { padding: .5em 1.5em; border-bottom: 1px solid var(--rule); color: var(--dim); }
main { max-width: 136ch; margin: 0 auto; padding: 1em 1.5em 4em; }
.hdr, .ftr { display: flex; justify-content: space-between; font-weight: bold; }
.ftr { margin-top: 3em; font-weight: normal; color: var(--dim); }
h1 { font-size: 1em; font-weight: bold; margin: 1.4em 0 .5em; }
h2 { font-size: 1em; font-weight: bold; margin: 1.8em 0 .5em; letter-spacing: .02em; }
h3 { font-size: 1em; font-weight: bold; margin: 1.4em 0 .4em 3ch; }
h3::before { content: ""; }
main > p, .entry, dl, pre, .text { margin-left: 5ch; }
main > pre.ex, main > pre.syn { margin-left: 5ch; }
p { margin: .5em 0; max-width: 110ch; }
pre { margin: .5em 0; white-space: pre; overflow-x: auto; }     /* code stays on its lines; a long one scrolls */
pre.syn { padding: .6em 1em; background: var(--code); border-left: 3px solid var(--rule); }
pre.decl { font-weight: bold; margin: 1.2em 0 .3em; }
pre.ex { padding: .6em 1em; background: var(--code); border-left: 3px solid var(--rule); }
.entry { margin-top: .4em; }
.entry .text { margin-left: 3ch; max-width: 110ch; }
.entry .text pre.ex, .text pre.ex { margin-left: 0; }
dl.trail { margin: .3em 0 0 4ch; }
dl.trail dt { font-weight: bold; margin-top: .4em; }
dl.trail dd { margin: 0 0 0 4ch; color: var(--ink); }
dl.files dt { font-weight: bold; margin-top: .6em; }
dl.files dd { margin: 0 0 0 4ch; }
table { border-collapse: collapse; margin-left: 7ch; }
td, th { text-align: left; padding: .25em 1.5em .25em 0; vertical-align: top; }
th { font-weight: bold; }
.dim { color: var(--dim); }
/* C, highlighted by build.py */
.c-cm  { color: #6a737d; font-style: italic; }
.c-pp  { color: #8a3fa8; }
.c-str { color: #0a7a3b; }
.c-num { color: #b35c00; }
.c-kw  { color: #1d4ed8; font-weight: bold; }
.c-ty  { color: #0e7490; }
pre.decl .c-ty, pre.decl .c-kw { font-weight: bold; }
@media (prefers-color-scheme: dark) {
  .c-cm  { color: #8b949e; }
  .c-pp  { color: #d2a8ff; }
  .c-str { color: #7ee787; }
  .c-num { color: #ffa657; }
  .c-kw  { color: #79c0ff; }
  .c-ty  { color: #56d4dd; }
}
@media (max-width: 700px) {
  main > p, .entry, dl, pre, .text, table, h3 { margin-left: 1ch; }
  .hdr span:nth-child(2), .ftr span:nth-child(2) { display: none; }
}
"""


def build():
    version_str = version()
    parsed = []
    index = {}                                           # name -> (page, anchor)
    for header, page, subject_hint in PAGES:
        subject, top_paras, entries = parse_header(os.path.join(INCLUDE, header))
        parsed.append((header, page, subject or subject_hint, top_paras, entries))
        for e in entries:
            if e.kind != "entry":
                continue
            for _code, _t, names, public in e.decls:
                if public:
                    for n in names:
                        index.setdefault(n, (page, anchor_id(n)))
    index.setdefault("ioxd_run", ("ioxd_run", "ioxd_run"))
    for header, page, subject, top_paras, entries in parsed:
        see = [(p, "3") for _h, p, _s in PAGES if p != page] + [("ioxd_examples", "7"), ("ioxd", "7")]
        out = render_page(header, page, subject, top_paras, entries, index, version_str, EXAMPLES.get(page, []), see)
        with open(os.path.join(HERE, page + ".html"), "w") as f:
            f.write(out)

    # ioxd(7)
    with open(os.path.join(HERE, "ioxd.7.html"), "w") as f:
        f.write(page_html("ioxd", "7", OVERVIEW, version_str))

    # ioxd_examples(7): the programs under playground/examples, whole, their top comment first
    link = make_linker(index, "ioxd_examples")
    ex_dir = os.path.join(ROOT, "playground", "examples")
    parts = ["<h2>NAME</h2><p>ioxd_examples - whole programs, one per way of using the library; each builds as "
             "ioxd-example-&lt;name&gt; with <code>make examples</code></p>",
             "<h2>DESCRIPTION</h2>"]
    toc = []
    for fn in sorted(os.listdir(ex_dir)):
        if not fn.endswith(".c"):
            continue
        with open(os.path.join(ex_dir, fn)) as f:
            src = f.read()
        head = re.match(r"/\*(.*?)\*/\n", src, re.S)
        intro = comment_text(head.group(0).split("\n")) if head else []
        code = src[head.end():].lstrip("\n") if head else src
        name = fn[:-2]
        title = intro[0] if intro and not isinstance(intro[0], tuple) else fn
        title = re.sub(r"^\S+\s+-\s+", "", title)
        toc.append(f'<tr><td><a href="#{name}">{esc(fn)}</a></td><td>{link(esc(title))}</td></tr>')
        parts.append(f'<h3 id="{name}">{esc(fn)}</h3>')
        parts.append('<div class="entry"><div class="text">' + render_paras(intro[1:], link) + "</div>"
                     '<pre class="ex">' + link(highlight(code.rstrip())) + "</pre></div>")
    parts.insert(2, "<table>" + "\n".join(toc) + "</table>")
    parts.append("<h2>SEE ALSO</h2><p>" + ", ".join(f'<a href="{p}.html">{p}(3)</a>' for _h, p, _s in PAGES) + ', <a href="ioxd.7.html">ioxd(7)</a></p>')
    with open(os.path.join(HERE, "ioxd_examples.html"), "w") as f:
        f.write(page_html("ioxd_examples", "7", "\n".join(parts), version_str))

    # every name
    rows = []
    for n in sorted(index, key=lambda s: s.lower()):
        page, aid = index[n]
        rows.append(f'<tr><td><a href="{page}.html#{aid}">{esc(n)}</a></td><td class="dim">{page}(3)</td></tr>')
    names_body = ("<h2>NAME</h2><p>functions - every public name of libioxd, alphabetically, with the page that describes it</p>"
                  "<h2>DESCRIPTION</h2><table>" + "\n".join(rows) + "</table>")
    with open(os.path.join(HERE, "functions.html"), "w") as f:
        f.write(page_html("functions", "3", names_body, version_str))

    # the front page
    rows = "\n".join(f'<tr><td><a href="{p}.html">{p}(3)</a></td><td>&lt;{esc(h)}&gt;</td><td>{esc(s)}</td></tr>'
                     for h, p, s in PAGES)
    front = f"""<h1>libioxd manual</h1>
<p>The manual of libioxd, an HTTP/1.1 server library on io_uring for Linux, in the shape of man pages:
one page per public header, generated from the headers themselves, so what a page says is what the
header declares. Start with the overview.</p>
<h2>SECTION 7: OVERVIEW</h2>
<table><tr><td><a href="ioxd.7.html">ioxd(7)</a></td><td></td><td>the library, its model and its limits</td></tr>
<tr><td><a href="ioxd_examples.html">ioxd_examples(7)</a></td><td></td><td>whole programs: streaming, middleware, groups, raw pipes</td></tr></table>
<h2>SECTION 3: HEADERS</h2>
<table>{rows}
<tr><td><a href="functions.html">functions(3)</a></td><td></td><td>every public name, alphabetically</td></tr></table>
<h2>SEE ALSO</h2>
<p>The repository at <a href="https://github.com/MDA2AV/libioxd">github.com/MDA2AV/libioxd</a>. This manual is built
from the headers by <code>manual/build.py</code>; <code>make manual</code> refreshes it.</p>
"""
    with open(os.path.join(HERE, "index.html"), "w") as f:
        f.write(page_html("index", "", front, version_str).replace("<title>index() - libioxd manual</title>", "<title>libioxd manual</title>")
                .replace('<div class="hdr"><span>INDEX()</span><span>libioxd Programmer\'s Manual</span><span>INDEX()</span></div>',
                         '<div class="hdr"><span>LIBIOXD</span><span>libioxd Programmer\'s Manual</span><span>LIBIOXD</span></div>')
                .replace("<span>INDEX()</span></div>\n</main>", "<span>LIBIOXD</span></div>\n</main>"))
    with open(os.path.join(HERE, "style.css"), "w") as f:
        f.write(STYLE)
    print(f"manual: {len(PAGES)} pages, {len(index)} names, version {version_str}")


if __name__ == "__main__":
    build()
