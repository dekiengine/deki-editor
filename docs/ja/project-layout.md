# プロジェクトの構成

Deki のプロジェクトがディスク上でどのような構成になっているか、エディターが何を生成するか、生成された状態がおかしくなったときにどう復旧するかを説明します。

## 構成

```
<project>/
├── deki.json                    project identity and entry points
├── deki-packages.json           declared package dependencies
├── deki-packages-lock.json      resolved versions
│
├── src/                         gameplay code
├── assets/                      sprites, fonts, scenes, data, rendering
├── packages/                    installed package sources
├── platforms/                   build targets
│
├── settings/                    project settings
│
└── generated/                   everything the editor generates
    ├── runtime/                 package DLLs the editor loads
    ├── build/                   CMake/Ninja tree
    ├── cache/                   asset cache
    ├── shadow/                  hot-reload copies of runtime/
    └── CMakeLists.txt           regenerated on open/build
```

`platforms/` には、パッケージからコピーされたものも含め、ビルドターゲットごとに 1 つのディレクトリがあります。[platforms.md](platforms.md) を参照してください。

`generated/` 以下はすべて使い捨てです。そのため、プロジェクトの `.gitignore` は一覧ではなく 1 行です。一覧は実態とずれていくもので、実際にずれました。何も作成しない `build_refl/` が載っている一方で、追跡対象だった `settings/` フォルダーを隠していました。

## 移植性

プロジェクトのディレクトリは別のマシンにコピーして、そのまま開けます。プロジェクト内のどこにも絶対パスは記録されていません。

- パスは実行時にプロジェクトのルートから導出されます。構成の唯一の定義である `src/core/ProjectPaths.h` を参照してください。
- 生成される `CMakeLists.txt` は、`DEKI_PROJECT_ROOT` を自身の場所から導出し、エンジンとエディターの場所は configure 時にエディターから受け取ります (`-DDEKI_ENGINE_PATH` / `-DDEKI_EDITOR_DIR`、`EditorSettings::GetCMakeLocationArgs` が組み立てます)。以前はこれらを埋め込んでいたため、エディターのインストール先が移動した途端に壊れていました。
- `src/generated/` 以下にコミットされるリフレクションの出力は、相対的なソースパスを使います。

**エディター**を移動しても同じく安全です。エンジンのパスは呼び出しのたびに実行中の実行ファイルから解決され (`EditorSettings::GetEnginePath`)、保存されることはありません。ダウンロードしたエディターは、隣にある `sdk/` フォルダーからそれらを見つけます (`EditorSettings::GetSdkDir`)。ビルドツリーから実行したエディターは、ビルドフォルダーを囲むソースのチェックアウトからそれらを見つけます。

## 復旧

**編集 → メンテナンス** を使います。操作はコストの低い順に並んでいます。低コストの操作で解決しなかった場合にだけ、次の段階に進んでください。

| 症状 | 操作 | コスト |
|---|---|---|
| パッケージが古い、またはアンインストールしたパッケージがまだ読み込まれる | パッケージのコピーをリセット | 数秒。次の読み込み時に再コピーされます |
| インポート後にアセットの表示がおかしい、または古い | アセットキャッシュをクリア | アセットを再インポート |
| リンカーや CMake の混乱、configure の失敗 | クリーンビルド | 全体を再コンパイル |
| 最初からやり直す | 完全クリーン | 再ビルド + 再インポート |

それぞれが `generated/` 以下の 1 つのディレクトリに対応します。手作業で削除しても同じことになります。メニューがあるのは、構成を覚えておかなくて済むようにするためです。プレイモード中やビルド中は無効になります。マップされた DLL の削除は途中で失敗し、始める前より悪い状態を残すためです。

## shadow/ が runtime/ と同じ階層にある理由

パッケージは `generated/runtime/` から直接ではなく、`generated/shadow/` 内のコピーから読み込まれます。Windows はマップされた DLL をロックするので、コピーを読み込むことで元のファイルを書き込み可能なままにし、エディターが読み込み中のパッケージをビルドし直せるようにしています。

`shadow/` は意図的に `runtime/` の中ではなく隣に置いています。`runtime/` はコピーの*元*であり、コピー先をコピー元の中に入れると、ローダーが自分自身のコピーを列挙してしまうためです。`shadow/owner.pid` は所有するプロセスを記録するので、クラッシュしたインスタンスの残りは手作業で削除しなくても自動的に回収されます。

## 構造のバージョン

`deki.json` には `"structure"` があります。エディターは読み込み時にこれを `ProjectPaths::kStructureVersion` と比較し、構成の移行を 1 回だけ実行してから新しいバージョンを記録します。そのため、移行していないプロジェクトでも常に自身の状態を示せます。各移行がディレクトリの形から状態を推測する必要はありません。
