# M5Stack公式StackChanのサーボ仕様を公式ソースコードから検証する（＋KariPom移植用仕様）

## はじめに

筆者は「かりポム（KariPom）」という、M5Stack CoreS3をベースにした自作の掌サイズアニマトロニクスロボットを開発・運用している。今後、このKariPomの機能をM5Stack公式の「StackChan」へ移植する計画があり、その事前調査として、StackChanの公式ソースコードを実際にclone・読解した記録を残す。

調査の過程で、**StackChan公式リポジトリの内部に、サーボの左右方向に関する矛盾した記述が3箇所存在する**ことが分かった。ドキュメントだけを読んでいては気づけない内容だったため、同じくStackChanの改造を考えている方の参考になればと思い公開する。

### 関連リンク（KariPomについて）

- **KariPom本体**（今回のStackChan移植対象）：[github.com/kariagepompadour/KariPom](https://github.com/kariagepompadour/KariPom)
- KariPom Desktop（KariPomから派生したPC版）：[github.com/kariagepompadour/KariPom-Desktop](https://github.com/kariagepompadour/KariPom-Desktop)
- KariPom Web（KariPomから派生したブラウザ版）：[github.com/kariagepompadour/KariPom-Web](https://github.com/kariagepompadour/KariPom-Web)　／　[公開ページ](https://kariagepompadour.github.io/KariPom-Web/)（インストール不要でブラウザから試せる版）
- GitHub: [@kariagepompadour](https://github.com/kariagepompadour)

本記事は2部構成になっている。

- **第1部**：StackChan公式サーボ仕様の検証結果（StackChanユーザー全般に関係する内容）
- **第2部**：筆者のKariPomをStackChanへ移植する際の設計メモ（KariPom固有の内容）

## 免責事項・商標について

- 「StackChan」「スタックチャン」は石川真也氏が創始したプロジェクトに由来する登録商標であり、本記事はM5Stack社が販売する公式StackChan（`m5stack/StackChan-BSP`、`m5stack/StackChan`）を対象とした非公式の第三者調査である。M5Stack社およびStackChanコミュニティの公式見解ではない。
- 本記事は2026年8月時点で公開されている上記リポジトリのソースコードを直接参照して作成した。将来のアップデートで記述内容が変わる可能性がある。
- 本記事の内容によって生じたいかなる損害についても筆者は責任を負わない。実機での動作確認は自己責任で行ってください。
- **本記事の解説文章（地の文）のライセンス**：[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)（引用・転載時は出典を明記してください）。
- **本記事中に引用しているソースコード自体は、CC BY 4.0の対象ではなく、各原著作物のライセンスがそのまま適用される**。具体的には、`m5stack/StackChan-BSP`および`m5stack/StackChan`からの引用箇所はMITライセンス（Copyright (c) 2026 M5Stack Technology CO LTD）、KariPomのコード引用箇所はKariPom本体リポジトリのライセンスに従う。コードを転載・改変する場合は、本記事のCC BY表記ではなく、それぞれの原著作物のライセンス条項を確認・遵守すること。

---

# 第1部：StackChan公式サーボ仕様の検証

## 参照したソース

| リポジトリ | 内容 |
|---|---|
| [`m5stack/StackChan-BSP`](https://github.com/m5stack/StackChan-BSP) | Arduino開発用BSP。**mainブランチ＝タグ1.1.0**（2026-08-27時点の最新）を基準に確認。タグ1.0.0〜1.0.2も比較確認済み（1.0.2→1.1.0の差分はサーボドライバのライブラリを`SCServo_lib`から`FTServo_Arduino`へ差し替えたのみで、本記事が参照する`servo_init()`の設定値・`motion.h`・`BasicMovement.ino`・Yaw/Pitch角度体系・Motion APIには影響なし） |
| [`m5stack/StackChan`](https://github.com/m5stack/StackChan) | M5Stack公式StackChanファームウェアの公開ソース（ESP-IDFベース、[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)派生）。※このリポジトリ自身のREADMEに「本リポジトリの更新は、実際にリリースされるファームウェア・モバイルアプリより遅れる場合がある」旨の記載があるため、本記事では「現在販売されている全個体に必ずこのコードのバイナリが書き込まれている」という意味ではなく、M5Stackが公開している実装として扱う |

## サーボ通信の基本仕様

```cpp
// src/M5StackChan.cpp（BSP）/ firmware/main/hal/hal_servo.cpp（公式ファームウェア公開ソース）で共通
_scs_bus.begin(UART_NUM_1, 1000000, 6, 7);
```

`SCSerial::begin()`のシグネチャは`(uart_num, baud_rate, tx_pin, rx_pin)`であるため：

- UART1、**1,000,000bps（1Mbps）**、**TX=G6、RX=G7**
- サーボはFeetech系スマートバスサーボ（半二重）。Yaw（左右）＝サーボID 1、Pitch（上下）＝サーボID 2
- 角度単位は**10単位＝1°**（内部はint、0.1°刻み。`set_angle_impl()`の変換式`angle*16/5/10`、コメント「1歩＝0.3125度」で確認）
- Yaw軸のみ360°連続回転に対応（`enablePwmMode=true`）。Pitchは非対応

## 角度範囲：BSPと公式ファームウェア公開ソースで値が違う

これはドキュメントに明記されておらず、実際に両リポジトリを比較して初めて分かったことだが、**BSP（Arduino開発用）とM5Stack公式StackChanファームウェアの公開ソースとで、Pitchの可動域が異なる**。

| | Yaw角度範囲 | Pitch角度範囲 |
|---|---|---|
| StackChan-BSP（Arduino） | −128.0°〜+128.0° | **0.0°〜90.0°**（フル可動） |
| 公式ファームウェア公開ソース（ESP-IDF） | −128.0°〜+128.0° | **3.0°〜87.0°**（安全マージンあり） |

```cpp
// BSP: src/M5StackChan.cpp
pitch_servo_config.angleLimit = uitk_intl::Vector2i(0, 900);

// 公式ファームウェア公開ソース: firmware/main/hal/hal_servo.cpp
pitch_servo_config.angleLimit = Vector2i(30, 870);
```

安全マージンを設けている理由の明記はソース中に見当たらない。この3〜87°が「M5Stack公式の推奨可動域」であると明言した記述もない。あくまで公開ソース上でこの値が採用されているという事実のみが確認できている（より保守的な値を採用していることから、機構保護の意図があると推測している）。**BSPでゼロから開発する場合、安全側に倒すのであれば、Pitchの可動域を公式ファームウェア公開ソースと同じ3〜87°に手動でクランプする、という選択肢がある。**

さらに公式ファームウェア公開ソースには、BSPには存在しない**ストール保護機構**がPitch軸のみに実装されている。

```cpp
// firmware/main/hal/hal_servo.cpp（要約）
// 50msごとに電流・負荷をチェックし、
//   ・目標位置との差が一定以上あるのに実位置がほぼ動かない
//   ・かつ電流 or 負荷が閾値超え、または急上昇
// という状態が2サンプル連続したら、その方向の可動域をその場で動的に狭めて保護する
```

Yaw軸にはこの保護は無効化されている（`enableStallProtection`はPitchのみtrue）。

## 【本題】Yaw軸の正負と左右方向の対応：公式ソース内に3つの矛盾する記述

これが本記事で一番伝えたい内容である。**「サーボ角度を正の値にすると頭が左を向くのか右を向くのか」について、StackChan公式リポジトリ内に相互に矛盾する3つの記述がある。**

### 証拠①：BSP公式サンプルのコメント → 「正＝左」

```cpp
// examples/Servo/BasicMovement/BasicMovement.ino
M5StackChan.Motion.moveX(1000, 200);
M5StackChan.Display().printf("> Turn Left (Slow: 200)\n");

M5StackChan.Motion.moveX(-1000, 200);
M5StackChan.Display().printf("> Turn Right (Slow: 200)\n");
```

### 証拠②：公式ファームウェア公開ソースのユニットテスト → 「正＝左」（ただし性質が①③とは異なる）

```cpp
// firmware/tests/motion_math_test.cpp
expectAngles(calculatePointLookAngles(0.0f, 1.0f, 0.0f), MotionAngles{900, 0}, "point left");
expectAngles(calculatePointLookAngles(0.0f, -1.0f, 0.0f), MotionAngles{-900, 0}, "point right");
```

`calculatePointLookAngles(x, y, z)`は右手系座標（`motion.h`のdocstringで「Y軸：正が左、負が右」と定義）から`atan2(y, x)`でyaw角を計算する関数。y=+1.0（左）を入れるとyaw=+900が返ることを、コメントではなく実際に実行される自動テストで検証している。ただし、このテストが検証しているのは「数式が、docstringで定義された座標系の通りに計算結果を返すか」という**数式と座標系定義の内部整合性**であり、**その座標系の定義自体（Y軸正＝左）が実機サーボの物理的な左右と一致しているかどうかまでは検証していない**。

### 証拠③：公式ファームウェア公開ソースのAIエージェント用システムプロンプト → 「負＝左」

```cpp
// firmware/main/hal/hal_mcp.cpp
// xiaozhi-esp32のMCPツールとして、AIが実際にmotion.yawServo().moveWithSpeed()を叩く際のプロンプト文言
"Max ranges: Yaw(-128 to 128, -128 as your left), Pitch(0 to 90, 90 as your up)."
```

こちらは実際に音声AIエージェントがロボットの首を動かす本番コードで使われる文言であり、「-128＝ロボット自身の左」と明記されている。

### まとめ

| 証拠 | 種別 | 主張 |
|---|---|---|
| ① BasicMovement.ino | 開発者が書いたサンプルコメント | 正＝左 |
| ② motion_math_test.cpp | 自動テスト（ただし数式の内部整合性を検証しているだけで、実機の物理的な向きとは無関係） | 正＝左 |
| ③ hal_mcp.cpp | 本番AIエージェント用プロンプト文字列 | 負＝左 |

2対1で「正＝左」派が多いが、①と③は開発者が人間の言葉で与えた方向ラベル（コメント・プロンプト文字列）に過ぎず、②は自動テストとして信頼できるものの実機の左右そのものを検証してはいない。つまり単純な多数決でも、テストの有無だけでも判断できない。**ソースコード分析だけではこれ以上決着しない。実機で`Motion.moveX(小さな正の値, speed)`を送り、目視で確認する以外に確定方法がない。**

### 実機お持ちの方への確認依頼

以下は安全に確認できる最小テストコードです。実機をお持ちの方は、ぜひ結果を教えていただけると嬉しいです（Issueやコメントでご連絡ください）。

```cpp
#include <Arduino.h>
#include <M5StackChan.h>

void setup() {
    M5StackChan.begin();
    M5StackChan.Display().setTextSize(2);
    M5StackChan.Display().printf("Touch to test yaw+300\n");
}

void loop() {
    M5StackChan.update();
    if (M5StackChan.TouchSensor.wasPressed()) {
        M5StackChan.Motion.goHome(300);
        delay(1500);
        M5StackChan.Display().printf("Moving to yaw=+300...\n");
        M5StackChan.Motion.moveX(300, 200);  // 十分小さく安全な角度（30.0°相当）
        delay(1500);
        // ここで物理的にどちらを向いたか目視確認
        M5StackChan.Motion.goHome(300);
    }
}
```

`moveX(正の値)`で頭が**左**を向けば証拠①②が正しく、**右**を向けば証拠③が正しいことになる。

## Pitch軸の上下方向（こちらは矛盾なし）

```cpp
// motion.h docstring
// Y-axis (Pitch): -1.0 (Max Down) <---> 1.0 (Max Up)
// BasicMovement.ino
M5StackChan.Motion.moveY(900, 200);
M5StackChan.Display().printf("> Look Up (Slow: 200)\n");
M5StackChan.Motion.moveY(0, 200);
M5StackChan.Display().printf("> Look Down (Slow: 200)\n");
```

こちらは全ての証拠が一致している。**角度0＝下向き最大、角度900(90°)＝上向き最大**で確定。

## 起動時の初期位置

```cpp
// M5StackChan.cpp
void M5StackChan_Class::begin()
{
    M5.begin();
    TouchSensor.begin();
    io_expander_init();
    servo_init();   // ← ここでサーボの角度リミット・ゼロ点をNVSから読むだけ
    ina226_init();
}
```

`begin()`内には自動でhome位置（0,0）へ移動させる処理が無い。ゼロ点（正面に対応する生位置）はNVS保存値が優先され、未保存時はYaw raw460／Pitch raw620がデフォルトとして採用される（`get_zero_pos_from_nvs()`）。起動直後の実際の見た目の挙動は実機での確認を推奨する。

## GPIO・周辺機能まとめ

StackChan公式BSPが使用するGPIOは以下の通り。CoreS3の外部拡張ポート（Port A: G1/G2、Port B: G8/G9、Port C: G17/G18）はいずれも**未使用**であり、外部拡張の余地は大きい。

| GPIO/バス | 用途 |
|---|---|
| G1, G2（Port A） | 未使用（外部I2C用に確保のみ） |
| G5 | IR送信 |
| G6 | サーボバスUART1 TX（常時使用） |
| G7 | サーボバスUART1 RX（常時使用） |
| G8, G9（Port B） | 未使用 |
| G10 | IR受信 |
| G11（内部I2C SCL）, G12（内部I2C SDA） | CoreS3標準機能＋IOエキスパンダ(PY32, 0x6F)／タッチセンサ(Si12T, 0x68)／NFC／INA226(0x41) |
| G17, G18（Port C） | 未使用 |

---

# 第2部：KariPomからの移植記録

以下は、筆者の自作ロボット「KariPom」（M5Stack CoreS3ベース、2軸SG90サーボ・顔表示・Visualizer・Lighting等を実装）をStackChanへ移植するための設計メモ。

## KariPom現行仕様（`karipom_cores3.ino`より）

### サーボ

```cpp
#include <ESP32Servo.h>
Servo servoUD;  // 上下サーボ
Servo servoLR;  // 左右サーボ

#define UD_SERVO_PIN 2  // 上下サーボ：Aポート
#define LR_SERVO_PIN 1  // 左右サーボ：Aポート

// 上下（角度大＝上）
const int SERVO_UD_UP     = 150;  // 上を向く（最大）
const int SERVO_UD_CENTER = 90;   // 正面
const int SERVO_UD_DOWN   = 70;   // 下を向く（最小・ソフトリミット）

// 左右（角度大＝左）
const int SERVO_LR_LEFT   = 120;  // 左を向く（最大、CENTER+30°）
const int SERVO_LR_CENTER = 90;   // 正面
const int SERVO_LR_RIGHT  = 60;   // 右を向く（最小、CENTER-30°）
```

KariPomはSG90×2軸のPWM駆動、角度値が大きいほど上/左を向く、という自前の符号規則を持つ。StackChanのFeetechバスサーボとは駆動方式も符号規則も異なるため、移植時は「意味（上下左右）」だけを引き継ぎ、駆動方式自体はStackChanの`Motion.moveX/moveY`に置き換える。

### ジョイスティック（Port B）

```cpp
const int JOY_X_PIN = 9;  // Port B
const int JOY_Y_PIN = 8;  // Port B
```

12bit ADC（ESP32デフォルト）、ローパスフィルタ＋デッドゾーン＋ヒステリシスで安定化。

### Karipom Ear（PCM1808 + Pico2、Port C経由）

```cpp
Serial2.setRxBufferSize(2048);
Serial2.begin(115200, SERIAL_8N1, 18, 17);  // RX=G18, TX=G17（Port C）
```

ESP32のUART2ペリフェラル（`Serial2`）を使用。Pico2側でPCM1808のI2Sを処理し、FFT値やSPEAK_START/STOPをUART経由でCoreS3へ送信、Visualizer・口パクと連動させている。

## GPIO競合表

| GPIO | StackChan公式 | KariPom | 判定 |
|---|---|---|---|
| G1, G2（Port A） | 未使用 | UD/LRサーボ（PWM） | 移植ではPWM制御自体を持ち込まないため無関係 |
| G6, G7 | サーボUART1（常時使用） | 未使用 | 競合なし。今後もKariPom側はこの2ピンに触れない |
| G8, G9（Port B） | 未使用 | ジョイスティック | **競合なし、そのまま移植可** |
| G17, G18（Port C） | 未使用 | Karipom Ear（`Serial2`） | **競合なし、そのまま移植可** |

UARTペリフェラルも、StackChanのサーボバス（UART_NUM_1）とKariPom Ear（`Serial2`＝UART_NUM_2）で完全に別物であり、競合しない。

## 移植方針

StackChan公式BSP（`M5StackChan.begin()`等）をベースとして、その上にKariPomの顔描画・Visualizer・Lighting・Wi-Fi/WebUI等を追加する方針を採用する。両者ともM5Unified/M5GFXという共通基盤の上に乗っており、GPIO競合もないため、実装レイヤーが独立して共存できる。

サーボ制御について特に重要な設計課題は「意味の翻訳層」——KariPomのUD/LR角度（自前の符号規則）を、StackChanの`Motion.moveX/moveY`（別の角度体系・前述の通り符号が未確定）へ変換する関数の実装である。これは前述の「Yaw軸の正負」問題が解決してから着手する。

これに加えて、CoreS3の初期化、Display、SD、内蔵マイク／スピーカー、Wi-Fi/WebUI、サーボ電源制御などについても、StackChan-BSPとのソフトウェア統合作業が必要になる。

## まとめ・今後

StackChan公式ソースの調査により、以下が明らかになった。

- サーボ通信・角度体系はBSPとドキュメントレベルでは分かるが、**BSPと公式ファームウェア公開ソースで実際のPitch可動域が異なる**という、ソースコード比較でしか気づけない差分がある
- **Yaw軸の正負と左右の対応は、公式ソース内に3つの矛盾した記述があり、実機確認以外に確定方法がない**
- KariPomのようなCoreS3ベースの自作ロボットをStackChanへ移植する場合、外部拡張ポート（Port A/B/C）がすべて未使用のため、GPIO競合の心配なく機能追加が可能

本記事執筆時点では、筆者自身が公式StackChanを購入するかどうかは未定であり、実機検証には至っていない。今後実機を入手した場合には、本調査をもとにKariPomの移植を試みる予定である。

また、本記事は筆者自身の将来の移植作業のためだけでなく、KariPomを公式StackChanへ移植してみたい方の参考資料として公開するものである。実機をお持ちの方によるYaw軸の方向確認や、実際の移植結果についての情報提供も歓迎する。
