# 現代検索依存ライブラリ

現代検索基盤は、OSが提供するsystem dependencyと、CMake `FetchContent`でcommitを固定する
source dependencyを分けて管理します。`cmake/Dependencies.cmake`をversion lockの正本とし、
branch名や移動可能なtagだけを指定してはいけません。

## System dependency

| Library | Minimum | 用途 |
|---|---:|---|
| ICU4C | 67 | Unicode正規化、word/sentence/grapheme境界 |
| libcurl | 7.68 | embedding HTTP client |
| libevent | 2.x | HTTP server/event loop |

system dependencyはOSのsecurity updateを受け取るためsourceをrepositoryへ複製しません。

## Commit固定dependency

| Library | Version/commit | License | 用途 |
|---|---|---|---|
| yyjson | 0.12.0 / `8b4a38dc994a110abaec8a400615567bd996105f` | MIT | JSON parse/write |
| tomlc99 | `29076dfd095bbbbd50a3c1b2760d29f4b83e74ac` | MIT | `config.toml` parse |
| USearch | 2.24.0 / `40d127f472e9073875566f0e9308c0302b89100a` | Apache-2.0 | HNSW ANN |

USearchのC++実装は`yappod_usearch` targetだけでcompileし、Yappod2からはC99 APIを使用します。
外部sourceにはYappod2固有のwarnings-as-errorsを適用しません。

## 更新手順

1. upstream release、license、security情報を確認する。
2. `cmake/Dependencies.cmake`のcommit SHAを更新する。
3. clean build directoryからconfigureし、`dependency_smoke`を実行する。
4. 全CTest、macOS/Ubuntu CI、sanitizerを成功させる。
5. versionとcommitの組をこの文書へ反映する。

依存取得済みのbuild treeでは`FETCHCONTENT_UPDATES_DISCONNECTED`により暗黙更新しません。
