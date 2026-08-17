#!/bin/bash
# karipom_sck_helper のビルドスクリプト（開発者専用・通常は実行不要）
#
# 一般ユーザーはこのスクリプトを実行する必要はありません。KariPom Companionは
# このスクリプトが生成した「事前ビルド済みバイナリ（karipom_sck_helper）」を
# そのまま起動するだけなので、Xcode・Command Line Tools・Swift compilerは不要です。
#
# 開発者がScreenCaptureKit helperのSwiftソース（karipom_sck_helper.swift）を
# 変更した場合のみ、このスクリプトを実行してバイナリを再生成し、
# 生成された karipom_sck_helper をリポジトリへコミットしてください。
# KariPom-Desktop側にも同じSwiftソースの複製があるため、変更した場合は
# 同じ内容をそちらにも反映し、同様にビルドし直してください。
#
# 要件（ビルドする開発者のMacのみ。実行するだけの一般ユーザーには不要）:
#   - Xcode Command Line Tools（xcode-select --install）
#   - macOS 13 Ventura以降のSDK（Xcode 14.3以降）
#
# 生成物:
#   karipom_sck_helper … arm64 + x86_64 の Universal Binary（lipoで結合）
#
# 将来.app配布版を作る場合は、この karipom_sck_helper をアプリバンドルの
# Contents/Resources/（またはContents/MacOS/）へ同梱し、アプリ本体と一緒に
# コードサイニング・公証（notarization）の対象に含めてください。
#
# 使い方:
#   cd <このスクリプトがあるディレクトリ（mac-companion/resources）>
#   ./build_helper.sh

set -euo pipefail
cd "$(dirname "$0")"

SRC="karipom_sck_helper.swift"
OUT="karipom_sck_helper"
FRAMEWORKS=(-framework Foundation -framework ScreenCaptureKit -framework CoreMedia -framework AudioToolbox)

if ! command -v swiftc >/dev/null 2>&1; then
    echo "エラー: swiftcが見つかりません。Xcode Command Line Toolsをインストールしてください:" >&2
    echo "  xcode-select --install" >&2
    exit 1
fi

echo "[1/3] arm64向けにビルド中..."
swiftc -O -target arm64-apple-macos13.0 "$SRC" "${FRAMEWORKS[@]}" -o "${OUT}_arm64"

echo "[2/3] x86_64向けにビルド中..."
swiftc -O -target x86_64-apple-macos13.0 "$SRC" "${FRAMEWORKS[@]}" -o "${OUT}_x86_64"

echo "[3/3] Universal Binaryへ統合中..."
lipo -create -output "$OUT" "${OUT}_arm64" "${OUT}_x86_64"
rm -f "${OUT}_arm64" "${OUT}_x86_64"
chmod +x "$OUT"

echo ""
echo "--- 生成結果 ---"
file "$OUT"
lipo -info "$OUT"
echo ""
echo "完了しました。'$OUT' を git add してコミットしてください。"
