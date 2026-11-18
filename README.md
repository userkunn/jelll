# JELLL V5 — Linux 版リライト

> **V5.3（第3次最終検証監査・全項目合格）**: R-05 — generation は加算のみでリセット/巻き戻しを
> 完全廃止(上限到達スロットは永久退役、後継は virgin スロットのみ、枯渇時は alloc=0)。
> R-02 — `free` は mutex 下の LIVE→DRAINING 一回きりの状態遷移で解放権を排他取得(2番目以降は
> 即 return)、破棄直前に index+generation を再照合。R-06 — `/proc` による fd 実体検証が不能な
> 場合はフォールバックせず fail-closed で拒絶。
>
> **V5.2（第2次レッドチーム監査 R-01〜R-10 対応）**: IDL のポインタは「定義済み struct の
> 単一ポインタ (Foo*)」のみ許可（int*/void*/多重ポインタは構文エラー）。ポインタ戻り値は
> ハンドルに変換されて返る。ハンドルは 20bit 世代 × 12bit スロット（4096 スロット・世代
> 枯渇スロットは安全に退役、リークなし）。`--threads` のメソッド実行中はピン留めで
> 並行 free から保護。`call` の TOCTOU を fd 検証で封鎖、ブロック走査は線形時間。
> dev サーバは分割リクエストを正しく再構成。llvm-nm 失敗はビルド失敗。セットアップは
> 全成果物の digest/commit をバイナリ固定値で検証。
>
> **V5.1（レッドチーム監査対応）**: 監査 C-01〜C-08 / M-01〜M-06 / S-01〜S-08 / L-01〜L-06 を
> 修正済み。主な挙動変化 — (1) JS へのポインタ露出を廃止しハンドル API（`T_alloc_h` /
> `T_free_h` / `T_get_f_h` / `T_set_f_h` / `S_m_h` + `T_live_count` / `T_slot_count`）に移行。
> 生ポインタ API（`T_alloc` 等）は lang.c 内部専用。(2) `--export-all` 廃止 → コンパイル済み
> オブジェクトから llvm-nm で列挙した C リンケージ関数 + ハンドル API のみ
> `-sEXPORTED_FUNCTIONS` で明示公開。(3) `call` は include-once（同じファイルは1回だけ展開、
> 再帰深度 64・総展開数上限付き）でプロジェクトルート外への参照を拒否。(4) lang.sync は
> `struct X { ... };` の末尾 `;` を許可。(5) dev サーバは Host ヘッダを検証（非ループバックは
> 421）。(6) `--threads` は lang.c + lang.sync のみ（Rust/Zig は共有メモリー非対応）。

C/C++・Rust・Zig と JS/TS/TSX を1つの `.jelll` ファイルにまとめ、
コマンド一発で WebAssembly 製 Web アプリを生成するビルドツール（Linux 実装）。

元 Windows 実装 (`jelll.cpp`, 1,351行) の欠陥を除去し、POSIX/Linux 用に
ゼロ警告・堅牢なエラー処理で書き直したものです。

---

## 必要環境

| ツール | 用途 | 備考 |
|---------------------|----------------------------|------------------------------------|
| GCC/G++ (C++17) | jelll 本体のビルド | `make` |
| Node.js + npm | esbuild / Zig インデックス解決 | https://nodejs.org/ |
| Emscripten (emcc) | C/C++ → WASM | `jelll --setup` で自動導入 |
| esbuild | JS/TS バンドル | `jelll --setup` で自動導入 |
| Rust (rustc + wasm32) | Rust → WASM（任意） | `jelll --setup` |
| Zig | Zig → WASM（任意） | `jelll --setup` |

## ビルドとインストール

```sh
make            # ./jelll が生成される（-Wall -Wextra -Wpedantic -Werror・警告ゼロ）
sudo make install   # /usr/local/bin/jelll へ
```

## ツールチェーンセットアップ

```sh
jelll --setup    # または jelll setup
```

矢印キーで移動、スペースで選択/解除、Enter で確定するマルチセレクト UI。
選択したコンパイラが自動でダウンロード・インストール・PATH 登録されます
（インストール先: `~/.local/share/jelll/tools`、`JELLL_TOOLCHAIN_DIR` で変更可）。
`JELLL_SETUP_DRY_RUN=1` でコマンド表示のみ（テスト用）。

ツール探索は環境変数なしでも動きます: `JELLL_TOOLCHAIN_DIR` →
`~/.local/share/jelll/tools`（`--setup` の既定）→ `~/.cache/jelll-tools`
（`tests/install_toolchains.sh` の既定）の順で、**存在するディレクトリ**を
自動採用します。テストハーネス以外に環境変数の export は不要です。

## 使い方

```sh
jelll app.jelll            # ビルド → dist/ に出力
jelll dev app.jelll        # 開発サーバー http://127.0.0.1:8000（保存で自動リビルド・ライブリロード）
jelll --watch app.jelll    # jelll dev のエイリアス
jelll --threads app.jelll  # 共有メモリー+スレッド版（std::thread 等。配信に COOP/COEP 要）
jelll --version / --help
```

対応拡張子: `.jelll`（推奨）/ `.jsbc` / `.tsbc` / `.tsxbc`（レガシー、同一処理のエイリアス）。

## 入力ファイル形式

`lang.X { ... }` ブロックを記述します（詳細は同梱の JELLL V5 技術マニュアル参照）。
同じ言語の複数ブロックは出現順に連結、JS/TS/TSX のコンパイルモードは
TSX > TS > JS で自動判定。コメント・文字列リテラル内のブロックや `call` は無視。
`call "relative/path"` は再帰展開し、循環参照は検出します。
ネイティブブロック（c/rs/zig）とスクリプトブロック（js/ts/tsx）が各1つ以上必要です。
構造体/メソッド宣言は `lang.sync`（IDL）ブロックに記述します（`lang.c` には本体のみ）。
`lang.sync` はネイティブ 1 つ以上ルールの対象外ですが、`lang.sync` だけではビルドできません。

## 出力構成

```
project/
├── app.jelll
├── .bc_build/               # 中間ファイル
│   ├── core.cpp / core.rs / core.zig / core_rs.o / libcore_rs.a ...
│   ├── entry.js|.ts|.tsx    # グルーコード + sync クラス + スクリプト（出現順）
│   ├── emcc.stderr.log / rustc.stderr.log / zig.stderr.log / esbuild.stderr.log
└── dist/                    # このディレクトリを配信
    ├── native.wasm          # ネイティブコード → WASM（16MiB 共有メモリー固定）
    ├── native.mjs           # Emscripten ES モジュールグルー（★仕様書の3ファイルに無い追加分）
    ├── bundle.js            # バンドル済み JS/TS (ESM)
    └── index.html           # CSS/HTML 注入済み
```

## WASM ランタイム API

| グローバル | 型 | 説明 |
|----------------|--------------------|---------------------------------------|
| `native` | Object | エクスポートされた関数がプロパティとして利用可能（`native.add(2,2)`）。ポインタを取る/返す生 API は非公開で、ハンドル版（`T_alloc_h` 等）のみ |
| `sharedBuffer` | ArrayBufferLike | ヒープ全体のビュー（`new Uint8Array(sharedBuffer)`） |
| `sharedMemory` | WebAssembly.Memory | ヒープ本体（自動成長: 初期32MB → 最大2GB） |

**メモリー上限はありません**（旧仕様の固定16MiBは撤去）。ヒープは必要に応じて自動成長します。
成長後は `sharedBuffer` が差し替わるため、グローバル経由（getter）で参照し直してください。
`sharedBuffer` は常に `sharedMemory.buffer` の同一オブジェクトです — JS 側の書き込みが
そのままネイティブから見えます。なお `sharedBuffer` という名前は互換のため残していますが、
通常ビルドでは一般の `ArrayBuffer` です（`--threads` 時のみ `SharedArrayBuffer`）。

## lang.sync（IDL ブロック・V5）

構造体とメソッドの宣言は **`lang.sync` ブロック（IDL）** に書き、C++ からは切り離します。
C++ 側の構造体スクレイピング（`// @jelll-sync`）は **V5 で廃止** されました。

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

**構文**: `struct <名前> { <型> <フィールド名>; ... <メソッド名>(<型> <引数名>, ...) -> <戻り値型>; ... }`
宣言は `;` 区切り 1 行 1 宣言。構造体内のコメントは無視されます。

**型テーブル（IDL で使えるのは以下のみ）**

| IDL | C++ 生成型 | TS | 備考 |
|---|---|---|---|
| `int` / `int32` | `int` | number | |
| `uint` / `uint32` | `unsigned int` | number | |
| `int64` | `long long` | bigint | getter/setter は符号付き64ビット |
| `uint64` | `unsigned long long` | bigint | |
| `float` | `float` | number | |
| `double` | `double` | number | |
| `bool` | `bool` | boolean | 境界は `unsigned char` |
| `<型>*` | `<型>*` | number (ハンドル) | getter は「ハンドル」を返し、setter はハンドルを受け取る（ポインタ素値は JS に露出しない）。基底型はテーブル型・`void`・`lang.sync` の構造体名 |
| `void` | `void` | void | 戻り値専用 |

- `long long` 等の**多語型名や `const` 修飾子は IDL では使いません**（`int64` 等のテーブル綴りを使用）。
  テーブル外の型は即 `Unsupported type` エラーです（`string` 等も同様）。
- **メソッド本体は lang.c に自由に書けます**。生成される `extern "C"` プロトタイプが
  C リンケージを自動的に与えるので、本体側に `extern "C"` は**あってもなくても**動きます。
  マクロ・`inline`・`__attribute__`・コメントの書式は lang.c 内では完全自由です
  （IDL はユーザーコードとは独立に解析されるため、`#define` やコメントの影響を受けません）。
- **構造体の定義（`struct Circle { ... };`）を lang.c に再定義しないでください**（エラー）。
  フィールド宣言は生成コードが担います。
- **メソッド本体が無いとリンクエラー**になります（生成されるリンク参照 `&X_method` が
  未定義シンボルを検出）。実行時に壊れず、ビルドで止まります。
- **IDL 構造体は自明型（trivial）** です: コンストラクタ/デストラクタは IDL に存在せず、
  `X_alloc()` は配置 new による**値初期化（全フィールド 0）**、`X_free()` は単純な
  スロット返却（`free()` のダブルフリー/誤ポインタ検出は維持）。再利用セルも毎回 0 に
  初期化されます。ポインタフィールドは 0（null）初期化です。
- **予約名の拒否**: 構造体/フィールド/メソッド/引数名に `constructor` / `free` /
  `fromPtr` / `liveCount` / `slotCount` / `_ptr` は使用できません（TS クラスと衝突）。
  フィールドと同名のメソッド、生成名（`X_alloc` / `X_free` / `X_live_count` /
  `X_slot_count` / `X_get_*` / `X_set_*`）と衝突するメソッドも拒否します。
- **レガシー `// @jelll-sync` マーカーが lang.c に残っていると即エラー**になり、
  `lang.sync { ... }` への移行を指示します。
- `Circle.liveCount()` / `Circle.slotCount()` で使用中/確保済みスロット数を取得できます。

### 生成アロケータの設計（レビュー指摘への対応を含む）

- **O(1) alloc/free**: 各スラブ（256セル）がフリーリストを持ち、確保は先頭ポップ、
  解放はプッシュのみ。全スラブ線形走査はありません。
- **値初期化ライフサイクル**: `X_alloc` は配置 new（`::new (&cell->item) X()`）で
  全フィールドをゼロ初期化し、`X_free` でスロットを返却します（IDL 構造体は自明型）。
  テスト `test_sync_lifecycle` で固定。
- **スレッド安全性**: `--threads` ビルドでは生成コードの管理テーブル操作を
  `std::mutex` で保護し、ユーザーコードの `std::thread` から直接 `X_alloc()` /
  `X_free()` を呼んでも安全です（`test_threads_race` で固定）。非スレッドビルドでは
  ロックは一切生成されません（オーバーヘッドゼロ）。`--threads` はコンパイル時に
  `-pthread -DJELLL_THREADS=1` を付けるので、ユーザーコードも `#if JELLL_THREADS`
  で区別できます。
- **呼び出し順序の自由**: プロトタイプ宣言がユーザー本体より前に出るため、
  ユーザーコード（グローバル ctor や `std::thread` ラムダ内を含む）から生成 API を
  直接呼べます。
- **野良ポインタは「ガードが先に死ぬ」問題への対処**: `X_free` はデリファレンス**前**に
  算術検証のみで判定します（`__builtin_wasm_memory_size` によるヒープ範囲チェック +
  アラインメント）。範囲外アドレスは wasm トラップを起こす前に拒否されるため、
  JS から `X_free(0xdeadbeef)` のようなゴミポインタを渡してもモジュールは死にません。
  続いてオーナースラブの範囲・magic・**型タグ**（別型ポインタの混用を拒否）・
  インデックス範囲・ポイスンを検証。getter/setter も同じ範囲チェックを行い、
  不正ポインタには 0/false を返します（テスト `test_wildptr_guard` で固定）。
- **二重解放/誤ポインタ検出**: 使用中セルの `next_free` にポイスン値（0x4a454c4c）を
  立て、`X_free` で検証チェーンを通らない解放は無視します。「解放後に割り当て直された
  セルへの遅延ポインタ」の再利用検出は不可能（malloc 系と同じ）で、それは自明な
  エイリアスとして振る舞います。
- **メモリ方針（ハイウォーター + 空スラブ解放）**: 空になったスラブは**1枚だけ暖気キャッシュ**
  し、2枚目以降は解放して返却します。使用中スロットはコンパクトなセルに詰めるため、
  `free()` 済みスロットのダングリング参照も型上アクセス不能になります。
- **メソッドのリンケージ自動調整**: `lang.sync` の宣言から `extern "C"` プロトタイプと
  リンク参照を生成するため、メソッド本体は `extern "C"` なしでもマングルされず、
  JS から `native.Circle_area(...)` で直接呼べます。**メソッド以外の通常関数**を
  `native.X` で呼ぶには従来通り `extern "C"` で囲んでください。ヘルパー関数・
  グローバル変数・`namespace` など lang.c は完全な C++ として自由に書けます。

## 開発モード

`jelll dev` は初回ビルド後に 127.0.0.1:8000 で配信、500ms ポーリングで変更検知、
保存で自動リビルド、ライブリロードします。通常ビルドは特殊ヘッダー不要で、
どの静的ホスティングでもそのまま動きます。`--threads` ビルドの配信時のみ
COOP/COEP ヘッダーを付与します（SharedArrayBuffer のブラウザ要件）。
低速クライアント対策として、ヘッダー受信は2秒・送信は5秒で打ち切り、
ヘッダー未完の接続は 400 で即クローズします（Slowloris 耐性。テストで固定）。
ハンドラは**上限8スレッドのプール**で並行処理され、巨大な `native.wasm` の送信中も
ライブリロードポーリングがブロックされません（キューイングはカーネルの backlog に委譲）。
パスはスラッシュ正規化 + `weakly_canonical` による dist ディレクトリ内包検証を行い、
`//etc/passwd` や `..` を含む URL でホストのファイルを読み出せません（LFI 対策）。
組み込み用途向けに `stop_dev_server()` で受信スレッドを安全に停止できます。

---

## 仕様書からの意図的な差分・解釈（重要）

技術マニュアルは**参考扱い**とし、矛盾・欠落は「ちゃんと動く」を優先して解決しています。

1. **`lang.c` は C++ としてコンパイルする**
   マニュアルは「C/C++」としながら、サンプルはすべて `extern "C"` 付き C++。
   中間ファイルも `core.cpp` と明記。よって Emscripten の C++ 入力として扱います。
   純粋な C（C++として非合法な構文）はコンパイルエラーになります。
2. **固定16MiBメモリーを撤去**（ユーザー指示による）
   ヒープは自動成長（初期32MB → 最大2GB）。「16MBを超えたら死ぬ」制約は存在しません。
   `sharedBuffer`/`sharedMemory` は実ヒープの公開で、別メモリーを持つ旧実装の不具合は修正済み。
3. **`dist/native.mjs` の追加**
   Emscripten の実行には生成 JS グルーが必須です。マニュアルの3ファイル構成には
   この実態が反映されていないため、4ファイル構成を正式とします。
4. **共有メモリーは通常ビルドでは使わない**
   SharedArrayBuffer には COOP/COEP が必要で、GitHub Pages 等の静的ホスティングで
   そのまま動かせないため。スレッドが必要な場合のみ `--threads` で共有メモリー化し、
   開発サーバーはその時だけ COOP/COEP を付与します。
5. **エラーメッセージ「No lang.c section found」の適用範囲**
   V4 ルール（C/Rust/Zig いずれでも可）を優先し、ネイティブブロックが**皆無のときのみ**この
   エラーを出します（hint に lang.rs/lang.zig でも可と表示）。
6. **スクリプトブロックは全言語を出現順に連結**（旧実装は言語別にグルーイングし順序が壊れていた）
   ブロック走査のコメント/文字列マスクは言語別で、JS/TS/C/CSS のシングルクォート文字列
   （`'}'` を含む）は正しく文字列扱いします（Rust のライフタイム `'a` のみ例外処理）。
   JS/TS の正規表現リテラルはキーワード文脈（`return /}/;` 等）も正しく判定し、
   内部の波括弧がブロック抽出を壊しません。
7. **Rust は rustc 直コンパイル（外部クレート非対応）**
   `--crate-type=staticlib` + オブジェクト併用で `#[no_mangle]` エクスポートと std の遅延リンクを両立。
   std は単スレッド前提で利用可能（スレッド生成を伴う std 機能は対象外）。
   `extern "C"` 以外の Rust 関数はマングル名でエクスポートされますが API ではありません。
   ※ `.o` と `.a` の二重リンクについて: リンカー（wasm-ld）はライブラリを**遅延抽出**し、
   `core_rs.o` が直接定義済みのシンボルを持つメンバーは `.a` から展開されないため
   重複定義にはなりません（`.a` 内にクレートオブジェクトのコピーが存在するのは事実ですが、
   展開されない以上衝突しません）。実測でも本構成のリンクは再現可能（同一サイズ）で、
   `core::fmt::Write::write_str` 等の std を使うテストが重複なしで通っています。
   代替案の「`.a` のみ + `--whole-archive`」は重複を生まないものの、実測で
   **バイナリ +39%・不要エクスポート +759 個**（無名シンボルの JS 警告付き）でした。
   「`.o` のみ」は std を含められないため不可能です。なお `.o` + `.a` に
   `--whole-archive` を付けると確かに `duplicate symbol` で落ちますが、それは本構成の
   フラグではありません。リンカ仕様はピン留めしたツールチェーン＋テストスイートで
   固定しており、将来の変更はリンクエラーという**ビルド時・大声の**失敗として現れる
   （実行時に壊れない）ため、地雷ではなく管理された依存です。
8. **Zig は `wasm32-freestanding`**（std は freestanding で動くサブセットのみ）
9. **レガシー拡張子 `.jsbc/.tsbc/.tsxbc` は `.jelll` のエイリアス**（モード自動判定も同一）
10. **セキュリティ修正**: コマンド実行にシェルを使用しない（旧実装のコマンドインジェクション／
    パス空白での破壊を修正）。出力はアトミック書き込み。
11. **`lang.sync`（IDL）導入・構造体スクレイピング廃止（V5）**
    `// @jelll-sync` を付けた C++ 構造体宣言のスクレイピングは精度限界（走査ヒューリスティックの
    破破綻）と観測されたため廃止し、宣言を IDL ブロックに分離しました。C++ 側には
    メソッド本体のみを記述します。IDL プロパティ（コメント耐性・順序非依存・
    `#define` 耐性・struct リテラル/式/修飾子の無効化）はすべて保証されます。
    旧マーカーには移行エラー（error041）を返します。IDL 構造体は自明型として
    値初期化されます（ユーザー ctor/dtor の記述はできません）。
12. **`export` を含むスクリプトブロック・`import` はトップレベルで利用可能**
    （ユーザーコードはモジュールトップレベルで、初期化完了**後**に実行されます。
    旧実装は WASM 初期化前に実行され `native.*` が未定義になるレースがありました）

## 「エラーが出ない」の範囲

- 本ツール自体は厳格コンパイル（`-Werror`）・境界チェック・シェル非使用で、正常系/異常系を
  テストで固定しています（下記）。
- 一方、**任意のユーザー入力・任意のコンパイラバージョン・任意のブラウザ環境で
  「絶対に失敗しない」ことを保証することはできません**。入力が仕様に反する場合は
  マニュアルのエラーリファレンスに準拠した診断で停止し、`.bc_build/*.log` に原因を残します。
- 同一プロジェクトへの**複数プロセスの同時ビルドは未対応**（`.bc_build` を共有するため）。
  dev サーバー単体の利用・逐次ビルドが対象です。

## テスト

```sh
tests/install_toolchains.sh   # 初回のみ: Emscripten/Rust/Zig/esbuild/Playwright を ~/.cache/jelll-tools へ
cd tests && python3 -m pytest -q
```

- ブラウザ実機（Playwright/Chromium）で C/Rust/Zig 連携・std 使用・@jelll-sync
  （プール枯渇・use-after-free 検出・bigint）・出現順混合・コメント/文字列内ブロック無視を検証
- 生成 API の堅牢性: 野良ポインタ/型混用/二重解放の拒否（`test_wildptr_guard`）、
  ctor/dtor ライフサイクル（`test_sync_lifecycle`）、4スレッド競合 2万 alloc/free
  （`test_threads_race`、`--threads` 実機）
- エラーパス全11種（エラーリファレンス準拠メッセージの一致確認）
- 開発サーバーの COOP/COEP・自動リビルド・ライブリロード検知・
  停滞クライアント並行耐性（1.5秒以内応答）と切断リクエストへの 400
- `jelll --setup` は疑似 TTY で UI 操作（矢印・スペース・Enter）を確認（dry-run）
