# かりポム（KariPom）

![ピンクとティファニーブルーのかりポム2体（作者製作機）](docs/manual/images/karipom_cover.png)

**かりポム（KariPom）** は、M5Stack CoreS3を頭脳にした、しゃべって、動いて、耳をすませる小さなデスクトップ・アニマトロニクスロボットです。2026年夏に作者が個人で設計・製作した作品で、このリポジトリはそのコード・3Dデータ・マニュアル一式を「ひとつの完成した作品」として記録・共有するために公開しています。

## このリポジトリについて

本プロジェクトは、個人製作作品の記録・共有を目的として公開しています。商品化・販売・工作キット化を目的としたものではありません。製作にはArduino、電子工作、3Dプリント等の基礎知識が必要です。作者による個別の製作サポートは行っていませんが、公開している2冊のマニュアルとソースコードは、AI（ChatGPT・Claudeなど）に読み込ませて相談しながら進める使い方を想定して書かれています。セットアップ系の相談をする際は、動かして終わりにせず、次回からはダブルクリックだけで起動できる状態まで整えてもらうのがおすすめです（詳しくは各マニュアルを参照）。

## 主な機能

表情表示と音声再生、サーボによる首の上下・左右動作、PC音声とのリアルタイム口パク連動（BBX）、Visualizer（8種）・Lighting（16種）による画面演出、Webブラウザから操作できるWeb UI（KariPom Lab）、ジョイスティック操作、独り言機能、睡眠・起床モード、SDカードによるログ・音声ファイル管理など。拡張ハードウェア「Karipom Ear」（PCM1808＋Raspberry Pi Pico 2 H）を追加すると、PCなしでライン入力の音に反応するBBXを楽しめます。

## リポジトリ構成

| フォルダ | 内容 |
|---|---|
| `cores3-main/` | かりポム本体（M5Stack CoreS3）のArduinoファームウェア |
| `pico2-pcm1808/` | Karipom Ear拡張用のRaspberry Pi Pico 2ファームウェア。**完成版は `pico2-pcm1808/karipom_pico2/karipom_pico2.ino` です**（スケッチ内部ヘッダの「Step2」は開発当時の名称です） |
| `mac-companion/` | PC側アプリ「KariPom Companion」（Python／Windows・macOS・Linux対応） |
| `3d_models/` | 筐体の3Dプリントデータ（`karipom.3mf` ＋ `stl/` にSTL 9点） |
| `sdcard/` | microSDカードにコピーして使う公開用データ（`faces/` の表情PNG、`sounds/` の音声配置ガイド） |
| `docs/manual/` | かりポム ユーザーマニュアル |
| `docs/pcm1808_pico2/` | Karipom Ear 設計・製作マニュアル（正式版） |
| `docs/karipom_ear/` | Karipom Ear開発時の仕様書・確認手順書などの開発記録 |

## ドキュメント

まずは **[かりポム ユーザーマニュアル](docs/manual/karipom_manual.md)**（[docx版](docs/manual/karipom_manual.docx)）をご覧ください。何ができるか・どう遊ぶか・どこから始めるかを、初めての方向けにまとめています。

電子工作を伴うKaripom Ear拡張（PCM1808＋Pico2）、LINE OUT増設、Bluetooth入力（MH-M18）については、**[Karipom Ear 設計・製作マニュアル](docs/pcm1808_pico2/PCM1808_Pico2_設計マニュアル.md)**（[docx版](docs/pcm1808_pico2/PCM1808_Pico2_設計マニュアル.docx)）が正式資料です。`docs/karipom_ear/` にある各種仕様書・手順書は開発過程の記録であり、内容が正式マニュアルと重複・相違する場合は正式マニュアルを優先してください。

なお、LINE OUT増設は作者の実機で製作・動作確認済みです。Bluetooth入力（MH-M18）は設計・手順書まで完成していますが、作者の実機にはまだ搭載していないオプションです。

## 3Dモデル

`3d_models/karipom.3mf`（全パーツ一体のプロジェクトファイル）と、`3d_models/stl/` の9パーツ（body／chest／hip／pelvis／spinal_cord／left_shoulder／right_shoulder／ears／feet）を公開しています。色は自由に選べます。部品構成の詳細はユーザーマニュアル第5章をご覧ください。

Karipom Ear（PCM1808＋Pico2）を収納する筐体は`stl/`内の`karipom_hip.stl`です。LINE OUTを増設する場合向けに、3.5mmジャック用の開口を追加したオプション版 `karipom_hip_lineout.stl` も公開しています（増設しない場合は`karipom_hip.stl`のままで構いません）。詳しくはKaripom Ear設計・製作マニュアル第11章をご覧ください。

## SDカード用データ

かりポムの動作にはmicroSDカードが必要です（詳しくはユーザーマニュアル第17章「SDカード管理」をご覧ください）。本リポジトリの `sdcard/` フォルダには、そのmicroSDカードで使う公開用の付属データを収録しています。

利用する際は、`sdcard` フォルダそのものではなく、フォルダの**中身**（`faces/`・`sounds/`）を、フォルダ構成を保ったままmicroSDカードのルートへコピーしてください。

- `faces/` — Face Gallery・ランダムFace（睡眠時・独り言時）などで使える公開用の表情PNG13点
- `sounds/` — 音声ファイルの配置方法を説明する `README.md`（WAVファイル自体は収録していません）

音声合成サービスや音声素材ごとのライセンス・再配布条件を本リポジトリへ持ち込まないため、WAV音声ファイル（固定音声・独り言用とも）は同梱していません。音声を使いたい場合は、お好みの音声合成ソフト等でWAVファイルをご自身でご用意ください。固定音声として必要なファイル名や、独り言音声の配置場所については [`sdcard/sounds/README.md`](sdcard/sounds/README.md) をご覧ください。音声ファイルが無くても本体のプログラムは停止せず動作を継続し、該当する音声再生のみ行われません。

## 姉妹プロジェクト：KariPom Desktop

CoreS3の実機がなくても、PCだけでかりポムの口パク体験を楽しめるスタンドアロン版を、別リポジトリで公開しています。

**KariPom Desktop** — https://github.com/kariagepompadour/KariPom-Desktop

## ライセンス

本リポジトリは非商用利用を前提に公開しています。詳細は [LICENSE.md](LICENSE.md) をご覧ください。

| 対象 | ライセンス |
|---|---|
| ソースコード（Arduinoファームウェア・Pythonアプリ） | 独自の非商用ライセンス（個人製作・改変・非商用での公開共有は可、商用利用は不可） |
| 3Dモデル・マニュアル・画像等 | [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/deed.ja)（表示・非営利） |
| 「KariPom」「かりポム」等の名称・ロゴ | 上記ライセンスの対象外（商標的・ブランド的な使用は許可していません） |

---

Copyright (c) 2026 Kariage POMPADOUR Entertainment Corporation
