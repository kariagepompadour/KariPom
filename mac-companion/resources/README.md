# mac-companion/resources/

KariPom CompanionのmacOś用ScreenCaptureKit音声helperの配布物です。

- `karipom_sck_helper.swift` — helperのSwiftソース（開発者向け。KariPom-Desktop側の同名ファイルと内容は同一に保ってください）
- `karipom_sck_helper` — 上記から事前ビルドした実行ファイル（Universal Binary: arm64 + x86_64）。**このリポジトリに実バイナリとしてコミットされています。**
- `build_helper.sh` — `karipom_sck_helper.swift` から `karipom_sck_helper` を再ビルドするスクリプト（開発者専用。Xcode Command Line Toolsが必要）

## 一般ユーザー向け

何もする必要はありません。`karipom_companion.py` は起動時にこのディレクトリの
`karipom_sck_helper` を自動的に見つけて実行します。Xcode・Command Line Tools・
Swift compilerのインストールは不要です。

## 開発者向け（Swiftソースを変更した場合のみ）

```
cd mac-companion/resources
./build_helper.sh
git add karipom_sck_helper karipom_sck_helper.swift
```

KariPom-Desktopリポジトリ側にも同じhelperの複製（`resources/`）があります。
Swiftソースを変更した場合は、両リポジトリで同じ内容に更新し、それぞれで
`build_helper.sh` を実行してバイナリを再生成してください（リポジトリが
別れているため、バイナリそのものは共有できません）。
