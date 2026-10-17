# 言語

エディターは英語で書かれており、英語、日本語 (`ja`)、簡体字中国語 (`zh`)、韓国語 (`ko`) で表示できます。言語は 編集 > エディター設定 > 言語、またはプロジェクト一覧で選びます。選択はすぐに反映され、次のセッションにも引き継がれます。

英語以外の各言語は、英語のテキストから訳文への対応表で、言語ごとに 1 つの JSON ファイルです。

- エディターの表: `resources/lang/<code>.json` (例: `resources/lang/ja.json`) と、`resources/lang/<code>/` 内のファイル (エディターの日本語は領域ごとに分かれています: `ja/app.json`、`ja/views.json`、...)
- パッケージの表: パッケージ内の `editor/lang/<code>.json`

```json
{
    "Save": "保存",
    "%d objects": "%d 個のオブジェクト"
}
```

アイコンで始まるラベルは、アイコンを除いた形で検索されます。`"New Project"` の項目は `ICON_TI_PLUS "  New Project"` にも適用されます。

項目のないテキストは英語で表示されるので、表は少しずつ埋めていけます。次の翻訳者へのメモとして、コメント (`//`) を書けます。エディターとパッケージが同じテキストを翻訳している場合は、エディターの訳が使われます。

## 表を翻訳する

足りないものは次のコマンドで探します。

```
python tools/ci/check_translations.py --missing
python tools/ci/check_translations.py --missing --package ../packages/deki-2d
```

コードが表示するテキストのうち表にまだないものと、テキストがもうコードにない項目をすべて一覧にします。

実行中のエディターで翻訳されずに表示されているものを確認するには、`DEKI_LOC_MISSING=<file>` を指定して起動します。訳なしで描画された各テキストが、そのファイルに 1 回ずつ書き出されます。

訳文中の `%d`、`%s`、`%.1f` は英語と同じ順序にしてください。値はその順序で埋め込まれます。

## コードでテキストを表示する

英語で書き、描画するときに `Tr` に通します (`<deki-editor/Localization.h>`)。

```cpp
ImGui::TextUnformatted(Tr("No scene open"));
snprintf(buf, sizeof(buf), Tr("%d objects"), count);
```

ImGui のラベルはウィジェットの ID でもあるので、`TrLabel` に通します。`TrLabel` はどの言語でも ID を同じに保ちます。ポップアップのように名前で検索されるものには、両側でこれを使ってください。

```cpp
if (ImGui::Button(TrLabel("Save")))
ImGui::OpenPopup(TrLabel("Add Screen Size"));
BeginSchematicModal(TrLabel("Add Screen Size"), 360.0f);
```

ラベルの表に置くテキストは、書く場所で `TrMark` で印を付け、描画する場所で `Tr` に通します。

```cpp
constexpr const char* kModes[] = { TrMark("Automatic"), TrMark("Off") };
```

共通のヘルパーは渡されたものを翻訳するので、呼び出し側はそのままの英語を渡します。対象は、セクションの見出し (`SchematicSectionBegin`)、プロパティの行 (`BeginPropertyRow`、`EditorUI::PropertyRow`)、行のツールチップ、`Tooltip`、`SchematicCombo` の項目、`SchematicDialogButtons` のボタンです。これらに `Tr(...)` を渡すと二重に翻訳されます。

プロジェクトに由来する名前やパス (オブジェクト、アセット、ファイル) は、決して `Tr` に通しません。

## パッケージ自身の名前

インスペクターに表示されるパッケージのコンポーネントの名前はリフレクションから得られ、エディターが翻訳します。対象は、コンポーネント名 ("Sprite Component")、カテゴリ、説明、プロパティ名 ("tintColor" は "Tint Color" と表示されます)、ツールチップ、グループ、C++ に書かれたままの列挙値 ("TopLeft")、作成メニューの項目、アセットの種類の名前です。これらはパッケージの `editor/lang/<code>.json` に書きます。エディターはプロジェクトを開くときに、プロジェクトのすべてのパッケージの表を読み込みます。`check_translations.py --package` は、パッケージの生成済みリフレクション (`generated/*Reflection*.gen.cpp`、パッケージをビルドすると書き出されます) からそれらを一覧にします。

すべての表は 1 つの共有の検索先にも入り、そこではエディターの表が先に読まれます。エディターがパッケージのコンポーネントやノードを描画している間 (インスペクター、コンポーネントを追加、プロジェクト設定、ノードグラフのウィンドウ) は、そのパッケージ自身の表が先に検索されるので、パッケージは単語を独自に訳せます。deki-fsm の "Add" は算術演算の 加算 ですが、エディターの "Add" ボタンは 追加 のままです。パッケージのクラスを自分で描画するコードも、`Localization::ScopedPackage scope(meta->package);` で同じようにします。`Deki::ComponentMeta::package` と `DekiNodeMeta::package` は、そのクラスが属するパッケージを示します。

## フォント

Noto Sans JP、SC、KR がエディターのすべてのフォントの後ろに統合されているので、日本語、中国語、韓国語のテキストはどの言語でも描画されます。英語のエディターでのファイル名やプロジェクト名も同様です。グリフは最初に表示されたときに描画されます。漢字、CJK の句読点、全角形は 3 つのフォントすべてにあり、字形が異なります。これらは現在の言語のフォントで描画されます (`SetCjkPreference`、それ以外の言語では日本語)。

Tabler のアイコンフォントは、エディターが使うアイコン (私用領域と U+FA17) に限定しています。全角の句読点の位置にもアイコンがあり、そのままでは "（" や "，" を置き換えてしまうためです。
