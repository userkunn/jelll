# JELLL V5 Linux rewrite - integration tests.
# Run: cd tests && python3 -m pytest -q
import http.server
import json
import os
import shutil
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
FIXTURES = TESTS / "fixtures"
E2E = TESTS / "e2e"
JELLL = TESTS.parent / "jelll"

TOOLCHAIN = os.environ.get("JELLL_TOOLCHAIN_DIR", "/home/user/.cache/jelll-tools")
ENV = dict(os.environ, JELLL_TOOLCHAIN_DIR=TOOLCHAIN)
# Playwright browsers live outside ~/.cache/ms-playwright's default so the
# installer controls them; both paths are snapshot-excluded.
ENV["PLAYWRIGHT_BROWSERS_PATH"] = os.environ.get(
    "PLAYWRIGHT_BROWSERS_PATH", "/home/user/.cache/jelll-tools/pw-browsers"
)


def run_jelll(args, cwd=None, timeout=180):
    return subprocess.run(
        [str(JELLL)] + args, cwd=cwd, env=ENV, capture_output=True, text=True, timeout=timeout
    )


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *args):
        pass


class Server:
    def __init__(self, dist: Path):
        self.dist = dist
        self.httpd = None
        self.thread = None
        self.port = None

    def __enter__(self):
        handler = lambda *a, **k: COOPCOEPHandler(*a, directory=str(self.dist), **k)
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.httpd.shutdown()
        self.httpd.server_close()


def browser_results(url):
    r = subprocess.run(
        ["node", "browser_test.mjs", url],
        cwd=E2E,
        env=ENV,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert r.stdout.strip(), f"no browser output: {r.stderr}"
    payload = json.loads(r.stdout.strip().splitlines()[-1])
    assert not payload["errors"], f"browser errors: {payload['errors']}"
    assert r.returncode == 0, f"browser harness failed: {r.stderr}"
    return payload["results"]


def build_and_run(fixture_name, threads=False):
    src = FIXTURES / fixture_name
    args = ["--threads"] if threads else []
    r = run_jelll(args + [str(src)], cwd=FIXTURES)
    assert r.returncode == 0, f"build failed: {r.stderr}"
    dist = src.parent / "dist"
    with Server(dist) as srv:
        return browser_results(f"http://127.0.0.1:{srv.port}/")


def build_error(fixture_name):
    src = FIXTURES / fixture_name
    r = run_jelll([str(src)], cwd=FIXTURES)
    assert r.returncode == 1, f"expected failure, got {r.returncode}: {r.stdout}{r.stderr}"
    return r


# ---- successful builds (browser-verified) ---------------------------------

def test_minimal():
    res = build_and_run("minimal.jelll")
    assert res["v"] == 5
    assert res["body"] == "add(2,3) = 5"


def test_multi_language():
    res = build_and_run("multi.jelll")
    assert res == {"c": 10, "rs": 20, "zig": 30, "sum": 60}


def test_rust_std():
    res = build_and_run("ruststd.jelll")
    assert res["sum"] == 4950


def test_sync_structs():
    res = build_and_run("sync.jelll")
    assert abs(res["area"] - 3.14159 * 2500) < 1.0
    assert abs(res["r2"] - 100.0) < 0.001
    assert res["x"] == 100
    assert res["freed"] is True
    assert res["stampValue"] == "42"
    assert res["r299"] == 7          # instance #299 works (beyond old 256 cap)
    assert res["live300"] == 300     # growable pool has no cap
    assert res["liveAfter"] == 0
    assert res["memOk"] is True      # JS<->C sharing across memory growth
    assert res["bufSize"] >= 32 * 1024 * 1024
    assert res["areaFn"] is True


def test_threads_mode():
    res = build_and_run("threads.jelll", threads=True)
    assert res["v"] == 42
    assert res["sab"] is True        # SharedArrayBuffer in --threads builds
    assert res["mem"] is True


def test_sync_lifecycle():
    # Value semantics of lang.sync structs: placement-new value-initialization
    # zeroes every cell on alloc (and again on slab reuse), zero leaks, live
    # counter exact. User-written C++ ctors/dtors are gone by design.
    res = build_and_run("lifecycle.jelll")
    assert res["r1"] == 0            # value-initialized on alloc, not garbage
    assert res["r2"] == 9.5
    assert res["r3"] == 3 and res["r4"] == 0
    assert res["r5"] == 0 and res["r6"] == 0   # recycled cell zeroed again


def test_quote_strings():
    # Single-quoted JS strings containing '}' must be masked as strings, or the
    # block scan ends early and the code is silently cut in half.
    res = build_and_run("strings.jelll")
    assert res["ok"] is True


def test_struct_extras():
    # Underscored struct name (My_Counter), IDL comments, methods WITHOUT
    # `extern "C"` (generated prototypes grant C linkage), a self-pointer
    # field, and a void method typed ": void" in the TS decl.
    res = build_and_run("structfx.jelll")
    assert res == {"r1": 42, "r2": 42, "r3": 0, "r4": 0, "r5": True}
    entry = (FIXTURES / ".bc_build" / "entry.ts").read_text()
    assert "reset(): void;" in entry


def test_modifiers_and_initializers():
    # Noise immunity: `#define`, `inline`, `__attribute__`, and brace/quote
    # comment traps in lang.c must not affect lang.sync processing at all --
    # the IDL is parsed before user code exists. lang.c remains fully free-form
    # C++ (macros, statics, attributes).
    res = build_and_run("modifiers.jelll")
    assert res == {"r1": 14, "r2": 28, "r3": 14, "r4": 7}


def test_regex_literals():
    # Braces inside JS regex literals (return /}/;) must not leak into block
    # brace matching; division must still parse as division.
    res = build_and_run("regex.jelll")
    assert res == {"r1": True, "r2": False, "r3": True, "r4": True, "r5": True}


def test_reserved_field_names():
    # Field names that collide with generated TS wrapper members
    # (constructor / _ptr / free / fromPtr / liveCount / slotCount).
    for fix in ("fieldctor.jelll", "fieldresv.jelll"):
        r = build_error(fix)
        assert "reserved" in r.stderr


def test_pointer_field_roundtrip():
    # IDL pointer field (`Holder* link;`): TS getter/setter expose the
    # unsigned int handle and it round-trips through wasm memory.
    res = build_and_run("fieldptr.jelll")
    assert res == {"r1": 1, "r2": 2}


def test_method_field_collision():
    r = build_error("fieldcoll.jelll")
    assert "collides" in r.stderr


def test_wildptr_guard():
    # free()/getters validate pointers arithmetically BEFORE dereferencing:
    # unmapped addresses, mapped garbage, cross-type pointers and double frees
    # are all rejected without trapping the wasm module.
    res = build_and_run("wildptr.jelll")
    assert res["stampAlive"] is True   # wrong-type free rejected
    assert res["boxAlive"] is True     # garbage frees rejected
    assert res["clean"] is True        # double free rejected
    assert res["recovered"] is True    # allocator still healthy


def test_threads_race():
    # 4 std::thread lambdas call the generated extern "C" API directly (through
    # the generated forward decls) for 20k alloc/free pairs; no races, no leaks.
    res = build_and_run("threads_race.jelll", threads=True)
    assert res["r1"] == 3.5
    assert res["live"] == 0


def test_order_and_assets():
    res = build_and_run("order.jelll")
    assert res["order"][0] == "js1"
    assert res["order"][1].startswith("js1b:")
    assert res["order"][2] == "ts1"
    assert res["order"][3] == "js2"
    html = (FIXTURES / "dist" / "index.html").read_text()
    assert "#app { color: #0f0; }" in html
    assert '<div id="app"></div>' in html


def test_ignores_comments_and_strings():
    res = build_and_run("ignore.jelll")
    assert res == {"ok": True, "sLen": len("lang.zig { not_a_block }")}


def test_legacy_sync_marker():
    # The scraping-era `// @jelll-sync` marker in lang.c must fail fast with a
    # migration hint pointing at lang.sync.
    r = build_error("legacysync.jelll")
    assert "@jelll-sync" in r.stderr
    assert "lang.sync" in r.stderr
    assert "移行" in r.stderr


def test_missing_method_body():
    # A method declared in lang.sync with no body fails at LINK time through
    # the generated link reference -- not at runtime.
    r = build_error("missingmethod.jelll")
    assert "リンクエラー" in r.stderr


def test_bad_idl_type():
    # Unsupported IDL type names are a precise BuildError at parse time.
    r = build_error("badtype.jelll")
    assert "Unsupported type" in r.stderr
    assert "string" in r.stderr


def test_legacy_extension():
    res = build_and_run("legacy.tsbc")
    assert res["v"] == 5


# ---- error paths (manual error reference) ---------------------------------

def test_unsupported_extension():
    r = build_error("badext.txt")
    assert "Unsupported file extension" in r.stderr


def test_no_native_section():
    r = build_error("nonative.jelll")
    assert "No lang.c section found" in r.stderr


def test_no_script_section():
    r = build_error("noscript.jelll")
    assert "No script section found" in r.stderr


def test_broken_c():
    r = build_error("brokenc.jelll")
    assert "emcc build failed" in r.stderr
    assert "emcc.stderr.log" in r.stderr


def test_broken_ts():
    r = build_error("brokents.jelll")
    assert "esbuild bundle failed" in r.stderr
    assert "esbuild.stderr.log" in r.stderr


def test_missing_call_target():
    r = build_error("missingcall.jelll")
    assert "Failed to read call target" in r.stderr


def test_circular_call():
    r = run_jelll([str(FIXTURES / "circ" / "a.jelll")], cwd=FIXTURES)
    assert r.returncode == 1
    assert "Circular call detected" in r.stderr


# ---- outputs / structure ---------------------------------------------------

def test_outputs_and_logs():
    src = FIXTURES / "minimal.jelll"
    r = run_jelll([str(src)], cwd=FIXTURES)
    assert r.returncode == 0
    dist = FIXTURES / "dist"
    bdir = FIXTURES / ".bc_build"
    for name in ("native.wasm", "native.mjs", "bundle.js", "index.html"):
        assert (dist / name).exists(), f"missing {name}"
    for name in ("core.cpp", "entry.js", "emcc.stderr.log", "esbuild.stderr.log"):
        assert (bdir / name).exists(), f"missing {name}"
    core = (bdir / "core.cpp").read_text()
    assert "#line" in core and "int add" in core
    bundle = (dist / "bundle.js").read_text()
    assert '"native.wasm"' in bundle
    assert "import.meta.url" in bundle
    assert '"./native.mjs"' in bundle


# ---- dev server ------------------------------------------------------------

def test_dev_server_and_livereload(tmp_path):
    work = tmp_path / "proj"
    shutil.copytree(FIXTURES, work, ignore=shutil.ignore_patterns("dist", ".bc_build", "circ", "*.txt"))
    src = work / "minimal.jelll"
    proc = subprocess.Popen(
        [str(JELLL), "dev", str(src)],
        cwd=work,
        env=ENV,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        deadline = time.time() + 60
        status = None
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", 8000), timeout=1):
                    break
            except OSError:
                time.sleep(0.3)
        else:
            pytest.fail("dev server did not start")

        import urllib.request

        def get(path):
            with urllib.request.urlopen("http://127.0.0.1:8000" + path, timeout=5) as r:
                return r.headers, r.read()

        headers, body = get("/")
        # Default builds are NOT cross-origin isolated (works on any static
        # host); COOP/COEP are only sent for --threads builds.
        assert "Cross-Origin-Embedder-Policy" not in headers
        assert b"bundle.js" in body
        assert b"__jelll__/status" in body  # live reload script injected

        headers, body = get("/bundle.js")
        assert headers["Content-Type"].startswith("text/javascript")
        headers, body = get("/native.wasm")
        assert headers["Content-Type"] == "application/wasm"

        _, body = get("/__jelll__/status")
        id1 = json.loads(body)["id"]

        time.sleep(1.2)  # mtime resolution
        src.write_text(src.read_text() + "\n// touched\n")
        deadline = time.time() + 30
        id2 = None
        while time.time() < deadline:
            _, body = get("/__jelll__/status")
            id2 = json.loads(body)["id"]
            if id2 != id1:
                break
            time.sleep(0.5)
        assert id2 != id1, "no rebuild after change"
        # Slowloris resistance: a client holding a half-sent header must not
        # block others. SO_RCVTIMEO(2s) caps the stalled read and the truncated
        # request gets 400 + close.
        # (a) Bounded concurrency: a client holding a half-sent header must not
        #     delay other requests (handlers run in a bounded thread pool; the
        #     serial server would block ~2s inside its read timeout).
        t0 = time.time()
        stalled = socket.create_connection(("127.0.0.1", 8000), timeout=2)
        stalled.sendall(b"GET / HTTP/1.1\r\nHost: 127.0.0.1:8000")  # never completes
        with socket.create_connection(("127.0.0.1", 8000), timeout=2) as s2:
            s2.sendall(b"GET /__jelll__/status HTTP/1.1\r\nHost: 127.0.0.1:8000\r\n\r\n")
            assert s2.recv(4096).startswith(b"HTTP/1.1 200")
        latency = time.time() - t0
        assert latency < 1.5, f"status blocked {latency:.1f}s behind a stalled client"
        # (b) Truncated request (client half-closes): 400 + close, promptly.
        half = socket.create_connection(("127.0.0.1", 8000), timeout=2)
        half.sendall(b"GET / HTTP/1.1\r\nHost: 127.0.0.1:8000")
        half.shutdown(socket.SHUT_WR)
        assert half.recv(4096).startswith(b"HTTP/1.1 400")
        half.close()
        stalled.close()
        # (b2) S-05: DNS rebinding - a non-loopback Host must be rejected 421.
        with socket.create_connection(("127.0.0.1", 8000), timeout=2) as s4:
            s4.sendall(b"GET /index.html HTTP/1.1\r\nHost: evil.example.com\r\n\r\n")
            assert s4.recv(4096).startswith(b"HTTP/1.1 421")
        # (b3) R-04: a request split across TCP reads must be reassembled
        # intact (the old loop appended buf[0..n) on every read).
        with socket.create_connection(("127.0.0.1", 8000), timeout=5) as s5:
            s5.sendall(b"GET /index.html HTTP/1.1\r\n")
            time.sleep(0.2)
            s5.sendall(b"Host: 127.0.0.1:8000\r\n\r\n")
            assert s5.recv(8192).startswith(b"HTTP/1.1 200")
        # (c) LFI: absolute-path URLs must never escape the dist dir. They are
        #     served as relative paths inside dist (404) or rejected outright
        #     (400) - but must never return file contents.
        for evil in (b"GET //etc/passwd HTTP/1.1\r\nHost: 127.0.0.1:8000\r\n\r\n",
                     b"GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: 127.0.0.1:8000\r\n\r\n",
                     b"GET /../../etc/passwd HTTP/1.1\r\nHost: 127.0.0.1:8000\r\n\r\n"):
            with socket.create_connection(("127.0.0.1", 8000), timeout=2) as s3:
                s3.sendall(evil)
                resp = s3.recv(4096)
                assert resp.startswith((b"HTTP/1.1 400", b"HTTP/1.1 404")), (evil, resp[:40])
                assert b"root:" not in resp, (evil, resp[:80])
    finally:
        # Cleanup only - assertions must never skip process teardown (a leaked
        # listener on port 8000 poisons every later run).
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


# ---- CLI basics ------------------------------------------------------------

def test_cli_version_and_usage():
    r = run_jelll(["--version"])
    assert r.returncode == 0 and "5.0" in r.stdout
    r = run_jelll([])
    assert r.returncode == 2
    r = run_jelll(["--nope"])
    assert r.returncode == 2


# ---- Red-team regressions (M-01 / M-02 / S-01) ------------------------------

def test_deep_template_nesting_rejected(tmp_path):
    """M-01: pathological ${ nesting must hit the depth cap, not segfault."""
    # 65 levels of template nesting > kMaxNest (64).
    deep = ("`${" * 65) + "1" + ("}`" * 65)   # 65 nested template literals
    src = tmp_path / "tpldeep.jelll"
    src.write_text("lang.js {\nconst s = " + deep + ";\n}\n")
    r = run_jelll([str(src)])
    assert r.returncode == 1, (r.returncode, r.stdout[-200:], r.stderr[-200:])
    assert "Nesting too deep" in r.stderr or "深い" in r.stderr, r.stderr


def test_call_dag_expands_each_file_once(tmp_path):
    """M-02: `call` is include-once - a DAG shares nodes instead of exploding."""
    # depth-20 chain where each level calls the SAME next file twice:
    # naive re-expansion = 2^20 visits (seconds-to-minutes); include-once = 21.
    n = 20
    for i in range(n):
        nxt = i + 1
        (tmp_path / f"f{i}.jelll").write_text(
            f"call \"f{nxt}.jelll\"\ncall \"f{nxt}.jelll\"\n"
        )
    (tmp_path / f"f{n}.jelll").write_text(
        "lang.js {\nconsole.log('shared-block-once');\n}\n"
    )
    main = tmp_path / "main.jelll"
    main.write_text(
        "lang.c {\nextern \"C\" { int add(int a, int b) { return a + b; } }\n}\n"
        "call \"f0.jelll\"\n"
        "lang.js {\nnative.add(1, 2);\n}\n"
    )
    t0 = time.time()
    r = run_jelll([str(main)], timeout=120)
    assert r.returncode == 0, r.stdout[-200:] + r.stderr[-200:]
    assert time.time() - t0 < 30, "call graph did not stay linear"
    entry = (tmp_path / ".bc_build" / "entry.js").read_text()
    assert entry.count("shared-block-once") == 1, "shared file expanded more than once"


def test_call_outside_project_root_rejected(tmp_path):
    """S-01: call targets must stay inside the project root."""
    outside = Path("/tmp/jelll_outside_target.jelll")
    outside.write_text("lang.js {\nconst q = 1;\n}\n")
    main = tmp_path / "main.jelll"
    main.write_text("call \"/tmp/jelll_outside_target.jelll\"\n")
    try:
        r = run_jelll([str(main)])
    finally:
        outside.unlink(missing_ok=True)
    assert r.returncode == 1
    assert "outside project root" in r.stderr, r.stderr


# ---- Second regression audit (R-01 .. R-10) ---------------------------------

def test_r01_nonstruct_pointers_rejected():
    """R-01: int*/void*/multi-pointers never enter the handle scheme."""
    for fx, needle in (
        ("badptr1.jelll", "int*"),
        ("badptr2.jelll", "void*"),
        ("badptr3.jelll", "A**"),
    ):
        r = build_error(fx)
        assert "Unsupported type" in r.stderr, (fx, r.stderr)
        assert needle in r.stderr, (fx, r.stderr)


def test_r10_pointer_return_builds_and_runs():
    """R-10: `get() -> Foo*` compiles and returns a working handle."""
    res = build_and_run("ptrret.jelll")
    assert res["same"] is True
    assert res["gv"] == 7


def test_r05_generation_churn_no_leak():
    """R-05 (3rd audit): the exact 1,048,574-cycle ABA schedule.

    A stale generation-1 handle must never match (or read through) the object
    allocated after the generation ceiling forced a slot retirement."""
    res = build_and_run("genloop.jelll")
    assert res["staleEqFresh"] is False     # retired slot never re-mints the old token
    assert res["freshIdx"] > 511            # retirement minted a VIRGIN slot (not a
                                            # recycled gen-reset one - the 3rd audit bug)
    assert res["staleRead"] == 0            # stale handle reads nothing (no ABA/UAF)
    assert res["freshRead"] == 123          # the fresh object is intact
    assert res["liveEnd"] == 0              # no stranded cells


def test_r02_pinned_method_survives_concurrent_free():
    """R-02: free_h() drains pins; a running method body keeps its payload."""
    res = build_and_run("pinrace.jelll", threads=True)
    assert res["r"] == 6       # body read 5, wrote 6, returned 6 - payload live
    assert res["liveAfter"] == 0   # the drained free landed afterwards


def test_r02_double_free_exclusive():
    """R-02 (3rd audit): of two concurrent frees on one handle, exactly one
    wins the destroy ticket; the loser must not touch a recycled cell."""
    res = build_and_run("dfree.jelll", threads=True)
    assert res["r"] == 1               # second free_begin won nothing
    assert res["liveAfter"] == 1       # destroyed exactly once (only `a` lives)
    assert res["survivor"] == 0        # a.x untouched by the losing destroy
    assert res["liveFinal"] == 0


def test_r03_many_blocks_linear_time(tmp_path):
    """R-03: 12,000 empty lang.c blocks must scan in linear time."""
    srcf = tmp_path / "many.jelll"
    srcf.write_text("lang.c {}\n" * 12000)
    t0 = time.time()
    r = run_jelll([str(srcf)], cwd=tmp_path)
    dt = time.time() - t0
    assert r.returncode == 1
    assert "No script section found" in r.stderr, r.stderr
    assert dt < 3.0, f"block scan still quadratic: {dt:.1f}s for 12000 blocks"
