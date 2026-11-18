# JELLL

**One `.jelll` file in, a live WebAssembly web app out.** C/C++ (or Rust, or Zig)
plus JS/TS/TSX in a single source file, built with one command. A Linux-native
build tool with a hardened pipeline: explicit exports, a handle-based runtime
(malformed pointers cannot crash or corrupt), and a live-reload dev server.

**1 つの `.jelll` ファイルから、動く WebAssembly 製 Web アプリを生成します。**
C/C++(または Rust、Zig)と JS/TS/TSX を単一ファイルにまとめ、コマンド一発でビルド。
Linux 向けネイティブ実装で、明示エクスポート・ハンドルベースのランタイム
(不正ポインタでもクラッシュや破壊が起きない)・ライブリロード付き開発サーバを備えます。

> **Security hardening history / セキュリティ強化の経緯**
>
> - **V6 — final verification audit: all green / 第3次最終検証監査・全項目合格**:
>   generation counters are append-only (a saturated slot retires forever and its
>   cell continues on a virgin slot; no ABA revival) / generation は加算のみで
>   リセット禁止(上限到達スロットは永久退役、後継は virgin スロットのみで ABA 復活なし)。
>   `free` takes exclusive destroy ownership via a one-shot LIVE→DRAINING
>   transition under the mutex and re-checks index+generation before the
>   destructor / `free` は mutex 下の一回きり LIVE→DRAINING 遷移で解放権を排他取得し、
>   破棄直前に index+generation を再照合。`/proc` verification failure is
>   fail-closed (no path fallback) / fd 実体検証不能時は fail-closed で拒絶。
> - **V5.2 — regression audit R-01…R-10 / 第2次監査対応**: IDL pointers are
>   exactly one level to a declared struct (`Foo*`); `int*` / `void*` / `Foo**`
>   are syntax errors / IDL のポインタは struct 単一ポインタのみ(int*/void*/多重は構文エラー)。
>   Pointer returns map through `__jelll_handle_of_*` / ポインタ戻り値はハンドル変換。
>   Handles are 20-bit generation × 12-bit slot / ハンドルは 20bit 世代 × 12bit スロット。
>   Method bodies run pin-protected against concurrent frees / メソッド実行中はピンで
>   並行 free から保護。`call` TOCTOU closed via fd verification, block scanning
>   is linear-time / `call` の TOCTOU を fd 検証で封鎖・ブロック走査は線形時間。
>   Split HTTP requests reassemble correctly / 分割 HTTP リクエストを正しく再構成。
>   llvm-nm failure fails the build; setup verifies pinned digests/commits /
>   llvm-nm 失敗はビルド失敗、セットアップは固定 digest/commit を検証。
> - **V5.1 — first red-team audit / 初回レッドチーム監査対応**: no raw pointers
>   cross into JS — the exported surface is the handle API (`T_alloc_h`,
>   `T_free_h`, `T_get_f_h`, `T_set_f_h`, `S_m_h`, `T_live_count`,
>   `T_slot_count`); raw `T_alloc` / `T_free` / … stay inside `lang.c` /
>   JS への生ポインタ露出を廃止しハンドル API のみ公開(生 API は lang.c 内部専用)。
>   `--export-all` replaced by explicit `-sEXPORTED_FUNCTIONS` / 明示エクスポートに移行。
>   `call` is include-once with depth/size caps and project-root confinement /
>   `call` は include-once・深さ/サイズ上限付き・プロジェクトルート内に限定。
>   dev server validates the `Host` header (421 otherwise) / dev サーバは Host 検証(違反 421)。

---

## Prerequisites / 必要環境

| Tool / ツール | Purpose / 用途 | Notes / 備考 |
|---------------------|----------------------------|------------------------------------|
| GCC/G++ (C++17) | Build jelll itself / jelll 本体のビルド | `make` |
| Node.js + npm | esbuild install, Zig index / esbuild 導入・Zig 索引 | https://nodejs.org/ |
| Emscripten (emcc) | C/C++ → WASM | auto via `jelll --setup` / 自動導入可 |
| esbuild | JS/TS bundling / JS/TS バンドル | auto via `jelll --setup` / 自動導入可 |
| Rust (rustc + wasm32) | Rust → WASM (optional / 任意) | auto via `jelll --setup` |
| Zig | Zig → WASM (optional / 任意) | auto via `jelll --setup` |

## Build & install / ビルドとインストール

```sh
make            # produces ./jelll (-Wall -Wextra -Wpedantic -Werror, zero warnings)
                # ./jelll を生成(警告ゼロ)
sudo make install   # → /usr/local/bin/jelll
```

## Toolchain setup / ツールチェーンセットアップ

```sh
jelll --setup    # or / または jelll setup
```

A multi-select TUI: arrows to move, space to toggle, Enter to confirm. Selected
toolchains are downloaded, installed and registered on `PATH` (install root:
`~/.local/share/jelll/tools`, override with `JELLL_TOOLCHAIN_DIR`). Set
`JELLL_SETUP_DRY_RUN=1` to print commands without executing (for tests).
Every download is verified against digests/commits pinned in the binary.

矢印キーで移動、スペースで選択/解除、Enter で確定するマルチセレクト UI です。
選択したコンパイラを自動でダウンロード・インストール・PATH 登録します(インストール先:
`~/.local/share/jelll/tools`、`JELLL_TOOLCHAIN_DIR` で変更可)。
`JELLL_SETUP_DRY_RUN=1` でコマンド表示のみ(テスト用)。ダウンロード物はすべて、
バイナリに埋め込んだ digest/commit と照合されます。

Tool discovery works without any environment variables: `JELLL_TOOLCHAIN_DIR` →
`~/.local/share/jelll/tools` (the `--setup` default) → `~/.cache/jelll-tools`
(the test-suite default). The first **existing** directory wins.

ツール探索は環境変数なしでも動きます(`JELLL_TOOLCHAIN_DIR` → `~/.local/share/jelll/tools`
→ `~/.cache/jelll-tools` の順に、存在するディレクトリを自動採用)。テストハーネス以外に
export は不要です。

## Usage / 使い方

```sh
jelll app.jelll            # build → dist/        # ビルド → dist/ に出力
jelll dev app.jelll        # dev server http://127.0.0.1:8000 (rebuild + live reload on save)
                           # 開発サーバ(保存で自動リビルド・ライブリロード)
jelll --watch app.jelll    # alias of / エイリアス jelll dev
jelll --threads app.jelll  # shared memory + threads (needs COOP/COEP to serve)
                           # 共有メモリー+スレッド版(配信に COOP/COEP 要)
jelll --version / --help
```

Accepted extensions / 対応拡張子: `.jelll` (recommended / 推奨), `.jsbc`, `.tsbc`,
`.tsxbc` (legacy aliases with identical behavior / 同一処理のレガシー・エイリアス)。
`--threads` builds accept `lang.c` + `lang.sync` only (Rust/Zig objects cannot
carry the shared-memory feature set) / `--threads` は lang.c + lang.sync のみ対象
(Rust/Zig は共有メモリーの feature 整合性を満たせないため)。

## Input format / 入力ファイル形式

Code lives in `lang.X { ... }` blocks (see the bundled technical manual for the
full reference). Blocks of the same language concatenate in source order; the
JS/TS/TSX compile mode is auto-detected (TSX > TS > JS). Blocks and `call`
directives inside comments or string literals are ignored. `call "relative/path"`
expands recursively (include-once: each file expands once, so DAGs stay linear),
cycles are detected, and targets outside the project root are rejected. At least
one native block (`lang.c` / `lang.rs` / `lang.zig`) and one script block
(`lang.js` / `lang.ts` / `lang.tsx`) are required. Struct/method declarations go
in a `lang.sync` (IDL) block; `lang.c` carries method bodies only.

コードは `lang.X { ... }` ブロックに記述します(詳細は同梱の技術マニュアル参照)。
同じ言語のブロックは出現順に連結、JS/TS/TSX のコンパイルモードは TSX > TS > JS で
自動判定。コメント・文字列リテラル内のブロックや `call` は無視されます。
`call "relative/path"` は再帰展開します(include-once: 同一ファイルは1回のみ展開され、
DAG でも線形)。循環参照は検出し、プロジェクトルート外のターゲットは拒否します。
ネイティブブロック(c/rs/zig)とスクリプトブロック(js/ts/tsx)が各1つ以上必要です。
構造体/メソッド宣言は `lang.sync`(IDL)ブロック、本体は `lang.c` に記述します。

## Output layout / 出力構成

```
project/
├── app.jelll
├── .bc_build/               # intermediates / 中間ファイル
│   ├── core.cpp / core.rs / core.zig / core_rs.o / libcore_rs.a ...
│   ├── entry.js|.ts|.tsx    # glue + sync classes + scripts (source order)
│   │                        # グルーコード + sync クラス + スクリプト(出現順)
│   ├── emcc.stderr.log / rustc.stderr.log / zig.stderr.log / esbuild.stderr.log
└── dist/                    # serve this directory / このディレクトリを配信
    ├── native.wasm          # native code → WASM (growable heap: 32MB → 2GB)
    │                        # ネイティブコード → WASM(自動成長ヒープ 32MB→2GB)
    ├── native.mjs           # Emscripten ES-module glue / Emscripten ES モジュール
    ├── bundle.js            # bundled JS/TS (ESM) / バンドル済み JS/TS
    └── index.html           # with CSS/HTML injected / CSS/HTML 注入済み
```

## WASM runtime API / WASM ランタイム API

| Global / グローバル | Type / 型 | Description / 説明 |
|----------------|--------------------|---------------------------------------|
| `native` | Object | Exported functions as properties (`native.add(2,2)`). Raw pointer APIs are never exported — only the handle API (`T_alloc_h`, …) / エクスポート済み関数が利用可能。生ポインタ API は非公開でハンドル版のみ |
| `sharedBuffer` | ArrayBufferLike | View over the whole heap (`new Uint8Array(sharedBuffer)`) / ヒープ全体のビュー |
| `sharedMemory` | WebAssembly.Memory | The heap itself (auto-growing: 32MB initial → 2GB max) / ヒープ本体(自動成長) |

**There is no fixed memory cap** / **メモリー上限はありません**。The heap grows on
demand. After growth `sharedBuffer` is swapped, so re-read it through the global
(getter). `sharedBuffer` is always the very same object as
`sharedMemory.buffer`: JS writes are visible to native code directly. The name
is kept for compatibility; in regular builds it is a plain `ArrayBuffer`
(a `SharedArrayBuffer` only with `--threads`).

ヒープは必要に応じて自動成長します。成長後は `sharedBuffer` が差し替わるため、
グローバル経由(getter)で参照し直してください。`sharedBuffer` は常に
`sharedMemory.buffer` と同一オブジェクトで、JS 側の書き込みがそのままネイティブから
見えます。名前は互換のため残していますが、通常ビルドでは一般の `ArrayBuffer` です
(`--threads` 時のみ `SharedArrayBuffer`)。

## lang.sync (IDL block / IDL ブロック)

Struct and method declarations live in a **`lang.sync` block (IDL)**, separate
from the C++ bodies. Scraping struct declarations out of C++ is not supported.

構造体とメソッドの宣言は **`lang.sync` ブロック(IDL)** に書き、C++ 本体とは切り離します。

```
lang.sync {
  struct Circle {
    float x;
    float y;
    float radius;
    area() -> float;
    scale(float s) -> float;
  }
}

lang.c {
float Circle_area(Circle* self) { return 3.14159f * self->radius * self->radius; }
float Circle_scale(Circle* self, float s) { self->radius *= s; return self->radius; }
}
```

**Syntax / 構文**: `struct <Name> { <Type> <field>; ... <method>(<Type> <arg>, ...) -> <Ret>; ... }`
One declaration per `;`. Comments inside the struct body are ignored. A trailing
`;` after the closing `}` is allowed (`struct X { ... };`).

宣言は `;` 区切り 1 行 1 宣言。構造体内のコメントは無視されます。`}` の後の `;` は
付けても付けなくても構いません(`struct X { ... };` が使えます)。

**Type table — the only types the IDL accepts / 型テーブル(IDL で使えるのは以下のみ)**

| IDL | Generated C++ / C++ 生成型 | TS | Notes / 備考 |
|---|---|---|---|
| `int` / `int32` | `int` | number | |
| `uint` / `uint32` | `unsigned int` | number | |
| `int64` | `long long` | bigint | signed 64-bit accessors / 符号付き64ビット |
| `uint64` | `unsigned long long` | bigint | |
| `float` | `float` | number | |
| `double` | `double` | number | |
| `bool` | `bool` | boolean | boundary is `unsigned char` / 境界は `unsigned char` |
| `<Struct>*` | `<Struct>*` | number (handle / ハンドル) | getters return a handle, setters accept one — raw pointer values never reach JS. The base must be a declared `lang.sync` struct: `int*`, `void*`, `Foo**` are rejected / getter はハンドルを返し setter はハンドルを受け取る(ポインタ素値は JS に露出しない)。基底は lang.sync の struct のみ(int*/void*/多重ポインタは拒否) |
| `void` | `void` | void | return only / 戻り値専用 |

- Multi-word type names (`long long`) and qualifiers (`const`) are not IDL; use
  the table spellings. Anything outside the table is an immediate
  `Unsupported type` error / 多語型名や `const` は IDL では使わずテーブル綴りを
  使用。テーブル外の型は即 `Unsupported type` エラー。
- **Method bodies are free-form C++ in `lang.c`**. The generated `extern "C"`
  prototypes grant C linkage automatically, so `extern "C"` in the body is
  optional. Macros, `inline`, `__attribute__`, comment styles — all unconstrained
  (the IDL is parsed independently of user code) / **メソッド本体は lang.c に自由に
  書けます**。生成プロトタイプが C リンケージを与えるので `extern "C"` はあっても
  なくても動きます。マクロ・`inline`・属性・コメントも完全自由(IDL はユーザーコードと
  独立に解析)。
- **Never re-declare the struct in `lang.c`** (that is an error) — field
  declarations are generated / **構造体を lang.c に再定義しないでください**(エラー)。
- **A missing method body is a link error**, caught at build time via generated
  link references (never a runtime trap) / **メソッド本体が無いとリンクエラー**
  (ビルドで検出、実行時に壊れません)。
- **IDL structs are trivial**: no user constructors/destructors. `X_alloc()`
  value-initializes every field to 0 (recycled cells too); `X_free()` returns the
  slot (double-free / wrong-pointer calls are detected and ignored) / **IDL 構造体は
  自明型**: ctor/dtor は書けず、`X_alloc()` は全フィールド 0 初期化(再利用セルも毎回)、
  `X_free()` はスロット返却(二重 free/誤ポインタは検出され無視)。
- **Reserved names / 予約名**: `constructor` / `free` / `fromPtr` / `liveCount` /
  `slotCount` / `_ptr` are rejected for structs, fields, methods and parameters.
  Method names colliding with fields or with generated symbols (`X_alloc`,
  `X_free`, `X_live_count`, `X_slot_count`, `X_get_*`, `X_set_*`, and their `_h`
  forms) are rejected / TS クラスと衝突する予約名・生成名との衝突は拒否。
- **A legacy `// @jelll-sync` marker anywhere in `lang.c` is an immediate error**
  with a migration hint / 旧 `// @jelll-sync` マーカーが残っていると移行を指示する
  エラーになります。
- `Circle.liveCount()` / `Circle.slotCount()` report live and reserved slots /
  使用中/確保済みスロット数を取得できます。

### Generated allocator & handle runtime / 生成アロケータとハンドルランタイム

- **O(1) alloc/free**: each slab (256 cells) owns a free list; allocation pops
  the head, release pushes back. No linear slab scans / 各スラブ(256セル)が
  フリーリストを持ち、確保は先頭ポップ・解放はプッシュのみ(O(1))。
- **Handles, not pointers / ポインタではなくハンドル**: every JS-facing entry
  takes or returns an opaque handle `(generation << 12 | slot)` resolved through
  an authoritative slot table that checks liveness, generation and the exact
  type. Forged, stale, cross-type and double-freed handles are rejected
  structurally — malformed input can return 0 or do nothing, never crash or
  corrupt / JS 境界はすべてハンドル(世代+スロット)を介し、生存・世代・型をスロット
  テーブルで検証。偽造・stale・型混用・二重 free は構造的に拒否(壊れることは
  ありません)。
- **Generation is append-only (no ABA) / generation は加算のみ(ABA なし)**:
  counters never reset or wrap. A slot at the 20-bit ceiling retires forever and
  its cell continues on a fresh, never-used slot; if no safe slot remains,
  `alloc` reports failure (0) / カウンタのリセット/巻き戻しは一切なし。上限到達
  スロットは永久退役してセルには virgin スロットを割り当て、枯渇時は alloc が 0。
  With `--threads`, method bodies run with their receiver (and pointer
  arguments) **pinned**: `free` marks the slot draining, refuses new calls and
  destroys only after the last pin drops; destroy ownership is exclusive (a
  concurrent second free returns immediately) and re-validates index+generation
  right before the destructor / `--threads` ではメソッド実行中は対象をピン留めし、
  free はドレイン後にのみ破棄。解放権は排他で、破棄直前に index+generation を再照合。
  Non-thread builds generate no locking at all / 非スレッドビルドではロックは生成されません。
- **Raw C API stays in `lang.c` / 生 C API は lang.c 内部専用**: `X_alloc()`,
  `X_free()`, `X_get_*()`, `X_set_*()`, `X_method()` keep pointer signatures for
  C interop and are validated (slab membership + liveness + type) before any
  dereference, but they are **not exported to JS** / ポインタ版の生 API は C 相互
  作用用に残しますが JS にはエクスポートしません。
- **Wild-pointer safety / 野良ポインタ対策**: every raw-pointer entry validates
  arithmetically (heap range via `__builtin_wasm_memory_size`, alignment) and by
  slab membership **before** dereferencing, so garbage like `X_free(0xdeadbeef)`
  is rejected without trapping the module; getters return 0/false on invalid
  input (`test_wildptr_guard`) / 全ての生ポインタ経路はデリファレンス前に算術検証と
  スラブ所属検証を行うため、ゴミポインタでも wasm トラップを起こさず拒否されます。
- **Call-order freedom / 呼び出し順序の自由**: prototypes precede user code, so
  generated API is callable from global constructors and `std::thread` lambdas
  alike / プロトタイプが本体より前に出るため、グローバル ctor やスレッドラムダからも
  生成 API を呼べます。
- **Memory policy / メモリ方針**: an empty slab is kept warm (one), the rest are
  released back / 空スラブは 1 枚だけ暖気キャッシュし、2 枚目以降は解放。
- **Linkage / リンケージ**: `extern "C"` prototypes and link references are
  generated from the IDL, so bodies need no `extern "C"` and calls from JS are
  direct. Non-method free functions still need `extern "C"` to be visible as
  `native.X`. `lang.c` is otherwise full, free C++ / IDL から `extern "C"`
  プロトタイプとリンク参照を生成。メソッド以外の関数を `native.X` で呼ぶ場合は
  従来通り `extern "C"` で囲みます。

## Dev mode / 開発モード

`jelll dev` builds once, serves `dist/` on 127.0.0.1:8000, polls for changes
(500ms), rebuilds on save and live-reloads. Regular builds need no special
headers and run on any static host; only `--threads` builds get COOP/COEP (the
browser requirement for `SharedArrayBuffer`). Header reads are cut at 2s and
writes at 5s; incomplete requests get 400 + close (Slowloris resistance). A
bounded pool (8 handlers) keeps live-reload polling responsive even while a huge
`native.wasm` streams out. Paths are slash-normalized and containment-checked
with `weakly_canonical`, so `//etc/passwd` or `..` URLs cannot read host files
(LFI). Non-loopback `Host` headers are rejected with 421 (DNS rebinding), and a
per-project `flock` rejects concurrent builds of the same project with a clean
error. `stop_dev_server()` shuts the listener down safely for embedding.

`jelll dev` は初回ビルド後に 127.0.0.1:8000 で配信、500ms ポーリングで変更検知、
保存で自動リビルド、ライブリロードします。通常ビルドは特殊ヘッダー不要でどの静的
ホスティングでも動きます(`--threads` の配信時のみ COOP/COEP を付与)。ヘッダー受信は
2 秒・送信は 5 秒で打ち切り、ヘッダー未完の接続は 400 で即クローズ(Slowloris 耐性)。
ハンドラは上限 8 スレッドのプールで並行処理され、巨大な `native.wasm` の送信中でも
ライブリロードがブロックされません。パスはスラッシュ正規化 + `weakly_canonical` の
内包検証で LFI を防ぎ、非ループバックの `Host` は 421 で拒否(DNS rebinding 対策)。
同一プロジェクトへの同時ビルドは flock で検出し、明確なエラーで拒否します。

## Intentional deviations from the manual / 仕様書からの意図的な差分・解釈（重要）

The technical manual is treated as **reference**; where it contradicts or omits,
"make it work correctly" wins.

技術マニュアルは**参考扱い**とし、矛盾・欠落は「ちゃんと動く」を優先して解決しています。

1. **`lang.c` compiles as C++ / `lang.c` は C++ としてコンパイル**
   The manual says "C/C++" but every sample is C++ with `extern "C"`, and the
   intermediate is `core.cpp`. Pure C that is illegal C++ is a compile error /
   サンプルは全て C++ で中間も `core.cpp`。C++ として非合法な純 C はコンパイルエラー。
2. **No fixed 16MiB memory / 固定16MiBメモリーなし** (explicit requirement /
   ユーザー指示): the heap auto-grows (32MB initial → 2GB max). `sharedBuffer` /
   `sharedMemory` expose the real heap / ヒープは自動成長し、`sharedBuffer`/
   `sharedMemory` は実ヒープを公開します。
3. **`dist/native.mjs` is part of the output / 出力に含む** — Emscripten needs its
   JS glue; the manual's three-file layout omits it, so the four-file layout is
   canonical / Emscripten の JS グルーが必須のため 4 ファイル構成を正式とします。
4. **Shared memory only with `--threads` / 共有メモリーは `--threads` のみ** —
   `SharedArrayBuffer` requires COOP/COEP, which plain static hosting lacks;
   the dev server adds the headers only in that mode / COOP/COEP が必要なため。
5. **"No lang.c section found" scope / 適用範囲** — raised only when **no**
   native block exists (lang.rs / lang.zig also qualify; see the hint) /
   ネイティブブロックが皆無のときのみ(lang.rs/lang.zig でも可)。
6. **Script blocks concatenate in source order / スクリプトは出現順に連結** (the
   old per-language regrouping broke ordering). Comment/string masking is
   per-language, handles `'}'` in single-quoted strings, Rust lifetimes `'a`,
   and JS regex literals such as `return /}/;` / コメント/文字列マスクは言語別で、
   シングルクォート内の `}`・Rust ライフタイム・JS 正規表現リテラルも正しく判定。
7. **Rust: direct rustc, no external crates / Rust は rustc 直コンパイル・外部
   クレート非対応** — `--crate-type=staticlib` plus the object file gives
   `#[no_mangle]` exports and lazy std linking; std is single-thread scoped.
   Non-`extern "C"` functions export mangled (not API). The `.o` + `.a` link is
   sound: wasm-ld extracts library members lazily and symbols already defined
   in `core_rs.o` are not pulled from the archive (measured reproducible;
   `--whole-archive` on `.a`-only bloated the binary +39% and leaked 759 extra
   exports) / `.o`+`.a` の二重リンクは遅延抽出により重複定義にならず実測再現可能。
   Toolchain pins + the test suite keep linker changes loud (build-time), never
   silent runtime breakage / ピン留めツールチェーン+テストでリンカ変更はビルド時に顕在化。
8. **Zig targets `wasm32-freestanding`** (freestanding std subset only) /
   Zig は `wasm32-freestanding`(freestanding で動く std サブセットのみ)。
9. **Legacy extensions `.jsbc/.tsbc/.tsxbc` alias `.jelll`** (identical mode
   detection) / レガシー拡張子は `.jelll` のエイリアス。
10. **No shell in the pipeline / コマンド実行にシェル不使用** (the old injection /
    space-in-path breakage is gone); outputs are written atomically / 出力は
    アトミック書き込み。
11. **`lang.sync` IDL replaces struct scraping / IDL 導入・スクレイピング廃止** —
    scraping C++ declarations proved unfixable; declarations live in the IDL,
    bodies in `lang.c`. All the robustness properties (comment immunity,
    `#define` immunity, order independence) are guaranteed. A legacy marker
    errors with a migration hint. IDL structs are value-initialized trivial
    types / 宣言は IDL、本体は lang.c。旧マーカーは移行エラー。IDL 構造体は値初期化
    される自明型。
12. **`export`/`import` at the top level of scripts works**, and user code runs
    **after** WASM initialization completes (no more `native.*` undefined race) /
    スクリプトのトップレベル `export`/`import` が使え、ユーザーコードは WASM 初期化
    完了後に実行されます。

## Scope of "no errors" / 「エラーが出ない」の範囲

- The tool itself is strictly compiled (`-Werror`, zero warnings),
  bounds-checks everything it parses and executes no shell. Normal and failure
  paths are pinned by the test suite / 本ツール自体は厳格コンパイル(-Werror・警告
  ゼロ)・境界チェック・シェル非使用で、正常系/異常系をテストで固定しています。
- **We cannot promise "never fails" for arbitrary user input, arbitrary compiler
  versions or arbitrary browsers.** Input violating the spec stops with
  diagnostics in the manual's error-reference style and leaves the cause in
  `.bc_build/*.log` / 任意の入力・コンパイラ・ブラウザで「絶対に失敗しない」保証は
  できません。仕様に反する入力はリファレンス準拠の診断で停止し、ログに原因を残します。
- Concurrent builds of the same project are rejected with a clean error
  (per-project `flock`); serial builds and a single dev server are the supported
  modes / 同一プロジェクトへの同時ビルドは flock で明確に拒否。逐次ビルドと
  dev サーバ単体が対象です。

## Tests / テスト

```sh
tests/install_toolchains.sh   # first time / 初回のみ: toolchains + Playwright → ~/.cache/jelll-tools
cd tests && python3 -m pytest -q
```

- Real-browser (Playwright/Chromium) E2E for C/Rust/Zig interop, std usage,
  lifecycle, bigint, mixed block ordering, comment/string immunity /
  ブラウザ実機で C/Rust/Zig 連携・std 使用・ライフサイクル・bigint・出現順混合・
  コメント/文字列耐性を検証
- Runtime robustness: wild/cross-type/double-free rejection (`test_wildptr_guard`),
  generation-churn ABA schedule (1,048,574 cycles), exclusive double-free
  (`test_r02_double_free_exclusive`), 4-thread 20k alloc/free race
  (`test_threads_race`, real `--threads`) / 野良ポインタ・世代枯渇 ABA・二重 free
  排他・4 スレッド競合を固定
- All error paths (message-level assertions per the manual's error reference) /
  エラーパス全種をメッセージ一致で固定
- Dev server: COOP/COEP, auto-rebuild, live reload, stalled-client concurrency
  (<1.5s), truncated requests → 400, split requests reassembled, `Host`
  rejection → 421 / 開発サーバの COOP/COEP・自動リビルド・停滞クライアント耐性・
  分割リクエスト・Host 拒否を固定
- `jelll --setup` driven through a pseudo-TTY (arrows, space, Enter) in dry-run /
  `jelll --setup` は疑似 TTY で UI 操作を確認(dry-run)
