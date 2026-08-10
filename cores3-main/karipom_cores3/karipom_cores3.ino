// ============================================================
// かりポム (KariPom) - M5Stack CoreS3 顔アニメーションロボット
// Last updated: build time = FW_BUILD (__DATE__ __TIME__), rename file to match Build after compile
// Base: karipom_20260628_1710_stable_2.ino (tag: stable_2.2_FINAL)
// Change: idle UD range 100/80 -> 110/70 only (handleIdleServoMotion body untouched)
// Change 2026/07/07: moveSmooth ease-in-out / LR_ATTACH_SETTLE_MS 50->100 /
//   IDLE LR PRE-ATTACH settle delay / idle LR centering waitMs 10->15
// Change 2026/07/07 (2): LR attach diagnosis logs (moveSmooth/joystick re-attach) /
//   LR_ATTACH_SAG_OFFSET_DEG experimental knob (default 0 = no behavior change)
// Change 2026/07/07 (3): joystick Yaw slew-rate limit (1deg/30ms, non-blocking) /
//   joystick re-attach first write = lrNow (was targetLR) / lrEnsureAttached dead-code note
// Change 2026/07/07 (4): Yaw speed ~50% as standard spec (LR_SLOWDOWN_FACTOR=2) /
//   JOY_LR_SLEW_STEP_MS=30 / Face Gallery UI: 🎨顔を選ぶ btn / 🙂標準の顔に戻す btn / /resetface route
// Change 2026/07/07 (5): prepareLogToilet() size guard (SAFE_LOG_MAX_BYTES=512KB) /
//   archiveOrDeleteLog() added / oversized logs archived to /logtoilet/old_logs/ before line-count
// Change 2026/07/07 (6): joystick optional (JOY_FLOAT_VARIANCE_TH/JOY_STABLE_COUNT) /
//   calib variance check / runtime float detect+disable / stable-center re-enable / WAV loop guard /
//   Cockpit JOY ON/OFF status display
// Change 2026/07/07 (7): joy re-enable uses runtime recalib (not joyCenterX/Y proximity) /
//   applyJoyCalibThresholds() extracted / recalibrateJoystickRuntime() added /
//   calibrateJoystick() delegates threshold calc to applyJoyCalibThresholds()
// Change 2026/07/07 (8): calibrateJoystick() → no-op stub (joystickEnabled never set true inside) /
//   setup() calibrateJoystick() call removed → boot log only /
//   joystickEnabled=false at all times until runtime recalib confirms stable connection
// ============================================================
// karipom_face_v46_5_3_talk_micro_motion_smooth_wide
// v46を基準に、Mouth座標のSerial出力だけを停止。v47の自動更新変更は入れない
// v38を基準に、リセット/再起動直後のサーボ暴れ対策を追加
// v29を基準に、カメラによる自動首振りをOFF。Web手動操作とDEMOの「黒目先行→首移動→黒目中央」だけを確認する版
// Change 2026/07/16 (EAR Step4): Ear FFTをfftLevel[]/lastFftPacketTimeへ接続（Visualizer Face反映）/
//   Ear優先・途絶400msでUDP FFTへ自動復帰 / EAR_FFT_TO_FACE_ENABLEDで無効化可
// Change 2026/07/16 (EAR Step4.1): 音声ソース選択と入力源を整合（LINE IN=Ear / UDP=Mac）/
//   isVisualizerFaceEnabledにLINE IN追加 / updateAudioInputのLINEIN枠をEar接続 / WebUI文言更新
// Change 2026/07/16 (EAR Step5.1): Ear SPEAKをexternalSpeakingへ接続（LINE IN選択時のみ・口パク）/
//   UDP SPEAK_STOPにLINE INガード追加 / EAR_SPEAK_TO_MOUTH_ENABLEDで無効化可（Phase 0完成）
#include <M5CoreS3.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <time.h>
#include <ESP32Servo.h>
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include "esp_system.h"
#include "esp_sntp.h"     // sntp_get_sync_status() / SNTP_SYNC_STATUS_COMPLETED
#include <Preferences.h>
#include <vector>
#include <algorithm>

Servo servoUD;  // 上下サーボ（UD_SERVO_PIN=2）
Servo servoLR;  // 左右サーボ（LR_SERVO_PIN=1）

// ====================================================
// ファームウェアバージョン
// ブラウザキャッシュ対策として、Web Cockpitページのタイトル・見出しに
// このバージョン文字列を表示する。ビルドごとに更新すること。
// ====================================================
// ====================================================
// ファームウェアバージョン・ビルド情報
// ログの先頭に記録し、どのビルドで取得したログか判別できるようにする。
// FW_VERSION は手動更新、FW_BUILD は __DATE__ __TIME__ で自動記録。
// ====================================================
#define FW_VERSION "stable_2.3"
#define FW_BUILD   (__DATE__ " " __TIME__)

// ====================================================
// FFTデバッグ表示モード（Visualizer Face 開発用・暫定）
// 有効時：黒背景に8本のFFTバーだけを表示し、顔表示系はスキップする。
//         （UDP受信・WebUI・サーボフェイルセーフは通常動作）
// 無効時（コメントアウト）：従来の顔表示システムが100%従来通り動作する。
//         ※FFTパケットの値保存とログ抑止のみ常時有効（ログ汚染対策）
// ====================================================
#define FFT_DISPLAY_TEST

// ============================================================================
// 【Arduino IDE 自動プロトタイプ生成対策】Psychedelic / Trance の PsyPal 型の前方定義
//
// ■ なぜここ（ファイル先頭）に置く必要があるのか
//   Arduino IDE / arduino-cli は .ino をコンパイルする前に ctags でスケッチを走査し、
//   全関数の【プロトタイプを自動生成して、"最初の関数定義の直前" へ一括挿入】する。
//   このスケッチで最初に現れる関数定義は直下の logFirmwareInfo() であり、
//   自動生成プロトタイプはその直前（＝この位置）へ差し込まれる。
//
//   Psychedelic ブロックには
//       static void psyFlashOpGrid(const PsyPal& P) { ... }
//   のように【ユーザー定義の struct を引数に取る関数】がある。
//   struct PsyPal の定義はファイル後半（Lighting #15 のブロック内）にあるため、
//   自動生成された
//       static void psyFlashOpGrid(const PsyPal &P);
//   が logFirmwareInfo() の直前に挿入された時点では PsyPal が未知となり、
//       error: 'PsyPal' does not name a type
//   でコンパイルが停止する。
//   （arduino-cli は #line ディレクティブで行番号を元の定義行へ戻すため、
//     エラー表示はブロック内の定義行を指すが、実際の失敗箇所はこの挿入位置である）
//
//   既存コードがこの問題を踏んでいなかったのは、ユーザー定義 struct を引数に取る
//   関数が Fighter Duel の sfDrawFighter(const SFFighter&, ...) だけで、そこには
//   struct 定義の直後に明示的な前方宣言が書かれていたため（＝ctags は
//   「プロトタイプが既にある」と判断して自動生成をスキップする）。
//
// ■ 対策
//   (1) 型そのものを、自動生成プロトタイプの挿入位置より前＝ここで定義する（本ブロック）。
//       これにより ctags の挙動に依存せず、どんな順序で挿入されても型が既知になる。
//   (2) さらに Lighting #15 ブロック側で、PsyPal を引数に取る全関数へ明示的な
//       前方宣言を置く（既存 sfDrawFighter と同じ流儀。二重の保険）。
//   実体の配色テーブル PSY_PAL[12] は、設計の可読性のため
//   Lighting #15 のブロック内に置いたままにしてある。
// ============================================================================
struct PsyPal { uint16_t bg, mn, ac; uint8_t dark; };

// logFirmwareInfo():
//   Firmware/Build情報をログに出力する。
//   setup序盤（SD未初期化）とSD初期化後の2回呼ぶことで
//   Serial/WebログとSDログの両方に確実に残す。
void logFirmwareInfo() {
  addLog("========================================");
  addLog("Firmware : " + String(FW_VERSION));
  addLog("Build    : " + String(FW_BUILD));
  addLog("========================================");
}

// ====================================================
// 時刻同期フラグ
//
// 優先順位：
//   1. NTP同期（最優先）
//   2. Mac TIME_SYNC（UDPで受信）
//   3. RTC継続（一度でも同期済みならRTCで継続）
//   4. 未同期（一度も同期できていない場合は時計表示せずFace Galleryへ）
//
// ── BM8563 RTC 時刻管理（2026/07/06） ────────────────────────────────
// NTP同期成功時にBM8563へ時刻を書き込み、次回起動時にESP32システム時刻へ復元する。
// これにより Wi-Fiなし/APモード/NTP失敗時でも過去同期済みの時刻でログを打刻できる。
bool rtcTimeValid = false;  // BM8563から有効な時刻を読み出せたか（setup時に評価）

// BM8563 → ESP32システム時刻 へ復元する。
// 戻り値: true=成功（有効な時刻）、false=未設定または異常値
bool restoreTimeFromRtc() {
  auto dt = CoreS3.Rtc.getDateTime();
  int yr = dt.date.year;
  int mo = dt.date.month;
  int dy = dt.date.date;
  int hr = dt.time.hours;
  int mi = dt.time.minutes;
  int sc = dt.time.seconds;
  if (yr < 2020 || yr > 2099 || mo < 1 || mo > 12 || dy < 1 || dy > 31) {
    return false;
  }
  struct tm t = {};
  t.tm_year  = yr - 1900;
  t.tm_mon   = mo - 1;
  t.tm_mday  = dy;
  t.tm_hour  = hr;
  t.tm_min   = mi;
  t.tm_sec   = sc;
  t.tm_isdst = -1;
  // BM8563にはJST時刻が保存されている。
  // mktime() はTZ=JST-9環境下でこれをJSTローカル時刻として解釈し、UTC epochを返す。
  // つまり "JST 10:00" → UTC 01:00 相当のepochが得られる（正しい変換）。
  // settimeofday()にUTC epochを渡し、以降getLocalTime()がJSTで返るようになる。
  time_t epoch = mktime(&t);
  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  return true;
}

// ESP32システム時刻（JST）→ BM8563 へ書き込む。NTP同期成功後に呼ぶ。
// getLocalTime()はJST時刻を返すため、BM8563にもJSTで保存される。
// 次回起動時のrestoreTimeFromRtc()でmktime()がJST→UTC変換して正しく復元される。
void saveTimeToRtc() {
  struct tm t;
  if (!getLocalTime(&t, 0)) {
    Serial.println("[saveTimeToRtc] getLocalTime failed, skip");
    return;
  }

  // 書き込む時刻をログに残す（ズレ検証用）
  char preBuf[48];
  snprintf(preBuf, sizeof(preBuf),
           "RTC WRITE INPUT (JST): %04d/%02d/%02d %02d:%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  addLog(String(preBuf));
  m5::rtc_datetime_t dt;
  dt.date.year    = t.tm_year + 1900;
  dt.date.month   = t.tm_mon  + 1;
  dt.date.date    = t.tm_mday;
  dt.date.weekDay = t.tm_wday;
  dt.time.hours   = t.tm_hour;
  dt.time.minutes = t.tm_min;
  dt.time.seconds = t.tm_sec;
  CoreS3.Rtc.setDateTime(dt);

  // 書き込み後に読み返して確認
  auto verify = CoreS3.Rtc.getDateTime();
  char buf[64];
  snprintf(buf, sizeof(buf),
           "RTC WRITE -> READ BACK: %04d/%02d/%02d %02d:%02d:%02d",
           verify.date.year, verify.date.month, verify.date.date,
           verify.time.hours, verify.time.minutes, verify.time.seconds);
  addLog(String(buf));

  // getLocalTimeでESP32システム時刻側も確認
  struct tm check = {};
  if (getLocalTime(&check, 0)) {
    char lbuf[48];
    snprintf(lbuf, sizeof(lbuf),
             "LOCALTIME AFTER RTC WRITE: %04d/%02d/%02d %02d:%02d:%02d",
             check.tm_year + 1900, check.tm_mon + 1, check.tm_mday,
             check.tm_hour, check.tm_min, check.tm_sec);
    addLog(String(lbuf));
  } else {
    addLog("LOCALTIME AFTER RTC WRITE: getLocalTime FAILED");
  }
}
// ─────────────────────────────────────────────────────────────────────

// timeEverSynced: NTPまたはMac TIME_SYNCで一度でも時刻同期できたら true。
//   true になると drawClock() が時計表示を許可し、
//   getLocalTime() が失敗してもRTCの内部時刻を使って継続できる。
//   false のままだと時計表示せずスリープ中はFace Galleryにフォールバック。
// ====================================================
bool timeEverSynced = false;

// ====================================================
// Camera latest frame
// Web表示用の最新BMP画像
// ====================================================
uint8_t* latestBmp = nullptr;
size_t latestBmpSize = 0;
unsigned long latestBmpTime = 0;

// ====================================================
// Camera view settings
// ====================================================
const unsigned long BMP_UPDATE_INTERVAL = 3000;

// 将来の　BMP縮小保存用（現在は未使用）
const int CAMERA_BMP_WIDTH = 240;
const int CAMERA_BMP_HEIGHT = 240;

// ====================================================
// ファイル読み込みサイズ上限
// malloc前にチェックしてヒープ枯渇・クラッシュを防ぐ。
// WAV: ESP32のヒープは最大約4MB。余裕を見て1MBに制限。
// PNG: 320x240 RGBA相当で約300KB。ディスプレイ解像度以上のPNGは弾く。
// 変更時は実機のヒープ空き（ESP.getFreeHeap()）を確認してから広げること。
// ====================================================
const size_t MAX_WAV_SIZE = 1024UL * 1024UL;  // 1 MB
const size_t MAX_PNG_SIZE = 512UL  * 1024UL;  // 512 KB

// ====================================================
// LCDバックライト標準輝度
// 2026/07/21: 画面周辺の発熱・消費電力軽減のため 100%(255) → 70% へ変更。
// setBrightness()は0〜255の範囲のため、単純に70を入れず255*0.70=178.5の
// 実際の70%相当値（四捨五入で179）を使用する。
// 起動時（setup）・スリープ復帰時（wakeUp）の両方でこの1値を共通参照することで、
// どちらか一方だけ変更漏れが起きて100%へ戻ってしまう事故を防ぐ。
// 顔画像・背景色・描画内容・白背景デザインには一切影響しない（バックライト輝度のみ）。
// ====================================================
const uint8_t LCD_BACKLIGHT_STANDARD = 179;  // 255 * 0.70 = 178.5 → 179（70%相当）

// ====================================================
// Network services
// Web server + UDP audio link
// ====================================================
WebServer server(80);
WiFiUDP udp;
const int UDP_PORT = 12345;
String apName = "KariPom_AP";
String apPass = "karipom123";

#define UD_SERVO_PIN 2  // 上下サーボ（servoUD）：Aポート2分岐サーボ
#define LR_SERVO_PIN 1  // 左右サーボ（servoLR）：Aポート2分岐サーボ

// 現在の首角度。起動直後は安全のため数値で初期化する
int udNow = 90;  // 上下サーボの現在角度
int lrNow = 90;  // 左右サーボの現在角度

// ====================================================
// リセット時サーボ暴れ対策
// RTCメモリに最後の角度を保存し、再起動時にその角度から再開する。
// スタンバイ姿勢などからリセットしても、いきなりセンターへ戻さない。
// リセット時サーボ暴れ対策用。RTCメモリに最後の角度を保存する
// ====================================================
RTC_DATA_ATTR uint32_t rtcServoMagic = 0;
RTC_DATA_ATTR int rtcUdNow = 90;
RTC_DATA_ATTR int rtcLrNow = 90;

// RESET REASONをRTC（不揮発）に保存する。
// SDが使えない状況でも前回のリセット原因を確認できる。
RTC_DATA_ATTR int rtcLastResetReason = -1;  // -1=未記録
const uint32_t SERVO_RTC_MAGIC = 0x4B415250;  // "KARP"

// ====================================================
// Neck servo angles
// 命名規則：
//   SERVO_UD_* = 上下サーボ（servoUD / UD_SERVO_PIN=2）の角度値
//   SERVO_LR_* = 左右サーボ（servoLR / LR_SERVO_PIN=1）の角度値
//   HEAD_*     = 「かりポムが見た目でどちらを向くか」を基準にした alias
// ====================================================

// 上下サーボ角度（servoUD / UD_SERVO_PIN=2）
// UPボタンで頭が上を向く。角度値が大きいほど上を向く。
const int SERVO_UD_UP     = 150;  // 上を向く（最大）
const int SERVO_UD_CENTER = 90;   // 正面
// ── SERVO_UD_DOWN 50→70（2026/07/09・サーボ保護ソフトリミット）──────
// 根拠:
//  (1) アイドル可動域は既に「ピンク筐体の機械的負荷軽減」で70に制限済み
//      （SERVO_UD_IDLE_DOWN）。Web DOWNとジョイスティックYだけ50が残っていた。
//  (2) DOWN(50)保持中のジョイスティックADC同相ディップが dx/dy=700〜900 と
//      UP時(約250)の3倍超 → UDサーボのストール級電流（突っ張り）の証拠。
//  (3) DOWN連打後の「ギュン」音はストール由来のギヤ滑り/蓄積歪み解放が疑われ、
//      LRサーボを損傷させたのと同型の故障モード。
// この定数はWeb DOWN・ジョイスティックY・constrain範囲・talk微動の
// 全経路に一括で効く（HEAD_DOWN alias経由）。
// シーソー構造など機構改修後に50へ戻して再評価すること。
const int SERVO_UD_DOWN   = 70;   // 下を向く（最小・暫定ソフトリミット、旧50）

// アイドル専用・上下可動域（ピンク筐体の機械的負荷軽減）
// 通常姿勢・睡眠・なでなでは SERVO_UD_UP/DOWN を使用。
// アイドル動作（handleIdleServoMotion）のみここを参照。
// 2026/07/09: IDLE上下を±20°→±15°へ縮小（静音・低負荷化）。
// IDLE LR振り幅制限（IDLE_LR_SWING_MAX=15）でギュイン音は減ったが、
// UDのIDLE上下動作後にもマイク反応が残ったため、IDLE動作全体を控えめにする。
// Web操作・ジョイスティック・talk微動・センター復帰は SERVO_UD_UP/DOWN 側を
// 参照するため影響なし（この2定数はIDLE専用）。
// 経緯: 旧100/80 → 110/70（±20°拡大・切り分けテスト）→ 105/75（±15°・本変更）
// ── ジョイスティック上方向ソフトリミット（2026/07/10・ベアリング化後ギヤ鳴き対策）──
// ベアリング化でUD機構の負荷特性が変化し、SERVO_UD_UP=150への全可動で
// ギヤ鳴きが発生。ジョイスティック経路のみ上限を10°下げて保護する。
// Web UP / sleep/wakeup / なでなで等の他経路は HEAD_UP(=150) を直接参照するため影響なし。
// 機構再評価後は SERVO_UD_UP と揃えること。
const int JOY_UD_UP = 140;           // ジョイスティック上限（SERVO_UD_UP=150より10°小）

const int SERVO_UD_IDLE_UP   = 105;  // アイドル時の上限（IDLE専用・90中心+15°）
const int SERVO_UD_IDLE_DOWN = 75;   // アイドル時の下限（IDLE専用・90中心-15°）

// アイドル専用・左右（LR）振り幅（2026/07/09・サーボ保護）
// IDLE左右動作（swing=26〜28前後）後のセンター復帰〜AUTO DETACH周辺で
// LRの「ギュイン/ギュオン」音がマイクログと目視で確認されたため、
// IDLEの振り幅のみを控えめに制限する（負荷切り分けを兼ねる）。
// Web操作・ジョイスティック・talk微動の左右可動域には一切影響しない
// （それらは HEAD_LEFT/RIGHT や TALK_MICRO_LR_RANGE を参照するため）。
// 音が収まったら段階的に広げて再評価。機構改修後は旧 random(15, 31) へ。
const int IDLE_LR_SWING_MIN = 2;   // IDLE左右振りの最小角（度・旧8）のっそりモード
const int IDLE_LR_SWING_MAX = 5;   // IDLE左右振りの最大角（度・旧15）のっそりモード

// ── IDLE左右動作 比較試験フラグ（ギュイン音原因切り分け用）──────────
// true: IDLEのLR動作をスキップする（LR動作が原因かどうかを確認するため）。
// ギュイン音消滅が確認されたら false に戻してLR再有効化する。
// Web操作・ジョイスティック・talk微動・AUTO DETACHは一切影響しない。
const bool IDLE_LR_DISABLED = true; // 2026/07/12: テスト用 IDLE LR停止（ギュワン音切り分け）

// 左右サーボ角度（servoLR / LR_SERVO_PIN=1）
// LEFTボタンで頭が左を向く。角度値が大きいほど左を向く。
const int SERVO_LR_LEFT   = 120;  // 左を向く（最大）CENTERから+30°
const int SERVO_LR_CENTER = 90;   // 正面
const int SERVO_LR_RIGHT  = 60;   // 右を向く（最小）CENTERから-30°

// semantic aliases（HEAD_* は動作方向で命名。内部実装を意識せず使える）
const int HEAD_UP                = SERVO_UD_UP;
const int HEAD_DOWN              = SERVO_UD_DOWN;
const int HEAD_VERTICAL_CENTER   = SERVO_UD_CENTER;

const int HEAD_LEFT              = SERVO_LR_LEFT;
const int HEAD_RIGHT             = SERVO_LR_RIGHT;
const int HEAD_HORIZONTAL_CENTER = SERVO_LR_CENTER;

const int noseX = 160;
const int noseY = 145;

bool toggle = false;

unsigned long lastNoseMove = 0;   // handleNoseMotion 専用（覚醒中・睡眠中共通）
unsigned long lastNoseDrawTime = 0;  // updateNose() が実際に鼻を再描画した最終時刻（NOSE STALL診断用）
extern bool yawnMode;  // 実体は後方で定義（診断ハートビートが先に参照するため前方宣言）
unsigned long lastBlinkCheck = 0;
unsigned long lastMotionTime = 0;
unsigned long nextBlinkInterval = 3000;
unsigned long sleepStartTime = 0;

// ── sleep解除用 連続確認カウンタ ─────────────────────────
// 1回のADC読み取りやIMU値だけでwakeしないよう、
// 複数回連続して条件を満たした場合のみwakeする設計。
// sleep直後の誤wakeを防ぐため、sleepStartTime+SLEEP_WAKE_IGNORE_MS 以内は無視。
const unsigned long SLEEP_WAKE_IGNORE_MS  = 3000;  // sleep直後3秒は無視
const unsigned long SLEEP_JOY_CONFIRM_MS  = 400;   // ジョイスティック連続確認時間
const unsigned long SLEEP_IMU_CONFIRM_MS  = 400;   // IMU連続確認時間
const float         SLEEP_IMU_THRESHOLD   = 1.4f;  // IMU閾値（旧1.2fから引き上げ）

unsigned long sleepJoyConfirmStart = 0;  // ジョイスティック確認開始時刻
unsigned long sleepImuConfirmStart = 0;  // IMU確認開始時刻
unsigned long lastEnvReportTime = 0;
unsigned long lastSleepCameraCheck = 0;
unsigned long lastIdleMoveTime = 0;
unsigned long nextIdleMoveInterval = 90000;  // 初回は90秒後くらい
unsigned long lastMutterPlayedTime = 0;

const unsigned long MUTTER_COOLDOWN = 15000;  // 15秒は次の独り言を禁止

// 低バッテリー通知
const int    BATTERY_LOW_THRESHOLD   = 30;          // 30%未満で通知
const unsigned long BATTERY_LOW_COOLDOWN = 1800000; // 30分（ms）
unsigned long lastHungryPlayedTime   = 0;            // 最後に再生した時刻

// 起動完了まで電源表示を抑制するフラグ。
// setup()末尾の drawFace() 直前に true にセットし、
// 起動シーケンス（×目画面・WiFi接続中等）では描画しない。
bool batteryDisplayEnabled = false;

int nextNoseInterval = 180;
int cameraMotionCount = 0;

unsigned long lastLookAtMotion = 0;

const unsigned long MOTION_LOOK_COOLDOWN = 1500;  // 追従後1.5秒は次の反応をしない
const int MOTION_TRIGGER = 450;                   // 動き全体のしきい値。小さすぎるとチラつく
const int MOTION_DIFF = 120;                      // 左右差のしきい値


unsigned long alertUntil = 0;

unsigned long lastExternalStopTime = 0;
const unsigned long AFTER_EXTERNAL_SPEAK_GRACE = 8000;

// ====================================================
// Grove Thumb Joystick / PORT C  ── LPFによる安定化版 (v50_6)
//
// v50_5までの問題：
//   ・閾値(ON_TH)を上げても「生値がノイズで揺れている」根本原因は変わらない
//   ・ピンク個体はADCノイズが大きく、閾値を650まで上げても震えが止まらなかった
//
// v50_6の方針：
//   ・ローパスフィルター（指数移動平均）でノイズを先に除去する
//   ・フィルター後の値でデッドゾーン・ヒステリシスを判定する
//   ・閾値はティファニーブルー個体でも動く現実的な値に戻す
//
// ピン
const int JOY_X_PIN = 9;  // Port B
const int JOY_Y_PIN = 8;  // Port B

// ── キャリブレーション結果（起動時に calibrateJoystick() が書き込む） ──
int joyCenterX = 2048;
int joyCenterY = 2048;

// ── 実ストローク（キャリブレーション時に計算） ──────────
// LOW方向ストローク = joyCenterX、HIGH方向ストローク = 4095 - joyCenterX
// 短い方を使って両方向で対称な感度にする。
int joyStrokeX = 2048;  // X軸の有効ストローク（short側）
int joyStrokeY = 2048;  // Y軸の有効ストローク（short側）

// ── サーボ用ヒステリシス閾値（キャリブレーション時に動的計算） ──
// ON_TH  = ストロークの 20%（ここを超えたらサーボ動作開始）
// OFF_TH = ストロークの 12%（ここを下回ったらサーボ停止）
// 固定値ではなく割合で決めることで、どの個体でも同じ操作感になる。
int joyServoOnThX = 200;  // calibrateJoystick() で上書きされる
int joyServoOffThX = 120;
int joyServoOnThY = 200;
int joyServoOffThY = 120;

// ── 黒目用ヒステリシス閾値（同様に動的計算） ──
// ON_TH  = ストロークの 30%
// OFF_TH = ストロークの 20%
int joyEyeOnThX = 300;
int joyEyeOffThX = 200;
int joyEyeOnThY = 300;
int joyEyeOffThY = 200;

// ── ローパスフィルター ──
float joyFilteredX = 2048.0f;
float joyFilteredY = 2048.0f;
// alpha を小さくするほどノイズ除去が強くなるが応答が遅くなる。
// ピンク筐体のノイズ対策のため0.4→0.2に下げる。
// ティファニーブルーで反応が遅く感じる場合は0.3に上げること。
const float JOY_LPF_ALPHA = 0.2f;

// ── ループ毎のwrite間隔 ──
const unsigned long JOY_WRITE_INTERVAL = 20;  // ms（旧50msから短縮）

// ── 状態変数 ──
unsigned long lastJoystickWriteTime = 0;
bool joystickWasActive = false;

// ── ジョイスティック固着（stuck）検出 ────────────────────
// 同じ方向入力が STUCK_THRESHOLD_MS 以上続いたら固着と判定し入力を無視する。
// 中央に戻ったら自動解除。
unsigned long joyActiveStartTime = 0;      // アクティブ入力が始まった時刻
bool joyStuck = false;                     // 固着フラグ
const unsigned long JOY_STUCK_THRESHOLD_MS = 1500;  // 固着判定時間（ms）

// ジョイスティック異常値（断線・浮き・接触不良）の連続カウント
// 連続してJOY_ABNORMAL_THRESHOLDを超えたらその回の処理をスキップする
int joyAbnormalCount = 0;
const int JOY_ABNORMAL_THRESHOLD = 3;  // 連続3回異常で無効化

// ジョイスティック入力タイムアウト
// 最後にアクティブ入力があった時刻。一定時間経過で全状態を強制リセット。
// 故障・接触不良・倒しっぱなしで永続的に入力が残るのを防ぐ。
unsigned long lastJoyActiveTime = 0;
const unsigned long JOY_INPUT_TIMEOUT_MS = 5000;  // 5秒で強制リセット

bool joyServoXActive = false;
bool joyServoYActive = false;
bool joyEyeXActive = false;
bool joyEyeYActive = false;

// ====================================================
// ジョイスティック接続検出・浮き判定（2026/07/07）
//
// 未接続ADCは値が安定しない（ループごとに大きく変動する）。
// calibrateJoystick() の平均値チェックだけでは「浮きが中央付近で
// 安定したように見える」ケースを捕捉できないため、ランタイムでも
// 検出・無効化・復帰判定を行う。
//
// 無効化条件（handleJoystick内・動的）:
//   JOY_FLOAT_WINDOW_MS 間に JOY_FLOAT_VARIANCE_TH を超える分散が
//   JOY_FLOAT_MIN_COUNT 回以上観測 → joystickEnabled=false
//
// 復帰条件:
//   JOY_STABLE_WINDOW_MS 間、中心値との偏差が JOY_STABLE_TH 以下の
//   サンプルが JOY_STABLE_COUNT 回以上連続 → joystickEnabled=true
// ====================================================
const int            JOY_FLOAT_VARIANCE_TH  = 150;   // この分散（ADC差分）を超えたら浮きサンプル
const int            JOY_FLOAT_MIN_COUNT    = 8;     // この回数超えたら無効化
const unsigned long  JOY_FLOAT_WINDOW_MS    = 500;   // 分散カウントのウィンドウ幅

const int            JOY_STABLE_TH          = 80;    // 安定とみなす中心偏差（ADC値）
const int            JOY_STABLE_COUNT       = 30;    // 連続安定サンプル数で復帰
const unsigned long  JOY_STABLE_WINDOW_MS   = 2000;  // 復帰判定の最大待機時間

// ランタイム状態（handleJoystick内で更新）
int            joyFloatCount        = 0;    // 現在ウィンドウ内の浮きサンプル数
unsigned long  joyFloatWindowStart  = 0;    // 浮きカウントウィンドウ開始時刻
int            joyPrevRawX          = -1;   // 前回rawX（分散計算用）
int            joyPrevRawY          = -1;   // 前回rawY（分散計算用）
int            joyStableCount       = 0;    // 連続安定サンプル数（復帰判定用）
unsigned long  joyStableWindowStart = 0;    // 復帰判定ウィンドウ開始時刻

// ── ウォームアップ（ON直後の入力無視期間）──────────────────────
// runtime recalib 直後、LPFが中心値に収束するまでの間に
// 古い（不安定な）ADC値がそのまま入力されると誤動作する。
// JOY_WARMUP_MS の間は入力を無視し、LPFを中心値へ安定させる。
const unsigned long JOY_WARMUP_MS  = 700;   // ウォームアップ時間（ms）
unsigned long       joyWarmupUntil = 0;    // この時刻まで入力無視（0=制限なし）

// ====================================================
// Wi-Fi reconnect monitor
// ====================================================
unsigned long lastWiFiCheckTime = 0;
const unsigned long WIFI_CHECK_INTERVAL = 10000;
bool wifiReconnectRunning = false;  // 再接続処理中フラグ

bool clockShownInSleep = false;
const unsigned long SLEEP_FACE_DURATION = 180000;  // 3分

// ====================================================
// スリープ中 Face 表示（NTP未取得時の時計代替）
//
// mutterFaceActive（呟き演出）とは完全に別管理。
// 呟き終了処理（drawFace()/showEvent("N")）はスリープ中に呼ばれないため
// 干渉しないが、フラグを分けることで将来の変更にも安全。
//
// SLEEP_FACE_ROTATE_MS: Sleep Lighting Carousel（Face Gallery ⇔ 目×Lighting）の
//   表示切替周期。実機確認の結果、既定45秒では短すぎたため3分へ変更した
//   （2026-08-04）。Lighting自体のアニメーション更新周期(LIGHT_COMPOSITE_MS)とは
//   別物で、こちらは「何を表示するか」を選び直す周期だけを指す。
// ====================================================
bool sleepFaceActive   = false;        // スリープ中にFace画像を表示中
unsigned long lastSleepFaceRotateTime = 0;  // 最後にFace画像を切り替えた時刻（現在は未使用の名残）
const unsigned long SLEEP_FACE_ROTATE_MS = 180000;  // Sleep Carousel切替周期（3分）

// Face表示試行時刻管理
// SDなし・PNGなしで showSleepFace() が失敗しても毎loop呼ばれないよう制御する。
// SLEEP_FACE_RETRY_MS 経過後に再試行する（その間は睡眠目のまま）。
unsigned long lastSleepFaceAttemptTime = 0;
const unsigned long SLEEP_FACE_RETRY_MS = 60000;  // 失敗後の再試行間隔（60秒）

// ====================================================
// パックマンのモンスター移動法：視線だけ先に動かす
// サーボは動かさず、黒目だけ大きめにずらして方向感を確認する
// ====================================================
int eyeOffsetX = 0;
int eyeOffsetY = 0;
const int EYE_SHIFT_PIXELS = 14;  // 黒目先行用。テスト用に大きめ。自然版では10〜12へ調整
String eyeDirectionLabel = "CENTER";

// ====================================================
// Camera gaze tracking
// 動いたものを黒目でチラッと追う。首は追跡しない。
// 2秒後に黒目と首をセンターへ戻す。
// ====================================================
const bool ENABLE_CAMERA_GAZE = true;

// ====================================================
// 実行時設定フラグ（NVSに永続化。Preferencesライブラリ使用）
// ボタンを押した瞬間にNVSへ書き込まれ、再起動後も保持される。
// デフォルトはすべてOFF。動作への影響が大きいため、
// 必要なときだけWebUIから個別にONにする運用を推奨。
// ====================================================
bool cfg_enableCamera    = false;
bool cfg_enableToiletCam = false;
bool cfg_enableSDLog     = false;

// ====================================================
// 🎙 音声入力フレームワーク（ソース選択）
// ※Arduino IDE の自動プロトタイプ生成より前に型を確定させるため、ここで定義する。
//
// かりポムの口パク・表情・首の微動を駆動する「音」の入力元を選択する。
//   AUDIO_SRC_OFF    : 音声入力なし（口パク停止）
//   AUDIO_SRC_UDP    : Mac(Python)からのUDP SPEAK_START/STOP【現行方式】
//   AUDIO_SRC_MIC    : CoreS3内蔵マイクのRMS（周囲の音に反応する専用モード）
//   AUDIO_SRC_LINEIN : PCM1808 I2S ADCによるLINE入力【ハード未実装・ソフト枠のみ】
//
// 各ソースは updateAudioInput() 内で共通データ audioLevel と
// 発話状態 externalSpeaking へ変換される。以降の
// 口パク（updateExternalMouth）・talk micro motion・表情制御は
// ソースの違いを一切意識しない。
//
// 選択値はNVSへ永続化（saveConfig/loadConfig）。適用は setup() 末尾の
// applyBootAudioSource() で行うため、起動音は常にスピーカーから再生される。
// ====================================================
enum AudioSource : uint8_t {
  AUDIO_SRC_OFF    = 0,
  AUDIO_SRC_UDP    = 1,
  AUDIO_SRC_MIC    = 2,
  AUDIO_SRC_LINEIN = 3,
};

AudioSource audioSource = AUDIO_SRC_UDP;  // 現行動作維持のためデフォルトUDP
int audioLevel = 0;                       // 共通音量データ（0〜1023目安）
uint8_t cfg_bootAudioSource = AUDIO_SRC_UDP;  // NVSから読んだ起動時ソース

// 自動プロトタイプ対策の明示宣言
void setAudioSource(AudioSource src, const char* reason);
const char* audioSourceName(AudioSource src);
String audioSourceShortLabel(AudioSource src);
String audioSourceDescriptionHtml(AudioSource src);
String karipomDisplayName();

// ====================================================
// キャラクタースタイル（顔の着せ替えではなく、最初に選ぶキャラクター人格）
// 違いは目のまつ毛のみ（drawOpenEyes()内のdrawEyelashes()で描画）。
// 選択値はNVSへ永続化（saveConfig/loadConfig）。デフォルトはKariPom。
// ====================================================
enum CharacterStyle : uint8_t {
  CHARACTER_KARIPOM      = 0,
  CHARACTER_MISS_KARIPOM = 1,
};

CharacterStyle cfg_characterStyle = CHARACTER_KARIPOM;  // デフォルト: かりポム

const char* characterStyleName(CharacterStyle s);

// ====================================================
// 内蔵マイク閾値設定変数
// constではなくintにすることで、Webから実行時変更・NVS永続化できる設計にしている。
// Web設定化: saveConfig()/loadConfig() に putInt/getInt 追加済み。
// /mic_config?on=NNN&off=NNN&hold=NNN ハンドラで即時反映。
// 推奨範囲: on=10〜2000 / off=5〜500 / hold=100〜5000
// ====================================================
int cfg_micOnThreshold     = 150;   // デフォルト: 職場環境ノイズ(35〜60)・キーボード音を除外する値
int cfg_micOffThreshold    = 50;    // デフォルト: ON_TH=150の1/3。静音フロア(5-8)の約6倍
unsigned long cfg_micReleaseHoldMs = 1000;  // デフォルト: 短文発話後の素早い口パク終了を優先

Preferences karipomPrefs;

// ====================================================
// 発生確率設定（WebUIから変更可・NVS永続化）
// saveConfig()/loadConfig() で使うため、Preferences より前に定義する。
// ====================================================
int cfg_mutterChance     = 10;  // 独り言発生確率ラベル値 (0/5/10/20/30)。10=標準（0.2%/s 相当）
int cfg_mutterFaceChance = 20;  // 呟き時の顔出現確率 (%, 0/10/20/25/50/100)
int cfg_idleChance       = 2;   // IDLE動作頻度 (0=OFF / 1=少なめ / 2=標準 / 3=多め)

// ====================================================
// 📊 Visualizer Framework — モード定義（v1.0 / 2026-07-21）
//
// 目的：
//   「グライコON/OFF」という2値設定を、今後Visualizerを増やせる
//   モード選択型の共通設定へ拡張する。
//
// 設計方針：
//   ・設定は入力元（Wi-Fi / LINE IN / 将来のBluetooth）から完全に独立。
//     どの入力元でも同じ cfg_visualizerMode と同じ描画コードを使う。
//   ・ここ（メタ情報）は FFT_DISPLAY_TEST の有無に関わらず常にコンパイルされる。
//     WebUIはこのテーブルだけを見るため、描画側の #ifdef と切り離せる。
//   ・描画関数テーブル（VIZ_RENDER_FN[] / VIZ_INTERVAL_MS[]）は
//     ファイル末尾の Visualizer Framework 本体側（#ifdef FFT_DISPLAY_TEST 内）にある。
//
// 【Visualizerを追加する手順】
//   1. 下の enum VisualizerMode に新しい値を VIZ_MODE_COUNT の直前へ追加
//   2. 下の VIZ_MODES[] に {id, label, note} を同じ並び順で追加
//   3. ファイル末尾で void vizRenderXxx(const AudioVizState&, bool) を実装
//   4. VIZ_RENDER_FN[] / VIZ_INTERVAL_MS[] へ同じ並び順で追加
//   → 既存Visualizerのコードには一切触れずに追加できる。
// ====================================================
enum VisualizerMode : uint8_t {
  VIZ_MODE_OFF   = 0,   // 表示しない（通常の顔）
  VIZ_MODE_EQ    = 1,   // Graphic EQ（Classic）＝従来のグライコフェイス
  VIZ_MODE_HALO  = 2,   // Audio Halo（顔を囲む放射スペクトラム）
  VIZ_MODE_MIRROR= 3,   // Mirror Wave（上下対称ネオンリボン波形）
  VIZ_MODE_RHYTHM= 4,   // 8-Lane Rhythm（8バンドFFTをそのまま流す音ゲー風の流れる譜面）
  VIZ_MODE_KALEIDO=5,   // Kaleidoscope（万華鏡・6分割の頂点鏡映方式）
  VIZ_MODE_AVU   = 6,   // Analog VU（8バンドFFTを8個のアナログVUメーターへ / 4×2配置）
  VIZ_MODE_BLOCKS= 7,   // Mega Blocks（大型落ち物ブロック。8-Lane Rhythmとは無関係の独立モード。仮称）
  VIZ_MODE_COUNT
};

// NVSの vizMode キーが「未保存」であることを表す番兵値（旧設定からの移行判定に使う）
static const uint8_t VIZ_MODE_UNSET = 255;

struct VizModeInfo {
  const char* id;     // URL / ログ用の短い識別子
  const char* label;  // WebUI表示名
  const char* note;   // WebUI説明文
};

// enum VisualizerMode と必ず同じ並び順にすること
const VizModeInfo VIZ_MODES[VIZ_MODE_COUNT] = {
  { "off",  "OFF",
    "ビジュアライザーを表示しません。通常の顔表示のままです。" },
  { "eq",   "Graphic EQ（Classic）",
    "従来のグライコフェイス。顔に縦バーを重ねて表示します（左＝低音／右＝高音）。バーが顔を侵食する既存の演出そのままです。" },
  { "halo", "Audio Halo",
    "顔を囲む楕円から画面外周へ向かって、48方向のスペクトラムが広がります。各方向の先端が滑らかにつながり、顔の周囲を巨大な波形の王冠が取り囲みます。真下＝低音／真上＝高音の左右対称。オートゲインにより小〜中音量でも常に大きく展開し、表情と口パクは常に最前面に表示されます。" },
  { "mirror", "Mirror Wave",
    "8バンドFFTを横方向へ補間し、上下対称のネオンリボン状波形として画面いっぱいに描きます。線ではなく太いリボン＋ネオングローで、見た瞬間に『音が動いている』と分かります。帯域と音量で色相がシアン→青→紫→マゼンタ→黄へ流れます。Lightingの上へ重ね描きでき、顔は最前面。上端48pxの情報パネルには侵入しません。" },
  { "rhythm", "8-Lane Rhythm",
    "Graphic EQ（Classic）の8バンド表示を、そのまま時間履歴として上から下へ流す上位互換Visualizerです。Bandの色・ゲイン・白背景・顔はGraphic EQと同じものを使用し、独立したゲーム画面ではありません。横軸＝周波数8バンド、縦軸＝時間として、曲の強弱がそのまま縞模様のように流れます。ピーク検出や正解判定は行いません。表示領域はGraphic EQ等と同じく上部48pxを除く画面下側で、センサー・バッテリー・IP表示はこれまでどおり常時表示されます（他のVisualizer・Lightingには影響しません）。" },
  { "kaleido", "Kaleidoscope",
    "実物の万華鏡のような、6分割の回転対称・鏡映対称模様を描くビジュアライザーです。点・短い線・小さな三角形・リングなど最大12個のシンプルな図形を、60度ごとの回転と鏡映で12方向へ複製し、黒背景にネオン／宝石調の色鮮やかな幾何学模様を作ります。低音のバンドは中心寄り、高音のバンドは外周寄りに図形を配置し、音量の滑らかな変化で全体がゆっくり回転速度を変え、無音時も止まらずゆっくり回り続けます。低域の急な立ち上がりで新しい図形が一瞬明るく生成されます。ピクセル単位の重い三角関数は使わず、初期化時に求めた6方向の回転定数と少数の図形座標だけで模様を作るため負荷は軽めです。顔・上部48pxの扱いは他のVisualizerと同じです。" },
  { "avu", "Analog VU",
    "1970〜80年代のオーディオアンプ／ミキサーに並んでいたアナログVUメーターを、左右2chではなく『8バンドFFTそれぞれに1個ずつ』割り当てた8連アナログ・スペクトラムメーターです。上部48pxを除く画面へ4個×2段で配置し、上段左からband0〜band3、下段左からband4〜band7＝左上から右下へ低域→高域になります。クリーム色の盤面・黒い主目盛り・右端の赤いピークゾーン・赤い針・濃色の外枠という実機的な意匠で、背景は黒い機器パネル調です。8本の針は完全に独立して振れ、低音の強い曲は左上側、ボーカル中心は中央寄り、ハイハット等は右下側がよく動きます。針は立ち上がりに素早く追従し、下降時だけ機械式メーターらしい慣性でゆっくり戻ります。各バンドの絶対値と8バンド内での相対的な強弱の両方を使うため、小音量でも帯域ごとの違いが残り、音量を上げれば全体の振れ幅も大きくなります。顔・上部48pxの扱いは他のVisualizerと同じです。" },
  { "blocks", "Tetromino Dance",
    "落ち物パズルを連想させる大型ブロックが、かりポムの顔の周囲を少数（最大4個）落下・回転・移動するビジュアライザーです。8-Lane Rhythmのような固定8レーンの縦流しではなく、各ブロックが画面内を独立して自由に動きます。ブロックの一辺は目の直径・口の幅と同程度の大きさで、細かい粒を大量に流す表現にはしていません。回転は瞬間切り替えではなく、ブロック中心を軸に数フレームかけてクルッと回るアニメーションです。普段はまっすぐ落下し、ときどき数フレームかけて真横へ滑るように移動してからまた落下する、落ち物パズル特有の動きを再現しています。音の強さで落下速度が、低音の立ち上がりで回転・真横移動の発生が変化します。FFTの8バンドを画面の8列へ直接対応させる方式は採用していません。下端に到達したブロックは積み上がらず消え、新しいブロックが上部から出現します。他の白背景Visualizerと同じ白背景の上にブロックを描いた後、既存のdrawVisualizerFaceParts()を最後に描くため、かりポムの顔は常にブロックの手前に表示されます。" },
};

// 現在のVisualizerモード（Wi-Fi / LINE IN / 将来のBluetooth 共通・NVS永続化）
VisualizerMode cfg_visualizerMode = VIZ_MODE_EQ;  // 既定：従来どおり Graphic EQ

// 旧設定との互換用フラグ。cfg_visualizerMode != OFF と常に同期させ、
// 旧NVSキー(udpVisiFace)・旧URL(/udp_visualizer_face)を生かしたまま残す。
// （旧ファームへ戻した場合でも「ONだった」情報が失われないようにするため）
bool cfg_udpVisualizerFace = true;  // グライコフェイス（UDP時のFFTバー表示）デフォルトON

// ====================================================
// 🎲 Audio Visualizer Random（v1.0 / 2026-07-27）
//
// 既存のVisualizer選択（cfg_visualizerMode・1つだけ選ぶ方式）はそのまま使い、
// 「選び方」だけを増やす追加機能。描画ロジック・候補一覧(VIZ_MODES[])・
// isVisualizerFaceEnabled()（MIC時は表示しない既存判定）には一切手を入れない。
//
// ・cfg_visualizerMode        … 現在実際に描画へ使われる値（従来どおり）
// ・cfg_vizManualMode         … ユーザーが最後に「手動選択」した値（NVS "vizMode" に保存）
//     Random ON中は cfg_visualizerMode だけが自動で変わり、
//     cfg_vizManualMode は変更しない＝Randomの一時的な選択を手動選択として誤保存しない。
// ・Random OFF（手動選択 or トグルOFF）で cfg_visualizerMode を cfg_vizManualMode へ戻す。
// ====================================================
bool     cfg_vizRandomOn            = false;  // NVS "vizRndOn"
uint8_t  cfg_vizRandomIntervalMin   = 5;       // NVS "vizRndMin"（5/10/15のみ。既定5分）
VisualizerMode cfg_vizManualMode    = VIZ_MODE_EQ;  // NVS "vizMode"（ユーザーの手動選択のみ）
unsigned long  vizRandomLastSwitchMs = 0;      // 0=まだ一度も選んでいない（ON直後/起動直後は即選択）
int8_t   vizRandomLastPick          = -1;      // 直前に選んだVIZ_MODE_*（連続回避用）

// ====================================================
// 💡 Lighting Framework — モード定義（v1.0 / 2026-07-22）
//
// Visualizer Framework とは【別カテゴリ】の背景照明。
//   Layer0 Lighting（背景演出・複数同時ON可）
//     ↓
//   Layer1 Visualizer（音の可視化・1つ選択）
//     ↓
//   Layer2 顔（目・鼻・口・まつ毛）
//
// ・Lighting は「複数同時ON」を前提とするためビットマスクで保持する。
// ・FFTは照明を動かす“きっかけ”として使う（測定器ではない）。
// ・描画関数テーブル(LIGHT_RENDER_FN[])は #ifdef FFT_DISPLAY_TEST 側にある。
//   ここ（メタ情報）はWebUI/NVSが参照するため常にコンパイルされる。
//
// 【Lightingを追加する手順（Visualizerと同じ4ステップ）】
//   1. 下の enum LightingMode に LIGHT_XXX を追加（LIGHT_MODE_COUNT の直前）
//   2. 下の LIGHT_MODES[] に {id,label,note} を同じ並び順で追加
//   3. #ifdef側で void lightRenderXxx(bool needsInit,bool fullRepaint) を実装
//   4. #ifdef側の LIGHT_RENDER_FN[] へ同じ並び順で追加
//   → WebUI・NVS・合成処理は変更不要。
// ====================================================
enum LightingMode : uint8_t {
  LIGHT_DISCO  = 0,   // Disco Floor（背景）
  LIGHT_LASER  = 1,   // Laser Show（オーバーレイ）
  LIGHT_AURORA = 2,   // Aurora（背景）
  LIGHT_MATRIX = 3,   // Matrix（背景）
  LIGHT_RACE    = 4,  // Retro Race（背景・Pole Position風スクリーンセーバー）
  LIGHT_SKYRAID = 5,  // Sky Raid（背景・Xevious風縦スクロールスクリーンセーバー）
  LIGHT_EYESLOT = 6,  // Eye Slot（背景・黒目がスロットリールに変わる演出）
  LIGHT_CLASSICRACE = 7,  // Classic Race（背景・1970年代後半風トップビューレースデモ）
  LIGHT_ASTEROID = 8,     // Asteroid Field（背景・ワイヤーフレーム隕石が漂う宇宙空間演出）
  LIGHT_TUNNEL = 9,       // Tempest Tunnel（背景・中央へ吸い込まれるワイヤーフレームトンネル演出）
  LIGHT_PACMAN = 10,          // PAC-MAN Arcade（背景・Retro Game Lighting 第1弾 1/3）
  LIGHT_STREETFIGHTER = 11,   // Fighter Duel（背景・Retro Game Lighting 第1弾 2/3）
  LIGHT_MARIO = 12,           // 8-Bit Runner（背景・Retro Game Lighting 第1弾 3/3）
  LIGHT_MISSILE = 13,         // Missile Defense（背景・Laser Showの発展形。自動迎撃デモ）
  LIGHT_PSYCHE  = 14,         // Psychedelic / Trance（背景・FLASH/MOTION/ACCENTの三層モンタージュ）
  LIGHT_VORTEX  = 15,         // Hypnotic Vortex（背景・中心へ向かってテーパーする6本の太い螺旋アームがゆっくり実回転）
  // 2026-07-30〜31: Optical Illusionシリーズとして Expanding Hole / Moire Breathing /
  //   Hypnotic Vortex の3種を試作したが、実機評価の結果 Hypnotic Vortex のみ正式採用。
  //   Expanding Hole / Moire Breathingは不採用となり、関連コードは全て削除済み
  //   （enum値も再利用しないよう欠番にはせず、LIGHT_VORTEXをそのままbit15に詰めた）。
  LIGHT_AQUARIUM = 16,        // Aquarium（背景・特定作品の模倣ではないKariPom独自表現の水槽風スクリーンセーバー）
  LIGHT_FLYINGPOMPADOUR = 17, // Flying Pompadour（背景・1990年代スクリーンセーバーへのオマージュ。著作物のデザインは使わずKariPom独自の飛行体で表現）
  LIGHT_RAINBOWWASHER = 18,   // Rainbow Washing Machine（背景・中心から放射状に広がる多数の高彩度三角片が洗濯機の脱水サイクル風に速度・方向を変えながら回転）
  LIGHT_PIXELINVASION = 19,   // Pixel Invasion（背景・1970年代末〜80年代初頭の固定画面シューティングへのオマージュ。黒背景に紫/水色/緑のオリジナルドット絵敵編隊・自機・赤いシールド・UFOが自動で動き続ける）
  // ↓ 将来ここへ追加（Defender / Scramble / Pong / Block Breaker / Fire / Neon …）
  // cfg_lightingMask は uint32_t（今後のLighting追加を見込んで正式採用。bit31=32番目まで拡張余地あり）。
  LIGHT_MODE_COUNT
};

struct LightModeInfo { const char* id; const char* label; const char* note; };

// enum LightingMode と必ず同じ並び順にすること
const LightModeInfo LIGHT_MODES[LIGHT_MODE_COUNT] = {
  { "disco", "Disco Floor",
    "1970年代のディスコフロア。画面全体が大きなLEDタイルになり、音楽に合わせて虹色に流れ、ビートでフロア全体がフラッシュします。白い顔は描かず、光る床の上に目・鼻・口だけが浮かびます。Visualizerと重ねて使えます。" },
  { "laser", "Laser Show",
    "暗い会場を横切る蛍光グリーンのレーザービーム。四隅からの交差、扇状の走査、左右のすれ違い、ビートで一瞬現れる大きなX字など5演出が自動で切り替わります。中心が白く光る疑似グローで、細い針にはなりません。あえて緑1色で“レーザーらしさ”を出しています。Brightness設定の影響を受けず常に鮮やか（Brightnessを下げるとDiscoの床は暗くなり、緑ビームがより際立ちます）。Disco FloorやVisualizerと重ねられます（Disco Floorより前面／Visualizerより背面）。" },
  { "aurora", "Aurora",
    "画面全体にやわらかいオーロラの光幕。青緑〜シアン〜紫ピンクのカーテンが横に波打ちながらゆっくり流れます。Discoの派手さと対照的な癒し系で、無音時もゆっくり漂う待機演出になります。背景（面）演出なのでLaserやVisualizerを重ねられます（Disco Floorと同じ背景レイヤーで、両方ONの時は後から選んだ方が表示されます）。Brightness設定が効きます。" },
  { "matrix", "Matrix",
    "黒背景に蛍光グリーンの太い縦カラムが落下する未来的・サイバーな背景。大きな発光セルで構成し、先頭が白緑に光ります。音量で流れる速度が変化し、低音で一瞬加速＋シアンの横スキャン線が一閃、高音でランダムなスパーク。Laser（緑ビーム）を重ねると雨をレーザーが切り裂く演出に。上端は黒テーマ。Brightness設定が効きます。" },
  { "race", "Retro Race",
    "MSXやPC-6001へ無理に移植したような、カクカクした低解像度のPole Position風レースデモ。プレイヤー操作なし・ゲームオーバーなしで、自車が道路のカーブや前方車両を自動で避けながら永遠に走り続けるスクリーンセーバーです。道路の曲がり方や対向車の出現間隔・追い越しはランダムで、毎回少し違う展開になります。かりポムの黒目がカーブや前方車両の方向を追い、「自分で運転している」ように見えるのが特徴。Brightness設定が効きます。" },
  { "skyraid", "Sky Raid",
    "MSXやPC-6001へ無理に移植したような、カクカクした低解像度のXevious風縦スクロールスクリーンセーバー（Retro Arcadeシリーズ第2弾）。プレイヤー操作なし・ゲームオーバーなしで、自機が編隊を組まずばらけて現れる敵をよけながら自動で飛び続け、自動ショットで時おり敵を撃墜します（小さな爆発演出つき）。背景は森・草地・道路・川・基地・滑走路などをランダムに組み合わせ、毎回少し違う地形になります。かりポムの黒目は手前の敵ではなく画面上部＝飛行方向を見ており、「前方空域を見ながら飛んでいる」ように見えるのが特徴。Brightness設定が効きます。" },
  { "eyeslot", "Eye Slot",
    "かりポムの顔はそのままに、左右の黒目だけが小さなスロットリール（2リール）に変わる演出です。上から絵柄が流れてきて中央のラインへ吸着するように停止し、左リールが止まった少し後に右リールが止まる、期待感のある止まり方をします。絵柄が揃うと控えめなフラッシュが入ります。ゲームではなく眺めて楽しむデモで、自動で回転→停止→結果表示を延々と繰り返します。鼻・口・まゆ毛など黒目以外の顔は通常どおりです。Brightness設定が効きます。" },
  { "classicrace", "Classic Race",
    "1970年代後半のアーケードレースゲームを思わせる、超レトロな真上視点（トップビュー）のレースデモです。Retro Race（疑似3D視点）とは方向性が異なり、擬似遠近感を使わず画面中央の道路がゆっくり左右に蛇行するだけのミニマルな作り。プレイヤー操作・ゲームオーバー・スコア・クラッシュ・爆発は一切無く、シンプルなF1風の自車がコーナーへ自然に合わせて走り、数台の敵車をゆるく避けながら永遠に走り続けるスクリーンセーバーです。時々チェッカーラインやオイル染みが流れます。かりポムの黒目は道路の先ではなく次のコーナーの方向をごく小さく見ており、落ち着いた視線が特徴。Brightness設定が効きます。" },
  { "asteroid", "Asteroid Field",
    "1980年前後のベクターゲームを思わせる、真っ黒な宇宙空間にワイヤーフレームの隕石が静かに漂うアート系スクリーンセーバーです。ゲームではなく、10〜15個の不規則な多角形の隕石がそれぞれ異なる方向・速度・回転でゆっくり漂い、画面端まで来ると反対側から自然に現れます（ラップアラウンド）。隕石はネオンピンク・ネオングリーン・ネオンイエローの塗りなし輪郭線のみで描かれ、背景には控えめな星が瞬きます。かりポムの黒目は隕石を追わず、通常のLighting Galleryと同じ自然な動きのままです。Brightness設定が効きます。" },
  { "tunnel", "Tempest Tunnel",
    "1981年のアーケードゲーム『Tempest』へのオマージュ作品です。Tempestの象徴であるワイヤーフレームのトンネルだけをモチーフにしたベクターアート系スクリーンセーバーで、自機・敵・弾・スコアなどのゲーム要素は一切無く、8〜16角形の多角形リングが画面中央へゆっくり吸い込まれるように流れ、全体がゆるやかに回転しながら軽く脈動します。線はネオンカラーの塗りなし輪郭線のみで、色相もゆっくり変化し続けます。未来的な万華鏡のような、眺めているだけで心地よい演出です。かりポムの黒目は通常のLighting Galleryと同じ自然な動きのままです。Brightness設定が効きます。" },
  { "pacman", "PAC-MAN Arcade",
    "1980年代のドットイート迷路アクションをモチーフにした自動デモです（実在ゲームのマップ・画像・スプライトは使用せず、独自デザインの迷路です）。黄色い自機が外周ループ通路を自動で巡回してドットを食べ進み、4隅のパワーエサが点滅、3体の色違いオバケがあらかじめ決めた安全な経路（外周の別位相／中央通路の往復）だけを移動します。プレイヤー操作・当たり判定・スコアは無く、ドットを食べ尽くす、または一定時間ごとに自動で復活し無限に周回し続けます。表示領域は他のLighting同様、上部48pxの情報パネルを除く画面下側です。Brightness設定が効きます。" },
  { "streetfighter", "Fighter Duel",
    "1990年代前半の対戦格闘ゲームをモチーフにした自動デモです。頭・胴・腕・脚を持つブロック体型の2人のファイターが横向きのステージで、構え・前後移動・パンチ・キック・ジャンプ・しゃがみ・被弾のけぞりを自動で繰り返し、時おり飛び道具を撃ち合います。体力ゲージは時間経過と被弾で減り続け、0になると『K.O.』を表示して数秒後に体力・位置をリセットし次のラウンドへ進みます。勝敗はそのつど変わり、固定の勝者はいません。プレイヤー操作・厳密な判定・成績記録は行いません。表示領域は他のLighting同様、上部48pxの情報パネルを除く画面下側です。Brightness設定が効きます。" },
  { "mario", "8-Bit Runner",
    "横スクロールアクションをモチーフにした自動デモです（実在ゲームの画像・ロゴ・キャラクターデータは使用せず、赤い帽子と青いオーバーオールのジェネリックな8bit風ランナーです）。プレイヤー自身ではなく背景・地面・ブロック・土管・雲がスクロールして流れることで前進感を出し、ランナーは自動で敵や土管を飛び越え、アイテムブロックを下から叩いてコインを獲得しながら走り続けます。一定距離を進むとステージが自然にリセットされ、また最初から流れ始めます。プレイヤー操作・ゲームオーバー・スコア判定は行いません。表示領域は他のLighting同様、上部48pxの情報パネルを除く画面下側です。Brightness設定が効きます。" },
  { "missile", "Missile Defense",
    "1980年代のミサイル迎撃アーケードゲームを連想させる自動迎撃デモです（実在ゲームの再現ではなく、Laser Showの発展形という位置づけです）。黒い空と黄色い地面、地上3ヶ所の小さな基地の上を、上空から飛来する敵ミサイル2〜3本が軌跡を伸ばしながら降下します。1つの照準が現在の標的へ滑らかに追尾してロックオンすると、3基地のいずれかから迎撃レーザーが発射され、命中した敵ミサイルは軌跡ごと消えて爆発・煙が残り、徐々に消えていきます。照準はすぐ次の標的へ移り、迎撃した分は新しい敵ミサイルが補充されるため、防空戦が絶えず続いているように見えます。プレイヤー操作・弾薬数・勝敗・ゲームオーバーはありません。表示領域は他のLighting同様、上部48pxの情報パネルを除く画面下側です。Brightness設定が効きます。" },
  { "psyche", "Psychedelic / Trance",
    "1960〜70年代のサイケデリック映像とトランスのVJを思わせる、強烈で予測不能な抽象映像です。眺める演出ではなく『目に飛び込んでくる』ことを狙っています。内部は3つの層でできています。MOTION（0.72〜1.44秒）は動きを目で追える主役で、色とりどりの輪が内側から次々生まれて迫りながら画面外へ抜けるExpanding Rings、回転しながら中心が移動し途中で突然逆回転する偏心スパイラル、2つの焦点がすれ違って干渉縞が崩壊・再生するモアレ、頂点が画面を横断しながら高速回転する不規則放射ウェッジ、回転しながら加速して飛び散る破片三角の5種類が、必ず全種類登場するよう順不同で回ります。FLASH（90〜270ms）は歪んだОPグリッド・巨大な不均一ドット・斜め細線・うねりストライプを静止の一撃として2〜5連射し、そのたびに配色世界が丸ごと入れ替わります。ACCENT（90〜180ms）はMOTIONの途中へ突然割り込み、補色反転・色面スタブ・画面を占有する巨大図形・スキャンライン断裂、そしてFace Galleryの顔が一瞬だけ映り込みます。割り込み中もMOTIONは裏で進み続けるため、戻ってきたときには渦が飛んでいます。MOTIONは終盤ほど加速し、最も盛り上がった瞬間に断ち切られて次の世界へ落ちます。対称や規則正しさは意図的に避け、中心の偏り・不均一な間隔・極端な大小・歪みで落ち着かなさを作っています。音声には一切反応せず、演出単体で成立します。表示領域は他のLighting同様、上部48pxの情報パネルを除く画面下側で、顔は常に最前面です。Brightness設定が効きます。" },
  { "vortex", "Hypnotic Vortex",
    "中心へ向かってテーパーする太い螺旋アーム6本（黒3本＋白3本が完全交互）を持つ、催眠の渦巻きを思わせる演出です。実際の回転はゆっくりですが、渦の分厚さと中心へのすぼまり方によって『中心へ吸い込まれる』『奥へ落ちていく』ように感じられます。中央が黒く塗りつぶされたワイヤーフレームのTempest Tunnel（既存Lighting）とは異なり、太い塗りつぶし面が画面の大部分を占める点が特徴です。白黒のみのシンプルな配色です。表示領域は他のLighting同様、上部48pxの情報パネルを除く画面下側で、顔は常に最前面です。Brightness設定が効きます。" },
  { "aquarium", "Aquarium",
    "往年の水槽スクリーンセーバーを思わせる、眺めていて心地よい水中Lightingです（特定ソフトウェアの画像・デザインを再現したものではなく、KariPom独自の表現です）。上ほど淡く下ほど沈む水の濃淡と、ゆっくり漂う淡い光の筋で深い水中を感じさせます。体色・体形・ヒレ・模様の異なる数種類の魚が3〜6匹、大きさ・泳ぐ高さ・速度をそれぞれ変えて左右どちらの向きにも泳ぎ、尾びれと各ヒレは常に小さくはためいて生きて泳いでいるように見えます。画面外へ抜けた魚は少し間を置いてから自然なタイミングで再登場します。下部には岩・水草・砂地を控えめに配置し、気泡が下から上へゆっくり立ちのぼります。動きはせわしなくせず、ぼんやり眺めていられる速度感を重視しています。Brightness設定が効きます。" },
  { "flyingpompadour", "Flying Pompadour",
    "1990年代の名作スクリーンセーバーへのオマージュ演出です（元作品の画像・デザインはそのまま使用せず、羽の生えたKariPomらしい小型飛行体をモチーフにしたKariPom独自のデザインです）。夕暮れから夜へ沈む空を背景に、丸いフォルムとポンパドール状のシルエットを持つ小さな飛行体が斜め方向に画面を横切ります。複数体が一定間隔ではなくランダムなタイミングで現れ、大きさ・速度の異なる奥と手前の2層で奥行きを表現します。羽は4コマの羽ばたきアニメーションで動き、本体には陰影とハイライトを付けて立体感を出しています。見た瞬間にあの雰囲気を思わせつつも、グラフィックは完全にオリジナルです。Brightness設定が効きます。" },
  { "rainbowwasher", "Rainbow Washing Machine",
    "画面中央の消失点を軸に、赤・橙・黄・黄緑・緑・シアン・青・紫・マゼンタなど彩度の高い三角形・くさび形の色片が510個、中心付近の小さな点として次々に生まれては外側へ実際に移動しながらだんだん大きくなり、最外周に達すると消えてまた中心付近から生まれ直す、極彩色のトンネルのようなLightingです。半径を5つの帯に分け、内側から外側へ帯ごとに回転方向・速度を変えている（隣り合う帯が互いに逆方向へ回る）ため、複数の渦が重なって滑っていくように見えます。Hypnotic Vortexの単色6分割ウェッジが1枚岩として一定速度で回るのとは異なり、色片の数・中心から外周への移動・帯ごとの逆回転のいずれの点でも別物です。加速・停止・反転を伴う洗濯機の脱水サイクルのような動きではなく、回転は止まることなく常時継続する、トランス感のある演出です。Psychedelic / Tranceのような視覚モチーフの切替や点滅も行いません。Brightness設定が効きます。" },
  { "pixelinvasion", "Pixel Invasion",
    "1970年代末〜1980年代初頭のカラー化された固定画面シューティングゲームの雰囲気を思わせる、レトロアーケード風の自動アニメーションLightingです（実在ゲームのスプライトデータは一切使用せず、上段マゼンタ・中段ターコイズ・下段グリーンに塗り分けたKariPom独自のドット絵敵編隊です）。背景は完全な黒一色で、星や背景スクロールなどの宇宙演出は入れていません。5段の敵編隊が数ピクセル単位でカッ、カッ、カッと左右へ移動し、端に達すると方向転換して少し下降します。画面下部ではオリジナルデザインの自機が自動で左右往復しながら時々弾を発射し、敵側も時おり下方向へ弾を撃ち返します。自機と敵編隊の間には赤いピクセルアートのシールドが4つあり、弾が当たった場所から少しずつ欠けていきます。ときどき画面上部をUFOが横切ります。スコアや残機、GAME OVERの概念は無く、編隊が減ったり画面下に近づいたりすると新しい編隊とシールドへ自然に切り替わり、眺めている間ずっとアニメーションが続きます。Brightness設定が効きます。" },
};

// Lightingの複数選択状態（ビットマスク・NVS "lightMask" に保存）。0=全OFF。
// 2026-07-25: Asteroid Field(LIGHT_ASTEROID=8)追加に伴い uint8_t → uint16_t へ拡張。
//   uint8_tは8bit幅のためビット位置0〜7（Disco〜Classic Race）までしか表現できず、
//   ビット位置8（Asteroid Field）が (uint8_t)(1u<<8)=0 に切り捨てられ、選択操作が
//   常に no-op になっていた（実機不具合の直接原因）。uint16_tなら最大16モードまで
//   拡張余地があり、既存ビット0〜7の意味・値は一切変更しないため後方互換。
// 2026-07-30: Hypnotic Vortex追加検討時点でLIGHT_MODE_COUNTがuint16_tの上限
//   （最大16モード＝bit0〜15）に達する見込みとなったため uint16_t → uint32_t へ
//   再拡張した。試作した3モードのうちExpanding Hole/Moire Breathingは実機評価で
//   不採用となりLIGHT_VORTEX=15の1つだけが残ったが（2026-07-31）、今後のLighting
//   追加を見込んでuint32_t仕様はそのまま正式採用とする（16bitへは戻さない）。
//   既存ビット0〜14の意味・値は一切変更しないため後方互換（NVS読込側も3段
//   フォールバック(getUInt→getUShort→getUChar)で対応。loadConfig参照）。
uint32_t cfg_lightingMask = 0;

// ====================================================
// 🎲 Lighting Random（v1.0 / 2026-07-27）
//
// 既存のLighting選択（cfg_lightingMask・複数同時ONが可能なビットマスク）は
// そのまま使い、「選び方」だけを増やす追加機能。各Lightingの描画内容・
// ビットマスクの意味・背景は最後に選んだ1つが採用される既存の合成ルールには
// 一切手を入れない（Random中も内部的には「1ビットだけ立てる」という形で
// 既存の合成ルールに従う）。
//
// ・cfg_lightingMask       … 現在実際に描画へ使われるマスク（従来どおり）
// ・cfg_lightingManualMask … ユーザーが最後に「手動選択」した状態のマスク
//     （NVS "lightMask" に保存。Random ON中は変更しない＝Randomの一時的な
//      選択を手動選択として誤保存しない）
// ・Random OFF（手動選択 or トグルOFF）で cfg_lightingMask を
//   cfg_lightingManualMask へ戻す。
// ====================================================
bool     cfg_lightingRandomOn          = false;  // NVS "lightRndOn"
uint8_t  cfg_lightingRandomIntervalMin = 5;        // NVS "lightRndMin"（5/10/15のみ。既定5分）
uint32_t cfg_lightingManualMask        = 0;        // NVS "lightMask"（ユーザーの手動選択のみ。2026-07-30: uint16_t→uint32_t）
unsigned long lightingRandomLastSwitchMs = 0;      // 0=まだ一度も選んでいない（ON直後/起動直後は即選択）
int8_t   lightingRandomLastPick        = -1;       // 直前に選んだLIGHT_*（連続回避用）

// ====================================================
// Lighting のレイヤー種別 と 上端パネル・ヘッダーテーマ（v1.6）
//
// 各Lightingが「面(背景)/ビーム(オーバーレイ)」と「上端パネルを白/黒どちらで
// 表示すると読みやすいか(HEADER_LIGHT/HEADER_DARK)」を自己申告する。
// コンポジタが実際に採用した背景Lightingのヘッダーテーマを上端パネルへ渡すので、
// 「LightingがONだから黒」ではなく「今表示中の背景に最適なテーマ」を選べる。
// → 今後 Fire / Neon / Matrix 等を足すときも、この表に1行足すだけで拡張できる。
// ここは showSensors()/drawBattery()（ファイル前半）から参照するため早期に定義する。
// ====================================================
#define LIGHT_LAYER_BG   0    // 面（背景）
#define LIGHT_LAYER_OVL  1    // オーバーレイ（ビーム等）
#define HEADER_LIGHT     0    // 上端パネル＝白背景・通常文字色
#define HEADER_DARK      1    // 上端パネル＝黒背景・明色文字

// enum LightingMode と同じ並び。追加時はここへ1行ずつ足す。
const uint8_t LIGHT_LAYER[LIGHT_MODE_COUNT] = {
  LIGHT_LAYER_BG,    // LIGHT_DISCO  … 面
  LIGHT_LAYER_OVL,   // LIGHT_LASER  … ビーム
  LIGHT_LAYER_BG,    // LIGHT_AURORA … 面
  LIGHT_LAYER_BG,    // LIGHT_MATRIX … 面
  LIGHT_LAYER_BG,    // LIGHT_RACE   … 面
  LIGHT_LAYER_BG,    // LIGHT_SKYRAID … 面
  LIGHT_LAYER_BG,    // LIGHT_EYESLOT … 面（実体は通常の白い顔＋黒目部分のみリール）
  LIGHT_LAYER_BG,    // LIGHT_CLASSICRACE … 面
  LIGHT_LAYER_BG,    // LIGHT_ASTEROID … 面
  LIGHT_LAYER_BG,    // LIGHT_TUNNEL … 面
  LIGHT_LAYER_BG,    // LIGHT_PACMAN … 面
  LIGHT_LAYER_BG,    // LIGHT_STREETFIGHTER … 面
  LIGHT_LAYER_BG,    // LIGHT_MARIO … 面
  LIGHT_LAYER_BG,    // LIGHT_MISSILE … 面
  LIGHT_LAYER_BG,    // LIGHT_PSYCHE  … 面
  LIGHT_LAYER_BG,    // LIGHT_VORTEX  … 面
  LIGHT_LAYER_BG,    // LIGHT_AQUARIUM … 面
  LIGHT_LAYER_BG,    // LIGHT_FLYINGPOMPADOUR … 面
  LIGHT_LAYER_BG,    // LIGHT_RAINBOWWASHER … 面
  LIGHT_LAYER_BG,    // LIGHT_PIXELINVASION … 面
};
const uint8_t LIGHT_HEADER[LIGHT_MODE_COUNT] = {
  HEADER_LIGHT,      // LIGHT_DISCO  … 明るい原色の床 → 上端は白背景が読みやすい
  HEADER_DARK,       // LIGHT_LASER  … （オーバーレイ。背景採用時の想定値）
  HEADER_DARK,       // LIGHT_AURORA … 暗い夜空 → 黒
  HEADER_DARK,       // LIGHT_MATRIX … 黒背景 → 黒
  HEADER_LIGHT,      // LIGHT_RACE   … 明るい空色の空 → 上端は白背景が読みやすい
  HEADER_LIGHT,      // LIGHT_SKYRAID … 明るめの地表色 → 上端は白背景が読みやすい
  HEADER_LIGHT,      // LIGHT_EYESLOT … 通常の白い顔 → 上端は白背景が読みやすい
  HEADER_LIGHT,      // LIGHT_CLASSICRACE … 明るい草地の緑 → 上端は白背景が読みやすい
  HEADER_DARK,       // LIGHT_ASTEROID … 真っ黒な宇宙空間 → 黒
  HEADER_DARK,       // LIGHT_TUNNEL … 真っ黒な背景 → 黒
  HEADER_DARK,       // LIGHT_PACMAN … 黒背景の迷路 → 黒
  HEADER_LIGHT,      // LIGHT_STREETFIGHTER … 明るい空色のステージ → 白
  HEADER_LIGHT,      // LIGHT_MARIO … 明るい青空 → 白
  HEADER_DARK,       // LIGHT_MISSILE … 黒い空が大部分を占める → 黒
  // Psychedelicは90msごとに配色世界が変わるが、ヘッダーテーマは【固定】にする。
  // ショットごとに白/黒を切り替えると lightingHeaderDark() の変化検出が毎フレーム
  // 発火し、上端の情報パネル（センサー・IP・電池）が点滅してしまうため。
  // 暗い背景／高彩度背景のどちらでも黒帯＋明色文字が最も読めるので DARK で固定する。
  HEADER_DARK,       // LIGHT_PSYCHE  … 配色は激変するがヘッダーは黒固定
  HEADER_LIGHT,      // LIGHT_VORTEX  … 白背景に黒い螺旋アーム → 上端は白背景が読みやすい
  HEADER_DARK,       // LIGHT_AQUARIUM … 深い水中の暗めの青緑背景 → 黒
  HEADER_DARK,       // LIGHT_FLYINGPOMPADOUR … 夕暮れ〜夜空の暗い背景 → 黒
  HEADER_DARK,       // LIGHT_RAINBOWWASHER … 黒背景に高彩度の色片 → 黒
  HEADER_DARK,       // LIGHT_PIXELINVASION … 完全な黒背景 → 黒
};

// 上端パネルを黒テーマにすべきか（採用中の背景Lightingのヘッダーに従う）。
//   ・面(背景)が採用されていればそのモードの LIGHT_HEADER を使う
//     （Disco と Aurora 両方ON時は後勝ち＝Aurora が採用され HEADER_DARK）
//   ・面が無くオーバーレイ(Laser)だけの時は暗い会場なので黒
//   ・Lighting全OFF は呼び出し側(lightingScreenActive)でガードされ白になる
bool lightingHeaderDark() {
  int bg = -1;
  for (uint8_t li = 0; li < (uint8_t)LIGHT_MODE_COUNT; li++) {
    if ((cfg_lightingMask & (1u << li)) && LIGHT_LAYER[li] == LIGHT_LAYER_BG) bg = (int)li;
  }
  if (bg >= 0) return (LIGHT_HEADER[bg] == HEADER_DARK);
  return (cfg_lightingMask != 0);   // 背景なし＋オーバーレイのみ → 暗い会場＝黒
}

// ====================================================
// 🌙 Sleep Lighting Carousel（眺めて楽しいSleep表示）v1.0
//
// Sleep中（SLEEP_FACE_DURATION経過後）に、既存の「目」3種（時計／Eye Slot／
// 閉じ目）と既存Lighting（Eye Slotを除く）を独立ランダムに組み合わせて表示する。
// 実体は handleSleepMode() 側（updateSleepLightingCarousel）と
// Lighting Framework側（sleepComposeEyeLightFrame、#ifdef FFT_DISPLAY_TEST内）に分かれる。
//
// 【重要】cfg_lightingMask・cfg_lightingManualMask・cfg_lightingRandomOn・
// gLightingActive・screenFxLighting・Visualizer関連状態には一切書き込まない。
// ここで使うのはすべてSleep専用のローカル状態であり、起床後にユーザーの
// Lighting設定・通常Lighting/Visualizerの動作へ影響しない設計とする。
// ====================================================
enum SleepEyeKind : uint8_t {
  SLEEP_EYE_CLOCK   = 0,   // 時計の目（既存Sleep時計顔の数字をそのまま利用）
  SLEEP_EYE_EYESLOT = 1,   // Eye Slotの目（既存lightRenderEyeSlot()のリールを再利用）
  SLEEP_EYE_CLOSED  = 2,   // 閉じた目（既存drawSleepEyes()の線をそのまま利用）
  SLEEP_EYE_KIND_COUNT
};

bool     sleepCarouselStarted      = false;  // SLEEP_FACE_DURATION経過後にtrue（1回だけ遷移）
unsigned long sleepCarouselNextSwitchMs = 0;  // 次にパターン(Gallery/目+Lighting)を選び直す時刻。0=即選択
int8_t   sleepLastPattern          = -1;     // 直近パターン（現状は連続回避に使わない。ログ用に保持）

// 「目×Lighting」表示中かどうか。updateNose()の直接描画フォールバック抑止と
// drawBattery()の配色判定にだけ使う、Sleep専用の狭いスコープのフラグ。
// Sleep突入時・起床時に必ずfalseへ戻す（handleSleepTransition/wakeUp参照）。
bool     sleepLightingComposeActive = false;
bool     sleepPanelDark             = false; // 現在の背景Lightingヘッダーテーマ（true=黒）

uint8_t  sleepEyeKind        = SLEEP_EYE_CLOCK;
uint8_t  sleepBgLightMode    = LIGHT_AURORA;
int8_t   sleepLastEyeKind    = -1;   // 直前に選ばれた目（連続回避用）
int8_t   sleepLastBgLightMode = -1;  // 直前に選ばれた背景Lighting（連続回避用）
bool     sleepLightNeedsInit = true; // 背景Lightingの選択が変わった直後はtrue（全面init）

// 「目×Lighting」表示中の顔パーツ（時計の目／閉じ目／鼻／鼻口線／口）に使う
// 黒本体＋白縁取りの、白縁ぶんの太さ（実機確認 2026-08-04）。
// 背景Lightingの明暗によらず一定の視認性を確保するための値。
// 既存の鼻の白ハロー（黒18x12楕円に対し白20x14楕円＝各辺+2px＝直径+4px）を基準にした。
const int SLEEP_OUTLINE_PX = 4;

// ====================================================
// 💡 Lighting 共通 Brightness（v1.3）— Framework全体の明るさ
//
// Disco Floor / Laser Show / 将来のAurora/Fire/Neon/Matrix すべてに共通で効く。
// 各Lightingが個別に持つのではなく、描画の最終出力（lightFillRect/lightDrawLine）で
// 一括適用するため、新しいLightingを追加しても自動的にBrightnessが効く。
//
// 適用方法（色味を変えず明るさだけ）：
//   RGB各チャンネルを同じ係数で乗算する＝HSVのV成分だけを下げるのと等価
//   （hue/saturationは保存される）。係数のマッピングは知覚ガンマ(1/2.2)で行い、
//   スライダーの各段が知覚的に均等になるようにする（sRGB空間で自然な減光）。
//     例) 80% → 0.80^(1/2.2) ≒ 0.90倍（少しだけ暗い＝白飛びしない既定）
//         20% → 0.20^(1/2.2) ≒ 0.49倍（クラブっぽい暗さ・真っ暗にはしない）
// ====================================================
uint8_t  cfg_lightingBrightness = 80;   // 明るさ(%)。NVS "lightBri"。既定80%
uint16_t gLightBriQ8 = 233;             // Q8係数(0..256)。描画時に色へ乗算。80%相当で初期化
bool     gLightNeedReinit = false;      // 明るさ変更時に合成を全面initし直すフラグ（即時反映）
bool     gLightBgFilled    = false;      // 背景(面)がこのフレームで塗られたか（Laserの暗背景判定）
// 2026-07-31: このフレームで実際に採用された背景(面)Lightingのenum値（未採用時は0xFF）。
//   既存の描画順・ロジックには一切影響しない「記録するだけ」の追加で、
//   Hypnotic Vortex限定の顔パーツ処理（口の白backing）が、他の既存Lighting
//   （Disco/Aurora/Tempest Tunnel等）では従来どおり何も変えないよう正確に
//   判定するために使う（sceneDrawLightingLayer参照）。
uint8_t  gLightActiveBgMode = 0xFF;
bool     gEyeSlotActive    = false;      // Eye Slot（お目々スロット）がこのフレームの背景として採用中か
                                          // （drawVisualizerFaceParts()側で通常の黒目描画を止めるためのフラグ）

// cfg_lightingBrightness(%) から知覚ガンマで Q8係数を再計算する。
// loadConfig() と WebUIハンドラ・setup から呼ぶ（毎フレームは呼ばない）。
void recomputeLightBrightness() {
  int p = cfg_lightingBrightness;
  if (p < 0)   p = 0;
  if (p > 100) p = 100;
  float f = powf((float)p / 100.0f, 1.0f / 2.2f);   // 知覚ガンマ
  int q = (int)lroundf(256.0f * f);
  if (q < 0)   q = 0;
  if (q > 256) q = 256;
  gLightBriQ8 = (uint16_t)q;
}

// Lighting合成モードが現在アクティブか（Disco等の背景照明を描画中）。
// showSensors()/drawBattery() が上端の情報パネルを「黒背景＋明色文字」へ
// 切り替えるためにここで参照する（これらの関数はファイル前半にあるため、
// 描画側より前でグローバル宣言しておく必要がある）。
// 実際の true/false 切替は updateScreenEffects()（#ifdef FFT_DISPLAY_TEST内）で行う。
bool screenFxLighting = false;


// ====================================================
// 🐰 KariPom Name（個体識別名）
// 複数台運用時・将来のBluetooth対応時に「どの個体か」を
// 分かりやすくするための表示名（文字列・空欄可）。
// 他のcfg_設定と同じ実装方式でNVSへ永続化（saveConfig/loadConfig）。
// 空欄の場合はHome画面等の表示名としてIPアドレスを使用する
// （karipomDisplayName()参照）。
// ====================================================
String cfg_karipomName = "";  // NVS未保存時は空欄（表示はIPアドレスにフォールバック）

bool   configEverSaved = false;   // saveConfig()が一度でも実行済みか（初回は必ず保存するための判定）
String lastSavedConfigSnapshot = "";  // 直前にNVSへ保存した全設定値のスナップショット（重複保存判定用。時間は使わない）

void saveConfig() {
  // 2026/07/14: 同一内容のCFG SAVEDがログに2回記録される件の対策。
  // 呼び出し元(各Web設定ハンドラ)は変更しない最小差分とするため、
  // 「これから保存する全設定値」を直前に実際に保存した値と比較し、
  // 完全一致する場合だけNVS書き込み・addLog()をスキップする。
  // 時間(millis())は判定に使わないため、300ms以内の別項目の変更も必ず保存される。
  // 2026/07/18: Ear統合版への復帰にあたり、追加された kpName(cfg_karipomName) も比較対象へ含める。
  String snapshot = String(cfg_enableCamera)
    + "," + String(cfg_enableToiletCam)
    + "," + String(cfg_enableSDLog)
    + "," + String((uint8_t)audioSource)
    + "," + String(cfg_micOnThreshold)
    + "," + String(cfg_micOffThreshold)
    + "," + String(cfg_micReleaseHoldMs)
    + "," + String(cfg_mutterChance)
    + "," + String(cfg_mutterFaceChance)
    + "," + String(cfg_idleChance)
    + "," + String(cfg_udpVisualizerFace)
    + "," + String((uint8_t)cfg_vizManualMode)
    + "," + String(cfg_vizRandomOn)
    + "," + String(cfg_vizRandomIntervalMin)
    + "," + String(cfg_lightingManualMask)
    + "," + String(cfg_lightingRandomOn)
    + "," + String(cfg_lightingRandomIntervalMin)
    + "," + String(cfg_lightingBrightness)
    + "," + String((uint8_t)cfg_characterStyle)
    + "," + cfg_karipomName;

  if (configEverSaved && snapshot == lastSavedConfigSnapshot) {
    return;  // 直前の保存内容と完全一致 → 重複保存とみなしスキップ（RAM側は既に更新済みのため実害なし）
  }

  karipomPrefs.begin("karipom", false);
  karipomPrefs.putBool("camera",    cfg_enableCamera);
  karipomPrefs.putBool("toiletCam", cfg_enableToiletCam);
  karipomPrefs.putBool("sdLog",     cfg_enableSDLog);
  karipomPrefs.putUChar("audioSrc",  (uint8_t)audioSource);
  karipomPrefs.putInt("micOnTh",   cfg_micOnThreshold);
  karipomPrefs.putInt("micOffTh",  cfg_micOffThreshold);
  karipomPrefs.putInt("micRelMs",  (int)cfg_micReleaseHoldMs);
  karipomPrefs.putInt("mutterCh",  cfg_mutterChance);
  karipomPrefs.putInt("mutterFace", cfg_mutterFaceChance);
  karipomPrefs.putInt("idleCh",    cfg_idleChance);
  karipomPrefs.putBool("udpVisiFace", cfg_udpVisualizerFace);   // 旧キー（互換維持）
  karipomPrefs.putUChar("vizMode",    (uint8_t)cfg_vizManualMode);  // Visualizerモード（ユーザーの手動選択のみ。Randomの一時選択は保存しない）
  karipomPrefs.putBool("vizRndOn",    cfg_vizRandomOn);               // Visualizer Random ON/OFF
  karipomPrefs.putUChar("vizRndMin",  cfg_vizRandomIntervalMin);      // Visualizer Random 切替間隔(分・5/10/15)
  karipomPrefs.putUInt("lightMask", cfg_lightingManualMask);          // Lighting（背景照明・ユーザーの手動選択のみ。Randomの一時選択は保存しない。2026-07-30: UShort→UInt）
  karipomPrefs.putBool("lightRndOn",  cfg_lightingRandomOn);          // Lighting Random ON/OFF
  karipomPrefs.putUChar("lightRndMin", cfg_lightingRandomIntervalMin);// Lighting Random 切替間隔(分・5/10/15)
  karipomPrefs.putUChar("lightBri",   cfg_lightingBrightness);        // Lighting共通の明るさ(%)
  karipomPrefs.putString("kpName", cfg_karipomName);
  karipomPrefs.putUChar("charStyle", (uint8_t)cfg_characterStyle);
  karipomPrefs.end();

  // NVSへの書き込みが完了してからスナップショットを更新する。
  // put*()の途中で失敗しても「保存済み」扱いにはしない（次回同一内容を誤ってスキップしないため）。
  configEverSaved = true;
  lastSavedConfigSnapshot = snapshot;

  addLog("CFG SAVED: CAMERA=" + String(cfg_enableCamera ? "ON" : "OFF")
       + " TOILET_CAM=" + String(cfg_enableToiletCam ? "ON" : "OFF")
       + " SD_LOG=" + String(cfg_enableSDLog ? "ON" : "OFF")
       + " AUDIO_SRC=" + String(audioSourceName(audioSource))
       + " MIC_ON=" + String(cfg_micOnThreshold)
       + " MIC_OFF=" + String(cfg_micOffThreshold)
       + " MIC_HOLD=" + String(cfg_micReleaseHoldMs)
       + " MUTTER_CH=" + String(cfg_mutterChance)
       + " MUTTER_FACE=" + String(cfg_mutterFaceChance)
       + " IDLE_CH=" + String(cfg_idleChance)
       + " KARIPOM_NAME=" + String(cfg_karipomName.length() > 0 ? cfg_karipomName : String("(empty)"))
       + " CHARACTER=" + String(characterStyleName(cfg_characterStyle))
       + " VIZ=" + String(VIZ_MODES[cfg_vizManualMode].id)
       + " VIZ_RND=" + String(cfg_vizRandomOn ? "ON" : "OFF") + "/" + String(cfg_vizRandomIntervalMin) + "min"
       + " LIGHT=" + String(cfg_lightingManualMask)
       + " LIGHT_RND=" + String(cfg_lightingRandomOn ? "ON" : "OFF") + "/" + String(cfg_lightingRandomIntervalMin) + "min"
       + " BRI=" + String(cfg_lightingBrightness));
}

void loadConfig() {
  karipomPrefs.begin("karipom", true);
  cfg_enableCamera    = karipomPrefs.getBool("camera",    false);  // デフォルトOFF
  cfg_enableToiletCam = karipomPrefs.getBool("toiletCam", false);  // デフォルトOFF
  cfg_enableSDLog     = karipomPrefs.getBool("sdLog",     false);  // デフォルトOFF
  // 音声入力ソースは読み込みのみ行い、ここでは適用しない。
  // 起動音（online_ready.wav 等）を必ず再生するため、
  // 適用は setup() 末尾の applyBootAudioSource() で行う。
  cfg_bootAudioSource = karipomPrefs.getUChar("audioSrc", AUDIO_SRC_UDP);
  cfg_micOnThreshold    = karipomPrefs.getInt("micOnTh",  150);   // NVS未保存時: 職場ノイズ対応デフォルト
  cfg_micOffThreshold   = karipomPrefs.getInt("micOffTh",  50);   // NVS未保存時: ON_TH=150の1/3
  cfg_micReleaseHoldMs  = (unsigned long)karipomPrefs.getInt("micRelMs", 1000);  // NVS未保存時: 1000ms
  cfg_mutterChance      = karipomPrefs.getInt("mutterCh",   10);  // NVS未保存時: 標準
  cfg_mutterFaceChance  = karipomPrefs.getInt("mutterFace", 20);  // NVS未保存時: 20%
  cfg_idleChance        = karipomPrefs.getInt("idleCh",      2);  // NVS未保存時: 標準
  // ── Visualizer モード（v1.0で3モード化）──
  // 旧版は bool udpVisiFace（ON=グライコ表示 / OFF=非表示）だけだった。
  // 新キー vizMode が未保存(=VIZ_MODE_UNSET)なら旧キーから自動移行する。
  //   旧 ON  → VIZ_MODE_EQ （既存Graphic EQ利用者は更新後もEQのまま）
  //   旧 OFF → VIZ_MODE_OFF
  // 無効値(範囲外)を読み込んだ場合は VIZ_MODE_EQ へフォールバックする。
  cfg_udpVisualizerFace = karipomPrefs.getBool("udpVisiFace", true);
  {
    uint8_t vm = karipomPrefs.getUChar("vizMode", VIZ_MODE_UNSET);
    // Lighting: v1.7でcfg_lightingMaskをuint8_t→uint16_tへ拡張したため、
    // NVS上のキー型もUChar→UShortへ変わった。v1.6以前に保存された
    // UChar値はgetUShortでは型不一致となりsentinel(0xFFFF)が返るため、
    // その場合のみ旧UChar形式として読み直す（後方互換・値の意味は不変）。
    // 2026-07-30: Optical Illusionシリーズ3種追加でuint16_t→uint32_tへ再拡張。
    // NVS上のキー型もUShort→UIntへ変わった。同じ理由でgetUIntが型不一致
    // sentinel(0xFFFFFFFF)を返す場合は、上記の2段（UShort→UChar）を
    // そのままフォールバックとして使う（3段フォールバック・値の意味は不変）。
    {
      uint32_t storedMask32 = karipomPrefs.getUInt("lightMask", 0xFFFFFFFFUL);
      if (storedMask32 != 0xFFFFFFFFUL) {
        cfg_lightingManualMask = storedMask32;
      } else {
        uint16_t storedMask16 = karipomPrefs.getUShort("lightMask", 0xFFFF);
        if (storedMask16 == 0xFFFF) {
          cfg_lightingManualMask = (uint32_t)karipomPrefs.getUChar("lightMask", 0);  // 旧v1.6以前 or 未保存(全OFF)
        } else {
          cfg_lightingManualMask = storedMask16;   // 旧v1.7〜v2.x（uint16_t時代）
        }
      }
    }
    cfg_lightingBrightness = karipomPrefs.getUChar("lightBri", 80);  // Lighting共通の明るさ: 既定80%
    if (cfg_lightingBrightness < 10 || cfg_lightingBrightness > 100) cfg_lightingBrightness = 80;  // 無効値フォールバック
    recomputeLightBrightness();
    if (vm == VIZ_MODE_UNSET) {
      vm = cfg_udpVisualizerFace ? (uint8_t)VIZ_MODE_EQ : (uint8_t)VIZ_MODE_OFF;
    } else if (vm >= (uint8_t)VIZ_MODE_COUNT) {
      vm = (uint8_t)VIZ_MODE_EQ;
    }
    cfg_vizManualMode = (VisualizerMode)vm;

    // 🎲 Random ON/OFF・切替間隔（5/10/15分のみ有効。未保存/不正値は既定へフォールバック）
    cfg_lightingRandomOn          = karipomPrefs.getBool("lightRndOn", false);
    cfg_lightingRandomIntervalMin = karipomPrefs.getUChar("lightRndMin", 5);
    if (cfg_lightingRandomIntervalMin != 5 && cfg_lightingRandomIntervalMin != 10 && cfg_lightingRandomIntervalMin != 15) {
      cfg_lightingRandomIntervalMin = 5;
    }
    cfg_vizRandomOn          = karipomPrefs.getBool("vizRndOn", false);
    cfg_vizRandomIntervalMin = karipomPrefs.getUChar("vizRndMin", 5);
    if (cfg_vizRandomIntervalMin != 5 && cfg_vizRandomIntervalMin != 10 && cfg_vizRandomIntervalMin != 15) {
      cfg_vizRandomIntervalMin = 5;
    }

    // Random ONで再起動した場合：起動直後は個別選択を全解除した状態にし、
    // lastSwitchMs=0（まだ一度も選んでいない）のままにしておくことで、
    // setup()後の最初のloop()（updateLightingRandomTick/updateVisualizerRandomTick）が
    // 要件7どおり「待たずに1つ選んで開始」する。Random OFFなら従来どおり手動選択を復元する。
    if (cfg_lightingRandomOn) {
      cfg_lightingMask = 0;
      lightingRandomLastSwitchMs = 0;
      lightingRandomLastPick = -1;
    } else {
      cfg_lightingMask = cfg_lightingManualMask;
    }
    if (cfg_vizRandomOn) {
      cfg_visualizerMode = VIZ_MODE_OFF;
      vizRandomLastSwitchMs = 0;
      vizRandomLastPick = -1;
    } else {
      cfg_visualizerMode = cfg_vizManualMode;
    }
    cfg_udpVisualizerFace = (cfg_visualizerMode != VIZ_MODE_OFF);  // 旧キーを同期
  }
  cfg_karipomName       = karipomPrefs.getString("kpName", "");  // NVS未保存時: 空欄（IP表示にフォールバック）
  cfg_characterStyle    = (CharacterStyle)karipomPrefs.getUChar("charStyle", CHARACTER_KARIPOM);  // NVS未保存時: KariPom
  karipomPrefs.end();
  addLog("CFG LOADED: CAMERA=" + String(cfg_enableCamera ? "ON" : "OFF")
       + " TOILET_CAM=" + String(cfg_enableToiletCam ? "ON" : "OFF")
       + " SD_LOG=" + String(cfg_enableSDLog ? "ON" : "OFF")
       + " AUDIO_SRC=" + String(audioSourceName((AudioSource)cfg_bootAudioSource))
       + " MIC_ON=" + String(cfg_micOnThreshold)
       + " MIC_OFF=" + String(cfg_micOffThreshold)
       + " MIC_HOLD=" + String(cfg_micReleaseHoldMs)
       + " MUTTER_CH=" + String(cfg_mutterChance)
       + " MUTTER_FACE=" + String(cfg_mutterFaceChance)
       + " IDLE_CH=" + String(cfg_idleChance)
       + " KARIPOM_NAME=" + String(cfg_karipomName.length() > 0 ? cfg_karipomName : String("(empty)"))
       + " CHARACTER=" + String(characterStyleName(cfg_characterStyle))
       + " VIZ=" + String(VIZ_MODES[cfg_visualizerMode].id)
       + " VIZ_RND=" + String(cfg_vizRandomOn ? "ON" : "OFF") + "/" + String(cfg_vizRandomIntervalMin) + "min"
       + " LIGHT=" + String(cfg_lightingMask)
       + " LIGHT_RND=" + String(cfg_lightingRandomOn ? "ON" : "OFF") + "/" + String(cfg_lightingRandomIntervalMin) + "min"
       + " BRI=" + String(cfg_lightingBrightness));
}

// ====================================================
// 🎲 Lighting Random / Audio Visualizer Random — 切替タイマー（v1.0 / 2026-07-27）
//
// 目的：Lighting・Visualizerとも種類が増えてきたため、ユーザーが毎回手動で
// 選ばなくても一定時間ごとに様々な演出を自動で切り替えて楽しめるようにする。
// 描画内容・候補一覧・既存の選択方式（Lightingはビットマスク／Visualizerは
// 単一モード）・MIC時にVisualizerを使わない既存判定には一切手を入れない。
// ここは「選び方」を追加するだけの薄いレイヤーで、loop()から毎回呼ぶ。
//
// ・LightingとVisualizerのRandomは完全に独立（片方のON/OFFがもう片方に影響しない）。
// ・切替間隔は5/10/15分の3択のみ。ここ(randomIntervalMs)を見れば一目で分かる。
// ・millis()オーバーフローに対しても安全な符号なし引き算 (now - last) で比較する
//   （既存のLIGHT_COMPOSITE_MS等と同じ手法）。
// ・lastSwitchMs==0 は「まだ一度も選んでいない」を意味し、待たずに即選択する
//   （ON直後・Random ONのまま起動した直後の両方をこの1条件でカバーする）。
// ====================================================

// Random切替間隔(分)→ms。5/10/15分以外が渡された場合は5分にフォールバックする。
// 間隔を調整したい場合はここだけを見ればよい。
unsigned long randomIntervalMs(uint8_t minutes) {
  if (minutes != 5 && minutes != 10 && minutes != 15) minutes = 5;
  return (unsigned long)minutes * 60000UL;
}

// Lighting Random：ON中は一定間隔でLIGHT_MODES[]から1つをランダム選択し、
// cfg_lightingMask をそのビット1つだけにする（複数同時ONという既存仕様は
// 変更せず、Random中は常に「1つだけ選ぶ」運用にしている）。
// cfg_lightingManualMask は変更しない＝ユーザーの手動選択はそのまま保持される。
void updateLightingRandomTick() {
  if (!cfg_lightingRandomOn) return;
  unsigned long now = millis();
  unsigned long interval = randomIntervalMs(cfg_lightingRandomIntervalMin);
  if (lightingRandomLastSwitchMs != 0 &&
      (unsigned long)(now - lightingRandomLastSwitchMs) < interval) {
    return;  // まだ切替時刻ではない
  }

  uint8_t pick = 0;
  if (LIGHT_MODE_COUNT > 1) {
    do {
      pick = (uint8_t)random(0, (long)LIGHT_MODE_COUNT);
    } while ((int)pick == lightingRandomLastPick);
  }

  cfg_lightingMask = (uint32_t)(1u << pick);
  gLightNeedReinit = true;   // 既存の/lightingハンドラ・Brightness変更と同じく全面initし直す
  lightingRandomLastPick     = (int8_t)pick;
  lightingRandomLastSwitchMs = now;
  addLog("LIGHTING RANDOM: " + String(LIGHT_MODES[pick].id)
       + " (next in " + String(cfg_lightingRandomIntervalMin) + "min)");
}

// Audio Visualizer Random：ON中は一定間隔でVIZ_MODES[]から1つをランダム選択する。
// OFF(VIZ_MODE_OFF)は「演出」ではないため候補から除外する。
// MIC選択時にVisualizerが表示されない既存判定(isVisualizerFaceEnabled)は
// 変更していないため、MIC中にこの関数がcfg_visualizerModeを変更しても
// 実際の描画には影響しない（従来どおり表示されない）。
void updateVisualizerRandomTick() {
  if (!cfg_vizRandomOn) return;
  unsigned long now = millis();
  unsigned long interval = randomIntervalMs(cfg_vizRandomIntervalMin);
  if (vizRandomLastSwitchMs != 0 &&
      (unsigned long)(now - vizRandomLastSwitchMs) < interval) {
    return;  // まだ切替時刻ではない
  }

  uint8_t candidateCount = (uint8_t)VIZ_MODE_COUNT - 1;  // OFFを除いた候補数
  uint8_t pick = (uint8_t)VIZ_MODE_OFF;
  if (candidateCount >= 1) {
    if (candidateCount == 1) {
      pick = 1;  // 候補が1つしかない場合はそれを選ぶ（連続回避の判定不能を回避）
    } else {
      do {
        pick = (uint8_t)random(1, (long)VIZ_MODE_COUNT);  // 1..VIZ_MODE_COUNT-1（OFF除外）
      } while ((int)pick == vizRandomLastPick);
    }
  }

  cfg_visualizerMode    = (VisualizerMode)pick;
  cfg_udpVisualizerFace = (cfg_visualizerMode != VIZ_MODE_OFF);  // 既存の同期ロジックを踏襲
  vizRandomLastPick     = (int8_t)pick;
  vizRandomLastSwitchMs = now;
  addLog("VISUALIZER RANDOM: " + String(VIZ_MODES[pick].id)
       + " (next in " + String(cfg_vizRandomIntervalMin) + "min)");
}

uint8_t gazePrev[16][12];
bool gazeHasPrev = false;

int gazeState = 0;  // -1:左 0:中央 1:右
int gazeLastCenterX = 160;
int gazeLastCenterY = 120;

unsigned long lastGazeUpdate = 0;
unsigned long lastGazeTime = 0;

const int GAZE_LEFT_THRESHOLD = 145;
const int GAZE_RIGHT_THRESHOLD = 160;
const int GAZE_DIFF_THRESHOLD = 45;
const int GAZE_MIN_COUNT = 12;                    // 動きの検出 12 → 15 → 18
const unsigned long GAZE_UPDATE_INTERVAL = 2000;  // 反応頻度 800 → 1500 → 2000
const unsigned long GAZE_RETURN_MS = 1000;        // 戻る時間 2000 → 1000

const int GAZE_EYE_X = 18;
const int GAZE_EYE_Y = 10;

// 手動スタンバイ（旧: 覚醒/非覚醒モード, awakeMode）は2026/07/20廃止。
// 画面が完全静止しフリーズと誤認される原因になっていたため、
// 自動スリープ（sleepMode）に一本化した。
// タッチ中の喜び顔判定にのみ faceTouching を引き続き使用する。
bool faceTouching = false;

// ====================================================
// 🚽 ログトイレ設定
// ====================================================
const char* LOG_TOILET_DIR = "/logtoilet";
const char* LOG_TOILET_FILE = "/logtoilet/karipom.log";
bool logToiletEnabled = true;

// ====================================================
// System states
// かりポムの現在状態を表すフラグ
// ====================================================
bool sleepMode = false;         // 睡眠中
bool alertMode = false;         // 警戒中
bool petMode = false;           // なでなで反応中
bool soundBusy = false;         // WAV再生中
bool imageFaceMode = false;     // PNG顔画像を表示中
bool externalSpeaking = false;  // Mac音声連携で発話中

// ====================================================
// 呟き変顔演出
// handleRandomMutter() が低確率でPNG顔を一時表示する。
// mutterFaceActive=true の間だけ、呟き終了後に drawFace() で戻す。
// Web FaceGallery からの手動表示（imageFaceMode=true のまま残す）とは
// 干渉しない（手動表示時は mutterFaceActive=false のまま）。
// ====================================================
bool mutterFaceActive = false;  // 呟き演出でPNGを表示中

// cfg_mutterChance / cfg_mutterFaceChance / cfg_idleChance は
// saveConfig()/loadConfig() より前（上部）に定義済み。

unsigned long lastInteractionTime = 0;
const unsigned long SLEEP_TIMEOUT = 900000;  // 15分無操作で、あくび→スリープ

unsigned long lastImuAlertTime = 0;
unsigned long lastMutterCheck = 0;
unsigned long lastVolumeDisplayTime = 0;

unsigned long lastSleepMotionLogTime = 0;  // 睡眠中モーションログ間引き用

// ====================================================
// ジョイスティック連打対策
// 短時間に何度も押されても、古い入力を溜め込まない
// ====================================================
int currentVolume = 0;
float currentAccel = 0;
int currentMotionLevel = 0;

uint8_t previousPixels[64];
bool hasPrevious = false;

// ====================================================
// サーボ軸個別有効化スイッチ
//
// ENABLE_UD_SERVO = false … 上下サーボ（servoUD / UD_SERVO_PIN=2）を完全無効化
// ENABLE_LR_SERVO = false … 左右サーボ（servoLR / LR_SERVO_PIN=1）を完全無効化
//
// 無効化された軸は attach/write/detach を一切行わない。
// setup()時のattachもスキップされ、常にセンター値のまま扱う。
// ====================================================
const bool ENABLE_UD_SERVO = true;
const bool ENABLE_LR_SERVO = true;

bool servoBusy = false;

// ====================================================
// Web操作専用の排他制御
//
// moveSmooth()内でserver.handleClient()が呼ばれるため、
// 動作中に別のWebリクエストが割り込む可能性がある。
// webServoBusyで「Web操作そのものの排他」を管理し、
// 重複検出時はログに残す。
// ====================================================
bool webServoBusy = false;
unsigned long lastWebServoCmdTime = 0;
String lastWebServoButton = "";
const unsigned long WEB_SERVO_DUP_WINDOW_MS = 500;

// Web操作直後は自動IDLE/MUTTERを数秒だけ抑制する（操作と自動動作の競合回避）。
// lastWebServoCmdTime を基準に、この時間内は canDoIdleAction() が false を返す。
const unsigned long WEB_ACTION_IDLE_SUPPRESS_MS = 4000;

// ====================================================
// 左右サーボ（servoLR）自動 detach 管理
//
// 左右サーボは機構上の負荷により保持トルクをかけ続けると
// 悲鳴（ハンチング）が発生する場合がある。
// 上下サーボ（servoUD）は現状常時attachのままにする。
//
// LR_DETACH_DELAY_MS : 動作終了から detach までの猶予（2000ms基準）
// LR_ATTACH_SETTLE_MS: attach直後のwrite()までの安定待ち
//   短すぎると急動が起きる。
//   2026/07/07: センター復帰時の「ギャワッ」異音対策として
//   旧50ms→100msへ増加（従来コメントの既定手順どおり）。
// ====================================================
const unsigned long LR_DETACH_DELAY_MS  = 2000;
const unsigned long LR_ATTACH_SETTLE_MS = 100;

// ====================================================
// LR_ATTACH_SAG_OFFSET_DEG（2026/07/07・実験用ノブ / デフォルト0＝挙動不変）
//
// ESP32Servo（S3/MCPWM・LEDCとも）は attach() 時点ではPWMを出さず
// （duty=0初期化）、最初の write() でPWM＝保持トルクが開始される。
// detach中は頭部自重で物理角度が lrNow からズレているため、
// 最初の write(lrNow) の瞬間に「ズレ→lrNow」の全速補正が起きる。
// これが「ギュイン」の第一容疑（切り分けログで確認のこと）。
//
// ── 正負の決め方（SERVO_LR_LEFT=120=大、SERVO_LR_RIGHT=60=小）──
//   detach後に頭が「右へ流れる」 → 物理角は lrNow より小 → 負の値（例 -5）
//   detach後に頭が「左へ流れる」 → 物理角は lrNow より大 → 正の値（例 +5）
//
// ── 実測手順 ──
//   1. servoLRをdetach（LR_DETACH_DELAY_MSを極短くするかWebから手動操作）
//   2. 頭が止まった位置を目視し、lrNow(通常90)からのズレ方向と度数を推定
//   3. 上記ルールで符号と値を設定（まず ±3 程度から試すと安全）
//   4. LR RE-ATTACH ログの attachAngle / lrNow / SAG を確認して微調整
//
// 設定時は IDLE LR PRE-ATTACH と moveSmooth 再attach のどちらでも
// 「物理推定位置へ通電→1度/50msでlrNowへ戻す」ランプを通る。
// 0 のままなら従来と完全に同一挙動（ランプは走らない）。
// ====================================================
const int LR_ATTACH_SAG_OFFSET_DEG = 0;

// ====================================================
// JOY_LR_SLEW_STEP_MS（2026/07/07）
//
// ジョイスティックX軸→servoLR追従は、従来 write(targetLR) の一発反映で、
// Yawで唯一 moveSmooth を通らない「移動を伴う直接write」だった。
// スティックを速く倒すと1回の更新で数度〜十数度ジャンプし、
// 急加速の「キュッ」音の原因になる。
//
// 対策：handleJoystick() はループ毎（smartDelay中も）呼ばれるため、
// 1回の呼び出しで lrNow を targetLR へ最大1度だけ近づける
// スルーレート制限方式にする（この間隔で1度ずつ）。
// delay() を使わないため操作追従・Web/UDP・口パクは一切阻害しない。
// 15ms/度 ≒ 最大約66度/秒。応答は保ちつつ急加速を抑える。
// 実機検証の結果、30ms/度（約33度/秒）にすると動きが落ち着き自然に見えるため
// LR_SLOWDOWN_FACTOR=2 に合わせて 30ms を標準仕様として設定。
// ====================================================
const unsigned long JOY_LR_SLEW_STEP_MS = 30;

// ====================================================
// JOY_SERVO_RESUME_DELAY_MS（2026/07/09・ジョイスティック幻入力対策）
//
// サーボ駆動・保持電流＋Wi-Fi送信バーストで電源レールが沈むと、
// ratiometric出力のジョイスティックADCに「両軸同時・同方向」の
// 同相偏差が乗り、実入力と区別できない（APモードで実測・再現済み）。
// servoBusy中および解除後この時間は handleJoystick() の入力判定を停止する。
// MICの MIC_SERVO_RESUME_DELAY_MS と同じ考え方（発生源が同じ電流イベント）。
// 実測値: Web UP直後の幻入力は移動完了+約8msで発生、続くCockpitページ
// 配信（TXバースト）は+約480msまで継続 → 600msで両方を覆う。
// ====================================================
const unsigned long JOY_SERVO_RESUME_DELAY_MS = 600;
unsigned long joyServoResumeAt = 0;  // この時刻までジョイスティック判定停止（0=制限なし）

// ── JOY_FLOAT_GRACE_AFTER_SERVO_MS（2026/07/09 改修4）──────────────
// サーボ保持ハンチングの電流ノイズは抑制窓（600ms）より長く続くことがあり、
// 浮き疑いサンプルが8回に達して JOY DISABLED → JOY CALIB → JOY ENABLED の
// サイクルが毎Web操作で発生していた（実測）。浮き検出の本来の目的は
// 「未接続・断線の検出」であり、サーボ起因の一時ノイズで発動させない。
// サーボ動作終了からこの時間内の浮き疑いサンプルは、当該loopのスキップ
// （改修3-A）のみ行い、無効化カウントには積まない。
// 副作用: 本当に未接続になった場合のJOY DISABLED確定が最大この時間だけ
// 遅れるが、その間も疑いサンプルは毎loopスキップされるため実害はない。
const unsigned long JOY_FLOAT_GRACE_AFTER_SERVO_MS = 5000;
unsigned long lastServoMoveEndAt = 0;  // 最後にサーボが動いた時刻（moveSmooth終了/servoBusy観測）

unsigned long lrLastMoveTime = 0;  // 最後に左右サーボを動かした時刻
bool lrDetachScheduled = false;    // detach 待ちフラグ
String   lrMoveCaller = "";    // 呼び出し元タグ（moveSmooth前に設定）
uint32_t lrMoveGen    = 0;     // LR移動世代番号

// ── LRサーボ・フェイルセーフ（2026/07/09、コメント更新 2026/07/20）──────
// handleLrAutoDetach()は externalSpeaking / lrDetachScheduled / servoBusy /
// loop()内の早期return（sleep）に依存しており、どれか1つの不整合で
// attachが無期限に残る（例: sleepMode中はloop()が早期returnするため
// handleLrAutoDetach()が一度も呼ばれない）。
// ※旧・手動スタンバイ（awakeMode）は2026/07/20廃止済み。
//   本フェイルセーフは自動スリープ（sleepMode）用として引き続き必要。
// ここは「フラグを一切信用しない最後の砦」として loop()先頭
// （sleepゲートより前）で毎回実行する。通常時は何もしない。
//  (1) 非発話中: 最終LR動作（lrLastMoveTime）から5秒以上attachが残って
//      いたら強制detach。通常の2秒AUTO DETACHが正しく動く限り発動しない。
//  (2) 発話中でも: 連続attachが90秒を超えたら一旦強制detach。
//      （MIC自己帰還等でexternalSpeakingが固着した場合の保持時間上限。
//        次のtalk micro motionでmoveSmoothが自動再attachするため動作は継続）
// 発動ログ「LR FAILSAFE DETACH」は通常保護をすり抜けた証拠として扱うこと。
const unsigned long LR_FAILSAFE_QUIET_MS = 5000;   // (1) 非発話・無動作の許容attach時間
const unsigned long LR_MAX_ATTACH_MS     = 90000;  // (2) 連続attachの絶対上限
unsigned long lrAttachStartTime = 0;               // 連続attach開始時刻（0=非attach）

void handleLrFailsafe() {
  if (!ENABLE_LR_SERVO) return;
  if (!servoLR.attached()) { lrAttachStartTime = 0; return; }
  if (lrAttachStartTime == 0) { lrAttachStartTime = millis(); return; }

  unsigned long now = millis();

  // (1) 非発話中の取りこぼし救済（sleepゲート・フラグ不整合対応）
  if (!externalSpeaking && !servoBusy &&
      (now - lrLastMoveTime > LR_FAILSAFE_QUIET_MS)) {
    servoLR.detach();
    lrDetachScheduled = false;
    lrAttachStartTime = 0;
    addLog("LR FAILSAFE DETACH (quiet)");
    suppressMicStart(2000, "detach");  // detach音のMIC誤START抑制（通常detachと同手順）
    return;
  }

  // (2) 連続attach時間の上限（発話固着・MIC帰還ループ対策）
  if (now - lrAttachStartTime > LR_MAX_ATTACH_MS) {
    servoLR.detach();
    lrDetachScheduled = false;
    lrAttachStartTime = 0;
    addLog("LR FAILSAFE DETACH (max attach)");
    suppressMicStart(2000, "detach");
  }
}

// lrEnsureAttached(): detach済みなら attach → 安定待ち → 現在角度を write
// ⚠️ 2026/07/07 精査結果: 呼び出し元ゼロ（デッドコード）。実行されないため
//    「ギュイン」音の原因ではあり得ない。削除候補（次の大整理時）。
void lrEnsureAttached() {
  if (!ENABLE_LR_SERVO) return;
  if (!servoLR.attached()) {
    servoLR.attach(LR_SERVO_PIN);
    delay(LR_ATTACH_SETTLE_MS);
    servoLR.write(lrNow);
    delay(LR_ATTACH_SETTLE_MS);
  }
  lrDetachScheduled = false;
  lrLastMoveTime = millis();
}

// ── servoBusy中のMIC一時停止（2026/07/06） ────────────────────────────
// サーボ動作中のギヤ音・筐体振動をMICが拾う根本対策。
// servoBusy==true の間は updateMicInput() を丸ごとスキップ。
// servoBusy が false に戻った後 MIC_SERVO_RESUME_DELAY_MS 経過するまで
// さらに停止を延長し、停止直後の残響も除外する。
// ログは状態遷移時（paused/resumed）のみ出力し、8ms周期の洪水を防ぐ。
const unsigned long MIC_SERVO_RESUME_DELAY_MS = 500;
unsigned long micServoResumeAt  = 0;   // この時刻以降にMIC再開（0=制限なし）
bool          micServoPaused    = false;  // ログ制御用（現在停止中かどうか）

// ── START判定パラメータ（2026/07/06 改訂）─────────────────────────────
// WEAK帯（ON_TH ≤ level < MIC_STRONG_THRESHOLD）：
//   MIC_WEAK_HOLD_MS 継続してからSTART。単発・短時間ノイズを除外。
// STRONG帯（MIC_STRONG_THRESHOLD ≤ level < MIC_SPIKE_LOG_LEVEL）：
//   即START可。ただしスパイククールダウン中は禁止。
// SPIKE帯（level ≥ MIC_SPIKE_LOG_LEVEL）：
//   MIC_SPIKE_COOLDOWN_MS の間 START禁止（reason=spike_cooldown）。
//
// 帯域目安： WEAK=150〜499 / STRONG=500〜599 / SPIKE=600以上
const int           MIC_STRONG_THRESHOLD    = 500;   // STRONG帯の下限（即START対象）
const unsigned long MIC_WEAK_HOLD_MS        = 300;   // WEAK帯でSTARTするのに必要な継続時間
const unsigned long MIC_SPIKE_COOLDOWN_MS   = 1500;  // スパイク後のSTART禁止期間

unsigned long micWeakStartTime  = 0;   // WEAK帯でON_TH超え開始した時刻（0=なし）
unsigned long micSpikeAt        = 0;   // 最後にMIC_SPIKE_IGNOREDが発生した時刻（0=なし）

// ── MICセッション解析用（SESSION START/PEAK/END ログ）──────────────────
unsigned long micSessionStart   = 0;   // MIC SPEAK START した時刻
int           micSessionPeak    = 0;   // セッション中の最大level値
// ─────────────────────────────────────────────────────────────────────
// サーボ動作音・WAV残響・MIC再開直後のスパイクで口パクが始まるのを防ぐ。
// 抑制するのは「START判定」のみ。MIC LEVELログ・STOP判定・発話継続は影響なし。
//
// (1) イベント直後クールダウン：suppressMicStart(ms, reason) で開始。
//     - MUTTER/WAV終了・EAR resume後・MICソース切替直後 : 5000ms
//     - IDLE SERVO END後                               : 3000ms
//     - LR SERVO AUTO DETACH後                         : 2000ms
// (2) 三段確認：ON_TH越えが MIC_START_CONFIRM_WINDOW_MS 以内に3回
//     （MIC_START_CONFIRM_MIN_GAP_MS 以上離れて）出た場合のみSTART。
//     単発・二発ノイズ（level>=300 スパイク含む）ではSTARTしない。
unsigned long micStartSuppressUntil = 0;         // この時刻までSTART禁止
char micStartSuppressReason[12] = "";            // ログ用（mutter/servo/detach）

const unsigned long MIC_START_CONFIRM_WINDOW_MS  = 500;  // 3回目のON_TH越えの有効期限（1回目基準）
const unsigned long MIC_START_CONFIRM_MIN_GAP_MS = 40;   // 各ヒット間の最小間隔（同一ピークの連続ポーリング除外）
const int           MIC_SPIKE_LOG_LEVEL          = 600;  // SPIKE帯の下限（600以上はSPIKE扱い）
unsigned long micOnFirstHitTime = 0;             // 未発話中の1回目ON_TH越え時刻（0=なし）
unsigned long micOnSecondHitTime = 0;            // 未発話中の2回目ON_TH越え時刻（0=なし）
int           micOnFirstHitLevel = 0;            // 1回目越え時のレベル（スパイクログ用）

void suppressMicStart(unsigned long ms, const char* reason) {
  unsigned long until = millis() + ms;
  if (until > micStartSuppressUntil) {
    micStartSuppressUntil = until;
    strncpy(micStartSuppressReason, reason, sizeof(micStartSuppressReason) - 1);
    micStartSuppressReason[sizeof(micStartSuppressReason) - 1] = '\0';
  }
  micOnFirstHitTime  = 0;  // 保留中の1回目ヒットも破棄
  micOnSecondHitTime = 0;
  micOnFirstHitLevel = 0;
  micWeakStartTime   = 0;  // WEAK帯継続判定もリセット
}

// lrMarkMoved(): 左右サーボへのwrite完了後に呼ぶ。detachタイマーをリセット。
void lrMarkMoved() {
  if (!ENABLE_LR_SERVO) return;
  lrLastMoveTime = millis();
  lrDetachScheduled = true;
}

// handleLrAutoDetach(): loop()から毎回呼ぶ。LR_DETACH_DELAY_MS経過後にdetach。
void handleLrAutoDetach() {
  if (!ENABLE_LR_SERVO) return;

  // 発話中はdetachしない（FAILSAFE上限90秒で強制detach）
  if (externalSpeaking) return;

  if (!lrDetachScheduled) return;
  if (servoBusy) return;
  if (!servoLR.attached()) { lrDetachScheduled = false; return; }
  if (millis() - lrLastMoveTime > LR_DETACH_DELAY_MS) {
    servoLR.detach();
    lrDetachScheduled = false;
    addLog("LR SERVO AUTO DETACH");

    // detach音・振動によるMIC SPEAK START誤発火を抑制
    suppressMicStart(2000, "detach");
  }
}

bool speakRequested = false;
char speakPath[80];

bool micEnabled = true;  // ⚠️ レガシー: 現在は書き込みのみで参照箇所なし（旧・常時マイク方式の残骸）。
                         // 音声入力フレームワーク導入により役割喪失。削除候補（次の大整理時）。

void appendToiletBootLog(String resetReasonText) {
  if (!cfg_enableSDLog) return;  // SD_LOG=OFFなら何もしない

  if (!SD.exists("/logtoilet")) {
    SD.mkdir("/logtoilet");
  }

  File logFile = SD.open("/logtoilet/toilet_log.txt", FILE_APPEND);

  if (!logFile) {
    addLog("TOILET BOOT LOG: OPEN FAILED");
    return;
  }

  String ts = getTimestampString();
  int bat = CoreS3.Power.getBatteryLevel();
  logFile.println("================================================");
  logFile.println("BOOT START " + ts + "  RESET=" + resetReasonText + "  BAT=" + String(bat) + "%");
  logFile.println("================================================");

  logFile.close();

  addLog("TOILET BOOT LOG SAVED");
}

// ====================================================
// Web Log
// Serial出力と同時にWebでも確認できる簡易ログ
// ====================================================
String webLog = "";
const int WEB_LOG_MAX_LENGTH = 3000;  // 旧12000。String.remove()のヒープ再配置コスト削減のため縮小。

// ====================================================
// SDログ RAMバッファ（ドライブレコーダー方式）
//
// addLog() のたびにSDへ書かず、まずRAMへ蓄積する。
// ・30秒ごとに定期フラッシュ
// ・重要イベント発生時は即時フラッシュ（直前のRAMログも含む）
// ・sleepMode中の起床判定がSDアクセスでブロックされるのを防ぐ
// ====================================================
const int   SD_LOG_BUFFER_MAX  = 80;   // RAMに保持する最大行数
const int   SD_LOG_LINE_MAX    = 160;  // 1行の最大文字数（120→160: STATE HEARTBEAT+psram欄の切り捨て防止）

// 2026/07/20: 30000 → 10000 へ変更（電源断解析）。
// 電源が瞬断するとRAMバッファ上の未フラッシュ分は失われるため、
// 「ログ末尾＝停止時刻」とみなせる精度がフラッシュ間隔で決まる。
// 30秒では欠損窓が広すぎるため10秒へ短縮する。
// 追加される書き込み量は PWR SNAP 1行/分程度で、SD負荷はほぼ増えない。
const unsigned long SD_LOG_FLUSH_INTERVAL = 10000;  // 定期フラッシュ間隔(ms)

// ====================================================
// Previous Log / 画像記録 上限設定
// ====================================================
const int PREV_LOG_MAX_LINES  = 10000; // karipom_prev.log の最大保持行数
const int COMPRESS_MAX_LINES  = 500;   // Compress 実行後に残す最大行数
const int SNAPSHOT_MAX_COUNT  = 50;    // /logtoilet/*.bmp の最大保持枚数

// 固定長2次元配列（String配列をやめてヒープ断片化を排除）
char        sdLogBuffer[SD_LOG_BUFFER_MAX][SD_LOG_LINE_MAX];  // 定数参照に統一（旧: リテラル[80][120]で定数と乖離していた）
int         sdLogHead    = 0;
int         sdLogCount   = 0;
unsigned long lastSdFlushTime = 0;

// 重要イベントキーワード（addLog() 引数の【前方一致】）
//
// 2026/07/20 全件監査（電源断解析）:
//   判定は msg.startsWith() なので、実ログの「先頭」と一致しないキーワードは
//   一度も発火しない。全 addLog() の先頭トークンを洗い出して突き合わせた結果、
//   以下を修正した。
//
//   【削除】
//     "RESET"  … 実ログは "RESET LR SKIPPED" / "RESET FACE:" のみで、
//                電源解析上の重要イベントではない。目的の
//                "PREV RESET REASON" / "BOOT_RESET_" には前方一致しない。
//     "ERROR"  … 先頭が "ERROR" の addLog() は1件も存在しない（死にキーワード）
//     "PANIC"  … 同上（PANIC は esp_reset_reason 由来で BOOT_RESET_PANIC になる）
//     "WDT"    … 同上（同じく BOOT_RESET_WDT になる）
//     "WAKE START" / "WAKE END" … 該当する addLog() が存在しない
//
//   【追加】
//     "PWR "              … 電源バイタル／電源状態変化イベント（最重要）
//     "PREV RESET REASON" … 前回リセット理由（RTCメモリ生存判定に使う）
//     "BOOT_RESET_"       … 今回のリセット理由
//
//   【変更】
//     "BAT" → "BATTERY"   … "BAT" でも "BATTERY LOW" には前方一致していたが、
//                           意図が読みにくいため実ログに合わせて明示化。
//                           電池挿抜は "PWR BAT ..." 側で捕捉する。
//
//   【維持（実ログの先頭と一致することを確認済み）】
//     "WAKE TRIGGER" / "SLEEP" / "WIFI LOST" / "WIFI CONNECTED" / "WIFI FAILED"
//     "UDP" / "SPEAK" / "BOOT" / "CFG" / "SERVO ATTACH" / "JOY CALIB"
//     "BROWNOUT" / "DRAW CLOCK"
const char* SD_FLUSH_KEYWORDS[] = {
  "PWR ",                                    // 電源バイタル・電源状態変化（最優先）
  "BOOT_RESET_", "PREV RESET REASON",        // リセット理由（前回／今回）
  "WAKE TRIGGER",
  "SLEEP", "WIFI LOST", "WIFI CONNECTED", "WIFI FAILED",
  "UDP", "SPEAK", "BATTERY",
  "BOOT", "CFG", "SERVO ATTACH", "JOY CALIB",
  "BROWNOUT", "DRAW CLOCK",  // 時計描画・電源異常は即保存
  nullptr  // 終端
};

bool sdLogNeedsFlush = false;  // 即時フラッシュ要求フラグ

// ====================================================
// SDログバッファ ドロップ件数（2026/07/20 追加）
//
// sdLogBuffer はリングバッファなので、フラッシュ間隔内に
// SD_LOG_BUFFER_MAX 行を超えると古い行が無言で捨てられていた。
// 「ログに穴が開いたこと」に気付けないと解析を誤るため、
// 捨てた件数を数えて次回フラッシュ時に1行だけ記録する。
//
// ※ この記録処理から addLog() を呼ぶと再帰するため、
//   flushSdLog() 内で logFile へ直接 println する。
uint32_t sdLogDroppedCount = 0;

// ====================================================
// loop() 周回カウンタ（2026/07/20 追加）
//
// PWR SNAP に loops= として載せる。60秒あたりの増分が急減していれば、
// 停止前に何かがloopをブロックしていたと判断できる。
// 電源断とハングを切り分けるための、行数を増やさない診断材料。
// ====================================================
uint32_t loopCounter = 0;

// ====================================================
// flushSdLog()
// RAMバッファの内容をSDカードにまとめて書き出す。
// ・open → 全行write → close を1回で済ませる
// ・sleepMode中の起床判定からは呼ばない（ブロッキング防止）
// ====================================================
void flushSdLog() {
  if (!cfg_enableSDLog || !logToiletEnabled) return;
  if (sdLogCount == 0) return;

  if (!SD.exists(LOG_TOILET_DIR)) SD.mkdir(LOG_TOILET_DIR);

  File logFile = SD.open(LOG_TOILET_FILE, FILE_APPEND);
  if (!logFile) return;

  // バッファあふれで失われた行がある場合、その件数を先頭に1行だけ残す。
  // addLog() を経由すると再帰するため、ここで直接 println する。
  if (sdLogDroppedCount > 0) {
    char dropLine[64];
    snprintf(dropLine, sizeof(dropLine),
             "[%lu] SDLOG DROPPED n=%lu",
             millis(), (unsigned long)sdLogDroppedCount);
    logFile.println(dropLine);
    Serial.println(dropLine);   // シリアル側にも同じ事実を残す
    sdLogDroppedCount = 0;
  }

  int start = (sdLogCount < SD_LOG_BUFFER_MAX) ? 0 : sdLogHead;

  for (int i = 0; i < sdLogCount && i < SD_LOG_BUFFER_MAX; i++) {
    int idx = (start + i) % SD_LOG_BUFFER_MAX;
    logFile.println(sdLogBuffer[idx]);  // char配列は直接println可能
  }

  logFile.close();

  sdLogHead       = 0;
  sdLogCount      = 0;
  sdLogNeedsFlush = false;
  lastSdFlushTime = millis();
}

void addLog(const String& msg) {

  struct tm timeinfo;
  char line[SD_LOG_LINE_MAX];

  // getLocalTime() のデフォルトタイムアウトは5秒。
  // NTP未取得・APモード時に毎回5秒ブロックしていた（sleep復帰遅延の真犯人）。
  // タイムアウト0msを指定して即時失敗させ、[BOOT]タグにフォールバックする。
  if (getLocalTime(&timeinfo, 0)) {
    snprintf(line, sizeof(line),
             "[%04d/%02d/%02d %02d:%02d:%02d][%lu] %s",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
             millis(), msg.c_str());
  } else if (rtcTimeValid) {
    // NTP未取得だがRTC時刻が有効 → RTC由来の時刻で打刻（[RTC]タグ付き）
    // restoreTimeFromRtc()でsettimeofday済みのためgetLocalTimeが成功するはずだが、
    // 万一失敗した場合の保険としてBM8563を直読みする。
    auto dt = CoreS3.Rtc.getDateTime();
    snprintf(line, sizeof(line),
             "[RTC][%04d/%02d/%02d %02d:%02d:%02d][%lu] %s",
             dt.date.year, dt.date.month, dt.date.date,
             dt.time.hours, dt.time.minutes, dt.time.seconds,
             millis(), msg.c_str());
  } else {
    snprintf(line, sizeof(line), "[BOOT][%lu] %s", millis(), msg.c_str());
  }

  // ① Serialログ（即時・常時）
  Serial.println(line);

  // ② Webシステムログ（即時）
  // sleepMode中はWebログ更新をスキップして起床判定を最優先にする。
  // また String.remove() はヒープ再配置で数百ms かかるため、
  // サイズを3000に抑えて remove() 頻度を下げる。
  if (!sleepMode) {
    webLog += line;
    webLog += "<br>";
    if (webLog.length() > WEB_LOG_MAX_LENGTH) {
      webLog.remove(0, webLog.length() - WEB_LOG_MAX_LENGTH);
    }
  }

  // ③ SDログ：RAMバッファに蓄積（直接書き込まない）
  if (cfg_enableSDLog && logToiletEnabled) {

    // 固定長charバッファに安全コピー（String代入によるヒープ断片化を排除）
    strncpy(sdLogBuffer[sdLogHead], line, SD_LOG_LINE_MAX - 1);
    sdLogBuffer[sdLogHead][SD_LOG_LINE_MAX - 1] = '\0';
    sdLogHead = (sdLogHead + 1) % SD_LOG_BUFFER_MAX;
    if (sdLogCount < SD_LOG_BUFFER_MAX) {
      sdLogCount++;
    } else {
      // 満杯のまま上書きした ＝ 最古の1行を失った
      sdLogDroppedCount++;
    }

    // 重要イベントキーワードチェック → 即時フラッシュ要求
    if (!sleepMode) {  // sleepMode中はフラッシュ要求しない
      for (int k = 0; SD_FLUSH_KEYWORDS[k] != nullptr; k++) {
        if (msg.startsWith(SD_FLUSH_KEYWORDS[k])) {
          sdLogNeedsFlush = true;
          break;
        }
      }
    }
  }
}

void updateNose(int newOffset);

// ====================================================
// 電源診断（2026/07/20 追加）— 前方宣言
// 実体は handleDiagnosticHeartbeat() の直前に定義している。
// Arduino IDE の自動プロトタイプ生成に依存しないよう明示的に宣言する。
// ====================================================
void captureAxp2101BootForensics();  // 起動時フォレンジック（CoreS3.begin()直後に1回）
void handlePowerVitals();            // 1秒周期の電源監視＋60秒スナップショット
String powerLoadContext();           // 負荷コンテキスト "S---" 等

// ====================================================
// ログトイレ管理
// 起動時に前回ログを退避し、新しいログを作成する。
// karipom.log      : 今回起動分
// karipom_prev.log : 前回起動分
// ログ肥大化防止と不具合解析を両立する。
// ====================================================
// ====================================================
// prepareLogToilet()
// 起動時に karipom.log の内容を karipom_prev.log へ追記する。
// karipom_prev.log は削除せず長期保存。
// PREV_LOG_MAX_LINES を超えた場合は古い行から切り捨てる。
//
// ログサイズガード（2026/07/07）:
//   巨大ログによる起動遅延・OOM を防ぐため、行カウントの前に
//   ファイルサイズを確認する。上限を超えていれば行カウント・
//   全読み込みをスキップし、old_logs へ退避して新規作成する。
//   退避先が存在しない場合は削除して新規作成する。
//
//   閾値: SAFE_LOG_MAX_BYTES（デフォルト 512KB）
//         = 1行80〜100文字 × 5000〜6000行相当
//         これを超えたら「壊れているか肥大化している」と判断し退避。
// ====================================================

const uint32_t SAFE_LOG_MAX_BYTES = 512UL * 1024UL;  // 512KB

// 指定ファイルを /logtoilet/old_logs/ へ退避する。
// 退避先に書き込めなければ削除して新規作成する。
// 戻り値: "archived" / "deleted" / "skipped"（ファイルが存在しない）
static String archiveOrDeleteLog(const char* srcPath) {
  if (!SD.exists(srcPath)) return "skipped";

  if (!SD.exists("/logtoilet/old_logs")) {
    SD.mkdir("/logtoilet/old_logs");
  }

  // 退避先パス: /logtoilet/old_logs/<元ファイル名>
  //   例: /logtoilet/karipom.log → /logtoilet/old_logs/karipom.log
  const char* slash = strrchr(srcPath, '/');
  String basename = slash ? String(slash + 1) : String(srcPath);
  String dstPath  = "/logtoilet/old_logs/" + basename;

  // 同名が既にある場合は上書き（= 古い退避ファイルを置き換える）
  if (SD.exists(dstPath.c_str())) SD.remove(dstPath.c_str());

  if (SD.rename(srcPath, dstPath.c_str())) {
    return "archived";
  }

  // rename が失敗した場合（SDによっては非対応）はコピーして元を削除
  File src = SD.open(srcPath, FILE_READ);
  File dst = SD.open(dstPath.c_str(), FILE_WRITE);
  bool copyOk = false;
  if (src && dst) {
    const size_t BUF = 256;
    uint8_t buf[BUF];
    while (src.available()) {
      size_t n = src.read(buf, BUF);
      if (n > 0) dst.write(buf, n);
    }
    copyOk = true;
  }
  if (src) src.close();
  if (dst) dst.close();
  SD.remove(srcPath);  // コピー成否に関わらず元を削除
  return copyOk ? "archived" : "deleted";
}

// ====================================================
// compressLogFile(path, keepLines)
// 指定ファイルを末尾 keepLines 行だけ残して切り詰める。
// Web Cockpit の Compress ボタンから呼び出す。
// 戻り値: "compressed" / "skipped"（行数が keepLines 以下）/ "error"
// ====================================================
static String compressLogFile(const char* path, int keepLines) {
  if (!SD.exists(path)) return "skipped";

  // 行数カウント
  int totalLines = 0;
  {
    File f = SD.open(path, FILE_READ);
    if (!f) return "error";
    while (f.available()) {
      if (f.read() == '\n') totalLines++;
    }
    f.close();
  }

  if (totalLines <= keepLines) return "skipped";

  int skipLines = totalLines - keepLines;

  const char* tmpPath = "/logtoilet/compress_tmp.log";
  if (SD.exists(tmpPath)) SD.remove(tmpPath);

  File src  = SD.open(path, FILE_READ);
  File tmp  = SD.open(tmpPath, FILE_WRITE);
  bool ok   = false;

  if (src && tmp) {
    int skipped = 0;
    while (src.available()) {
      String line = src.readStringUntil('\n');
      if (skipped < skipLines) { skipped++; continue; }
      tmp.println(line);
    }
    ok = true;
  }
  if (src) src.close();
  if (tmp) tmp.close();

  if (!ok) {
    if (SD.exists(tmpPath)) SD.remove(tmpPath);
    return "error";
  }

  // --- 元ファイルを削除し、一時ファイルで置き換える ---
  // rename を試み、失敗時はコピー→削除方式にフォールバック。
  // archiveOrDeleteLog() と同方針。
  SD.remove(path);

  if (SD.rename(tmpPath, path)) {
    return "compressed";  // rename 成功
  }

  // rename 失敗（SD環境によっては非対応）→ コピー方式にフォールバック
  addLog("COMPRESS WARN: rename failed, fallback to copy");
  {
    File csrc = SD.open(tmpPath, FILE_READ);
    File cdst = SD.open(path,    FILE_WRITE);
    bool copyOk = false;
    if (csrc && cdst) {
      const size_t BUF = 512;
      uint8_t buf[BUF];
      while (csrc.available()) {
        size_t n = csrc.read(buf, BUF);
        if (n > 0) cdst.write(buf, n);
      }
      copyOk = true;
    }
    if (csrc) csrc.close();
    if (cdst) cdst.close();

    if (copyOk) {
      SD.remove(tmpPath);
      return "compressed";  // コピー成功
    }
  }

  // rename・コピー共に失敗。
  // 圧縮データは compress_tmp.log に保持されており、手動回収可能。
  addLog("COMPRESS ERROR: rename+copy both failed. Data in compress_tmp.log");
  return "error";
}

void prepareLogToilet() {

  if (!SD.exists("/logtoilet")) {
    SD.mkdir("/logtoilet");
  }

  // ── サイズガード: 巨大・破損ログの退避（行カウント前に実施）────────────

  bool prevOversized = false;
  bool curOversized  = false;

  if (SD.exists("/logtoilet/karipom_prev.log")) {
    File f = SD.open("/logtoilet/karipom_prev.log", FILE_READ);
    if (f) {
      uint32_t sz = f.size();
      f.close();
      if (sz > SAFE_LOG_MAX_BYTES) {
        prevOversized = true;
        String result = archiveOrDeleteLog("/logtoilet/karipom_prev.log");
        addLog("PREV LOG OVERSIZE(" + String(sz) + "B): " + result);
      }
    }
  }

  if (SD.exists("/logtoilet/karipom.log")) {
    File f = SD.open("/logtoilet/karipom.log", FILE_READ);
    if (f) {
      uint32_t sz = f.size();
      f.close();
      if (sz > SAFE_LOG_MAX_BYTES) {
        curOversized = true;
        String result = archiveOrDeleteLog("/logtoilet/karipom.log");
        addLog("CUR LOG OVERSIZE(" + String(sz) + "B): " + result);
        // karipom.log が退避されたので、以降の append 処理は不要
        return;
      }
    }
  }

  // karipom.log がなければ（退避済みを含む）何もしない
  if (!SD.exists("/logtoilet/karipom.log")) return;

  // ── 通常処理（ファイルサイズが閾値以内）─────────────────────────────

  // --- karipom_prev.log の既存行数をカウント ---
  int existingLines = 0;
  if (!prevOversized && SD.exists("/logtoilet/karipom_prev.log")) {
    File countFile = SD.open("/logtoilet/karipom_prev.log", FILE_READ);
    if (countFile) {
      while (countFile.available()) {
        if (countFile.read() == '\n') existingLines++;
      }
      countFile.close();
    }
  }

  // --- karipom.log の行数をカウント ---
  int newLines = 0;
  {
    File countFile = SD.open("/logtoilet/karipom.log", FILE_READ);
    if (countFile) {
      while (countFile.available()) {
        if (countFile.read() == '\n') newLines++;
      }
      countFile.close();
    }
  }

  int totalLines = existingLines + newLines;

  // --- 上限を超える場合: 古い行をスキップして temp に書き出す ---
  if (totalLines > PREV_LOG_MAX_LINES && SD.exists("/logtoilet/karipom_prev.log")) {
    int skipLines = totalLines - PREV_LOG_MAX_LINES;

    File srcFile  = SD.open("/logtoilet/karipom_prev.log", FILE_READ);
    File tempFile = SD.open("/logtoilet/karipom_temp.log", FILE_WRITE);

    if (srcFile && tempFile) {
      int skipped = 0;
      while (srcFile.available()) {
        String line = srcFile.readStringUntil('\n');
        if (skipped < skipLines) {
          skipped++;
        } else {
          tempFile.println(line);
        }
      }
      srcFile.close();
      tempFile.close();

      SD.remove("/logtoilet/karipom_prev.log");
      SD.rename("/logtoilet/karipom_temp.log", "/logtoilet/karipom_prev.log");
      addLog("PREV LOG TRIMMED: removed " + String(skipLines) + " old lines");
    } else {
      if (srcFile)  srcFile.close();
      if (tempFile) tempFile.close();
    }
  }

  // --- karipom.log を karipom_prev.log に追記 ---
  File srcLog  = SD.open("/logtoilet/karipom.log", FILE_READ);
  File prevLog = SD.open("/logtoilet/karipom_prev.log", FILE_APPEND);

  if (srcLog && prevLog) {
    while (srcLog.available()) {
      prevLog.println(srcLog.readStringUntil('\n'));
    }
    srcLog.close();
    prevLog.close();
    SD.remove("/logtoilet/karipom.log");
    addLog("PREV LOG APPENDED: total ~" + String(min(totalLines, PREV_LOG_MAX_LINES)) + " lines");
  } else {
    if (srcLog)  srcLog.close();
    if (prevLog) prevLog.close();
  }
}

// ====================================================
// Mac音声連動モード
// BlackHole等でMac音声を検知したPythonからUDPで
// SPEAK_START / SPEAK_STOP を受け取り、口パクへ反映する。
// Web画面のボタンでON/OFFする。
// ====================================================
bool macAudioLinkEnabled = true;  // デフォルトON

// ===== KARIPOM EAR v2 (Phase 0 / Step 3+4) =====
// Step 3：UART受信診断（統計のみ）
// Step 4.1：FFT行を厳密パースし fftLevel[]/lastFftPacketTime へ供給。
//   入力源はWebUIの音声ソース選択と一致させる（v1.1整合修正）：
//     LINE IN選択＝Ear専用（UDP FFTは読み捨て）
//     UDP選択＝Mac専用（Ear FFTは統計カウントのみ・反映しない）
//     内蔵マイク/OFF＝従来どおり（どちらのFFTも表示に使わない）
//   LINE IN選択中にEarが途絶した場合：バーは既存仕様どおり約0.5秒で減衰し
//   通常顔へ復帰する。UDPへの自動切替は行わない（切替はWebUIの明示操作のみ）。
// SPEAK_START/STOP は externalSpeaking へ未接続（口パクはStep 5）。
// EAR_UART_ENABLED を未定義＝既存と同一バイナリ（安全弁）。
// EAR_FFT_TO_FACE_ENABLED を未定義＝Step 3相当（受信統計のみ）へ後退。
// Step 5.1：Ear SPEAK_START/STOP を externalSpeaking へ接続（口パク）。
//   LINE IN選択中のみ有効。UDP選択中はEar SPEAKは統計のみ、Mac UDPが従来どおり駆動。
//   LINE IN選択中にEarが途絶した場合：既存のSPEAK_TIMEOUT_MS(5秒)watchdogが
//   口を閉じる（watchdogはソース非依存・無変更）。UDPへの自動切替はしない。
#define EAR_UART_ENABLED
#ifdef EAR_UART_ENABLED
#define EAR_FFT_TO_FACE_ENABLED     // Step 4機能フラグ（コメントアウトで無効化）
#define EAR_SPEAK_TO_MOUTH_ENABLED  // Step 5機能フラグ（コメントアウト＝Step 4.1相当へ後退）
void handleEarUart();  // 明示プロトタイプ（既存コードの流儀に合わせる）
bool earFftFresh();    // Ear FFTが400ms以内に届いているか（統計表示用）

const unsigned long EAR_STATS_INTERVAL_MS = 10000;  // 統計ログ間隔

char          earRxLineBuf[64];      // 行バッファ（String不使用・断片化回避）
uint8_t       earRxLen        = 0;
unsigned long earRxLines      = 0;   // 累計：確定した行数
unsigned long earRxFft        = 0;   // 累計：正しい書式のFFT行
unsigned long earRxSpeakStart = 0;   // 累計：SPEAK_START
unsigned long earRxSpeakStop  = 0;   // 累計：SPEAK_STOP
unsigned long earRxBadLines   = 0;   // 累計：解釈不能・過長・書式不正
unsigned long earRxLastMs     = 0;   // 最終受信時刻（millis）
unsigned long earLastStatsMs  = 0;
unsigned long earPrevLines    = 0;   // 10秒窓の増分計算用
unsigned long earPrevFft      = 0;
// --- Step 4 追加 ---
const unsigned long EAR_FFT_HOLD_MS = 400;  // Ear優先の保持時間（＝この途絶でUDPへ復帰）
unsigned long earLastFftOkMs  = 0;   // 最後に8値パース成功したFFT行の時刻
unsigned long earFftApplied   = 0;   // fftLevel[]へ反映した累計回数
unsigned long earPrevApplied  = 0;   // 10秒窓の増分計算用
unsigned long earSpeakApplied = 0;   // Step 5：Earが口パクを開始した累計回数
#endif
// ===== KARIPOM EAR v2 ここまで =====

String lastUdpState = "";

// ===== FFT Visualizer 受信値 =====
// Python側から "FFT:25,13,0,0,0,0,0,0"（8バンド・0〜100）が約12.5回/秒届く。
// handleUDP()は値の保存のみ行い、描画はloop()側（FFT_DISPLAY_TEST時のみ）。
uint8_t fftLevel[8] = {0};
unsigned long lastFftPacketTime = 0;
const unsigned long FFT_RX_TIMEOUT_MS = 400;  // 受信途絶とみなす時間（バー減衰開始）

// ====================================================
// 音声セッション継続判定（v1.2）— スリープ抑制・上端パネルの共通判定
//
// これまで「口パク中か」を externalSpeaking だけで判定していたため、
// Wi-Fi/LINE INで音楽が流れFFTが届いていても externalSpeaking が一瞬でも
// false になるとスリープ判定を素通りしてしまう不具合があった。
// 下の共通関数に一本化し、口パク・Visualizer・Lighting・スリープが
// 別々の「音声継続判定」を持たないようにする。
// ====================================================
// FFTが「新鮮」とみなす時間。Visualizer側の FFT_FACE_ACTIVE_MS(=500) と同値。
const unsigned long AUDIO_SESSION_FRESH_MS = 500;

// 音声セッションが継続中か（スリープ抑制に使う）。
//   ・externalSpeaking（発話・口パク中）
//   ・または Wi-Fi/LINE IN から新鮮なFFTパケットが届いている
// 「単に音源がUDP/LINE INに選択されているだけ」では true にしない。
bool isActiveAudioSession() {
  if (externalSpeaking) return true;
  if ((audioSource == AUDIO_SRC_UDP || audioSource == AUDIO_SRC_LINEIN) &&
      lastFftPacketTime != 0 &&
      (millis() - lastFftPacketTime) <= AUDIO_SESSION_FRESH_MS) {
    return true;
  }
  return false;
}

// Lighting本体の描画継続判定（定義は #ifdef FFT_DISPLAY_TEST 側）。2026-08-09追加：
// 直下のlightingScreenActive()から、上端パネルの判定にも同じ猶予を適用するために呼ぶ
// （前方宣言。理由は直下のlightingScreenActive()のコメント参照）。
#ifdef FFT_DISPLAY_TEST
bool lightingActiveWithGrace();
#endif

// 現在“本当にLighting画面を表示中か”（上端の黒い情報パネルを出す条件）。
// screenFxLighting が状態遷移（スリープ／画像顔）をまたいで残っても、
// ここで sleep/imageFace/音停止/マスク0 を見て確実に打ち消す。
//
// 2026-07-25: 内蔵マイク(AUDIO_SRC_MIC)選択時もLightingを利用可能にする。
//   UDP/LINE INの「FFT新鮮判定」は、外部ソース（PC/Ear）が実際に接続・
//   送信中かを見極めるための仕組みで、常時オンボードで動き続ける内蔵マイクには
//   そもそも当てはまらない（lastFftPacketTimeはUDP/LINE IN受信時のみ更新されるため、
//   MIC選択中は絶対に更新されず、この条件を課すとLightingが永久に有効化されない）。
//   そのためMIC選択時のみFFT新鮮判定を免除し、他条件（sleep/imageFace/マスク0判定）は
//   従来どおり適用する。Audio Visualizer側(isVisualizerFaceEnabled)は変更していないため、
//   MIC選択時にVisualizerが表示されない既存仕様は維持される。
//
// 2026-08-09修正（実機確認：Lighting表示中に上部2行が一瞬白くなる不具合）:
//   【原因】上端パネルの黒/白判定はここでAUDIO_SESSION_FRESH_MS(500ms)の
//   FFT新鮮判定のみで行っていたが、Lighting本体の描画継続判定
//   （updateScreenEffects()が使うlightingActiveWithGrace()）には既に
//   LIGHTING_FFT_GRACE_MS(3秒)の猶予があり、両者の基準がずれていた。
//   UDP/LINE IN受信間隔が500ms〜3秒だけ空いた瞬間、Lighting本体（背景・
//   隕石・トンネル等）は猶予内なので継続描画されるのに対し、上端パネルは
//   猶予なしのこの関数がfalseを返し、handleSensorDisplay()の独立した
//   300ms周期タイマーがshowSensors()を呼んだ瞬間に上端2行だけ白く
//   塗り直されてしまっていた（Aquarium/Flying Pompadour固有ではなく、
//   Tempest Tunnel等の既存Lightingでも同じ経路で起きうる一般的な不具合）。
//   【対策】AUDIO_SESSION_FRESH_MS以内なら従来どおり即trueとし、それを
//   超えた場合のみlightingActiveWithGrace()と同じ猶予判定にフォールバック
//   する。これにより上端パネルとLighting本体の「表示継続中か」の判定が
//   完全に一致し、白フラッシュが発生しなくなる。sleep/imageFace/マスク0の
//   判定・MICの扱い・showSensors()/drawBattery()の呼び出し方は変更していない。
bool lightingScreenActive() {
  if (!(screenFxLighting && !sleepMode && !imageFaceMode && cfg_lightingMask != 0)) return false;
  if (audioSource == AUDIO_SRC_MIC) return true;
  if (!(audioSource == AUDIO_SRC_UDP || audioSource == AUDIO_SRC_LINEIN)) return false;
  if (lastFftPacketTime != 0 && (millis() - lastFftPacketTime) <= AUDIO_SESSION_FRESH_MS) return true;
#ifdef FFT_DISPLAY_TEST
  return lightingActiveWithGrace();
#else
  return false;
#endif
}

#ifdef FFT_DISPLAY_TEST
// Lighting合成モードの統一終了処理（定義は #ifdef FFT_DISPLAY_TEST 側・前方宣言）。
void exitLightingCompositeMode(bool redrawTopPanel);
// Sleep Lighting Carousel（定義は #ifdef FFT_DISPLAY_TEST 側・LIGHT_RENDER_FN[]の後）。
// handleSleepMode()（本ファイル前方）から呼ぶための前方宣言。
void updateSleepLightingCarousel();
#endif

// フェイルセーフ用タイムアウト
// 当初は7秒だったが、YouTubeやBGM用途では
// 曲間や一時停止で誤検出し、口パクが停止した。
// 常設ロボット運用を考慮し、3時間へ延長。
// （2026/06 デバッグ記録）

unsigned long lastExternalSpeakTime = 0;
const unsigned long EXTERNAL_SPEAK_TIMEOUT = 10800000;  // 3時間
unsigned long lastMouthPakuTime = 0;
bool mouthPakuOpen = false;

// Mac音声連動中の「話している時の微動」
// 口パクだけだと直立不動に見えるので、1〜2秒ごとにごく小さく首を揺らす。
unsigned long lastTalkMicroMoveTime = 0;
unsigned long nextTalkMicroMoveInterval = 1200;

int lastTalkTargetUD = HEAD_VERTICAL_CENTER;
int lastTalkTargetLR = HEAD_HORIZONTAL_CENTER;

const int TALK_MICRO_UD_RANGE   = 3;    // 上下方向の頷き幅（servoUD=上下サーボ）
const int TALK_MICRO_LR_RANGE = 3;    // 左右方向の揺れ幅（servoLR=左右サーボ）
const int TALK_MICRO_MOVE_WAIT_MS = 20;  // 微動時の1度あたり待ち時間（少し遅く＝滑らか）

unsigned long lastSpeakPacketTime = 0;
const unsigned long SPEAK_TIMEOUT_MS = 5000;
bool micEarPaused = false;  // 独り言WAV再生中のマイク解析一時停止フラグ（AUDIO_SRC_MIC専用）

// ====================================================
// 🎙 内蔵マイクモード（AUDIO_SRC_MIC）のチューニング値
//
// CoreS3のPDMマイクとスピーカーは同一I2Sを共有するため、
// 内蔵マイクモード中は setAudioSource() が Speaker.end()→Mic.begin() を行い、
// isSpeakerAllowed()=false となって全スピーカー再生
// （mutter/yawn/hungry/tone/その他すべて）が停止する。
// →「周囲の音に反応する専用モード」（フラワーロック方式）。
//
// レベル判定：8msぶん(128サンプル@16kHz)の平均絶対値をEMA平滑して micLevel とし、
// ヒステリシス（ON/OFFしきい値＋リリース保持時間）で externalSpeaking を駆動する。
// externalSpeaking 以降の口パク・微動は UDP方式と完全に共通。
//
// しきい値は実機の環境音で要調整。DEBUG_MIC_LEVEL=true にすると
// 2秒ごとに MIC LEVEL ログが出るので、静音時/発話時の値を見て決める。
// ====================================================
const uint32_t MIC_SAMPLE_RATE  = 16000;
const size_t   MIC_REC_SAMPLES  = 128;   // 128サンプル ≒ 8ms/loop（ブロッキング許容範囲）

int micLevel = 0;                        // 平滑済みマイクレベル（audioLevelへ反映）
unsigned long micLastAboveTime = 0;

const bool DEBUG_MIC_LEVEL = true;       // 初期しきい値調整用。調整完了後は false 推奨

// UDPモード時の audioLevel 値（SPEAK_START/STOPの2値のため固定値で表現）
// 将来Python側が音量値を送るようになったらここを実測値に置き換える。
const int AUDIO_LEVEL_UDP_ACTIVE = 512;

bool isAfterExternalSpeakGrace() {
  return millis() - lastExternalStopTime > AFTER_EXTERNAL_SPEAK_GRACE;
}

bool canDoIdleAction() {
  // Web操作直後 WEB_ACTION_IDLE_SUPPRESS_MS の間は自動IDLE/MUTTERを抑制。
  // millis()ロールオーバー安全のため減算比較を用いる。
  if (lastWebServoCmdTime != 0 &&
      (millis() - lastWebServoCmdTime) < WEB_ACTION_IDLE_SUPPRESS_MS) {
    return false;
  }
  return !sleepMode && !alertMode && !petMode && !externalSpeaking && isAfterExternalSpeakGrace();
}

bool canSleep() {
  return !sleepMode && !alertMode && !petMode;
}

// ====================================================
// 安全設定：まずはWeb操作を基準にする
// ====================================================
// trueにするとカメラ差分で左右を見る。安定確認後にONにする。
const bool ENABLE_CAMERA_LOOK = false;  // v37: カメラによる方向付き首追従はOFF。方向追従は人感センサー追加後に再検討

// trueにすると待機中にランダムで首を動かす。安定確認後にONにする。
const bool ENABLE_IDLE_SERVO = true;  // v38: 控えめな待機中ランダム首振りを復活

// trueにすると睡眠移行/起床時に上下首振りを行う。安定確認後にONにする。
const bool ENABLE_SLEEP_WAKE_SERVO = false;

// ====================================================
// サーボ軸個別有効化スイッチは ENABLE_UD_SERVO / ENABLE_LR_SERVO として
// ファイル先頭側（LRサーボ自動detach管理ブロックの直前）に移動済み。

// ====================================================
// デバッグログフラグ
// true にするとカテゴリごとの詳細ログが出力される。
// 通常運用時はすべて false にしてログを読みやすくする。
// ====================================================
const bool DEBUG_IDLE      = false;  // IDLE CHECK elapsed/interval
const bool DEBUG_SLEEP     = false;  // SLEEP MOTION
const bool DEBUG_SERVO     = false;  // RESTORE SERVO RTC詳細
const bool DEBUG_JOYSTICK  = false;  // JOY STUCK/RECOVER/TIMEOUT/ABNORMAL
const bool DEBUG_ENV       = false;  // ENV V= A= M=（30秒ごと）

// ジョイスティック有効フラグ
// 起動時は常に false で開始。calibrateJoystick() は no-op スタブに変更済み。
// handleJoystick() のランタイム安定確認 → recalibrateJoystickRuntime() で
// 接続を確認してから true になる。起動中の接続タイミングに依存しない。
bool joystickEnabled = false;  // 起動時は常に false。runtime recalib で ON になる

// WiFi
bool connectWiFiFromSD();
String readWifiTxt();
void startAPMode();
void handleWiFiReconnect();

//Web
void handleSaveWifi();

// 通信
void handleSpeakRequest();
void handleCommunication();
void handleDiagnosticHeartbeat();
void handleUDP();
void updateExternalMouth();

// 音声入力フレームワーク
void updateAudioInput();
void updateMicInput();
int  getAudioLevel();
bool isSpeakerAllowed();
void applyBootAudioSource();
void pauseKaripomEarForMutter();
void resumeKaripomEarAfterMutter();
bool restoreTimeFromRtc();
void saveTimeToRtc();

// 睡眠
bool handleSleepMode(bool touchedHead, bool cameraStimulus, bool imuStimulus, bool soundStimulus);
bool handleSleepTransition();
bool handleStandbyGate();

// 動作制御
void handleIdleServoMotion();
void handlePetTouch(bool touchedHead);
void handleSoundAlert(bool soundStimulus);
void handleSoundActivity(int volume);
bool isHeadTouched();
void handleJoystick();
void applyJoyCalibThresholds();
void recalibrateJoystickRuntime(int stableCenterX, int stableCenterY,
                                int meanDevX,       int meanDevY);

// 表情・動き
void handleNoseMotion();
void handleBlinkMotion();
void handleAlertRelease();

// センサー
int updateVolume();
void handleSensorDisplay();
bool updateCameraStimulus();
bool updateImuStimulus();
bool updateSoundStimulus(int volume);

//独り言
void handleRandomMutter();
void playMutterOnce(const char* reason);
void playWavFromSD(const char* path, bool skipMouthAnim = false);

// pickMutterFromFolder() の結果種別。
// ※Arduino IDE の自動プロトタイプ生成より前に型を確定させるため、ここで定義する。
//   MUTTER_PICK_FOUND     : 対象wavが決まった（outPath 有効）
//   MUTTER_PICK_NO_FOLDER : /sounds/mutter フォルダが開けない → 旧方式フォールバック可
//   MUTTER_PICK_EMPTY     : フォルダはあるが .wav が0件 → 旧方式フォールバック可
//   MUTTER_PICK_TIMEOUT   : 探索が時間超過 → フォールバックせず中止（存在しない旧ファイル再生を防ぐ）
enum MutterPickResult {
  MUTTER_PICK_FOUND,
  MUTTER_PICK_NO_FOLDER,
  MUTTER_PICK_EMPTY,
  MUTTER_PICK_TIMEOUT
};
MutterPickResult pickMutterFromFolder(char* outPath, size_t outSize);

// mutterフォルダ方式ヘルパーが本体定義より前で使うため前方宣言
bool isWavFilename(const String& name);

// ページ共通ヘッダ/フッタをログストリーミングが先に使うため前方宣言
String karipomPageHeader(const String& title);
String karipomPageFooter();
String htmlEscape(String text);


bool connectWiFiFromSD() {
  bool wifiConnected = false;

  File wifiFile = SD.open("/wifi.txt");

  if (wifiFile) {
    addLog("wifi.txt: OPEN OK");

    while (wifiFile.available() && !wifiConnected) {
      String line = wifiFile.readStringUntil('\n');
      line.trim();

      if (line.length() == 0) continue;
      if (line.startsWith("#")) continue;

      int commaPos = line.indexOf(',');

      if (commaPos > 0) {
        String ssid = line.substring(0, commaPos);
        String password = line.substring(commaPos + 1);

        ssid.trim();
        password.trim();

        addLog("TRY SSID = " + ssid);
        // Serial.println("PASS=[" + password + "]");
        // Serial.print("SSID length=");
        // Serial.println(ssid.length());
        // Serial.print("PASS length=");
        // Serial.println(password.length());

        drawBootFace();

        CoreS3.Display.fillRect(0, 26, 320, 24, WHITE);
        CoreS3.Display.setTextColor(PURPLE);
        CoreS3.Display.setTextSize(2);
        CoreS3.Display.drawString("CONNECTING...", 5, 26);

        CoreS3.Display.fillRect(0, 50, 320, 24, WHITE);
        CoreS3.Display.drawString(ssid, 5, 50);

        if (!ssidFoundInScan(ssid)) {
          addLog("SSID NOT FOUND: " + ssid);
          continue;
        }

        addLog("CONNECTING TO " + ssid);

        drawBootFace();

        CoreS3.Display.fillRect(0, 26, 320, 24, WHITE);
        CoreS3.Display.setTextColor(PURPLE);
        CoreS3.Display.setTextSize(2);
        CoreS3.Display.drawString("WiFi Scan...", 5, 26);

        CoreS3.Display.fillRect(0, 50, 320, 24, WHITE);
        CoreS3.Display.drawString(ssid, 5, 50);

        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.disconnect(true);
        smartDelay(500);

        WiFi.begin(ssid.c_str(), password.c_str());

        unsigned long wifiStart = millis();

        while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 8000) {
          smartDelay(500);
          Serial.print(".");
          Serial.print(" ");
          Serial.println(WiFi.status());
        }

        Serial.println();

        addLog("WiFi.status() = " + String(WiFi.status()));

        if (WiFi.status() == WL_CONNECTED) {
          wifiConnected = true;

          WiFi.setSleep(false);
          addLog("WiFi Sleep OFF");

          addLog("WIFI CONNECTED");
          addLog("SSID = " + ssid);
          addLog("IP = " + WiFi.localIP().toString());

          // Wi-Fi接続中はBootFaceで待機
          CoreS3.Display.fillScreen(WHITE);
          drawBootFace();
          smartDelay(1500);

        } else {
          addLog("WIFI FAILED");
          addLog("FAILED SSID = " + ssid);
          addLog("WiFi Status = " + String(WiFi.status()));

          WiFi.disconnect(true);
          smartDelay(500);
        }
      }
    }

    wifiFile.close();
  }

  return wifiConnected;
}

String readWifiTxt() {
  String text = "";

  File file = SD.open("/wifi.txt", FILE_READ);

  if (!file) {
    return "";
  }

  while (file.available()) {
    text += (char)file.read();
  }

  file.close();

  return text;
}

void loadAPConfigFromSD() {
  File file = SD.open("/wifi.txt", FILE_READ);

  if (!file) {
    addLog("AP CONFIG: wifi.txt not found. Use default AP.");
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;

    if (line.startsWith("AP_NAME=")) {
      apName = line.substring(String("AP_NAME=").length());
      apName.trim();
    }

    if (line.startsWith("AP_PASS=")) {
      apPass = line.substring(String("AP_PASS=").length());
      apPass.trim();
    }
  }

  file.close();

  if (apName.length() == 0) apName = "KariPom_AP";
  if (apPass.length() < 8) apPass = "karipom123";

  addLog("AP CONFIG SSID = " + apName);
  addLog("AP CONFIG PASS LENGTH = " + String(apPass.length()));
}

void startAPMode() {

  //  drawFace();
  //  showSensors();
  drawBootFace();

  CoreS3.Display.fillRect(0, 26, 320, 24, WHITE);
  CoreS3.Display.setTextSize(2);
  CoreS3.Display.setTextColor(RED);
  CoreS3.Display.drawString("NO WIFI FOUND", 5, 26);

  playWavFromSD("/sounds/wifi_failed_1.wav");

  // smartDelay(3000);  // 起動遅延の原因なので削除

  loadAPConfigFromSD();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), apPass.c_str());

  IPAddress apIP = WiFi.softAPIP();

  addLog("AP MODE START");
  addLog("AP SSID = " + apName);
  addLog("AP IP = " + apIP.toString());

  CoreS3.Display.fillRect(0, 26, 320, 24, WHITE);
  drawIpStatusOnly();
}

void handleWiFiReconnect() {
  if (millis() - lastWiFiCheckTime > WIFI_CHECK_INTERVAL) {
    lastWiFiCheckTime = millis();

    bool nowConnected = (WiFi.status() == WL_CONNECTED);

    if (WiFi.getMode() == WIFI_STA && !nowConnected) {
      if (!wifiReconnectRunning) {
        addLog("WIFI LOST");
        WiFi.reconnect();
        wifiReconnectRunning = true;
      }
    }

    if (wifiReconnectRunning && nowConnected) {
      addLog("WIFI RECONNECTED");

      udp.stop();
      smartDelay(100);  // 旧:delay(100) → Web/UDP を止めずに待機
      udp.begin(UDP_PORT);
      addLog("UDP REBOUND");

      wifiReconnectRunning = false;
    }
  }
}

void handleCommunication() {
  server.handleClient();
  handleUDP();
#ifdef EAR_UART_ENABLED
  handleEarUart();   // KARIPOM EAR v2 (Step 3)
#endif
  updateAudioInput();      // 音声入力フレームワーク：選択ソース→audioLevel/externalSpeaking変換
  updateExternalMouth();
  yield();  // delay(1)はブロッキング。yieldでWDTを解消しつつ即時リターン

  handleWiFiReconnect();

  // SDログフラッシュ処理
  // sleepMode中は起床判定を優先するためフラッシュしない。
  // 覚醒中のみ、定期フラッシュまたは即時フラッシュ要求があれば実行。
  if (!sleepMode && cfg_enableSDLog && logToiletEnabled) {
    if (sdLogNeedsFlush ||
        millis() - lastSdFlushTime > SD_LOG_FLUSH_INTERVAL) {
      flushSdLog();
    }
  }

  // ====================================================
  // 定期ヒープ監視ログ
  // フラッシュと同タイミング（30秒ごと）で出力する。
  // WAV再生前後のヒープ変動と組み合わせてメモリリークを検出する。
  // ====================================================
  static unsigned long lastHeapLogTime = 0;
  const unsigned long HEAP_LOG_INTERVAL = 300000UL;  // 5分ごと（30秒だと多すぎる）
  if (!sleepMode && millis() - lastHeapLogTime > HEAP_LOG_INTERVAL) {
    lastHeapLogTime = millis();
    char hbuf[160];
    snprintf(hbuf, sizeof(hbuf),
             "HEAP MONITOR: free=%u minFree=%u largest=%u psram=%u minPsram=%u",
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMinFreeHeap(),
             (unsigned)ESP.getMaxAllocHeap(),
             (unsigned)ESP.getFreePsram(),      // PSRAM残量（未搭載/無効なら0）
             (unsigned)ESP.getMinFreePsram());  // PSRAM史上最小残量（リーク検出用）
    addLog(hbuf);
  }

  // ====================================================
  // 診断ハートビート（30秒ごとのLOOP/STATE、鼻60秒停止のNOSE STALL）
  // handleCommunication() は loop() 冒頭・handleStandbyGate()（自動スリープ判定）より前で毎回呼ばれるため、
  // sleep中やゲートreturn時でもハートビートが途切れない。
  // ====================================================
  handleDiagnosticHeartbeat();

  // ====================================================
  // 電源バイタル（2026/07/20 追加・常時有効）
  //
  // ハートビートと同じ理由でここに置く。handleStandbyGate() の
  // 早期returnより前で呼ばれるため、sleepMode中も電源監視が途切れない。
  // 実処理は1秒に1回だけ（内部でmillis()判定）。
  // ====================================================
  handlePowerVitals();
}

// ============================================================================
// ============================================================================
//  電源診断（2026/07/20 追加）
//
//  目的：
//    「ごく稀に完全に電源OFFになる」現象について、次回起動時に原因を判別する。
//
//  設計方針（Power Debugモードは作らない）：
//    層B: 起動時フォレンジック … AXP2101のIRQラッチを起動時に1回だけ生値で読む
//    層A: 常時電源バイタル     … 1秒周期監視 + 60秒スナップショット + 変化点イベント
//
//    稀にしか起きない事象に「デバッグモード」は噛み合わない
//    （ONにしている時に限って起きない）ため、いずれも常時有効とする。
//
//  なぜ起動時フォレンジックが決定打なのか：
//    AXP2101 の IRQ ステータスレジスタ(0x48/0x49/0x4A)は、AXP2101自身に
//    入力（電池 or VBUS）が残っている限り、DCDC/LDO出力を落としても
//    内容が保持される。つまり「AXP2101が何を理由に出力を切ったか」を
//    次回起動時に読み出せる。ESP32側のRTCメモリは電源断で消えるため、
//    これが唯一デバイス内に残る証拠となる。
//
//  CoreS3 での制約（M5Unified 実ソースで確認済み）：
//    ・電流は一切取得できない
//        AXP2101_Class::getBatteryChargeCurrent()    → return 0（未実装）
//        AXP2101_Class::getBatteryDischargeCurrent() → return 0（未実装）
//        AXP2101_Class::getVBUSCurrent()             → return 0（未実装）
//        Power_Class::getBatteryCurrent()            → CoreS3分岐で return 0
//      よって突入電流は「電圧サグ」と「その瞬間の負荷」の相関から推定する。
//    ・isACIN() / getACINVoltage() / getACINCurrent() / getAPSVoltage()
//      / getBatteryPower() も return false / return 0 のスタブ。使用しない。
//    ・getInternalTemperature() は
//        return 22 + ((7274 - readRegister16(0x3C)) / 20);
//      で readRegister16() の戻り値が std::size_t（符号なし）のため、
//      生値が7274を超えると符号なし減算がアンダーフローする。使用しない。
//      （PMIC過熱は IRQ の DIE_OVER_TEMP ビットで検出する）
// ============================================================================
// ============================================================================

// ── AXP2101 レジスタ番号 ────────────────────────────────────────────────
// M5Unified src/utility/power/AXP2101_Class.cpp / .hpp で実際に使われている
// レジスタ番号をそのまま定数化したもの（推測値ではない）。
//   0x00 : PMU status1  bit5=VBUS good（isVBUS()）/ bit3=BAT present（getBatState()）
//   0x01 : PMU status2  bit6-5=充電状態（getChargeStatus() / isCharging()）
//   0x48 : IRQ status 0（AXP2101_IRQSTAT0）
//   0x49 : IRQ status 1（AXP2101_IRQSTAT1）
//   0x4A : IRQ status 2（AXP2101_IRQSTAT2）
static const uint8_t AXP2101_REG_STATUS1  = 0x00;
static const uint8_t AXP2101_REG_STATUS2  = 0x01;
static const uint8_t AXP2101_REG_IRQSTAT0 = 0x48;
static const uint8_t AXP2101_REG_IRQSTAT1 = 0x49;
static const uint8_t AXP2101_REG_IRQSTAT2 = 0x4A;

// IRQ ENABLE レジスタ（enableIRQ() の書き込み先）。
// 2026/07/20 追加: enableIRQ() の戻り値が信用できないため、
// 書き込み後にこの3本を生読みして期待値と突き合わせる。
static const uint8_t AXP2101_REG_IRQEN0   = 0x40;
static const uint8_t AXP2101_REG_IRQEN1   = 0x41;
static const uint8_t AXP2101_REG_IRQEN2   = 0x42;

// ── 起動時フォレンジックの生値 ──────────────────────────────────────────
// setup() の SD初期化前に読み出すため、SD直書き用に保持しておく。
uint8_t axpBootIrq48   = 0;
uint8_t axpBootIrq49   = 0;
uint8_t axpBootIrq4A   = 0;
uint8_t axpBootStat00  = 0;
uint8_t axpBootStat01  = 0;
bool    axpBootForensicDone = false;
String  axpBootForensicLine;   // "PWR BOOT IRQ48=0x.. ..."（SD直書き用・一次情報）
String  axpBootCauseLine;      // "PWR BOOT CAUSE ..."（SD直書き用）
String  axpBootInfoLine;       // "PWR BOOT INFO ..."（情報ビット。無ければ空）
String  axpBootUnmappedLine;   // "PWR BOOT UNMAPPED ..."（未解釈ビット。無ければ空）
String  axpBootIrqEnLine;      // "PWR BOOT IRQEN ..."（有効化の読戻し検証結果）

// ====================================================
// AXP2101 IRQステータス ビット名テーブル（全24ビットを網羅）
//
// 2026/07/20 修正:
//   初版は「電源断に直結するビット」しかデコードしていなかったため、
//   実機で IRQ48=0x10（bit4 = GAUGE_NEW_SOC）がラッチされていたのに
//   CAUSE NONE と表示され、「IRQなし」と誤解する出力になっていた。
//   全24ビットに名前を与え、さらに
//     critical … 電源断の原因になり得るビット
//     info     … 正常動作でも立つビット（電源断の原因ではない）
//   に分類する。どちらにも属さないビットが立っていた場合は
//   UNMAPPED として生値ごと表示し、見落としを防ぐ。
//
// ビット定義は M5Unified の axp2101_irq_t に対応（実ソース確認済み）:
//   IRQSTAT0(0x48) : bit0..7 = BAT_UNDER_TEMP .. WARNING_LEVEL2
//   IRQSTAT1(0x49) : bit0..7 = PKEY_POSITIVE_EDGE .. VBUS_INSERT
//   IRQSTAT2(0x4A) : bit0..7 = BAT_OVER_VOLTAGE .. WDT_EXPIRE
// ====================================================
static const char* AXP_IRQ48_NAME[8] = {
  "BAT_UNDER_TEMP",     // bit0  AXP2101_IRQ_BAT_UNDER_TEMP
  "BAT_OVER_TEMP",      // bit1  AXP2101_IRQ_BAT_OVER_TEMP
  "BAT_CHG_UNDER_TEMP", // bit2  AXP2101_IRQ_BAT_CHG_UNDER_TEMP
  "BAT_CHG_OVER_TEMP",  // bit3  AXP2101_IRQ_BAT_CHG_OVER_TEMP
  "GAUGE_NEW_SOC",      // bit4  AXP2101_IRQ_GAUGE_NEW_SOC
  "GAUGE_WDT_TIMEOUT",  // bit5  AXP2101_IRQ_GAUGE_WDT_TIMEOUT
  "SOC_WARN_LV1",       // bit6  AXP2101_IRQ_WARNING_LEVEL1
  "SOC_WARN_LV2"        // bit7  AXP2101_IRQ_WARNING_LEVEL2
};
static const char* AXP_IRQ49_NAME[8] = {
  "PEK_POS_EDGE",       // bit0  AXP2101_IRQ_PKEY_POSITIVE_EDGE
  "PEK_NEG_EDGE",       // bit1  AXP2101_IRQ_PKEY_NEGATIVE_EDGE
  "PEK_LONG",           // bit2  AXP2101_IRQ_PKEY_LONG_PRESS
  "PEK_SHORT",          // bit3  AXP2101_IRQ_PKEY_SHORT_PRESS
  "BAT_REMOVE",         // bit4  AXP2101_IRQ_BAT_REMOVE
  "BAT_INSERT",         // bit5  AXP2101_IRQ_BAT_INSERT
  "VBUS_REMOVE",        // bit6  AXP2101_IRQ_VBUS_REMOVE
  "VBUS_INSERT"         // bit7  AXP2101_IRQ_VBUS_INSERT
};
static const char* AXP_IRQ4A_NAME[8] = {
  "BAT_OVP",            // bit0  AXP2101_IRQ_BAT_OVER_VOLTAGE
  "CHG_TIMER",          // bit1  AXP2101_IRQ_CHAGER_TIMER
  "DIE_OVER_TEMP",      // bit2  AXP2101_IRQ_DIE_OVER_TEMP
  "CHG_START",          // bit3  AXP2101_IRQ_BAT_CHG_START
  "CHG_DONE",           // bit4  AXP2101_IRQ_BAT_CHG_DONE
  "BATFET_OCP",         // bit5  AXP2101_IRQ_BATFET_OVER_CURR
  "LDO_OCP",            // bit6  AXP2101_IRQ_LDO_OVER_CURR
  "PMIC_WDT"            // bit7  AXP2101_IRQ_WDT_EXPIRE
};

// critical = 電源断の原因になり得るビット
//   0x48: bit0-3(温度異常) + bit7(SOC警告Lv2)        = 0x8F
//   0x49: bit2(PEK長押し) + bit4(電池抜け) + bit6(VBUS喪失) = 0x54
//   0x4A: bit0,1,2(過電圧/充電タイマ/過熱) + bit5,6,7(OCP/OCP/WDT) = 0xE7
static const uint8_t AXP_IRQ48_CRITICAL = 0x8F;
static const uint8_t AXP_IRQ49_CRITICAL = 0x54;
static const uint8_t AXP_IRQ4A_CRITICAL = 0xE7;

// info = 正常動作でも立つビット（電源断の原因ではない）
//   0x48: bit4(SOC更新) bit5(ゲージWDT) bit6(SOC警告Lv1) = 0x70
//   0x49: bit0,1(PEKエッジ) bit3(PEK短押し) bit5(電池挿入) bit7(VBUS挿入) = 0xAB
//   0x4A: bit3(充電開始) bit4(充電完了)                   = 0x18
static const uint8_t AXP_IRQ48_INFO = 0x70;
static const uint8_t AXP_IRQ49_INFO = 0xAB;
static const uint8_t AXP_IRQ4A_INFO = 0x18;

// ====================================================
// appendAxpIrqNames()
// 指定マスクに含まれる立っているビットの名称を dst に追記する。
// ====================================================
static void appendAxpIrqNames(String& dst, uint8_t value, uint8_t mask,
                              const char* const* names) {
  for (int b = 0; b < 8; b++) {
    if ((value & mask) & (1 << b)) {
      if (dst.length() > 0) dst += " ";
      dst += names[b];
    }
  }
}

// ====================================================
// decodeAxpBootIrq()
// IRQラッチの生値を critical / info / unmapped の3系統に分解する。
//
// ※ ここで生成するのは「解釈」であって一次情報ではない。
//   解析の一次情報はあくまで PWR BOOT 行の生16進値。
// ====================================================
static void decodeAxpBootIrq(uint8_t s0, uint8_t s1, uint8_t s2,
                             String& critical, String& info,
                             uint8_t& un0, uint8_t& un1, uint8_t& un2) {
  critical = "";
  info     = "";

  // 電源断に直結する順（0x4A → 0x49 → 0x48）で並べると読みやすい
  appendAxpIrqNames(critical, s2, AXP_IRQ4A_CRITICAL, AXP_IRQ4A_NAME);
  appendAxpIrqNames(critical, s1, AXP_IRQ49_CRITICAL, AXP_IRQ49_NAME);
  appendAxpIrqNames(critical, s0, AXP_IRQ48_CRITICAL, AXP_IRQ48_NAME);

  appendAxpIrqNames(info, s2, AXP_IRQ4A_INFO, AXP_IRQ4A_NAME);
  appendAxpIrqNames(info, s1, AXP_IRQ49_INFO, AXP_IRQ49_NAME);
  appendAxpIrqNames(info, s0, AXP_IRQ48_INFO, AXP_IRQ48_NAME);

  // critical / info のどちらにも属さない立ちビット（本来は0になるはず）
  un0 = s0 & ~(AXP_IRQ48_CRITICAL | AXP_IRQ48_INFO);
  un1 = s1 & ~(AXP_IRQ49_CRITICAL | AXP_IRQ49_INFO);
  un2 = s2 & ~(AXP_IRQ4A_CRITICAL | AXP_IRQ4A_INFO);

  if (critical.length() == 0) critical = "NONE";
}

// ====================================================
// captureAxp2101BootForensics()
//
// 【実行位置が重要】
//   CoreS3.begin(cfg) の直後、かつ
//   ・最初の CoreS3.update()
//   ・getPekPress() / Power.getKeyState()
//   より前に、必ず1回だけ呼ぶこと。
//
//   getPekPress() は reg 0x49 の PEK ビットを読んだ後に書き戻してクリアする。
//   また M5.config() の pmic_button が有効な場合、ライブラリが BtnPWR の
//   ためにPMICキー状態を参照し得る。よって最初に生レジスタを確保する。
//
// 【使用しないもの（レビュー指摘どおり）】
//   ・getIRQStatuses()
//       戻り値が (0x48<<16)|(0x49<<8)|(0x4A) で、axp2101_irq_t の
//       ビット定義（0x48がbit0-7）とバイト順が逆。enum でマスクすると誤判定する。
//   ・isXxxIrq() 系ヘルパ
//       内部の intRegister[] はRAM上のシャドウで、enableIRQ() を呼ぶまでゼロ。
//       起動直後は常に false を返すため、フォレンジックには使えない。
//
// 【使用API】
//   CoreS3.Power.Axp2101.readRegister8(reg)   … I2C_Device の public メソッド
//   CoreS3.Power.Axp2101.clearIRQStatuses()   … 0x48/0x49/0x4A に 0xFF を書く
//   CoreS3.Power.Axp2101.enableIRQ(mask)      … 0x40/0x41/0x42 へ書く（enum整合）
// ====================================================
void captureAxp2101BootForensics() {

  if (axpBootForensicDone) return;   // 二重実行防止
  axpBootForensicDone = true;

  // ── ① 生レジスタ読み出し（解釈より前に、まず確保する）──────────────
  axpBootIrq48  = CoreS3.Power.Axp2101.readRegister8(AXP2101_REG_IRQSTAT0);
  axpBootIrq49  = CoreS3.Power.Axp2101.readRegister8(AXP2101_REG_IRQSTAT1);
  axpBootIrq4A  = CoreS3.Power.Axp2101.readRegister8(AXP2101_REG_IRQSTAT2);
  axpBootStat00 = CoreS3.Power.Axp2101.readRegister8(AXP2101_REG_STATUS1);
  axpBootStat01 = CoreS3.Power.Axp2101.readRegister8(AXP2101_REG_STATUS2);

  // ── ② 生値の行を作る（これが一次情報。必ず残す）──────────────────
  {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "PWR BOOT IRQ48=0x%02X IRQ49=0x%02X IRQ4A=0x%02X STAT00=0x%02X STAT01=0x%02X",
             axpBootIrq48, axpBootIrq49, axpBootIrq4A, axpBootStat00, axpBootStat01);
    axpBootForensicLine = String(buf);
  }

  // ── ③ 解釈行（生値とは別行にして、桁あふれで生値を潰さない）────────
  {
    // STAT00 bit5 = VBUS good / bit3 = BAT present（isVBUS() / getBatState() と同じビット）
    // STAT01 bit6-5 = 充電状態（0b01=充電 / 0b10=放電 / 0b00=待機）
    int chgBits = (axpBootStat01 >> 5) & 0b11;
    const char* chgText = (chgBits == 1) ? "CHARGE"
                        : (chgBits == 2) ? "DISCHARGE"
                        : "STANDBY";

    String  critical, info;
    uint8_t un0 = 0, un1 = 0, un2 = 0;
    decodeAxpBootIrq(axpBootIrq48, axpBootIrq49, axpBootIrq4A,
                     critical, info, un0, un1, un2);

    // CAUSE行 … 電源断の原因候補（無ければ NONE）
    // critical 10ビットが同時に立つ極端なケースでは 150文字近くなるため
    // バッファは余裕をもって確保する。仮にSD側で切れても、生値は
    // 直前の PWR BOOT 行に残っているため一次情報は失われない。
    {
      char buf[192];
      snprintf(buf, sizeof(buf),
               "PWR BOOT CAUSE %s vbus=%d bat=%d chg=%s",
               critical.c_str(),
               (axpBootStat00 & 0x20) ? 1 : 0,   // isVBUS() と同じ判定
               (axpBootStat00 & 0x08) ? 1 : 0,   // getBatState() と同じ判定
               chgText);
      axpBootCauseLine = String(buf);
    }

    // INFO行 … 正常動作でも立つビット。
    // これがあると「CAUSE NONE なのに IRQ が非ゼロ」の理由が説明できる。
    // 行を分けているのは、ビットが多数立っても CAUSE 行を押し出さないため。
    if (info.length() > 0) {
      axpBootInfoLine = "PWR BOOT INFO " + info;
    }

    // UNMAPPED行 … critical/info のどちらにも分類されない立ちビット。
    // 全24ビットを網羅済みのため通常は出ないが、
    // 想定外のビットを「IRQなし」と誤解しないための保険として残す。
    if (un0 || un1 || un2) {
      char buf[96];
      snprintf(buf, sizeof(buf),
               "PWR BOOT UNMAPPED IRQ48=0x%02X IRQ49=0x%02X IRQ4A=0x%02X",
               un0, un1, un2);
      axpBootUnmappedLine = String(buf);
    }
  }

  // ── ④ 即時出力 ────────────────────────────────────────────────
  // この時点ではまだ SD.begin() 前なので、SDへは setup() 側で直書きする。
  // addLog() は Serial + webLog に出力する（SDバッファは経由するが、
  // cfg_enableSDLog は loadConfig() 前で false のため実質Serialのみ）。
  addLog(axpBootForensicLine);
  addLog(axpBootCauseLine);
  if (axpBootInfoLine.length()     > 0) addLog(axpBootInfoLine);
  if (axpBootUnmappedLine.length() > 0) addLog(axpBootUnmappedLine);

  // ── ⑤ IRQステータスをクリア ────────────────────────────────────
  // クリアしないとビットが起動をまたいで累積し、
  // 「今回の停止原因」と「過去の履歴」が区別できなくなる。
  // clearIRQStatuses() は 0x48/0x49/0x4A に 0xFF を書き込む（実装確認済み）。
  CoreS3.Power.Axp2101.clearIRQStatuses();

  // ── ⑥ 次回の原因判定に必要なIRQを有効化 ────────────────────────
  // enum は m5 名前空間。setIRQEnRegister() は
  //   mask & 0xFF → 0x40 / mask>>8 → 0x41 / mask>>16 → 0x42
  // と分解しており、axp2101_irq_t のビット定義と整合している（実装確認済み）。
  //
  // 監視対象（依頼どおり7種）:
  //   BATFET過電流   : AXP2101_IRQ_BATFET_OVER_CURR (1<<21) → 0x42 bit5
  //   Battery Remove : AXP2101_IRQ_BAT_REMOVE       (1<<12) → 0x41 bit4
  //   VBUS Remove    : AXP2101_IRQ_VBUS_REMOVE      (1<<14) → 0x41 bit6
  //   PEK Long Press : AXP2101_IRQ_PKEY_LONG_PRESS  (1<<10) → 0x41 bit2
  //   PMIC過熱       : AXP2101_IRQ_DIE_OVER_TEMP    (1<<18) → 0x42 bit2
  //   LDO過電流      : AXP2101_IRQ_LDO_OVER_CURR    (1<<22) → 0x42 bit6
  //   SOC Warning L2 : AXP2101_IRQ_WARNING_LEVEL2   (1<<7)  → 0x40 bit7
  {
    uint64_t mask = (uint64_t)m5::AXP2101_IRQ_BATFET_OVER_CURR
                  | (uint64_t)m5::AXP2101_IRQ_BAT_REMOVE
                  | (uint64_t)m5::AXP2101_IRQ_VBUS_REMOVE
                  | (uint64_t)m5::AXP2101_IRQ_PKEY_LONG_PRESS
                  | (uint64_t)m5::AXP2101_IRQ_DIE_OVER_TEMP
                  | (uint64_t)m5::AXP2101_IRQ_LDO_OVER_CURR
                  | (uint64_t)m5::AXP2101_IRQ_WARNING_LEVEL2;
    // mask = 0x645480 → 0x40=0x80 / 0x41=0x54 / 0x42=0x64

    bool libRet = CoreS3.Power.Axp2101.enableIRQ(mask);

    // ------------------------------------------------------------------
    // 【重要】enableIRQ() の戻り値は信用してはいけない
    //
    // M5Unified の実装（AXP2101_Class.cpp）:
    //
    //   bool AXP2101_Class::setIRQEnRegister(uint64_t registerEn, bool enable)
    //   {
    //     int res = 0;
    //     ...
    //     res |= writeRegister8(AXP2101_IRQEN0, intRegister[0]);
    //     ...
    //     return res == 0;
    //   }
    //
    // I2C_Device::writeRegister8() は「成功=true(1) / 失敗=false(0)」を返す。
    // よって書き込みが成功するほど res は 1 になり、`res == 0` は false。
    // つまり **書き込み成功時に false（NG）が返る** という戻り値の反転バグ。
    //
    // 逆に全書き込みが失敗した場合のみ true（OK）が返るため、
    // 戻り値で成否を判定すると常に真逆の結論になる。
    //
    // → 戻り値は参考値（lib=）としてのみ記録し、
    //   実際の成否は IRQ ENABLE レジスタ 0x40/0x41/0x42 の
    //   【生読み】と期待値の突き合わせで判定する。
    // ------------------------------------------------------------------
    uint8_t want0 = (uint8_t)( mask        & 0xFF);   // 0x80
    uint8_t want1 = (uint8_t)((mask >>  8) & 0xFF);   // 0x54
    uint8_t want2 = (uint8_t)((mask >> 16) & 0xFF);   // 0x64

    uint8_t got0 = CoreS3.Power.Axp2101.readRegister8(AXP2101_REG_IRQEN0);
    uint8_t got1 = CoreS3.Power.Axp2101.readRegister8(AXP2101_REG_IRQEN1);
    uint8_t got2 = CoreS3.Power.Axp2101.readRegister8(AXP2101_REG_IRQEN2);

    // setIRQEnRegister() は既存値とのOR（read-modify-write）で書き込むため、
    // 他のビットが立っていても構わない。「要求ビットがすべて立っているか」で判定する。
    bool verified = ((got0 & want0) == want0)
                 && ((got1 & want1) == want1)
                 && ((got2 & want2) == want2);

    char buf[144];
    snprintf(buf, sizeof(buf),
             "PWR BOOT IRQEN want=%02X/%02X/%02X got=%02X/%02X/%02X result=%s lib=%d",
             want0, want1, want2,
             got0, got1, got2,
             verified ? "OK" : "NG",
             libRet ? 1 : 0);
    axpBootIrqEnLine = String(buf);
    addLog(axpBootIrqEnLine);
  }
}

// ====================================================
// powerLoadContext()
// 電源イベント発生時に「何が動いていたか」を4文字で表す。
//
//   1文字目 S : サーボがattach中（PWM出力中＝電流を引いている）
//   2文字目 W : WAV再生中（soundBusy）
//   3文字目 C : カメラ機能が有効（定期的にフレーム取得している）
//   4文字目 - : 予約。Wi-Fi送信中の状態は確実に取得する手段がないため
//               推測で埋めず、常に '-' とする。
//
// CoreS3では電流を測れないため、「電圧が落ちた瞬間に何が動いていたか」の
// 相関だけが突入電流を推定する手段になる。
// ====================================================
String powerLoadContext() {
  char c[5];
  bool servoOn = (ENABLE_UD_SERVO && servoUD.attached())
              || (ENABLE_LR_SERVO && servoLR.attached());
  c[0] = servoOn         ? 'S' : '-';
  c[1] = soundBusy       ? 'W' : '-';
  c[2] = cfg_enableCamera ? 'C' : '-';
  c[3] = '-';   // 予約（Wi-Fi送信状態は取得不可のため推測しない）
  c[4] = '\0';
  return String(c);
}

// ====================================================
// handlePowerVitals()
//
// 1秒周期で電源状態を読み、
//   ・状態が変化したときだけ即時イベントログ
//   ・60秒ごとに1行だけスナップショット（PWR SNAP）
// を出力する。Power Debugモードは設けず常時有効。
//
// 使用API（すべてCoreS3で実際に値が取れることを確認済み）:
//   Axp2101.isVBUS()          … reg 0x00 bit5
//   Axp2101.getBatState()     … reg 0x00 bit3（電池実装検出）
//   Axp2101.getChargeStatus() … reg 0x01 bit6-5（-1:放電 / 0:待機 / 1:充電）
//   Power.getBatteryVoltage() … reg 0x34（mV）
//   Power.getBatteryLevel()   … reg 0xA4（%）
//   Power.getVBUSVoltage()    … reg 0x38（mV。VBUS無しなら0）
//
// 電流系・温度は使用しない（取得不可 / ライブラリ不具合のため）。
// ====================================================
void handlePowerVitals() {

  const unsigned long PWR_POLL_INTERVAL     = 1000;    // 監視周期 1秒
  const unsigned long PWR_SNAP_INTERVAL     = 60000;   // スナップショット 60秒
  const int           PWR_VSAG_THRESHOLD_MV = 100;     // 電圧サグ判定 100mV
  const unsigned long PWR_VSAG_COOLDOWN     = 10000;   // サグログのクールダウン 10秒

  static unsigned long lastPoll     = 0;
  static unsigned long lastSnap     = 0;
  static unsigned long lastVsagLog  = 0;

  static bool  stateInit   = false;
  static bool  prevVbus    = false;
  static bool  prevBatState= false;
  static int   prevChg     = 0;
  static int   prevVbat    = 0;     // 直近サンプルのバッテリー電圧[mV]

  static int   vmin        = 0;     // 直近ウィンドウ内の最小電圧
  static int   vmax        = 0;     // 直近ウィンドウ内の最大電圧

  unsigned long now = millis();
  if (now - lastPoll < PWR_POLL_INTERVAL) return;
  lastPoll = now;

  // ── 1秒周期で読む項目（I2C 4トランザクション程度）──────────────
  bool vbus     = CoreS3.Power.Axp2101.isVBUS();
  bool batState = CoreS3.Power.Axp2101.getBatState();
  int  chg      = CoreS3.Power.Axp2101.getChargeStatus();   // -1 / 0 / 1
  int  vbat     = (int)CoreS3.Power.getBatteryVoltage();    // mV

  // 初回はイベントを出さず、基準値の初期化だけ行う
  if (!stateInit) {
    stateInit    = true;
    prevVbus     = vbus;
    prevBatState = batState;
    prevChg      = chg;
    prevVbat     = vbat;
    vmin         = vbat;
    vmax         = vbat;
    lastSnap     = now;
    return;
  }

  // ── 最小／最大電圧の更新 ──────────────────────────────────────
  if (vbat < vmin) vmin = vbat;
  if (vbat > vmax) vmax = vbat;

  String load = powerLoadContext();

  // ── 状態変化イベント（変化したときだけ出す）────────────────────
  if (vbus != prevVbus) {
    addLog(String(vbus ? "PWR VBUS OK" : "PWR VBUS LOST")
         + " vbat=" + String(vbat) + " load=" + load);
    prevVbus = vbus;
  }

  if (batState != prevBatState) {
    addLog(String(batState ? "PWR BAT PRESENT" : "PWR BAT REMOVED")
         + " vbus=" + String(vbus ? 1 : 0));
    prevBatState = batState;
  }

  if (chg != prevChg) {
    addLog("PWR CHG state=" + String(chg)
         + " soc=" + String((int)CoreS3.Power.getBatteryLevel())
         + " vbat=" + String(vbat));
    prevChg = chg;
  }

  // ── 電圧サグ（直前サンプルから閾値以上の低下）──────────────────
  // CoreS3では電流を測れないため、サグ＋負荷コンテキストで突入電流を推定する。
  // 連続発生でログが埋まらないようクールダウンを設ける。
  int drop = prevVbat - vbat;
  if (drop >= PWR_VSAG_THRESHOLD_MV && (now - lastVsagLog) > PWR_VSAG_COOLDOWN) {
    lastVsagLog = now;
    addLog("PWR VSAG vbat=" + String(vbat)
         + " prev=" + String(prevVbat)
         + " drop=" + String(drop)
         + " load=" + load);
  }
  prevVbat = vbat;

  // ── 60秒スナップショット（1行だけ）────────────────────────────
  if (now - lastSnap >= PWR_SNAP_INTERVAL) {
    lastSnap = now;

    // ここでだけ読む項目（毎秒読む必要がないもの）
    int soc    = (int)CoreS3.Power.getBatteryLevel();
    int vbusMv = (int)CoreS3.Power.getVBUSVoltage();   // VBUS無しなら0

    char sbuf[152];
    snprintf(sbuf, sizeof(sbuf),
             "PWR SNAP vbus=%d bat=%d chg=%d soc=%d vbat=%d vbus_mv=%d "
             "vmin=%d vmax=%d loops=%lu load=%s",
             vbus ? 1 : 0,
             batState ? 1 : 0,
             chg,
             soc,
             vbat,
             vbusMv,
             vmin,
             vmax,
             (unsigned long)loopCounter,
             load.c_str());
    addLog(sbuf);

    // ウィンドウをリセット（次の60秒の最小／最大を取る）
    vmin = vbat;
    vmax = vbat;
  }
}

// ====================================================
// handleDiagnosticHeartbeat()
// 30秒ごとに LOOP HEARTBEAT と STATE HEARTBEAT を出力し、
// 鼻アニメーションが60秒以上更新されていない場合のみ NOSE STALL を出す。
//
// 目的：次回「鼻が止まった」現象が出たとき、ログだけで
//   ・loop()自体が止まっていたのか（HEARTBEATが途切れる）
//   ・loopは回っていたが描画が止まっていたのか（HEARTBEATは出るがNOSE STALLも出る）
// を切り分けられるようにする。
// ====================================================
void handleDiagnosticHeartbeat() {
  unsigned long now = millis();

  static unsigned long lastHeartbeatTime = 0;
  const unsigned long HEARTBEAT_INTERVAL = 30000UL;  // 30秒

  if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    lastHeartbeatTime = now;

    addLog("LOOP HEARTBEAT t=" + String(now));

    // currentFaceMode 相当：現在の顔表示状態を短い文字列で
    const char* faceMode =
        imageFaceMode   ? (mutterFaceActive ? "MUTTER_PNG" : "IMAGE_PNG")
      : sleepMode       ? "SLEEP"
      : alertMode       ? "ALERT"
      : petMode         ? "PET"
      : externalSpeaking? "SPEAK"
      : "NORMAL";

    char sbuf[200];
    snprintf(sbuf, sizeof(sbuf),
             "STATE HEARTBEAT sleep=%d pet=%d alert=%d speak=%d wav=%d face=%s src=%s lvl=%d freeHeap=%u psram=%u",
             sleepMode ? 1 : 0,
             petMode ? 1 : 0,
             alertMode ? 1 : 0,
             externalSpeaking ? 1 : 0,
             soundBusy ? 1 : 0,       // isPlayingWav 相当
             faceMode,
             audioSourceName(audioSource),
             audioLevel,
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getFreePsram());
    addLog(sbuf);
  }

  // --- NOSE STALL 検出（60秒以上、鼻の実描画がない場合のみ1回出す） ---
  static unsigned long lastNoseStallLogged = 0;
  const unsigned long NOSE_STALL_THRESHOLD = 60000UL;  // 60秒

  // imageFaceMode/sleepMode 等で鼻を意図的に止めている場合は誤検出しない。
  // （updateNose自身がimageFaceMode/yawnModeでreturnするため、それらは正常な停止）
  bool noseIntentionallyPaused = imageFaceMode || alertMode || petMode || yawnMode;

  if (!noseIntentionallyPaused &&
      lastNoseDrawTime != 0 &&
      (now - lastNoseDrawTime) > NOSE_STALL_THRESHOLD) {
    // 同じ停止に対して繰り返しログを出さない（30秒に1回まで）
    if (now - lastNoseStallLogged > 30000UL) {
      lastNoseStallLogged = now;
      addLog("NOSE STALL: no nose redraw for " +
             String((now - lastNoseDrawTime) / 1000) + "s" +
             " (face=" + String(imageFaceMode ? "IMG" : "NORMAL") +
             " sleep=" + String(sleepMode ? 1 : 0) +
             " speak=" + String(externalSpeaking ? 1 : 0) +
             " wav=" + String(soundBusy ? 1 : 0) + ")");
    }
  }
}

// ############################################################################
// #  Unified Scene Canvas  v1.0  統合描画パイプライン（2026-07-26）           #
// ############################################################################
//
// ■ 目的
//   通常顔・Lighting・Visualizer を「同じオフスクリーンCanvas上で1フレームとして
//   合成 → 完成後に液晶へ一括転送」する共通パイプラインへ統一する。
//   これにより顔パーツ更新時の白消去矩形（fillRect(..., WHITE)）が原理的に不要と
//   なり、Disco / Aurora / Laser などの色付き背景の上へ白い四角が露出しなくなる。
//
// ■ 共通パイプライン（描画順は常にこの順序）
//     1. Canvasへ背景を描画          … sceneCanvas.fillSprite(WHITE)
//     2. Lightingを描画              … cfg_lightingMask != 0 のときのみ
//     3. Visualizerを描画
//     4. 目・まつ毛・鼻・口を描画
//     5. 完成したCanvasを液晶へ転送  … scenePush()
//   Lighting全OFF（cfg_lightingMask == 0）でも通常表示を例外扱いせず、白背景を
//   「1つの背景」として同じパイプラインを通す。Canvas描画用のLighting ON/OFF
//   フラグは新設しない（判定は既存の cfg_lightingMask のみ）。
//
// ■ Canvasの仕様
//   ・320 x 240 / 16bit(RGB565) / PSRAM = 153,600 bytes
//   ・液晶と同じ寸法で確保するため、Canvasの座標系は液晶の絶対座標と完全に一致する。
//     → 既存描画関数へ -48 のような座標補正を入れる必要が一切ない（座標変換ゼロ）。
//        width()/height() も液晶と同値になるため、既存コードの前提が崩れない。
//   ・液晶へ転送するのは y = SCENE_TOP(48) 〜 239 の行だけ（scenePush()がクランプ）。
//     上端48pxの情報パネル(showSensors)がCanvas転送で上書きされることはない。
//   ・確保はsetup()で1回だけ。毎フレームの createSprite/deleteSprite は行わない。
//   ・確保失敗時は gSceneReady=false となり、全描画が従来どおり CoreS3.Display へ
//     直接行われる（＝安全なフォールバック。白消去矩形もそのときだけ復活する）。
//
// ■ 描画先の一元管理
//   gGfx は既定で液晶を指し、合成中だけ sceneCanvas を指す。描画関数は GFX.xxx と
//   書くだけで液晶とCanvasの両方へ同じ描画命令を出せるため、二重実装は不要。
// ############################################################################
#define SCENE_TOP   48     // Canvasを液晶へ転送する最上段Y（上端48pxの情報パネルを守る）
#define SCENE_W    320
#define SCENE_H    240

M5Canvas sceneCanvas(&CoreS3.Display);

bool gSceneReady        = false;  // Canvas確保に成功したか（falseなら全て従来の直接描画）
bool gSceneOnCanvas     = false;  // 現在Canvasへ合成中か（各描画関数の白消去抑止に使う）
bool gSceneNeedFullPush = false;  // 次回の転送を全面にする（特殊画面から戻った直後など）

// 描画先ポインタ。M5GFX(液晶)とM5Canvas(Sprite)の共通基底へのポインタなので、
// 同じ描画命令をそのままどちらへも出せる。
m5gfx::LovyanGFX* gGfx = &CoreS3.Display;
#define GFX (*gGfx)

// ── 顔の状態（統合パイプライン用）──────────────────────────────
// 目・鼻・口の各描画関数は「状態を更新して再合成を依頼する」役割へ変更した。
// 実際の描画は sceneDrawNormalFace() がCanvas上で1フレームとしてまとめて行うため、
// 前回の顔パーツを消すための白い矩形は通常表示パイプラインから完全に不要になる。
enum FaceEyeMode : uint8_t {
  FACE_EYE_OPEN = 0,   // 通常の開眼（ミスかりポムはまつ毛付き）
  FACE_EYE_BLINK,      // まばたき
  FACE_EYE_PET         // なでなで反応時のペット目
};
FaceEyeMode gFaceEyeMode    = FACE_EYE_OPEN;
int         gFaceNoseOffset = 0;      // updateNose()が受け取る鼻の上下オフセット
bool        gFaceMouthTalk  = false;  // true=おしゃべり口(赤い楕円) / false=通常の逆Y口
int         gFaceMouthMx = noseX;          // drawMouthOpen()が抽選した口のゆらぎを保持
int         gFaceMouthMy = noseY + 26;
int         gFaceMouthMw = 18;
int         gFaceMouthMh = 12;

// あくび中フラグ。trueの間はupdateNose()が逆Y口を描かない。
// （あくび画面は統合Canvasの対象外＝従来どおり液晶へ直接描くため、ここで宣言する）
bool yawnMode = false;

// Visualizer表示中フラグ。統合パイプラインの判定で参照するためファイル前半で宣言する
// （実体の更新は updateVisualizerFace() ＝ #ifdef FFT_DISPLAY_TEST 側）。
bool visualizerFaceActive = false;

// 顔パーツ更新時にCanvasを転送する範囲（従来の白消去矩形と同等の範囲）。
// 変化しうる画素を確実に含みつつ、転送量を従来の部分描画と同程度に保つ。
#define FACE_EYES_X    16
#define FACE_EYES_Y    38
#define FACE_EYES_W   288
#define FACE_EYES_H    96
#define FACE_MOUTH_X  (noseX - 45)
#define FACE_MOUTH_Y  (noseY - 25)
#define FACE_MOUTH_W   90
#define FACE_MOUTH_H  105

// ── 前方宣言 ────────────────────────────────────────────────
void drawEyelashes();
void sceneDrawNormalFace();
void scenePush(int x, int y, int w, int h);
#ifdef FFT_DISPLAY_TEST
void drawVisualizerFaceParts(bool forceClear);
// Eye Slotのリールを「状態を進めずに」再描画する（実体はLighting Framework側）。
// sceneComposeAndPush()がVisualizer描画直後の再描画に使う（後述）。
void eslotDrawReelsFrame();
// 統合描画パイプライン本体（実体は Lighting Framework 側＝ファイル末尾付近）。
void sceneComposeAndPush(bool lightInit, bool lightFull,
                         bool vizOn, uint8_t vizMode, bool vizInit,
                         int px, int py, int pw, int ph);
#endif

// Lighting / Visualizer が定周期でCanvasを合成している最中か。
// この間、顔パーツ側からの再合成要求は無視する（演出の状態を余分に進めないため）。
static inline bool sceneEffectsActive() {
  return screenFxLighting || visualizerFaceActive;
}

// 顔パーツを統合Canvas経由で描くべき状態か。
// 睡眠画面・画像顔・あくび画面はCanvas統合の対象外なので従来どおり直接描画する。
static inline bool sceneFaceOnCanvas() {
  return gSceneReady && !sleepMode && !imageFaceMode && !yawnMode;
}

// 次回の転送を全面にする（睡眠画面・画像顔・あくび等から通常画面へ戻る際に呼ぶ）。
static inline void sceneInvalidate() { gSceneNeedFullPush = true; }

// ── Canvasの確保（setup()から1回だけ）──────────────────────────
void sceneCanvasInit() {
  sceneCanvas.setPsram(true);       // 内部RAMを圧迫しないよう必ずPSRAMへ確保する
  sceneCanvas.setColorDepth(16);    // 液晶と同じRGB565
  if (sceneCanvas.createSprite(SCENE_W, SCENE_H)) {
    gSceneReady = true;
    sceneCanvas.setTextWrap(false);
    sceneCanvas.fillSprite(WHITE);
    gSceneNeedFullPush = true;
    char b[160];
    snprintf(b, sizeof(b),
             "SCENE CANVAS OK: %dx%d 16bit %u bytes psramFree=%u",
             SCENE_W, SCENE_H, (unsigned)(SCENE_W * SCENE_H * 2),
             (unsigned)ESP.getFreePsram());
    addLog(b);
  } else {
    gSceneReady = false;
    addLog("SCENE CANVAS ALLOC FAIL -> fallback: draw directly to LCD");
  }
}

// ── Canvasの一部を液晶へ転送 ────────────────────────────────
// yは必ず SCENE_TOP 以上へクランプするため、上端48pxの情報パネルは決して壊れない。
// Canvasは液晶と同じ座標系なので、転送位置の計算に座標変換は発生しない。
void scenePush(int x, int y, int w, int h) {
  if (!gSceneReady) return;
  if (gSceneNeedFullPush) {                 // 特殊画面から戻った直後は全面を転送する
    gSceneNeedFullPush = false;
    x = 0; y = SCENE_TOP; w = SCENE_W; h = SCENE_H - SCENE_TOP;
  }
  if (x < 0) { w += x; x = 0; }
  if (y < SCENE_TOP) { h -= (SCENE_TOP - y); y = SCENE_TOP; }
  if (x + w > SCENE_W) w = SCENE_W - x;
  if (y + h > SCENE_H) h = SCENE_H - y;
  if (w <= 0 || h <= 0) return;
  // 転送範囲をクリップで限定する。pushSpriteは液晶側のクリップ矩形を尊重するため、
  // 交差した領域だけが実際にSPIへ流れる（＝従来の部分描画と同等の転送量）。
  CoreS3.Display.setClipRect(x, y, w, h);
  sceneCanvas.pushSprite(&CoreS3.Display, 0, 0);
  CoreS3.Display.clearClipRect();
}

// ── 合成の開始／終了（描画先をCanvasへ切替える）────────────────
// 戻り値 false は「Canvas未確保 → 従来どおり液晶へ直接描画」を意味する。
bool sceneBeginCompose() {
  if (!gSceneReady) return false;
  gGfx = &sceneCanvas;
  gSceneOnCanvas = true;
  sceneCanvas.fillSprite(WHITE);   // 1. Canvasへ背景を描画（Lighting全OFF時はこれが背景）
  return true;
}
void sceneEndCompose(bool onCanvas) {
  if (!onCanvas) return;
  gSceneOnCanvas = false;
  gGfx = &CoreS3.Display;
}

// ── 顔パーツ更新からの再合成 ────────────────────────────────
// Lighting/Visualizer表示中は updateScreenEffects() 側が定周期で合成するため、
// ここでは何もしない（演出の状態を余分に進めない／更新頻度を上げない）。
void sceneRenderFace(int x, int y, int w, int h) {
  if (!gSceneReady) return;
  if (sceneEffectsActive()) return;
#ifdef FFT_DISPLAY_TEST
  // Lighting時・Visualizer時とまったく同じ統合パイプラインを通す。
  // Lighting/Visualizerはこの状態では非表示のため、実質「白背景＋顔」になる。
  sceneComposeAndPush(false, false, false, 0, false, x, y, w, h);
#else
  bool onCanvas = sceneBeginCompose();   // 1. 背景（白）
  sceneDrawNormalFace();                 // 4. 目・まつ毛・鼻・口
  sceneEndCompose(onCanvas);
  scenePush(x, y, w, h);                 // 5. 液晶へ転送
#endif
}

// 太い線
// 2026-08-05: color引数をuint32_t→int32_tへ変更（根本原因の1行修正）。
// M5GFXのWHITE/BLACK等の色定数はconstexpr int（=int32_t）で、RGB565値をそのまま
// 保持している。GFX.fillCircle(...)はテンプレートで色の型ごとに解釈を切り替えており、
// int32_t型はRGB565としてそのまま解釈されるが、uint32_t型はRGB888として解釈され
// 変換されてしまう。旧シグネチャ（uint32_t color）にWHITE(0xFFFF)を渡すと、
// 0x0000FFFFがRGB888として解釈されR=0,G=255,B=255＝水色になっていた
// （BLACK=0x0000はどちらの解釈でも黒のため、これまで顕在化していなかった）。
// int32_tへ変更することで、本関数を通るWHITE/BLACK等すべての色が
// GFX.fillCircle/fillEllipse等への直接指定と同じRGB565解釈になり、正しい色で描画される。
void drawThickLine(int x0, int y0, int x1, int y1, int thickness, int32_t color) {
  int dx = x1 - x0;
  int dy = y1 - y0;
  int steps = max(abs(dx), abs(dy));

  for (int i = 0; i <= steps; i++) {
    float t = (float)i / steps;
    int x = x0 + dx * t;
    int y = y0 + dy * t;
    GFX.fillCircle(x, y, thickness / 2, color);
  }
}

// ====================================================
// drawEyelashes()
// Miss KariPom専用：drawOpenEyes()の黒目（中心90/230, 90 ±eyeOffset, 半径20）の
// 外側上部から、円周に根元が接した状態で約45°方向へ短いまつ毛を3本ずつ描く。
// 通常のかりポムとの違いはこのまつ毛のみ（他の装飾は追加しない）。
// ====================================================
void drawEyelashes() {
  const int   LASH_COUNT = 3;
  const int   LASH_LEN   = 9;   // まつ毛の長さ(px)
  const int   LASH_THICK = 4;   // まつ毛の太さ(px)
  const int   EYE_RADIUS = 20;  // drawOpenEyes()の黒目半径と一致させる
  // 目の外側上部（水平から見た角度）を横方向寄り〜上寄りへ3本分散。中心は約45°。
  const float LASH_ANGLES_DEG[LASH_COUNT] = {25.0f, 45.0f, 65.0f};

  int leftCx  = 90  + eyeOffsetX;
  int rightCx = 230 + eyeOffsetX;
  int cy      = 90  + eyeOffsetY;

  for (int i = 0; i < LASH_COUNT; i++) {
    float rad = LASH_ANGLES_DEG[i] * PI / 180.0f;
    float dx  = cos(rad);
    float dy  = sin(rad);

    // 左目：外側は左方向（-x）
    int lRootX = leftCx - (int)(EYE_RADIUS * dx);
    int lRootY = cy     - (int)(EYE_RADIUS * dy);
    int lTipX  = leftCx - (int)((EYE_RADIUS + LASH_LEN) * dx);
    int lTipY  = cy     - (int)((EYE_RADIUS + LASH_LEN) * dy);
    drawThickLine(lRootX, lRootY, lTipX, lTipY, LASH_THICK, BLACK);

    // 右目：外側は右方向（+x）
    int rRootX = rightCx + (int)(EYE_RADIUS * dx);
    int rRootY = cy      - (int)(EYE_RADIUS * dy);
    int rTipX  = rightCx + (int)((EYE_RADIUS + LASH_LEN) * dx);
    int rTipY  = cy      - (int)((EYE_RADIUS + LASH_LEN) * dy);
    drawThickLine(rRootX, rRootY, rTipX, rTipY, LASH_THICK, BLACK);
  }
}

// ====================================================
// 顔パーツの「形」だけを描くヘルパー群（描画先は GFX ＝ Canvas または液晶）
//
// 位置・サイズ・色・まつ毛の仕様は従来の drawOpenEyes()/drawBlinkEyes()/
// drawPetEyes()/updateNose()/drawMouthOpen()/drawMouthClosed() と完全に同一。
// 違いは「白消去を含まない」点だけ。白消去が不要なのは、統合パイプラインが
// 毎フレーム背景（白 or Lighting）からCanvasを作り直すため。
// ====================================================
static void faceShapeOpenEyes() {
  // 黒目を数pxだけ動かす。サーボは動かさない安全演出。
  GFX.fillCircle(90 + eyeOffsetX, 90 + eyeOffsetY, 20, BLACK);
  GFX.fillCircle(230 + eyeOffsetX, 90 + eyeOffsetY, 20, BLACK);

  if (cfg_characterStyle == CHARACTER_MISS_KARIPOM) {
    drawEyelashes();
  }
}

static void faceShapeBlinkEyes() {
  drawThickLine(
    72 + eyeOffsetX,
    90 + eyeOffsetY,
    108 + eyeOffsetX,
    90 + eyeOffsetY,
    6,
    BLACK);

  drawThickLine(
    212 + eyeOffsetX,
    90 + eyeOffsetY,
    248 + eyeOffsetX,
    90 + eyeOffsetY,
    6,
    BLACK);
}

static void faceShapePetEyes() {
  drawThickLine(72, 94, 108, 86, 6, BLACK);
  drawThickLine(212, 86, 248, 94, 6, BLACK);
}

// ====================================================
// 目の各関数は「目の状態を更新して再合成を依頼する」役割になった。
// 通常表示パイプラインでは白消去矩形を一切描かない。
// 下段の CoreS3.Display.fillRect(..., WHITE) は、Canvas未確保（PSRAM確保失敗）や
// 睡眠画面・画像顔・あくび画面といったCanvas統合対象外の状態でのみ実行される
// 従来どおりのフォールバック経路。
// ====================================================
void drawOpenEyes() {
  if (imageFaceMode) return;
  gFaceEyeMode = FACE_EYE_OPEN;

  if (sceneFaceOnCanvas()) { sceneRenderFace(FACE_EYES_X, FACE_EYES_Y, FACE_EYES_W, FACE_EYES_H); return; }

  // 消去範囲を y=45〜125 に制限（鼻上端に被らないよう）
  // 旧: fillRect(50, 50, 80, 75) → Miss KariPomのまつ毛がgaze移動時(最大±18px)に
  // はみ出す可能性があったため、上下左右に少し余裕を持たせた（他要素との重なりなし確認済み）。
  CoreS3.Display.fillRect(40, 45, 90, 80, WHITE);
  CoreS3.Display.fillRect(190, 45, 90, 80, WHITE);
  faceShapeOpenEyes();
}

void drawBlinkEyes() {
  if (imageFaceMode) return;
  gFaceEyeMode = FACE_EYE_BLINK;

  if (sceneFaceOnCanvas()) { sceneRenderFace(FACE_EYES_X, FACE_EYES_Y, FACE_EYES_W, FACE_EYES_H); return; }

  // 消去範囲を y=50〜125 に制限（鼻上端 noseY-12=133 より十分上で止める）
  // 旧: fillRect(20, 50, 280, 90) → y=50〜140 で鼻上端(133)に被っていた
  CoreS3.Display.fillRect(20, 50, 280, 75, WHITE);
  faceShapeBlinkEyes();
}

void drawPetEyes() {
  if (imageFaceMode) return;
  gFaceEyeMode = FACE_EYE_PET;

  if (sceneFaceOnCanvas()) { sceneRenderFace(FACE_EYES_X, FACE_EYES_Y, FACE_EYES_W, FACE_EYES_H); return; }

  // 消去範囲を y=50〜125 に制限（鼻上端に被らないよう）
  // 旧: fillRect(45, 50, 240, 85) → y=50〜135 で鼻上端(133)に被っていた
  CoreS3.Display.fillRect(45, 50, 240, 75, WHITE);
  faceShapePetEyes();
}

// 閉じた目の線だけを描く（消去なし・GFX経由）。drawSleepEyes()（単色白背景・消去あり）と
// Sleep Lighting Carousel（背景Lighting毎フレーム描き直し・消去不要）の両方から呼べるよう
// 線の座標・太さをここへ一本化した（見た目・座標は既存drawSleepEyes()と完全に同じ）。
// thicknessは省略時7（従来どおり）。Sleep Carouselの白縁取り版から太さ違いで再利用するために
// 引数化したが、既存呼び出し側（drawSleepEyes()）は省略時の挙動が完全に同じなので無影響。
void drawClosedEyeLines(uint32_t color, int thickness = 7) {
  drawThickLine(72, 94, 108, 94, thickness, color);
  drawThickLine(212, 94, 248, 94, thickness, color);
}

// 閉じた目を黒＋白縁取りで描く（Sleep Lighting Carousel「目×Lighting」専用）。
// 白（太め）を先に描き、同じ座標に黒（既存の太さ7）を重ねることで縁取りにする。
void drawClosedEyeLinesOutlined() {
  drawClosedEyeLines(WHITE, 7 + SLEEP_OUTLINE_PX);
  drawClosedEyeLines(BLACK, 7);
}

void drawSleepEyes() {
  addLog("DRAW SLEEP EYES");

  // 消去範囲を y=55〜125 に制限（鼻上端に被らないよう）
  // 旧: fillRect(55, 55, 210, 70) → y=55〜125 でギリギリ。明示的に75に制限。
  CoreS3.Display.fillRect(55, 55, 210, 70, WHITE);
  drawClosedEyeLines(BLACK);

  drawIpStatusOnly();
}

// ====================================================
// ====================================================
// calibrateJoystick()  ── no-op スタブ（2026/07/07 廃止）
//
// 旧実装: 起動時に64サンプルを採取して joyCenterX/Y を決定し、
//         条件次第で joystickEnabled=true にしていた。
// 問題:   起動中にジョイスティックを接続すると、ADCが安定する前の
//         過渡値を中心として採用してしまい、以降の首振りやwakeUpに
//         誤入力が混入する。
// 廃止後: joystickEnabled は宣言時 false で固定。
//         有効化は handleJoystick() 内のランタイム安定確認
//         → recalibrateJoystickRuntime() の一本化フローで行う。
//         起動時・動作中・遅延接続のすべてで同一ロジックが適用される。
// setup() 内の呼び出しも削除済み（→ 起動ログのみ出力）。
// ====================================================
void calibrateJoystick() {
  // no-op: 何もしない
  // joystickEnabled は false のまま。runtime recalib を待つ。
  addLog("JOY CALIB: skipped (deprecated, runtime recalib active)");
}

// ====================================================
// applyJoyCalibThresholds()
//
// joyCenterX/Y から stroke・閾値・LPF初期値を再計算して全変数へ書き込む。
// calibrateJoystick() の初回起動時と、recalibrateJoystickRuntime() の
// 両方から呼び出すことで、閾値計算ロジックを1箇所に集約する。
// ====================================================
void applyJoyCalibThresholds() {
  // ── 実ストロークを計算（LOW側とHIGH側の短い方を採用） ──
  int strokeXLow  = joyCenterX;
  int strokeXHigh = 4095 - joyCenterX;
  joyStrokeX = max(min(strokeXLow, strokeXHigh), 100);

  int strokeYLow  = joyCenterY;
  int strokeYHigh = 4095 - joyCenterY;
  joyStrokeY = max(min(strokeYLow, strokeYHigh), 100);

  // ── 閾値をストロークの割合で動的計算 ──
  joyServoOnThX  = joyStrokeX * 15 / 100;
  joyServoOffThX = joyStrokeX *  8 / 100;
  joyServoOnThY  = joyStrokeY * 15 / 100;
  joyServoOffThY = joyStrokeY *  8 / 100;

  joyEyeOnThX  = joyStrokeX * 22 / 100;
  joyEyeOffThX = joyStrokeX * 12 / 100;
  joyEyeOnThY  = joyStrokeY * 22 / 100;
  joyEyeOffThY = joyStrokeY * 12 / 100;

  // ── LPF初期値を中心値に合わせる ──
  joyFilteredX = (float)joyCenterX;
  joyFilteredY = (float)joyCenterY;

  addLog("JOY CALIB: centerX=" + String(joyCenterX) + " centerY=" + String(joyCenterY)
         + " strokeX=" + String(joyStrokeX) + " strokeY=" + String(joyStrokeY));
  addLog("JOY THRESHOLDS: servoOnX=" + String(joyServoOnThX)
         + " servoOnY=" + String(joyServoOnThY)
         + " eyeOnX=" + String(joyEyeOnThX)
         + " eyeOnY=" + String(joyEyeOnThY));
}

// ====================================================
// recalibrateJoystickRuntime()
//
// disabled中の復帰時に呼ぶ。引数の安定値を新しい中心として採用し、
// applyJoyCalibThresholds() で全閾値・LPFを再計算してから
// joystickEnabled=true にする。
//
// 再キャリブ前に接続判定（condA/B/C）を再チェックし、
// 依然として浮き状態なら復帰しない。
// ====================================================
void recalibrateJoystickRuntime(int stableCenterX, int stableCenterY,
                                int meanDevX,       int meanDevY) {
  // ── 接続判定（condA/B/C: 旧来3条件 + condD: 中心値の妥当範囲）──
  // condD: 中立状態のADCは通常 1000〜3095 付近に収まる。
  //        それ以外の場合は倒しっぱなしか非接続と判断して再試行。
  const int JOY_CENTER_VALID_MIN = 1000;
  const int JOY_CENTER_VALID_MAX = 3095;
  bool condA = (stableCenterX < 300 && stableCenterY < 300);
  bool condB = (stableCenterX > 3800 || stableCenterY > 3800);
  bool condC = (meanDevX > JOY_FLOAT_VARIANCE_TH
             || meanDevY > JOY_FLOAT_VARIANCE_TH);
  bool condD = (stableCenterX < JOY_CENTER_VALID_MIN || stableCenterX > JOY_CENTER_VALID_MAX
             || stableCenterY < JOY_CENTER_VALID_MIN || stableCenterY > JOY_CENTER_VALID_MAX);

  if (condA || condB || condC || condD) {
    String reason = condA ? "low-ADC"
                  : condB ? "high-ADC"
                  : condC ? "floating(variance)"
                  :         "center-out-of-range";
    addLog("JOY RECALIB SKIP: still not connected (" + reason
           + " cx=" + String(stableCenterX) + " cy=" + String(stableCenterY)
           + " devX=" + String(meanDevX) + " devY=" + String(meanDevY) + ")");
    // ウィンドウをリセットして次のウィンドウで再試行
    joyStableCount       = 0;
    joyStableWindowStart = millis();
    return;
  }

  // ── 新しい中心値を採用して閾値を再計算 ──
  joyCenterX = stableCenterX;
  joyCenterY = stableCenterY;
  applyJoyCalibThresholds();

  // ── ストロークのバリデーション ──
  // 中心値が端に偏りすぎていると stroke が極端に小さくなり
  // 閾値が実質ゼロになって誤動作する。再試行して次ウィンドウへ。
  const int JOY_STROKE_MIN = 400;
  if (joyStrokeX < JOY_STROKE_MIN || joyStrokeY < JOY_STROKE_MIN) {
    addLog("JOY RECALIB SKIP: stroke too small (strokeX=" + String(joyStrokeX)
           + " strokeY=" + String(joyStrokeY)
           + " cx=" + String(joyCenterX) + " cy=" + String(joyCenterY) + ")");
    joyStableCount       = 0;
    joyStableWindowStart = millis();
    return;
  }

  // ── ランタイム浮き検出の状態をクリアして復帰 ──
  joystickEnabled      = true;
  joyFloatCount        = 0;
  joyFloatWindowStart  = millis();
  joyPrevRawX          = -1;
  joyPrevRawY          = -1;
  joyStableCount       = 0;
  joyStableWindowStart = 0;
  joyAbnormalCount     = 0;

  // ── ウォームアップ開始（ON直後 JOY_WARMUP_MS は入力無視）──
  joyWarmupUntil = millis() + JOY_WARMUP_MS;

  addLog("JOY ENABLED (runtime recalib)"
         " centerX=" + String(joyCenterX) + " centerY=" + String(joyCenterY)
         + " strokeX=" + String(joyStrokeX) + " strokeY=" + String(joyStrokeY)
         + " devX=" + String(meanDevX) + " devY=" + String(meanDevY));
  addLog("  servoOnThX=" + String(joyServoOnThX) + " servoOnThY=" + String(joyServoOnThY)
         + " servoOffThX=" + String(joyServoOffThX) + " servoOffThY=" + String(joyServoOffThY)
         + " eyeOnThX=" + String(joyEyeOnThX) + " eyeOnThY=" + String(joyEyeOnThY)
         + " warmup=" + String(JOY_WARMUP_MS) + "ms");
}

void drawBootFace() {
  sceneInvalidate();   // 起動画面は統合Canvasの対象外（この時点ではCanvas未確保）
  CoreS3.Display.fillScreen(WHITE);

  // 起動中の × 目
  CoreS3.Display.setTextDatum(MC_DATUM);
  CoreS3.Display.setTextColor(BLACK);
  CoreS3.Display.setTextSize(5);
  CoreS3.Display.drawString("X", 90, 90);
  CoreS3.Display.drawString("X", 230, 90);

  // 鼻と口は通常顔と同じ位置・固定
  updateNose(0);

  drawBattery();

  // ここで必ず左上基準に戻す
  CoreS3.Display.setTextDatum(TL_DATUM);
}

void playWakeTone();
void playSleepTone();
void servoDemoMotion();
bool handleAwakeModeTouch();
void handleUDP();
void updateExternalMouth();
void updateTalkMicroMotion();
void resetTalkMicroMotion();

void prepareLogToilet();

void drawWifiStatusOnClock() {
  CoreS3.Display.fillRect(0, 175, 320, 55, WHITE);

  CoreS3.Display.setTextSize(2);

  if (WiFi.status() == WL_CONNECTED) {
    CoreS3.Display.setTextColor(BLACK);

    CoreS3.Display.drawString(
      WiFi.SSID(),
      20,
      180);

    CoreS3.Display.drawString(
      WiFi.localIP().toString(),
      20,
      205);

  } else {
    CoreS3.Display.setTextColor(RED);

    CoreS3.Display.drawString(
      "WIFI FAILED",
      20,
      190);
  }
}

void drawIpStatusOnly() {
  String ipText;

  if (WiFi.getMode() == WIFI_AP) {
    ipText = "IP " + WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    ipText = "IP " + WiFi.localIP().toString();
  } else {
    ipText = "IP OFFLINE";
  }

  CoreS3.Display.setTextSize(2);
  CoreS3.Display.setTextColor(PURPLE);
  CoreS3.Display.drawString(ipText, 5, 26);
}

void drawClock() {
  addLog("DRAW CLOCK");

  // 時刻同期済みか確認する。
  // NTPまたはMac TIME_SYNCで一度でも同期できていれば、
  // getLocalTime()が失敗してもRTCの内部時刻を使って表示できる。
  // 一度も同期できていない場合は時計を表示しない（呼び出し元でFaceにフォールバック）。
  if (!timeEverSynced) {
    addLog("DRAW CLOCK SKIPPED: never synced");
    return;
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    // 同期済みだがgetLocalTime失敗（まれなケース）→ RTC継続として表示を試みる
    addLog("DRAW CLOCK: getLocalTime failed, using RTC");
  }

  CoreS3.Display.fillScreen(WHITE);

  // 目の線は描かず、目の位置に時計を出す
  updateClockText();

  updateNose(0);
  updateZzz();

  drawIpStatusOnly();
  drawBattery();
}

// 目の位置に「時:分」を描くだけ（消去なし・GFX経由・色指定可）。
// updateClockText()（単色白背景・消去あり・CoreS3.Display直描き）から呼ばれる。
// 数字の座標・書式をここへ一本化した（見た目は既存と完全に同じ）。
// getLocalTime()に失敗した場合は何も描かない（時計未表示のまま）＝既存仕様を踏襲。
void drawClockEyeDigits(uint32_t color) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return;

  char hourBuf[4];
  char minBuf[4];
  sprintf(hourBuf, "%02d", timeinfo.tm_hour);
  sprintf(minBuf, "%02d", timeinfo.tm_min);

  GFX.setTextColor(color);
  GFX.setTextSize(4);

  // 左目：時（太字風）
  GFX.drawString(hourBuf, 68, 68);
  GFX.drawString(hourBuf, 69, 68);
  GFX.drawString(hourBuf, 70, 68);

  // 中央：コロン
  GFX.drawString(":", 148, 68);
  GFX.drawString(":", 149, 68);
  GFX.drawString(":", 150, 68);

  // 右目：分（太字風）
  GFX.drawString(minBuf, 198, 68);
  GFX.drawString(minBuf, 199, 68);
  GFX.drawString(minBuf, 200, 68);
}

void updateClockText() {
  CoreS3.Display.fillRect(45, 58, 235, 60, WHITE);
  drawClockEyeDigits(BLACK);
}

// 白縁取り＋黒本体で1文字列を描く（Sleep Lighting Carousel「目×Lighting」専用）。
// 周囲へ白でオフセット描画してから同じ位置に黒本体を重ね、縁取り文字にする。
// 太さはSLEEP_OUTLINE_PX/2（顔パーツの白縁取りと統一）。
static void drawOutlinedGlyph(const String& text, int x, int y) {
  const int r = SLEEP_OUTLINE_PX / 2;
  GFX.setTextColor(WHITE);
  for (int dx = -r; dx <= r; dx++) {
    for (int dy = -r; dy <= r; dy++) {
      if (dx == 0 && dy == 0) continue;
      GFX.drawString(text, x + dx, y + dy);
    }
  }
  GFX.setTextColor(BLACK);
  GFX.drawString(text, x, y);
}

// 時計の目を黒＋白縁取りで描く（Sleep Lighting Carousel「目×Lighting」専用）。
// drawClockEyeDigits()と全く同じ座標・書式（時・コロン・分、各太字風に3回オフセット描画）を
// 踏襲し、1回ごとの描画をdrawOutlinedGlyph()に置き換えただけ（形・位置・サイズは無変更）。
void drawClockEyeDigitsOutlined() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return;

  char hourBuf[4];
  char minBuf[4];
  sprintf(hourBuf, "%02d", timeinfo.tm_hour);
  sprintf(minBuf, "%02d", timeinfo.tm_min);

  GFX.setTextSize(4);

  drawOutlinedGlyph(hourBuf, 68, 68);
  drawOutlinedGlyph(hourBuf, 69, 68);
  drawOutlinedGlyph(hourBuf, 70, 68);

  drawOutlinedGlyph(":", 148, 68);
  drawOutlinedGlyph(":", 149, 68);
  drawOutlinedGlyph(":", 150, 68);

  drawOutlinedGlyph(minBuf, 198, 68);
  drawOutlinedGlyph(minBuf, 199, 68);
  drawOutlinedGlyph(minBuf, 200, 68);
}

void updateZzz() {
  static String lastZzz = "";

  const char* zzzText;
  int zzzStep = (millis() / 500) % 3;

  if (zzzStep == 0) {
    zzzText = "Z";
  } else if (zzzStep == 1) {
    zzzText = "Zz";
  } else {
    zzzText = "Zzz...";
  }

  // 前回のZzzを白で上書きして消す
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setTextSize(3);
  CoreS3.Display.drawString(lastZzz, 20, 110);

  // 今回のZzzを黒で描く
  CoreS3.Display.setTextColor(BLACK);
  CoreS3.Display.drawString(zzzText, 20, 110);

  lastZzz = String(zzzText);
}

// 通常顔（白背景＋開眼＋鼻＋逆Y口）へ戻す統一入口。
// 顔の状態を初期値へ戻したうえで、統合パイプラインで1フレーム合成して全面転送する。
// 上端48pxは統合Canvasの対象外のため、従来の fillScreen(WHITE) と同じ結果になるよう
// ここで白へ戻す（直後に showSensors() が文字を描き直す既存の流れは不変）。
void drawFace() {
  imageFaceMode = false;

  gFaceEyeMode    = FACE_EYE_OPEN;
  gFaceNoseOffset = 0;
  gFaceMouthTalk  = false;

  if (gSceneReady && !sleepMode && !yawnMode) {
    // 上端パネルは統合Canvasの対象外なので、従来の fillScreen(WHITE) と同じ結果に
    // なるようここで白へ戻す（Lighting表示中は配色を showSensors() に任せる）。
    if (!screenFxLighting) CoreS3.Display.fillRect(0, 0, SCENE_W, SCENE_TOP, WHITE);
    sceneInvalidate();                                          // 次の転送を全面にする
    sceneRenderFace(0, SCENE_TOP, SCENE_W, SCENE_H - SCENE_TOP);
    return;
  }

  // ── フォールバック（Canvas未確保／睡眠・あくび中）：従来どおり液晶へ直接描く ──
  CoreS3.Display.fillScreen(WHITE);

  drawOpenEyes();

  CoreS3.Display.fillEllipse(noseX, noseY, 18, 12, BLACK);

  drawThickLine(noseX, noseY + 8, noseX, noseY + 22, 6, BLACK);
  drawThickLine(noseX, noseY + 22, noseX - 20, noseY + 32, 6, BLACK);
  drawThickLine(noseX, noseY + 22, noseX + 20, noseY + 32, 6, BLACK);
}

void drawFaceImage(const char* path) {
  sceneInvalidate();   // 画像顔は統合Canvasの対象外。復帰時に全面転送させる
  CoreS3.Display.fillScreen(WHITE);

  File file = SD.open(path);

  if (!file) {
    CoreS3.Display.setTextColor(RED);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.drawString("PNG OPEN FAIL", 40, 100);
    CoreS3.Display.drawString(path, 20, 130);
    showSensors();
    return;
  }

  size_t size = file.size();

  // サイズ上限チェック（巨大PNGによるヒープ枯渇を防ぐ）
  if (size == 0 || size > MAX_PNG_SIZE) {
    file.close();
    char sbuf[80];
    snprintf(sbuf, sizeof(sbuf), "PNG SIZE NG: size=%u max=%u", (unsigned)size, (unsigned)MAX_PNG_SIZE);
    CoreS3.Display.setTextColor(RED);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.drawString(sbuf, 10, 100);
    addLog(sbuf);
    showSensors();
    return;
  }

  uint8_t* buf = (uint8_t*)malloc(size);

  if (!buf) {
    file.close();
    CoreS3.Display.setTextColor(RED);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.drawString("PNG MALLOC FAIL", 40, 100);
    showSensors();
    return;
  }

  size_t readSize = file.read(buf, size);
  file.close();

  bool ok = false;
  if (readSize == size) {
    ok = CoreS3.Display.drawPng(buf, size, 0, 0);
  }

  free(buf);

  if (!ok) {
    CoreS3.Display.fillScreen(WHITE);
    CoreS3.Display.setTextColor(RED);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.drawString("PNG LOAD FAIL", 40, 100);
    CoreS3.Display.drawString(path, 20, 130);
  }

  imageFaceMode = true;

  showSensors();
}

// yawnMode（あくび中フラグ）は Unified Scene Canvas ブロックで宣言済み。

// 鼻の形（GFXへ描く）。逆Y口も従来どおりここに含む。
static void faceShapeNoseAndVMouth() {
  GFX.fillEllipse(noseX, noseY + gFaceNoseOffset, 18, 12, BLACK);

  drawThickLine(noseX, noseY + gFaceNoseOffset + 8, noseX, noseY + 22, 6, BLACK);
  drawThickLine(noseX, noseY + 22, noseX - 20, noseY + 32, 6, BLACK);
  drawThickLine(noseX, noseY + 22, noseX + 20, noseY + 32, 6, BLACK);
}

// 鼻・鼻から口への線・口を黒＋白縁取りで描く（Sleep Lighting Carousel「目×Lighting」専用）。
// 形状・座標は既存faceShapeNoseAndVMouth()と完全に同じ。白縁（各形状をSLEEP_OUTLINE_PXぶん
// 太く/大きくしたもの）を白で先に描いてから、黒本体は既存faceShapeNoseAndVMouth()を
// そのまま呼んで重ねるだけ（通常Faceの鼻・口描画＝faceShapeNoseAndVMouth()自体は無改造）。
static void drawNoseAndMouthOutlined() {
  const int o = SLEEP_OUTLINE_PX;
  // 実機確認2026-08-05: 鼻の白backingだけは通常Lighting側（drawVisualizerFaceParts）
  // と同程度の太さ（固定20×14）に合わせる。口・鼻口線のbackingは従来どおりSLEEP_OUTLINE_PXのまま。
  GFX.fillEllipse(noseX, noseY + gFaceNoseOffset, 20, 14, WHITE);
  drawThickLine(noseX, noseY + gFaceNoseOffset + 8, noseX, noseY + 22, 6 + o, WHITE);
  drawThickLine(noseX, noseY + 22, noseX - 20, noseY + 32, 6 + o, WHITE);
  drawThickLine(noseX, noseY + 22, noseX + 20, noseY + 32, 6 + o, WHITE);

  faceShapeNoseAndVMouth();
}

// おしゃべり口（鼻＋縦線＋赤い楕円）。ゆらぎは drawMouthOpen() が抽選した値を使う。
static void faceShapeNoseAndTalkMouth() {
  GFX.fillEllipse(noseX, noseY + gFaceNoseOffset, 18, 12, BLACK);
  drawThickLine(noseX, noseY + gFaceNoseOffset + 8, noseX, noseY + 25, 6, BLACK);
  GFX.fillEllipse(gFaceMouthMx, gFaceMouthMy, gFaceMouthMw, gFaceMouthMh, RED);
}

void updateNose(int newOffset) {
  if (imageFaceMode) return;

  // あくび中は鼻エリアの消去も逆Y口の描画もスキップする。
  // fillRect が赤い丸の上半分を消してしまうのを防ぐ。
  if (yawnMode) return;

  gFaceNoseOffset = newOffset;
  gFaceMouthTalk  = false;   // 従来どおり、鼻の再描画は口を逆Y口へ戻す

  lastNoseDrawTime = millis();  // 実際に鼻を描いた時刻（NOSE STALL診断用）

  // Sleep Lighting Carousel（目×Lighting表示）が動作中は、ここで直接液晶へ描かない。
  // 状態(gFaceNoseOffset)の更新だけ行い、実際の描画は次回のSleep合成サイクル
  // （sleepComposeEyeLightFrame）が背景Lightingごと描き直す際にまとめて反映する。
  // ここで直接描いてしまうと、Lighting背景の上に白い矩形の穴が開いてしまうため。
  if (sleepLightingComposeActive) return;

  if (sceneFaceOnCanvas()) { sceneRenderFace(FACE_MOUTH_X, FACE_MOUTH_Y, FACE_MOUTH_W, FACE_MOUTH_H); return; }

  // ── フォールバック（Canvas未確保／睡眠画面）：従来どおり液晶へ直接描く ──
  CoreS3.Display.fillRect(noseX - 35, noseY - 25, 70, 75, WHITE);
  faceShapeNoseAndVMouth();
}

// ====================================================
// sceneDrawNormalFace()
// 統合パイプラインの「4. 目・まつ毛・鼻・口を描画」。
// Lighting も Visualizer も表示していない通常表示のときの顔1フレーム分。
// 白消去は行わない（呼び出し元が毎フレーム背景から作り直すため不要）。
// ====================================================
void sceneDrawNormalFace() {
  if (imageFaceMode) return;

  switch (gFaceEyeMode) {
    case FACE_EYE_BLINK: faceShapeBlinkEyes(); break;
    case FACE_EYE_PET:   faceShapePetEyes();   break;
    default:             faceShapeOpenEyes();  break;
  }

  if (gFaceMouthTalk) faceShapeNoseAndTalkMouth();
  else                faceShapeNoseAndVMouth();
}

void drawSleepScreen() {
  addLog("DRAW SLEEP SCREEN");

  // 睡眠画面は統合Canvasの対象外（液晶へ直接描く）。通常画面へ戻ったときに
  // 前フレームのCanvas内容と食い違わないよう、次回の転送を全面にする。
  sceneInvalidate();

  // sleep移行時は必ず全画面クリアしてから描画する。
  // 部分消去では赤い丸口の残像が残る問題が繰り返し発生したため、
  // 白画面が一瞬出ることより残像の方が問題として全画面消去に変更。
  CoreS3.Display.fillScreen(WHITE);

  drawSleepEyes();
  updateNose(0);
  updateZzz();
  drawBattery();

  // 睡眠中はVAM/IPを更新しない
}

void blinkEyes() {
  if (imageFaceMode) return;
  drawBlinkEyes();
  delay(80);
  drawOpenEyes();
  // まばたき後に鼻を再描画（目の消去矩形が鼻上端に被った場合の保険）
  updateNose(toggle ? -1 : 1);
}

void yawn() {

  // あくび表示とyawn.wav再生だけ担当。
  // sleep顔への描画責任は drawSleepScreen() に一本化。
  yawnMode = true;
  sceneInvalidate();   // あくび画面は統合Canvasの対象外。復帰時に全面転送させる

  CoreS3.Display.fillScreen(WHITE);
  drawOpenEyes();
  CoreS3.Display.fillEllipse(noseX, noseY, 18, 12, BLACK);

  showSensors();  // showSensors を先に呼んでから赤い丸を描く

  // あくびの口（赤い丸）を最後に描く（他の描画に上書きされないよう）
  CoreS3.Display.fillCircle(noseX, noseY + 45, 25, RED);

  unsigned long savedInteractionTime = lastInteractionTime;

  if (!soundBusy) {
    File yawnCheck = SD.open("/sounds/yawn.wav");
    if (yawnCheck) {
      yawnCheck.close();
      playWavFromSD("/sounds/yawn.wav", true);
    } else {
      smartDelay(1200);
    }
  } else {
    smartDelay(1200);
  }

  // WAV再生後にスリープタイマーをリセットしないよう元の値に戻す
  lastInteractionTime = savedInteractionTime;

  yawnMode = false;  // あくび終了。drawSleepScreen()が全画面クリアするので残像は残らない

  // yawn() はここで終了。sleep顔は drawSleepScreen() が担当する。
}

void drawMouthOpen() {
  if (imageFaceMode) return;

  // おしゃべり用の口：鼻に近い横長楕円。ゆらぎの抽選は従来と同じ（呼ばれるたびに再抽選）。
  gFaceMouthMx = noseX + random(-1, 2) * 2;
  gFaceMouthMy = noseY + 26 + random(-1, 2) * 2;
  gFaceMouthMw = 18 + random(-1, 2) * 2;
  gFaceMouthMh = 12 + random(-1, 2) * 2;
  gFaceMouthTalk = true;

  if (sceneFaceOnCanvas()) { sceneRenderFace(FACE_MOUTH_X, FACE_MOUTH_Y, FACE_MOUTH_W, FACE_MOUTH_H); return; }

  // ── フォールバック（Canvas未確保／睡眠・あくび中）：従来どおり液晶へ直接描く ──
  // 逆Y口のV部分を消す
  CoreS3.Display.fillRect(
    noseX - 35,
    noseY + 25,
    70,
    55,
    WHITE);

  // 鼻から口への縦線を描き直す
  drawThickLine(
    noseX,
    noseY + 8,
    noseX,
    noseY + 25,
    6,
    BLACK);

  CoreS3.Display.fillEllipse(
    gFaceMouthMx,
    gFaceMouthMy,
    gFaceMouthMw,
    gFaceMouthMh,
    RED);

  // showSensors();
}

void drawMouthClosed() {
  if (imageFaceMode) return;

  gFaceMouthTalk = false;

  if (sceneFaceOnCanvas()) { sceneRenderFace(FACE_MOUTH_X, FACE_MOUTH_Y, FACE_MOUTH_W, FACE_MOUTH_H); return; }

  // ── フォールバック（Canvas未確保／睡眠・あくび中）：従来どおり液晶へ直接描く ──
  // 鼻〜口まわりだけ広めに消す
  CoreS3.Display.fillRect(
    noseX - 45,
    noseY - 15,
    90,
    85,
    WHITE);

  // 鼻
  CoreS3.Display.fillEllipse(noseX, noseY, 18, 12, BLACK);

  // 元の逆Y口
  drawThickLine(noseX, noseY + 8, noseX, noseY + 22, 6, BLACK);
  drawThickLine(noseX, noseY + 22, noseX - 20, noseY + 32, 6, BLACK);
  drawThickLine(noseX, noseY + 22, noseX + 20, noseY + 32, 6, BLACK);
}

void mouthPakuPaku() {
  if (imageFaceMode) return;

  for (int i = 0; i < 4; i++) {
    drawMouthOpen();
    smartDelay(120);

    drawMouthClosed();
    smartDelay(120);
  }
}

bool isHeadTouched() {
  auto touch = CoreS3.Touch.getDetail();
  return touch.isPressed() && touch.y < 180;
}

void showDebug(const char* msg) {
  CoreS3.Display.fillRect(260, 24, 60, 24, WHITE);

  if (strcmp(msg, "S") == 0) {
    CoreS3.Display.setTextColor(MAGENTA);
  } else if (strcmp(msg, "W") == 0) {
    CoreS3.Display.setTextColor(CYAN);
  } else if (strcmp(msg, "C") == 0) {
    CoreS3.Display.setTextColor(RED);
  } else if (strcmp(msg, "I") == 0) {
    CoreS3.Display.setTextColor(ORANGE);
  } else if (strcmp(msg, "N") == 0) {
    CoreS3.Display.setTextColor(BLUE);
  } else {
    CoreS3.Display.setTextColor(BLACK);
  }

  CoreS3.Display.setTextSize(2);
  CoreS3.Display.drawString(msg, 270, 28);
}

void showEvent(const char* msg) {
  showDebug(msg);
  smartDelay(300);  // 旧:delay(300) → イベント表示中もWeb/UDP/鼻動作を継続
  showDebug("-");
}

// ====================================================
// isExternalPowered()
// 外部給電（USB-C / DIN BASE の DC INPUT 9-24V）中かどうかを返す。
// AXP2101 の外部入力は VBUS の1系統のみ（AXP192 にあった ACIN は
// 廃止され、M5Unified の Axp2101.isACIN() は常に false を返すスタブ）。
// USB-C も DIN BASE の DC INPUT（9-24V→5V降圧後にバスへ供給）も
// 最終的に同じ VBUS ピンに合流するため、isVBUS() が事実上
// 「外部給電中」の判定となる。
// 判定方法を将来変更する場合はこの関数だけ直せばよい。
// ====================================================
bool isExternalPowered() {
  return CoreS3.Power.Axp2101.isVBUS();
}

// ====================================================
// drawBattery()
// おでこ右側にバッテリー残量を常時表示する。
// showSensors() が呼ばれない画面（スリープ・時計・BootFace）でも
// この関数を呼ぶことで常にバッテリー状態を確認できる。
// 表示位置：右上 (230, 4) 付近（showSensors と同じ座標）
//
// アイコン判定：充電中かどうかではなく VBUS（USB電源入力）の有無で切替。
//   USB給電中  → プラグアイコン（接触子上向き・黒）
//   バッテリー → バッテリーアイコン（矩形 + 残量充填）
// 残量ロジック・更新周期・表示位置は変更なし。
// ====================================================
void drawBattery() {
  // 起動シーケンス中（×目画面・WiFi接続中等）は描画しない。
  // batteryDisplayEnabled は setup()末尾の drawFace() 直前で true になる。
  if (!batteryDisplayEnabled) return;

  int batteryLevel = CoreS3.Power.getBatteryLevel();

  // 外部給電判定（USB-C / DIN BASE DC INPUT）。詳細は isExternalPowered() 参照。
  bool usbPower    = isExternalPowered();

  uint16_t batColor;
  if (batteryLevel > 50) {
    batColor = GREEN;
  } else if (batteryLevel > 20) {
    batColor = ORANGE;
  } else {
    batColor = RED;
  }

  // 上端パネルのテーマに合わせて電池アイコンの配色も切り替える（v1.6）。
  // 採用中の背景Lightingのヘッダーテーマ（白/黒）に従う。
  // Sleep Lighting Carousel中（sleepLightingComposeActive=true）だけは、
  // Sleep側が独自に選んだ背景Lightingのテーマ(sleepPanelDark)を使う。
  // それ以外（false）は従来の判定式と完全に同じ結果になる。
  bool panelBlack = sleepLightingComposeActive
                       ? sleepPanelDark
                       : (lightingScreenActive() && lightingHeaderDark());
  uint16_t iconFg = panelBlack ? WHITE : BLACK;

  // 背景を消してから描画（他の表示を汚さないよう右上エリアのみ）
  CoreS3.Display.fillRect(225, 0, 95, 24, panelBlack ? BLACK : WHITE);

  CoreS3.Display.setTextSize(2);

  if (usbPower) {
    // プラグアイコン（接触子が上向き・黒）
    // ピクセルレイアウト (x=229〜244, y=3〜20):
    //   接触子 左: fillRect(231,3,3,5)  右: fillRect(239,3,3,5)
    //              ──5px間隔──
    //   本体:      fillRect(229,8,15,9)  ← 接触子より幅広で包む
    //   コード:    fillRect(235,17,4,3)  ← 本体中央から下に短く
    CoreS3.Display.fillRect(231, 3,  3, 5, iconFg);   // 接触子（左）
    CoreS3.Display.fillRect(239, 3,  3, 5, iconFg);   // 接触子（右）
    CoreS3.Display.fillRect(229, 8, 15, 9, iconFg);   // 本体
    CoreS3.Display.fillRect(235, 17, 4, 3, iconFg);   // コード
  } else {
    // 🔋 バッテリーアイコン：外枠 + ポジ端子 + 残量充填
    CoreS3.Display.drawRect(230, 5, 14, 14, iconFg);         // ボディ外枠
    CoreS3.Display.fillRect(244, 9, 3, 6, iconFg);           // ポジ端子
    int fillW = max(1, batteryLevel * 12 / 100);
    CoreS3.Display.fillRect(231, 6, fillW, 12, batColor);     // 残量充填
  }

  CoreS3.Display.setTextColor(iconFg);
  // DIN BASE の POWER OFF 時は内蔵バッテリーが切り離され、
  // USB給電中でも getBatteryLevel() が 0 を返す。
  // その場合は "0%" ではなく "OFF" と表示する。
  //
  // 2026/07/20 修正:
  //   旧コメントには「AXP2101 reg 0x00 bit3 に BAT present 表示があるが、
  //   M5Unified の AXP2101_Class に公開APIがない」とあったが、これは誤り。
  //   M5Unified の実ソースに
  //     bool AXP2101_Class::getBatState(void)
  //     { return readRegister8(0x00) & 0x08; }
  //   が public として存在するため、電池実装状態を直接取得できる。
  //
  //   見た目を変えないため、従来の判定条件（usbPower && level==0）は
  //   そのまま残し、getBatState() による判定を「追加」する形にした。
  //   これにより、電池が物理的に切り離されているのに残量値が0以外を
  //   返すケースでも正しく "OFF" と表示できる。
  //   従来 "OFF" になっていたケースの表示は一切変わらない。
  bool batPresent = CoreS3.Power.Axp2101.getBatState();

  if (usbPower && (!batPresent || batteryLevel == 0)) {
    CoreS3.Display.drawString("OFF", 252, 4);
  } else {
    CoreS3.Display.drawString(String(batteryLevel) + "%", 252, 4);
  }
}

void showSensors() {

  // 上端48pxのテーマは「今表示中の背景Lightingが持つヘッダーテーマ」で決める（v1.6）。
  //   ・lightingScreenActive() … 実際にLighting画面表示中か（sleep/画像顔/音停止/OFFで白へ）
  //   ・lightingHeaderDark()    … 採用中の背景に応じて黒(true)/白(false)
  //   例: Discoのみ/Disco+Laser→白、Laserのみ/Aurora/Aurora+Laser→黒、Disco+Aurora→黒(Aurora採用)
  bool panelBlack = lightingScreenActive() && lightingHeaderDark();
  CoreS3.Display.fillRect(0, 0, 320, 48, panelBlack ? BLACK : WHITE);

  CoreS3.Display.setTextSize(2);

  // V/A/M の色ラベルは黒背景でも視認できる明色。IPは黒背景時のみ白へ。
  CoreS3.Display.setTextColor(panelBlack ? CYAN : MAGENTA);
  CoreS3.Display.drawString(
    "V" + String(currentVolume),
    5, 4);

  CoreS3.Display.setTextColor(ORANGE);
  CoreS3.Display.drawString(
    "A" + String(currentAccel, 1),
    80, 4);

  CoreS3.Display.setTextColor(panelBlack ? YELLOW : RED);
  CoreS3.Display.drawString(
    "M" + String(currentMotionLevel),
    160, 4);

  String ipText;

  if (WiFi.getMode() == WIFI_AP) {
    ipText = "IP " + WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    ipText = "IP " + WiFi.localIP().toString();
  } else {
    ipText = "IP OFFLINE";
  }

  CoreS3.Display.setTextSize(2);
  CoreS3.Display.setTextColor(panelBlack ? WHITE : PURPLE);
  CoreS3.Display.drawString(ipText, 5, 26);

  drawBattery();
}

void blinkNormal() {
  blinkEyes();
}

void blinkSound() {
  showEvent("S");
  blinkEyes();
}

void blinkImu() {
  showEvent("I");
  blinkEyes();
  smartDelay(120);
  blinkEyes();
}

void blinkCamera() {
  showEvent("C");
  blinkEyes();
  smartDelay(120);
  blinkEyes();
  smartDelay(120);
  blinkEyes();
}

void setEyeDirection(int offsetX, int offsetY, const String& label) {
  if (imageFaceMode) return;

  // 同じ方向なら再描画しない。画面ちらつき防止。
  if (eyeOffsetX == offsetX && eyeOffsetY == offsetY && eyeDirectionLabel == label) {
    return;
  }

  eyeOffsetX = offsetX;
  eyeOffsetY = offsetY;
  eyeDirectionLabel = label;

  drawOpenEyes();
}

void updateCameraGaze(uint16_t* pixels, int width, int height) {
  if (!ENABLE_CAMERA_GAZE || !cfg_enableCamera) return;
  if (imageFaceMode || sleepMode || petMode || alertMode || soundBusy || externalSpeaking) return;

  int motionCount = 0;
  long sumX = 0;
  long sumY = 0;

  for (int gy = 0; gy < 12; gy++) {
    for (int gx = 0; gx < 16; gx++) {

      int x = gx * (width / 16);
      int y = gy * (height / 12);

      uint16_t p = pixels[y * width + x];

      int r = (p >> 11) & 0x1F;
      int g = (p >> 5) & 0x3F;
      int b = p & 0x1F;

      uint8_t gray = (r * 8 + g * 4 + b * 8) / 3;

      if (gazeHasPrev) {
        int diff = abs((int)gray - (int)gazePrev[gx][gy]);

        if (diff > GAZE_DIFF_THRESHOLD) {
          motionCount++;
          sumX += x;
          sumY += y;
        }
      }

      gazePrev[gx][gy] = gray;
    }
  }

  if (gazeHasPrev && motionCount >= GAZE_MIN_COUNT && millis() - lastGazeUpdate > GAZE_UPDATE_INTERVAL) {
    lastGazeUpdate = millis();

    int centerX = sumX / motionCount;
    int centerY = sumY / motionCount;

    gazeLastCenterX = centerX;
    gazeLastCenterY = centerY;

    if (centerX < GAZE_LEFT_THRESHOLD) {
      gazeState = -1;
      lastGazeTime = millis();
      setEyeDirection(-GAZE_EYE_X, 0, "GAZE_LEFT");
    } else if (centerX > GAZE_RIGHT_THRESHOLD) {
      gazeState = 1;
      lastGazeTime = millis();
      setEyeDirection(GAZE_EYE_X, 0, "GAZE_RIGHT");
    }
  }

  gazeHasPrev = true;

  if (millis() - lastGazeTime > GAZE_RETURN_MS) {
    if (gazeState != 0) {
      gazeState = 0;
      setEyeDirection(0, 0, "GAZE_CENTER");

      // 視線が戻るタイミングで首もセンターへ戻す
      if (canSleep() && millis() - lastInteractionTime > SLEEP_TIMEOUT) {
        servoBusy = true;
        moveSmooth(servoUD, udNow, HEAD_VERTICAL_CENTER, 20);
        lrMoveCaller = "gaze_center";
        moveSmooth(servoLR, lrNow, HEAD_HORIZONTAL_CENTER, 20);
        servoBusy = false;
      }
    }
  }
}

void updateEyesByCameraMotion(long leftMotion, long rightMotion) {
  long totalMotion = leftMotion + rightMotion;

  // 動きが小さい時は中央へ戻す。
  if (totalMotion < MOTION_TRIGGER) {
    setEyeDirection(0, 0, "CENTER");
    return;
  }

  if (leftMotion > rightMotion + MOTION_DIFF) {
    // カメラ左側の動き：まず黒目だけ左へチラッ
    setEyeDirection(EYE_SHIFT_PIXELS, 0, "LEFT");

  } else if (rightMotion > leftMotion + MOTION_DIFF) {
    // カメラ右側の動き：まず黒目だけ右へチラッ
    setEyeDirection(-EYE_SHIFT_PIXELS, 0, "RIGHT");

  } else {
    setEyeDirection(0, 0, "CENTER");
  }
}

void saveServoPositionToRtc() {
  rtcServoMagic = SERVO_RTC_MAGIC;
  rtcUdNow = udNow;
  rtcLrNow = lrNow;
}

void restoreServoPositionFromRtc() {

  // 起動時は常にセンター位置から開始する（安全設計）。
  // RTC復帰（前回位置から再開）は attach直後の急動リスクがあるため廃止済み。
  // 電源OFF前に safeServoShutdown() でセンターへ戻してdetachするため、
  // 次回起動時はセンター開始で問題ない。

  udNow   = HEAD_VERTICAL_CENTER;
  lrNow = HEAD_HORIZONTAL_CENTER;

  // udNow/lrNow を安全範囲にクランプ（定数変更時の保護）
  udNow = constrain(udNow, SERVO_UD_DOWN, SERVO_UD_UP);
  lrNow = constrain(lrNow, SERVO_LR_RIGHT, SERVO_LR_LEFT);

  saveServoPositionToRtc();

  addLog("RESTORE SERVO: center start ud=" + String(udNow) + " lr=" + String(lrNow));
}

void writeServoHoldCurrentPosition() {
  // attach直後：udNow/lrNowは常にセンターのため急動しない。
  // センター角度をゆっくり書き込んで安定させる。
  // 無効化された軸は write しない（setup側で attach 自体もスキップ済み）。
  if (ENABLE_UD_SERVO)   servoUD.write(udNow);
  if (ENABLE_LR_SERVO) servoLR.write(lrNow);
  delay(300);  // サーボ位置安定待ち（attach直後・必須・残す）
}

// ====================================================
// safeServoShutdown()
// 電源OFF・終了系処理の直前に呼ぶ。
// 現在位置からセンターへゆっくり戻し、位置を保存してからdetachする。
// detachするとサーボがトルクフリーになり、電源OFF時に外力で動いても
// 次回起動時にセンターから安全に再開できる。
// ====================================================
void safeServoShutdown() {
  addLog("SERVO SAFE SHUTDOWN START");

  // LRサーボが自動 detach されていた場合に備えて再 attach する。
  // moveSmooth 内でも自動 attach されるが、ここで明示的に行う。
  // ENABLE_LR_SERVO=false の場合は対象外。
  if (ENABLE_LR_SERVO && !servoLR.attached()) {
    servoLR.attach(LR_SERVO_PIN);
    delay(LR_ATTACH_SETTLE_MS);
    servoLR.write(lrNow);
    delay(LR_ATTACH_SETTLE_MS);
  }

  // 現在位置からセンターへゆっくり戻す
  // （moveSmooth内部で無効軸は自動スキップされる）
  servoBusy = true;
  moveSmooth(servoUD,   udNow,   HEAD_VERTICAL_CENTER,   20);
  lrMoveCaller = "shutdown";
  moveSmooth(servoLR, lrNow, HEAD_HORIZONTAL_CENTER,  20);
  servoBusy = false;

  // センター位置を保存
  saveServoPositionToRtc();

  delay(100);  // detach前の安定待ち（必須・短いため残す）

  // detachしてトルクフリーに（電源OFF時の暴れを防ぐ）
  // 無効軸はそもそも attach されていないため detach() は安全だが、
  // 明示的にガードして意図を明確にする。
  if (ENABLE_UD_SERVO   && servoUD.attached())   servoUD.detach();
  if (ENABLE_LR_SERVO && servoLR.attached()) servoLR.detach();

  addLog("SERVO SAFE SHUTDOWN DONE: detached");
}

// ====================================================
// サーボ移動イージング（2026/07/07）
//
// 従来の等速移動（1度ごとに waitMs 固定）を、動き始めと終わりが
// ゆっくりになる ease-in-out へ変更。生き物らしい動きと、
// 停止・発進時の瞬間トルク負荷の軽減（LRサーボの悲鳴対策）が目的。
//
// カーブ：中央区間は従来どおり waitMs、両端へ近づくほど
//   waitMs × (1 + SERVO_EASE_EDGE_GAIN × edge^2)
// まで減速する（edge=0:中央, 1:両端）。
//
// GAIN=1.5 の場合：
//   両端 = waitMs×2.5、中央 = waitMs×1.0、平均 ≒ waitMs×1.5
// → 全体速度も約1.5倍ゆっくりになる（速度より滑らかさ優先の方針）。
// 呼び出し側の waitMs 指定・関数シグネチャは一切変更しない。
// ====================================================
const float SERVO_EASE_EDGE_GAIN = 1.5f;

// ====================================================
// LR_SLOWDOWN_FACTOR（Yaw標準速度倍率）
//
// Yaw（servoLR）のみ、moveSmooth の1度あたり待ち時間をこの倍率で延長する。
// 実機検証の結果、速度を約50%（FACTOR=2）にすると動きが落ち着き
// 動物らしく自然に見えることが確認されたため、正式仕様として設定。
// Pitch（servoUD）には一切影響しない（moveSmooth内で軸判定して適用）。
// ジョイスティック追従は別系統（JOY_LR_SLEW_STEP_MS）のため個別に設定。
//   2 = Yaw速度約50%（現在の標準仕様）
//   1 = 従来速度（速度比較をしたい場合のみ）
// ====================================================
const int LR_SLOWDOWN_FACTOR = 2;

void moveSmooth(Servo& s, int& nowAngle, int targetAngle, int waitMs) {
  // 軸が無効化されている場合は attach/write/detach を一切行わず即終了。
  // nowAngle はセンター値のまま据え置く（呼び出し元の計算には影響しない）。
  if (&s == &servoUD && !ENABLE_UD_SERVO) {
    nowAngle = targetAngle;  // ロジック上の整合性のみ保つ（実機には反映しない）
    return;
  }
  if (&s == &servoLR && !ENABLE_LR_SERVO) {
    nowAngle = targetAngle;
    return;
  }

  // LRサーボが detach されていたら動作前に再 attach する。
  // UDサーボは常時 attach のため対象外。
  // moveSmooth はサーボ操作の最終経路なので、ここで一括して担保する。
  if (&s == &servoLR && !servoLR.attached()) {
    // ── LR_ATTACH_SAG_OFFSET_DEG による急動作緩和 ──────────────────
    // detach中に頭が自重でズレた場合、write(lrNow) の瞬間に全速補正が起きる。
    // SAG_OFFSETを物理ズレ方向に設定し「推定物理位置→lrNow」のランプを通すことで
    // PWM開始時の瞬間補正量を最小化する。
    // 正負の決め方: 頭が右流れ→負、左流れ→正（LR_ATTACH_SAG_OFFSET_DEGコメント参照）
    int sagOffset   = LR_ATTACH_SAG_OFFSET_DEG;
    int attachAngle = constrain(lrNow + sagOffset, SERVO_LR_RIGHT, SERVO_LR_LEFT);
    const char* sagDir = (sagOffset > 0) ? "L" : (sagOffset < 0) ? "R" : "-";

    servoLR.attach(LR_SERVO_PIN);
    delay(LR_ATTACH_SETTLE_MS);  // PWM前安定待ち（必須）
    servoLR.write(attachAngle);  // PWM開始: 物理位置推定値から通電（急動抑制）
    addLog("LR RE-ATTACH (moveSmooth): attachAngle=" + String(attachAngle)
           + " lrNow=" + String(lrNow)
           + " SAG=" + String(sagOffset) + " dir=" + String(sagDir));
    delay(LR_ATTACH_SETTLE_MS);  // 通電安定待ち

    // SAGオフセット分を 1度/50ms でlrNowへ戻してから本ステップループへ
    // SAG=0のとき attachAngle==lrNow のためループはスキップされる（挙動不変）
    while (attachAngle != lrNow) {
      attachAngle += (lrNow > attachAngle) ? 1 : -1;
      servoLR.write(attachAngle);
      delay(50);  // 1度/50ms ≒ IDLE PRE-ATTACHランプと同速
    }
  }

  // サーボ保護：どこから呼ばれても 10〜170 度の範囲外に出ないようにクランプ。
  targetAngle = constrain(targetAngle, 10, 170);

  // Yaw（servoLR）のみ1度あたり待ち時間を標準倍率で延長。
  // Pitch（servoUD）は無変更。moveSmooth を通るYaw全経路に一括で効く。
  if (&s == &servoLR) waitMs *= LR_SLOWDOWN_FACTOR;

  // ── イージング付きステップ移動（2026/07/07）＋LR ABORT-safe（2026/07/11）─
  bool     isLrMove   = (&s == &servoLR);
  String   thisCaller = lrMoveCaller;
  uint32_t myGen      = 0;
  bool     lrAborted  = false;

  if (isLrMove) {
    myGen = ++lrMoveGen;
    const char* dirStr = (targetAngle > nowAngle) ? "L(+)"
                       : (targetAngle < nowAngle) ? "R(-)" : "=";
    addLog("LR MOVE START caller=" + thisCaller + " start=" + String(nowAngle)
           + " target=" + String(targetAngle) + " dir=" + String(dirStr)
           + " lrNow=" + String(lrNow) + " att=" + String(servoLR.attached() ? "Y" : "N")
           + " t=" + String(millis()));
  }

  int totalSteps = abs(targetAngle - nowAngle);

  if (totalSteps == 0) {
    server.handleClient();
    handleUDP();
    if (isLrMove && lrMoveGen != myGen) {
      addLog("LR MOVE ABORT(0step) caller=" + thisCaller + " gen=" + String(myGen) + "->" + String(lrMoveGen) + " lrNow=" + String(lrNow) + " t=" + String(millis()));
      lrAborted = true;
    } else {
      s.write(targetAngle);
      delay(waitMs);
    }
  } else {
    int dir = (targetAngle > nowAngle) ? 1 : -1;

    for (int i = 1; i <= totalSteps; i++) {
      server.handleClient();
      handleUDP();
      if (isLrMove && lrMoveGen != myGen) {
        addLog("LR MOVE ABORT caller=" + thisCaller + " gen=" + String(myGen) + "->" + String(lrMoveGen)
               + " step=" + String(i) + "/" + String(totalSteps) + " lrNow=" + String(lrNow)
               + " target=" + String(targetAngle) + " t=" + String(millis()));
        lrAborted = true;
        break;
      }
      s.write(nowAngle + dir * i);

      float progress = (totalSteps > 1)
                       ? (float)(i - 1) / (float)(totalSteps - 1)
                       : 0.5f;
      float edge = fabsf(progress - 0.5f) * 2.0f;
      int stepWait = (int)(waitMs * (1.0f + SERVO_EASE_EDGE_GAIN * edge * edge) + 0.5f);
      delay(stepWait);
    }
  }

  if (!lrAborted) { nowAngle = targetAngle; }
  saveServoPositionToRtc();

  if (isLrMove) {
    if (!lrAborted) lrMarkMoved();
    addLog("LR MOVE " + String(lrAborted ? "ABORT END" : "END") + " caller=" + thisCaller
           + " lrNow=" + String(lrNow) + " att=" + String(servoLR.attached() ? "Y" : "N")
           + " t=" + String(millis()));
  } else if (&s == &servoLR) {
    lrMarkMoved();
  }

  // ── ジョイスティック幻入力対策・主対策（2026/07/09 改修2）──────────
  joyServoResumeAt = millis() + JOY_SERVO_RESUME_DELAY_MS;
  lastServoMoveEndAt = millis();  // 浮き判定グレース（改修4）の起点
}

void smartDelay(unsigned long ms) {
  unsigned long start = millis();

  while (millis() - start < ms) {
    server.handleClient();
    handleUDP();
    updateExternalMouth();
    handleNoseMotion();
    handleJoystick();
    delay(1);
  }
}

// ====================================================
void moveWithEyeLead(Servo& s, int& nowAngle, int targetAngle, int waitMs,
                     int eyeX, int eyeY, const String& label) {
  // パックマンのモンスター移動法：
  // 先に移動方向を見る → 首が動く → 到着後に視線を中央へ戻す。
  setEyeDirection(eyeX, eyeY, label);
  // 旧: smartDelay(120) はここにあったが体感できないため削除

  moveSmooth(s, nowAngle, targetAngle, waitMs);

  // 旧: smartDelay(120) はここにあったが体感できないため削除
  setEyeDirection(0, 0, "CENTER");

  updateNose(0);
}

void moveToCenterWithEyeLead(Servo& s, int& nowAngle, int targetAngle, int waitMs) {
  // 正面へ戻る動きも直接moveSmooth()せず、目線を中央に固定してから動かす。
  setEyeDirection(0, 0, "CENTER");
  smartDelay(80);  // 旧:delay(80) → Web/UDP を止めず待機
  moveSmooth(s, nowAngle, targetAngle, waitMs);
  smartDelay(80);  // 旧:delay(80)
  setEyeDirection(0, 0, "CENTER");
}

void lookAtMotionDirection(long leftMotion, long rightMotion) {
  if (millis() - lastLookAtMotion < MOTION_LOOK_COOLDOWN) return;

  long totalMotion = leftMotion + rightMotion;
  if (totalMotion < MOTION_TRIGGER) return;

  if (leftMotion > rightMotion + MOTION_DIFF) {
    // パックマンのモンスター移動法：先に左を見る → 首が左へ → 目を中央へ戻す
    servoBusy = true;
    lrMoveCaller = "cam";
    moveWithEyeLead(servoLR, lrNow, HEAD_LEFT, 10,
                    EYE_SHIFT_PIXELS, 0, "LEFT");
    servoBusy = false;
    lastLookAtMotion = millis();

  } else if (rightMotion > leftMotion + MOTION_DIFF) {
    // パックマンのモンスター移動法：先に右を見る → 首が右へ → 目を中央へ戻す
    servoBusy = true;
    lrMoveCaller = "cam";
    moveWithEyeLead(servoLR, lrNow, HEAD_RIGHT, 10,
                    -EYE_SHIFT_PIXELS, 0, "RIGHT");
    servoBusy = false;
    lastLookAtMotion = millis();
  }
}

void lookAround() {
  // 実機の左右軸は servoLR 側。attach()は触らない。
  lrMoveCaller = "lookAround";
  moveWithEyeLead(servoLR, lrNow, HEAD_LEFT, 15,
                  EYE_SHIFT_PIXELS, 0, "LEFT");
  smartDelay(300);  // 旧:delay(300)

  lrMoveCaller = "lookAround";
  moveWithEyeLead(servoLR, lrNow, HEAD_RIGHT, 15,
                  -EYE_SHIFT_PIXELS, 0, "RIGHT");
  smartDelay(300);  // 旧:delay(300)

  lrMoveCaller = "lookAround_center";
  moveToCenterWithEyeLead(servoLR, lrNow, HEAD_HORIZONTAL_CENTER, 15);
}


void sleepyLook() {
  // 実機の上下軸は servoUD 側。下を向いてから正面へ戻す。
  moveWithEyeLead(servoUD, udNow, HEAD_DOWN, 25,
                  0, EYE_SHIFT_PIXELS, "DOWN");
  smartDelay(500);

  moveToCenterWithEyeLead(servoUD, udNow, HEAD_VERTICAL_CENTER, 25);
}


void wakeUp(String wakeReason) {

  // playWakeTone();

  {
    char wbuf[128];
    snprintf(wbuf, sizeof(wbuf),
             "WAKE START reason=%s V=%d A=%.1f M=%d",
             wakeReason.c_str(), currentVolume, currentAccel, currentMotionLevel);
    addLog(wbuf);
  }

  // 2026/07/21: 起床時に戻す明るさを固定値128→標準輝度定数(70%相当)へ統一。
  CoreS3.Display.setBrightness(LCD_BACKLIGHT_STANDARD);  // 起床時に液晶の明るさを戻す

  sleepMode = false;
  lastInteractionTime = millis();

  // スリープ中Face表示フラグをリセット
  // （wakeUp後は drawFace() が通常顔に戻すため imageFaceMode も自動解除される）
  sleepFaceActive = false;
  lastSleepFaceRotateTime = 0;
  lastSleepFaceAttemptTime = 0;

  // Sleep Lighting Carousel（目×Lighting）の状態を起床時に確実にクリアする。
  // sleepLightingComposeActiveをここでfalseに戻すことで、以降のupdateNose()は
  // 通常どおりの経路（sceneFaceOnCanvas()）に戻る。gEyeSlotActive/eyeOffsetX/Yも
  // Eye Slotの目を使っていた場合に備えて明示的にリセットする（通常のLighting合成側でも
  // 毎フレームリセットされるため必須ではないが、Sleep側の後始末として明示しておく）。
  sleepLightingComposeActive = false;
  sleepCarouselStarted       = false;
  sleepCarouselNextSwitchMs  = 0;
  sleepLastEyeKind           = -1;
  sleepLastBgLightMode       = -1;
  sleepLastPattern           = -1;
  gEyeSlotActive = false;
  eyeOffsetX = 0;
  eyeOffsetY = 0;

  // 連続確認カウンタをリセット
  sleepJoyConfirmStart = 0;
  sleepImuConfirmStart = 0;

  if (ENABLE_SLEEP_WAKE_SERVO) {
    lrMoveCaller = "wakeUp_center";
    moveToCenterWithEyeLead(servoLR, lrNow, HEAD_HORIZONTAL_CENTER, 25);
  }

  drawFace();
  showSensors();

  blinkEyes();

  if (ENABLE_SLEEP_WAKE_SERVO) {
    lookAround();

    // 背伸び
    lrMoveCaller = "wakeUp_stretch";
    moveWithEyeLead(servoLR, lrNow, HEAD_LEFT, 20,
                    EYE_SHIFT_PIXELS, 0, "LEFT");
    smartDelay(1000);

    lrMoveCaller = "wakeUp_center";
    moveToCenterWithEyeLead(servoLR, lrNow, HEAD_HORIZONTAL_CENTER, 20);
  }

  drawFace();
  showSensors();

  {
    char wbuf[128];
    snprintf(wbuf, sizeof(wbuf),
             "WAKE END reason=%s V=%d A=%.1f M=%d",
             wakeReason.c_str(), currentVolume, currentAccel, currentMotionLevel);
    addLog(wbuf);
  }

  // 起床直後に、睡眠中の経過時間でアイドル動作が即発動しないようにする
  // ジョイスティック・タッチ・IMU・音声などの反応は止めない
  lastIdleMoveTime = millis();
  nextIdleMoveInterval = random(50000, 120000);
  addLog("IDLE TIMER RESET AFTER WAKE");
}

void servoDemoMotion() {
  // 上下：servoUD（UD_SERVO_PIN=2）が物理的に上下を動かす。
  moveWithEyeLead(servoUD, udNow, HEAD_UP, 15,
                  0, -EYE_SHIFT_PIXELS, "UP");
  smartDelay(300);  // 旧:delay(300)

  moveWithEyeLead(servoUD, udNow, HEAD_DOWN, 15,
                  0, EYE_SHIFT_PIXELS, "DOWN");
  smartDelay(300);  // 旧:delay(300)

  moveToCenterWithEyeLead(servoUD, udNow, HEAD_VERTICAL_CENTER, 15);
  smartDelay(300);  // 旧:delay(300)

  // 左右：servoLR（LR_SERVO_PIN=1）が物理的に左右を動かす。
  lrMoveCaller = "demo";
  moveWithEyeLead(servoLR, lrNow, HEAD_LEFT, 20,
                  EYE_SHIFT_PIXELS, 0, "LEFT");
  smartDelay(500);

  lrMoveCaller = "demo";
  moveWithEyeLead(servoLR, lrNow, HEAD_RIGHT, 20,
                  -EYE_SHIFT_PIXELS, 0, "RIGHT");
  smartDelay(500);

  // 正面へ戻す
  lrMoveCaller = "demo_center";
  moveToCenterWithEyeLead(servoLR, lrNow, HEAD_HORIZONTAL_CENTER, 20);
  moveToCenterWithEyeLead(servoUD, udNow, HEAD_VERTICAL_CENTER, 15);
}


// 手動スタンバイ（enterAwakeMode/enterStandbyMode, 6秒長押し切替）は
// 2026/07/20廃止。画面と各種アニメーションが完全静止し、フリーズと
// 誤認される原因になっていたため撤去した。
// 覚醒中のタッチ反応（なでなで喜び顔）のみ以下に残す。
// 自動スリープ（sleepMode）への移行・復帰（短押し/音/ジョイスティック/揺れ）は
// handleSleepTransition() / handleStandbyGate() 側で従来どおり処理する。
bool handleAwakeModeTouch() {
  bool touchedHead = isHeadTouched();

  if (touchedHead) {
    if (!faceTouching) {
      faceTouching = true;
    }

    // タッチ中は喜ぶ顔
    if (!sleepMode) {
      if (!petMode) {
        petMode = true;
        drawPetEyes();
      }
    }

    // 押している途中は下の通常処理へ流さない
    return true;
  }

  // 指を離した瞬間
  if (faceTouching) {
    faceTouching = false;

    // 喜び顔解除
    if (petMode) {
      petMode = false;
      drawOpenEyes();
    }

    if (!sleepMode) {
      alertMode = false;
      lastInteractionTime = millis();

      if (ENABLE_SLEEP_WAKE_SERVO) {
        servoDemoMotion();
      }

      drawFace();
      showSensors();

      return true;
    }
  }

  return false;
}

void playWakeTone() {
  if (!isSpeakerAllowed()) return;  // 内蔵マイクモード中はtoneも禁止

  CoreS3.Speaker.setVolume(1);
  delay(30);  // スピーカー音量設定の安定待ち（必須・短いため残す）

  CoreS3.Speaker.tone(600, 60);
}

void playSleepTone() {
  if (!isSpeakerAllowed()) return;  // 内蔵マイクモード中はtoneも禁止

  CoreS3.Speaker.setVolume(1);
  delay(30);  // スピーカー音量設定の安定待ち（必須・短いため残す）

  CoreS3.Speaker.tone(350, 120);
}

bool ssidFoundInScan(String targetSsid) {
  addLog("WiFi scan start");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  smartDelay(500);

  int n = WiFi.scanNetworks();

  addLog("SCAN FOUND: " + String(n));

  for (int i = 0; i < n; i++) {
    String foundSsid = WiFi.SSID(i);

    // SSID一覧はSerialのみに出力（addLogはwebLog文字列追加+SD書き込みチェックで重いため）
    Serial.println("[SCAN] " + String(i + 1) + ": " + foundSsid
                   + " RSSI=" + String(WiFi.RSSI(i))
                   + " ENC=" + String(WiFi.encryptionType(i)));

    if (foundSsid == targetSsid) {
      addLog("TARGET SSID FOUND: " + targetSsid);
      WiFi.scanDelete();
      return true;
    }
  }

  WiFi.scanDelete();

  addLog("TARGET SSID NOT FOUND: " + targetSsid);
  return false;
}

bool checkCameraMotion(bool doAction) {
  if (!CoreS3.Camera.get()) {
    return false;
  }

  uint16_t* pixels = (uint16_t*)CoreS3.Camera.fb->buf;

  int width = CoreS3.Display.width();
  int height = CoreS3.Display.height();

  // 睡眠中は追視しない。起床検知だけに集中する。
  if (!sleepMode) {
    updateCameraGaze(pixels, width, height);
  }

  long diffSum = 0;
  long leftMotion = 0;
  long rightMotion = 0;
  bool detected = false;

  int index = 0;

  for (int y = 20; y < height; y += height / 8) {
    for (int x = 20; x < width; x += width / 8) {

      int pos = y * width + x;
      uint16_t p = pixels[pos];

      int r = (p >> 11) & 0x1F;
      int g = (p >> 5) & 0x3F;
      int b = p & 0x1F;

      uint8_t current = (r * 3 + g * 6 + b * 1);

      if (hasPrevious) {
        int diff = abs((int)current - (int)previousPixels[index]);

        diffSum += diff;

        if (x < width / 2) {
          leftMotion += diff;
        } else {
          rightMotion += diff;
        }
      }

      previousPixels[index] = current;
      index++;
    }
  }

  if (hasPrevious) {
    int motionLevel = diffSum / 64;
    currentMotionLevel = motionLevel;

    if (sleepMode) {
      // 寝ている時も、2回連続で動きを検出した時だけ起床

      // デバッグ用
      if (motionLevel >= 60) {

        if (DEBUG_SLEEP) addLog("SLEEP MOTION HIGH M=" + String(motionLevel));

      } else if (millis() - lastSleepMotionLogTime > 30000) {

        lastSleepMotionLogTime = millis();

        if (currentMotionLevel >= 10) {
          if (DEBUG_SLEEP) addLog("SLEEP MOTION M=" + String(currentMotionLevel));
        }
      }

      if (motionLevel > 55) {
        cameraMotionCount++;
      } else {
        cameraMotionCount = 0;
      }

      if (cameraMotionCount >= 2) {
        detected = true;
        cameraMotionCount = 0;
        lastMotionTime = millis();

        if (sleepMode && doAction) {
          saveToiletSnapshot();
        }
      }

    } else {
      // 起きている時は従来どおり慎重に判定
      if (motionLevel > 50) {
        cameraMotionCount++;
      } else {
        cameraMotionCount = 0;
      }

      if (cameraMotionCount >= 2 && millis() - lastMotionTime > 3000) {
        cameraMotionCount = 0;
        lastMotionTime = millis();
        detected = true;

        if (doAction && !alertMode && !petMode && !sleepMode) {
          blinkCamera();

          alertMode = true;
          alertUntil = millis() + 700;
        }
      }
    }
  }

  hasPrevious = true;

  if (ENABLE_CAMERA_LOOK && cfg_enableCamera && doAction && !servoBusy && !petMode && !sleepMode) {
    lookAtMotionDirection(leftMotion, rightMotion);
  }

  if (!sleepMode && millis() - latestBmpTime > BMP_UPDATE_INTERVAL) {
    saveLatestBmp();
  }

  CoreS3.Camera.free();

  return detected;
}

bool checkImuMotion(bool doAction) {
  float ax, ay, az;

  CoreS3.Imu.getAccel(&ax, &ay, &az);

  float accel = sqrt(ax * ax + ay * ay + az * az);
  currentAccel = accel;

  // sleep中はhandleStandbyGate()がSLEEP_IMU_THRESHOLD(1.4f)で判定するため
  // ここでの閾値は覚醒中のみ有効。sleep中はdoAction=falseで呼ばれる。
  float imuWakeThreshold = 1.3f;  // 覚醒中の警戒閾値（変更なし）

  if (accel > imuWakeThreshold && millis() - lastImuAlertTime > 5000) {
    lastImuAlertTime = millis();

    if (doAction && !alertMode && !petMode && !sleepMode) {
      blinkImu();
      alertMode = true;
      alertUntil = millis() + 1000;
    }

    return true;
  }

  return false;
}

void mutter() {  // ⚠️ レガシー: 呼び出し箇所なし（WAV方式 playMutterOnce に置換済み）。削除候補。
  if (!isSpeakerAllowed()) return;  // 内蔵マイクモード中はtoneも禁止

  CoreS3.Speaker.setVolume(1);

  CoreS3.Speaker.tone(650, 25);
  delay(35);

  CoreS3.Speaker.tone(650, 25);
  delay(25);
}

// ====================================================
// tryShowMutterFace()
// /faces ディレクトリ内のPNGをランダムに1枚選んで drawFaceImage() で表示する。
// 成功時は mutterFaceActive=true を立てて true を返す。
// SDが無い・/facesが無い・PNGが1枚も無い場合は false を返す（演出なし）。
// ====================================================
bool tryShowMutterFace() {
  File dir = SD.open("/faces");
  if (!dir) return false;

  // まずPNGファイルを最大32枚リストアップ（Stringを使わずchar配列で）
  const int MAX_FACES = 32;
  char faceNames[MAX_FACES][64];
  int faceCount = 0;

  File entry = dir.openNextFile();
  while (entry && faceCount < MAX_FACES) {
    String name = String(entry.name());
    if (!entry.isDirectory() && !name.startsWith(".") && name.endsWith(".png")) {
      name.toCharArray(faceNames[faceCount], 64);
      faceCount++;
    }
    entry = dir.openNextFile();
  }
  dir.close();

  if (faceCount == 0) return false;

  // ランダムに1枚選択
  int idx = random(0, faceCount);
  char path[80];
  snprintf(path, sizeof(path), "/faces/%s", faceNames[idx]);

  addLog("MUTTER FACE: " + String(path));
  drawFaceImage(path);

  mutterFaceActive = true;
  return true;
}

// ====================================================
// showSleepFace()
// スリープ中にNTPが使えない場合の時計代替表示。
// /faces 内のPNGをランダムに1枚選んで drawFaceImage() で表示する。
//
// ・成功時: sleepFaceActive=true、lastSleepFaceRotateTime をリセットして true を返す
// ・SDなし・/facesなし・PNG0枚の場合: 何もせず false を返す（睡眠目のまま）
// ・drawFaceImage() が imageFaceMode=true にするため、
//   鼻ヒクヒク・口パクが handleNoseMotion() の既存ガードで自動停止する
// ====================================================
bool showSleepFace() {
  File dir = SD.open("/faces");
  if (!dir) return false;

  const int MAX_FACES = 32;
  char faceNames[MAX_FACES][64];
  int faceCount = 0;

  File entry = dir.openNextFile();
  while (entry && faceCount < MAX_FACES) {
    String name = String(entry.name());
    if (!entry.isDirectory() && !name.startsWith(".") && name.endsWith(".png")) {
      name.toCharArray(faceNames[faceCount], 64);
      faceCount++;
    }
    entry = dir.openNextFile();
  }
  dir.close();

  if (faceCount == 0) return false;

  int idx = random(0, faceCount);
  char path[80];
  snprintf(path, sizeof(path), "/faces/%s", faceNames[idx]);

  addLog("SLEEP FACE: " + String(path));
  drawFaceImage(path);

  sleepFaceActive = true;
  lastSleepFaceRotateTime = millis();
  return true;
}

// ====================================================
// mutter一覧キャッシュ（RAM）
// 起動時とmutterアップロード/削除時にのみSDを1回走査してファイル名を保持する。
// AUTO/JOYSTICK の再生はこのキャッシュから即座にランダム選択するため、
// 再生のたびにSDフォルダを全走査しない（探索タイムアウトを根本的に回避）。
// 1件あたり最大39文字（"mutter_xxx.wav" は十分収まる）× 最大400件 ≒ 15.6KB。
// ====================================================
const int  MUTTER_CACHE_MAX      = 400;
const int  MUTTER_CACHE_NAME_LEN = 40;
char       mutterCacheNames[MUTTER_CACHE_MAX][MUTTER_CACHE_NAME_LEN];
int        mutterCacheCount = 0;
bool       mutterCacheReady = false;

// キャッシュを再構築する。起動時とmutterファイル増減時に呼ぶ。
// ここでの走査は初期化フェーズ想定なのでタイムアウトは設けない。
void rebuildMutterCache() {
  mutterCacheCount = 0;
  mutterCacheReady = false;

  File dir = SD.open("/sounds/mutter");
  if (!dir) {
    addLog("MUTTER CACHE: dir open failed");
    return;
  }

  File entry = dir.openNextFile();
  while (entry && mutterCacheCount < MUTTER_CACHE_MAX) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.startsWith(".") && isWavFilename(name)) {
        strncpy(mutterCacheNames[mutterCacheCount], name.c_str(),
                MUTTER_CACHE_NAME_LEN - 1);
        mutterCacheNames[mutterCacheCount][MUTTER_CACHE_NAME_LEN - 1] = '\0';
        mutterCacheCount++;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();

  mutterCacheReady = true;
  addLog("MUTTER CACHE: built count=" + String(mutterCacheCount));
}

// キャッシュからランダムに1件選んで outPath に書き込む。
// 戻り値：選べたら true。キャッシュ未構築 or 空なら false。
bool pickMutterFromCache(char* outPath, size_t outSize) {
  if (!mutterCacheReady || mutterCacheCount <= 0) return false;
  int idx = random(0, mutterCacheCount);
  snprintf(outPath, outSize, "/sounds/mutter/%s", mutterCacheNames[idx]);
  return true;
}

// ====================================================
// pickMutterFromFolder()
// /sounds/mutter/ 内の .wav からランダムに1つ選ぶ。
// 選べた場合は outPath にフルパスを書き込み true を返す。
// フォルダが無い / .wav が1枚も無い場合は false を返す（呼び出し側で旧方式へフォールバック）。
//
// メモリ方針：全ファイル名を配列に保持しない。
//   1回目の走査で .wav 件数を数える → random で対象番号を決める
//   → 2回目の走査で該当番号のWAVだけを取り出す
// これによりファイル数が何百件に増えてもRAM消費は一定（ファイル名1つ分のみ）。
// ※通常はキャッシュ（pickMutterFromCache）が使われ、この関数は
//   キャッシュ未構築時のフォールバック探索としてのみ呼ばれる。
// ====================================================
// mutter探索のタイムアウト（ms）。これを超えたら探索を打ち切り false を返す。
// 【調整指針】ログの "MUTTER SEARCH: candidates=N (pass1 ms=...)" と
//   "MUTTER SEARCH DONE ... total_ms=..." を見て決める。
//   候補300件以上で毎回 TIMEOUT が出る場合は 4000〜5000 へ上げる。
//   推奨レンジ：3000〜5000ms。5000を超える値は操作不能感が出るため非推奨。
const unsigned long MUTTER_SEARCH_TIMEOUT_MS = 3000;

// 探索ループ中に「副作用のない」処理だけを回す軽量ポンプ。
// handleJoystick() はサーボ駆動・wakeUp()・servoBusy 操作など副作用が大きいため
// あえて含めない（探索中のサーボ競合・顔状態変更・別動作割り込みを防ぐ）。
// ジョイスティック/タッチはタイムアウト内に loop() へ戻ってから通常処理される。
// ここで維持するのは Web応答・UDP受信・外部音声との口パク同期のみ。
static inline void mutterSearchPump() {
  server.handleClient();
  handleUDP();
  updateExternalMouth();
}

MutterPickResult pickMutterFromFolder(char* outPath, size_t outSize) {
  unsigned long searchStart = millis();
  addLog("MUTTER SEARCH START: /sounds/mutter");

  // ---- 1回目：.wav 件数を数える ----
  File dir = SD.open("/sounds/mutter");
  if (!dir) {
    addLog("MUTTER SEARCH: dir open failed");
    return MUTTER_PICK_NO_FOLDER;
  }

  int count = 0;
  int scanned = 0;
  bool timedOut = false;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.startsWith(".") && isWavFilename(name)) count++;
    }
    scanned++;
    // 10件ごとに他処理を回し、タイムアウトを確認
    if ((scanned % 10) == 0) {
      mutterSearchPump();
      if (millis() - searchStart > MUTTER_SEARCH_TIMEOUT_MS) {
        timedOut = true;
        break;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();

  if (timedOut) {
    addLog("MUTTER SEARCH TIMEOUT (pass1) ms=" + String(millis() - searchStart)
         + " scanned=" + String(scanned));
    return MUTTER_PICK_TIMEOUT;
  }

  addLog("MUTTER SEARCH: candidates=" + String(count)
       + " (pass1 ms=" + String(millis() - searchStart) + ")");

  if (count == 0) return MUTTER_PICK_EMPTY;

  // ---- 対象番号を決める ----
  int target = random(0, count);  // 0 .. count-1

  // ---- 2回目：target 番目の .wav だけを取り出す ----
  dir = SD.open("/sounds/mutter");
  if (!dir) {
    addLog("MUTTER SEARCH: dir reopen failed");
    return MUTTER_PICK_NO_FOLDER;
  }

  int idx = 0;
  int scanned2 = 0;
  bool found = false;
  entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.startsWith(".") && isWavFilename(name)) {
        if (idx == target) {
          snprintf(outPath, outSize, "/sounds/mutter/%s", name.c_str());
          found = true;
          break;
        }
        idx++;
      }
    }
    scanned2++;
    if ((scanned2 % 10) == 0) {
      mutterSearchPump();
      if (millis() - searchStart > MUTTER_SEARCH_TIMEOUT_MS) {
        timedOut = true;
        break;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();

  if (timedOut) {
    addLog("MUTTER SEARCH TIMEOUT (pass2) ms=" + String(millis() - searchStart));
    return MUTTER_PICK_TIMEOUT;
  }

  if (found) {
    addLog("MUTTER SEARCH DONE: target=" + String(target)
         + "/" + String(count)
         + " total_ms=" + String(millis() - searchStart));
    return MUTTER_PICK_FOUND;
  }

  // count>0 だったのに取り出せなかった（走査中にファイルが消えた等の稀ケース）。
  // 存在保証がないため旧方式へは流さず EMPTY 扱いにする。
  addLog("MUTTER SEARCH: target not found total_ms="
       + String(millis() - searchStart));
  return MUTTER_PICK_EMPTY;
}

// 再生対象の mutter wav を決定して outPath に書き込む。
// 「顔変更より前に」呼ぶことで、探索失敗/時間超過時に顔を変えずに済む。
// 戻り値：再生してよいパスが決まれば true、中止すべきなら false。
//
// フォールバック方針（mutterファイルは /sounds/mutter/ へ移動済み）：
//   FOUND     → そのパスを再生
//   NO_FOLDER → フォルダ自体が無い環境向けに旧番号方式へフォールバック
//   EMPTY     → フォルダはあるが空 → 旧番号方式へフォールバック
//   TIMEOUT   → 中身がある可能性が高いのに走査しきれなかっただけ。
//               存在しない旧形式ファイルを鳴らさないため、フォールバックせず中止。
bool resolveMutterPath(char* outPath, size_t outSize) {
  if (soundBusy || externalSpeaking) return false;

  // ---- 最優先：RAMキャッシュから即選択（SD走査なし・タイムアウトなし）----
  // 起動時 rebuildMutterCache() 済みなら、ここでほぼ確実に決まる。
  if (pickMutterFromCache(outPath, outSize)) {
    addLog("MUTTER SELECT: " + String(outPath));
    return true;
  }

  // ---- キャッシュ未構築/空の場合のみ、従来のフォルダ走査へ ----
  MutterPickResult r = pickMutterFromFolder(outPath, outSize);

  if (r == MUTTER_PICK_FOUND) {
    addLog("MUTTER SELECT: " + String(outPath));
    return true;
  }

  if (r == MUTTER_PICK_TIMEOUT) {
    // 走査はタイムアウトしたが、キャッシュが後から使えるならそこから選ぶ。
    // （通常はキャッシュ優先で先に返るためここには来ないが、保険）
    if (pickMutterFromCache(outPath, outSize)) {
      addLog("MUTTER SELECT (cache after timeout): " + String(outPath));
      return true;
    }
    // キャッシュも無ければ旧方式へは流さず中止。
    addLog("MUTTER RESOLVE: timeout -> abort (no legacy fallback)");
    return false;
  }

  // r == NO_FOLDER / EMPTY のみ旧番号方式へフォールバック
  int n = random(1, 319);  // mutter_001.wav ～ mutter_318.wav（旧方式・移行用）
  snprintf(outPath, outSize, "/sounds/mutter_%03d.wav", n);
  addLog("MUTTER(legacy): " + String(outPath));
  return true;
}

// 指定パスの mutter wav を再生し、変顔中なら通常顔へ戻す。
void playMutterPath(const char* path) {
  // mutterのWAV再生そのものはユーザーのInteractionではないため、
  // Sleepタイマー(lastInteractionTime)を更新させない。
  // yawn()と同じsave→再生→restoreパターンを踏襲し、共通処理のplayWavFromSD()
  // 自体（lastInteractionTime更新を含む）は無改造のまま利用する。
  // 手動mutter（ジョイスティック押し込み等）による正当なInteraction更新は、
  // 呼び出し元（例：ジョイスティック押し込み処理）が別途行っているため維持される。
  unsigned long savedInteractionTime = lastInteractionTime;
  playWavFromSD(path);
  lastInteractionTime = savedInteractionTime;

  // 変顔演出中だった場合、WAV終了後に通常顔へ戻す
  if (mutterFaceActive) {
    mutterFaceActive = false;
    addLog("MUTTER FACE: restore normal");
    drawFace();
    showEvent("N");
  }
}



// ===== End SD Web Config Helpers =====

// ===== Behavior Helpers =====

void handlePetTouch(bool touchedHead) {
  if (touchedHead) {
    lastInteractionTime = millis();

    if (!imageFaceMode && !petMode) {
      petMode = true;
      drawPetEyes();
    }
  } else {
    if (petMode) {
      petMode = false;
      drawOpenEyes();
    }
  }
}

// ====================================================
// マイクと音声入力フレームワークの関係
// CoreS3.Mic.record() と Speaker.playWav() の同時使用は音割れする
// （2026/06/07確認済み。soundBusyフラグでも改善しなかった）。
// このため常時マイク（updateVolume）は引き続き無効のままとし、
// マイクは「内蔵マイクモード（AUDIO_SRC_MIC）」専用とする。
// 同モード中は setAudioSource() が Speaker.end()→Mic.begin() を行い、
// スピーカー再生を全面停止することでI2S競合を根本回避している。
// updateVolume()/updateSoundStimulus() は従来どおり0固定（動作無変更）。
// ====================================================

void handleSoundAlert(bool soundStimulus) {

  if (soundStimulus && !alertMode && !petMode) {

    lastInteractionTime = millis();

    blinkSound();

    alertMode = true;
    alertUntil = millis() + 1000;
  }
}

void handleSoundActivity(int volume) {

  // 周囲が騒がしい時はスリープタイマーを延長
  if (!sleepMode && volume > 500) {
    lastInteractionTime = millis();
  }
}

void handleNoseMotion() {
  // sleepMode チェックを外し、覚醒中・睡眠中問わず鼻ヒクヒクを継続する。
  // imageFaceMode（画像表示中）・alertMode・petMode 中は止める。
  if (!imageFaceMode && !alertMode && !petMode && millis() - lastNoseMove > nextNoseInterval) {

    lastNoseMove = millis();

    toggle = !toggle;

    updateNose(toggle ? -1 : 1);

    nextNoseInterval = random(120, 280);
  }
}

void handleBlinkMotion() {
  if (!imageFaceMode && !alertMode && !petMode && millis() - lastBlinkCheck > nextBlinkInterval) {

    lastBlinkCheck = millis();

    if (random(0, 100) < 45) {  // 瞬き発生率 45%
      blinkEyes();

      if (random(0, 100) < 8) {  // 次の瞬きまで 3～10秒
        smartDelay(180);  // 旧:delay(180) → 二重まばたき間隔
        showEvent("N");
        blinkEyes();
      }
    }

    nextBlinkInterval = random(3000, 10000);
  }
}

void handleAlertRelease() {
  if (!imageFaceMode && alertMode && (long)(millis() - alertUntil) >= 0) {

    alertMode = false;
    drawOpenEyes();
  }
}

// ====================================================
// handleBatteryLow()
// バッテリー残量が BATTERY_LOW_THRESHOLD(30%) 未満になったら
// /sounds/hungry.wav を再生して充電を促す。
// mutter と同じ優先順位（canDoIdleAction() で管理）。
// 30分クールダウンで連続再生を防止。
// ====================================================
void handleBatteryLow() {
  if (sleepMode) return;
  if (soundBusy) return;
  if (externalSpeaking) return;
  if (!canDoIdleAction()) return;
  if (millis() - lastHungryPlayedTime < BATTERY_LOW_COOLDOWN) return;

  // 外部給電中（USB-C / DIN BASE DC INPUT）は「充電してください」警告を行わない。
  // DIN BASE の POWER OFF 時（bat=0 誤検出）の hungry.wav も抑止する。
  // BROWNOUT WARNING ログより前に置くのは、DIN BASE OFF時に bat=0 →
  // 毎ループ WARNING が出て SDログをスパムするのを防ぐため
  // （外部給電中はそもそもBROWNOUTしない）。
  if (isExternalPowered()) return;

  int bat = CoreS3.Power.getBatteryLevel();

  // 5%以下はBROWNOUT直前の可能性 → 即ログ（SDフラッシュキーワードに該当）
  if (bat <= 5) {
    addLog("BROWNOUT WARNING: battery=" + String(bat) + "% (critical)");
  }

  if (bat < BATTERY_LOW_THRESHOLD) {
    addLog("BATTERY LOW: " + String(bat) + "% → playing hungry.wav");
    lastHungryPlayedTime = millis();
    playWavFromSD("/sounds/hungry.wav");
  }
}

// 独り言を「今すぐ1回」再生する共通処理。
// 確率・クールダウン判定は含まない（呼び出し側の責任）。
// handleRandomMutter()（自発）と JOYSTICK MUTTER（押し込み）の両方から使う。
// 抑制条件（sleep/alert/pet/external/web/soundBusy）は呼び出し側で確認する。
// reason はログ用のトリガ名（"AUTO" / "JOYSTICK" など）。
void playMutterOnce(const char* reason) {
  // 内蔵マイクモード（AUDIO_SRC_MIC）中も独り言を再生する。
  // WAV再生の前後で pauseKaripomEarForMutter() / resumeKaripomEarAfterMutter()
  // によりマイク解析を一時停止・再開することでハウリングを防ぐ。
  // MIC以外のモードでは pause/resume は何もしないため動作に影響しない。

  unsigned long mcStart = millis();
  addLog("MUTTER CHECK START [" + String(reason) + "]");

  // ---- 手順1：先に再生対象のwavを決定する ----
  // 探索が失敗/タイムアウトした場合はここで打ち切り、顔は一切変更しない。
  // この時点ではまだ pause しない（マイク停止時間を最小限にするため）。
  char path[80];
  if (!resolveMutterPath(path, sizeof(path))) {
    addLog("MUTTER CHECK ABORT: no wav resolved ms="
         + String(millis() - mcStart));
    lastMutterPlayedTime = millis();  // 連続再試行を避ける
    return;  // pause前なので resumeKaripomEarAfterMutter() 不要
  }

  // ---- 手順2：wavが決まってからマイク解析を一時停止 ----
  pauseKaripomEarForMutter();

  // ---- 手順3：変顔演出（確率 cfg_mutterFaceChance %）----
  if (random(0, 100) < cfg_mutterFaceChance) {
    if (!tryShowMutterFace()) {
      showEvent("N");
    }
  } else {
    showEvent("N");
  }

  // ---- 手順4：WAV再生 ----
  // playMutterPath → playWavFromSD は完了・エラー・中断のいずれでもここへ戻るため resume漏れなし。
  playMutterPath(path);
  lastMutterPlayedTime = millis();

  // ---- 手順5：マイク解析を再開 ----
  resumeKaripomEarAfterMutter();

  addLog("MUTTER CHECK END [" + String(reason) + "] ms=" + String(millis() - mcStart));
}

void handleRandomMutter() {
  if (!soundBusy && canDoIdleAction() && millis() - lastMutterCheck > 1000 && millis() - lastMutterPlayedTime > MUTTER_COOLDOWN) {

    lastMutterCheck = millis();

    // 独り言発生確率（1秒ごとに判定）
    // cfg_mutterChance (0/5/10/20/30) → 閾値 = value/4 per-1000
    //   10(標準) → 2/1000 = 0.2%/s ≈ 平均8分（旧ハードコード値と同等）
    int mutterThreshold = cfg_mutterChance / 4;
    if (mutterThreshold > 0 && random(0, 1000) < mutterThreshold) {
      playMutterOnce("AUTO");
    }
  }
}

bool updateCameraStimulus() {

  // カメラが無効な場合は処理をスキップ（CoreS3.Camera.get()のブロッキングを防ぐ）
  if (!cfg_enableCamera) return false;

  bool cameraStimulus = false;

  static unsigned long lastAwakeCameraCheck = 0;

  if (sleepMode) {

    if (millis() - sleepStartTime > 3000 && millis() - lastSleepCameraCheck > 1000) {

      lastSleepCameraCheck = millis();
      cameraStimulus = checkCameraMotion(false);
    } else {
      cameraStimulus = false;
    }

  } else {

    if (millis() - lastAwakeCameraCheck > 500) {
      lastAwakeCameraCheck = millis();
      cameraStimulus = checkCameraMotion(true);
    } else {
      cameraStimulus = false;
    }
  }

  return cameraStimulus;
}

bool updateImuStimulus() {
  // 睡眠中は感度を下げ、覚醒中は通常判定
  return checkImuMotion(!sleepMode);
}

bool updateSoundStimulus(int volume) {
  // TODO [将来対応]: CoreS3マイクを復活させる時は、WAV再生時の音割れ/I2S競合を再確認する。
  // 現在はマイク無効のため volume は常に 0 で、この関数は常に false を返す。
  return volume > 1200;
}

int updateVolume() {
  // 現在はマイク入力停止中のため 0 固定
  int volume = 0;

  currentVolume = volume;

  return volume;
}

bool handleSleepMode(bool touchedHead,
                     bool cameraStimulus,
                     bool imuStimulus,
                     bool soundStimulus) {

  if (!sleepMode) return false;

  // ── 眺めて楽しいSleep表示（Sleep Lighting Carousel）──
  // 入眠直後 SLEEP_FACE_DURATION（3分）は、既存どおり静止した閉じ目のまま何もしない
  // （drawSleepScreen()による入眠演出はここでは一切変更しない）。
  // 3分経過後に一度だけカルーセルを開始し、以後は SLEEP_FACE_ROTATE_MS（3分）ごとに
  // 「Face Gallery」か「目×Lighting」かをランダムに切り替え続ける。
  if (!sleepCarouselStarted && millis() - sleepStartTime > SLEEP_FACE_DURATION) {
    sleepCarouselStarted = true;
    sleepCarouselNextSwitchMs = 0;   // 次回呼び出しで即座に最初のパターンを選ばせる
    addLog("SLEEP CAROUSEL START");
  }

  if (sleepCarouselStarted) {
    updateSleepLightingCarousel();
    yield();  // WDT対策：長時間スリープ中もタスクスケジューラに制御を返す
  }

  bool wakeReady = (long)(millis() - alertUntil) >= 0;

  // IMU wakeは handleStandbyGate() に一本化。
  // ここでは touch / camera / sound のみ処理する。
  // imuStimulus は checkImuMotion() の旧閾値(1.2f)を使うため除外。
  if (wakeReady && (touchedHead || cameraStimulus || soundStimulus)) {

    String wakeReason = "UNKNOWN";

    if (touchedHead) {
      wakeReason = "TOUCH";
    } else if (cameraStimulus) {
      wakeReason = "MOTION";
    } else if (soundStimulus) {
      wakeReason = "SOUND";
    }

    {
      char wbuf[160];
      snprintf(wbuf, sizeof(wbuf),
               "WAKE TRIGGER reason=%s H=%d C=%d S=%d V=%d A=%.1f M=%d",
               wakeReason.c_str(),
               (int)touchedHead, (int)cameraStimulus, (int)soundStimulus,
               currentVolume, currentAccel, currentMotionLevel);
      addLog(wbuf);
    }

    if (wakeReason == "MOTION") {
      saveToiletSnapshot();
    }

    wakeUp(wakeReason);
    return true;
  }

  if (DEBUG_ENV && millis() - lastEnvReportTime > 30000) {
    lastEnvReportTime = millis();
    addLog("ENV V=" + String(currentVolume)
         + " A=" + String(currentAccel, 2)
         + " M=" + String(currentMotionLevel)
         + " BAT=" + String(CoreS3.Power.getBatteryLevel())
         + " IMU_TH=" + String(sleepMode ? SLEEP_IMU_THRESHOLD : 1.3f, 1));
  }

  // 2026/07/15: Face Gallery画像表示中(sleepFaceActive=true)はZZZを重ねない。
  // 画像は必ずしも寝顔ではないため。通常の寝顔（drawSleepScreen/drawClock）表示中は
  // sleepFaceActive=falseのため従来通り毎ループ描画され、アニメーションは維持される。
  // Sleep Lighting Carousel（Face Gallery／目×Lighting）表示中はZzzを出さない仕様
  // （2026-08-04実機確認：Zzzは入眠直後3分間の静止閉じ目でのみ表示する）。
  if (!sleepFaceActive && !sleepLightingComposeActive) {
    updateZzz();
  }

  // 鼻ヒクヒクは handleNoseMotion() が sleepMode 中も担当するためここでは不要。

  return true;
}

void handleSpeakRequest() {
  if (speakRequested) {
    speakRequested = false;

    addLog("SPEAK PLAY: " + String(speakPath));

    playWavFromSD(speakPath);
  }
}

void handleSensorDisplay() {
  if (sleepMode) return;

  if (millis() - lastVolumeDisplayTime > 300) {
    lastVolumeDisplayTime = millis();
    showSensors();
  }
}

// たまに首を動かす（v38: 控えめ版）
// 方針：必ず「目線先行 → 首移動 → 目線中央」。長い複合動作は避ける。
void handleIdleServoMotion() {

  if (servoBusy) return;
  if (!ENABLE_IDLE_SERVO) return;
  if (cfg_idleChance == 0) return;  // WebUI で OFF に設定されている場合
  if (!canDoIdleAction()) return;

  static unsigned long lastIdleDebug = 0;

  if (DEBUG_IDLE && millis() - lastIdleDebug > 5000) {
    addLog(
      "IDLE CHECK elapsed=" + String(millis() - lastIdleMoveTime) + " interval=" + String(nextIdleMoveInterval));
    lastIdleDebug = millis();
  }

  if (millis() - lastIdleMoveTime > nextIdleMoveInterval) {

    servoBusy = true;

    addLog("IDLE SERVO START");

    // action=0: 左右（50%）、action=1: 下（25%）、action=2: 上（25%）
    int actionRaw = random(0, 4);
    int action = (actionRaw <= 1) ? 0 : (actionRaw == 2 ? 1 : 2);

    addLog("IDLE STEP1: action=" + String(action)
         + " ud=" + String(udNow) + " lr=" + String(lrNow));

    if (action == 0) {
      if (IDLE_LR_DISABLED) {
        // ── IDLE左右動作 比較試験：スキップ ──────────────────────────────
        // IDLE_LR_DISABLED=true の間、左右サーボを動かさずこのターンをスキップ。
        // servoBusy解除・lastIdleMoveTime更新は共通テールで実行される。
        addLog("IDLE LR SKIPPED (IDLE_LR_DISABLED=true)");
        goto idle_common_tail;
      }

      // ── AUTO DETACH後の再attach初動対策 ────────────────────
      // detach中は保持トルクが無く、頭の自重で物理位置が lrNow（通常センター）
      // からズレる。その状態で moveSmooth() が再attach→最初のwriteをすると、
      // 物理位置から目標角へ一気に飛んで「ガタッ」と動く。
      // 左右idleの振り始め前に、現在角(lrNow)へ先に通電してなじませてから
      // 通常のスイープへ入る。attach済みの場合は何もしない（挙動不変）。
      if (ENABLE_LR_SERVO && !servoLR.attached()) {
        servoLR.attach(LR_SERVO_PIN);
        delay(LR_ATTACH_SETTLE_MS);  // 2026/07/07: 他のattach経路と手順統一（急動防止）

        // 2026/07/07: 最初のwrite＝PWM開始＝保持トルク開始点。
        // LR_ATTACH_SAG_OFFSET_DEG（デフォルト0）設定時のみ、たわみ推定位置へ
        // 先に通電し、なじませた後 1度25ms で lrNow へ小刻みに戻す。
        // オフセット0なら firstAngle==lrNow でループは走らず、従来と同一挙動。
        int firstAngle = constrain(lrNow + LR_ATTACH_SAG_OFFSET_DEG,
                                   SERVO_LR_RIGHT, SERVO_LR_LEFT);
        servoLR.write(firstAngle);
        {
          const char* sagDir = (LR_ATTACH_SAG_OFFSET_DEG > 0) ? "L"
                             : (LR_ATTACH_SAG_OFFSET_DEG < 0) ? "R" : "-";
          addLog("IDLE LR PRE-ATTACH: attachAngle=" + String(firstAngle)
                 + " lrNow=" + String(lrNow)
                 + " SAG=" + String(LR_ATTACH_SAG_OFFSET_DEG) + " dir=" + String(sagDir));
        }
        smartDelay(250);  // なじませ待ち（この間もWeb/UDP/口パクは動く）

        while (firstAngle != lrNow) {
          firstAngle += (lrNow > firstAngle) ? 1 : -1;
          servoLR.write(firstAngle);
          smartDelay(50);  // Yaw標準速度（LR_SLOWDOWN_FACTOR=2相当）に合わせた1度あたり待ち
        }

        lrMarkMoved();    // detachタイマー整合（直後のmoveSmoothでも更新される）
      }

      // LRサーボ負荷対策: swing幅は IDLE_LR_SWING_MIN〜MAX（定数定義側に経緯）。
      // 旧: 最大50°→30°（2026/07/07）→ 最大15°（2026/07/09 ギュイン音対策）。
      int swing = random(IDLE_LR_SWING_MIN, IDLE_LR_SWING_MAX + 1);
      int holdMs = random(800, 1201);

      if (random(0, 2) == 0) {

        int targetLeft = constrain(
          HEAD_HORIZONTAL_CENTER + swing, SERVO_LR_RIGHT, SERVO_LR_LEFT);

        addLog("IDLE STEP2: LEFT swing=" + String(swing)
             + " target=" + String(targetLeft)
             + " hold=" + String(holdMs));

        addLog("IDLE STEP3: moveWithEyeLead LEFT start");
        lrMoveCaller = "idle";
        moveWithEyeLead(servoLR, lrNow, targetLeft, 15,  // のっそりモード（旧: 10）
                        EYE_SHIFT_PIXELS, 0, "LEFT");
        addLog("IDLE STEP4: moveWithEyeLead LEFT done");

        addLog("IDLE STEP5: hold start " + String(holdMs) + "ms");
        smartDelay(holdMs);  // 旧:delay(holdMs) → WDT対策でsmartDelayに変更
        addLog("IDLE STEP6: hold done");

      } else {

        int targetRight = constrain(
          HEAD_HORIZONTAL_CENTER - swing, SERVO_LR_RIGHT, SERVO_LR_LEFT);

        addLog("IDLE STEP2: RIGHT swing=" + String(swing)
             + " target=" + String(targetRight)
             + " hold=" + String(holdMs));

        addLog("IDLE STEP3: moveWithEyeLead RIGHT start");
        lrMoveCaller = "idle";
        moveWithEyeLead(servoLR, lrNow, targetRight, 15,  // のっそりモード（旧: 10）
                        -EYE_SHIFT_PIXELS, 0, "RIGHT");
        addLog("IDLE STEP4: moveWithEyeLead RIGHT done");

        addLog("IDLE STEP5: hold start " + String(holdMs) + "ms");
        smartDelay(holdMs);  // 旧:delay(holdMs) → WDT対策でsmartDelayに変更
        addLog("IDLE STEP6: hold done");
      }

      addLog("IDLE STEP7: center H start");
      // 2026/07/07: LRセンター復帰の悲鳴（ギャワッ）対策として 10→15ms へ。
      // のっそりモード: 15→20ms（イージング込みで約23〜50ms/度）。
      lrMoveCaller = "idle_center";
      moveToCenterWithEyeLead(servoLR, lrNow, HEAD_HORIZONTAL_CENTER, 20);  // のっそりモード（旧: 15）
      addLog("IDLE STEP8: center H done");

    } else if (action == 1) {

      addLog("IDLE STEP2: DOWN target=" + String(SERVO_UD_IDLE_DOWN));
      addLog("IDLE STEP3: moveWithEyeLead DOWN start");
      moveWithEyeLead(servoUD, udNow, SERVO_UD_IDLE_DOWN, 18,  // のっそりモード（旧: 12）
                      0, EYE_SHIFT_PIXELS, "DOWN");
      addLog("IDLE STEP4: moveWithEyeLead DOWN done");

      addLog("IDLE STEP5: hold 500ms");  // のっそりモード（旧: 150ms）
      smartDelay(500);
      addLog("IDLE STEP6: hold done");

      addLog("IDLE STEP7: center V start");
      moveToCenterWithEyeLead(servoUD, udNow, HEAD_VERTICAL_CENTER, 18);  // のっそりモード（旧: 12）
      addLog("IDLE STEP8: center V done");

    } else {

      addLog("IDLE STEP2: UP target=" + String(SERVO_UD_IDLE_UP));
      addLog("IDLE STEP3: moveWithEyeLead UP start");
      moveWithEyeLead(servoUD, udNow, SERVO_UD_IDLE_UP, 18,  // のっそりモード（旧: 12）
                      0, -EYE_SHIFT_PIXELS, "UP");
      addLog("IDLE STEP4: moveWithEyeLead UP done");

      addLog("IDLE STEP5: hold 500ms");  // のっそりモード（旧: 150ms）
      smartDelay(500);
      addLog("IDLE STEP6: hold done");

      addLog("IDLE STEP7: center V start");
      moveToCenterWithEyeLead(servoUD, udNow, HEAD_VERTICAL_CENTER, 18);  // のっそりモード（旧: 12）
      addLog("IDLE STEP8: center V done");
    }

    idle_common_tail:
    servoBusy = false;

    lastIdleMoveTime = millis();
    // IDLE動作頻度に応じて次回インターバルを決定
    switch (cfg_idleChance) {
      case 1:  nextIdleMoveInterval = random(120000, 300000); break;  // 少なめ: 2〜5分
      case 3:  nextIdleMoveInterval = random(15000,  45000);  break;  // 多め:   15〜45秒
      default: nextIdleMoveInterval = random(45000,  120000); break;  // 標準(現行): 45秒〜2分
    }

    addLog("IDLE SERVO END ud=" + String(udNow) + " lr=" + String(lrNow));

    // サーボ動作音・筐体振動によるMIC SPEAK START誤発火を抑制
    suppressMicStart(3000, "servo");

    yield();  // WDT対策：IDLE SERVO END直後に制御を返す
  }
}

// ====================================================
// ====================================================
// ====================================================
// handleJoystick()  ── LPF安定化版 (v50_6)
//
// ■ v50_5からの変更
//   生の analogRead() 値をそのまま閾値判定していたため、
//   ADCノイズ（ピンク個体では±30以上）が閾値を跨ぎ続けて震えた。
//   閾値を650まで上げても止まらなかった原因はこれ。
//
//   v50_6では最初にローパスフィルター（指数移動平均）をかけ、
//   平滑化後の値でデッドゾーン・ヒステリシスを判定する。
//   ノイズは LPF で除去するため、閾値は現実的な値に戻した。
//
// ■ LPF式
//   filtered = filtered * (1 - alpha) + raw * alpha
//   alpha=0.15: ノイズ除去と操作応答のバランス点
//   （小さくするほど滑らか・応答遅、大きくするほど生値に近い）
// ====================================================
void handleJoystick() {

  // ── joystickEnabled=false の間は再キャリブ判定のみ行う ──────────
  // 旧: joyCenterX/Y への近さで復帰判定 → 未接続起動時は仮値で当てにならない
  // 新: 生値自体の「安定性（前後サンプル差分）」を追跡し、安定したら
  //     その値を新しい中心として再キャリブしてから復帰する。
  //     joyCenterX/Y への依存を排除しているため、起動時に未接続でも
  //     あとから接続した時点で自然に ON へ復帰できる。
  if (!joystickEnabled) {
    int rawX = analogRead(JOY_X_PIN);
    int rawY = analogRead(JOY_Y_PIN);

    // 復帰ウィンドウの初期化
    if (joyStableWindowStart == 0) {
      joyStableWindowStart = millis();
      joyPrevRawX          = -1;
      joyPrevRawY          = -1;
    }

    // 安定性判定: 前回サンプルとの差分が JOY_STABLE_TH 以内なら安定とみなす
    // joyCenterX/Y ではなく「値が動いていないか」で判定する
    bool isStable = (joyPrevRawX >= 0
                  && abs(rawX - joyPrevRawX) <= JOY_STABLE_TH
                  && abs(rawY - joyPrevRawY) <= JOY_STABLE_TH);
    if (isStable) {
      joyStableCount++;
    } else {
      joyStableCount = 0;
    }
    joyPrevRawX = rawX;
    joyPrevRawY = rawY;

    // ウィンドウ満了時に判定
    if (millis() - joyStableWindowStart > JOY_STABLE_WINDOW_MS) {
      if (joyStableCount >= JOY_STABLE_COUNT) {
        // 安定確認 → 32サンプルで平均・偏差を計算して再キャリブ
        long sumX = 0, sumY = 0, devSumX = 0, devSumY = 0;
        const int RC_SAMPLES = 32;
        for (int i = 0; i < RC_SAMPLES; i++) {
          sumX += analogRead(JOY_X_PIN);
          sumY += analogRead(JOY_Y_PIN);
          delay(2);
        }
        int rcCenterX = (int)(sumX / RC_SAMPLES);
        int rcCenterY = (int)(sumY / RC_SAMPLES);
        for (int i = 0; i < RC_SAMPLES; i++) {
          devSumX += abs(analogRead(JOY_X_PIN) - rcCenterX);
          devSumY += abs(analogRead(JOY_Y_PIN) - rcCenterY);
          delay(2);
        }
        int rcDevX = (int)(devSumX / RC_SAMPLES);
        int rcDevY = (int)(devSumY / RC_SAMPLES);

        // condA/B/C を再チェックして復帰 or ウィンドウリセット
        recalibrateJoystickRuntime(rcCenterX, rcCenterY, rcDevX, rcDevY);
      } else {
        // 安定しなかった → ウィンドウをリセットして再試行
        joyStableCount       = 0;
        joyStableWindowStart = millis();
        joyPrevRawX          = -1;
        joyPrevRawY          = -1;
      }
    }
    return;
  }

  // ── ウォームアップ中は入力を無視して LPF を中心値へ収束させる ──
  // recalibrateJoystickRuntime() が joyWarmupUntil を設定する。
  // 期間中は joyFilteredX/Y を joyCenterX/Y へ引き戻し続け、
  // ウォームアップ終了後に自然な値から入力判定が始まるようにする。
  if (joyWarmupUntil > 0 && millis() < joyWarmupUntil) {
    // LPF を中心値方向へゆっくり引き戻す（ノイズ混入防止）
    int rawXw = analogRead(JOY_X_PIN);
    int rawYw = analogRead(JOY_Y_PIN);
    joyFilteredX = joyFilteredX * (1.0f - JOY_LPF_ALPHA) + (float)rawXw * JOY_LPF_ALPHA;
    joyFilteredY = joyFilteredY * (1.0f - JOY_LPF_ALPHA) + (float)rawYw * JOY_LPF_ALPHA;
    joyPrevRawX = rawXw;
    joyPrevRawY = rawYw;
    return;  // 入力判定・サーボ制御は一切行わない
  }

  // ── サーボ動作中・直後のジョイスティック入力停止（2026/07/09 幻入力対策）──
  // 【現象】APモードでWebのUPを押した直後、未接触のジョイスティックが
  //   absX=251/absY=248（両軸同時・同方向・ほぼ同量）の偏差を検出し、
  //   LRが再attachされ90→93へ動いた（「右上」に見える複合動作の正体）。
  //   ログ: WEB SERVO CMD UP → LR RE-ATTACH (joystick) → JOY X/Y SERVO
  // 【原因・最有力仮説】サーボ電流＋Wi-Fi TXバーストによる電源レール低下
  //   （または共通GND電位差/EMI）。両軸が同時に同方向へ同量ずれるのは
  //   供給・GND・ADC基準の共通シフトの証拠で、機械的な実入力では起きない。
  // 【対策・二層構成（2026/07/09 改修2）】
  //   (a) 主対策: moveSmooth() 終了時に joyServoResumeAt=millis()+600ms をセット。
  //       Webハンドラの moveSmooth は同期実行中に handleJoystick() が呼ばれない
  //       ため、下記(b)の servoBusy 検出だけでは移動終了直後を抑制できない
  //       （APモード再現テストで実証済み）。
  //   (b) 本ブロック: smartDelay 経由で servoBusy 中に呼ばれた場合の窓延長と、
  //       抑制窓内の入力判定停止・LPF（joyFilteredX/Y）中心固定を担当する。
  //   STA/AP共通で常時有効（レール低下はSTAでも電源条件次第で発生するため、
  //   モード分岐にすると環境依存の挙動差が生まれ調査困難になる）。
  //   ジョイスティック操作自体は servoBusy を立てないため自己ブロックしない。
  //   なおWAV再生中（soundBusyのみ）は従来どおり入力を受け付ける（挙動不変）。
  if (servoBusy) {
    joyServoResumeAt = millis() + JOY_SERVO_RESUME_DELAY_MS;
    lastServoMoveEndAt = millis();  // smartDelay経由の動作中も浮き判定グレースを延長
    joyFilteredX = (float)joyCenterX;
    joyFilteredY = (float)joyCenterY;
    return;
  }
  if (joyServoResumeAt != 0) {
    if (millis() < joyServoResumeAt) {
      // 解除直後の残留期間（保持電流の安定・TXバースト終息を待つ）
      // ── 窓の実動作の可視化（2026/07/09 改修3）──
      // 窓内にservo ON閾値超えの偏差が来ていた場合のみログを出す。
      // 「JOY WINDOW SUPPRESS」が出る＝窓が幻入力を実際に止めた証拠。
      // 出ないのに幻入力が起きる場合は窓の外（同相検出3-Bの担当）。
      {
        int rxw = analogRead(JOY_X_PIN) - joyCenterX;
        int ryw = analogRead(JOY_Y_PIN) - joyCenterY;
        if (abs(rxw) > joyServoOnThX || abs(ryw) > joyServoOnThY) {
          static unsigned long lastWindowLog = 0;
          if (millis() - lastWindowLog > 500) {
            lastWindowLog = millis();
            addLog("JOY WINDOW SUPPRESS devX=" + String(rxw)
                 + " devY=" + String(ryw)
                 + " remain=" + String(joyServoResumeAt - millis()) + "ms");
          }
        }
      }
      joyFilteredX = (float)joyCenterX;
      joyFilteredY = (float)joyCenterY;
      return;
    }
    joyServoResumeAt = 0;
    if (DEBUG_JOYSTICK) addLog("JOY RESUME AFTER SERVO");
  }

  // ── 生値読み取り ──────────────────────────────────────
  int rawX = analogRead(JOY_X_PIN);
  int rawY = analogRead(JOY_Y_PIN);

  // ── ランタイム浮き検出（2026/07/07）──────────────────────
  // calibrateJoystick() で「接続済み」と判定されても、動作中に
  // 抜け・接触不良が起きると rawX/Y が前回値から大きくジャンプする。
  // 前回サンプルとの差分（絶対値）を「浮きサンプル」としてカウントし、
  // JOY_FLOAT_WINDOW_MS 内に JOY_FLOAT_MIN_COUNT 回超えたら無効化する。
  bool joyPushRaw = (rawX > 4000 && rawY > 4000);  // 押し込みは除外
  if (!joyPushRaw && joyPrevRawX >= 0) {
    int dx = abs(rawX - joyPrevRawX);
    int dy = abs(rawY - joyPrevRawY);
    if (dx > JOY_FLOAT_VARIANCE_TH || dy > JOY_FLOAT_VARIANCE_TH) {
      // ── サーボ動作後グレース（2026/07/09 改修4）────────────────
      // サーボ起因の電流ノイズ期間中は「未接続」判定に積まない。
      // スキップ（サーボwriteへ渡さない）のみ行い、カウンタは動かさない。
      // これで JOY DISABLED → CALIB → ENABLED の再キャリブ連発と、
      // レール電圧が沈んだ状態で中心値を掴む誤キャリブの両方を防ぐ。
      if (lastServoMoveEndAt != 0 &&
          millis() - lastServoMoveEndAt < JOY_FLOAT_GRACE_AFTER_SERVO_MS) {
        static unsigned long lastGraceSkipLog = 0;
        if (millis() - lastGraceSkipLog > 500) {
          lastGraceSkipLog = millis();
          addLog("JOY FLOAT SAMPLE SKIPPED (servo grace) dx=" + String(dx)
               + " dy=" + String(dy));
        }
        joyPrevRawX = rawX;
        joyPrevRawY = rawY;
        return;
      }

      // 浮きカウントウィンドウを初期化（初回 or ウィンドウ期限切れ）
      if (joyFloatWindowStart == 0
          || millis() - joyFloatWindowStart > JOY_FLOAT_WINDOW_MS) {
        joyFloatCount       = 0;
        joyFloatWindowStart = millis();
      }
      joyFloatCount++;
      if (joyFloatCount >= JOY_FLOAT_MIN_COUNT) {
        // 無効化：全サーボフラグをクリアしてから disabled へ
        joyServoXActive    = false;
        joyServoYActive    = false;
        joyEyeXActive      = false;
        joyEyeYActive      = false;
        joystickWasActive  = false;
        joyStuck           = false;
        joyFilteredX       = (float)joyCenterX;
        joyFilteredY       = (float)joyCenterY;
        joystickEnabled    = false;
        joyStableCount     = 0;
        joyStableWindowStart = millis();
        joyPrevRawX        = -1;
        joyPrevRawY        = -1;
        addLog("JOY DISABLED: not connected/floating (dx=" + String(dx)
               + " dy=" + String(dy) + " count=" + String(joyFloatCount) + ")");
        return;
      }
      // ── 浮き疑いサンプルの同一loop即時スキップ（2026/07/09 改修3）──
      // 従来はJOY DISABLED確定（8回）を待つ間、疑いサンプル1〜7個が
      // LPF→入力判定→サーボwriteへ素通りし、無効化より先に
      // JOY X/Y SERVO が1回実行されていた（APモード実測ログで確認）。
      // 疑いサンプルはカウントのみ行い、このloopでは一切使わずreturnする。
      // 素早い実操作がまれにここへ入っても、スキップは当該loopのみで
      // スルーレート追従（1度/30ms）への影響は体感できない。
      {
        static unsigned long lastFloatSkipLog = 0;
        if (millis() - lastFloatSkipLog > 500) {
          lastFloatSkipLog = millis();
          addLog("JOY FLOAT SAMPLE SKIPPED dx=" + String(dx)
               + " dy=" + String(dy) + " count=" + String(joyFloatCount));
        }
      }
      joyPrevRawX = rawX;
      joyPrevRawY = rawY;
      return;
    } else {
      // 正常サンプルが来たらウィンドウをリセット（散発的ノイズを無視）
      if (millis() - joyFloatWindowStart > JOY_FLOAT_WINDOW_MS) {
        joyFloatCount       = 0;
        joyFloatWindowStart = millis();
      }
    }
  }
  joyPrevRawX = rawX;
  joyPrevRawY = rawY;

  // ── 異常値判定（断線・浮き・接触不良ガード） ──────────────
  // 押し込み（rawX>4000 && rawY>4000）は正常値なので除外した上で、
  // 片方だけ極端に低い（< 100）は断線・浮きと判定する。
  //
  // 判定条件：
  //   押し込みでないのに、rawX<100 または rawY<100 → 異常
  //
  // 連続 JOY_ABNORMAL_THRESHOLD 回以上異常が続いた場合のみ処理をスキップ。
  // 1回のノイズでは反応しない。
  bool joyAbnormal = !joyPushRaw && (rawX < 100 || rawY < 100);

  if (joyAbnormal) {
    joyAbnormalCount++;
    if (joyAbnormalCount >= JOY_ABNORMAL_THRESHOLD) {
      // 全フラグをリセットしてサーボへの異常指令を防ぐ
      joyServoXActive   = false;
      joyServoYActive   = false;
      joyEyeXActive     = false;
      joyEyeYActive     = false;
      joystickWasActive = false;
      joyStuck          = false;
      joyActiveStartTime = 0;
      // フィルター値を中心に戻す（サーボが中央に留まるよう）
      joyFilteredX = (float)joyCenterX;
      joyFilteredY = (float)joyCenterY;
      if (DEBUG_JOYSTICK) addLog("JOY ABNORMAL: rawX=" + String(rawX) + " rawY=" + String(rawY));
      return;
    }
  } else {
    joyAbnormalCount = 0;  // 正常値が来たらリセット
  }

  // ── ローパスフィルター適用 ────────────────────────────
  // ノイズを除去してから以降の判定に使う。
  // push判定（4000超）は生値で行う（フィルターがかかると反応が遅れる）。
  joyFilteredX = joyFilteredX * (1.0f - JOY_LPF_ALPHA) + rawX * JOY_LPF_ALPHA;
  joyFilteredY = joyFilteredY * (1.0f - JOY_LPF_ALPHA) + rawY * JOY_LPF_ALPHA;

  int x = (int)joyFilteredX;
  int y = (int)joyFilteredY;

  // ── 押し込み判定（生値で判定・最優先） ──────────────────
  // push 時は両軸が 4000 超になる。フィルター後では反応が遅れるため生値を使う。
  // joyPushRaw は異常値判定で計算済み。
  //
  // 【役割変更】筐体改修で左右も離せばセンター復帰するようになったため、
  // 押し込みは「センター戻し」ではなく「ランダム独り言WAV再生」に変更。
  // joyPushRaw はレベル信号（押している間ずっとtrue）なので、
  // 立ち上がりエッジ（false→true）でのみ1回再生してデバウンスする。
  static bool joyPushPrev = false;
  if (joyPushRaw) {
    bool pushEdge = !joyPushPrev;  // 押し込みの瞬間だけ true
    joyPushPrev = true;

    // 押し込み中にLPFが4095側へ汚染されるのを防ぐ（旧コードにあったリセットを復元）。
    // これが無いと、離した瞬間にフィルタ値が中心へ収束するまでの間
    // devX/devY が大きな正値になり、頭が右下方向へ一瞬誤追従する。
    joyFilteredX = (float)joyCenterX;
    joyFilteredY = (float)joyCenterY;

    if (sleepMode) {
      // 従来どおり押し込みでも起床できる（独り言は鳴らさない）
      wakeUp("JOYSTICK_PUSH");
    } else if (pushEdge) {
      // ── ジョイスティック手動独り言の判定 ──
      // WAV再生中・サーボ動作中・sleepMode・petMode だけを hard block とする。
      // alertMode / externalSpeaking は MICモードの押し込み音で誤発火するため
      // hard block から外し、MICモード時はクリアしてから再生する。
      // 自動独り言（handleRandomMutter）は canDoIdleAction() を経由するため、
      // alertMode / externalSpeaking による抑制は従来どおり維持される。
      const bool hardBlocked = soundBusy || servoBusy || sleepMode || petMode;
      if (!hardBlocked) {
        // MICモードで alertMode / externalSpeaking が立っていた場合は
        // 押し込み筐体音をマイクが拾ったものとみなしてクリアする。
        if (audioSource == AUDIO_SRC_MIC && (externalSpeaking || alertMode)) {
          bool wasAlert   = alertMode;
          bool wasSpeak   = externalSpeaking;
          alertMode       = false;
          externalSpeaking = false;
          // stopExternalSpeaking()を通らずに発話を打ち切るため、
          // attach保持が残っていたら通常の2秒AUTO DETACHを再スケジュールする。
          if (ENABLE_LR_SERVO && servoLR.attached()) {
            lrMarkMoved();
          }
          micLevel         = 0;
          micLastAboveTime = 0;
          micOnFirstHitTime  = 0;  // START三段確認の保留ヒットも破棄
          micOnSecondHitTime = 0;
          micOnFirstHitLevel = 0;
          micWeakStartTime   = 0;  // WEAK帯継続判定もリセット
          String overrideLog = "JOYSTICK MUTTER: override MIC";
          if (wasSpeak)  overrideLog += " speaking";
          if (wasAlert)  overrideLog += " alert";
          addLog(overrideLog);
        } else {
          addLog("JOYSTICK MUTTER");
        }
        playMutterOnce("JOYSTICK");
      } else {
        addLog("JOYSTICK MUTTER skip (busy/suppressed)");
      }
      lastInteractionTime = millis();
    }
    return;
  }
  joyPushPrev = false;  // 離したらエッジをリセット（次回の押し込みを検出可能に）

  // ── 睡眠中の起床処理（通常操作より厳しい条件） ────────────
  if (sleepMode) {
    // sleep直後3秒は無視（誤wake防止）
    if (millis() - sleepStartTime < SLEEP_WAKE_IGNORE_MS) {
      sleepJoyConfirmStart = 0;
      return;
    }

    int devXraw = rawX - joyCenterX;
    int devYraw = rawY - joyCenterY;

    // sleep解除用閾値：通常のservoOn閾値の2倍（より明確に倒した場合のみ）
    int wakeThX = joyServoOnThX * 2;
    int wakeThY = joyServoOnThY * 2;

    if (abs(devXraw) > wakeThX || abs(devYraw) > wakeThY) {
      // 閾値超え → 連続確認タイマー開始
      if (sleepJoyConfirmStart == 0) {
        sleepJoyConfirmStart = millis();
      }
      // 400ms以上連続して閾値を超えていたらwake
      if (millis() - sleepJoyConfirmStart > SLEEP_JOY_CONFIRM_MS) {
        addLog(String("WAKE TRIGGER reason=JOYSTICK")
             + " raw=(" + String(rawX) + "," + String(rawY) + ")"
             + " center=(" + String(joyCenterX) + "," + String(joyCenterY) + ")"
             + " dx=" + String(devXraw) + " dy=" + String(devYraw)
             + " th=" + String(wakeThX) + "/" + String(wakeThY)
             + " dur=" + String(millis() - sleepJoyConfirmStart));
        sleepJoyConfirmStart = 0;
        wakeUp("JOYSTICK");
        joyServoXActive   = false;
        joyServoYActive   = false;
        joystickWasActive = false;
      }
    } else {
      // 閾値を下回ったらリセット
      sleepJoyConfirmStart = 0;
    }
    return;
  }

  // ── フィルター後の値で中心からのずれを計算 ────────────
  int devX = x - joyCenterX;
  int devY = y - joyCenterY;
  int absX = abs(devX);
  int absY = abs(devY);

  // ── 同相ディップ検出（2026/07/09 改修3）─────────────────────
  // 電源レール低下による幻入力は「両軸同時・同方向・ほぼ同量」が特徴
  // （実測: absX=251/absY=248、absX=260/absY=262 → 差1〜3カウント）。
  // 機械的な実入力で両軸がここまで一致することはまれ。
  // 両軸ともservo ON閾値超え・同符号・差が大きい方の20%未満なら
  // 同相ノイズと判定し、このloopは駆動しない（フラグ・LPFは維持）。
  // 完全対角の実操作を弾く可能性はあるが、角度をわずかに変えれば動く。
  // 抑制窓（600ms）に依存しないため、UD保持ハンチング等で窓の外に
  // ずれ込んだ幻入力もここで止まる。
  if (absX > joyServoOnThX && absY > joyServoOnThY &&
      ((devX < 0) == (devY < 0)) &&
      (abs(absX - absY) * 5 < max(absX, absY))) {
    static unsigned long lastCommonModeLog = 0;
    if (millis() - lastCommonModeLog > 500) {
      lastCommonModeLog = millis();
      addLog("JOY COMMON-MODE SUPPRESSED devX=" + String(devX)
           + " devY=" + String(devY));
    }
    return;
  }

  // ── ジョイスティック固着（stuck）検出 ────────────────────
  bool anyActive = (absX > joyServoOnThX || absY > joyServoOnThY
                 || absX > joyEyeOnThX  || absY > joyEyeOnThY);

  if (anyActive) {
    // 入力が続いている
    if (joyActiveStartTime == 0) {
      joyActiveStartTime = millis();  // 入力開始時刻を記録
    }
    if (!joyStuck && millis() - joyActiveStartTime > JOY_STUCK_THRESHOLD_MS) {
      joyStuck = true;
      if (DEBUG_JOYSTICK) addLog("JOY STUCK: X=" + String(x) + " Y=" + String(y)
           + " elapsed=" + String(millis() - joyActiveStartTime));
    }
  } else {
    // ニュートラルに戻った
    if (joyStuck) {
      if (DEBUG_JOYSTICK) addLog("JOY RECOVER: X=" + String(x) + " Y=" + String(y));
      joyStuck = false;
    }
    joyActiveStartTime = 0;
  }

  // ── stuck中はヒステリシス判定・サーボ・黒目をすべてスキップ ──
  if (joyStuck) {
    joyServoXActive   = false;
    joyServoYActive   = false;
    joyEyeXActive     = false;
    joyEyeYActive     = false;
    joystickWasActive = false;
    setEyeDirection(0, 0, "CENTER");  // 黒目を確実に中央へ戻す
    return;
  }

  // ── サーボ用ヒステリシス判定（軸ごとに独立した閾値を使う） ──
  if (joyServoXActive) {
    if (absX < joyServoOffThX) joyServoXActive = false;
  } else {
    if (absX > joyServoOnThX) joyServoXActive = true;
  }

  if (joyServoYActive) {
    if (absY < joyServoOffThY) joyServoYActive = false;
  } else {
    if (absY > joyServoOnThY) joyServoYActive = true;
  }

  // ── 黒目用ヒステリシス判定 ────────────────────────────
  if (joyEyeXActive) {
    if (absX < joyEyeOffThX) joyEyeXActive = false;
  } else {
    if (absX > joyEyeOnThX) joyEyeXActive = true;
  }

  if (joyEyeYActive) {
    if (absY < joyEyeOffThY) joyEyeYActive = false;
  } else {
    if (absY > joyEyeOnThY) joyEyeYActive = true;
  }

  // ── 全軸ニュートラル判定 ──────────────────────────────
  bool bothServoNeutral = (!joyServoXActive && !joyServoYActive);
  bool bothEyeNeutral   = (!joyEyeXActive  && !joyEyeYActive);

  if (bothServoNeutral && bothEyeNeutral) {
    if (joystickWasActive) {
      saveServoPositionToRtc();
      joystickWasActive = false;
      lastJoyActiveTime = 0;  // タイムアウトタイマーリセット
    }
    setEyeDirection(0, 0, "CENTER");  // 黒目を確実に中央へ戻す
    return;
  }

  // ── 入力タイムアウト（5秒で強制リセット） ────────────────
  // 故障・接触不良・倒しっぱなしで永続入力になるのを防ぐ。
  // JOY_STUCK_THRESHOLD_MS（1500ms）よりも長い5秒を設定。
  if (lastJoyActiveTime == 0) {
    lastJoyActiveTime = millis();
  }
  if (millis() - lastJoyActiveTime > JOY_INPUT_TIMEOUT_MS) {
    if (DEBUG_JOYSTICK) addLog("JOY TIMEOUT: forcing reset");
    joyServoXActive    = false;
    joyServoYActive    = false;
    joyEyeXActive      = false;
    joyEyeYActive      = false;
    joystickWasActive  = false;
    joyStuck           = false;
    joyActiveStartTime = 0;
    lastJoyActiveTime  = 0;
    joyFilteredX       = (float)joyCenterX;
    joyFilteredY       = (float)joyCenterY;
    setEyeDirection(0, 0, "CENTER");
    return;
  }

  // externalSpeaking中もジョイスティック操作を受け付ける
  // （口パクと首・目の動きは競合しないため）
  if (servoBusy) {
    return;
  }

  // ── 書き込みレート制限 ────────────────────────────────
  if (millis() - lastJoystickWriteTime < JOY_WRITE_INTERVAL) {
    return;
  }

  lastJoystickWriteTime = millis();
  joystickWasActive     = true;

  // lastInteractionTime はサーボまたは黒目が実際にアクティブなときだけ更新。
  // ノイズで閾値付近をうろついていても lastInteractionTime を更新しないため
  // スリープタイムアウトがリセットされない。
  if (joyServoXActive || joyServoYActive || joyEyeXActive || joyEyeYActive) {
    lastInteractionTime = millis();
  }

  // ── X軸（左右倒し）→ servoLR（左右）比例マッピング ──────
  // 筐体再設計でジョイスティックを土台側へ移設したため、Yaw可動が
  // ジョイスティック入力へ影響しなくなった。上下（Y軸→servoUD）と
  // 同じ考え方で、左右も黒目＋首（servoLR）を同時追従させる。
  // HEAD_LEFT=大きい角度 / HEAD_RIGHT=小さい角度（コメント定義に一致）。
  if (joyServoXActive) {
    int targetLR;
    // 分母は理論端(0/4095)ではなく、キャリブ済みの対称ストローク(joyStrokeX)を使う。
    // さらに85%で飽和させ、物理レバーが端まで届かない個体差とLPF遅れを吸収。
    // これによりスティック最大倒しで確実に HEAD_LEFT=120 / HEAD_RIGHT=60 へ到達し、
    // Web操作と左右可動幅が一致する。
    int effStroke = joyStrokeX * 85 / 100;
    float ratio = (float)(absX - joyServoOnThX)
                  / (float)max(effStroke - joyServoOnThX, 1);
    ratio = constrain(ratio, 0.0f, 1.0f);

    if (devX < 0) {
      // 左へ倒す → HEAD_RIGHT 方向（2026/07/10: リンク機構変更により左右反転）
      targetLR = (int)(HEAD_HORIZONTAL_CENTER
                        + ratio * (HEAD_RIGHT - HEAD_HORIZONTAL_CENTER));
    } else {
      // 右へ倒す → HEAD_LEFT 方向（2026/07/10: リンク機構変更により左右反転）
      targetLR = (int)(HEAD_HORIZONTAL_CENTER
                        + ratio * (HEAD_LEFT - HEAD_HORIZONTAL_CENTER));
    }
    targetLR = constrain(targetLR,
                          min(HEAD_RIGHT, HEAD_LEFT),
                          max(HEAD_RIGHT, HEAD_LEFT));
    if (targetLR != lrNow) {
      if (ENABLE_LR_SERVO) {
        // ── 2026/07/07: スルーレート制限（1度刻み・ノンブロッキング）──
        // 旧: servoLR.write(targetLR) の一発反映（Yaw唯一の直接ジャンプwrite）。
        // 新: JOY_LR_SLEW_STEP_MS ごとに lrNow を targetLR へ1度だけ近づける。
        // handleJoystick はループ毎（smartDelay中も）呼ばれるため滑らかに追従する。
        // 書き込む角度は常に lrNow 自身なので、管理値と実位置は絶対に食い違わない。
        static unsigned long lastJoyLrStep = 0;
        if (millis() - lastJoyLrStep >= JOY_LR_SLEW_STEP_MS) {
          lastJoyLrStep = millis();
          if (!servoLR.attached()) {
            servoLR.attach(LR_SERVO_PIN);
            // 最初のwrite＝PWM開始＝保持トルク開始点。
            // 旧: targetLR直書き → 新: 他経路と同じく現在角(lrNow)で通電し、
            // 次ステップ以降の1度刻みで追従させる（再attach直後のジャンプ排除）。
            servoLR.write(lrNow);
            addLog("LR RE-ATTACH (joystick) target=" + String(targetLR)
                 + " lrNow=" + String(lrNow));
          } else {
            lrMoveCaller = "joy";
            lrNow += (targetLR > lrNow) ? 1 : -1;
            servoLR.write(lrNow);
          }
          lrMarkMoved();  // LR自動detachタイマーを更新（moveSmooth()と同じ扱い）
        }
      } else {
        lrNow = targetLR;  // 無効軸はロジック整合のみ（実機なし・従来どおり）
      }

      // 【一時ログ】可動幅確認用。300msスロットルでRAMログの溢れを防ぐ。
      // 確認が済んだらこのブロックごと削除してよい。
      static unsigned long lastJoyXLog = 0;
      if (millis() - lastJoyXLog > 300) {
        lastJoyXLog = millis();
        addLog("JOY X SERVO target=" + String(targetLR)
             + " now=" + String(lrNow)
             + " ratio=" + String(ratio, 2)
             + " absX=" + String(absX));
      }
    }
  }

  // ── Y軸（上下倒し）→ servoUD（上下）比例マッピング ─────
  if (joyServoYActive) {
    int targetUD;
    // X軸と同方式：分母は理論端(0/4095)ではなくキャリブ済みの対称ストローク
    // (joyStrokeY)の85%を使い、物理レバーが端まで届かない個体差とLPF遅れを吸収。
    // スティック最大倒しで確実に HEAD_UP / HEAD_DOWN の最大角へ到達する。
    int effStroke = joyStrokeY * 85 / 100;
    float ratio = (float)(absY - joyServoOnThY)
                  / (float)max(effStroke - joyServoOnThY, 1);
    ratio = constrain(ratio, 0.0f, 1.0f);

    // 2026/07/10: Y軸反転（前に倒す=下向き、後ろに引く=上向き）
    if (devY < 0) {
      // 前に倒す → 下向き（制限なし・HEAD_DOWN=70）
      targetUD = (int)(HEAD_VERTICAL_CENTER
                        + ratio * (HEAD_DOWN - HEAD_VERTICAL_CENTER));
    } else {
      // 後ろに引く → 上向き（JOY_UD_UP=140 ソフトリミット適用）
      targetUD = (int)(HEAD_VERTICAL_CENTER
                        + ratio * (JOY_UD_UP - HEAD_VERTICAL_CENTER));
    }
    targetUD = constrain(targetUD,
                          min(HEAD_DOWN, JOY_UD_UP),
                          max(HEAD_DOWN, JOY_UD_UP));
    if (targetUD != udNow) {
      if (ENABLE_UD_SERVO) {
        servoUD.write(targetUD);
      }
      udNow = targetUD;

      // 【一時ログ】可動幅確認用。300msスロットルでRAMログの溢れを防ぐ。
      // 確認が済んだらこのブロックごと削除してよい。
      static unsigned long lastJoyYLog = 0;
      if (millis() - lastJoyYLog > 300) {
        lastJoyYLog = millis();
        addLog("JOY Y SERVO target=" + String(targetUD)
             + " ratio=" + String(ratio, 2)
             + " absY=" + String(absY));
      }
    }
  }

  // ── 黒目演出 ─────────────────────────────────────────
  int eyeX = 0;
  int eyeY = 0;
  String eyeLabel = "CENTER";

  if (joyEyeXActive) {
    eyeX     = (devX < 0) ? EYE_SHIFT_PIXELS : -EYE_SHIFT_PIXELS;
    eyeLabel = (devX < 0) ? "LEFT" : "RIGHT";
  } else if (joyEyeYActive) {
    // 2026/07/10: Y軸反転に合わせて黒目も反転
    eyeY     = (devY < 0) ? EYE_SHIFT_PIXELS : -EYE_SHIFT_PIXELS;
    eyeLabel = (devY < 0) ? "DOWN" : "UP";
  }

  setEyeDirection(eyeX, eyeY, eyeLabel);
}

bool handleSleepTransition() {
  // 音楽再生・口パク中（＝発話中 or 新鮮なFFT受信中）はスリープしない。
  // かつ、その間は lastInteractionTime を更新し続けることで、音が止まった
  // “その時点”から通常のSLEEP_TIMEOUTを計測する（古い時刻での即時スリープ防止）。
  if (isActiveAudioSession()) {
    lastInteractionTime = millis();
    return false;
  }

  if (!sleepMode && !alertMode && !petMode && millis() - lastInteractionTime > SLEEP_TIMEOUT) {

    yawn();

    sleepMode = true;

    // スリープへ入る瞬間に Lighting合成状態を確実に解除しておく（黒帯残り防止）。
    // 以降 loop() は睡眠ゲートで早期returnし updateScreenEffects() に到達しないため、
    // ここで解除しないと screenFxLighting が残り、起床後に黒帯が残留する。
#ifdef FFT_DISPLAY_TEST
    exitLightingCompositeMode(false);   // 睡眠画面をこれから描くので再描画は不要
#endif

    if (ENABLE_SLEEP_WAKE_SERVO) {
      moveWithEyeLead(servoUD, udNow, HEAD_DOWN, 25,
                      0, EYE_SHIFT_PIXELS, "DOWN");
    }

    drawSleepScreen();

    addLog("SLEEP");

    // スリープ中は LRサーボの保持トルクが不要なため即 detach する。
    // wakeUp() 時に moveSmooth(servoLR,...) が再 attach する。
    // ENABLE_LR_SERVO=false の場合は servoLR.attached() が常にfalseのため
    // 自然にスキップされるが、意図を明確にするため明示ガードする。
    if (ENABLE_LR_SERVO && servoLR.attached()) {
      delay(100);  // 直前の動作安定待ち
      servoLR.detach();
      lrDetachScheduled = false;
      addLog("LR SERVO DETACH ON SLEEP");
    }

    // スリープ移行直前にSDログをフラッシュしておく。
    // sleep中はフラッシュしないため、直前ログをここで確実に保存する。
    flushSdLog();

    sleepStartTime = millis();
    clockShownInSleep  = false;
    sleepFaceActive    = false;
    lastSleepFaceRotateTime = 0;
    lastSleepFaceAttemptTime = 0;

    // Sleep Lighting Carousel（目×Lighting）の状態を新しいSleepセッション用にリセットする。
    // cfg_lightingMask等の共有Lighting設定はここでは一切触れない（そもそも参照していない）。
    sleepCarouselStarted        = false;
    sleepCarouselNextSwitchMs   = 0;
    sleepLightingComposeActive  = false;
    sleepLastEyeKind            = -1;
    sleepLastBgLightMode        = -1;
    sleepLastPattern            = -1;
    sleepLightNeedsInit         = true;

    hasPrevious = false;
    cameraMotionCount = 0;
    // lastNoseMove はリセットしない（スリープ中も鼻ヒクヒクを継続するため）
    lastBlinkCheck = millis();

    alertUntil = millis() + 1500;

    return true;
  }

  return false;
}

bool handleStandbyGate() {

  // ── sleepMode 中の起床判定 ────────────────────────────
  // sleepMode 中はタッチ・IMU で wakeUp() できるようにする。
  // handleAwakeModeTouch() より前に評価して、タッチ値を消費される前に
  // 起床処理に届かせる。
  if (sleepMode) {
    // タッチ起床（即時・変更なし）
    if (isHeadTouched()) {
      addLog("WAKE TRIGGER reason=TOUCH (sleepMode)");
      wakeUp("TOUCH");
      return true;
    }

    // IMU起床（連続確認方式）
    // sleep直後3秒は無視
    if (millis() - sleepStartTime < SLEEP_WAKE_IGNORE_MS) {
      sleepImuConfirmStart = 0;
      return false;
    }

    float ax, ay, az;
    CoreS3.Imu.getAccel(&ax, &ay, &az);
    float accel = sqrt(ax * ax + ay * ay + az * az);
    currentAccel = accel;

    if (accel > SLEEP_IMU_THRESHOLD) {
      // 閾値超え → 連続確認タイマー開始
      if (sleepImuConfirmStart == 0) {
        sleepImuConfirmStart = millis();
      }
      // 400ms以上連続して閾値を超えていたらwake
      if (millis() - sleepImuConfirmStart > SLEEP_IMU_CONFIRM_MS) {
        {
          char ibuf[128];
          snprintf(ibuf, sizeof(ibuf),
                   "WAKE TRIGGER reason=IMU A=%.2f th=%.1f dur=%lu",
                   accel, SLEEP_IMU_THRESHOLD,
                   millis() - sleepImuConfirmStart);
          addLog(ibuf);
        }
        sleepImuConfirmStart = 0;
        wakeUp("IMU");
        return true;
      }
    } else {
      // 閾値を下回ったらリセット
      sleepImuConfirmStart = 0;
    }

    return false;
  }

  // ── 覚醒中のタッチ処理（なでなで喜び顔）───────────────
  // 手動スタンバイ（旧awakeModeゲート）は廃止済み。
  if (handleAwakeModeTouch()) {
    return true;
  }

  return false;
}

// ====================================================
// playDirectionWav()  ジョイスティック方向音声専用再生
//
// playWavFromSD() との違い：
// ・口パク演出（drawMouthOpen/Closed）なし
// ・smartDelay(120) なし → whileループが1msごとに回る
// ・handleJoystick() / handleNoseMotion() を毎ms呼ぶ
// ・ジョイスティック入力があれば即座に再生を中断して追従
// ・1秒以内の短いSEに最適化
// ====================================================
void playDirectionWav(const char* path) {
  // 内蔵マイクモード中は全スピーカー再生禁止（自己音声の拾い込み防止）
  if (!isSpeakerAllowed()) return;
  if (soundBusy) return;
  soundBusy = true;

  lastInteractionTime = millis();

  micEnabled = false;
  delay(50);  // マイク無効化の安定待ち（音割れ防止・必須）

  File f = SD.open(path);
  if (!f) {
    addLog("DIR WAV OPEN FAIL: " + String(path));
    micEnabled = true;
    soundBusy = false;
    return;
  }

  size_t size = f.size();

  // サイズ上限チェック（playWavFromSD と同基準）
  if (size == 0 || size > MAX_WAV_SIZE) {
    char buf128[128];
    snprintf(buf128, sizeof(buf128), "DIR WAV SIZE NG: %s size=%u",
             path, (unsigned)size);
    addLog(buf128);
    f.close();
    micEnabled = true;
    soundBusy = false;
    return;
  }

  uint8_t* buf = (uint8_t*)malloc(size);
  if (!buf) {
    addLog("DIR WAV MALLOC FAIL: heap=" + String(ESP.getFreeHeap()));
    f.close();
    micEnabled = true;
    soundBusy = false;
    return;
  }

  size_t readSize = f.read(buf, size);
  f.close();

  if (readSize != size) {
    free(buf);
    micEnabled = true;
    soundBusy = false;
    return;
  }

  addLog("PLAY DIR WAV: " + String(path));

  CoreS3.Speaker.setVolume(128);
  CoreS3.Speaker.playWav(buf, size, 1, false);

  while (CoreS3.Speaker.isPlaying()) {
    lastInteractionTime = millis();

    server.handleClient();
    handleUDP();

    // Mac音声優先
    if (macAudioLinkEnabled && externalSpeaking) {
      CoreS3.Speaker.stop();
      break;
    }

    // ジョイスティック入力があれば即中断
    // （新しい方向に動かした場合は音声より追従を優先）
    // joystickEnabled=false（未接続・浮き）の間は読み取りをスキップ
    if (joystickEnabled) {
      int rawX = analogRead(JOY_X_PIN);
      int rawY = analogRead(JOY_Y_PIN);
      int devXraw = rawX - joyCenterX;
      int devYraw = rawY - joyCenterY;
      if (abs(devXraw) > joyServoOnThX || abs(devYraw) > joyServoOnThY) {
        CoreS3.Speaker.stop();
        break;
      }
    }

    handleNoseMotion();
    delay(5);  // 1ループ5ms → 1秒WAVで約200回ループ（120msより大幅に短縮）
  }

  free(buf);
  micEnabled = true;
  soundBusy = false;
}

void playWavFromSD(const char* path, bool skipMouthAnim) {
  // 内蔵マイクモード中は全スピーカー再生禁止（自己音声の拾い込み防止）
  if (!isSpeakerAllowed()) {
    addLog("WAV SKIP (MIC MODE): " + String(path));
    return;
  }
  if (soundBusy) return;
  soundBusy = true;

  // WAV再生はユーザー操作に起因するため、スリープタイマーをリセット
  lastInteractionTime = millis();

  micEnabled = false;
  delay(100);  // マイク無効化の安定待ち（音割れ防止・必須）

  File f = SD.open(path);

  if (!f) {
    addLog("WAV OPEN FAIL: " + String(path));

    micEnabled = true;
    soundBusy = false;
    return;
  }

  size_t size = f.size();

  // サイズ上限チェック（SD破損・巨大ファイルによるヒープ枯渇を防ぐ）
  if (size == 0 || size > MAX_WAV_SIZE) {
    char buf128[128];
    snprintf(buf128, sizeof(buf128), "WAV SIZE NG: %s size=%u max=%u",
             path, (unsigned)size, (unsigned)MAX_WAV_SIZE);
    addLog(buf128);
    f.close();
    micEnabled = true;
    soundBusy = false;
    return;
  }

  addLog("PLAY WAV PRE: heap=" + String(ESP.getFreeHeap())
       + " minHeap=" + String(ESP.getMinFreeHeap())
       + " size=" + String((unsigned)size));

  uint8_t* buf = (uint8_t*)malloc(size);

  if (!buf) {
    addLog("WAV MALLOC FAIL: heap=" + String(ESP.getFreeHeap()));
    f.close();
    micEnabled = true;
    soundBusy = false;
    return;
  }

  size_t readSize = f.read(buf, size);
  f.close();

  if (readSize != size) {
    char buf128[128];
    snprintf(buf128, sizeof(buf128), "WAV READ FAIL: %s read=%u expect=%u",
             path, (unsigned)readSize, (unsigned)size);
    addLog(buf128);
    free(buf);
    micEnabled = true;
    soundBusy = false;
    return;
  }

  addLog("PLAY WAV: " + String(path));

  CoreS3.Speaker.setVolume(128);

  smartDelay(50);  // 旧:delay(50) → スピーカー安定待ち（短いので残す、ただしWeb継続のためsmartDelayに）

  // 非同期再生で開始。終了判定は推定durationではなく isPlaying() を使う。
  CoreS3.Speaker.playWav(buf, size, 1, false);

  while (CoreS3.Speaker.isPlaying()) {
    // WAV再生中もスリープタイマーをリセット（再生中にスリープ移行しないよう）
    lastInteractionTime = millis();

    server.handleClient();
    handleUDP();

    if (macAudioLinkEnabled && externalSpeaking) {
      addLog("MAC AUDIO PRIORITY: stop local WAV");
      CoreS3.Speaker.stop();
      break;
    }

    // 再生中も鼻ヒクヒクとジョイスティック入力を生かす
    handleNoseMotion();
    handleJoystick();

    updateTalkMicroMotion();

    if (!skipMouthAnim) drawMouthOpen();
    smartDelay(120);

    server.handleClient();
    handleUDP();

    if (macAudioLinkEnabled && externalSpeaking) {
      CoreS3.Speaker.stop();
      break;
    }

    handleNoseMotion();
    handleJoystick();

    updateTalkMicroMotion();

    if (!skipMouthAnim) drawMouthClosed();
    smartDelay(120);
  }

  resetTalkMicroMotion();

  if (!skipMouthAnim) drawMouthClosed();

  free(buf);
  addLog("PLAY WAV END: heap=" + String(ESP.getFreeHeap()));

  micEnabled = true;
  soundBusy = false;
}

void saveLatestBmp() {
  if (!CoreS3.Camera.fb) return;

  int width = CoreS3.Display.width();
  int height = CoreS3.Display.height();

  uint16_t* pixels = (uint16_t*)CoreS3.Camera.fb->buf;

  const int headerSize = 54;
  const int rowSize = width * 3;
  const int imageSize = rowSize * height;
  const int fileSize = headerSize + imageSize;

  // 初回だけ確保。サイズが変わった時だけ作り直す。
  if (!latestBmp || latestBmpSize != fileSize) {
    if (latestBmp) {
      free(latestBmp);
      latestBmp = nullptr;
      latestBmpSize = 0;
    }

    latestBmp = (uint8_t*)malloc(fileSize);

    if (!latestBmp) {
      addLog("latestBmp malloc failed");
      return;
    }

    latestBmpSize = fileSize;
  }

  memset(latestBmp, 0, fileSize);

  latestBmp[0] = 'B';
  latestBmp[1] = 'M';
  latestBmp[2] = fileSize;
  latestBmp[3] = fileSize >> 8;
  latestBmp[4] = fileSize >> 16;
  latestBmp[5] = fileSize >> 24;
  latestBmp[10] = headerSize;

  latestBmp[14] = 40;
  latestBmp[18] = width;
  latestBmp[19] = width >> 8;
  latestBmp[20] = width >> 16;
  latestBmp[21] = width >> 24;
  latestBmp[22] = height;
  latestBmp[23] = height >> 8;
  latestBmp[24] = height >> 16;
  latestBmp[25] = height >> 24;
  latestBmp[26] = 1;
  latestBmp[28] = 24;

  int dst = headerSize;

  for (int y = height - 1; y >= 0; y--) {
    for (int x = 0; x < width; x++) {
      uint16_t p = pixels[y * width + x];
      p = (p >> 8) | (p << 8);

      uint8_t r = ((p >> 11) & 0x1F) << 3;
      uint8_t g = ((p >> 5) & 0x3F) << 2;
      uint8_t b = (p & 0x1F) << 3;

      latestBmp[dst++] = b;
      latestBmp[dst++] = g;
      latestBmp[dst++] = r;
    }
  }

  latestBmpTime = millis();
}

String getTimestampString() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 0)) {
    if (rtcTimeValid) {
      // NTP未取得だがRTCが有効 → BM8563を直読み
      auto dt = CoreS3.Rtc.getDateTime();
      char buf[32];
      snprintf(buf, sizeof(buf), "RTC%04d%02d%02d_%02d%02d%02d",
               dt.date.year, dt.date.month, dt.date.date,
               dt.time.hours, dt.time.minutes, dt.time.seconds);
      return String(buf);
    }
    return "notime_" + String(millis());
  }

  char buf[32];

  sprintf(buf, "%04d%02d%02d_%02d%02d%02d",
          timeinfo.tm_year + 1900,
          timeinfo.tm_mon + 1,
          timeinfo.tm_mday,
          timeinfo.tm_hour,
          timeinfo.tm_min,
          timeinfo.tm_sec);

  return String(buf);
}

void appendToiletLog(String reason, String path) {
  File logFile = SD.open("/logtoilet/toilet_log.txt", FILE_APPEND);

  if (!logFile) {
    addLog("TOILET LOG: OPEN FAILED");
    return;
  }

  logFile.print(getTimestampString());
  logFile.print(",");
  logFile.print(reason);
  logFile.print(",");
  logFile.println(path);

  logFile.close();

  addLog("TOILET LOG SAVED: " + path);
}

void saveToiletSnapshot() {
  // 画像記録が無効なら何もしない（SDログとは独立して動作する）
  if (!cfg_enableToiletCam) return;

  // NTP未取得の場合はスキップ（ファイル名のソート順を保証するため）
  struct tm timecheck;
  if (!getLocalTime(&timecheck, 0)) {
    addLog("TOILET SNAPSHOT: SKIP (no NTP time)");
    return;
  }

  if (!CoreS3.Camera.fb) {
    addLog("TOILET SNAPSHOT: NO CAMERA FB");
    return;
  }

  if (!SD.exists("/logtoilet")) {
    SD.mkdir("/logtoilet");
  }

  // --- 枚数上限チェック: SNAPSHOT_MAX_COUNT を超えたら古いものから削除 ---
  {
    // ファイル名を収集してソート（名前順 = 時系列順）
    std::vector<String> bmpFiles;
    File root = SD.open("/logtoilet");
    if (root) {
      File f = root.openNextFile();
      while (f) {
        String name = String(f.name());
        if (!f.isDirectory() && !name.startsWith(".") && name.endsWith(".bmp")) {
          bmpFiles.push_back(name);
        }
        f = root.openNextFile();
      }
      root.close();
    }

    // 昇順ソート（古い順）
    std::sort(bmpFiles.begin(), bmpFiles.end());

    // 上限を超えている分だけ古いものから削除
    int deleteCount = (int)bmpFiles.size() - SNAPSHOT_MAX_COUNT + 1; // +1 = これから保存する1枚分
    for (int i = 0; i < deleteCount && i < (int)bmpFiles.size(); i++) {
      String delPath = "/logtoilet/" + bmpFiles[i];
      SD.remove(delPath.c_str());
      addLog("SNAPSHOT TRIMMED: " + delPath);
    }
  }

  saveLatestBmp();

  if (!latestBmp || latestBmpSize == 0) {
    addLog("TOILET SNAPSHOT: BMP FAILED");
    return;
  }

  String path = "/logtoilet/" + getTimestampString() + ".bmp";

  File file = SD.open(path, FILE_WRITE);

  if (!file) {
    addLog("TOILET SNAPSHOT: OPEN FAILED");
    return;
  }

  file.write(latestBmp, latestBmpSize);
  file.close();

  addLog("TOILET SNAPSHOT SAVED: " + path);

  appendToiletLog("MOTION_WAKE", path);
}

void handleUDP() {

  int packetSize;

  while ((packetSize = udp.parsePacket()) > 0) {

    char buf[64];  // TIME_SYNC メッセージ（最大35バイト）に対応するため64バイトに拡張
    int len = udp.read(buf, sizeof(buf) - 1);

    if (len <= 0) {
      continue;
    }

    buf[len] = '\0';

    // ── FFT ────────────────────────────────────────────────────────
    // FFTパケットは毎秒約12.5回届き、値が毎回変わるため、
    // 「UDP state changed」判定・SDログ・シリアルへは一切出力しない。
    // （出力するとSDログが毎秒12.5行増えて汚染される）
    // 値の保存だけ行って次のパケット処理へ進む。
    // ※ String生成より前に判定することで、FFTパケットでは
    //   Stringヒープ確保もlastUdpState比較も発生させない。
    // フォーマット: FFT:25,13,0,0,0,0,0,0（8バンド・0〜100・低音→高音）
    // ────────────────────────────────────────────────────────────
    if (strncmp(buf, "FFT:", 4) == 0) {
#if defined(EAR_UART_ENABLED) && defined(EAR_FFT_TO_FACE_ENABLED)
      // ソース整合ゲート（Step 4.1）：LINE IN選択中はFFTをEar専用とし、
      // UDPのFFT行は読み捨てる（SPEAK/TIME_SYNC等の他分岐には影響しない）。
      // UDP選択中は従来どおりUDPが供給する。
      if (audioSource == AUDIO_SRC_LINEIN) continue;
#endif
      int v[8];
      if (sscanf(buf + 4, "%d,%d,%d,%d,%d,%d,%d,%d",
                 &v[0], &v[1], &v[2], &v[3],
                 &v[4], &v[5], &v[6], &v[7]) == 8) {
        for (int i = 0; i < 8; i++) {
          fftLevel[i] = (uint8_t)constrain(v[i], 0, 100);
        }
        lastFftPacketTime = millis();
      }
      continue;  // ログ・lastUdpState更新・SPEAK系分岐を行わずに次のパケットへ
    }

    String udpState = String(buf);

    if (udpState != lastUdpState) {
      addLog("UDP state changed: " + String(udpState));
      lastUdpState = udpState;
    }

    // ── TIME_SYNC ──────────────────────────────────────────────────
    // Mac音声連携PythonからUDPで時刻を受信してESP32のRTCを補正する。
    // NTP同期できない環境（APモード・Wi-Fiなし）でも時刻を扱えるようにする。
    //
    // フォーマット: TIME_SYNC,YYYY-MM-DDTHH:MM:SS+09:00
    // 例:           TIME_SYNC,2026-07-02T09:15:30+09:00
    //
    // NTPが成功済みの場合はNTPを優先し、TIME_SYNCは受け付けない。
    // ──────────────────────────────────────────────────────────────
    if (strncmp(buf, "TIME_SYNC,", 10) == 0) {
      if (!timeEverSynced) {
        // NTPで同期済みでない場合のみ適用（NTP優先）
        const char* isoStr = buf + 10;  // "YYYY-MM-DDTHH:MM:SS+09:00" の部分

        struct tm t = {};
        // ISO 8601形式を解析: YYYY-MM-DDTHH:MM:SS（タイムゾーンは無視してJSTとして扱う）
        int parsed = sscanf(isoStr, "%d-%d-%dT%d:%d:%d",
                            &t.tm_year, &t.tm_mon, &t.tm_mday,
                            &t.tm_hour, &t.tm_min, &t.tm_sec);

        if (parsed == 6) {
          t.tm_year -= 1900;  // struct tm は 1900年基準
          t.tm_mon  -= 1;     // struct tm は 0基準（0=1月）
          t.tm_isdst = 0;

          // mktime() はシステムのタイムゾーン設定（configTime で JST +9h 設定済み）を
          // 考慮してローカル時刻を UTC epoch に変換して返す。
          // つまり「09:15:30 JST」として渡すと、UTC 00:15:30 の epoch が返る。
          // ここで手動の -9h 補正を加えると二重補正になるため行わない。
          time_t epochUTC = mktime(&t);

          struct timeval tv = { .tv_sec = epochUTC, .tv_usec = 0 };
          settimeofday(&tv, nullptr);

          timeEverSynced = true;
          addLog("TIME_SYNC OK: " + String(isoStr));
        } else {
          addLog("TIME_SYNC PARSE ERROR: " + String(isoStr));
        }
      } else {
        // NTP同期済みの場合はスキップ
        addLog("TIME_SYNC SKIPPED: NTP already synced");
      }

    } else if (strcmp(buf, "SPEAK_START") == 0) {

      if (macAudioLinkEnabled) {

        lastExternalSpeakTime = millis();
        lastSpeakPacketTime   = millis();
        // lastInteractionTime は externalSpeaking が false→true の初回のみ更新。
        // ハートビートで毎回更新するとsleepタイマーが永遠にリセットされる。

        if (!externalSpeaking) {
          // 新規発話開始（false→true）
          externalSpeaking    = true;
          mouthPakuOpen       = false;
          lastMouthPakuTime   = 0;
          lastTalkMicroMoveTime    = 0;
          nextTalkMicroMoveInterval = random(800, 1800);

          // 口パク開始を最優先：ログより先に状態確定
          if (soundBusy) {
            CoreS3.Speaker.stop();
            soundBusy = false;
            // ログは後で出す（遅延を最小化）
          }

          if (sleepMode) {
            wakeUp("MAC_AUDIO");
          }

          // 発話開始時だけ lastInteractionTime を更新
          lastInteractionTime = millis();

          addLog("SPEAK_START handled lat=" + String(millis()));
        }
        // ハートビート（すでに externalSpeaking=true）の場合は何もしない
      }

    } else if (strcmp(buf, "SPEAK_STOP") == 0) {
#if defined(EAR_UART_ENABLED) && defined(EAR_SPEAK_TO_MOUTH_ENABLED)
      // ソース整合（Step 5.1）：LINE IN選択中はEar発話をMacの迷子STOPで閉じない。
      // （SPEAK_STARTは既存の macAudioLinkEnabled ゲートがLINE IN中falseのため挿入不要）
      if (audioSource == AUDIO_SRC_LINEIN) continue;
#endif

      if (externalSpeaking) {
        // 口閉じを最優先：ログより先に実行
        externalSpeaking = false;
        mouthPakuOpen    = false;
        drawMouthClosed();
        resetTalkMicroMotion();
        lastExternalStopTime = millis();
        // 発話終了時点でsleepタイマーをリセット（stopExternalSpeaking()と対称）。
        // externalSpeaking=falseになった直後にhandleSleepTransition()がガードを
        // 素通りするため、ここで更新しないと古いlastInteractionTimeでスリープする。
        lastInteractionTime  = millis();

        addLog("EXTERNAL STOP lat=" + String(millis()));
      }
    }
  }
}

// ===== KARIPOM EAR v2 (Phase 0 / Step 3) =====
#ifdef EAR_UART_ENABLED
// earFftFresh() — Ear FFTが保持時間内か（Step 4：UDP FFTゲート判定）
bool earFftFresh() {
  return (earLastFftOkMs != 0) && (millis() - earLastFftOkMs <= EAR_FFT_HOLD_MS);
}

// handleEarUart() — Step 3：受信診断 ＋ Step 4：FFT供給
// ・1バイトずつ読み、'\n'で行確定（'\r'は無視）。固定長バッファ・String不使用
// ・FFT行は毎回ログしない（既存のFFTログ抑止ポリシーと同じ思想）
// ・10秒ごとに統計を addLog 1行だけ出す（SDログ肥大防止）
void handleEarUart() {
  while (Serial2.available() > 0) {
    char c = (char)Serial2.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (earRxLen < sizeof(earRxLineBuf) - 1) {
        earRxLineBuf[earRxLen++] = c;
      } else {
        earRxLen = 0;            // 過長行：行ごと破棄
        earRxBadLines++;
      }
      continue;
    }
    // ---- 行確定 ----
    earRxLineBuf[earRxLen] = '\0';
    uint8_t len = earRxLen;
    earRxLen = 0;
    if (len == 0) continue;
    earRxLines++;
    earRxLastMs = millis();

    if (strncmp(earRxLineBuf, "FFT:", 4) == 0) {
      // Step 4：厳密パース。数値8個（カンマ区切り・余分な文字なし）を
      // すべて読めた場合のみ採用。各値は0〜100へ制限（clamp）。
      uint8_t vals[8];
      const char* p = earRxLineBuf + 4;
      bool okParse = true;
      for (int k = 0; k < 8; k++) {
        if (*p < '0' || *p > '9') { okParse = false; break; }  // 数字始まり必須
        long v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) {
          v = v * 10 + (*p - '0');
          p++;
          digits++;
        }
        if (*p >= '0' && *p <= '9') { okParse = false; break; }  // 4桁以上は不正
        if (v > 100) v = 100;   // 0〜100制限（vは非負でしか作られない）
        vals[k] = (uint8_t)v;
        if (k < 7) {
          if (*p != ',') { okParse = false; break; }
          p++;
        }
      }
      if (okParse && *p != '\0') okParse = false;  // 8値の後に余分な文字がないこと
      if (okParse) {
        earRxFft++;
        earLastFftOkMs = millis();
#ifdef EAR_FFT_TO_FACE_ENABLED
        // ソース整合（Step 4.1）：LINE IN選択中のみVisualizerへ供給。
        // UDP/MIC/OFF選択中は統計カウントのみ（表示・状態に触れない）。
        if (audioSource == AUDIO_SRC_LINEIN) {
          for (int k = 0; k < 8; k++) { fftLevel[k] = vals[k]; }
          lastFftPacketTime = millis();
          earFftApplied++;
        }
#endif
      } else {
        earRxBadLines++;
      }
    } else if (strcmp(earRxLineBuf, "SPEAK_START") == 0) {
      earRxSpeakStart++;
#ifdef EAR_SPEAK_TO_MOUTH_ENABLED
      // Step 5.1：LINE IN選択中のみ口パクへ接続。
      // 処理内容はUDP経路（handleUDPのSPEAK_START）と同一手順の複製
      // （Phase 0はコード重複を許容。共通化はPhase 1）。
      if (audioSource == AUDIO_SRC_LINEIN) {
        lastExternalSpeakTime = millis();
        lastSpeakPacketTime   = millis();   // 既存5秒watchdogを共用
        if (!externalSpeaking) {
          externalSpeaking    = true;
          mouthPakuOpen       = false;
          lastMouthPakuTime   = 0;
          lastTalkMicroMoveTime     = 0;
          nextTalkMicroMoveInterval = random(800, 1800);
          if (soundBusy) {
            CoreS3.Speaker.stop();
            soundBusy = false;
          }
          if (sleepMode) {
            wakeUp("EAR_AUDIO");
          }
          lastInteractionTime = millis();
          earSpeakApplied++;
          addLog("EAR SPEAK_START handled lat=" + String(millis()));
        }
        // ハートビート（すでにexternalSpeaking=true）は時刻更新のみ（既存仕様と同じ）
      }
#endif
    } else if (strcmp(earRxLineBuf, "SPEAK_STOP") == 0) {
      earRxSpeakStop++;
#ifdef EAR_SPEAK_TO_MOUTH_ENABLED
      // LINE IN選択中のみ閉口（UDP選択中のMac発話をEarのSTOPで壊さない）
      if (audioSource == AUDIO_SRC_LINEIN && externalSpeaking) {
        externalSpeaking = false;
        mouthPakuOpen    = false;
        drawMouthClosed();
        resetTalkMicroMotion();
        lastExternalStopTime = millis();
        // 発話終了時点でsleepタイマーをリセット（UDPのSPEAK_STOP・stopExternalSpeaking()と対称）。
        // LINE INでも「発話終了後からスリープタイマーを開始する」挙動を適用する。
        lastInteractionTime  = millis();
        addLog("EAR EXTERNAL STOP lat=" + String(millis()));
      }
#endif
    } else {
      earRxBadLines++;
    }
  }

  // ---- 10秒毎の統計ログ（1行のみ） ----
  if (millis() - earLastStatsMs >= EAR_STATS_INTERVAL_MS) {
    earLastStatsMs = millis();
    unsigned long dLines = earRxLines - earPrevLines;
    unsigned long dFft   = earRxFft   - earPrevFft;
    earPrevLines = earRxLines;
    earPrevFft   = earRxFft;
    unsigned long ageS = earRxLastMs ? (millis() - earRxLastMs) / 1000UL : 9999;
    // 現在のFFTソース判定（表示用）：Ear優先→UDP鮮度→NONE
    const char* fftSrc = "NONE";
    if (audioSource == AUDIO_SRC_LINEIN && earFftFresh()) {
      fftSrc = "EAR";
    } else if (audioSource == AUDIO_SRC_UDP && lastFftPacketTime != 0 &&
               millis() - lastFftPacketTime <= FFT_RX_TIMEOUT_MS) {
      fftSrc = "UDP";
    }
    unsigned long dApply = earFftApplied - earPrevApplied;
    earPrevApplied = earFftApplied;
    addLog("EAR RX: lines=" + String(earRxLines) + "(+" + String(dLines) +
           ") fft=" + String(earRxFft) + "(+" + String(dFft) +
           ") spkS=" + String(earRxSpeakStart) +
           " spkE=" + String(earRxSpeakStop) +
           " bad=" + String(earRxBadLines) +
           " lastRx=" + String(ageS) + "s" +
           " src=" + String(fftSrc) +
           " apply=" + String(earFftApplied) + "(+" + String(dApply) + ")" +
           " spkApply=" + String(earSpeakApplied) +
           ((audioSource == AUDIO_SRC_LINEIN && externalSpeaking) ? " spk=EAR" : ""));
  }
}
#endif
// ===== KARIPOM EAR v2 ここまで =====

void updateExternalMouth() {
  // 音声入力OFF時のみ強制リセット。
  // UDP/内蔵マイク/LINE INのいずれかが有効なら、各ソース
  // （handleUDP / updateMicInput / 将来のLINE IN）が externalSpeaking を
  // 駆動し、ここは共通の口パク描画・微動・タイムアウト監視のみ担当する。
  // 内蔵マイクモードは有効中 lastSpeakPacketTime を更新し続けるため、
  // 下のSPEAK_TIMEOUT_MS監視をそのまま共用できる。
  if (audioSource == AUDIO_SRC_OFF) {
    // 発話中にソースOFFへ切り替えた場合、talk micro motionが
    // lrDetachScheduled=false のままLRをattach保持している可能性がある。
    // stopExternalSpeaking()を通らない経路のため、ここで通常の
    // 2秒AUTO DETACHを再スケジュールしてフラグ不整合を防ぐ。
    if (externalSpeaking && ENABLE_LR_SERVO && servoLR.attached()) {
      lrMarkMoved();
    }
    externalSpeaking = false;
    mouthPakuOpen = false;
    return;
  }

  if (!externalSpeaking) return;
  /*
  static unsigned long lastDebug = 0;

  if (millis() - lastDebug > 5000) {
    addLog("SPEAK age = " + String(millis() - lastExternalSpeakTime));
    lastDebug = millis();
  }
*/
  // PythonからのSPEAKパケットが途絶えたら強制停止する
  if (externalSpeaking && millis() - lastSpeakPacketTime > SPEAK_TIMEOUT_MS) {
    addLog("SPEAK PACKET TIMEOUT -> forced STOP");
    stopExternalSpeaking();
    return;
  }

  if (imageFaceMode) {
    addLog("MAC AUDIO WARNING: imageFaceMode ON, mouth drawing skipped");
    return;
  }

  // lastInteractionTime はここで更新しない。
  // sleep抑制は handleSleepTransition() で externalSpeaking をチェックして行う。
  // （updateExternalMouth は毎loop呼ばれるため、ここで更新するとハートビート同様に
  //   sleepタイマーが永遠にリセットされる）

  updateTalkMicroMotion();

  if (millis() - lastMouthPakuTime > 140) {
    lastMouthPakuTime = millis();

    if (mouthPakuOpen) {
      drawMouthClosed();
      mouthPakuOpen = false;
    } else {
      drawMouthOpen();
      mouthPakuOpen = true;
    }
  }
}

void updateTalkMicroMotion() {
  if (!ENABLE_IDLE_SERVO) return;
  if (servoBusy) return;
  if (sleepMode || alertMode || petMode) return;

  unsigned long now = millis();
  if (now - lastTalkMicroMoveTime < nextTalkMicroMoveInterval) return;

  // ── 上下サーボ（servoUD）：ランダムな軽い頷き ──────────────
  // 30%の確率で頷き、70%はセンターへ戻る。
  int targetUD;
  bool doNod = (random(0, 10) < 3);

  if (doNod) {
    targetUD = HEAD_VERTICAL_CENTER + random(1, TALK_MICRO_UD_RANGE + 1);
  } else {
    targetUD = HEAD_VERTICAL_CENTER;
  }
  targetUD = constrain(targetUD, SERVO_UD_DOWN, SERVO_UD_UP);

  // ── 左右サーボ（servoLR）：左右首振り ──────────────────────
  // 30%確率でのみ動かす。動かさない回はlrNowを維持し、write()を呼ばない。
  // シーソー構造換装後に毎回発動する仕様へ戻すこと。
  int targetLR;
  bool doLrShift = false;  // 比較試験：talk中LR微動を無効化（通常: random(0,10)<3 = 30%）

  if (doLrShift) {
    targetLR =
      HEAD_HORIZONTAL_CENTER + random(-TALK_MICRO_LR_RANGE, TALK_MICRO_LR_RANGE + 1);
    targetLR = constrain(
      targetLR,
      lastTalkTargetLR - TALK_MICRO_LR_RANGE,
      lastTalkTargetLR + TALK_MICRO_LR_RANGE);
    targetLR = constrain(targetLR, SERVO_LR_RIGHT, SERVO_LR_LEFT);
  } else {
    targetLR = lrNow;
  }

  lastTalkTargetUD   = targetUD;
  lastTalkTargetLR = (doLrShift ? targetLR : lastTalkTargetLR);

  servoBusy = true;
  moveSmooth(servoUD, udNow, targetUD, TALK_MICRO_MOVE_WAIT_MS);
  if (doLrShift) {
    lrMoveCaller = "talk";
    moveSmooth(servoLR, lrNow, targetLR, TALK_MICRO_MOVE_WAIT_MS);
    // moveSmooth() 内の lrMarkMoved() が detach をスケジュールするが、
    // 発話中は次のtalk motionまで数百ms〜数秒で再attachされ、その繰り返しが
    // AUTO DETACH連発の原因になる。発話中はスケジュールを解除し保持を継続する。
    // （発話終了時の stopExternalSpeaking()→resetTalkMicroMotion() でセンター復帰、
    //   その後の通常アイドル動作で改めて1回だけ detach される正常挙動に戻る）
    lrDetachScheduled = false;
  }
  servoBusy = false;

  lastTalkMicroMoveTime = millis();

  if (doNod) {
    // 頷いた後は少し長めに間を置く（連続頷きを防ぐ）
    nextTalkMicroMoveInterval = random(1200, 2500);
  } else {
    // 通常の間隔
    nextTalkMicroMoveInterval = random(600, 1200);
  }
}

void resetTalkMicroMotion() {
  if (!ENABLE_IDLE_SERVO) return;

  bool wasServoBusy = servoBusy;
  servoBusy = true;

  moveSmooth(servoUD, udNow,
             HEAD_VERTICAL_CENTER,
             TALK_MICRO_MOVE_WAIT_MS);

  lrMoveCaller = "reset";
  if (servoLR.attached()) {
    moveSmooth(servoLR, lrNow,
               HEAD_HORIZONTAL_CENTER,
               TALK_MICRO_MOVE_WAIT_MS);
  } else {
    // detach済みのためLRを触らない。
    // lrNowは維持（実機位置の最善推定値）。
    // 次のLR動作時に moveSmooth() が現在位置から正しくattachする。
    addLog("RESET LR SKIPPED (detach) lrNow=" + String(lrNow));
  }

  servoBusy = wasServoBusy;

  // 微動履歴リセット
  lastTalkTargetUD = HEAD_VERTICAL_CENTER;
  lastTalkTargetLR = HEAD_HORIZONTAL_CENTER;

  lastTalkMicroMoveTime = 0;
  nextTalkMicroMoveInterval = random(900, 2200);
}

void stopExternalSpeaking() {
  externalSpeaking = false;
  mouthPakuOpen = false;
  lastExternalStopTime = millis();
  // 発話終了時点でsleepタイマーをリセットする。
  // handleSleepTransition()はexternalSpeaking=falseの瞬間を素通りするため、
  // 「会話終了後15分経過→スリープ」を正しく実現するにはここでの更新が必要。
  lastInteractionTime  = millis();

  drawMouthClosed();
  resetTalkMicroMotion();
}

// ====================================================
// 🎙 音声入力フレームワーク実装
//
// 構造：
//   [ソース]                [変換]                 [共通データ]        [既存処理（無変更）]
//   UDP(Mac)      ─┐
//   内蔵マイク    ─┼─ updateAudioInput() ─→ audioLevel       ─→ updateExternalMouth()
//   LINE IN(将来) ─┘                        externalSpeaking     talk micro motion / 表情
//
// 新ソース（PCM1808 / Bluetooth Audio等）を追加する手順：
//   1. AudioSource にenum値を追加
//   2. updateXxxInput() を実装（audioLevel と externalSpeaking を駆動。
//      有効中は lastSpeakPacketTime を更新してwatchdogを共用する）
//   3. updateAudioInput() のswitchに1行追加
//   4. Web UI（/ ページの音声入力セクション）と /audio_src ハンドラに選択肢を追加
//   既存の口パク・微動・表情コードには一切手を入れない。
// ====================================================

const char* audioSourceName(AudioSource src) {
  switch (src) {
    case AUDIO_SRC_OFF:    return "OFF";
    case AUDIO_SRC_UDP:    return "UDP";
    case AUDIO_SRC_MIC:    return "MIC";
    case AUDIO_SRC_LINEIN: return "LINE_IN";
    default:               return "UNKNOWN";
  }
}

const char* characterStyleName(CharacterStyle s) {
  switch (s) {
    case CHARACTER_KARIPOM:      return "KariPom";
    case CHARACTER_MISS_KARIPOM: return "MissKariPom";
    default:                     return "UNKNOWN";
  }
}

// ====================================================
// 🎤 Current Audio Source（Home画面の簡易表示用）
// 複数台運用時に「今どの経路で口パクしているか」を一目で
// 分かるようにするための短いラベル。将来Bluetooth Audioを
// 追加する場合はcaseを1つ増やすだけでよい構造にしている。
// ====================================================
String audioSourceShortLabel(AudioSource src) {
  switch (src) {
    // 2026/07/20: 表示名のみ「UDP」→「PC音声（Wi-Fi）」。
    // 内部識別子（AUDIO_SRC_UDP）とログ表記（audioSourceName()の"UDP"）は不変。
    case AUDIO_SRC_UDP:    return "💻 PC音声（Wi-Fi）";
    case AUDIO_SRC_LINEIN: return "🎧 LINE IN";
    case AUDIO_SRC_MIC:    return "🐰 内蔵マイク";
    // case AUDIO_SRC_BLUETOOTH: return "🔵 Bluetooth";  // 将来追加予定
    default:               return "⭕ OFF";
  }
}

// ====================================================
// Audio Source 説明（Home画面「🐰 Karipom Ear」用）
// 「どのかりポムが制御されるのか」を複数台運用時にも
// 迷わないよう、経路ごとの制御対象を明記する。
// 将来Bluetooth Audioを追加する場合はcaseを1つ増やすだけで
// 並べられる構造にしている（今回はBluetooth自体は未実装）。
// ====================================================
String audioSourceDescriptionHtml(AudioSource src) {
  String html;
  switch (src) {
    case AUDIO_SRC_UDP:
      html += "<p style='margin:8px 0 4px;'><b>💻 PC音声（Wi-Fi）</b></p>";
      html += "<p class='note'>";
      html += "PCの再生音をWi-Fi経由で受け取り、口パクとグライコ表示に使用します。<br>";
      html += "Windows / macOS / Linuxに対応しています。<br><br>";
      html += "PCで動作している <b>karipom_talk.py</b> から<br>";
      html += "このかりポムへ送信された音声で口パクします。<br><br>";
      html += "制御対象は、<br>";
      html += "<b>karipom_talk.py</b> の送信先IPアドレスに設定された<br>";
      html += "かりポムです。";
      html += "</p>";
      break;

    case AUDIO_SRC_LINEIN:
      html += "<p style='margin:8px 0 4px;'><b>🎧 LINE IN (PCM1808)</b></p>";
      html += "<p class='note'>";
      html += "このかりポム本体に接続された<br><br>";
      html += "PCM1808<br>↓<br>Pico2<br>↓<br>UART<br><br>";
      html += "の音声で口パクします。<br><br>";
      html += "<b>karipom_talk.py</b> は使用しません。";
      html += "</p>";
      break;

    case AUDIO_SRC_MIC:
      html += "<p style='margin:8px 0 4px;'><b>🐰 内蔵マイク</b></p>";
      html += "<p class='note'>";
      html += "このかりポム本体のCoreS3内蔵マイクが拾った<br>";
      html += "周囲の音で口パクします。<br><br>";
      html += "<b>karipom_talk.py</b> は使用しません。";
      html += "</p>";
      break;

    // case AUDIO_SRC_BLUETOOTH:  // 将来追加予定
    //   html += "<p style='margin:8px 0 4px;'><b>🔵 Bluetooth (将来対応)</b></p>";
    //   break;

    default:  // AUDIO_SRC_OFF
      html += "<p style='margin:8px 0 4px;'><b>⭕ OFF</b></p>";
      html += "<p class='note'>音声入力を使用しません（口パク停止）。</p>";
      break;
  }
  return html;
}

// ====================================================
// 🐰 KariPom Name の表示用ヘルパー
// cfg_karipomName が空欄の場合は、複数台運用時にも個体を
// 判別できるようIPアドレス（AP時はSoftAPのIP）を代わりに表示する。
// ====================================================
String karipomDisplayName() {
  if (cfg_karipomName.length() > 0) return cfg_karipomName;
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (WiFi.getMode() == WIFI_AP)     return WiFi.softAPIP().toString();
  return "OFFLINE";
}

// 内蔵マイクモード中はスピーカー再生を全面禁止する。
// （I2S共有によるハウリング・自己音声の拾い込み防止。2026/06/07の音割れ実測に基づく）
// すべてのWAV再生・tone再生はこの関数で入口ゲートする。
// 独り言WAV再生時のみ pauseKaripomEarForMutter() で micEarPaused=true になっており、
// その間はスピーカー再生を許可する（それ以外のMIC中は従来通り禁止）。
bool isSpeakerAllowed() {
  if (audioSource != AUDIO_SRC_MIC) return true;  // MIC以外は常に許可
  return micEarPaused;  // MIC中は「一時停止中」のときだけ許可
}

// ============================================================
// 🐰 Karipom Ear：独り言WAV再生中のI2S切替（マイク⇔スピーカー）
//
// CoreS3はマイクとスピーカーが同一I2Sバスを共有するため、
// MICモード中にWAVを再生するには I2S を実際に切り替える必要がある。
//
// pauseKaripomEarForMutter():
//   Mic.end() → delay(50) → Speaker.begin() → micEarPaused=true
//   isSpeakerAllowed() が true になり、playWavFromSD が通るようになる。
//
// resumeKaripomEarAfterMutter():
//   micEarPaused を必ず false に戻す（audioSource 変化後も漏れなし）。
//   MICモードのままなら Speaker.end() → delay(50) → Mic.begin() でマイクへ戻す。
//   WAV再生中にソース切替があった場合（audioSource != AUDIO_SRC_MIC）は
//   I2S切替はスキップし、フラグのクリアのみ行う。
// ============================================================

void pauseKaripomEarForMutter() {
  micEarPaused = true;  // フラグを先に立てて isSpeakerAllowed() を通す

  if (audioSource != AUDIO_SRC_MIC) return;  // MICモード以外はI2S切替不要

  // MIC → SPEAKER へ I2S を切替
  CoreS3.Mic.end();
  delay(50);  // I2S解放の安定待ち（setAudioSource と同じ値）
  CoreS3.Speaker.begin();
  addLog("[EAR] paused for mutter: MIC END -> SPEAKER BEGIN");
}

void resumeKaripomEarAfterMutter() {
  if (!micEarPaused) return;  // 二重再開を防ぐ

  if (audioSource != AUDIO_SRC_MIC) {
    // WAV再生中にソースが切り替わっていた場合:
    // setAudioSource() 側がすでに I2S を処理済みなので、フラグのクリアのみ行う。
    micEarPaused = false;
    addLog("[EAR] resumed after mutter: src changed, flag cleared only");
    return;
  }

  // 通常復帰（MICモードのまま）:
  // I2S切替・レベルリセットをすべて完了させてから micEarPaused を下げる。
  // こうすることで、マイク復帰が完全に終わる前に updateMicInput() が
  // 走り込むのを防ぐ（micEarPaused ガードが切替中も有効であり続ける）。

  // SPEAKER → MIC へ I2S を切替
  CoreS3.Speaker.end();
  delay(50);  // I2S解放の安定待ち
  bool micOk = CoreS3.Mic.begin();

  // 残響由来のレベルをリセット
  micLevel = 0;
  micLastAboveTime = 0;

  // WAV残響・マイク再開直後ノイズによる MIC SPEAK START 誤発火を抑制
  suppressMicStart(5000, "mutter");

  // マイク復帰完了後にフラグを下げて解析を再開する
  micEarPaused = false;

  addLog(micOk ? "[EAR] resumed after mutter: SPEAKER END -> MIC BEGIN OK"
               : "[EAR] resumed after mutter: SPEAKER END -> MIC BEGIN FAILED");
}

// 共通音量取得。どのソースが選ばれていても 0〜1023目安 の共通スケールで返す。
// 現在の口パクは externalSpeaking の2値駆動だが、将来
// 「音量に応じた口の開き量」を実装する際はこの値を使う。
int getAudioLevel() {
  return audioLevel;
}

// 内蔵マイクのポーリング（AUDIO_SRC_MIC時のみ呼ばれる）。
// 8msぶん録音→平均絶対値→EMA平滑→ヒステリシスで発話状態を駆動する。
void updateMicInput() {
  if (micEarPaused) return;  // 独り言WAV再生中は解析を止める（pauseKaripomEarForMutter参照）
  if (!CoreS3.Mic.isEnabled()) return;

  // ── servoBusy中はMIC処理を丸ごと停止 ───────────────────────────────
  // サーボのギヤ音・筐体振動をMICが拾うのを根本から防ぐ。
  // servoBusy==true の間、およびfalseに戻った後MIC_SERVO_RESUME_DELAY_MS間は
  // EMA更新・録音・START判定をすべてスキップする。
  if (servoBusy) {
    if (!micServoPaused) {
      micServoPaused = true;
      if (externalSpeaking) {
        unsigned long sessionDuration = (micSessionStart > 0) ? millis() - micSessionStart : 0;
        addLog("MIC SPEAK STOP BY SERVO");
        addLog("MIC SESSION END duration=" + String(sessionDuration)
             + " peak=" + String(micSessionPeak) + " reason=servo");
        micSessionStart = 0;
        micSessionPeak  = 0;
        stopExternalSpeaking();
      }
      addLog("MIC PAUSED FOR SERVO");
    }
    micServoResumeAt = millis() + MIC_SERVO_RESUME_DELAY_MS;
    micOnFirstHitTime  = 0;   // 保留中の三段確認ヒットを破棄
    micOnSecondHitTime = 0;
    micWeakStartTime   = 0;   // WEAK帯継続判定もリセット
    micOnFirstHitLevel = 0;
    return;
  }
  if (millis() < micServoResumeAt) {
    // servoBusy解除直後の遅延期間（残響・振動の余韻を待つ）
    micOnFirstHitTime  = 0;
    micOnSecondHitTime = 0;
    micWeakStartTime   = 0;
    return;
  }
  if (micServoPaused) {
    micServoPaused = false;
    micLevel         = 0;   // 停止中に溜まったEMA残を捨てる
    micLastAboveTime = 0;
    addLog("MIC RESUME AFTER SERVO");
  }
  // ────────────────────────────────────────────────────────────────────

  static int16_t micBuf[MIC_REC_SAMPLES];

  // record()はDMA完了まで最大 ≒8ms ブロックする。
  // smartDelay等と同等のオーダーでありloop()側の許容範囲内。
  if (!CoreS3.Mic.record(micBuf, MIC_REC_SAMPLES, MIC_SAMPLE_RATE)) return;

  uint32_t sumAbs = 0;
  for (size_t i = 0; i < MIC_REC_SAMPLES; i++) {
    int v = micBuf[i];
    if (v < 0) v = -v;
    sumAbs += (uint32_t)v;
  }
  int mean = (int)(sumAbs / MIC_REC_SAMPLES);

  // EMA平滑（新値1/4）：瞬間ノイズでの口パク誤発火を防ぐ
  micLevel = (micLevel * 3 + mean) / 4;
  audioLevel = micLevel;

  // しきい値調整用ログ（2秒ごと）
  if (DEBUG_MIC_LEVEL) {
    static unsigned long lastMicLevelLog = 0;
    if (millis() - lastMicLevelLog > 2000) {
      lastMicLevelLog = millis();
      addLog("MIC LEVEL: " + String(micLevel)
           + " (ON_TH=" + String(cfg_micOnThreshold)
           + " OFF_TH=" + String(cfg_micOffThreshold) + ")");
    }
  }

  if (micLevel >= cfg_micOnThreshold) {
    micLastAboveTime = millis();

    if (!externalSpeaking) {
      unsigned long nowMs = millis();

      // ── (1) イベント後クールダウン（servo/mutter/detach/boot）──────────
      if (nowMs < micStartSuppressUntil) {
        static unsigned long lastSuppressLog = 0;
        if (nowMs - lastSuppressLog > 500) {
          lastSuppressLog = nowMs;
          addLog("MIC START SUPPRESSED reason=" + String(micStartSuppressReason)
               + " level=" + String(micLevel));
        }
        micWeakStartTime   = 0;  // 保留カウントをリセット
        micOnFirstHitTime  = 0;
        micOnSecondHitTime = 0;
        return;
      }

      // ── (2) スパイク除外：level >= MIC_SPIKE_LOG_LEVEL は即除外 ────────
      if (micLevel >= MIC_SPIKE_LOG_LEVEL) {
        addLog("MIC SPIKE IGNORED level=" + String(micLevel));
        micSpikeAt         = nowMs;  // クールダウン開始
        micWeakStartTime   = 0;
        micOnFirstHitTime  = 0;
        micOnSecondHitTime = 0;
        return;
      }

      // ── (3) スパイク後クールダウン ──────────────────────────────────────
      if (micSpikeAt != 0 && nowMs - micSpikeAt < MIC_SPIKE_COOLDOWN_MS) {
        static unsigned long lastSpikeCdLog = 0;
        if (nowMs - lastSpikeCdLog > 500) {
          lastSpikeCdLog = nowMs;
          addLog("MIC START SUPPRESSED reason=spike_cooldown level=" + String(micLevel));
        }
        micWeakStartTime   = 0;
        micOnFirstHitTime  = 0;
        micOnSecondHitTime = 0;
        return;
      }

      // ── (4) STRONG帯（level >= MIC_STRONG_THRESHOLD）：即START ──────────
      if (micLevel >= MIC_STRONG_THRESHOLD) {
        // STRONG帯は継続判定なしで即START
        micWeakStartTime   = 0;
        micOnFirstHitTime  = 0;
        micOnSecondHitTime = 0;
        goto mic_do_start;
      }

      // ── (5) WEAK帯（ON_TH <= level < MIC_STRONG_THRESHOLD）：継続判定 ───
      // MIC_WEAK_HOLD_MS の間 ON_TH 超えが続いた場合のみSTART。
      // 1回でも ON_TH を下回ったら保留をリセット。
      if (micWeakStartTime == 0) {
        // WEAK帯への入り
        micWeakStartTime = nowMs;
        addLog("MIC START PENDING level=" + String(micLevel));
        // 旧三段確認カウンタをリセット（WEAK判定と重複しないよう）
        micOnFirstHitTime  = 0;
        micOnSecondHitTime = 0;
        return;
      }
      {
        unsigned long weakDuration = nowMs - micWeakStartTime;
        if (weakDuration < MIC_WEAK_HOLD_MS) {
          return;  // まだ継続時間が足りない
        }
        // WEAK帯で MIC_WEAK_HOLD_MS 継続 → START
        addLog("MIC START CONFIRMED level=" + String(micLevel)
             + " duration=" + String(weakDuration));
        micWeakStartTime   = 0;
        micOnFirstHitTime  = 0;
        micOnSecondHitTime = 0;
      }

      mic_do_start:
      // UDPのSPEAK_START相当（handleUDPと同じ初期化を行う）
      externalSpeaking          = true;
      mouthPakuOpen             = false;
      lastMouthPakuTime         = 0;
      lastTalkMicroMoveTime     = 0;
      nextTalkMicroMoveInterval = random(800, 1800);

      if (sleepMode) {
        wakeUp("MIC_AUDIO");
      }

      lastInteractionTime = millis();

      // MICセッション開始
      micSessionStart = millis();
      micSessionPeak  = micLevel;
      addLog("MIC SESSION START level=" + String(micLevel));
      addLog("MIC SPEAK START level=" + String(micLevel));
    }

    // 有効中はUDPハートビート相当としてwatchdogを更新
    lastSpeakPacketTime   = millis();
    lastExternalSpeakTime = millis();

  } else {
    // micLevel < cfg_micOnThreshold：WEAK帯の保留をリセット
    if (micWeakStartTime != 0) {
      unsigned long weakDuration = millis() - micWeakStartTime;
      addLog("MIC START SUPPRESSED reason=weak_short level=" + String(micLevel)
           + " duration=" + String(weakDuration));
      micWeakStartTime = 0;
    }
    micOnFirstHitTime  = 0;
    micOnSecondHitTime = 0;

  } if (externalSpeaking) {
    // セッション中のピーク更新
    if (micLevel > micSessionPeak) micSessionPeak = micLevel;

    if (micLevel < cfg_micOffThreshold &&
        millis() - micLastAboveTime > cfg_micReleaseHoldMs) {
      // UDPのSPEAK_STOP相当
      unsigned long sessionDuration = (micSessionStart > 0) ? millis() - micSessionStart : 0;
      addLog("MIC SPEAK STOP level=" + String(micLevel));
      addLog("MIC SESSION END duration=" + String(sessionDuration)
           + " peak=" + String(micSessionPeak) + " reason=silence_hold");
      micSessionStart = 0;
      micSessionPeak  = 0;
      stopExternalSpeaking();
    } else {
      // ON/OFFしきい値の中間帯：発話継続扱いでwatchdogのみ更新
      lastSpeakPacketTime = millis();
    }
  }
}

// 毎loop呼ばれるソースディスパッチャ（handleCommunicationから呼ぶ）。
// どのソースであっても最終的に audioLevel / externalSpeaking へ変換する。
void updateAudioInput() {
  switch (audioSource) {
    case AUDIO_SRC_UDP:
      // UDPはhandleUDP()がexternalSpeakingを駆動する。
      // audioLevelは2値表現（将来Pythonから音量値が来たら置き換え）。
      audioLevel = externalSpeaking ? AUDIO_LEVEL_UDP_ACTIVE : 0;
      break;

    case AUDIO_SRC_MIC:
      updateMicInput();
      break;

    case AUDIO_SRC_LINEIN:
      // KARIPOM EAR v2 (Step 4.1)：LINE IN＝Karipom Ear（PCM1808+Pico/Port C）。
      // externalSpeaking は handleEarUart()（Ear SPEAK、Step 5で接続）が駆動する。
      // audioLevelはUDPと同じ2値表現（Ear側から音量値が来たら置き換え）。
      audioLevel = externalSpeaking ? AUDIO_LEVEL_UDP_ACTIVE : 0;
      break;

    default:  // AUDIO_SRC_OFF
      audioLevel = 0;
      break;
  }
}

// 音声入力ソースの切替。Webハンドラ・起動時復元から呼ばれる。
// I2S切替（CoreS3はマイクとスピーカーが同一I2Sバスを共有）を安全な順序で行う。
void setAudioSource(AudioSource src, const char* reason) {
  if (src == audioSource) {
    // 同一ソース再選択はセッションリセット扱い（旧 /mac_audio_on の挙動を踏襲）
    stopExternalSpeaking();
    lastMutterPlayedTime = millis();
    addLog("AUDIO SRC RESELECT: " + String(audioSourceName(src))
         + " (session reset) [" + String(reason) + "]");
    return;
  }

  AudioSource prev = audioSource;

  // 進行中のスピーカー再生・発話を停止。
  // ※本関数はplayWavFromSD内のserver.handleClient()経由で
  //   再生ループ中に呼ばれる可能性がある。Speaker.stop()により
  //   isPlaying()=falseとなり、再生ループは安全に脱出する。
  if (soundBusy) {
    CoreS3.Speaker.stop();
    soundBusy = false;
    addLog("AUDIO SRC SWITCH: local WAV stopped");
  }
  stopExternalSpeaking();

  // 独り言mutter pause中にソース切替が発生した場合のフラグクリア。
  // resumeKaripomEarAfterMutter() は audioSource 変化後も必ず false に戻すが、
  // setAudioSource 側でもクリアしておくことで二重の安全網とする。
  if (micEarPaused) {
    micEarPaused = false;
    addLog("AUDIO SRC SWITCH: micEarPaused cleared");
  }

  // ── I2S切替：マイクモードから抜ける場合 → Mic停止→Speaker復帰 ──
  if (prev == AUDIO_SRC_MIC) {
    CoreS3.Mic.end();
    delay(50);  // I2S解放の安定待ち（必須）
    CoreS3.Speaker.begin();
    addLog("MIC END -> SPEAKER BEGIN");
  }

  audioSource = src;

  // 既存コードとの互換：UDP系ゲート（handleUDPのSPEAK受付、
  // WAV再生中のMac音声優先割込み判定）はこのフラグを参照し続ける。
  macAudioLinkEnabled = (src == AUDIO_SRC_UDP);

  // ── I2S切替：マイクモードへ入る場合 → Speaker停止→Mic開始 ──
  if (src == AUDIO_SRC_MIC) {
    CoreS3.Speaker.end();
    delay(50);  // I2S解放の安定待ち（必須）
    bool micOk = CoreS3.Mic.begin();
    micLevel = 0;
    micLastAboveTime = 0;
    // MIC開始直後のスパイク（BOOT_RESTORE含む）でSTARTしないよう抑制
    // EMA更新・STOP判定は通常どおり継続される
    suppressMicStart(5000, "boot");
    addLog(micOk ? "SPEAKER END -> MIC BEGIN OK" : "MIC BEGIN FAILED");
  }

  audioLevel = 0;

  // 切替直後に独り言を挟まない（旧 /mac_audio_on と同じ配慮）
  lastMutterPlayedTime = millis();

  addLog("AUDIO SRC: " + String(audioSourceName(prev))
       + " -> " + String(audioSourceName(audioSource))
       + " [" + String(reason) + "]");
}

// NVSに保存された音声入力ソースを起動完了後に適用する。
// setup()末尾（online_ready.wav再生後）から呼ぶことで、
// 起動確認音は選択ソースに関係なく必ず鳴る。
void applyBootAudioSource() {
  AudioSource saved = (AudioSource)cfg_bootAudioSource;
  if (saved > AUDIO_SRC_LINEIN) {
    // NVS異常値ガード
    addLog("AUDIO SRC RESTORE: invalid value " + String(cfg_bootAudioSource) + " -> UDP");
    saved = AUDIO_SRC_UDP;
  }

  if (saved != audioSource) {
    setAudioSource(saved, "BOOT_RESTORE");
  } else {
    addLog("AUDIO SRC: " + String(audioSourceName(audioSource)) + " (default)");
  }
}

// ===== SD Web Config Helpers =====
const char* WEB_CONFIG_PATH = "/config.txt";
// WEB_CONFIG_PATH は System Config ページの wifi.txt 編集などで引き続き使用。
// cfg_ フラグの永続化は NVS（Preferences）を使うため、
// config.txt への cfg_ フラグの読み書きは廃止。

String htmlEscape(String text) {
  text.replace("&", "&amp;");
  text.replace("<", "&lt;");
  text.replace(">", "&gt;");
  text.replace("\"", "&quot;");
  return text;
}

String readTextFileFromSD(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return "";

  String text = "";
  while (f.available()) {
    text += (char)f.read();
    if (text.length() > 12000) break;  // 念のため巨大ファイルを避ける
  }
  f.close();
  return text;
}

bool writeTextFileToSD(const char* path, const String& text) {
  if (SD.exists(path)) {
    SD.remove(path);  // FILE_WRITEは追記になりやすいので先に消す
  }

  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;

  size_t written = f.print(text);
  f.close();
  return written == text.length();
}

// ====================================================
// Webハンドラ処理時間プロファイル用ヘルパー
// 各ハンドラ先頭で webLogStart("/route")、送信直前で webLogEnd("/route", t0)。
// 処理時間ms・free heap・min free heap をログに出し、
// 「どのWebページ生成が重くて loop() をブロックしているか」を特定する。
// ====================================================
unsigned long webLogStart(const char* name) {
  addLog("WEB HANDLER START " + String(name));
  return millis();
}
void webLogEnd(const char* name, unsigned long t0) {
  char buf[160];
  snprintf(buf, sizeof(buf),
           "WEB HANDLER END %s ms=%lu free=%u minFree=%u",
           name,
           (unsigned long)(millis() - t0),
           (unsigned)ESP.getFreeHeap(),
           (unsigned)ESP.getMinFreeHeap());
  addLog(buf);
}

// ====================================================
// ログページ（ページング＋チャンク送信版）
//
// 目的：巨大ログを一括HTML化せず、指定ページ分の行だけを逐次送信する。
//   ・全件をStringに溜めない（server.sendContent で1行ずつ送る）
//   ・初期表示は最新 linesPerPage 行（page=1 が最新ページ）
//   ・page / lines パラメータでページ送り
//   ・RAM使用量はほぼ一定（1行分のバッファのみ）
//
// 行の数え方：
//   pass1 で総行数を数える（バイト走査、Stringを作らない）
//   → 「最新から」ページングするため、表示範囲を末尾基準で算出
//   → pass2 で対象範囲の行だけを送信
// ====================================================

// ログファイルの総行数を数える（'\n'カウント、最終行に改行が無くてもカウント）
long countFileLines(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;

  long lines = 0;
  bool sawAny = false;
  bool lastWasNL = true;
  const size_t BUFSZ = 512;
  uint8_t buf[BUFSZ];

  while (f.available()) {
    int n = f.read(buf, BUFSZ);
    for (int i = 0; i < n; i++) {
      sawAny = true;
      if (buf[i] == '\n') { lines++; lastWasNL = true; }
      else { lastWasNL = false; }
    }
  }
  f.close();

  // 末尾が改行で終わっていない最終行も1行として数える
  if (sawAny && !lastWasNL) lines++;
  return lines;
}

// 1行をHTMLエスケープしつつ<div>で送る（大きなStringを作らない）
static void sendLogLine(const String& line) {
  String safe = htmlEscape(line);
  server.sendContent("<div class='logline'>" + safe + "</div>");
}

void streamLogPage(const char* path, const char* label, int page, int linesPerPage,
                   long& outTotalLines, int& outShownLines, int& outTotalPages) {
  if (linesPerPage < 10)  linesPerPage = 10;
  if (linesPerPage > 500) linesPerPage = 500;
  if (page < 1) page = 1;

  long totalLines = countFileLines(path);
  int totalPages = (totalLines <= 0) ? 1 : (int)((totalLines + linesPerPage - 1) / linesPerPage);
  if (page > totalPages) page = totalPages;

  // 最新ページ(page=1)が末尾になるよう、末尾から数えた範囲を算出。
  // 表示範囲の行インデックス（0始まり, ファイル先頭から）：
  //   endLine   = totalLines - (page-1)*linesPerPage   (この手前まで)
  //   startLine = endLine - linesPerPage
  long endLine = totalLines - (long)(page - 1) * linesPerPage;
  long startLine = endLine - linesPerPage;
  if (startLine < 0) startLine = 0;
  if (endLine < 0) endLine = 0;

  // --- ヘッダ ---
  server.sendContent(karipomPageHeader("KariPom Lab – Log View"));
  server.sendContent("<h1>💩 Log View: " + String(label) + "</h1>");
  server.sendContent("<p><a href='/logtoilet'>🚽 Log Toilet</a> / <a href='/'>🏠 Lab Home</a></p>");

  // ナビ（上）
  {
    String fileArg = server.hasArg("file") ? server.arg("file") : "current";
    String base = "/logview?file=" + fileArg + "&lines=" + String(linesPerPage);
    String nav = "<p>";
    // newer = 末尾に近い = page小さい
    if (page > 1)          nav += "<a href='" + base + "&page=" + String(page - 1) + "'><button>⬅️ 新しい</button></a> ";
    nav += "<b>" + String(page) + " / " + String(totalPages) + " ページ</b> ";
    if (page < totalPages) nav += "<a href='" + base + "&page=" + String(page + 1) + "'><button>古い ➡️</button></a>";
    nav += "</p>";
    server.sendContent(nav);
    server.sendContent("<p class='note'>1ページ " + String(linesPerPage) +
                       " 行・最新ページが「1 ページ」です（下ほど新しいログ）。全 " +
                       String(totalLines) + " 行</p>");
  }

  server.sendContent("<div class='logbox'>");

  // --- pass2：対象範囲の行だけ送信 ---
  int shown = 0;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    server.sendContent("<div class='logline'>(ログを開けません: " + String(path) + ")</div>");
  } else {
    long lineIdx = 0;
    String cur = "";
    cur.reserve(SD_LOG_LINE_MAX + 8);
    const size_t BUFSZ = 512;
    uint8_t buf[BUFSZ];
    bool reachedEnd = false;

    while (f.available() && !reachedEnd) {
      int n = f.read(buf, BUFSZ);
      for (int i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\n') {
          if (lineIdx >= startLine && lineIdx < endLine) {
            sendLogLine(cur);
            shown++;
          }
          cur = "";
          lineIdx++;
          // 対象範囲を過ぎたら早期終了（残りは読まない）
          if (lineIdx >= endLine) { reachedEnd = true; break; }
        } else if (c != '\r') {
          if (cur.length() < 400) cur += c;  // 1行の暴走防止
        }
      }
    }
    // 末尾（改行なし最終行）
    if (!reachedEnd && cur.length() > 0 && lineIdx >= startLine && lineIdx < endLine) {
      sendLogLine(cur);
      shown++;
    }
    f.close();
  }

  if (shown == 0) {
    server.sendContent("<div class='logline'>(表示する行がありません)</div>");
  }

  server.sendContent("</div>");

  // ナビ（下）
  {
    String fileArg = server.hasArg("file") ? server.arg("file") : "current";
    String base = "/logview?file=" + fileArg + "&lines=" + String(linesPerPage);
    String nav = "<p>";
    if (page > 1)          nav += "<a href='" + base + "&page=" + String(page - 1) + "'><button>⬅️ 新しい</button></a> ";
    nav += "<b>" + String(page) + " / " + String(totalPages) + " ページ</b> ";
    if (page < totalPages) nav += "<a href='" + base + "&page=" + String(page + 1) + "'><button>古い ➡️</button></a>";
    nav += "</p>";
    server.sendContent(nav);
  }

  server.sendContent(karipomPageFooter());

  outTotalLines = totalLines;
  outShownLines = shown;
  outTotalPages = totalPages;
}

// streamLogPage をチャンク送信で返し、生成時間/行数/ページ/heapをデバッグログへ出す
void sendLogPage(const char* path, const char* label, int page, int linesPerPage) {
  unsigned long t0 = millis();
  addLog("LOG PAGE START");

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=UTF-8", "");

  long totalLines = 0;
  int shownLines = 0;
  int totalPages = 1;
  streamLogPage(path, label, page, linesPerPage, totalLines, shownLines, totalPages);

  server.sendContent("");  // チャンク終端

  addLog("LOG PAGE END shown=" + String(shownLines) +
         " page=" + String(page) + "/" + String(totalPages) +
         " total=" + String(totalLines) +
         " ms=" + String(millis() - t0) +
         " freeHeap=" + String(ESP.getFreeHeap()));
}

bool deleteFileFromSD(const char* path) {
  if (!SD.exists(path)) return true;
  return SD.remove(path);
}


// ===== WAV Upload/Delete Helpers =====
File soundUploadFile;
String soundUploadPath = "";
bool soundUploadOk = false;

// mutter専用アップロード状態（/sounds/mutter/ 配下・通常サウンドとは分離）
File mutterUploadFile;
String mutterUploadPath = "";
bool mutterUploadOk = false;

String urlEncode(const String& src) {
  String encoded = "";
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < src.length(); i++) {
    char c = src.charAt(i);
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

String sanitizeFilename(String name) {
  name.replace("\\", "/");
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);
  name.trim();

  String clean = "";
  for (size_t i = 0; i < name.length(); i++) {
    char c = name.charAt(i);
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') || c == '_' || c == '-' || c == '.') {
      clean += c;
    }
  }

  if (clean.length() == 0) return "";
  if (clean.indexOf("..") >= 0) return "";
  return clean;
}

bool isWavFilename(const String& name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".wav");
}

String karipomPageHeader(const String& title) {
  String html;
  html.reserve(3200);
  html += "<html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>";
  html += title;
  html += "</title>";
  // ===== KariPom Lab – 共通テーマ CSS =====
  html += "<style>";
  html += ":root{--bg:#0d0f12;--card:#14181e;--text:#cbd5e1;--accent:#38bdf8;"
          "--led-ok:#4ade80;--led-err:#ef4444;--led-warn:#facc15;"
          "--lcd-bg:#072216;--lcd-fg:#22c55e;--border:#252d3a}";
  html += "body{font-family:-apple-system,BlinkMacSystemFont,'Hiragino Sans','Yu Gothic',sans-serif;"
          "color:var(--text);background:var(--bg);padding:12px;line-height:1.6}";
  html += ".panel{background:var(--card);border-radius:6px;padding:16px;"
          "box-shadow:0 0 0 1px var(--border),inset 0 1px 0 rgba(255,255,255,.04);"
          "max-width:700px;margin:0 auto}";
  html += "h1,h2,h3{color:var(--accent);letter-spacing:.04em;"
          "border-bottom:1px solid var(--border);padding-bottom:4px;margin-top:18px}";
  html += "button{font-size:15px;padding:8px 14px;margin:3px;border-radius:3px;"
          "border:1px solid #2a3348;background:#1c2230;color:var(--text);"
          "box-shadow:0 2px 4px rgba(0,0,0,.5),inset 0 1px 0 rgba(255,255,255,.05);cursor:pointer}";
  html += "button:active{box-shadow:inset 0 2px 4px rgba(0,0,0,.6);transform:translateY(1px)}"
          "button:hover{background:#263040;border-color:#3a4d68}"
          "button:disabled,button[disabled]{opacity:.35;cursor:not-allowed;box-shadow:none;transform:none}"
          ".save:hover,.onbtn:hover{background:#193325;border-color:#2e7a48}"
          ".del:hover,.offbtn:hover{background:#2e1111;border-color:#7a2525}"
          ".play:hover{background:#152535;border-color:#1e5575}";
  html += ".note{font-size:13px;color:#4e5f72;line-height:1.6}";
  html += ".msg{padding:10px;background:#0c1016;border:1px solid var(--accent);"
          "border-radius:4px;font-size:16px;color:var(--accent)}";
  html += ".box,.upload{border:1px solid var(--border);border-radius:4px;"
          "padding:12px;background:#0c1016;margin:12px 0}";
  html += ".lab-card{background:#0c1016;border-radius:4px;padding:14px;margin:12px 0}";
  html += ".save,.onbtn{background:#0d2218;border-color:#1f5c35;color:var(--led-ok)}";
  html += ".del,.offbtn{background:#220d0d;border-color:#5c1a1a;color:var(--led-err)}";
  html += ".play{background:#0d1a2a;border-color:#1a4060;color:var(--accent)}";
  html += "textarea{width:96%;max-width:760px;height:300px;font-size:14px;"
          "background:#0c1016;border:1px solid var(--border);color:var(--text);border-radius:3px}";
  html += "a{color:var(--accent);text-decoration:none}";
  html += "a:hover{text-decoration:underline}";
  html += "li{margin:8px 0}";
  html += "hr{border:none;border-top:1px solid var(--border);margin:14px 0}";
  html += "pre{background:#0c1016;border:1px solid var(--border);border-radius:4px;"
          "padding:12px;overflow:auto;color:var(--lcd-fg);font-family:monospace}";
  html += ".logbox{background:var(--lcd-bg);color:var(--lcd-fg);border-radius:4px;"
          "padding:10px;font-family:monospace;font-size:13px;line-height:1.4;"
          "overflow:auto;max-height:70vh}";
  html += ".logline{white-space:pre-wrap;word-break:break-all;"
          "border-bottom:1px solid #0a1a0a;padding:1px 0}";
  html += "input[type=number],input[type=text],input[type=password],select{"
          "background:#0c1016;border:1px solid var(--border);color:var(--text);"
          "border-radius:3px;padding:4px}";
  html += "details summary{color:var(--accent);cursor:pointer;font-weight:bold}";
  html += "</style></head><body><div class='panel'>";
  // ===== End KariPom Lab CSS =====
  return html;
}

String karipomPageFooter() {
  return "</div></body></html>";
}

String soundsPageHtml(const String& message = "") {
  File root = SD.open("/sounds/");
  if (!root) root = SD.open("/sounds");

  String html = karipomPageHeader("KariPom Lab – Sounds");
  html += "<h1>🎵 KariPom Sound Gallery</h1>";
  html += "<p>";
  // 2026/07/21: 最上部ナビを共通4項目へ統一（Mutter Managerへの導線は下の note に残す）
  html += "<a href='/'>🏠 Lab Home</a> / ";
  html += "<a href='/config'>⚙️ Config</a> / ";
  html += "<a href='/files'>📂 File Manager</a> / ";
  html += "<a href='/logtoilet'>🚽 Log Toilet</a>";
  html += "</p>";
  html += "<p class='note'>💬 ひとこと（mutter）音声は別ページ「Mutter Manager」で管理します。ここには通常サウンド（効果音・システム音）だけを置いてください。</p>";

  if (message.length() > 0) {
    html += "<p class='msg'>";
    html += htmlEscape(message);
    html += "</p>";
  }

  html += "<div class='upload'>";
  html += "<h2>WAV Upload</h2>";
  html += "<form method='POST' action='/upload_sound' enctype='multipart/form-data' id='uploadForm'>";
  html += "<input type='file' id='fileInput' name='file' accept='.wav,audio/wav' style='display:none;' onchange='document.getElementById(\"uploadForm\").submit();'>";
  html += "<button type='button' onclick='document.getElementById(\"fileInput\").click();' style='font-size:18px;margin-right:8px;'>📂 ファイルを選択</button>";
  html += "<button type='submit' style='font-size:18px;'>⬆️ UPLOAD</button>";
  html += "</form>";
  html += "<p class='note'>保存先: <b>/sounds/ファイル名.wav</b>（同名ファイルは上書き）</p>";
  html += "</div>";

  html += "<h2>/sounds</h2>";
  html += "<ul>";

  int listedCount = 0;

  if (!root) {
    html += "<li>/sounds フォルダを開けません。SDカードを確認してください。</li>";
  } else {
    File file = root.openNextFile();
    while (file) {
      String name = String(file.name());
      String displayName = name;
      displayName.replace("/sounds/", "");
      displayName.replace("sounds/", "");

      // mutter関連は通常一覧に出さない（Mutter Managerで管理）：
      //  ・mutter サブディレクトリはスキップ
      //  ・旧形式 mutter_***.wav もスキップ（件数カウントもしない＝軽量化）
      bool isMutterRelated =
        displayName.startsWith("mutter_") && isWavFilename(displayName);

      if (file.isDirectory()) {
        // /sounds/mutter などのサブフォルダはスキップ
      } else if (!displayName.startsWith(".") && !isMutterRelated) {
        String encoded = urlEncode(displayName);
        html += "<li>";
        html += htmlEscape(displayName);
        html += " <a href='/soundfile?name=" + encoded + "' target='_blank'><button class='play'>🎵 Listen</button></a>";
        html += " <a href='/speak?name=" + encoded + "'><button>🐰 Speak</button></a>";
        html += " <a href='/confirm_delete_sound?name=" + encoded + "'><button class='del'>DELETE / 削除</button></a>";
        html += "</li>";
        listedCount++;
      }
      file = root.openNextFile();
    }

    root.close();
  }

  if (listedCount == 0) {
    html += "<li>通常サウンドWAVが見つかりません。</li>";
  }

  html += "</ul>";
  html += "<p class='note'>Listed 通常サウンド: ";
  html += String(listedCount);
  html += "</p>";
  html += "<p class='note'>💬 ひとこと（mutter）音声の管理は <a href='/mutter'>Mutter Manager</a> へ。</p>";

  html += karipomPageFooter();
  return html;
}

// ====================================================
// streamMutterPage()
// 「ひとこと（mutter）」専用管理ページ（ページング＋ストリーミング送信版）。
//
// 高速化の要点：
//  1. 全件を巨大な String に連結せず、server.sendContent() で逐次送信する
//     （ファイルが200〜300件でもRAM消費が一定。ヒープ断片化を防ぐ）
//  2. 1ページ MUTTER_PER_PAGE 件だけ表示（page=0,1,2,...）
//  3. 旧形式 /sounds/mutter_***.wav の全走査はこのページでは行わない
//     （件数チェックは別ボタン「旧形式を確認」= /mutter_legacy に分離）
//
// 呼び出し側で server.setContentLength(CONTENT_LENGTH_UNKNOWN) と
// server.send(200, ...) を済ませてから本関数を呼ぶこと。
// ====================================================
const int MUTTER_PER_PAGE = 50;

void streamMutterPage(int page, const String& message, int& outShownCount, bool& outHasNext) {
  if (page < 0) page = 0;

  // フォルダが無ければ作成（初回アクセスで自動生成）
  if (!SD.exists("/sounds")) SD.mkdir("/sounds");
  if (!SD.exists("/sounds/mutter")) SD.mkdir("/sounds/mutter");

  int startIdx = page * MUTTER_PER_PAGE;
  int endIdx   = startIdx + MUTTER_PER_PAGE;  // この手前まで表示

  // --- ヘッダ ---
  server.sendContent(karipomPageHeader("KariPom Lab – Mutter Manager"));
  server.sendContent("<h1>💬 ひとこと管理 (Mutter Manager)</h1>");
  // 2026/07/21: 最上部ナビを共通4項目へ統一（Sound Managerへの導線は下の note に残す）
  server.sendContent("<p><a href='/'>🏠 Lab Home</a> / <a href='/config'>⚙️ Config</a> / "
                     "<a href='/files'>📂 File Manager</a> / <a href='/logtoilet'>🚽 Log Toilet</a></p>");
  server.sendContent("<p class='note'>🎵 通常サウンド（効果音・システム音）の管理は <a href='/sounds'>Sound Manager</a> へ。</p>");

  server.sendContent("<p class='note'>💬 ここは「ひとこと（mutter）」音声の専用ページです。<br>"
                     "ファイル名は自由につけられます（例: <b>ohayou.wav</b> / <b>coffee_time.wav</b>）。<br>"
                     "保存先は <b>/sounds/mutter/</b> です。連番は不要で、追加・削除してもコード変更は要りません。</p>"
                     "<div class='box' style='background:#1a1200;border-left:4px solid var(--led-warn);padding:8px 12px;margin:8px 0;color:var(--text);'>"
                     "<b style='color:var(--led-warn);'>⚠️ ファイル登録に関する制限</b><ul style='margin:6px 0 0 0;padding-left:1.4em;'>"
                     "<li>登録できるファイルは最大 <b>400 件</b>まで起動時にRAMへキャッシュされます。<br>"
                       "超過分はランダム再生の対象外になります（SDには残りますが再生されません）。</li>"
                     "<li>ファイル名は <b>39文字以内</b>（拡張子 .wav を含む）にしてください。<br>"
                       "超過した場合、ファイル名が途中で切り捨てられ正常に再生されないことがあります。</li>"
                     "<li>ファイルの実体はSDカードに保存されます。SDカードの空き容量にご注意ください。</li>"
                     "<li>このページからのアップロード・削除では、キャッシュは自動で再構築されます。<br>"
                       "PCなどでSDカードを直接編集した場合は、かりポムを再起動してください。</li>"
                     "</ul></div>");

  if (message.length() > 0) {
    server.sendContent("<p class='msg'>" + htmlEscape(message) + "</p>");
  }

  // --- 発生確率設定 ---
  {
    auto sel = [](int a, int b) { return a == b ? " selected" : ""; };
    String s =
      "<div class='box'><h2>⚙️ 発生確率設定</h2>"
      "<form method='GET' action='/mutter_cfg'>"
      "<p>"
      "<label>💬 呟き発生確率：</label>"
      "<select name='chance'>"
      "<option value='0'"  + String(sel(cfg_mutterChance,  0)) + ">0%（OFF）</option>"
      "<option value='5'"  + String(sel(cfg_mutterChance,  5)) + ">5%</option>"
      "<option value='10'" + String(sel(cfg_mutterChance, 10)) + ">10%（標準）</option>"
      "<option value='20'" + String(sel(cfg_mutterChance, 20)) + ">20%</option>"
      "<option value='30'" + String(sel(cfg_mutterChance, 30)) + ">30%</option>"
      "</select>"
      " &nbsp; "
      "<label>😲 顔出現確率：</label>"
      "<select name='face'>"
      "<option value='0'"   + String(sel(cfg_mutterFaceChance,   0)) + ">0%</option>"
      "<option value='10'"  + String(sel(cfg_mutterFaceChance,  10)) + ">10%</option>"
      "<option value='20'"  + String(sel(cfg_mutterFaceChance,  20)) + ">20%（標準）</option>"
      "<option value='25'"  + String(sel(cfg_mutterFaceChance,  25)) + ">25%</option>"
      "<option value='50'"  + String(sel(cfg_mutterFaceChance,  50)) + ">50%</option>"
      "<option value='100'" + String(sel(cfg_mutterFaceChance, 100)) + ">100%</option>"
      "</select>"
      " &nbsp; "
      "<button type='submit'>💾 保存</button>"
      "</p></form>"
      "<p class='note'>呟き発生確率はジョイスティック・手動再生には影響しません。</p>"
      "</div>";
    server.sendContent(s);
  }

  // --- アップロード ---
  server.sendContent(
    "<div class='upload'><h2>WAV Upload</h2>"
    "<form method='POST' action='/upload_mutter' enctype='multipart/form-data' id='uploadForm'>"
    "<input type='file' id='fileInput' name='file' accept='.wav,audio/wav' style='display:none;' "
    "onchange='document.getElementById(\"uploadForm\").submit();'>"
    "<button type='button' onclick='document.getElementById(\"fileInput\").click();' "
    "style='font-size:18px;margin-right:8px;'>📂 ファイルを選択</button>"
    "<button type='submit' style='font-size:18px;'>⬆️ UPLOAD</button></form>"
    "<p class='note'>保存先: <b>/sounds/mutter/ファイル名.wav</b>（同名ファイルは上書き）</p></div>");

  // --- 一覧（このページ範囲だけ描画） ---
  server.sendContent("<h2>/sounds/mutter</h2><ul>");

  File root = SD.open("/sounds/mutter");
  int matchIdx = 0;      // .wav の通し番号（全体）
  int shownCount = 0;    // このページで実際に描画した件数
  bool hasNext = false;  // 次ページの有無

  if (!root) {
    server.sendContent("<li>/sounds/mutter フォルダを開けません。SDカードを確認してください。</li>");
  } else {
    File file = root.openNextFile();
    while (file) {
      bool isWav = false;
      String displayName;
      if (!file.isDirectory()) {
        displayName = String(file.name());
        int slash = displayName.lastIndexOf('/');
        if (slash >= 0) displayName = displayName.substring(slash + 1);
        if (!displayName.startsWith(".") && isWavFilename(displayName)) isWav = true;
      }

      if (isWav) {
        if (matchIdx >= startIdx && matchIdx < endIdx) {
          String encoded = urlEncode(displayName);
          // 1件ずつ送信（大きなStringを作らない）
          String li = "<li>";
          li += htmlEscape(displayName);
          li += " <a href='/mutterfile?name=" + encoded + "' target='_blank'><button class='play'>🎵 Listen</button></a>";
          li += " <a href='/speak_mutter?name=" + encoded + "'><button>🐰 Speak</button></a>";
          li += " <a href='/confirm_delete_mutter?name=" + encoded + "'><button class='del'>DELETE / 削除</button></a>";
          li += "</li>";
          server.sendContent(li);
          shownCount++;
        } else if (matchIdx >= endIdx) {
          // このページ範囲を超えた .wav が1件でもあれば次ページあり
          hasNext = true;
          break;  // これ以上は数えなくてよい（早期終了で高速化）
        }
        matchIdx++;
      }
      file = root.openNextFile();
    }
    root.close();
  }

  if (shownCount == 0) {
    if (page == 0) {
      server.sendContent("<li>mutter WAVファイルがまだありません。上のUPLOADから追加してください。</li>");
    } else {
      server.sendContent("<li>このページには表示するファイルがありません。</li>");
    }
  }

  server.sendContent("</ul>");

  // --- ページ移動 ---
  {
    String nav = "<p>";
    if (page > 0) {
      nav += "<a href='/mutter?page=" + String(page - 1) + "'><button>⬅️ 前へ</button></a> ";
    }
    nav += "<b>" + String(page + 1) + " ページ</b> ";
    if (hasNext) {
      nav += "<a href='/mutter?page=" + String(page + 1) + "'><button>次へ ➡️</button></a>";
    }
    nav += "</p>";
    server.sendContent(nav);
  }

  server.sendContent("<p class='note'>1ページ " + String(MUTTER_PER_PAGE) +
                     " 件表示。全件数は下のボタンで確認できます（重い処理のため分離）。</p>");

  // --- 旧形式チェックは別ボタンに分離（全走査を毎回やらない） ---
  server.sendContent("<p class='note'>"
                     "<a href='/mutter_legacy'><button>🔍 旧形式 mutter_***.wav を確認</button></a> "
                     "全件数カウントもこちらで確認できます。</p>");

  outShownCount = shownCount;
  outHasNext = hasNext;

  server.sendContent(karipomPageFooter());
}

// ====================================================
// sendMutterPage()
// streamMutterPage() をチャンク送信でクライアントへ返すラッパ。
// Content-Length不明のチャンク転送を宣言してからストリーミングする。
// ページ生成時間計測用のログ（START / END count/page）もここで出す。
// ====================================================
void sendMutterPage(int page, const String& message = "") {
  unsigned long t0 = millis();
  addLog("MUTTER PAGE START");

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=UTF-8", "");

  int shownCount = 0;
  bool hasNext = false;
  streamMutterPage(page, message, shownCount, hasNext);

  server.sendContent("");  // チャンク転送の終端

  addLog("MUTTER PAGE END count=" + String(shownCount) +
         " page=" + String(page) +
         " ms=" + String(millis() - t0));
}

// ===== End WAV Upload/Delete Helpers =====

String configPageHtml(const String& message = "") {
  String current = readTextFileFromSD(WEB_CONFIG_PATH);
  String wifiCurrent = readTextFileFromSD("/wifi.txt");

  String html = karipomPageHeader("KariPom Lab – Config");
  html += "<h1>⚙️ SYSTEM CONFIG</h1>";

  html += "<p>";
  html += "<a href='/'>🏠 Lab Home</a> / ";
  html += "<b>⚙️ Config</b> / ";
  html += "<a href='/files'>📂 File Manager</a> / ";
  html += "<a href='/logtoilet'>🚽 Log Toilet</a>";
  html += "</p>";

  html += "<div class='box'>";

  wifi_mode_t mode = WiFi.getMode();

  if (WiFi.status() == WL_CONNECTED) {
    html += "<p>📶 STA Mode : CONNECTED</p>";

    html += "<p>📶 Connected SSID : ";
    html += WiFi.SSID();
    html += "</p>";

    html += "<p>🖥 IP Address : ";
    html += WiFi.localIP().toString();
    html += "</p>";
  } else {
    html += "<p>📶 STA Mode : DISCONNECTED</p>";
  }

  if (mode == WIFI_AP || mode == WIFI_AP_STA) {

    html += "<p>📡 AP Mode : ON</p>";
    html += "<p>📛 AP Name : ";
    html += htmlEscape(apName);
    html += "</p>";
    html += "<p>🖥 AP IP : ";
    html += WiFi.softAPIP().toString();
    html += "</p>";

  } else {
    html += "<p>📡 AP Mode : OFF</p>";
  }

  html += "</div>";

  if (message.length() > 0) {
    html += "<p class='msg'>";
    html += htmlEscape(message);
    html += "</p>";
  }

  html += "<div class='box'>";
  html += "<h2>📶 /wifi Settings</h2>";
  html += "<p>1行につき <code>SSID,password</code> の形式で入力してください。</p>";
  html += "<p>Wi-Fiが見つからない時は、下の <code>AP_NAME</code> と <code>AP_PASS</code> でアクセスポイントを開始します。</p>";
  html += "<p>例：1号機</p>";
  html += "<pre>AP_NAME=KariPom_1\nAP_PASS=karipom123</pre>";
  html += "<p>例：2号機</p>";
  html += "<pre>AP_NAME=KariPom_2\nAP_PASS=karipom123</pre>";
  html += "<p>※ <code>AP_PASS</code> は8文字以上必要です。保存後、反映するにはかりポムを再起動してください。</p>";
  html += "<form method='POST' action='/save_wifi'>";
  html += "<textarea name='body'>";
  html += htmlEscape(wifiCurrent);
  html += "</textarea><br>";
  html += "<button class='save' type='submit'>SAVE Wi-Fi / 保存</button>";
  html += "</form>";
  html += "<p>※保存後、反映するにはかりポムを再起動してください。</p>";
  html += "</div>";

  html += "<div class='box'>";
  html += "<h2>⚙️ System Config (/config.txt)</h2>";
  html += "<p style='font-size:12px;color:#555;'>wifi.txt などと同様に自由に編集できます。</p>";
  html += "<form method='POST' action='/save_config'>";
  html += "<textarea name='body'>";
  html += htmlEscape(current);
  html += "</textarea><br>";
  html += "<button class='save' type='submit'>SAVE / 保存</button>";
  html += "</form>";

  html += "<form method='GET' action='/confirm_delete_config'>";
  html += "<button class='del' type='submit'>DELETE / 削除へ</button>";
  html += "</form>";
  html += "</div>";

  html += karipomPageFooter();
  return html;
}

void setup() {

  // 2026/07/20: 115200 → 921600 へ変更。
  // 電源断解析ではシリアルが唯一の実時間ブラックボックスになるため、
  // 帯域と1行あたりのブロッキング時間（160文字 ≒ 14ms → ≒ 1.7ms）を改善する。
  // ※ Arduino IDE シリアルモニタ側も 921600 bps に変更が必要。
  Serial.begin(921600);
  delay(300);  // Serial初期化安定待ち（必須・起動直後のため残す）

  esp_reset_reason_t reason = esp_reset_reason();
  int reasonNum = (int)reason;

  String resetReasonText = "";

  switch (reasonNum) {
    case 1:  resetReasonText = "POWERON";   break;  // ESP_RST_POWERON
    case 3:  resetReasonText = "SW";        break;  // ESP_RST_SW
    case 4:  resetReasonText = "PANIC";     break;  // ESP_RST_PANIC
    case 5:  resetReasonText = "WDT";       break;  // ESP_RST_INT_WDT
    case 6:  resetReasonText = "WDT";       break;  // ESP_RST_TASK_WDT
    case 7:  resetReasonText = "WDT";       break;  // ESP_RST_WDT
    case 8:  resetReasonText = "DEEPSLEEP"; break;  // ESP_RST_DEEPSLEEP
    case 9:  resetReasonText = "BROWNOUT";  break;  // ESP_RST_BROWNOUT
    case 11: resetReasonText = "USB";       break;  // ESP_RST_USB (ESP-IDF >= 4.3)
    default: resetReasonText = "UNKNOWN_" + String(reasonNum); break;
  }

  // シリアル・addLog・SD直接書き込み・appendToiletLog すべてで同一フォーマット
  String resetReasonLine = "BOOT_RESET_" + resetReasonText + " (" + String(reasonNum) + ")";

  // 前回値を先に退避してから今回値を保存する（順序が逆だと PREV が今回値になってしまう）
  int prevResetReason = rtcLastResetReason;
  rtcLastResetReason = reasonNum;

  Serial.println();
  Serial.println("########################################");
  Serial.println("########## RESET REASON ################");
  Serial.println("########################################");
  Serial.println(resetReasonLine);

  // 前回のRESET REASONをRTCから読み出して表示
  if (prevResetReason >= 0) {
    Serial.println("PREV RESET REASON = " + String(prevResetReason));
  }

  // addLog はSerial + webLog に記録（SDはまだ初期化されていない）
  // SD初期化後に再度 logFirmwareInfo() を呼び、SDログにも確実に残す。
  logFirmwareInfo();
  addLog("SYSTEM START");
  addLog(resetReasonLine);
  if (prevResetReason >= 0) {
    addLog("PREV RESET REASON = " + String(prevResetReason));
  }

  // タイムゾーンをJSTに設定する（WiFi未接続・RTC復元時のmktime用）。
  // NTP同期時は configTzTime("JST-9",...) がTZ設定を兼ねるが、
  // WiFi未接続・RTC復元のみの場合は configTzTime が呼ばれないため
  // ここで先に setenv/tzset を実行しておく。
  // ※ configTime(0,0,...) はTZ環境変数を"UTC0"にリセットするため使用しない。
  setenv("TZ", "JST-9", 1);
  tzset();

  auto cfg = M5.config();

  cfg.internal_mic = true;
  cfg.internal_spk = true;

  // ====================================================
  // CoreS3初期化
  //
  // LCDはまだ使えない状態。
  // drawFace()
  // drawSleepEyes()
  // drawBootFace()
  // drawFaceImage()
  //
  // などの描画関数は CoreS3.begin() 後で呼ぶこと。
  // ====================================================
  CoreS3.begin(cfg);

  // 2026/07/21: CoreS3.begin()直後はライブラリ既定値（実質100%/255）のため、
  // 起動直後に標準輝度（70%相当）を明示的に適用する。
  // 顔画像・背景色・描画内容には影響しない（バックライト輝度のみ）。
  CoreS3.Display.setBrightness(LCD_BACKLIGHT_STANDARD);

  // ====================================================
  // 起動時AXP2101フォレンジック（2026/07/20 追加・最優先項目）
  //
  // 【この位置である理由】
  //   ・CoreS3.begin(cfg) の直後  … I2Cが初期化済みでAXP2101を読める
  //   ・最初の CoreS3.update() より前 … PEKラッチが消費される前
  //   ・getPekPress() / getKeyState() を一度も呼ぶ前
  //
  //   AXP2101のIRQステータス(0x48/0x49/0x4A)は、PMIC自身に入力が残っていれば
  //   出力を落としても保持される。前回の「完全電源OFF」の原因は
  //   ここでしか読み出せないため、他の初期化処理より先に実行する。
  //
  //   SDはまだ初期化されていないため、この時点ではSerial + webLogのみ。
  //   SD.begin() 成功後に axpBootForensicLine / axpBootCauseLine を
  //   RESET REASONと同じ思想でSDへ直書きする。
  // ====================================================
  captureAxp2101BootForensics();

  CoreS3.Speaker.begin();

  drawBootFace();

  //smartDelay(1000);
  addLog("SETUP START");

  // ── BM8563 RTC 起動時チェック ────────────────────────────────────────
  // NTP未取得環境でも過去に同期済みのRTC時刻でログを打刻するため、
  // TZ設定（JST-9）完了後に即座にRTCから復元を試みる。
  // restoreTimeFromRtc() が成功すれば以降のgetLocalTime()がRTC由来の時刻を返す。
  rtcTimeValid = false;

  // ── BM8563 RTC 起動時診断（詳細版）────────────────────────────────
  // RTCが動作しているか、NTP書き込みが届いているかを段階的に確認する。

  // ステップ1: BM8563から生の日時値を読み出してログに出す
  {
    auto dt = CoreS3.Rtc.getDateTime();
    char rawBuf[64];
    snprintf(rawBuf, sizeof(rawBuf),
             "RTC RAW: %04d/%02d/%02d %02d:%02d:%02d wday=%d",
             dt.date.year, dt.date.month, dt.date.date,
             dt.time.hours, dt.time.minutes, dt.time.seconds,
             dt.date.weekDay);
    addLog(String(rawBuf));

    // ステップ2: 有効性判定
    int yr = dt.date.year;
    int mo = dt.date.month;
    int dy = dt.date.date;
    if (yr < 2020 || yr > 2099 || mo < 1 || mo > 12 || dy < 1 || dy > 31) {
      addLog("RTC TIME INVALID (year=" + String(yr) + " mon=" + String(mo)
           + " day=" + String(dy) + ") -> millis fallback");
    } else {
      // ステップ3: ESP32システム時刻にsettimeofdayで反映
      struct tm t = {};
      t.tm_year  = yr - 1900;
      t.tm_mon   = mo - 1;
      t.tm_mday  = dy;
      t.tm_hour  = dt.time.hours;
      t.tm_min   = dt.time.minutes;
      t.tm_sec   = dt.time.seconds;
      t.tm_isdst = -1;
      time_t epoch = mktime(&t);
      struct timeval tv = { epoch, 0 };
      int r = settimeofday(&tv, nullptr);
      addLog("RTC settimeofday result=" + String(r) + " epoch=" + String((long)epoch));

      // ステップ4: settimeofday後にgetLocalTimeで読み返して確認
      struct tm rtcCheck = {};
      bool gotTime = getLocalTime(&rtcCheck, 0);
      char verBuf[48];
      snprintf(verBuf, sizeof(verBuf),
               "%04d/%02d/%02d %02d:%02d:%02d",
               rtcCheck.tm_year + 1900, rtcCheck.tm_mon + 1, rtcCheck.tm_mday,
               rtcCheck.tm_hour, rtcCheck.tm_min, rtcCheck.tm_sec);
      if (gotTime) {
        addLog("RTC TIME OK: " + String(verBuf));
        rtcTimeValid = true;
      } else {
        addLog("RTC getLocalTime FAILED after settimeofday (epoch=" + String((long)epoch) + ")");
      }
    }
  }
  // ─────────────────────────────────────────────────────────────────────

  // audioSource はこの時点ではコンパイル時デフォルト（UDP）。
  // NVS保存値は loadConfig() で読み、setup()末尾の applyBootAudioSource() で適用される。
  addLog("BOOT audioSource=" + String(audioSourceName(audioSource))
       + " macAudioLinkEnabled=" + String(macAudioLinkEnabled ? "ON" : "OFF"));

  if (!SD.begin(4)) {

    addLog("SD CARD: FAIL");

  } else {

    addLog("SD CARD: OK");

    // RESET REASON を SD に直接保存（SDログシステム経由しない）
    // setup()序盤の addLog() はSD未初期化のため保存されない。
    // ここで改めて直接書き込むことで確実にSDに残す。
    {
      if (!SD.exists(LOG_TOILET_DIR)) SD.mkdir(LOG_TOILET_DIR);
      File rf = SD.open(LOG_TOILET_FILE, FILE_APPEND);
      if (rf) {
        rf.println("[BOOT][" + String(millis()) + "] === " + resetReasonLine + " ===");

        // 起動時AXP2101フォレンジック（2026/07/20 追加）
        // CoreS3.begin()直後に読み出した生値を、RAMバッファを経由せず
        // ここでSDへ直書きする。RESET REASONと同じ思想。
        // 電源断解析の一次情報なので、確実にSDへ残すことを最優先する。
        //
        // IRQEN行も対象に含める（初版は漏れていた）。
        // 監視が実際に有効化できたかどうかは、次回の停止原因が
        // 記録されるか否かを左右するため、SDに残す必要がある。
        {
          const String* lines[] = {
            &axpBootForensicLine,   // 一次情報（生レジスタ値）
            &axpBootCauseLine,      // 電源断の原因候補
            &axpBootInfoLine,       // 情報ビット（空なら出力しない）
            &axpBootUnmappedLine,   // 未解釈ビット（空なら出力しない）
            &axpBootIrqEnLine       // IRQ有効化の読戻し検証結果
          };
          for (int i = 0; i < 5; i++) {
            if (lines[i]->length() > 0) {
              rf.println("[BOOT][" + String(millis()) + "] " + *lines[i]);
            }
          }
        }

        rf.close();
      }
    }

    // config.txt を読み込んで実行時フラグを設定（SD書き込み系より先に実行）
    loadConfig();

    // mutter一覧をRAMキャッシュに構築（起動時1回だけSD走査）。
    // 以降のAUTO/JOYSTICK独り言はこのキャッシュから即選択し、
    // 再生のたびに /sounds/mutter を全走査しない（探索タイムアウト回避）。
    if (!SD.exists("/sounds/mutter")) SD.mkdir("/sounds/mutter");
    rebuildMutterCache();

    // cfg_enableSDLog が確定してからSD書き込み処理を実行
    if (cfg_enableSDLog) {
      prepareLogToilet();
      appendToiletLog(resetReasonLine, "");  // BOOT_RESET_xxx (N) 形式は resetReasonLine に含まれる

      // Firmware/Build情報をSDログに確実に残す。
      // setup序盤の logFirmwareInfo() はSD未初期化のためSDには保存されていない。
      // cfg_enableSDLog 確定後にここで再度呼ぶことでSDログにも記録される。
      logFirmwareInfo();
    }

    bool wifiConnected = connectWiFiFromSD();

    if (!wifiConnected) {
      startAPMode();
    }

    if (WiFi.status() == WL_CONNECTED) {
      addLog("WIFI CONNECTED");
      addLog("SSID = " + WiFi.SSID());
      addLog("IP = " + WiFi.localIP().toString());

    } else if (WiFi.getMode() == WIFI_AP) {
      addLog("AP MODE ACTIVE");
      addLog("AP IP = " + WiFi.softAPIP().toString());
    }

    if (WiFi.status() == WL_CONNECTED || WiFi.getMode() == WIFI_AP) {

      // ===== KariPom Web Server =====

      server.on("/log", []() {
        String html = "<html><head><meta charset='UTF-8'>";
        html += "<meta name='viewport' content='width=device-width,initial-scale=1.0'>";
        html += "<meta http-equiv='refresh' content='2'>";
        html += "<style>";
        html += ":root{--bg:#0d0f12;--card:#14181e;--text:#cbd5e1;--accent:#38bdf8;"
                "--lcd-bg:#072216;--lcd-fg:#22c55e;--border:#252d3a}";
        html += "body{font-family:monospace;background:var(--bg);color:var(--text);padding:12px}";
        html += ".panel{background:var(--card);border-radius:6px;padding:16px;"
                "box-shadow:0 0 0 1px var(--border);max-width:700px;margin:0 auto}";
        html += "h2{color:var(--accent);border-bottom:1px solid var(--border);padding-bottom:4px}";
        html += "a{color:var(--accent);text-decoration:none}";
        html += "hr{border:none;border-top:1px solid var(--border);margin:10px 0}";
        html += ".logbox{background:var(--lcd-bg);color:var(--lcd-fg);padding:10px;"
                "font-size:13px;line-height:1.4;overflow:auto;border-radius:4px;white-space:pre-wrap}";
        html += "</style>";
        html += "</head><body><div class='panel'>";

        html += "<h2>📜 KariPom Lab – Web Log</h2>";
        html += "<p>";
        html += "<a href='/'>🏠 Lab Home</a> / ";
        html += "<a href='/config'>⚙️ Config</a> / ";
        html += "<a href='/files'>📂 File Manager</a>";
        html += "</p>";
        html += "<hr>";
        html += webLog;

        html += "</body></html>";

        server.send(200, "text/html", html);
      });

      server.on("/", []() {
        unsigned long _t0 = webLogStart("/");
        String html;
        html.reserve(12000);
        html += "<html><head><meta charset='UTF-8'>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        html += "<title>KariPom Lab v";
        html += FW_VERSION;
        html += "</title>";

        // ===== KariPom Lab Theme =====
        html += "<style>";
        // CSS変数（カラーパレット）
        html += ":root{"
                "--bg:#0d0f12;--card:#14181e;--text:#cbd5e1;"
                "--accent:#38bdf8;--led-ok:#4ade80;--led-err:#ef4444;"
                "--led-warn:#facc15;--lcd-bg:#072216;--lcd-fg:#22c55e;"
                "--border:#252d3a;"
                "}";
        // ベース
        html += "body{font-family:-apple-system,BlinkMacSystemFont,'Hiragino Sans','Yu Gothic',sans-serif;"
                "color:var(--text);background:var(--bg);padding:12px;line-height:1.6}";
        // メインパネル：金属パネル風（外枠 + 内側ハイライト）
        html += ".panel{"
                "background:var(--card);"
                "border-radius:6px;"
                "padding:16px;"
                "box-shadow:0 0 0 1px var(--border),inset 0 1px 0 rgba(255,255,255,.04);"
                "max-width:700px;margin:0 auto}";
        // Lab ラベル（::before でバージョン表示）
        html += ".panel::before{"
                "content:'◈  CONTROL CONSOLE';"
                "display:block;color:var(--accent);"
                "font-size:10px;letter-spacing:.18em;"
                "opacity:.45;margin-bottom:8px;font-family:monospace}";
        // 見出し
        html += "h1,h2,h3{"
                "color:var(--accent);letter-spacing:.04em;"
                "border-bottom:1px solid var(--border);padding-bottom:4px;margin-top:18px}";
        html += "h1{font-size:19px;letter-spacing:.08em}";
        // ボタン：物理スイッチ風
        html += "button{"
                "font-size:15px;padding:8px 14px;margin:3px;"
                "border-radius:3px;border:1px solid #2a3348;"
                "background:#1c2230;color:var(--text);"
                "box-shadow:0 2px 4px rgba(0,0,0,.5),inset 0 1px 0 rgba(255,255,255,.05);"
                "cursor:pointer}";
        html += "button:active{box-shadow:inset 0 2px 4px rgba(0,0,0,.6);transform:translateY(1px)}"
          "button:hover{background:#263040;border-color:#3a4d68}"
          "button:disabled,button[disabled]{opacity:.35;cursor:not-allowed;box-shadow:none;transform:none}"
          ".save:hover,.onbtn:hover{background:#193325;border-color:#2e7a48}"
          ".del:hover,.offbtn:hover{background:#2e1111;border-color:#7a2525}"
          ".play:hover{background:#152535;border-color:#1e5575}";
        // ON / OFF ボタン
        html += ".onbtn{background:#0d2218;border-color:#1f5c35;color:var(--led-ok)}";
        html += ".offbtn{background:#220d0d;border-color:#5c1a1a;color:var(--led-err)}";
        // 必要な枠だけ .lab-card で暗くする（一括override廃止）
        html += ".lab-card{background:#0c1016;border-radius:4px;padding:14px;margin:12px 0}";
        // テキスト補助
        html += ".note{font-size:13px;color:#4e5f72;line-height:1.6}";
        html += "a{color:var(--accent);text-decoration:none}";
        html += "a:hover{text-decoration:underline}";
        html += "hr{border:none;border-top:1px solid var(--border);margin:14px 0}";
        // details / summary
        html += "details summary{color:var(--accent);cursor:pointer;font-weight:bold}";
        // フォーム部品
        html += "input[type=number],select{"
                "background:#0c1016;border:1px solid var(--border);"
                "color:var(--text);border-radius:3px;padding:4px}";
        // カメラ画像
        html += "#cam{border-radius:4px}";
        html += "</style>";
        // ===== End KariPom Lab Theme =====

        html += "</head><body><div class='panel'>";

        html += "<h1>🐰 KariPom Lab</h1>";

        // 2026/07/26: ページ移動ナビをタイトル直下（最上部）へ移動。
        //   従来はページ中央付近（PC音声の説明の下）にあり、他ページへの移動手段が
        //   分かりにくかった。リンク先・項目・順序・区切り文字は従来のまま。
        //   各項目を white-space:nowrap で包むことで、狭い画面でも項目の途中では
        //   折り返さず項目単位で改行される（最長の「📂 File Manager」でも十分短く、
        //   横スクロールは発生しない）。他ページのナビは変更していない。
        html += "<p style='margin:6px 0 14px;'>";
        html += "<span style='white-space:nowrap;'><a href='/'>🏠 Lab Home</a></span> / ";
        html += "<span style='white-space:nowrap;'><a href='/config'>⚙️ Config</a></span> / ";
        html += "<span style='white-space:nowrap;'><a href='/files'>📂 File Manager</a></span> / ";
        html += "<span style='white-space:nowrap;'><a href='/logtoilet'>🚽 Log Toilet</a></span>";
        html += "</p>";

        // カメラ・SDログのデフォルトOFF注意書き
        html += "<div class='lab-card' style='border:2px solid #f5a623;'>";
        html += "<b>⚡ 動作速度について</b><br>";
        html += "📷 カメラ処理・💾 SDログ・📸 SD画像記録は動作に影響します。<br>";
        html += "デフォルトはすべて <b>OFF</b> です。必要なときだけONにしてください。<br>";
        html += "<small style='color:#8aaccc;'>";
        html += "カメラ：";
        html += cfg_enableCamera    ? "<b style='color:var(--led-ok);'>ON</b>" : "<b style='color:var(--led-err);'>OFF</b>";
        html += "　SDログ（<a href='/logtoilet'>Log Toilet</a>）：";
        html += cfg_enableSDLog     ? "<b style='color:var(--led-ok);'>ON</b>" : "<b style='color:var(--led-err);'>OFF</b>";
        html += "　SD画像記録（<a href='#image-record'>↓ 下</a>）：";
        html += cfg_enableToiletCam ? "<b style='color:var(--led-ok);'>ON</b>" : "<b style='color:var(--led-err);'>OFF</b>";
        html += "</small></div>";

        // キャラクタースタイル選択（顔の着せ替えではなく、最初に選ぶキャラクター人格）
        html += "<div class='lab-card' style='border:1px solid var(--border);'>";
        html += "<h3>🎭 Character Style</h3>";
        html += "<p>現在：<b style='color:var(--led-ok);'>● " + String(characterStyleName(cfg_characterStyle)) + "</b></p>";
        html += "<p>";
        html += "<a href='/character_style?v=karipom'><button class='onbtn'" + String(cfg_characterStyle == CHARACTER_KARIPOM ? " style='font-weight:bold;border:2px solid green;'" : "") + ">🐰 KariPom</button></a> ";
        html += "<a href='/character_style?v=miss'><button class='onbtn'" + String(cfg_characterStyle == CHARACTER_MISS_KARIPOM ? " style='font-weight:bold;border:2px solid green;'" : "") + ">🎀 Miss KariPom</button></a>";
        html += "</p>";
        html += "<p class='note'>違いはまつ毛のみです。NVSに保存され、再起動後も保持されます。</p>";
        html += "</div>";

        html += "<div class='lab-card' style='border:1px solid var(--border);'>";
        html += "<h3>📡 Network Status</h3>";

        if (WiFi.status() == WL_CONNECTED) {
          html += "🟢 ONLINE<br>";
          html += "🏠 ";
          html += WiFi.SSID();
          html += "<br>";

        } else if (WiFi.getMode() == WIFI_AP) {
          html += "🟠 AP MODE<br>";
          html += "📡 " + apName + "<br>";

        } else {
          html += "🔴 OFFLINE<br>";
        }

        // 🐰 KariPom Name（個体識別名）。空欄の場合はIPアドレスを表示名として使用する。
        html += "<p style='margin:10px 0 2px;'>🐰 KariPom Name</p>";
        html += "<p style='margin:0 0 10px;font-size:18px;'><b>" + htmlEscape(karipomDisplayName()) + "</b></p>";

        html += "<p style='margin:10px 0 2px;'>🌐 IP Address</p>";
        html += "<p style='margin:0;font-size:18px;'><b>";
        if (WiFi.status() == WL_CONNECTED) {
          html += WiFi.localIP().toString();
        } else if (WiFi.getMode() == WIFI_AP) {
          html += WiFi.softAPIP().toString();
        } else {
          html += "-";
        }
        html += "</b></p>";

        // KariPom Name 編集フォーム（文字列・空欄可・NVS保存・既存cfg_設定と同じ実装方式）
        html += "<form method='GET' action='/karipom_name' style='margin-top:10px;'>";
        html += "<input type='text' name='name' value='" + htmlEscape(cfg_karipomName) + "' maxlength='32' placeholder='(空欄=IPアドレス表示)' style='width:200px;'>";
        html += " <button type='submit' class='onbtn'>💾 保存</button>";
        html += "</form>";
        html += "<p class='note'>空欄の場合は表示名としてIPアドレスを使用します。複数台運用時の識別にご利用ください。</p>";

        html += "</div>";  // ←ここで閉じる

        html += "<p class='note'>";

        // 2026/07/20: 表示名を「UDP(Mac)」→「PC音声（Wi-Fi）」へ変更。
        // Windows / macOS / Linux すべてに対応済みのため、Mac限定と読める表現を排除する。
        // 内部識別子（AUDIO_SRC_UDP / m=udp / src=UDP / NVS保存値）は一切変更しない。
        //
        // 2026/07/21: 説明文をkaripom_talk_20260719_multiOS.py（導入手順書）の実装に合わせて更新。
        //   ・WindowsはWASAPI Loopbackで既定の再生デバイスを直接取得するため、
        //     VB-CABLE等の仮想オーディオデバイスは不要（従来の説明は現行実装と不一致だったため修正）。
        //   ・Linuxを追加（PulseAudio/PipeWireのモニターソースを使用）。
        //     Linuxは開発環境にオーディオデバイスがなく実機検証未実施のため、
        //     「対応予定」ではなく「対応実装済み・実機未検証」と正確に表記する。
        //   ・実装／UDP通信／音声取得処理そのものは変更していない（説明文のみ）。
        //
        // 2026/07/25: macOS説明を「BlackHole 2chのみ」から「BlackHole 2ch＋複数出力装置
        //   （Multi-Output Device）を使う」ことが伝わる表記へ修正。詳細なAudio MIDI設定
        //   手順（スクリーンショット付き）はREADME側で案内するため、WebUIは簡潔な表記に留める。
        //   Windows/Linuxの説明文は変更なし。実装／UDP通信／音声取得処理は変更していない。
        html += "💻 PC音声（Wi-Fi）モードを使用する場合は、PC側でKariPom Talk（Pythonプログラム）を実行してください。<br>";
        html += "🔊 PC音声の取得方式：<br>";
        html += "・macOS：BlackHole 2ch＋複数出力装置（Multi-Output Device）を使用（詳細セットアップ手順はREADME参照）<br>";
        html += "・Windows：WASAPI Loopback（追加ソフト不要）<br>";
        html += "・Linux：PulseAudio / PipeWireのモニターソースを使用（対応実装済み・実機未検証）<br>";
        html += "📶 PCとCoreS3を同じWi-Fiに接続してください。<br>";
        html += "🖥 karipom_talk.py の M5_IP を、かりポム画面に表示されるCoreS3のIPアドレスに変更してください。";
        html += "</p>";

        // 2026/07/26: ここにあったページ移動ナビはタイトル直下（最上部）へ移動した。
        html += "<hr>";

        html += "<h2>🐰 Karipom Ear</h2>";

        // 🎤 Current Audio Source（複数台運用時に今どの経路で動作中かを一目で分かるように）
        html += "<p style='margin:10px 0 2px;'>🎤 Audio Input Source</p>";
        html += "<p style='margin:0 0 8px;font-size:20px;'><b>" + audioSourceShortLabel(audioSource) + "</b></p>";

        html += "<p>";
        html += "現在：<b style='color:var(--led-ok);font-size:20px;'>● ";
        // 2026/07/21: 表示のみ audioSourceName()（内部/ログ用の"UDP"表記）から
        // audioSourceShortLabel()（"💻 PC音声（Wi-Fi）"等のユーザー向け表記）へ変更。
        // 内部識別子・ログ出力（addLog等でのaudioSourceName利用）・保存値は不変。
        html += audioSourceShortLabel(audioSource);
        html += "</b>";
        if (audioSource == AUDIO_SRC_MIC) {
          // 2026/07/26: 改行位置を明示。従来は指定がなく、禁則処理で「開）」だけが
          //   次行へ送られていた。<br>で「終了後に自動再開）」をひとかたまりにする。
          html += " <span style='color:#e67e00;'>（独り言再生中・サーボ動作中はマイク解析を一時停止し、<br>終了後に自動再開）</span>";
        }
        html += "</p>";

        // 選択中のソースを緑枠で強調する4択ボタン
        // 2026-07-25: 表示順を「利用開始までに必要な追加準備が少ない順」へ再変更
        //   （OFF → 内蔵マイク → PC音声（Wi-Fi） → LINE IN）。
        //   LINE INは外部音声入力回路（PCM1808＋Pico 2＋3.5mm入力ジャック）が
        //   必要なため、PC音声（Wi-Fi・Python環境が必要）より後ろへ移動し、
        //   必要なハードウェアをラベルにも明示した。
        //   enum値・m=パラメータ（内部識別子）・ハンドラ側の処理は一切変更しない。
        html += "<p>";
        html += "<a href='/audio_src?m=off'><button class='offbtn'" + String(audioSource == AUDIO_SRC_OFF ? " style='font-weight:bold;border:2px solid green;'" : "") + ">⭕ OFF</button></a> ";
        html += "<a href='/audio_src?m=mic'><button class='onbtn'" + String(audioSource == AUDIO_SRC_MIC ? " style='font-weight:bold;border:2px solid green;'" : "") + ">🐰 内蔵マイク</button></a> ";
        html += "<a href='/audio_src?m=udp'><button class='onbtn'" + String(audioSource == AUDIO_SRC_UDP ? " style='font-weight:bold;border:2px solid green;'" : "") + ">💻 PC音声（Wi-Fi・Python必要）</button></a> ";
        // 表示名のみ変更（リンク先の m=line は内部識別子なので変更しない）
        html += "<a href='/audio_src?m=line'><button class='onbtn'" + String(audioSource == AUDIO_SRC_LINEIN ? " style='font-weight:bold;border:2px solid green;'" : "") + ">🔌 LINE IN（PCM1808＋Pico 2＋3.5mm入力ジャック必要）</button></a>";
        html += "</p>";

        // 選択中ソースの詳細説明（「どのかりポムが制御されるか」を明確化。将来Bluetooth追加時も同じ関数で対応）
        html += "<div class='lab-card' style='border:1px solid var(--border);'>";
        html += audioSourceDescriptionHtml(audioSource);
        html += "</div>";

        html += "<p class='note'>";
        html += "💻 PC音声（Wi-Fi）：PCの再生音をWi-Fi経由で受け取り、口パクとグライコ表示に使用します。Windows / macOS / Linuxに対応しています。";
        html += "送信中はFFTバーを顔に重ねた<b>グライコフェイス</b>を表示します（詳細設定でON/OFF可）。<br>";
        html += "🐰 内蔵マイク：CoreS3単体で周囲の音に反応して口パクします。独り言再生中・サーボ動作中はマイク解析を一時停止し、終了後に自動再開します。<br>";
        html += "🔌 LINE IN：Karipom Ear v2（PCM1808+Pico / Port C）。音楽でFFT Face＋口パク。<br>";
        html += "💻 PC音声（Wi-Fi）で反応しなくなった場合は、karipom_talk.py の再起動や再選択（セッションリセット）をお試しください。";
        html += "</p>";

        // ── Lighting 設定（背景照明・複数同時ON可・Visualizerとは別カテゴリ）──
        // チェックボックス方式。モード追加時はこのUIを書き換えず LIGHT_MODES[] へ追記するだけ。
        // 2026-07-25: 内蔵マイク選択時もLightingが正式に利用可能になったため、
        //   デフォルト展開条件にAUDIO_SRC_MICを追加。LINE IN/PC音声(UDP)の
        //   既存条件・Audio Visualizer側の展開条件・Lightingの動作判定/選択/
        //   保存/復元ロジックには一切触れていない（表示の折りたたみ状態のみ）。
        html += "<details" + String((audioSource == AUDIO_SRC_UDP || audioSource == AUDIO_SRC_LINEIN || audioSource == AUDIO_SRC_MIC) ? " open" : "") + ">";
        html += "<summary style='cursor:pointer;font-weight:bold;'>💡 Lighting（背景照明）</summary>";
        html += "<p style='margin:8px 0 4px;font-size:12px;color:#8aaccc;'>音楽に合わせて画面全体が光る背景演出です。<b>背景Lighting（Disco / Aurora / Matrix / Retro Race / Sky Raid / Eye Slot / Classic Race / Asteroid Field / Tempest Tunnel）は複数選択できますが、背景として描画されるのは最後に選んだ1つです。</b>Laser などの Overlay Lighting は、その背景へ重ねて描画されます。描画順は Lighting（背景→オーバーレイ）→ Visualizer → 顔 です。</p>";

        // ── 🎲 Lighting Random（v1.0 / 2026-07-27）──
        // 個別選択とRandomは相互排他。ON中は個別チェックを全解除し、
        // 一定時間ごとにLIGHT_MODES[]から1つを自動選択する（既存の描画・
        // 複数同時ON設計・保存/復元ロジックは変更しない。選び方の追加のみ）。
        // 2026-07-27: 個別演出一覧より前へ移動（Random機能に気付きやすくするため。
        //   ブロックの中身・機能・状態表示ロジックは一切変更していない）。
        html += "<p style='margin:14px 0 4px;font-weight:bold;'>🎲 Lighting Random：<b style='color:"
              + String(cfg_lightingRandomOn ? "var(--led-ok)" : "var(--led-err)") + ";'>● "
              + String(cfg_lightingRandomOn ? "ON" : "OFF") + "</b></p>";
        // Random ON中のみ「現在：○○」を表示する。cfg_lightingMaskはRandom中
        // 常に1ビットだけが立っている前提（Random tick側の仕様どおり）なので、
        // 立っているビットを探して表示するだけの参照用途（表示専用・状態は変更しない）。
        if (cfg_lightingRandomOn) {
          String curLightLabel = "-";
          for (uint8_t li = 0; li < (uint8_t)LIGHT_MODE_COUNT; li++) {
            if (cfg_lightingMask & (1u << li)) { curLightLabel = String(LIGHT_MODES[li].label); break; }
          }
          html += "<p style='margin:0 0 8px;font-size:13px;color:#8aaccc;'>現在：<b style='color:var(--led-ok);'>"
                + curLightLabel + "</b></p>";
        }
        html += "<p>";
        html += "<a href='/lighting_random?v=on'><button class='onbtn'"
              + String(cfg_lightingRandomOn ? " style='font-weight:bold;border:2px solid green;'" : "")
              + ">🎲 Random ON</button></a> ";
        html += "<a href='/lighting_random?v=off'><button class='offbtn'"
              + String(!cfg_lightingRandomOn ? " style='font-weight:bold;border:2px solid red;'" : "")
              + ">⭕ Random OFF</button></a>";
        html += "</p>";
        html += "<p style='margin:8px 0 4px;'>切替間隔：</p><p>";
        {
          const int lightRndLv[3] = { 5, 10, 15 };
          for (int k = 0; k < 3; k++) {
            bool sel = (cfg_lightingRandomIntervalMin == lightRndLv[k]);
            html += "<a href='/lighting_random_interval?m=" + String(lightRndLv[k]) + "'>";
            html += "<button class='onbtn'"
                  + String(sel ? " style='font-weight:bold;border:2px solid green;'" : "")
                  + ">" + String(lightRndLv[k]) + "分</button></a> ";
          }
        }
        html += "</p>";
        html += "<p class='note'>ONにすると、個別に選択していたLightingのチェックをすべて解除し、上の候補から一定時間ごとに1つを自動でランダム選択します（ON直後も待たずに1つ選んで開始します。直前と同じものは連続で選びません）。個別のLightingを手動で選び直すと自動的にOFFへ戻ります。ON/OFF・間隔（5/10/15分）の設定はNVSへ保存され、再起動後も維持されます。</p>";

        html += "<p>";
        for (uint8_t li = 0; li < (uint8_t)LIGHT_MODE_COUNT; li++) {
          // 🎲 Random ON中は cfg_lightingMask に自動選択のビットが立っていても、
          // 個別チェックとしては表示しない（手動選択とRandomの一時選択を
          // 見た目でも混同しないため。ロジック側のcfg_lightingMask自体は不変）。
          bool on = !cfg_lightingRandomOn && (cfg_lightingMask & (1u << li)) != 0;
          html += "<a href='/lighting?m=" + String(LIGHT_MODES[li].id) + "&v=" + String(on ? "off" : "on") + "'>";
          html += "<button class='" + String(on ? "onbtn" : "offbtn") + "'"
                + String(on ? " style='font-weight:bold;border:2px solid green;'" : "") + ">"
                + String(on ? "☑ " : "☐ ") + String(LIGHT_MODES[li].label) + "</button></a> ";
        }
        html += "</p>";
        html += "<table style='border-collapse:collapse;font-size:12px;margin-top:6px;'>";
        for (uint8_t li = 0; li < (uint8_t)LIGHT_MODE_COUNT; li++) {
          html += "<tr><td style='padding:3px 8px;vertical-align:top;white-space:nowrap;'><b>"
                + String(LIGHT_MODES[li].label) + "</b></td>";
          html += "<td style='padding:3px 8px;color:#8aaccc;'>" + String(LIGHT_MODES[li].note) + "</td></tr>";
        }
        html += "</table>";

        // ── Lighting 共通 Brightness（全Lightingへ一括適用・NVS保存）──
        html += "<p style='margin:10px 0 4px;font-weight:bold;'>🔆 Brightness（明るさ）：<b style='color:var(--led-ok);'>"
              + String(cfg_lightingBrightness) + "%</b></p>";
        html += "<p>";
        {
          const int briLv[5] = { 20, 40, 60, 80, 100 };
          for (int k = 0; k < 5; k++) {
            bool sel = (cfg_lightingBrightness == briLv[k]);
            html += "<a href='/lighting_brightness?v=" + String(briLv[k]) + "'>";
            html += "<button class='onbtn'"
                  + String(sel ? " style='font-weight:bold;border:2px solid green;'" : "")
                  + ">" + String(briLv[k]) + "%</button></a> ";
          }
        }
        html += "</p>";
        html += "<p class='note'>Lighting全体（Disco Floor / 今後追加の面演出）に共通で効く明るさです。色味を変えずLEDの明るさだけを知覚的に調整します。既定80%。NVS保存。<br><b>※ Laser Show は常に鮮やかにするため、この明るさの影響を受けません</b>（Brightnessを下げると床が暗くなり、緑レーザーがより際立ちます）。</p>";
        html += "<p class='note'>チェックは「有効候補」を表します。背景（Disco / Aurora / Matrix / Retro Race / Sky Raid / Eye Slot / Classic Race / Asteroid Field / Tempest Tunnel / Hypnotic Vortex）は最後に選んだ1つが採用され、Overlay（Laser）は背景へ重なります。例：Disco+Laser＝踊る床＋緑ビーム、Aurora+Laser＝夜空＋緑ビーム、Matrix+Laser＝緑の雨をレーザーが切り裂く。今後 Defender風 / Scramble風 / Pong / Block Breaker / Fire / Neon / Bubbles なども追加予定です。内蔵マイクモードでは使用しません。選択内容はNVSへ保存されます。</p>";
        html += "</details>";

        // ── Audio Visualizer 設定（入力元から独立した共通設定）──
        // 旧称：UDP(Mac)詳細設定内「グライコフェイス」（ON/OFFの2値）。
        // v1.0でモード選択型（OFF / Graphic EQ / Audio Halo）へ拡張した。
        // ・PC音声(Wi-Fi) / LINE IN(Karipom Ear) / 将来のBluetooth で同じ設定・同じ描画を使う
        // ・NVSキー(udpVisiFace)・旧URL(/udp_visualizer_face)は互換のため残している
        // ・モード追加時はこのUIを書き換えず、VIZ_MODES[]へ追記するだけでよい
        html += "<details" + String((audioSource == AUDIO_SRC_UDP || audioSource == AUDIO_SRC_LINEIN) ? " open" : "") + ">";
        html += "<summary style='cursor:pointer;font-weight:bold;'>📊 Audio Visualizer</summary>";

        // ── 🎲 Audio Visualizer Random（v1.0 / 2026-07-27）──
        // Lighting Randomとは完全に独立。個別選択とRandomは相互排他。
        // MIC選択時にVisualizerが表示されない既存判定(isVisualizerFaceEnabled)は
        // 変更していないため、MIC中はRandomが自動選択してもそのまま表示されない。
        // 2026-07-27: 個別演出一覧より前へ移動（Random機能に気付きやすくするため。
        //   ブロックの中身・機能・状態表示ロジックは一切変更していない）。
        html += "<p style='margin:14px 0 4px;font-weight:bold;'>🎲 Visualizer Random：<b style='color:"
              + String(cfg_vizRandomOn ? "var(--led-ok)" : "var(--led-err)") + ";'>● "
              + String(cfg_vizRandomOn ? "ON" : "OFF") + "</b></p>";
        // Random ON中のみ「現在：○○」を表示する（表示専用・状態は変更しない）。
        if (cfg_vizRandomOn) {
          html += "<p style='margin:0 0 8px;font-size:13px;color:#8aaccc;'>現在：<b style='color:var(--led-ok);'>"
                + String(VIZ_MODES[cfg_visualizerMode].label) + "</b></p>";
        }
        html += "<p>";
        html += "<a href='/visualizer_random?v=on'><button class='onbtn'"
              + String(cfg_vizRandomOn ? " style='font-weight:bold;border:2px solid green;'" : "")
              + ">🎲 Random ON</button></a> ";
        html += "<a href='/visualizer_random?v=off'><button class='offbtn'"
              + String(!cfg_vizRandomOn ? " style='font-weight:bold;border:2px solid red;'" : "")
              + ">⭕ Random OFF</button></a>";
        html += "</p>";
        html += "<p style='margin:8px 0 4px;'>切替間隔：</p><p>";
        {
          const int vizRndLv[3] = { 5, 10, 15 };
          for (int k = 0; k < 3; k++) {
            bool sel = (cfg_vizRandomIntervalMin == vizRndLv[k]);
            html += "<a href='/visualizer_random_interval?m=" + String(vizRndLv[k]) + "'>";
            html += "<button class='onbtn'"
                  + String(sel ? " style='font-weight:bold;border:2px solid green;'" : "")
                  + ">" + String(vizRndLv[k]) + "分</button></a> ";
          }
        }
        html += "</p>";
        html += "<p class='note'>ONにすると、個別に選択していたVisualizerの選択を解除し、上の候補（OFFを除く）から一定時間ごとに1つを自動でランダム選択します（ON直後も待たずに1つ選んで開始します。直前と同じものは連続で選びません）。個別のVisualizerを手動で選び直すと自動的にOFFへ戻ります。内蔵マイクモードでは従来どおりVisualizerは表示されません。ON/OFF・間隔（5/10/15分）の設定はNVSへ保存され、再起動後も維持されます。</p>";

        // 🎲 Random ON中は、この行を「手動選択中の演出」のようには見せない。
        // 現在の自動選択名は下のRandom欄の「現在：」に表示する（ロジックは不変）。
        html += "<p style='margin:8px 0 4px;'>📊 Visualizer：<b style='color:";
        if (cfg_vizRandomOn) {
          html += "var(--led-ok);'>🎲 Random中</b></p>";
        } else {
          html += (cfg_visualizerMode == VIZ_MODE_OFF) ? "var(--led-err);'>● " : "var(--led-ok);'>● ";
          html += String(VIZ_MODES[cfg_visualizerMode].label) + "</b></p>";
        }
        html += "<p>";
        for (uint8_t vi = 0; vi < (uint8_t)VIZ_MODE_COUNT; vi++) {
          // 🎲 Random ON中は cfg_visualizerMode に自動選択の値が入っていても、
          // 個別ボタンのハイライトとしては表示しない（手動選択とRandomの
          // 一時選択を見た目でも混同しないため。ロジック側は不変）。
          bool sel = !cfg_vizRandomOn && ((uint8_t)cfg_visualizerMode == vi);
          String cls = (vi == (uint8_t)VIZ_MODE_OFF) ? "offbtn" : "onbtn";
          String hl  = sel ? String(" style='font-weight:bold;border:2px solid ")
                             + String(vi == (uint8_t)VIZ_MODE_OFF ? "red" : "green") + ";'"
                           : String("");
          html += "<a href='/visualizer?m=" + String(VIZ_MODES[vi].id) + "'>";
          html += "<button class='" + cls + "'" + hl + ">📊 " + String(VIZ_MODES[vi].label) + "</button></a> ";
        }
        html += "</p>";
        html += "<table style='border-collapse:collapse;font-size:12px;margin-top:6px;'>";
        for (uint8_t vi = 0; vi < (uint8_t)VIZ_MODE_COUNT; vi++) {
          html += "<tr><td style='padding:3px 8px;vertical-align:top;white-space:nowrap;'><b>"
                + String(VIZ_MODES[vi].label) + "</b></td>";
          html += "<td style='padding:3px 8px;color:#8aaccc;'>" + String(VIZ_MODES[vi].note) + "</td></tr>";
        }
        html += "</table>";
        html += "<p class='note'>PC音声（Wi-Fi）、LINE IN（Karipom Ear）、将来のBluetooth入力で<b>共通</b>のオーディオビジュアライザー設定です（入力元ごとの個別設定はありません）。内蔵マイクモードでは使用しません。選択内容はNVSへ保存され、再起動後も維持されます。</p>";
        html += "</details>";

        // 内蔵マイク詳細設定フォーム
        html += "<details" + String(audioSource == AUDIO_SRC_MIC ? " open" : "") + ">";
        html += "<summary style='cursor:pointer;font-weight:bold;'>🎛 内蔵マイク詳細設定</summary>";
        html += "<form method='GET' action='/mic_config' style='margin-top:8px;'>";
        html += "<table style='border-collapse:collapse;'>";
        html += "<tr><td style='padding:4px 8px;'>MIC ON threshold</td>";
        html += "<td style='padding:4px 8px;'><input type='number' name='on' value='" + String(cfg_micOnThreshold) + "' min='10' max='2000' style='width:80px;'></td>";
        html += "<td style='padding:4px;font-size:12px;color:#8aaccc;'>10〜2000 　デフォルト:150 　現在: " + String(cfg_micOnThreshold) + "</td></tr>";
        html += "<tr><td style='padding:4px 8px;'>MIC OFF threshold</td>";
        html += "<td style='padding:4px 8px;'><input type='number' name='off' value='" + String(cfg_micOffThreshold) + "' min='5' max='500' style='width:80px;'></td>";
        html += "<td style='padding:4px;font-size:12px;color:#8aaccc;'>5〜500 　デフォルト:50 　現在: " + String(cfg_micOffThreshold) + "</td></tr>";
        html += "<tr><td style='padding:4px 8px;'>Release hold ms</td>";
        html += "<td style='padding:4px 8px;'><input type='number' name='hold' value='" + String(cfg_micReleaseHoldMs) + "' min='100' max='5000' style='width:80px;'></td>";
        html += "<td style='padding:4px;font-size:12px;color:#8aaccc;'>100〜5000 　デフォルト:1000 　現在: " + String(cfg_micReleaseHoldMs) + "</td></tr>";
        html += "</table>";
        html += "<p style='margin:8px 0 0;'><button type='submit' class='onbtn'>💾 保存して反映</button></p>";
        html += "</form>";
        html += "<p class='note'>OFF &lt; ON になるよう自動チェックします。反映はすぐ有効になります。</p>";
        html += "</details>";

        html += "<hr>";
        html += "<h2>👀 KariPom Eyes</h2>";

        // カメラ処理のON/OFFトグル（現在状態を表示）
        html += "<p>";
        html += "📷 カメラ：";
        if (cfg_enableCamera) {
          html += "<b style='color:var(--led-ok);font-size:20px;'>● ON</b>";
        } else {
          html += "<b style='color:var(--led-err);font-size:20px;'>● OFF</b>";
        }
        html += "</p>";
        html += "<p>";
        html += "<a href='/camera_on'><button class='onbtn'" + String(cfg_enableCamera ? " style='font-weight:bold;border:2px solid green;'" : "") + ">📷 ON</button></a> ";
        html += "<a href='/camera_off'><button class='offbtn'" + String(!cfg_enableCamera ? " style='font-weight:bold;border:2px solid red;'" : "") + ">📷 OFF</button></a>";
        html += "</p>";


        if (cfg_enableCamera) {
          html += "<a href='/camera.bmp' target='_blank'>";
          html += "<img id='cam' src='/camera.bmp' width='320'>";
          html += "</a>";
        }

        // カメラONのときだけ自動更新する
        if (cfg_enableCamera) {
          html += R"rawliteral(
<script>
setInterval(function(){
  document.getElementById('cam').src =
    '/camera.bmp?t=' + Date.now();
}, 3000);
</script>
)rawliteral";
        } else {
          // カメラOFF時は /faces/ フォルダからランダムに1枚選んで表示
          String randomFace = "";
          File faceDir = SD.open("/faces");
          if (faceDir) {
            // ファイル数を数える
            int count = 0;
            File f = faceDir.openNextFile();
            while (f) {
              String n = String(f.name());
              if (!n.startsWith(".") && (n.endsWith(".png") || n.endsWith(".PNG"))) count++;
              f = faceDir.openNextFile();
            }
            faceDir.close();

            // ランダムにインデックスを選ぶ
            if (count > 0) {
              int pick = random(0, count);
              faceDir = SD.open("/faces");
              int idx = 0;
              f = faceDir.openNextFile();
              while (f) {
                String n = String(f.name());
                if (!n.startsWith(".") && (n.endsWith(".png") || n.endsWith(".PNG"))) {
                  if (idx == pick) { randomFace = n; break; }
                  idx++;
                }
                f = faceDir.openNextFile();
              }
              faceDir.close();
            }
          }

          if (randomFace.length() > 0) {
            html += "<p style='color:#aaa;font-size:12px;'>📷 カメラOFF中 — Face Galleryからランダム表示</p>";
            html += "<img src='/facefile?name=" + randomFace + "' width='320' style='border-radius:8px;'>";
            html += "<p style='color:#aaa;font-size:11px;'>" + randomFace + "</p>";
          } else {
            html += "<p style='color:#aaa;font-size:12px;'>📷 カメラOFF中 — /faces フォルダに画像がありません</p>";
          }
        }

        html += "<p class='note'>かりポムから見た世界</p>";

        // --- SD画像設定（睡眠中動体検知撮影）---
        html += "<hr>";
        html += "<h3 id='image-record'>📸 SD画像設定</h3>";
        html += "<p class='note'>睡眠中の動体検知で自動撮影。SDログとは独立して設定できます。<br>";
        html += "⚠️ Wi-Fi未接続環境では時刻が取得できないため画像記録はできません。</p>";
        html += "<p>";
        html += "SD画像記録：";
        if (cfg_enableToiletCam) {
          html += "<b style='color:var(--led-ok);font-size:20px;'>● ON</b>";
        } else {
          html += "<b style='color:var(--led-err);font-size:20px;'>● OFF</b>";
        }
        html += "</p>";
        html += "<p>";
        html += "<a href='/toilet_cam_on'><button class='onbtn'" + String(cfg_enableToiletCam ? " style='font-weight:bold;border:2px solid green;'" : "") + ">📷 ON</button></a> ";
        html += "<a href='/toilet_cam_off'><button class='offbtn'" + String(!cfg_enableToiletCam ? " style='font-weight:bold;border:2px solid red;'" : "") + ">📷 OFF</button></a>";
        html += "</p>";
        html += "<p>";
        html += "<a href='/toiletgallery'><button>📸 Gallery を開く</button></a>";
        html += "</p>";

        html += "<hr>";
        html += "<h2>🤖 Motion Gallery</h2>";

        html += "<p><b>Manual Control</b></p>";

        html += "<p>";
        html += "<button onclick=\"sendCmd('/head_up')\">⬆️ UP</button> ";
        html += "<button onclick=\"sendCmd('/head_down')\">⬇️ DOWN</button>";
        html += "</p>";

        html += "<p>";
        html += "<button onclick=\"sendCmd('/head_left')\">⬅️ LEFT</button> ";
        html += "<button onclick=\"sendCmd('/head_right')\">➡️ RIGHT</button>";
        html += "</p>";

        html += "<p>";
        html += "<button onclick=\"sendCmd('/center')\">🎯 CENTER</button> ";
        html += "<button onclick=\"sendCmd('/demo')\">🎬 DEMO</button>";
        html += "</p>";
        html += "<p>";
        html += "<button onclick=\"if(confirm('電源OFFの直前に押してください。サーボをセンターへ戻してdetachします。')) sendCmd('/servo_shutdown')\" style='background:#1a1200;border-color:#7a5a00;color:var(--led-warn);'>🔒 SERVO SHUTDOWN</button>";
        html += "</p>";

        html += "<p class='note'>";
        html += "⚙️ サーボの初期位置確認やキャリブレーションには、<br>";
        html += "Motion Gallery の CENTER が利用できます。";
        html += "</p>";

        // --- IDLE動作頻度設定 ---
        html += "<p>";
        html += "<label>🤖 IDLE動作頻度：</label> ";
        html += "<select onchange=\"fetch('/idle_cfg?level='+this.value)\">";
        html += "<option value='0'" + String(cfg_idleChance==0?" selected":"") + ">OFF</option>";
        html += "<option value='1'" + String(cfg_idleChance==1?" selected":"") + ">少なめ（2〜5分）</option>";
        html += "<option value='2'" + String(cfg_idleChance==2?" selected":"") + ">標準（45秒〜2分）</option>";
        html += "<option value='3'" + String(cfg_idleChance==3?" selected":"") + ">多め（15〜45秒）</option>";
        html += "</select>";
        html += "</p>";

        // ── ジョイスティック状態表示（2026/07/07・オプション扱い） ──
        html += "<p>";
        html += "🕹️ ジョイスティック：";
        if (joystickEnabled) {
          html += "<b style='color:var(--led-ok);font-size:18px;'>● ON</b>";
          html += " <span style='color:#8aaccc;font-size:12px;'>(centerX=" + String(joyCenterX)
               + " centerY=" + String(joyCenterY) + ")</span>";
        } else {
          html += "<b style='color:#aaa;font-size:18px;'>● OFF</b>";
          html += " <span style='color:#aaa;font-size:12px;'>（未接続 / 浮き検出 — 接続後に自動復帰）</span>";
        }
        html += "</p>";

        html += R"rawliteral(
<script>
function sendCmd(url) {
  fetch(url).catch(function(err) {
    console.log(err);
  });
}
</script>
)rawliteral";

        html += "</div></body></html>";

        server.send(200, "text/html; charset=UTF-8", html);
        webLogEnd("/", _t0);
      });

      server.on("/camera.bmp", HTTP_GET, []() {
        if (!latestBmp || latestBmpSize == 0) {
          server.send(404, "text/plain", "No image");
          return;
        }

        server.setContentLength(latestBmpSize);
        server.send(200, "image/bmp", "");

        WiFiClient client = server.client();
        client.write(latestBmp, latestBmpSize);
      });

      // ── 音声入力ソース切替（4択：off / udp / mic / line）──
      // セッションリセット・I2S切替・独り言抑制は setAudioSource() 内で一括処理。
      server.on("/audio_src", []() {
        String m = server.arg("m");
        AudioSource src;

        if      (m == "off")  src = AUDIO_SRC_OFF;
        else if (m == "udp")  src = AUDIO_SRC_UDP;
        else if (m == "mic")  src = AUDIO_SRC_MIC;
        else if (m == "line") src = AUDIO_SRC_LINEIN;
        else {
          server.send(400, "text/plain", "bad audio source: " + m);
          return;
        }

        setAudioSource(src, "WEB");
        saveConfig();  // 選択をNVSへ永続化（再起動後も維持）

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // ── 内蔵マイク閾値設定 ──
      // GETパラメータ: on=NNN&off=NNN&hold=NNN
      // 範囲チェック（on:10-2000 / off:5-500 / hold:100-5000）と OFF<ON チェックを行い、
      // 合格した値だけ即時反映＋NVS保存する。不合格はエラー文字列で 400 を返す。
      server.on("/mic_config", []() {
        String errMsg = "";

        int newOn   = server.arg("on").toInt();
        int newOff  = server.arg("off").toInt();
        int newHold = server.arg("hold").toInt();

        if (newOn   < 10   || newOn   > 2000) errMsg += "ON threshold must be 10-2000. ";
        if (newOff  < 5    || newOff  > 500)  errMsg += "OFF threshold must be 5-500. ";
        if (newHold < 100  || newHold > 5000) errMsg += "Release hold must be 100-5000. ";
        if (errMsg.length() == 0 && newOff >= newOn) errMsg += "OFF threshold must be less than ON threshold. ";

        if (errMsg.length() > 0) {
          server.send(400, "text/plain; charset=utf-8", errMsg);
          return;
        }

        cfg_micOnThreshold   = newOn;
        cfg_micOffThreshold  = newOff;
        cfg_micReleaseHoldMs = (unsigned long)newHold;

        saveConfig();

        addLog("MIC CFG: ON=" + String(cfg_micOnThreshold)
             + " OFF=" + String(cfg_micOffThreshold)
             + " HOLD=" + String(cfg_micReleaseHoldMs));

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // ── Visualizerモード選択（OFF / Graphic EQ / Audio Halo …）──
      // /visualizer?m=off|eq|halo   ※idは VIZ_MODES[] の定義と一致
      // モードを増やしてもこのハンドラは変更不要。
      // ── Lighting トグル（複数選択・ビットマスク）──
      // /lighting?m=<id>&v=on|off   ※idは LIGHT_MODES[] と一致
      // モードを増やしてもこのハンドラは変更不要。
      server.on("/lighting", []() {
        String m = server.arg("m");
        String v = server.arg("v");
        int found = -1;
        for (uint8_t li = 0; li < (uint8_t)LIGHT_MODE_COUNT; li++) {
          if (m == String(LIGHT_MODES[li].id)) { found = (int)li; break; }
        }
        if (found < 0) { server.send(400, "text/plain", "bad mode: " + m); return; }
        uint32_t bit = (uint32_t)(1u << found);
        // 🎲 Lighting RandomがON中にユーザーが個別選択した場合は、
        // RandomをOFFにし、Randomが残していた一時的な選択は捨てて
        // ユーザーの手動選択のみを反映する（相互排他）。
        if (cfg_lightingRandomOn) {
          cfg_lightingRandomOn = false;
          cfg_lightingMask = 0;
          addLog("LIGHTING RANDOM: OFF (manual select)");
        }
        if (v == "on")       cfg_lightingMask |= bit;
        else if (v == "off") cfg_lightingMask &= (uint32_t)~bit;
        else { server.send(400, "text/plain", "bad value: " + v); return; }
        cfg_lightingManualMask = cfg_lightingMask;  // 手動選択としてNVS保存対象へ反映
        // マスク変更時は合成を全面initし直す（背景切替時の残像防止・将来の差分描画モード対策）。
        // 現状の背景レンダラーは毎フレーム全面描画だが、Brightness変更と同様に安全側へ寄せる。
        gLightNeedReinit = true;
        saveConfig();
        addLog("LIGHTING SET: " + String(LIGHT_MODES[found].id) + "="
             + String((cfg_lightingMask & bit) ? "ON" : "OFF")
             + " mask=" + String(cfg_lightingMask));
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // ── Lighting共通 Brightness（全Lightingへ一括適用）──
      // /lighting_brightness?v=NN  （NN=10〜100）
      server.on("/lighting_brightness", []() {
        String v = server.arg("v");
        int nv = v.toInt();
        if (nv < 10 || nv > 100) { server.send(400, "text/plain", "bad value: " + v); return; }
        cfg_lightingBrightness = (uint8_t)nv;
        recomputeLightBrightness();     // Q8係数を再計算
        gLightNeedReinit = true;        // 差分描画でも即時反映させる
        saveConfig();
        addLog("LIGHTING BRIGHTNESS: " + String(cfg_lightingBrightness) + "% (Q8=" + String(gLightBriQ8) + ")");
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // 🎲 Lighting Random ON/OFF
      // /lighting_random?v=on|off
      server.on("/lighting_random", []() {
        String v = server.arg("v");
        if (v == "on") {
          if (!cfg_lightingRandomOn) {
            cfg_lightingRandomOn = true;
            cfg_lightingMask = 0;               // 個別選択のチェックをすべて解除
            lightingRandomLastSwitchMs = 0;      // 次回tickで待たずに1つ選ぶ
            lightingRandomLastPick = -1;
            gLightNeedReinit = true;
            addLog("LIGHTING RANDOM: ON (interval=" + String(cfg_lightingRandomIntervalMin) + "min)");
          }
        } else if (v == "off") {
          if (cfg_lightingRandomOn) {
            cfg_lightingRandomOn = false;
            cfg_lightingMask = cfg_lightingManualMask;  // 直前の手動選択へ復帰
            gLightNeedReinit = true;
            addLog("LIGHTING RANDOM: OFF (restored manual selection)");
          }
        } else { server.send(400, "text/plain", "bad value: " + v); return; }
        saveConfig();
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // 🎲 Lighting Random 切替間隔（5/10/15分のみ）
      // /lighting_random_interval?m=5|10|15
      server.on("/lighting_random_interval", []() {
        String m = server.arg("m");
        int nv = m.toInt();
        if (nv != 5 && nv != 10 && nv != 15) { server.send(400, "text/plain", "bad value: " + m); return; }
        cfg_lightingRandomIntervalMin = (uint8_t)nv;
        saveConfig();
        addLog("LIGHTING RANDOM INTERVAL: " + String(cfg_lightingRandomIntervalMin) + "min");
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      server.on("/visualizer", []() {
        String m = server.arg("m");
        int found = -1;
        for (uint8_t vi = 0; vi < (uint8_t)VIZ_MODE_COUNT; vi++) {
          if (m == String(VIZ_MODES[vi].id)) { found = (int)vi; break; }
        }
        if (found < 0) { server.send(400, "text/plain", "bad value: " + m); return; }
        // 🎲 Visualizer RandomがON中にユーザーが個別選択した場合は、
        // RandomをOFFにし、ユーザーの手動選択を優先する（相互排他）。
        if (cfg_vizRandomOn) {
          cfg_vizRandomOn = false;
          addLog("VISUALIZER RANDOM: OFF (manual select)");
        }
        cfg_visualizerMode    = (VisualizerMode)found;
        cfg_udpVisualizerFace = (cfg_visualizerMode != VIZ_MODE_OFF);  // 旧キーを同期
        cfg_vizManualMode     = cfg_visualizerMode;  // 手動選択としてNVS保存対象へ反映
        saveConfig();
        addLog("VISUALIZER MODE SET: " + String(VIZ_MODES[found].id));
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // 旧URL（互換維持）：ON → Graphic EQ / OFF → OFF
      // 既存のブックマーク・ショートカットを壊さないために残している。
      server.on("/udp_visualizer_face", []() {
        String v = server.arg("v");
        // 🎲 こちらも個別選択の一種のため、Visualizer RandomがONなら解除する。
        if (cfg_vizRandomOn) {
          cfg_vizRandomOn = false;
          addLog("VISUALIZER RANDOM: OFF (manual select - legacy)");
        }
        if (v == "on") {
          // 直前がOFFだった場合のみ Graphic EQ を選択する。
          // 既に Audio Halo 等を選んでいる環境で ON を押しても勝手にEQへ戻さない。
          if (cfg_visualizerMode == VIZ_MODE_OFF) cfg_visualizerMode = VIZ_MODE_EQ;
        } else if (v == "off") {
          cfg_visualizerMode = VIZ_MODE_OFF;
        } else { server.send(400, "text/plain", "bad value: " + v); return; }
        cfg_udpVisualizerFace = (cfg_visualizerMode != VIZ_MODE_OFF);
        cfg_vizManualMode     = cfg_visualizerMode;  // 手動選択としてNVS保存対象へ反映
        saveConfig();
        addLog("UDP VISUALIZER FACE(legacy): " + String(cfg_udpVisualizerFace ? "ON" : "OFF")
             + " -> MODE=" + String(VIZ_MODES[cfg_visualizerMode].id));
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // 🎲 Audio Visualizer Random ON/OFF
      // /visualizer_random?v=on|off
      server.on("/visualizer_random", []() {
        String v = server.arg("v");
        if (v == "on") {
          if (!cfg_vizRandomOn) {
            cfg_vizRandomOn = true;
            cfg_visualizerMode = VIZ_MODE_OFF;   // 個別選択を解除
            cfg_udpVisualizerFace = false;
            vizRandomLastSwitchMs = 0;            // 次回tickで待たずに1つ選ぶ
            vizRandomLastPick = -1;
            addLog("VISUALIZER RANDOM: ON (interval=" + String(cfg_vizRandomIntervalMin) + "min)");
          }
        } else if (v == "off") {
          if (cfg_vizRandomOn) {
            cfg_vizRandomOn = false;
            cfg_visualizerMode    = cfg_vizManualMode;  // 直前の手動選択へ復帰
            cfg_udpVisualizerFace = (cfg_visualizerMode != VIZ_MODE_OFF);
            addLog("VISUALIZER RANDOM: OFF (restored manual selection)");
          }
        } else { server.send(400, "text/plain", "bad value: " + v); return; }
        saveConfig();
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // 🎲 Audio Visualizer Random 切替間隔（5/10/15分のみ）
      // /visualizer_random_interval?m=5|10|15
      server.on("/visualizer_random_interval", []() {
        String m = server.arg("m");
        int nv = m.toInt();
        if (nv != 5 && nv != 10 && nv != 15) { server.send(400, "text/plain", "bad value: " + m); return; }
        cfg_vizRandomIntervalMin = (uint8_t)nv;
        saveConfig();
        addLog("VISUALIZER RANDOM INTERVAL: " + String(cfg_vizRandomIntervalMin) + "min");
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // ── キャラクタースタイル選択（KariPom / Miss KariPom）──
      server.on("/character_style", []() {
        String v = server.arg("v");
        if (v == "karipom") { cfg_characterStyle = CHARACTER_KARIPOM; }
        else if (v == "miss") { cfg_characterStyle = CHARACTER_MISS_KARIPOM; }
        else { server.send(400, "text/plain", "bad value: " + v); return; }
        saveConfig();
        addLog("CHARACTER STYLE: " + String(characterStyleName(cfg_characterStyle)));
        if (!imageFaceMode) drawOpenEyes();  // 即座に見た目へ反映
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // ── 🐰 KariPom Name（個体識別名）──
      // GETパラメータ: name=... （空文字列可）
      // 32文字を超える場合は切り詰めてNVSへ即保存する。
      server.on("/karipom_name", []() {
        String newName = server.arg("name");
        newName.trim();
        if (newName.length() > 32) newName = newName.substring(0, 32);

        cfg_karipomName = newName;
        saveConfig();

        addLog("KARIPOM NAME SET: " + String(cfg_karipomName.length() > 0 ? cfg_karipomName : String("(empty -> IP表示)")));

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // ── 旧ルート互換（ブックマーク・外部スクリプト対策）──
      server.on("/mac_audio_on", []() {
        setAudioSource(AUDIO_SRC_UDP, "WEB_LEGACY");
        saveConfig();
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      server.on("/mac_audio_off", []() {
        setAudioSource(AUDIO_SRC_OFF, "WEB_LEGACY");
        saveConfig();
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // ── 実行時設定トグル（config.txt との連動は再起動後） ──
      server.on("/camera_on", []() {
        cfg_enableCamera = true;
        saveConfig();
        addLog("CFG: CAMERA = ON");
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      server.on("/camera_off", []() {
        cfg_enableCamera = false;
        saveConfig();
        addLog("CFG: CAMERA = OFF");
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      server.on("/toilet_cam_on", []() {
        cfg_enableToiletCam = true;
        saveConfig();
        addLog("CFG: TOILET_CAM = ON");
        server.sendHeader("Location", "/files");
        server.send(302, "text/plain", "");
      });

      server.on("/toilet_cam_off", []() {
        cfg_enableToiletCam = false;
        saveConfig();
        addLog("CFG: TOILET_CAM = OFF");
        server.sendHeader("Location", "/files");
        server.send(302, "text/plain", "");
      });

      server.on("/sdlog_on", []() {
        cfg_enableSDLog = true;
        saveConfig();
        addLog("CFG: SD_LOG = ON");
        server.sendHeader("Location", "/logtoilet");
        server.send(302, "text/plain", "");
      });

      server.on("/sdlog_off", []() {
        cfg_enableSDLog = false;
        saveConfig();
        addLog("CFG: SD_LOG = OFF");
        server.sendHeader("Location", "/logtoilet");
        server.send(302, "text/plain", "");
      });

      server.on("/head_up", []() {
        if (servoBusy || webServoBusy) {
          addLog("WEB SERVO CMD: button=UP axis=UP_DOWN REJECTED (busy)");
          server.sendHeader("Location", "/");
          server.send(302, "text/plain", "");
          return;
        }

        unsigned long now = millis();
        if (lastWebServoButton.length() > 0 && now - lastWebServoCmdTime < WEB_SERVO_DUP_WINDOW_MS) {
          addLog("WEB SERVO CMD: button=UP axis=UP_DOWN DUPLICATE within "
               + String(now - lastWebServoCmdTime) + "ms of button=" + lastWebServoButton);
        }
        lastWebServoButton  = "UP";
        lastWebServoCmdTime = now;

        addLog("WEB SERVO CMD: button=UP axis=UP_DOWN target=" + String(HEAD_UP));

        webServoBusy = true;
        servoBusy = true;

        moveWithEyeLead(servoUD, udNow, HEAD_UP, 15,
                        0, -EYE_SHIFT_PIXELS, "UP");

        servoBusy = false;
        webServoBusy = false;

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      server.on("/head_down", []() {
        if (servoBusy || webServoBusy) {
          addLog("WEB SERVO CMD: button=DOWN axis=UP_DOWN REJECTED (busy)");
          server.sendHeader("Location", "/");
          server.send(302, "text/plain", "");
          return;
        }

        unsigned long now = millis();
        if (lastWebServoButton.length() > 0 && now - lastWebServoCmdTime < WEB_SERVO_DUP_WINDOW_MS) {
          addLog("WEB SERVO CMD: button=DOWN axis=UP_DOWN DUPLICATE within "
               + String(now - lastWebServoCmdTime) + "ms of button=" + lastWebServoButton);
        }
        lastWebServoButton  = "DOWN";
        lastWebServoCmdTime = now;

        addLog("WEB SERVO CMD: button=DOWN axis=UP_DOWN target=" + String(HEAD_DOWN));

        webServoBusy = true;
        servoBusy = true;

        moveWithEyeLead(servoUD, udNow, HEAD_DOWN, 15,
                        0, EYE_SHIFT_PIXELS, "DOWN");

        servoBusy = false;
        webServoBusy = false;

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      server.on("/head_left", []() {
        if (servoBusy || webServoBusy) {
          addLog("WEB SERVO CMD: button=LEFT axis=LEFT_RIGHT REJECTED (busy)");
          server.sendHeader("Location", "/");
          server.send(302, "text/plain", "");
          return;
        }

        unsigned long now = millis();
        if (lastWebServoButton.length() > 0 && now - lastWebServoCmdTime < WEB_SERVO_DUP_WINDOW_MS) {
          addLog("WEB SERVO CMD: button=LEFT axis=LEFT_RIGHT DUPLICATE within "
               + String(now - lastWebServoCmdTime) + "ms of button=" + lastWebServoButton);
        }
        lastWebServoButton  = "LEFT";
        lastWebServoCmdTime = now;

        addLog("WEB SERVO CMD: button=LEFT axis=LEFT_RIGHT target=" + String(HEAD_LEFT));

        webServoBusy = true;
        servoBusy = true;

        lrMoveCaller = "web_left";
        moveWithEyeLead(servoLR, lrNow, HEAD_LEFT, 20,
                        EYE_SHIFT_PIXELS, 0, "LEFT");

        servoBusy = false;
        webServoBusy = false;

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      server.on("/head_right", []() {
        if (servoBusy || webServoBusy) {
          addLog("WEB SERVO CMD: button=RIGHT axis=LEFT_RIGHT REJECTED (busy)");
          server.sendHeader("Location", "/");
          server.send(302, "text/plain", "");
          return;
        }

        unsigned long now = millis();
        if (lastWebServoButton.length() > 0 && now - lastWebServoCmdTime < WEB_SERVO_DUP_WINDOW_MS) {
          addLog("WEB SERVO CMD: button=RIGHT axis=LEFT_RIGHT DUPLICATE within "
               + String(now - lastWebServoCmdTime) + "ms of button=" + lastWebServoButton);
        }
        lastWebServoButton  = "RIGHT";
        lastWebServoCmdTime = now;

        addLog("WEB SERVO CMD: button=RIGHT axis=LEFT_RIGHT target=" + String(HEAD_RIGHT));

        webServoBusy = true;
        servoBusy = true;

        lrMoveCaller = "web_right";
        moveWithEyeLead(servoLR, lrNow, HEAD_RIGHT, 20,
                        -EYE_SHIFT_PIXELS, 0, "RIGHT");

        servoBusy = false;
        webServoBusy = false;

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      server.on("/center", []() {
        if (servoBusy || webServoBusy) {
          addLog("WEB SERVO CMD: button=CENTER axis=BOTH REJECTED (busy)");
          server.sendHeader("Location", "/");
          server.send(302, "text/plain", "");
          return;
        }

        unsigned long now = millis();
        if (lastWebServoButton.length() > 0 && now - lastWebServoCmdTime < WEB_SERVO_DUP_WINDOW_MS) {
          addLog("WEB SERVO CMD: button=CENTER axis=BOTH DUPLICATE within "
               + String(now - lastWebServoCmdTime) + "ms of button=" + lastWebServoButton);
        }
        lastWebServoButton  = "CENTER";
        lastWebServoCmdTime = now;

        addLog("WEB SERVO CMD: button=CENTER axis=BOTH target=("
             + String(HEAD_VERTICAL_CENTER) + "," + String(HEAD_HORIZONTAL_CENTER) + ")");

        webServoBusy = true;
        servoBusy = true;

        moveToCenterWithEyeLead(servoUD, udNow, HEAD_VERTICAL_CENTER, 15);
        lrMoveCaller = "web_center";
        moveToCenterWithEyeLead(servoLR, lrNow, HEAD_HORIZONTAL_CENTER, 20);

        servoBusy = false;
        webServoBusy = false;

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });

      // サーボ安全シャットダウン（電源OFFの直前にWebから呼ぶ）
      server.on("/servo_shutdown", []() {
        server.send(200, "text/plain", "Servo safe shutdown...");
        safeServoShutdown();
      });

      server.on("/demo", []() {
        if (servoBusy) {
          server.sendHeader("Location", "/");
          server.send(302, "text/plain", "");
          return;
        }

        servoBusy = true;

        servoDemoMotion();

        servoBusy = false;

        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      });


      server.on("/config", HTTP_GET, []() {
        unsigned long _t0 = webLogStart("/config");
        server.send(200, "text/html; charset=UTF-8", configPageHtml());
        webLogEnd("/config", _t0);
      });

      server.on("/save_config", HTTP_POST, []() {
        addLog("CONFIG SAVE REQUEST");

        String body = server.arg("body");
        bool ok = writeTextFileToSD(WEB_CONFIG_PATH, body);

        if (ok) {
          addLog("CONFIG SAVE OK");
          server.send(200, "text/html; charset=UTF-8", configPageHtml("保存しました。"));
        } else {
          addLog("CONFIG SAVE FAIL");
          server.send(500, "text/html; charset=UTF-8", configPageHtml("保存に失敗しました。SDカードを確認してください。"));
        }
      });

      server.on("/save_wifi", HTTP_POST, []() {
        addLog("WIFI CONFIG SAVE REQUEST");

        String body = server.arg("body");

        bool ok = writeTextFileToSD("/wifi.txt", body);

        if (ok) {

          addLog("WIFI CONFIG SAVE OK");

          String html;
          html.reserve(1200);
          html += "<html><head><meta charset='UTF-8'>";
          html += "<meta http-equiv='refresh' content='3;url=/config'>";
          html += "<style>:root{--bg:#0d0f12;--card:#14181e;--text:#cbd5e1;"
                  "--accent:#38bdf8;--led-ok:#4ade80;--border:#252d3a}"
                  "body{font-family:monospace;background:var(--bg);color:var(--text);"
                  "display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
                  ".card{background:var(--card);border:1px solid var(--border);border-radius:6px;"
                  "padding:28px 32px;text-align:center;max-width:360px}"
                  "h2{color:var(--led-ok);margin:0 0 12px}p{color:var(--text);margin:6px 0}"
                  "a{color:var(--accent);text-decoration:none}</style>";
          html += "</head><body><div class='card'>";
          html += "<h2>✔ Wi-Fi設定を保存しました</h2>";
          html += "<p>3秒後に Config へ戻ります。</p>";
          html += "<p>反映するには再起動してください。</p>";
          html += "<p style='margin-top:14px'><a href='/config'>← Config</a></p>";
          html += "</div></body></html>";

          server.send(200, "text/html; charset=UTF-8", html);

        } else {

          addLog("WIFI CONFIG SAVE FAIL");

          server.send(500, "text/plain", "Save failed");
        }
      });

      server.on("/confirm_delete_config", HTTP_GET, []() {
        String html;
        html.reserve(2000);
        html += "<html><head><meta charset='UTF-8'><title>KariPom Lab – Delete Config</title>";
        html += "<style>:root{--bg:#0d0f12;--card:#14181e;--text:#cbd5e1;"
                "--accent:#38bdf8;--led-err:#ef4444;--border:#252d3a}"
                "body{font-family:monospace;background:var(--bg);color:var(--text);"
                "display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
                ".card{background:var(--card);border:1px solid var(--border);border-radius:6px;"
                "padding:28px 32px;text-align:center;max-width:380px}"
                "h2{color:var(--led-err);margin:0 0 12px}p{margin:6px 0}"
                "button{font-size:18px;margin:10px 4px;padding:10px 18px;border-radius:4px;"
                "background:#220d0d;border:1px solid #5c1a1a;color:var(--led-err);cursor:pointer}"
                "a{color:var(--accent);text-decoration:none}</style>";
        html += "</head><body><div class='card'>";
        html += "<h2>⚠ DELETE /config.txt ?</h2>";
        html += "<p>この操作は元に戻せません。</p>";
        html += "<form method='POST' action='/delete_config'>";
        html += "<button type='submit'>DELETE NOW / 本当に削除</button>";
        html += "</form>";
        html += "<p style='margin-top:14px'><a href='/config'>← キャンセル</a></p>";
        html += "</div></body></html>";

        server.send(200, "text/html; charset=UTF-8", html);
      });

      server.on("/delete_config", HTTP_POST, []() {
        addLog("CONFIG DELETE REQUEST");

        bool ok = deleteFileFromSD(WEB_CONFIG_PATH);

        if (ok) {

          addLog("CONFIG DELETE OK");

          server.send(200, "text/html; charset=UTF-8",
                      configPageHtml("削除しました。"));

        } else {

          addLog("CONFIG DELETE FAIL");

          server.send(500, "text/html; charset=UTF-8",
                      configPageHtml("削除に失敗しました。SDカードを確認してください。"));
        }
      });

      server.on("/files", []() {
        unsigned long _t0 = webLogStart("/files");
        String html = karipomPageHeader("KariPom Lab – File Manager");

        html += "<h1>📂 File Manager</h1>";

        html += "<p>";
        html += "<a href='/'>🏠 Lab Home</a> / ";
        html += "<a href='/config'>⚙️ Config</a> / ";
        html += "<b>📂 File Manager</b> / ";
        html += "<a href='/logtoilet'>🚽 Log Toilet</a>";
        html += "</p>";

        html += "<div class='box'>";
        html += "<h3>🎭 Face Gallery</h3>";
        html += "<p>表情画像の管理</p>";
        html += "<a href='/faces'><button>🎨 顔を選ぶ</button></a> ";
        html += "<a href='/resetface'><button>🙂 標準の顔に戻す</button></a>";
        html += "</div>";

        html += "<div class='box'>";
        html += "<h3>🎵 Sound Manager</h3>";
        html += "<p>通常サウンド（効果音・システム音）の管理</p>";
        html += "<a href='/sounds'><button>OPEN</button></a>";
        html += "</div>";

        html += "<div class='box'>";
        html += "<h3>💬 Mutter Manager / ひとこと管理</h3>";
        html += "<p>ひとこと（mutter）音声の管理（/sounds/mutter/）</p>";
        html += "<a href='/mutter'><button>OPEN</button></a>";
        html += "</div>";

        html += "</body></html>";

        server.send(200, "text/html; charset=UTF-8", html);
        webLogEnd("/files", _t0);
      });

      server.on("/sounds", HTTP_GET, []() {
        unsigned long _t0 = webLogStart("/sounds");
        server.send(200, "text/html; charset=UTF-8", soundsPageHtml());
        webLogEnd("/sounds", _t0);
      });

      server.on(
        "/upload_sound", HTTP_POST, []() {
          addLog("SOUND UPLOAD POST END");

          if (soundUploadOk && soundUploadPath.length() > 0) {
            File verify = SD.open(soundUploadPath.c_str(), FILE_READ);
            if (verify) {
              addLog(
                "SOUND UPLOAD OK: " + soundUploadPath + " size=" + String(verify.size()));
              verify.close();
            } else {
              addLog("SOUND UPLOAD FAIL: " + soundUploadPath);
            }
          }

          // アップロード完了直後に同じPOST応答内で一覧を作ると、
          // SDディレクトリの再読み込みが不安定になることがあるため、
          // 一度 /sounds へリダイレクトして新規GETで一覧を読み直す。
          server.sendHeader("Location", "/sounds");
          server.sendHeader("Cache-Control", "no-store");
          server.send(303, "text/plain", "See Other");

          soundUploadPath = "";
          soundUploadOk = false;
        },
        []() {
          HTTPUpload& upload = server.upload();

          if (upload.status == UPLOAD_FILE_START) {
            String filename = sanitizeFilename(upload.filename);
            addLog("SOUND UPLOAD START: " + filename);

            soundUploadOk = false;
            soundUploadPath = "";

            if (!isWavFilename(filename)) {
              addLog("SOUND UPLOAD REJECTED: not wav");
              return;
            }

            if (!SD.exists("/sounds")) {
              SD.mkdir("/sounds");
            }

            soundUploadPath = "/sounds/" + filename;

            if (SD.exists(soundUploadPath.c_str())) {
              SD.remove(soundUploadPath.c_str());
            }

            soundUploadFile = SD.open(soundUploadPath.c_str(), FILE_WRITE);
            if (!soundUploadFile) {
              addLog("SOUND UPLOAD OPEN FAILED");
              soundUploadPath = "";
              return;
            }

            soundUploadOk = true;

          } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (soundUploadFile) {
              soundUploadFile.write(upload.buf, upload.currentSize);
            }

          } else if (upload.status == UPLOAD_FILE_END) {
            if (soundUploadFile) {
              soundUploadFile.close();
            }
            addLog("SOUND UPLOAD END: " + String(upload.totalSize) + " bytes");

          } else if (upload.status == UPLOAD_FILE_ABORTED) {
            if (soundUploadFile) {
              soundUploadFile.close();
            }
            if (soundUploadPath.length() > 0 && SD.exists(soundUploadPath.c_str())) {
              SD.remove(soundUploadPath.c_str());
            }
            soundUploadOk = false;
            addLog("SOUND UPLOAD ABORTED");
          }
        });

      server.on("/confirm_delete_sound", HTTP_GET, []() {
        String filename = sanitizeFilename(server.arg("name"));
        String path = "/sounds/" + filename;

        String html = "<html><head><meta charset='UTF-8'><title>KariPom Lab – Delete Sound</title>";
        html += "<style>:root{--bg:#0d0f12;--card:#14181e;--text:#cbd5e1;--accent:#38bdf8;--led-err:#ef4444;--border:#252d3a}"
          "body{font-family:monospace;background:var(--bg);color:var(--text);padding:24px;line-height:1.6}"
          "h1{color:var(--led-err);border-bottom:1px solid var(--border);padding-bottom:4px;font-size:18px}"
          "b{color:var(--accent)}"
          "button{font-size:20px;margin:8px;padding:10px 20px;border-radius:4px;cursor:pointer}"
          ".del{background:#220d0d;border:1px solid #5c1a1a;color:var(--led-err)}"
          ".del:hover{background:#2e1111;border-color:#7a2525}"
          "a{color:var(--accent);text-decoration:none}</style>";
        html += "</head><body>";
        html += "<h1>DELETE WAV ?</h1>";
        html += "<p>対象: <b>" + htmlEscape(path) + "</b></p>";
        html += "<p>この操作は元に戻せません。</p>";
        html += "<form method='POST' action='/delete_sound'>";
        html += "<input type='hidden' name='name' value='" + htmlEscape(filename) + "'>";
        html += "<button class='del' type='submit'>DELETE NOW / 本当に削除</button>";
        html += "</form>";
        html += "<p><a href='/sounds'>キャンセル</a></p>";
        html += "</body></html>";
        server.send(200, "text/html; charset=UTF-8", html);
      });

      server.on("/delete_sound", HTTP_POST, []() {
        String filename = sanitizeFilename(server.arg("name"));

        addLog("SOUND DELETE REQUEST: " + filename);

        if (filename.length() == 0 || !isWavFilename(filename)) {
          addLog("SOUND DELETE INVALID FILENAME");
          server.send(400, "text/html; charset=UTF-8",
                      soundsPageHtml("削除できません。ファイル名が不正です。"));
          return;
        }

        String path = "/sounds/" + filename;
        bool ok = false;

        if (SD.exists(path.c_str())) {
          ok = SD.remove(path.c_str());
        }

        if (ok) {

          addLog("SOUND DELETE OK: " + path);

          server.send(200, "text/html; charset=UTF-8",
                      soundsPageHtml("削除しました: " + path));

        } else {

          addLog("SOUND DELETE FAIL: " + path);

          server.send(500, "text/html; charset=UTF-8",
                      soundsPageHtml("削除に失敗しました。"));
        }
      });

      // ==================================================
      // Mutter Manager（ひとこと管理）ルート群
      // 通常サウンドの /sounds 系ルートを踏襲し、/sounds/mutter/ 配下に限定する。
      // ==================================================

      // 一覧ページ（ページング＋チャンク送信）
      server.on("/mutter", HTTP_GET, []() {
        int page = 0;
        if (server.hasArg("page")) page = server.arg("page").toInt();
        if (page < 0) page = 0;
        sendMutterPage(page);
      });

      // 呟き発生確率・顔出現確率の設定保存
      server.on("/mutter_cfg", HTTP_GET, []() {
        bool changed = false;
        if (server.hasArg("chance")) {
          int v = server.arg("chance").toInt();
          if (v == 0 || v == 5 || v == 10 || v == 20 || v == 30) {
            cfg_mutterChance = v;
            changed = true;
          }
        }
        if (server.hasArg("face")) {
          int v = server.arg("face").toInt();
          if (v == 0 || v == 10 || v == 20 || v == 25 || v == 50 || v == 100) {
            cfg_mutterFaceChance = v;
            changed = true;
          }
        }
        if (changed) {
          saveConfig();
          addLog("MUTTER CFG: chance=" + String(cfg_mutterChance)
               + " face=" + String(cfg_mutterFaceChance));
        }
        server.sendHeader("Location", "/mutter");
        server.send(302, "text/plain", "");
      });

      // IDLE動作頻度の設定保存（Motion Gallery のドロップダウンから fetch で呼ぶ）
      server.on("/idle_cfg", HTTP_GET, []() {
        if (server.hasArg("level")) {
          int v = server.arg("level").toInt();
          if (v >= 0 && v <= 3) {
            cfg_idleChance = v;
            saveConfig();
            addLog("MOTION CFG: idleChance=" + String(cfg_idleChance));
          }
        }
        server.send(200, "text/plain", "ok");
      });

      // 旧形式 mutter_***.wav の件数確認（重い全走査はこのボタンでのみ実行）
      server.on("/mutter_legacy", HTTP_GET, []() {
        addLog("MUTTER LEGACY CHECK START");
        int legacyCount = 0;
        File sroot = SD.open("/sounds");
        if (sroot) {
          File f = sroot.openNextFile();
          while (f) {
            if (!f.isDirectory()) {
              String n = String(f.name());
              int s = n.lastIndexOf('/');
              if (s >= 0) n = n.substring(s + 1);
              if (n.startsWith("mutter_") && isWavFilename(n)) legacyCount++;
            }
            f = sroot.openNextFile();
          }
          sroot.close();
        }
        addLog("MUTTER LEGACY CHECK END count=" + String(legacyCount));

        String html = karipomPageHeader("KariPom Lab – Mutter Legacy Check");
        html += "<h1>🔍 旧形式 mutter チェック</h1>";
        html += "<p><a href='/mutter'>💬 Mutter Manager</a> / <a href='/sounds'>🎵 Sound Manager</a></p>";
        html += "<div class='box'>";
        html += "<p>旧形式 <b>/sounds/mutter_***.wav</b> の件数: <b>" + String(legacyCount) + "</b> 件</p>";
        if (legacyCount > 0) {
          html += "<p class='note' style='color:#e67e00;'>⚠️ /sounds/mutter/ にWAVが1件以上あれば新方式が優先されます（案A）。<br>";
          html += "旧ファイルは /sounds/mutter/ へ移動するか、Sound Managerで整理してください。</p>";
        } else {
          html += "<p class='note'>旧形式ファイルはありません。</p>";
        }
        html += "</div>";
        html += karipomPageFooter();
        server.send(200, "text/html; charset=UTF-8", html);
      });

      // アップロード（/sounds/mutter/ へ保存）
      server.on(
        "/upload_mutter", HTTP_POST, []() {
          addLog("MUTTER UPLOAD POST END");

          if (mutterUploadOk && mutterUploadPath.length() > 0) {
            File verify = SD.open(mutterUploadPath.c_str(), FILE_READ);
            if (verify) {
              addLog("MUTTER UPLOAD OK: " + mutterUploadPath + " size=" + String(verify.size()));
              verify.close();
            } else {
              addLog("MUTTER UPLOAD FAIL: " + mutterUploadPath);
            }
            // 一覧が変わったのでキャッシュを更新
            rebuildMutterCache();
          }

          server.sendHeader("Location", "/mutter");
          server.sendHeader("Cache-Control", "no-store");
          server.send(303, "text/plain", "See Other");

          mutterUploadPath = "";
          mutterUploadOk = false;
        },
        []() {
          HTTPUpload& upload = server.upload();

          if (upload.status == UPLOAD_FILE_START) {
            String filename = sanitizeFilename(upload.filename);
            addLog("MUTTER UPLOAD START: " + filename);

            mutterUploadOk = false;
            mutterUploadPath = "";

            if (!isWavFilename(filename)) {
              addLog("MUTTER UPLOAD REJECTED: not wav");
              return;
            }

            if (!SD.exists("/sounds")) SD.mkdir("/sounds");
            if (!SD.exists("/sounds/mutter")) SD.mkdir("/sounds/mutter");

            mutterUploadPath = "/sounds/mutter/" + filename;

            if (SD.exists(mutterUploadPath.c_str())) {
              SD.remove(mutterUploadPath.c_str());
            }

            mutterUploadFile = SD.open(mutterUploadPath.c_str(), FILE_WRITE);
            if (!mutterUploadFile) {
              addLog("MUTTER UPLOAD OPEN FAILED");
              mutterUploadPath = "";
              return;
            }

            mutterUploadOk = true;

          } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (mutterUploadFile) {
              mutterUploadFile.write(upload.buf, upload.currentSize);
            }

          } else if (upload.status == UPLOAD_FILE_END) {
            if (mutterUploadFile) {
              mutterUploadFile.close();
            }
            addLog("MUTTER UPLOAD END: " + String(upload.totalSize) + " bytes");

          } else if (upload.status == UPLOAD_FILE_ABORTED) {
            if (mutterUploadFile) {
              mutterUploadFile.close();
            }
            if (mutterUploadPath.length() > 0 && SD.exists(mutterUploadPath.c_str())) {
              SD.remove(mutterUploadPath.c_str());
            }
            mutterUploadOk = false;
            addLog("MUTTER UPLOAD ABORTED");
          }
        });

      // Listen（ブラウザ再生用にWAVをそのまま返す）
      server.on("/mutterfile", []() {
        String filename = sanitizeFilename(server.arg("name"));
        String path = "/sounds/mutter/" + filename;

        File file = SD.open(path);
        if (!file) {
          server.send(404, "text/plain", "FILE NOT FOUND");
          return;
        }
        server.streamFile(file, "audio/wav");
        file.close();
      });

      // Speak（本体スピーカーで発話テスト）
      server.on("/speak_mutter", []() {
        String filename = sanitizeFilename(server.arg("name"));
        String path = "/sounds/mutter/" + filename;

        addLog("WEB SPEAK MUTTER: " + path);

        path.toCharArray(speakPath, sizeof(speakPath));
        speakRequested = true;

        server.send(200, "text/html; charset=UTF-8",
                    "<html><head><meta charset='UTF-8'></head><body>"
                    "<h1>🐰 Speak Queued</h1>"
                    "<p><a href='/mutter'>Back</a></p>"
                    "</body></html>");
      });

      // 削除確認
      server.on("/confirm_delete_mutter", HTTP_GET, []() {
        String filename = sanitizeFilename(server.arg("name"));
        String path = "/sounds/mutter/" + filename;

        String html = "<html><head><meta charset='UTF-8'><title>KariPom Lab – Delete Mutter</title>";
        html += "<style>:root{--bg:#0d0f12;--card:#14181e;--text:#cbd5e1;--accent:#38bdf8;--led-err:#ef4444;--border:#252d3a}"
          "body{font-family:monospace;background:var(--bg);color:var(--text);padding:24px;line-height:1.6}"
          "h1{color:var(--led-err);border-bottom:1px solid var(--border);padding-bottom:4px;font-size:18px}"
          "b{color:var(--accent)}"
          "button{font-size:20px;margin:8px;padding:10px 20px;border-radius:4px;cursor:pointer}"
          ".del{background:#220d0d;border:1px solid #5c1a1a;color:var(--led-err)}"
          ".del:hover{background:#2e1111;border-color:#7a2525}"
          "a{color:var(--accent);text-decoration:none}</style>";
        html += "</head><body>";
        html += "<h1>DELETE MUTTER WAV ?</h1>";
        html += "<p>対象: <b>" + htmlEscape(path) + "</b></p>";
        html += "<p>この操作は元に戻せません。</p>";
        html += "<form method='POST' action='/delete_mutter'>";
        html += "<input type='hidden' name='name' value='" + htmlEscape(filename) + "'>";
        html += "<button class='del' type='submit'>DELETE NOW / 本当に削除</button>";
        html += "</form>";
        html += "<p><a href='/mutter'>キャンセル</a></p>";
        html += "</body></html>";
        server.send(200, "text/html; charset=UTF-8", html);
      });

      // 削除実行
      server.on("/delete_mutter", HTTP_POST, []() {
        String filename = sanitizeFilename(server.arg("name"));

        addLog("MUTTER DELETE REQUEST: " + filename);

        if (filename.length() == 0 || !isWavFilename(filename)) {
          addLog("MUTTER DELETE INVALID FILENAME");
          sendMutterPage(0, "削除できません。ファイル名が不正です。");
          return;
        }

        String path = "/sounds/mutter/" + filename;
        bool ok = false;

        if (SD.exists(path.c_str())) {
          ok = SD.remove(path.c_str());
        }

        if (ok) {
          addLog("MUTTER DELETE OK: " + path);
          rebuildMutterCache();  // 一覧が変わったのでキャッシュを更新
          sendMutterPage(0, "削除しました: " + path);
        } else {
          addLog("MUTTER DELETE FAIL: " + path);
          sendMutterPage(0, "削除に失敗しました。");
        }
      });

      server.on("/logtoiletview", []() {
        String which = server.arg("file");
        String path;
        if (which == "prev") {
          path = "/logtoilet/karipom_prev.log";
        } else if (which == "text") {
          path = "/logtoilet/toilet_log.txt";
        } else {
          path = LOG_TOILET_FILE;  // current (karipom.log)
        }
        File file = SD.open(path.c_str(), FILE_READ);
        if (!file) {
          server.send(404, "text/plain; charset=UTF-8", "🚽 (empty or not found): " + path);
          return;
        }
        server.streamFile(file, "text/plain; charset=UTF-8");
        file.close();
      });

      // ===== ページング版ログビューア（/logview） =====
      // 例: /logview?file=current&page=1&lines=100
      //   file : current(karipom.log) / prev(karipom_prev.log) / text(toilet_log.txt)
      //   page : 1始まり（1が最新ページ）
      //   lines: 1ページ行数（既定100, 10〜500）
      server.on("/logview", []() {
        String which = server.arg("file");
        const char* path;
        const char* label;
        if (which == "prev") {
          path = "/logtoilet/karipom_prev.log"; label = "Previous (karipom_prev.log)";
        } else if (which == "text") {
          path = "/logtoilet/toilet_log.txt";   label = "Event (toilet_log.txt)";
        } else {
          path = LOG_TOILET_FILE;               label = "Current (karipom.log)";
        }

        int page  = server.hasArg("page")  ? server.arg("page").toInt()  : 1;
        int lines = server.hasArg("lines") ? server.arg("lines").toInt() : 100;
        if (page < 1) page = 1;

        sendLogPage(path, label, page, lines);
      });

      // ===== Log Toilet Center (/logtoilet) =====
      server.on("/logtoilet", []() {
        unsigned long _t0 = webLogStart("/logtoilet");
        String html = karipomPageHeader("KariPom Lab – Log Toilet");
        html += "<h1>🚽 Log Toilet</h1>";
        html += "<p><a href='/'>🏠 Lab Home</a> / ";
        html += "<a href='/config'>⚙️ Config</a> / ";
        html += "<a href='/files'>📂 File Manager</a> / ";
        html += "<b>🚽 Log Toilet</b></p>";

        // --- SDログ設定 ---
        html += "<div class='box'>";
        html += "<h3>💾 SDログ設定</h3>";
        html += "<p class='note'>システムログの記録を制御します。OFFにするとログファイルへの書き込みが停止します。</p>";
        if (cfg_enableSDLog) {
          html += "<p>現在：<b style='color:var(--led-ok);font-size:20px;'>● ON</b></p>";
        } else {
          html += "<p>現在：<b style='color:var(--led-err);font-size:20px;'>● OFF</b></p>";
          html += "<p style='color:#e67e00;'>⚠️ OFFのためログファイルは更新されません。</p>";
        }
        html += "<p>";
        html += "<a href='/sdlog_on'><button class='onbtn'" + String(cfg_enableSDLog ? " style='font-weight:bold;border:2px solid green;'" : "") + ">💾 ON</button></a> ";
        html += "<a href='/sdlog_off'><button class='offbtn'" + String(!cfg_enableSDLog ? " style='font-weight:bold;border:2px solid red;'" : "") + ">💾 OFF</button></a>";
        html += "</p>";
        html += "</div>";

        // ファイルサイズ表示用ヘルパー
        auto fileSizeStr = [](const char* fpath) -> String {
          if (!SD.exists(fpath)) return "(なし)";
          File ff = SD.open(fpath, FILE_READ);
          if (!ff) return "(開けない)";
          size_t sz = ff.size();
          ff.close();
          if (sz == 0) return "0 B";
          if (sz < 1024) return String(sz) + " B";
          return String(sz / 1024) + " KB";
        };

        // --- Current Log ---
        html += "<div class='box'>";
        html += "<h3>💩 Current Log &nbsp;<small style='color:#8aaccc;font-weight:normal;'>karipom.log</small></h3>";
        html += "<p class='note'>現在起動中のログ</p>";
        html += "<p>サイズ：<b>" + fileSizeStr("/logtoilet/karipom.log") + "</b></p>";
        html += "<a href='/logview?file=current&page=1&lines=100' target='_blank'><button class='play'>💩 View (paged)</button></a> ";
        html += "<a href='/logtoiletview?file=current' target='_blank'><button>💩 Raw</button></a> ";
        html += "<a href='/compress_current_log' ";
        html += "onclick=\"return confirm('Current Log を末尾 " + String(COMPRESS_MAX_LINES) + " 行に圧縮します。\\n古い行は削除されます。続行しますか？');\">";
        html += "<button class='play'>📦 Compress</button></a> ";
        html += "<a href='/clear_current_log' ";
        html += "onclick=\"return confirm('Current Log (karipom.log) を削除します。\\nPrevious Log / Event Log には影響しません。\\n続行しますか？');\">";
        html += "<button class='del'>🗑 Clear</button></a>";
        html += "</div>";

        // --- Previous Log ---
        html += "<div class='box'>";
        html += "<h3>💩 Previous Log &nbsp;<small style='color:#8aaccc;font-weight:normal;'>karipom_prev.log</small></h3>";
        html += "<p class='note'>過去の起動ログを追記・長期保存（最新 " + String(PREV_LOG_MAX_LINES) + " 行）</p>";
        html += "<p>サイズ：<b>" + fileSizeStr("/logtoilet/karipom_prev.log") + "</b></p>";
        html += "<a href='/logview?file=prev&page=1&lines=100' target='_blank'><button class='play'>💩 View (paged)</button></a> ";
        html += "<a href='/logtoiletview?file=prev' target='_blank'><button>💩 Raw</button></a> ";
        html += "<a href='/compress_prev_log' ";
        html += "onclick=\"return confirm('Previous Log を末尾 " + String(COMPRESS_MAX_LINES) + " 行に圧縮します。\\n古い行は削除されます。続行しますか？');\">";
        html += "<button class='play'>📦 Compress</button></a> ";
        html += "<a href='/clear_prev_log' ";
        html += "onclick=\"return confirm('Previous Log (karipom_prev.log) を削除します。\\nCurrent Log / Event Log には影響しません。\\n続行しますか？');\">";
        html += "<button class='del'>🗑 Clear</button></a>";
        html += "</div>";

        // --- Event Log ---
        html += "<div class='box'>";
        html += "<h3>📋 Event Log &nbsp;<small style='color:#8aaccc;font-weight:normal;'>toilet_log.txt</small></h3>";
        html += "<p class='note'>起動・動体検知イベントの記録（追記・長期保存）</p>";
        html += "<p>サイズ：<b>" + fileSizeStr("/logtoilet/toilet_log.txt") + "</b></p>";
        html += "<a href='/logview?file=text&page=1&lines=100' target='_blank'><button class='play'>📋 View (paged)</button></a> ";
        html += "<a href='/logtoiletview?file=text' target='_blank'><button>📋 Raw</button></a> ";
        html += "<a href='/compress_event_log' ";
        html += "onclick=\"return confirm('Event Log を末尾 " + String(COMPRESS_MAX_LINES) + " 行に圧縮します。\\n古い行は削除されます。続行しますか？');\">";
        html += "<button class='play'>📦 Compress</button></a> ";
        html += "<a href='/clear_toilet_textlog' ";
        html += "onclick=\"return confirm('Event Log (toilet_log.txt) を削除します。\\nCurrent Log / Previous Log には影響しません。\\n続行しますか？');\">";
        html += "<button class='del'>🗑 Clear</button></a>";
        html += "</div>";

        html += karipomPageFooter();
        server.send(200, "text/html; charset=UTF-8", html);
        webLogEnd("/logtoilet", _t0);
      });

      server.on("/soundfile", []() {
        String filename = sanitizeFilename(server.arg("name"));

        String path = "/sounds/" + filename;

        File file = SD.open(path);

        if (!file) {
          server.send(404, "text/plain", "FILE NOT FOUND");
          return;
        }

        server.streamFile(file, "audio/wav");

        file.close();
      });

      server.on("/speak", []() {
        String filename = sanitizeFilename(server.arg("name"));
        String path = "/sounds/" + filename;

        addLog("WEB SPEAK: " + path);

        path.toCharArray(speakPath, sizeof(speakPath));
        speakRequested = true;

        server.send(200, "text/html; charset=UTF-8",
                    "<html><head><meta charset='UTF-8'></head><body>"
                    "<h1>🐰 Speak Queued</h1>"
                    "<p><a href='/sounds'>Back</a></p>"
                    "</body></html>");
      });

      server.on("/faces", []() {
        unsigned long _t0 = webLogStart("/faces");
        File root = SD.open("/faces");

        String html = karipomPageHeader("KariPom Lab – Face Gallery");

        html += "<h1>🎭 Face Gallery</h1>";

        html += "<p>";
        // 2026/07/21: 最上部ナビを共通4項目へ統一（Log Toiletを追加）
        html += "<a href='/'>🏠 Lab Home</a> / ";
        html += "<a href='/config'>⚙️ Config</a> / ";
        html += "<a href='/files'>📂 File Manager</a> / ";
        html += "<a href='/logtoilet'>🚽 Log Toilet</a>";
        html += "</p>";

        html += "<p class='note'>";
        html += "※ 表情画像は PNG ファイルだけアップロードできます。<br>";
        html += "※ PNG形式・320×240px 推奨。CoreS3の画面サイズに合わせる場合は 320×240px で作成してください。<br>";
        html += "※ リセットすると標準の表情に戻ります。";
        html += "</p>";

        if (!root) {
          html += "<div class='box'>/faces folder not found.</div>";
        } else {
          File file = root.openNextFile();

          while (file) {
            String name = String(file.name());

            if (!name.startsWith(".")) {
              html += "<div class='box' style='display:inline-block; margin:10px; text-align:center;'>";
              html += "<a href='/facefile?name=" + name + "' target='_blank'>";
              html += "<img src='/facefile?name=" + name + "' width='120'>";
              html += "</a><br>";
              html += name;

              html += "<br>";
              html += "<a href='/showface?name=" + name + "'><button>🎭 Wear</button></a>";
              html += "</div>";
            }

            handleNoseMotion();  // 長いSD走査中でも鼻アニメを止めない（自己ゲート付き・低コスト）
            file = root.openNextFile();
          }

          root.close();
        }

        html += "</body></html>";

        server.send(200, "text/html; charset=UTF-8", html);
        webLogEnd("/faces", _t0);
      });

      server.on("/facefile", []() {
        String filename = server.arg("name");

        String path = "/faces/" + filename;

        File file = SD.open(path);

        if (!file) {
          server.send(404, "text/plain", "FILE NOT FOUND");
          return;
        }

        server.streamFile(file, "image/png");

        file.close();
      });

      server.on("/showface", []() {
        String filename = server.arg("name");
        String path = "/faces/" + filename;

        addLog("SHOW FACE: " + path);

        drawFaceImage(path.c_str());

        server.sendHeader("Location", "/faces");
        server.send(302, "text/plain", "");
      });

      // /resetface: Web顔固定（imageFaceMode=true）を解除し、標準表情管理に復帰する。
      // drawFace() は imageFaceMode=false にした上で標準顔を描画するため、
      // 以降 瞬き・sleep・mutter・口パク等の通常表情制御がそのまま再開される。
      // /faces や /showface の挙動は一切変更しない。
      server.on("/resetface", []() {
        addLog("RESET FACE: imageFaceMode -> false");
        drawFace();   // imageFaceMode=false & 標準顔を即時描画
        showSensors();
        server.sendHeader("Location", "/files");
        server.send(302, "text/plain", "");
      });

      server.on("/toiletgallery", []() {
        File root = SD.open("/logtoilet");

        String html = karipomPageHeader("KariPom Lab – 画像記録");

        html += "<h1>📸 画像記録</h1>";

        html += "<p>";
        html += "<a href='/'>🏠 Lab Home</a> / ";
        html += "<a href='/config'>⚙️ Config</a> / ";
        html += "<a href='/files'>📂 File Manager</a> / ";
        html += "<a href='/logtoilet'>🚽 Log Toilet</a>";

        html += " / ";
        html += "<a href='/delete_all_toilet' onclick=\"return confirm('画像記録をすべて削除しますか？');\">🔥 Delete All Images</a>";

        html += "</p>";

        if (!root) {
          html += "<div class='box'>/logtoilet folder not found.</div>";
        } else {
          File file = root.openNextFile();

          while (file) {
            String name = String(file.name());

            if (!file.isDirectory() && !name.startsWith(".") && name.endsWith(".bmp")) {
              html += "<div class='box' style='display:inline-block; margin:10px; text-align:center;'>";
              html += "<a href='/toiletfile?name=" + name + "' target='_blank'>";
              html += "<img src='/toiletfile?name=" + name + "' width='160'>";
              html += "</a><br>";
              html += name;
              html += "<br>";

              html += "<a href='/delete_toilet?name=";
              html += name;
              html += "' onclick=\"return confirm('このうんち画像を削除しますか？');\">🗑 Delete</a>";

              html += "</div>";
            }

            file = root.openNextFile();
          }

          root.close();
        }

        html += "</body></html>";

        server.send(200, "text/html; charset=UTF-8", html);
      });

      server.on("/delete_toilet", []() {
        String filename = server.arg("name");

        filename = sanitizeFilename(filename);

        if (filename.length() == 0 || !filename.endsWith(".bmp")) {
          server.send(400, "text/plain", "BAD FILE NAME");
          return;
        }

        String path = "/logtoilet/" + filename;

        addLog("DELETE TOILET: " + path);

        if (SD.exists(path)) {
          SD.remove(path);
        }

        server.sendHeader("Location", "/toiletgallery");
        server.send(302, "text/plain", "");
      });

      server.on("/toiletfile", []() {
        String filename = server.arg("name");

        filename = sanitizeFilename(filename);

        String path = "/logtoilet/" + filename;

        File file = SD.open(path);

        if (!file) {

          server.send(404, "text/plain", "FILE NOT FOUND");

          return;
        }

        server.streamFile(file, "image/bmp");

        file.close();
      });

      server.on("/delete_all_toilet", []() {
        File root = SD.open("/logtoilet");

        addLog("DELETE ALL TOILET IMAGES");

        if (root) {
          File file = root.openNextFile();

          while (file) {
            String name = String(file.name());

            if (!file.isDirectory() && !name.startsWith(".") && name.endsWith(".bmp")) {
              String path = "/logtoilet/" + name;
              addLog("DELETE TOILET: " + path);
              SD.remove(path);
            }

            file = root.openNextFile();
          }

          root.close();
        }

        server.sendHeader("Location", "/toiletgallery");
        server.send(302, "text/plain", "");
      });

      server.on("/clear_toilet_textlog", []() {
        addLog("CLEAR TOILET TEXT LOG");

        const char* textLogPath = "/logtoilet/toilet_log.txt";
        if (SD.exists(textLogPath)) {
          SD.remove(textLogPath);
        }

        server.sendHeader("Location", "/logtoilet");
        server.send(302, "text/plain", "");
      });

      server.on("/clear_prev_log", []() {
        addLog("CLEAR PREVIOUS LOG");

        const char* prevLogPath = "/logtoilet/karipom_prev.log";
        if (SD.exists(prevLogPath)) {
          SD.remove(prevLogPath);
        }

        server.sendHeader("Location", "/logtoilet");
        server.send(302, "text/plain", "");
      });

      server.on("/clear_current_log", []() {
        // 2026/07/17: Clear実行の約30秒後に削除前のログが復活する不具合の修正。
        // 原因はSDファイル削除のみでRAMログバッファ（sdLogBuffer/sdLogHead/
        // sdLogCount）をクリアしておらず、削除後も定期フラッシュ（30秒ごと）
        // でバッファに残ったクリア前のログがkaripom.logへ再書き込みされていたため。
        // ここでバッファと未Flushフラグを完全にリセットし、Clear後は
        // ゼロから記録が始まるようにする。addLog()はここでは呼ばない
        // （呼ぶとバッファに新しい行が積まれてしまうため）。
        const char* curLogPath = "/logtoilet/karipom.log";
        if (SD.exists(curLogPath)) {
          SD.remove(curLogPath);
        }

        sdLogHead       = 0;
        sdLogCount      = 0;
        sdLogNeedsFlush = false;
        lastSdFlushTime = millis();  // クリア直後から30秒の定期フラッシュ間隔を数え直す

        server.sendHeader("Location", "/logtoilet");
        server.send(302, "text/plain", "");
      });

      server.on("/compress_current_log", []() {
        String result = compressLogFile("/logtoilet/karipom.log", COMPRESS_MAX_LINES);
        addLog("COMPRESS CURRENT LOG: " + result);
        server.sendHeader("Location", "/logtoilet");
        server.send(302, "text/plain", "");
      });

      server.on("/compress_prev_log", []() {
        String result = compressLogFile("/logtoilet/karipom_prev.log", COMPRESS_MAX_LINES);
        addLog("COMPRESS PREVIOUS LOG: " + result);
        server.sendHeader("Location", "/logtoilet");
        server.send(302, "text/plain", "");
      });

      server.on("/compress_event_log", []() {
        String result = compressLogFile("/logtoilet/toilet_log.txt", COMPRESS_MAX_LINES);
        addLog("COMPRESS EVENT LOG: " + result);
        server.sendHeader("Location", "/logtoilet");
        server.send(302, "text/plain", "");
      });

      udp.begin(UDP_PORT);

      addLog("UDP listening on port " + String(UDP_PORT));

      // ===== KARIPOM EAR v2 (Phase 0 / Step 3) =====
#ifdef EAR_UART_ENABLED
      // RXバッファ2048バイト＝FFT行の実通信量（約0.7〜0.9KB/s）で約2〜3秒分。
      // UART理論最大転送量（115200bps≒11.5KB/s）換算では約0.18秒分。
      // moveSmooth等の短いブロッキング区間（数百ms〜1秒級）を吸収する目的。
      Serial2.setRxBufferSize(2048);  // ※必ずbegin()より前に呼ぶこと
      Serial2.begin(115200, SERIAL_8N1, 18, 17);  // RX=G18, TX=G17（Port C）
      addLog("EAR UART INIT: RX=G18 TX=G17 115200bps buf=2048 (Step5.1 full LINEIN)");
#endif
      // ===== KARIPOM EAR v2 ここまで =====

      server.begin();
      addLog("KariPom Web Server START");

      // ==============================

      if (WiFi.status() == WL_CONNECTED) {
        // 2026/07/17: WAIT NTP TIMEのログだけ時刻が+9h（翌日）にずれる不具合の対策。
        // configTzTime()呼び出し直後にgetLocalTime()すると、SNTP初期化(sntp_init())の
        // 内部処理と重なるタイミングでRTC由来の現在時刻が正しく取得できないことがあった。
        // ここではRTC/NTP同期ロジック自体には手を入れず、「WAIT NTP TIME」のログだけを
        // configTzTime()呼び出しより前に出すことで、直前の"KariPom Web Server START"と
        // 同じ（正しい）時刻状態のまま記録されるようにする。
        addLog("WAIT NTP TIME");

        // configTzTime() はTZ文字列を直接指定してSNTPを起動する。
        // configTime(0,0,...) はTZ環境変数を"UTC0"にリセットするため使用しない。
        configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org");

        // getLocalTime() だけでは RTC由来のシステム時刻があれば即true を返すため、
        // SNTPの実同期完了を sntp_get_sync_status() で確認する。
        // SNTP_SYNC_STATUS_COMPLETED になるまで最大15秒ポーリングする。
        bool ntpSyncDone = false;
        unsigned long ntpWaitStart = millis();
        while (millis() - ntpWaitStart < 15000) {
          if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ntpSyncDone = true;
            break;
          }
          delay(200);
        }

        if (ntpSyncDone) {
          struct tm timeinfo;
          getLocalTime(&timeinfo, 0);
          addLog("NTP TIME OK");
          timeEverSynced = true;
          rtcTimeValid   = true;
          saveTimeToRtc();
          addLog("NTP TIME OK -> RTC UPDATED");
          playWakeTone();
        } else {
          addLog("NTP TIME FAIL");
          playSleepTone();
        }

      } else {
        addLog("AP MODE: NTP SKIPPED");
        playSleepTone();
      }
    } else {
      addLog("WIFI FAILED");

      drawBootFace();

      CoreS3.Display.fillRect(0, 26, 320, 24, WHITE);
      CoreS3.Display.setTextColor(RED);
      CoreS3.Display.setTextSize(2);
      CoreS3.Display.drawString("WIFI FAILED", 5, 26);
      CoreS3.Display.setTextColor(BLACK);
      smartDelay(3000);

      WiFi.disconnect(true);
      smartDelay(500);
    }
  }

  // playWavFromSD("/sounds/karipom_startup.wav");

  addLog("Battery Level = " + String(CoreS3.Power.getBatteryLevel()));

  // ── ジョイスティック：起動時は常に disabled で開始（2026/07/07）──
  // 旧: calibrateJoystick() を呼んで即座に中心値を採用・有効化していた。
  //     起動中の接続タイミング次第で過渡的なADC値を中心として採用し
  //     以降の首振り・wakeUp等に誤入力する問題があった。
  // 新: joystickEnabled は変数宣言時に false で初期化済み。
  //     有効化はすべて handleJoystick() 内のランタイム安定確認
  //     → recalibrateJoystickRuntime() フローで行う。
  //     起動時・動作中どちらの接続でも同一ロジックで安全に ON になる。
  addLog("JOY: disabled at boot, runtime recalib will activate when stable");

  // リセット時安全起動：
  // いきなりセンターへwriteせず、RTCに保存した最後の角度から再開する。
  restoreServoPositionFromRtc();

  addLog("SERVO CONFIG: UD=" + String(ENABLE_UD_SERVO ? "ON" : "OFF")
       + " LR=" + String(ENABLE_LR_SERVO ? "ON" : "OFF"));

  // 軸割り当て診断用：実際に使われているピン番号をそのままログに出す。
  // UD_SERVO_PIN と LR_SERVO_PIN が同値になっていないかをログだけで確認できる。
  addLog("SERVO PIN CONFIG: UD_SERVO_PIN=" + String(UD_SERVO_PIN)
       + " LR_SERVO_PIN=" + String(LR_SERVO_PIN));

  addLog("SERVO ATTACH START");

  if (ENABLE_UD_SERVO) {
    servoUD.attach(UD_SERVO_PIN);
    addLog("UD SERVO ATTACHED");
  } else {
    addLog("UD SERVO SKIPPED (disabled)");
  }

  if (ENABLE_LR_SERVO) {
    servoLR.attach(LR_SERVO_PIN);
    addLog("LR SERVO ATTACHED");
  } else {
    addLog("LR SERVO SKIPPED (disabled)");
  }

  writeServoHoldCurrentPosition();
  // 起動保持writeはlrMarkMoved()を通らない唯一の生writeだったため明示マーク。
  // これで起動約2秒後に通常のAUTO DETACHで解放される
  // （従来は最初のIDLE首振り＝60〜120秒後まで通電保持していた）。
  lrMarkMoved();
  addLog("SERVO HOLD DONE");

  lastIdleMoveTime = millis();
  nextIdleMoveInterval = random(60000, 120000);
  addLog("IDLE TIMER RESET AFTER BOOT");

  CoreS3.Display.setRotation(1);

  CoreS3.Camera.begin();
  CoreS3.Imu.begin();

  // 起動シーケンス完了。以降は電源表示を有効にする。
  // drawFace()の中でshowSensors()→drawBattery()が呼ばれるため、
  // drawFace()より前にフラグを立てる。
  batteryDisplayEnabled = true;

  // 統合描画Canvas（320x240 / 16bit / PSRAM = 153,600 bytes）をここで1回だけ確保する。
  // 以降 createSprite()/deleteSprite() は二度と呼ばない（毎フレームの動的確保は禁止）。
  // drawBootFace()等の起動画面はこの前に描かれるため、従来どおり液晶へ直接描画される。
  sceneCanvasInit();

  drawFace();
  playWavFromSD("/sounds/online_ready.wav");

  randomSeed(millis());
  nextBlinkInterval = random(3000, 10000);

  lastInteractionTime = millis();

  // 起動時ヒープ状況ログ（長時間運用の基準値として記録）
  {
    char hbuf[160];
    snprintf(hbuf, sizeof(hbuf),
             "BOOT HEAP: free=%u minFree=%u largest=%u psramFound=%d psram=%u/%u",
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMinFreeHeap(),
             (unsigned)ESP.getMaxAllocHeap(),
             psramFound() ? 1 : 0,
             (unsigned)ESP.getFreePsram(),      // PSRAM残量（未搭載/無効なら0）
             (unsigned)ESP.getPsramSize());     // PSRAM総量（1MB WAV mallocの行き先判定用）
    addLog(hbuf);
  }

  // NVS保存済みの音声入力ソースをここで適用する。
  // online_ready.wav 等の起動音再生後に行うため、起動確認音は必ず鳴る。
  // 内蔵マイクモードが保存されていた場合、ここでSpeaker.end()→Mic.begin()が走る。
  applyBootAudioSource();

}  // ← setup終了

#ifdef FFT_DISPLAY_TEST
// ############################################################################
// #                                                                          #
// #   KariPom Visualizer Framework  v1.0  (2026-07-21)                       #
// #                                                                          #
// ############################################################################
//
//   Wi-Fi(UDP) FFT ──┐
//   LINE IN   FFT ──┼──> fftLevel[] ──> AudioVizState(gViz) ──> Visualizer
//   Bluetooth FFT ──┘    （共通バッファ）   （共通・正規化済み）      Manager
//   （将来）                                                            │
//                                                                       ▼
//                                            OFF / Graphic EQ / Audio Halo
//
// ・入力元による描画分岐は存在しない。
//   fftLevel[] は handleUDP()（Wi-Fi）と handleEarUart()（LINE IN）の
//   両方から書き込まれる唯一の共通バッファであり、Visualizerはその先の
//   AudioVizState しか見ない。
// ・Visualizerは bands[] と bandCount を受け取る汎用入力で描画する。
//   将来 32/64バンド化しても VIZ_SRC_BAND_COUNT を変えるだけでよい。
// ・動的メモリ確保は一切行わない（全て static / 定数テーブル）。
//
// ============================================================================

// ── 送信側から届くバンド数 ──────────────────────────────
// 現在の通信仕様は8バンド固定（FFT:v0,...,v7）。
// 将来 KariPom Talk / Karipom Ear / 受信形式を32・64バンド化したときは、
//   ・fftLevel[] の要素数
//   ・handleUDP()/handleEarUart() のパース段数
//   ・この定数
// の3つを合わせるだけで、Visualizer側は一切変更不要。
#define VIZ_SRC_BAND_COUNT  8
#define VIZ_MAX_BANDS      64   // Visualizer側が受け入れられる上限（将来の64バンド対応）

// ── 共通オーディオ可視化ステート ─────────────────────────
// 「今、かりポムが感じている音」を入力元非依存で表す唯一の構造体。
struct AudioVizState {
  float    band[VIZ_MAX_BANDS];  // 0.0〜1.0（低音→高音）。受信途絶時は自動減衰
  uint8_t  bandCount;            // 有効バンド数（現在8／将来32・64）
  float    level;                // 全帯域平均（0.0〜1.0）
  float    bass;                 // 低域側1/4の平均（0.0〜1.0）
  bool     rxActive;             // FFTパケットを受信中か
  uint32_t nowMs;
};

static AudioVizState gViz;

// FFT受信値 → 共通ステートへ変換する唯一の場所。
// ここが「Wi-Fi / LINE IN / Bluetooth を吸収する層」になっている。
void vizUpdateState(uint32_t now) {
  uint8_t n = VIZ_SRC_BAND_COUNT;
  if (n > VIZ_MAX_BANDS) n = VIZ_MAX_BANDS;
  gViz.bandCount = n;
  gViz.nowMs     = now;

  bool timedOut  = (now - lastFftPacketTime > FFT_RX_TIMEOUT_MS);
  gViz.rxActive  = !timedOut;

  float sum = 0.0f, bsum = 0.0f;
  uint8_t bn = (uint8_t)((n + 3) / 4);   // 低域側1/4
  if (bn == 0) bn = 1;

  for (uint8_t i = 0; i < n; i++) {
    if (timedOut) {
      // 既存Graphic EQと同一の減衰（0〜100の整数で 3/4 ずつ）→ 約1秒で0へ
      int q = (int)lroundf(gViz.band[i] * 100.0f);
      q = q * 3 / 4;
      gViz.band[i] = (float)q * 0.01f;
    } else {
      gViz.band[i] = (float)fftLevel[i] * 0.01f;
    }
    sum += gViz.band[i];
    if (i < bn) bsum += gViz.band[i];
  }
  gViz.level = sum  / (float)n;
  gViz.bass  = bsum / (float)bn;
}

// ── 共通ヘルパ（全Visualizerが利用可能）──────────────────

// 共通ステート gViz（= bands[] + bandCount という汎用入力）から、
// p（0.0=最低域 〜 1.0=最高域）の位置のバンド値を線形補間で取り出す。
// バンド数が8でも32でも64でも、Visualizer側のコードは変わらない。
// ※あくまで「表示上の補間」であり、真の高分解能FFTではない点に注意。
float vizSampleBand(float p) {
  const AudioVizState& s = gViz;
  if (s.bandCount == 0) return 0.0f;
  if (s.bandCount == 1) return s.band[0];
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  float f  = p * (float)(s.bandCount - 1);
  int   i0 = (int)f;
  if (i0 > (int)s.bandCount - 2) i0 = (int)s.bandCount - 2;
  if (i0 < 0) i0 = 0;
  float t  = f - (float)i0;
  return s.band[i0] * (1.0f - t) + s.band[i0 + 1] * t;
}

// 既存Graphic EQと同一の8色アンカー（低音=青 → 高音=マゼンタ）。
// bandCount=8のとき p=i/7 は必ずアンカーに一致するため、
// 既存グライコの配色は1色も変わらない。
static const uint16_t VIZ_COLOR_ANCHOR[8] = {
  0x001F, 0x07FF, 0x07E0, 0xAFE5, 0xFFE0, 0xFD20, 0xF800, 0xF81F
};

uint16_t vizSpectrumColor(float p) {
  if (p <= 0.0f) return VIZ_COLOR_ANCHOR[0];
  if (p >= 1.0f) return VIZ_COLOR_ANCHOR[7];
  int idx = (int)(p * 7.0f + 0.5f);
  if (idx < 0) idx = 0;
  if (idx > 7) idx = 7;
  return VIZ_COLOR_ANCHOR[idx];
}

// RGB565を白へ寄せる（pct=100で原色、pct=0で白）。
uint16_t viz565Tint(uint16_t c, uint8_t pct) {
  if (pct > 100) pct = 100;
  int r = (c >> 11) & 0x1F;
  int g = (c >>  5) & 0x3F;
  int b =  c        & 0x1F;
  r = 31 - ((31 - r) * (int)pct) / 100;
  g = 63 - ((63 - g) * (int)pct) / 100;
  b = 31 - ((31 - b) * (int)pct) / 100;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// 表示ゲイン（高域ほど持ち上げる）。
// bandCount=8のときは既存Graphic EQのテーブルをそのまま使い、見た目を完全維持する。
static const float VIZ_EQ_GAIN8[8] = { 1.6f, 1.6f, 1.6f, 1.6f, 1.7f, 1.8f, 2.0f, 2.2f };

float vizBandGainP(float p) {          // 任意のスペクトル位置向け（8バンド曲線の一般化）
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  if (p < 0.5f) return 1.6f;
  return 1.6f + (p - 0.5f) * 1.2f;     // p=0.5→1.6 / p=1.0→2.2
}

float vizBandGain(uint8_t i, uint8_t n) {
  if (n == 8) return VIZ_EQ_GAIN8[i];  // 既存グライコと完全一致
  float p = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
  return vizBandGainP(p);
}

// ── 顔パーツの再描画（全Visualizer共通）──────────────────
// drawFace()/drawOpenEyes()/updateNose()は呼ばない
// （全画面・広範囲の白塗りが走るとVisualizerが消えるため）。
// ミスかりポムのまつ毛にも対応（既存drawEyelashes()を目オフセット0で再利用）。
// forceClear=true のときは、口の周辺を先に白で消してから描き直す。
// 口パクの開閉で形状が変わった瞬間に旧形状が残るのを防ぐ
// （Visualizerが顔の背面に回った v2.0 以降で必要になった処理）。
// Lighting合成モード中のみ true。Visualizer/顔パーツを「白で消さない・毎フレーム全描画」
// のオーバーレイモードにする。false のとき従来の描画・見た目は 1バイトも変わらない。
bool gLightingActive = false;

// ============================================================================
// vizOutlineRect() — Lighting併用時だけ矩形に「外=白 / 内=黒」の2重輪郭を付ける
//                    （v1.0 / 2026-07-30）
//
// ■ 何のために追加したか
//   Psychedelic / Trance のような高彩度Lightingの上では、Graphic EQ のバーと
//   8-Lane Rhythm のブロックが「描画されているのに見えない」状態になっていた。
//   原因は、両者のバー色（VIZ_COLOR_ANCHOR[8]）8色すべてが Psychedelic の
//   使用色（PSY_PAL + PSY_SPECTRUM の24色）と【完全に一致】しており、かつ
//   単色塗りで輪郭を持たないため、同色の背景に乗った瞬間に形が消えるため。
//   （Kaleidoscopeが埋もれないのは、画面のほぼ100%を自前の図形で塗り替えて
//     いるからで、Visualizer全体の問題ではなかった）
//
// ■ なぜ単色1色の縁取りではダメか
//   Psychedelicの背景色24色には【純黒 0x0000 と純白 0xFFFF の両方】が含まれる
//   （暗色9色／明色3色）。したがって、
//     ・BLACK のみ … 黒背景・暗い背景で輪郭が消える
//     ・WHITE のみ … 白背景・明るい背景で輪郭が消える
//     ・BLACK 2px  … 太くなるだけで、黒背景では依然として消える
//   実際に Psychedelic の代表4背景（赤×黄ウェッジ／マゼンタ×シアンOPグリッド／
//   白×黒OPグリッド／ライム×マゼンタ破片）へ6方式を描いて比較した結果、
//   全背景で形が読めたのは「白と黒を1pxずつ重ねる」2方式だけだった。
//
// ■ 外を白・内を黒にした理由
//   Psychedelicの使用色は暗色9色に対し明色3色で、暗い背景の方が3倍多い。
//   外側を白にすると、多数派である暗い背景に対して白いハローが直接効き、
//   一目で形が分かる。少数派の明るい背景では外側の白が背景へ溶けるが、
//   1px内側の黒リングが必ず境界を作るため、どちらの場合も輪郭が残る。
//   （どちらか一方は必ず背景とコントラストを持つ、という二重の保険）
//
// ■ 適用範囲
//   gLightingActive == false（Lighting OFF＝Visualizer単体表示）のときは
//   即 return するため、従来表示は1画素も変わらない。
//   呼び出しているのは Graphic EQ と 8-Lane Rhythm の2箇所のみで、
//   Halo / Mirror / Kaleidoscope / Analog VU には一切適用していない。
//
// ■ 描画量
//   1矩形あたり drawRect 2回だけ（LovyanGFXのdrawRectは1px枠なので2回重ねる）。
//   塗り面積は周長ぶん＝34x16のブロックなら約200px、160px高のバーでも約780px。
// ============================================================================
static inline void vizOutlineRect(int x, int y, int w, int h) {
  if (!gLightingActive) return;        // Lighting OFF時は何もしない（従来表示と完全に同一）
  if (w < 6 || h < 4) return;          // 極小の図形は輪郭で潰れるので付けない
  GFX.drawRect(x, y, w, h, WHITE);     // 外リング（暗い背景に対して効く）
  if (w < 8 || h < 6) return;
  GFX.drawRect(x + 1, y + 1, w - 2, h - 2, BLACK);   // 内リング（明るい背景に対して効く）
}

void drawVisualizerFaceParts(bool forceClear) {
  static bool prevMouthOpen = false;
  bool mouthOpen = (externalSpeaking && mouthPakuOpen);
  // Lighting中は背景が照明なので白で消さない（タイル塗りが旧口形状を消す）。
  // Canvas合成中も毎フレーム背景から作り直すため白消去は不要（白い四角の発生源を断つ）。
  if (!gLightingActive && !gSceneOnCanvas && (forceClear || mouthOpen != prevMouthOpen)) {
    GFX.fillRect(135, 149, 51, 36, WHITE);
  }
  prevMouthOpen = mouthOpen;

  // 黒目（drawOpenEyes()と同座標・同半径）
  // Lighting中は色付き床の上でも読めるよう、細い明色リムを先に敷く（白い顔ではない）。
  // 2026-07-23: Retro Race（Pole Position風デモ）向けに eyeOffsetX/Y を反映するよう対応。
  //   通常はLighting中に目オフセットを動かす機能が無いため（0,0）のままで従来と完全に同じ見た目。
  // 2026-07-23: Eye Slot（お目々スロット）中は、黒目の位置にリールをすでに
  //   lightRenderEyeSlot() 側で描いているため、ここでの黒目描画だけをスキップする
  //   （鼻・口・まゆ毛など他の顔パーツは今まで通りここで描き、通常の顔を維持する）。
  if (!gEyeSlotActive) {
    if (gLightingActive) {
      GFX.fillCircle(90  + eyeOffsetX, 90 + eyeOffsetY, 22, WHITE);
      GFX.fillCircle(230 + eyeOffsetX, 90 + eyeOffsetY, 22, WHITE);
    }
    GFX.fillCircle(90  + eyeOffsetX, 90 + eyeOffsetY, 20, BLACK);
    GFX.fillCircle(230 + eyeOffsetX, 90 + eyeOffsetY, 20, BLACK);
  }

  // ミスかりポムのまつ毛（既存関数を再利用。オフセットは一時的に0へ退避）
  if (cfg_characterStyle == CHARACTER_MISS_KARIPOM) {
    int savedEX = eyeOffsetX, savedEY = eyeOffsetY;
    eyeOffsetX = 0; eyeOffsetY = 0;
    drawEyelashes();
    eyeOffsetX = savedEX; eyeOffsetY = savedEY;
  }

  // 鼻・鼻口の縦線・口（黒/赤本体は不変。白backingを先に全部描いてから黒/赤本体を
  // まとめて後から描く方式に統一 — Sleep Lighting Carouselのdraw NoseAndMouthOutlined()
  // と同じ考え方。白同士・黒(赤)同士がそれぞれ先に一体化してから重なるため、
  // 鼻→縦線→口が継ぎ目なく連続して見える（旧実装は鼻の黒本体を先に確定させてから
  // 縦線backingを後追いで足していたため、backingが鼻の黒本体の一部を上書きして
  // 分断して見える不具合があった。今回は白backingを全パーツぶん先に描き切ってから
  // 黒/赤本体を描くことでこれを解消）。鼻のハローだけは実機確認の結果、太すぎたため
  // 従来どおり固定20×14に戻した（縦線backingの上端はnoseY+3付近まで届くため、
  // 20×14＝下端noseY+14でも縦線と重なり、連続性は維持される）。線・しゃべり口の
  // backingの太さ・拡張量はSleep Carouselと同じSLEEP_OUTLINE_PXを流用。黒/赤本体の
  // drawThickLine/fillEllipse呼び出し（座標・太さ・形状・色）は元のコードと完全に
  // 同一で一切変更していない。
  const int o = SLEEP_OUTLINE_PX;
  if (gLightingActive) {
    GFX.fillEllipse(noseX, noseY, 20, 14, WHITE);
    if (mouthOpen) {
      drawThickLine(noseX, noseY + 8, noseX, noseY + 25, 6 + o, WHITE);
      GFX.fillEllipse(noseX, noseY + 26, 19, 13, WHITE);  // 実機確認2026-08-05: 赤い口backingのみ縮小
    } else {
      drawThickLine(noseX, noseY +  8, noseX,      noseY + 22, 6 + o, WHITE);
      drawThickLine(noseX, noseY + 22, noseX - 20, noseY + 32, 6 + o, WHITE);
      drawThickLine(noseX, noseY + 22, noseX + 20, noseY + 32, 6 + o, WHITE);
    }
  }

  GFX.fillEllipse(noseX, noseY, 18, 12, BLACK);

  if (mouthOpen) {
    drawThickLine(noseX, noseY + 8, noseX, noseY + 25, 6, BLACK);
    GFX.fillEllipse(noseX, noseY + 26, 18, 12, RED);
  } else {
    drawThickLine(noseX, noseY +  8, noseX,      noseY + 22, 6, BLACK);
    drawThickLine(noseX, noseY + 22, noseX - 20, noseY + 32, 6, BLACK);
    drawThickLine(noseX, noseY + 22, noseX + 20, noseY + 32, 6, BLACK);
  }
}

// ============================================================================
// Visualizer #1 : Graphic EQ（Classic）
//
// 既存のグライコフェイスをそのまま Framework へ載せ替えたもの。
// レイアウト・色・ゲイン・減衰・80ms更新・差分描画・描画順は従来と同一。
// 変更点は「8固定 → bandCount汎用」化のみ（bandCount=8では完全に同じ絵）。
//
// 描画順（従来どおり／バーが最前面＝音楽が顔を侵食する演出）：
//   1. 下降したバーの減分を白で消去
//   2. 顔パーツを描画（消去で欠けた目・鼻・口を修復）
//   3. 立っている全バー本体を帯域色で最後に重ね塗り
// ============================================================================
void vizRenderGraphicEq(bool needsInit) {
  const AudioVizState& s = gViz;
  static int prevH[VIZ_MAX_BANDS];

  uint8_t n = s.bandCount;
  if (n == 0) return;

  const int COL_W        = 320 / n;   // bandCount=8 → 40（従来と同一）
  const int BAR_MX       = (COL_W >= 12) ? 3 : 1;  // 列内左右マージン（8バンド時=3）
  const int BAR_MAX_H    = 160;       // 最大バー高さ
  const int BAR_BOTTOM_Y = 200;       // 全バー共通の下端Y

  if (needsInit) {
    if (!gLightingActive && !gSceneOnCanvas) GFX.fillScreen(WHITE);
    for (int i = 0; i < VIZ_MAX_BANDS; i++) prevH[i] = 0;
    if (!gLightingActive) drawVisualizerFaceParts(true);
  }

  bool anyChange = false;

  // ── 手順1：差分検出と下降分の消去（無変化の列には一切描画しない）──
  for (uint8_t i = 0; i < n; i++) {
    int lvl  = (int)lroundf(s.band[i] * 100.0f);      // 従来の barLevel[] と同値（0〜100）
    int disp = (int)((float)lvl * vizBandGain(i, n));
    if (disp > 100) disp = 100;
    int h = disp * BAR_MAX_H / 100;
    if (h > BAR_MAX_H) h = BAR_MAX_H;

    if (!gLightingActive && !gSceneOnCanvas && h == prevH[i]) continue;   // overlay/Canvas時は毎フレーム全バー描画

    if (!gLightingActive && !gSceneOnCanvas && h < prevH[i]) {   // overlay/Canvas時は白消去しない
      GFX.fillRect(i * COL_W + BAR_MX, BAR_BOTTOM_Y - prevH[i],
                              COL_W - 2 * BAR_MX, prevH[i] - h, WHITE);
    }
    prevH[i] = h;
    anyChange = true;
  }

  if (!anyChange) return;   // 1画素も触れない

  // ── 手順2：顔パーツを描画（消去で欠けた部分の修復）──
  // Lighting中は顔をコンポジタが最前面に描くので、ここでは描かない（バーは顔の下）。
  if (!gLightingActive) drawVisualizerFaceParts(false);

  // ── 手順3：立っている全バー本体を最後に重ね塗り ＝ バーが最前面 ──
  for (uint8_t i = 0; i < n; i++) {
    if (prevH[i] > 0) {
      uint16_t c = vizSpectrumColor((n > 1) ? (float)i / (float)(n - 1) : 0.0f);
      GFX.fillRect(i * COL_W + BAR_MX, BAR_BOTTOM_Y - prevH[i],
                              COL_W - 2 * BAR_MX, prevH[i], c);
      // Lighting併用時だけ2重輪郭（外=白／内=黒）を付ける。バーの色・位置・幅・高さ・
      // 音反応ロジックは一切変更していない（gLightingActive==falseなら即returnするため
      // Visualizer単体表示の見た目は従来と1画素も変わらない）。
      vizOutlineRect(i * COL_W + BAR_MX, BAR_BOTTOM_Y - prevH[i],
                     COL_W - 2 * BAR_MX, prevH[i]);
    }
  }
}

// ============================================================================
// Visualizer #2 : Audio Halo — Circular Wave Crown (v3.0)
//
// ■ コンセプト
//   顔を囲む楕円の外周を発射点とし、そこから画面外周へ向かって
//   48方向のスペクトラムが伸びる。各方向の先端は隣同士で滑らかに結ばれ、
//   顔の周囲を「巨大な波形の王冠（クラウン）」が取り囲む。
//   内側は淡いオーラ、外側の波形エッジは原色で光る4段グラデーション。
//
//   ＝ v1のレイアウト（顔を囲む楕円）＋ v2の迫力（画面外周まで到達）
//
// ■ 設計思想：これは測定器ではなく「かりポムの演出」
//   FFT値の忠実な再現より「見ていて楽しいこと」を優先する。
//   そのため入力を大胆に持ち上げる3段構えを採用した。
//     ① オートゲイン(AGC)  … 直近ピークに合わせて最大3.1倍まで自動増幅
//     ② 非線形カーブ t^0.48 … 小音量ほど大きく持ち上げる（33点LUT）
//     ③ 高速応答            … Attack 0.90 / Decay 0.40（既存Graphic EQ並みの機敏さ）
//   小〜中音量（＝日常の使用音量）で常に王冠が大きく展開し、
//   かつ帯域ごとの形の違いが波としてはっきり動く状態を狙っている。
//
//   ※ v2までは Attack 0.65 / Decay 0.20 と減衰が遅く、これが
//     「Graphic EQに比べて動きが鈍い」印象の主因だった。
//     Graphic EQは平滑化を一切行わず fftLevel をそのまま表示している。
//
// ■ レイアウト
//   発射楕円 (AIN=84,  BIN=40)  中心(160,143) → x  76…244 / y 103…183
//   到達楕円 (AOUT=157, BOUT=93) 中心(160,143) → x   3…317 / y  50…236
//   ・顔の内側へは一切伸ばさない（発射点は常に発射楕円の上）
//   ・上方向の到達点は y=50（showSensors()が300ms毎に塗る y<48 へ侵入しない）
//   ・左右は画面端 x=3/317、下は y=236 まで到達
//   ・ベース楕円線は描かない（v1で「常時表示のリングにしか見えない」原因だったため）
//   ・到達側も「楕円」にしているため、最大入力でも画面矩形に張り付かず
//     波形（王冠）の形が保たれる
//
// ■ 帯の太さ
//   王冠は48セクターの帯として描かれ、1セクターの円周方向の幅は
//   発射楕円上で約8px、到達楕円上で約16px。
//   口の線（6px）と同等以上の存在感があり、細い針状にはならない。
//   半径方向の伸びは 53〜73px（方向による）で、これが音量で変化する。
//
// ■ 描画順（v2から継続：顔パーツが最前面）
//   手順1  変化したセクターの「差分帯」だけを白で消去
//   手順2  変化したセクターを4段グラデーションで再描画
//   手順3  目 → まつ毛 → 鼻 → 口 を最後に描画 ＝ 表情と口パクが常に読める
//
// ■ 描画負荷対策
//   ・sin/cos・発射/到達半径・色は初回1回だけ事前計算（毎フレーム0回）
//   ・音量カーブは33点LUTの線形補間（powf をループ内で呼ばない）
//   ・2px未満の変化のセクターは描き直さない
//   ・全セクター無変化なら1画素も触れない
//   ・動的メモリ確保なし（全て static）
// ============================================================================
#define HALO_SPOKES 48

static const float HALO_CX   = 160.0f;   // 王冠の中心（顔の中心）
static const float HALO_CY   = 143.0f;
static const float HALO_A_IN  =  84.0f;  // 発射楕円（顔を囲む）
static const float HALO_B_IN  =  40.0f;
static const float HALO_A_OUT = 157.0f;  // 到達楕円（画面外周）
static const float HALO_B_OUT =  93.0f;  // 中心143 - 93 = y50 ＝ センサー帯の直下

// ── 演出パラメータ（見え方の調整はここだけで完結する）──
static const float HALO_CURVE_EXP  = 0.48f;  // 音量カーブ指数（小さいほど小音量が伸びる）
static const float HALO_AGC_TARGET = 0.64f;  // AGCの目標レベル
static const float HALO_AGC_FLOOR  = 0.21f;  // AGCの分母下限（最大増幅 = TARGET/FLOOR ≒ 3.0倍）
static const float HALO_AGC_FALL   = 0.030f; // AGCピークの下降速度（小さいほどゆっくり）
static const float HALO_GATE_LO    = 0.020f; // 無音ゲート：これ以下は完全に0
static const float HALO_GATE_HI    = 0.065f; // 無音ゲート：これ以上でフル表示
static const float HALO_ATTACK     = 0.90f;  // 立ち上がりの追従（1.0で即座）
static const float HALO_DECAY      = 0.40f;  // 立ち下がりの追従（大きいほど機敏）

static bool     haloTableReady = false;
static float    haloDirX[HALO_SPOKES], haloDirY[HALO_SPOKES];   // 単位方向ベクトル
static float    haloRIn[HALO_SPOKES],  haloROut[HALO_SPOKES];   // 発射半径 / 到達半径
static float    haloPos[HALO_SPOKES];                           // スペクトル位置 0..1
static uint16_t haloCol[HALO_SPOKES][4];                        // 内→外の4段グラデ色
static float    haloCur[HALO_SPOKES];                           // 平滑化レベル 0..1
static float    haloDisp[HALO_SPOKES];                          // 波形化後の表示レベル
static float    haloL[HALO_SPOKES];                             // 今回の外周半径
static float    haloPrevLr[HALO_SPOKES];                        // 前回の外周半径
static float    haloCurveLut[33];                               // 音量カーブLUT（sqrt領域）
static float    haloAgcPeak = 0.0f;                             // AGC用の追従ピーク

// 4段グラデーションの境界（発射楕円=0.0 → 波形エッジ=1.0）
static const float HALO_SEG[5] = { 0.00f, 0.40f, 0.68f, 0.86f, 1.00f };
// 各段の濃さ（viz565Tint のパーセント。内側ほど淡く、外側の波形エッジが原色）
static const uint8_t HALO_SEG_TINT[4] = { 18, 42, 72, 100 };

// 音量カーブ：t^HALO_CURVE_EXP 相当。
// 添字を sqrt(t) にすることで小音量側ほどLUTが細かくなり、補間誤差が実質ゼロになる。
float vizHaloCurve(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  float f = sqrtf(t) * 32.0f;
  int   i = (int)f;
  if (i > 31) i = 31;
  float u = f - (float)i;
  return haloCurveLut[i] * (1.0f - u) + haloCurveLut[i + 1] * u;
}

// 48方向の幾何・色と音量カーブを一度だけ構築する
// （sinf/cosf/powf はここでしか呼ばない）
void vizHaloBuildTable() {
  for (int k = 0; k <= 32; k++) {
    // 添字が sqrt(t) なので t^EXP は (k/32)^(2*EXP) になる
    haloCurveLut[k] = powf((float)k / 32.0f, 2.0f * HALO_CURVE_EXP);
  }

  for (int j = 0; j < HALO_SPOKES; j++) {
    // 真下を0°として時計回り（y下向き正のスクリーン座標）
    float arad = (90.0f + (float)j * (360.0f / (float)HALO_SPOKES)) * (float)PI / 180.0f;
    float dx = cosf(arad);
    float dy = sinf(arad);
    haloDirX[j] = dx;
    haloDirY[j] = dy;

    // 方向ベクトルと楕円の交点までの距離（発射側・到達側）
    float ei = (dx / HALO_A_IN)  * (dx / HALO_A_IN)  + (dy / HALO_B_IN)  * (dy / HALO_B_IN);
    float eo = (dx / HALO_A_OUT) * (dx / HALO_A_OUT) + (dy / HALO_B_OUT) * (dy / HALO_B_OUT);
    haloRIn[j]  = 1.0f / sqrtf(ei);
    haloROut[j] = 1.0f / sqrtf(eo);
    if (haloROut[j] < haloRIn[j] + 4.0f) haloROut[j] = haloRIn[j] + 4.0f;

    // スペクトル位置：真下=0(最低域) → 真上=1(最高域)、左右対称に折り返す
    float rel  = (float)j * (360.0f / (float)HALO_SPOKES);
    float fold = (rel <= 180.0f) ? rel : (360.0f - rel);
    haloPos[j] = fold / 180.0f;

    uint16_t base = vizSpectrumColor(haloPos[j]);
    for (int q = 0; q < 4; q++) haloCol[j][q] = viz565Tint(base, HALO_SEG_TINT[q]);

    haloCur[j]     = 0.0f;
    haloDisp[j]    = 0.0f;
    haloL[j]       = haloRIn[j];
    haloPrevLr[j]  = haloRIn[j];
  }
  haloAgcPeak    = 0.0f;
  haloTableReady = true;
}

// 座標を画面内へ丸める（float誤差に対する保険。通常は到達楕円の時点で内側に収まる）
static inline int vizHaloClampX(float v) {
  int i = (int)v;
  if (i < 0)   i = 0;
  if (i > 319) i = 319;
  return i;
}
static inline int vizHaloClampY(float v) {
  int i = (int)v;
  if (i < 0)   i = 0;
  if (i > 239) i = 239;
  return i;
}

// セクター k（方向 k と k+1 の間）の、半径 a1..b1 / a2..b2 の帯を塗る
void vizHaloBand(int k, float a1, float b1, float a2, float b2, uint16_t color) {
  if (b1 <= a1 && b2 <= a2) return;
  int k2 = (k + 1) % HALO_SPOKES;
  float d1x = haloDirX[k],  d1y = haloDirY[k];
  float d2x = haloDirX[k2], d2y = haloDirY[k2];

  int p1x = vizHaloClampX(HALO_CX + d1x * a1), p1y = vizHaloClampY(HALO_CY + d1y * a1);
  int p2x = vizHaloClampX(HALO_CX + d2x * a2), p2y = vizHaloClampY(HALO_CY + d2y * a2);
  int p3x = vizHaloClampX(HALO_CX + d2x * b2), p3y = vizHaloClampY(HALO_CY + d2y * b2);
  int p4x = vizHaloClampX(HALO_CX + d1x * b1), p4y = vizHaloClampY(HALO_CY + d1y * b1);

  GFX.fillTriangle(p1x, p1y, p2x, p2y, p3x, p3y, color);
  GFX.fillTriangle(p1x, p1y, p3x, p3y, p4x, p4y, color);
}

void vizRenderAudioHalo(bool needsInit) {
  if (!haloTableReady) vizHaloBuildTable();

  if (needsInit) {
    if (!gLightingActive && !gSceneOnCanvas) GFX.fillScreen(WHITE);
    for (int j = 0; j < HALO_SPOKES; j++) {
      haloCur[j]    = 0.0f;
      haloDisp[j]   = 0.0f;
      haloL[j]      = haloRIn[j];
      haloPrevLr[j] = haloRIn[j];
    }
    haloAgcPeak = 0.0f;
    if (!gLightingActive) drawVisualizerFaceParts(true);
  }

  // ── ① オートゲイン（AGC）＋ 無音ゲート ──────────────────
  // 測定器ではないので、普段の小〜中音量でも王冠が大きく開くよう
  // 直近のピークに合わせて入力全体を持ち上げる。
  float peak = 0.0f;
  for (uint8_t i = 0; i < gViz.bandCount; i++) {
    if (gViz.band[i] > peak) peak = gViz.band[i];
  }
  if (peak > haloAgcPeak) haloAgcPeak = peak;                              // 立ち上がりは即座
  else haloAgcPeak += (peak - haloAgcPeak) * HALO_AGC_FALL;                // 下降はゆっくり

  float ref = haloAgcPeak;
  if (ref < HALO_AGC_FLOOR) ref = HALO_AGC_FLOOR;                          // 増幅上限を決める
  float agc = HALO_AGC_TARGET / ref;

  // 無音ゲート：本当に音が無いときにノイズを持ち上げないための緩やかな遮断
  float gate = (peak - HALO_GATE_LO) / (HALO_GATE_HI - HALO_GATE_LO);
  if (gate < 0.0f) gate = 0.0f;
  if (gate > 1.0f) gate = 1.0f;

  // ── ② 各方向のレベル更新（高速応答 ＋ 非線形カーブ）────────
  for (int j = 0; j < HALO_SPOKES; j++) {
    float t = vizSampleBand(haloPos[j]) * vizBandGainP(haloPos[j]) * agc;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;

    if (t > haloCur[j]) haloCur[j] += (t - haloCur[j]) * HALO_ATTACK;
    else                haloCur[j] += (t - haloCur[j]) * HALO_DECAY;
    if (haloCur[j] < 0.004f) haloCur[j] = 0.0f;

    haloDisp[j] = vizHaloCurve(haloCur[j]) * gate;
  }

  // ── ③ 隣同士を平滑化して「滑らかな波形」にする（王冠の外周線）──
  static float smoothed[HALO_SPOKES];
  for (int j = 0; j < HALO_SPOKES; j++) {
    int a = (j + HALO_SPOKES - 1) % HALO_SPOKES;
    int b = (j + 1) % HALO_SPOKES;
    smoothed[j] = (haloDisp[a] + haloDisp[j] * 2.0f + haloDisp[b]) * 0.25f;
  }

  bool anyChange = needsInit;
  for (int j = 0; j < HALO_SPOKES; j++) {
    float L = haloRIn[j] + smoothed[j] * (haloROut[j] - haloRIn[j]);
    // 2px未満の揺れでは描き直さない（ノイズ由来の再描画と負荷の抑制）
    if (fabsf(L - haloPrevLr[j]) < 1.0f) L = haloPrevLr[j];
    haloL[j] = L;
    if (L != haloPrevLr[j]) anyChange = true;
  }

  if (gLightingActive || gSceneOnCanvas) anyChange = true;   // overlay/Canvas時は毎フレーム全描画
  if (!anyChange) return;   // 無音・無変化フレームでは1画素も触れない

  // ── 手順1：変化したセクターの差分帯だけを白で消去（Lighting中・Canvas合成中はスキップ）──
  if (!gLightingActive && !gSceneOnCanvas)
  // 内側境界 = 新旧の小さい方 / 外側境界 = 新旧の大きい方（+1pxのはみ出し対策）。
  // 伸びた場合も縮んだ場合も、この1枚で過不足なく消える。
  for (int k = 0; k < HALO_SPOKES; k++) {
    int k2 = (k + 1) % HALO_SPOKES;
    if (haloL[k] == haloPrevLr[k] && haloL[k2] == haloPrevLr[k2]) continue;

    float a1 = (haloL[k]  < haloPrevLr[k])  ? haloL[k]  : haloPrevLr[k];
    float a2 = (haloL[k2] < haloPrevLr[k2]) ? haloL[k2] : haloPrevLr[k2];
    float b1 = (haloL[k]  > haloPrevLr[k])  ? haloL[k]  : haloPrevLr[k];
    float b2 = (haloL[k2] > haloPrevLr[k2]) ? haloL[k2] : haloPrevLr[k2];
    vizHaloBand(k, a1 - 1.0f, b1 + 1.0f, a2 - 1.0f, b2 + 1.0f, WHITE);
  }

  // ── 手順2：変化したセクターを4段グラデーションで再描画 ──
  for (int k = 0; k < HALO_SPOKES; k++) {
    int k2 = (k + 1) % HALO_SPOKES;
    bool changed = (haloL[k] != haloPrevLr[k]) || (haloL[k2] != haloPrevLr[k2]);
    if (!gLightingActive && !gSceneOnCanvas && !changed && !needsInit) continue;

    float s1 = haloL[k]  - haloRIn[k];
    float s2 = haloL[k2] - haloRIn[k2];
    if (s1 <= 1.0f && s2 <= 1.0f) continue;   // ほぼ発射楕円まで収束＝何も描かない

    for (int q = 0; q < 4; q++) {
      vizHaloBand(k,
                  haloRIn[k]  + s1 * HALO_SEG[q],     haloRIn[k]  + s1 * HALO_SEG[q + 1],
                  haloRIn[k2] + s2 * HALO_SEG[q],     haloRIn[k2] + s2 * HALO_SEG[q + 1],
                  haloCol[k][q]);
    }
  }

  for (int j = 0; j < HALO_SPOKES; j++) haloPrevLr[j] = haloL[j];

  // ── 手順3：顔パーツを最後に描画（Lighting中はコンポジタが最前面に描く）──
  if (!gLightingActive) drawVisualizerFaceParts(false);
}

// ============================================================================
// Visualizer #3 : Mirror Wave（v1.7）
//
// ■ コンセプト
//   見た瞬間に「音が動いている」と分かるVisualizer。
//   8バンドFFTを横方向へ補間し、上下対称のネオンリボン状波形として、
//   画面いっぱいに大きく描く（線ではなく太いリボン＋ネオングロー）。
//   Audio Halo（放射）との差別化＝“横に流れる対称波形”で一目瞭然。
//
// ■ 仕様
//   ・上下対称（中心 MW_CY を軸に、同じ高さで上下へ展開）
//   ・AGCで通常音量でも大きく動く
//   ・帯域位置＋時間で色相がシアン→青→紫→マゼンタ→黄→緑へ流れる（音で色変化）
//   ・縁を明るく（白へ寄せる）＝ネオングロー、内側は原色寄り
//   ・上端48pxの情報パネルへは侵入しない（MW_YMIN=50）
//   ・Lightingの上に重ね描き（gLightingActive=true）／単体時は白背景に描画
//   ・顔は最後（コンポジタ or 自前）で最前面に描画
// ============================================================================
#define MW_CY     143
#define MW_YMIN   50
#define MW_YMAX   236
#define MW_SX     4                     // 列ピッチ(px)
#define MW_NCOL   (320 / MW_SX + 1)     // 81列
#define MW_EDGE   6                     // リボン縁（ネオン）の太さ
#define MW_NPAL   6

static const uint8_t MW_PAL_RGB[MW_NPAL][3] = {
  {0,230,255},{0,140,255},{150,90,255},{255,60,220},{255,200,0},{60,255,140}
};
static bool     mwReady = false;
static uint16_t mwPal[MW_NPAL];
static int      mwPrevH[MW_NCOL];
static float    mwAgcPeak = 0.0f;
static float    mwColorPhase = 0.0f;
static float    mwCurveLut[33];

// 音量カーブ t^0.45（小音量を持ち上げる）。33点LUT・sqrt領域。
float mwCurve(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  float f = sqrtf(t) * 32.0f;
  int i = (int)f; if (i > 31) i = 31;
  float u = f - (float)i;
  return mwCurveLut[i] * (1.0f - u) + mwCurveLut[i + 1] * u;
}

void buildMwTable() {
  for (int i = 0; i < MW_NPAL; i++) {
    uint8_t r = MW_PAL_RGB[i][0], g = MW_PAL_RGB[i][1], b = MW_PAL_RGB[i][2];
    mwPal[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  for (int k = 0; k <= 32; k++) mwCurveLut[k] = powf((float)k / 32.0f, 0.90f); // (sqrt)^0.9 ≒ t^0.45
  for (int c = 0; c < MW_NCOL; c++) mwPrevH[c] = 0;
  mwReady = true;
}

void vizRenderMirrorWave(bool needsInit) {
  if (!mwReady) buildMwTable();
  bool overlay = gLightingActive;
  // Canvas合成中は毎フレーム背景から作り直すため、白消去・微振動抑制・差分スキップを止める。
  // 顔のレイヤー順は overlay 側で従来どおり保つ（単体時は本関数が顔を描く）。
  bool noErase = overlay || gSceneOnCanvas;

  if (needsInit) {
    if (!noErase) GFX.fillScreen(WHITE);
    for (int c = 0; c < MW_NCOL; c++) mwPrevH[c] = 0;
    mwAgcPeak = 0.0f;
    if (!overlay) drawVisualizerFaceParts(true);
  }

  // ── AGC（通常音量でも大きく動く）──
  float peak = 0.0f;
  for (uint8_t i = 0; i < gViz.bandCount; i++) if (gViz.band[i] > peak) peak = gViz.band[i];
  if (peak > mwAgcPeak) mwAgcPeak = peak;
  else mwAgcPeak += (peak - mwAgcPeak) * 0.03f;
  float ref = (mwAgcPeak < 0.20f) ? 0.20f : mwAgcPeak;
  float agc = 0.80f / ref;

  mwColorPhase += 0.02f + gViz.level * 0.30f;   // 音量で色が速く流れる

  const int maxHalf = (MW_YMAX - MW_YMIN) / 2;  // 93（MW_CY=143 が上下の中心）

  // ── 各列の半高を計算 ──
  static int newH[MW_NCOL];
  bool anyChange = noErase || needsInit;
  for (int c = 0; c < MW_NCOL; c++) {
    int x = c * MW_SX; if (x > 319) x = 319;
    float p = (float)x / 319.0f;
    float v = vizSampleBand(p) * agc; if (v > 1.0f) v = 1.0f; if (v < 0.0f) v = 0.0f;
    int h = (int)(mwCurve(v) * (float)maxHalf);
    if (h < 2) h = 2;
    if (!noErase && abs(h - mwPrevH[c]) < 2) h = mwPrevH[c];  // 微振動抑制（単体時）
    newH[c] = h;
    if (h != mwPrevH[c]) anyChange = true;
  }
  if (!anyChange) return;

  // ── 単体（白背景）時：縮んだ列の外側を白で消去（Canvas合成中は不要）──
  if (!noErase) {
    for (int c = 0; c < MW_NCOL; c++) {
      int x = c * MW_SX; int w = MW_SX; if (x + w > 320) w = 320 - x; if (w <= 0) continue;
      int oh = mwPrevH[c], nh = newH[c];
      if (nh < oh) {
        GFX.fillRect(x, MW_CY - oh, w, oh - nh, WHITE);   // 上側の余白
        GFX.fillRect(x, MW_CY + nh, w, oh - nh, WHITE);   // 下側の余白
      }
    }
  }

  // ── リボン本体（上下対称・ネオングロー）──
  for (int c = 0; c < MW_NCOL; c++) {
    int x = c * MW_SX; int w = MW_SX; if (x + w > 320) w = 320 - x; if (w <= 0) continue;
    int h = newH[c];
    int top = MW_CY - h, bot = MW_CY + h;
    if (top < MW_YMIN) top = MW_YMIN;
    if (bot > MW_YMAX) bot = MW_YMAX;

    // 色相：列位置 + 時間（音量）で流れる
    int hi = (int)((float)c * 0.10f + mwColorPhase);
    hi %= MW_NPAL; if (hi < 0) hi += MW_NPAL;
    uint16_t hue  = mwPal[hi];
    uint16_t body = viz565Tint(hue, 18);              // 内側＝ほぼ原色（わずかに白）
    uint16_t edge = viz565Tint(hue, 62);              // 縁＝白へ寄せたネオングロー

    int ctop = top + MW_EDGE, cbot = bot - MW_EDGE;
    if (cbot > ctop) GFX.fillRect(x, ctop, w, cbot - ctop, body);   // 本体
    GFX.fillRect(x, top, w, MW_EDGE, edge);                          // 上縁ネオン
    GFX.fillRect(x, bot - MW_EDGE, w, MW_EDGE, edge);               // 下縁ネオン

    mwPrevH[c] = h;
  }

  // ── 顔（最前面）。単体時はここで描く。overlay時はコンポジタが描く。──
  if (!overlay) drawVisualizerFaceParts(false);
}

// ============================================================================
// Visualizer #4 : 8-Lane Rhythm — Graphic EQの「時間履歴」版
//
// ■ 位置づけ（beatmaniaではなくGraphic EQが設計上の基準）
//   これは独立したbeatmania風ゲーム画面ではない。
//   Graphic EQ（Classic）が「横方向の周波数分布＝今の8バンド」を表示するのに対し、
//   本Visualizerは、Graphic EQが今まさに表示している「1瞬間の8バンド強度」を
//   1行分のスライスとして毎フレーム記録し、それを上から下へ流し続ける。
//     横軸 ＝ 周波数8バンド（Graphic EQと同じ列配置・同じ色）
//     縦軸 ＝ 時間（新しい瞬間ほど上、過去ほど下）
//   ＝ Graphic EQの上位互換（時間履歴つきEQ）という位置づけ。
//
// ■ Graphic EQからそのまま流用するもの（新規実装しない）
//   ・入力              … gViz.band[0..7]
//   ・Bandごとのゲイン  … vizBandGain(i, n)（VIZ_EQ_GAIN8[]と同一の式）
//   ・Bandの色          … vizSpectrumColor(p)（8色アンカーと同一）
//   ・通常背景          … 白背景。Graphic EQと同じ条件
//     （if (!gLightingActive && !gSceneOnCanvas) fillScreen(WHITE)）でのみ明示的に塗る。
//     Lighting併用時はGraphic EQ同様に明示的な塗りつぶしをせず、Lighting側の
//     背景をそのまま透過して見せる（強度0のセルには何も描かないため）。
//   ・顔                … drawVisualizerFaceParts()。Graphic EQと同じ
//     if (!gLightingActive) 条件・同じ「バーが最前面＝顔を侵食する」描画順。
//
// ■ ピーク検出は行わない
//   「強い音が来た瞬間だけ打つ」という考え方は採用しない。
//   単純に「今のGraphic EQ表示状態（ゲイン適用後の0〜100）」を毎フレーム
//   1行分の履歴として記録し、既存の履歴を1行分下へ送るだけ。
//   曲の強弱がそのまま縞模様として流れて見える。
//
// ■ セルの表現（Graphic EQのバー寸法をそのまま流用。独自の8等分は行わない）
//   横方向のX座標・幅は、Graphic EQが実際に使っている
//     COL_W = 320 / n, BAR_MX = (COL_W>=12)?3:1, barW = COL_W - 2*BAR_MX
//   という式をそのまま用いる（n=8のときCOL_W=40, BAR_MX=3, barW=34 で
//   vizRenderGraphicEq()と完全に同じバー幅・同じX座標になる）。強度によって
//   この幅を縮めることはしない（＝Graphic EQのバーと同じ太さで揃う）。
//   強度(0〜100)は明るさ（弱い→白っぽく薄い／強い→原色で明るい）にのみ反映する。
//   強度0のセルは何も描かない（白背景／Lighting背景のまま）。
//
// ■ 縦方向：連続線ではなく「塊＋隙間」の周期表示
//   同じBandの音が何フレームも続くと、履歴行がそのまま連続し縦線に見えてしまう。
//   そこで、履歴の行インデックス r に対して「RHY_BLOCK_ROWS行だけ描画→
//   RHY_GAP_ROWS行は必ず空白」という固定周期を描画側にだけ課す
//   （データ側は変更せず、rhyHist[]は毎フレーム連続して記録し続ける。
//   隙間はあくまで表示上の間引きであり、ピーク検出や区間判定の再導入ではない）。
//   これにより、信号が持続していても画面上は短いブロックが隙間を挟んで
//   連なって見える。
//
// ■ 描画領域：Graphic EQ等と同じくSCENE_TOP(48)〜SCENE_H-1のみ
//   センサー・バッテリー・IP表示（y=0〜47）は一切隠さず、従来どおり常時最前面に
//   表示され続ける。8-Lane RhythmはGraphic EQ／Halo／Mirrorと全く同じ扱いで、
//   Canvas上の描画自体をy=SCENE_TOP(48)〜SCENE_H-1(239)の範囲だけに収め、
//   scenePush()もSCENE_TOP以降のみを転送する（他Visualizerと同一の既定動作。
//   Rhythm専用の全画面転送・専用のshowSensors()再呼び出しは持たない）。
//   ブロックの一部もy<48へは描画しない（RHY_TOP=SCENE_TOPを起点にY座標を
//   計算しているため、そもそもy<48の座標が生成されない）。
//
// ■ RAM・描画負荷
//   履歴は静的配列 rhyHist[RHY_HIST_ROWS][RHY_LANES]（48×8=384byte）のみで
//   動的確保なし。毎フレーム全行を描き直すが、隙間行と強度0のセルは描画
//   自体をスキップするため、既存のMirror Wave（81列×最大3回≒243回）と
//   同程度以下のオーダーに収まる。
// ============================================================================
#define RHY_LANES        8
#define RHY_COL_W        (320 / RHY_LANES)                 // Graphic EQのCOL_Wと同一の式（=40）
#define RHY_BAR_MX       ((RHY_COL_W >= 12) ? 3 : 1)        // Graphic EQのBAR_MXと同一の式（=3）
#define RHY_BAR_W        (RHY_COL_W - 2 * RHY_BAR_MX)       // Graphic EQの実バー幅と同一（=34）

#define RHY_TOP          SCENE_TOP               // 描画開始Y。Graphic EQ等と同じくSCENE_TOP(48)より上へは描かない
#define RHY_AREA_H       (SCENE_H - RHY_TOP)      // 有効高さ（=192px）
#define RHY_ROW_H        4                        // 履歴1行の高さ(px)。落下速度・履歴進行の刻み幅＝変更しない
#define RHY_BLOCK_ROWS   3                        // ブロックとして連続描画する行数（=12px）
#define RHY_GAP_ROWS     1                        // ブロック間の隙間行数（=4px）
#define RHY_CYCLE_ROWS   (RHY_BLOCK_ROWS + RHY_GAP_ROWS)   // 4行周期
#define RHY_HIST_ROWS    (RHY_AREA_H / RHY_ROW_H)  // 48行 ≒ 48スライス分の履歴（192px÷4px。約1.9秒分）

// 2026/07/27 追加：1ブロックを描画する際の縦方向の高さだけを拡大する倍率。
// RHY_ROW_H（履歴1行=落下1コマぶんのY座標の刻み幅。48行の履歴本数・落下速度・
// 更新周期はすべてこれで決まっており、今回は一切変更しない）とは完全に別の値として
// 分離してある。ここを変えても、行の生成タイミング・移動量・履歴の本数には
// 影響しない（GFX.fillRect()の高さ引数だけに使う）。
#define RHY_BLOCK_HEIGHT_SCALE 4
#define RHY_DRAW_H       (RHY_ROW_H * RHY_BLOCK_HEIGHT_SCALE)   // 実際に描画するブロックの高さ(px)＝4×4=16px

// 2026/07/27 追加：ブロック生成判定（立ち上がり検出）用パラメータ。
// 「音量が大きい状態が続いている」ではなく「直前フレームから急上昇した瞬間」だけを
// 新規ブロックとして生成するための閾値。表示仕様（色・サイズ・落下・消滅等）には
// 一切関係せず、rhyHist[0][i]へ何を書き込むか（生成するorしない）だけに使う。
#define RHY_MIN_LEVEL         12    // 最低音量条件（0..100）。これ未満はノイズとして無視
#define RHY_RISE_THRESHOLD    10    // 直前フレームからの上昇量（0..100）。これ以上で「アタック」とみなす
#define RHY_LANE_COOLDOWN_MS  120   // レーン別クールダウン(ms)。同一レーンの連続生成を抑制

// [0]=最新（y=RHY_TOP＝画面上から3行目相当）〜 [RHY_HIST_ROWS-1]=最古（画面最下段）
static uint8_t rhyHist[RHY_HIST_ROWS][RHY_LANES];
static bool    rhyReady = false;

// 立ち上がり検出用の直前フレーム値と、レーン別クールダウンの終了時刻。
static int           rhyPrevLevel[RHY_LANES];
static unsigned long rhyLaneCooldownUntil[RHY_LANES];

void vizRenderRhythm(bool needsInit) {
  (void)needsInit;   // 意図的に不使用（下のコメント参照。VizRenderFn型のため引数自体は残す）
  const AudioVizState& s = gViz;
  uint8_t n = s.bandCount;
  if (n == 0) return;
  uint8_t lanes = (n < RHY_LANES) ? n : RHY_LANES;

  // 履歴クリアは「本当の初回」（rhyReady==false）だけに限定する。
  // needsInitは元々Graphic EQ等の「バー高さ配列をリセットして全面を描き直すきっかけ」
  // として汎用的に使われており、Lighting終了直後やFFTパケットが一瞬(500ms超)
  // 途切れて visualizerFaceActive が false→true に戻った直後にも true になる
  // （updateVisualizerFace()のON遷移／FFT_FACE_ACTIVE_MS判定。ここは共有ロジックのため
  // 変更しない）。EQ/Halo/Mirrorは1フレーム分の状態しか持たないため無害だが、
  // Rhythmは48行ぶんの時間履歴(rhyHist)を保持しているため、needsInitのたびに
  // クリアすると「モードを変えていないのに履歴が一斉に消えて0から溜まり直す」
  // 現象になる。そのためクリア条件からneedsInitを外し、このモード内だけで
  // 完結するrhyReadyのみで判定する（初回だけクリア、以降は電源が入っている限り
  // 継続する）。
  if (!rhyReady) {
    if (!gLightingActive && !gSceneOnCanvas) GFX.fillScreen(WHITE);   // Graphic EQと同一条件
    for (int r = 0; r < RHY_HIST_ROWS; r++)
      for (int i = 0; i < RHY_LANES; i++) rhyHist[r][i] = 0;
    for (int i = 0; i < RHY_LANES; i++) {
      rhyPrevLevel[i]         = 0;
      rhyLaneCooldownUntil[i] = 0;
    }
    if (!gLightingActive) drawVisualizerFaceParts(true);
    rhyReady = true;
  }

  // ── 1. 履歴を1行分下へ送る ──
  for (int r = RHY_HIST_ROWS - 1; r > 0; r--) {
    for (uint8_t i = 0; i < RHY_LANES; i++) rhyHist[r][i] = rhyHist[r - 1][i];
  }
  // 立ち上がり検出：current（既存のGraphic EQと同一のバンド値・同一のvizBandGain()
  // 適用後の値）と直前フレームの値を比較し、「MIN_LEVEL以上」かつ「直前から
  // RISE_THRESHOLD以上の急上昇」があった瞬間だけ、そのレーンへ新規ブロックを1個
  // 生成する（rhyHist[0][i]へdisp値をそのまま書き込む＝表示強度は従来通り）。
  // 高音量が持続しているだけでは rise が小さいため生成されない。
  // レーン別クールダウン(RHY_LANE_COOLDOWN_MS)中は同一レーンの再生成を抑制する。
  unsigned long nowMs = millis();
  for (uint8_t i = 0; i < lanes; i++) {
    int lvl  = (int)lroundf(s.band[i] * 100.0f);          // Graphic EQのbarLevel[]と同一
    int disp = (int)((float)lvl * vizBandGain(i, n));     // Graphic EQと同一のゲイン適用
    if (disp > 100) disp = 100;
    if (disp < 0)   disp = 0;

    int  rise   = disp - rhyPrevLevel[i];
    bool cooled = (nowMs >= rhyLaneCooldownUntil[i]);
    if (disp >= RHY_MIN_LEVEL && rise >= RHY_RISE_THRESHOLD && cooled) {
      rhyHist[0][i] = (uint8_t)disp;
      rhyLaneCooldownUntil[i] = nowMs + RHY_LANE_COOLDOWN_MS;
    } else {
      rhyHist[0][i] = 0;
    }
    rhyPrevLevel[i] = disp;
  }
  for (uint8_t i = lanes; i < RHY_LANES; i++) rhyHist[0][i] = 0;   // 将来の非8バンド化への保険

  // ── 2. 顔（Graphic EQと同じ条件・同じ最前面配置）──
  if (!gLightingActive) drawVisualizerFaceParts(false);

  // ── 3. 履歴グリッド本体（Graphic EQと同じ幅・X座標、隙間行はスキップ）──
  //    Graphic EQのバーと同様、これが最前面＝顔を侵食する描画順になる。
  //    yはRHY_TOP(=SCENE_TOP=48)を起点にするため、ブロックの一部も
  //    y<48（センサー/バッテリー/IP表示の領域）へは描画されない。
  //    2026/07/27改訂：行の生成タイミング・y座標(落下位置)の刻み幅(RHY_ROW_H)・
  //    履歴本数(RHY_HIST_ROWS)・隙間判定(RHY_CYCLE_ROWS/RHY_BLOCK_ROWS)は一切
  //    変更せず、実際に塗る高さだけをRHY_ROW_H→RHY_DRAW_H(4倍)にした。
  //    各行の上端yはこれまでと同じ位置のまま、その行の担当領域より下へ
  //    はみ出して太く描かれるだけなので、1回のアタック生成・1回の消滅・
  //    移動速度・履歴の進み方には影響しない。
  for (int r = 0; r < RHY_HIST_ROWS; r++) {
    if ((r % RHY_CYCLE_ROWS) >= RHY_BLOCK_ROWS) continue;   // 隙間行：何も描かず塊を分断する
    int y = RHY_TOP + r * RHY_ROW_H;   // 行の位置(落下座標)はこれまでと完全に同じ
    for (uint8_t i = 0; i < RHY_LANES; i++) {
      int disp = rhyHist[r][i];
      if (disp <= 0) continue;

      int tintPct = 100 - disp;        // 強いほど原色(低tint=明るい)、弱いほど白寄り(高tint=薄い)
      if (tintPct > 88) tintPct = 88;  // 最弱でもうっすら色が残るようにする
      if (tintPct < 0)  tintPct = 0;

      uint16_t hue = vizSpectrumColor((RHY_LANES > 1) ? (float)i / (float)(RHY_LANES - 1) : 0.0f);
      uint16_t col = viz565Tint(hue, (uint8_t)tintPct);

      int x = i * RHY_COL_W + RHY_BAR_MX;   // Graphic EQと完全に同じX座標（幅も変更なし）
      GFX.fillRect(x, y, RHY_BAR_W, RHY_DRAW_H, col);   // 高さだけRHY_ROW_H→RHY_DRAW_H(4倍)
      // Lighting併用時だけ2重輪郭（外=白／内=黒）を付ける。
      // ブロックの生成条件・疎な密度・落下速度・色（viz565Tintの白寄せ）は一切
      // 変更していない＝「疎であること」というこのVisualizerのデザインはそのまま。
      // 副次的な利点として、RHY_DRAW_H(16px)がRHY_ROW_H(4px)より大きいため
      // 縦に重なって描かれる近接ブロック同士も、この輪郭で1個ずつ分離して見える。
      vizOutlineRect(x, y, RHY_BAR_W, RHY_DRAW_H);
    }
  }
}

// ============================================================================
// Visualizer #6 : Kaleidoscope（万華鏡）── v4（A/Bターゲット・モーフィング方式）
//
// v3（境界鏡映ウェッジ＋各レイヤー1ファセット頂点をsin()でゆっくり往復させる
// 方式）は、黒い球の問題は解消したが実機では「一度できた花模様が回転している
// だけ」に見えた。原因は、
//   (1) ファセット頂点の可動域が角度±7〜10°・半径±22〜30%と狭く、
//       sin()による単純往復のため常に同じ振幅の呼吸に収束し、模様の骨格
//       （3レイヤー×4ファセットの構成比）自体は事実上不変だったこと。
//   (2) kalRot（全体回転）が0.03rad/frame(≒0.43rad/s、1周約14.6秒)と
//       速く、模様のわずかな呼吸よりも回転運動の方が知覚的に支配的だった
//       こと。
// の2点で、"形が変わる"ことより"同じ形が回る"ことの方が目立っていた。
//
// v4では、以下の方針で作り直す。
//   ・各リングの「厚み・可動頂点の角度・可動頂点の半径・色相」を、
//     現在値A→次の目標値Bへ3〜6秒かけて滑らかに補間するA/Bモーフ方式に
//     変更する。目標に到達したら A=B、新しいBを再抽選、を無限に繰り返す
//     （sin()による往復ではなく、常に次の新しい形へ向かい続ける）。
//   ・可動域を大幅に拡大する（リング厚み8〜22px、頂点角度4°〜26°、
//     頂点半径比15%〜85%、色相は全域）ことで、モーフ後は実際に
//     「別の万華鏡模様」に見える程度の変化量を確保する。
//   ・kalRotは1周約31秒まで大幅に減速し、回転を明確に脇役にする。
//   ・境界（θ=0°/30°）に頂点を固定する既存の鏡映接続の仕組み（direct/
//     mirror展開→最後にkalRotを一括適用）はv3のまま維持する。
//     kalDrawWedgeTri()は無変更。
//   ・リングを3→4に増やし、中心リング（半径0起点）も他と同じA/Bモーフの
//     対象にすることで、中心部だけが固定花芯に見える状態を避ける。
//   ・各リングのモーフ周期（3〜6秒）は互いに独立に再抽選するため、
//     全リングが同時に切り替わらず、組み替わり方が有機的になる。
//
// ■ 境界接続（維持）
//   θ=0°/30°の頂点は常に「そのリングの内周・外周半径×既知のcos/sin定数」
//   のみで決まり、リング内部の可動頂点（1個）だけがA/B間を補間される。
//   可動頂点は角度4°〜26°・半径比15%〜85%の範囲に必ずクランプされ、
//   境界（0°/30°の線）へは触れないため、direct/mirror展開後も隣接コピー・
//   隣接リングの境界は常に一致する。リング半径は「内周からの厚みの累積
//   和」として構成するため、リング同士も半径方向に必ず連続する（隙間なし）。
//
// ■ 自己交差・破綻の防止
//   ・リング厚みは常に正の値（8〜22px）に制限しているため、内周半径<外周
//     半径が常に保証され、リングの上下関係が反転することはない。
//   ・可動頂点の角度・半径比は境界から十分な余白（角度4°・半径比15%）を
//     残してクランプしているため、退化三角形（面積ほぼ0）や自己交差は
//     発生しない。
//   ・最外周の半径合計は最大88px（8〜22px×4リング）に収まるよう範囲を
//     選んでおり、bass発光時の+5%スケールを掛けても中心(160,144)から
//     上部48px保護ラインまでの距離96pxを超えない。
//
// ■ 音への反応（模様の変形量・明るさ・モーフ速度のみ。発射演出はしない）
//   帯域→対応リングの可動頂点の変形量へわずかに加算、level平滑値→明るさと
//   回転速度へわずかに上乗せ、bass立ち上がり→全リング半径を一瞬+5%だけ
//   均等に膨らませて発光させ、同時にモーフ速度を短時間だけ上げる
//   （「一瞬膨らむ」だけで、中心から何かを発射する演出は行わない）。
//
// ■ 顔・上部48pxとの関係（維持・無変更）
//   単体表示時（!gLightingActive）のみ自前で drawVisualizerFaceParts() を
//   呼ぶ。黒背景での視認性確保のため、既存関数の直前に同じ固定座標へ白い
//   下地（目の縁22px円・鼻の縁20×14楕円・既存関数と同一範囲の口消去矩形
//   135,149,51,36）だけを自前で敷く。これらはウェッジの鏡映複製処理には
//   一切参加しない。drawVisualizerFaceParts()自体は無変更。
// ============================================================================
#define KAL_TOP            48
#define KAL_CX            160
#define KAL_CY            144
#define KAL_SECTORS         6
#define KAL_RING_COUNT      4      // 内周から外周まで4リング（中心リング含む）
#define KAL_FACETS          4      // 1リングを4枚の三角形（ファセット）に分割

// 60°刻み・6方向の回転定数（cos/sin(0/60/120/180/240/300°)）。既知の値の
// 定数表として持つだけなので、複製処理そのものには実行時の三角関数を使わない。
static const float KAL_SEC_COS[KAL_SECTORS] = { 1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f };
static const float KAL_SEC_SIN[KAL_SECTORS] = { 0.0f, 0.8660254f, 0.8660254f, 0.0f, -0.8660254f, -0.8660254f };

// ウェッジの角度境界（0°と30°）はθ=0°/30°で固定の既知値なので、
// cos/sinも実行時計算不要の定数として持つ。
static const float KAL_A0_COS = 1.0f,       KAL_A0_SIN = 0.0f;        // 0°
static const float KAL_A30_COS = 0.8660254f, KAL_A30_SIN = 0.5f;      // 30°

// リングiは gViz.band[2i]（半径方向の変形担当）と band[2i+1]（角度方向の変形担当）
// の2バンドを個別に使う（平均しない）。8バンド全てが独立した意味を持つように、
// リング0=band0(半径)/band1(角度)、リング1=band2/band3、リング2=band4/band5、
// リング3=band6/band7、という固定対応にする（低域=内周…高域=外周の空間対応は維持）。

// ファセットごとの色相オフセット（同じリング内で少しずつ色味を変え、
// 宝石の断面・ステンドグラスのような見た目にする）。音が強いリングほど
// このオフセットの効きを強め、ファセット間の色差を広げる（■12対応）。
static const float KAL_FACET_HUE_OFS[KAL_FACETS] = { 0.0f, 0.05f, 0.10f, 0.15f };

// A/Bモーフの可動範囲（境界接続・自己交差防止のための制約。上のコメント参照）。
// 2026-07-29 仕上げ修正：巨大化のため、リング厚みをリングごとに非対称化した。
// 内周（リング0）は密度を保つためやや細く、外周（リング3）へ行くほど大きく
// 広げる。4リング合計（最外周半径）はKAL_GAP_MIN[]の和(190px)〜KAL_GAP_MAX[]の
// 和(240px)の範囲になる。画面中心(160,144)からクリップ後に見える4隅
// （y=48〜239の範囲）までの最大距離は約187pxなので、通常時から常にこれを
// 上回り、左右端・下端まで模様が届く（＝黒帯がほぼ残らない）。
static const float KAL_GAP_MIN[KAL_RING_COUNT] = { 30.0f, 40.0f, 50.0f, 70.0f };  // リング厚み最小(px)：内周→外周
static const float KAL_GAP_MAX[KAL_RING_COUNT] = { 40.0f, 50.0f, 60.0f, 90.0f };  // リング厚み最大(px)：内周→外周
static const float KAL_ANG_MIN   = 0.0698f,  KAL_ANG_MAX   = 0.4538f;   // 可動頂点の角度(rad)=4°〜26°
static const float KAL_FRAC_MIN  = 0.15f,    KAL_FRAC_MAX  = 0.85f;     // 可動頂点の半径比（base shape用）
static const float KAL_MORPH_MIN_SEC = 3.0f, KAL_MORPH_MAX_SEC = 6.0f;  // モーフ所要時間(秒)＝「遅い基礎アニメーション」層

// ── 音反応（base shapeとは独立な「速いVisualizer反応」層）──────────────
// base shape（A/Bモーフ）と audio deformation は完全に分離する。A/Bターゲット
// 自体(gapA/B・angA/B・fracA/B・hueA/B)は音声値で一切書き換えない。
// audio deformationは「frac(0..1)の奪い合い」ではなく、独立したpx/rad単位の
// オフセットとしてbase shapeの位置へ加算し、最後にリング内へクランプする
// （安全のためのクランプであり、通常入力で常時貼り付く上限ではない）。
//
// 2026-07-29 仕上げ修正：旧方式は「band envelopeの絶対値 × kalLevelFast（絶対
// 音量）」だけで変形量を決めていた。小音量の曲ではbandEnvelope自体の値も
// 小さく・kalLevelFastも小さく、同じ絶対音量に由来する2つの値を掛け合わせる
// ため二重に減衰し、「曲を変えても違いがほぼ見えない」原因になっていた。
// 今回は音の情報を完全に2種類へ分離する：
//   A. 絶対音量（kalLevelFast由来のkalAmp） … 派手さ・発光量の補助＋無音ゲート
//   B. 8バンド間の相対バランス（bandGained[]・bandMean・比率） … 形そのもの
//      （±方向の変形）を決める主役。曲の違いはBだけで作るため、Aを上げなくても
//      Bは常にはっきり見える（下のkalGate/kalLoudTrim・kalBandRelShape()参照）。
static const float KAL_AUD_RADIUS_PX  = 45.0f;   // 相対band差(±)による可動頂点の最大押し出し量(px)
static const float KAL_AUD_ANGLE_RAD  = 0.55f;   // 相対band差(±)による可動頂点の最大角度オフセット(rad)
static const float KAL_BASS_KICK_PX   = 22.0f;   // bass立ち上がり時、全リング共通で一瞬加算される半径オフセット(px)
// 2026-07-29 二次修正：「(bandGained[i]-bandMean)」という絶対値の差だけでは、
// bandGained[i]もbandMeanも全体音量に比例して一緒に縮むため、音量を下げると
// 差そのものも同じ比率で小さくなってしまい、「小音量でも曲の相対構成が残る」
// という目的を満たせていなかった。今回はbandMeanに対する「比率」
// （(bandGained[i]-bandMean)/bandMean）として正規化する。分子・分母が同じ
// 音量係数で割れるため、理論上は音量に依存しない（＝同じ曲なら音量を変えても
// 比率はほぼ一定に保たれる）。ただしbandMeanが無音付近で非常に小さくなると
// 比率が暴れるため、KAL_REL_EPSILONで分母に下限を設け、さらに最終的な
// kalGate（無音では0）とKAL_REL_RATIO_CLAMPの二重の安全策で無音時の
// ノイズ暴走を防ぐ（詳細はkalBandRelShape()参照）。
static const float KAL_REL_EPSILON     = 0.05f;  // 比率の分母(bandMean)の下限。無音付近での暴走を防ぐ固定フロア
static const float KAL_REL_RATIO_CLAMP = 2.5f;   // 比率そのものの安全クランプ（1帯域だけ突出した偏ったスペクトル対策）
static const float KAL_REL_GAIN        = 1.3f;   // 比率 → 形状変化量への変換ゲイン（sqrt圧縮前）
// Kaleidoscope専用の8バンド包絡線（attackはかなり高速、releaseはそれより遅い
// 非対称平滑化）。FFT本体・vizUpdateState()には一切触れず、gViz.band[]を読む
// だけ。2026-07-29：attackをさらに高速化（0.65→0.78）し、低音の「ドン」・
// 高域の「シャッ」・ボーカルの立ち上がりが数フレーム以内に形へ現れるようにした。
static const float KAL_ENV_ATTACK  = 0.78f;   // 立ち上がりはかなり高速に追従
static const float KAL_ENV_RELEASE = 0.16f;   // 下がるときは少し滑らかに戻す（FFTノイズの震え防止）
// 全体の「今どれだけ大きい音か」を表す高速包絡線（gViz.levelから作る、絶対
// 音量用）。2026-07-29：役割を「変形量の主要因」から「派手さ・発光量の補助＋
// 無音ゲート」へ変更した（形そのものはBが担う）。
static const float KAL_LEVEL_FAST_ATTACK  = 0.50f;
static const float KAL_LEVEL_FAST_RELEASE = 0.18f;
// 無音ゲート：kalLevelFastがこの値未満なら本当の無音とみなし音反応をゼロにする。
// ここからKAL_GATE_RANGE分だけ上がるとゲートは即座に全開(1.0)になる（＝小さいが
// 有効な音楽入力なら、音量を上げなくても曲の特徴がすぐはっきり見える＝■2・■7）。
static const float KAL_NOISE_FLOOR = 0.015f;
static const float KAL_GATE_RANGE  = 0.05f;

struct KalRing {
  float gapA,  gapB;     // リング厚み(px)のA/Bターゲット
  float angA,  angB;     // 可動頂点の角度(rad)のA/Bターゲット
  float fracA, fracB;    // 可動頂点の半径比のA/Bターゲット
  float hueA,  hueB;     // 色相位置(0..1)のA/Bターゲット
  float morphT;          // 0..1。A→Bの補間進捗
  float morphPeriod;     // このモーフ1回にかける秒数（3〜6秒でリングごとに独立抽選）
};
static KalRing kalRing[KAL_RING_COUNT];
static float   kalRot           = 0.0f;   // 全体回転角（ラジアン）。優先順位は音反応＞自動モーフ＞回転
static float   kalLevelFast     = 0.0f;   // gViz.levelの高速包絡線（音反応の絶対音量ゲイン・明るさに使用）
static float   kalBassAvg       = 0.0f;   // 低音平均（ビート検出の基準線）
static float   kalBassPulse     = 0.0f;   // 0..1。bass立ち上がりで1.0→数フレームで急減衰（呼吸・発光・脈動専用）
static unsigned long kalBeatCooldownUntil = 0;
static float   kalBandEnv[VIZ_SRC_BAND_COUNT] = {0,0,0,0,0,0,0,0};   // Kaleidoscope専用の8バンド包絡線（attack速い/release遅い）

// 0..1の一様乱数と、それを使った範囲抽選。既存のArduino random()を利用するのみ
// （新しい乱数源やライブラリは追加しない）。
static float kalRand01() { return (float)random(0, 10001) / 10000.0f; }
static float kalRandRange(float lo, float hi) { return lo + (hi - lo) * kalRand01(); }

// 指定リングの現在のBをAへ確定し、新しいBと新しいモーフ周期を抽選してモーフを
// 再スタートする。gap/ang/fracは常にKAL_*_MIN..MAXの範囲でしか抽選しないため、
// 境界接続・単調な半径積み上げ・退化三角形の防止という制約は新目標でも必ず
// 守られる（詳細はファイル先頭のコメント参照）。
static void kalPickNewTarget(uint8_t i) {
  KalRing& R = kalRing[i];
  R.gapA  = R.gapB;  R.gapB  = kalRandRange(KAL_GAP_MIN[i],  KAL_GAP_MAX[i]);
  R.angA  = R.angB;  R.angB  = kalRandRange(KAL_ANG_MIN,  KAL_ANG_MAX);
  R.fracA = R.fracB; R.fracB = kalRandRange(KAL_FRAC_MIN, KAL_FRAC_MAX);
  R.hueA  = R.hueB;  R.hueB  = kalRand01();
  R.morphT = 0.0f;
  R.morphPeriod = kalRandRange(KAL_MORPH_MIN_SEC, KAL_MORPH_MAX_SEC);
}

static void kalInitRings() {
  for (uint8_t i = 0; i < KAL_RING_COUNT; i++) {
    KalRing& R = kalRing[i];
    R.gapA = R.gapB = (KAL_GAP_MIN[i] + KAL_GAP_MAX[i]) * 0.5f;   // このリングのmin..max中間値
    R.angA = R.angB = 0.2617994f;                    // 15°（境界の中間）
    R.fracA = R.fracB = 0.5f;
    R.hueA = R.hueB = (float)i / (float)KAL_RING_COUNT;
    R.morphT = 0.0f;
    R.morphPeriod = kalRandRange(KAL_MORPH_MIN_SEC, KAL_MORPH_MAX_SEC);
    kalPickNewTarget(i);   // 初期状態(A=B=既定値)から最初のランダム目標(B)へ即モーフ開始
  }
  kalRot = 0.0f;
  kalLevelFast = 0.0f;
  kalBassAvg = 0.0f;
  kalBassPulse = 0.0f;
  kalBeatCooldownUntil = 0;
  for (uint8_t b = 0; b < VIZ_SRC_BAND_COUNT; b++) kalBandEnv[b] = 0.0f;
}

// VIZ_COLOR_ANCHOR[8]（既存・青→シアン→緑→…→マゼンタ）を循環パレットとして
// 補間する。既存light565Lerp()を再利用するのみで新しい配色テーブルは作らない。
static uint16_t kalHueColor(float phase) {
  if (phase < 0.0f) phase = 0.0f;
  if (phase >= 1.0f) phase -= (float)(int)phase;
  float t = phase * 8.0f;
  int i0 = (int)t;
  if (i0 > 7) i0 = 7;
  float frac = t - (float)i0;
  uint16_t c0 = VIZ_COLOR_ANCHOR[i0];
  uint16_t c1 = VIZ_COLOR_ANCHOR[(i0 + 1) % 8];
  return light565Lerp(c0, c1, (int)(frac * 255.0f));
}

// ウェッジ内の三角形1枚（座標はすでにkalRotによる全体回転を適用済みの
// 「ワールド座標」）を、6方向の回転×鏡映（Y反転してから同じ回転）で
// 12コピー複製して塗る。境界（θ=0°/30°）に置いた頂点は、隣接コピーの
// 対応する境界頂点と画面上で正確に一致するため、辺がつながって見える。
// 重要：kalRot（全体回転）は「6方向の鏡映展開（direct/mirror）を適用した後」に
// 適用する。direct変換（純粋な回転）は回転同士の合成なので前後どちらでも
// 結果は同じだが、mirror変換（反射）は「反射→回転」と「回転→反射」で結果が
// 逆回転になってしまう（反射は後続の回転の向きを反転させるため）。もし先に
// kalRotをウェッジ頂点へ適用してからdirect/mirrorへ渡すと、direct側コピーは
// +kalRot、mirror側コピーは実質-kalRot相当で回ることになり、kalRot≠0の瞬間に
// 隣接コピーの境界（θ=0°/30°）がずれて鏡映接続が崩れる。
// そのため、ここではまずθ=0°/30°の固定軸に対するdirect/mirror変換（種図形の
// 元の座標系のまま）を先に行い、その結果（12コピー全体）へ最後に共通の
// kalRotを一括で掛ける。全コピーへ同一の剛体回転を後から掛けるだけなので、
// direct/mirror変換で確立された境界の一致（連結）はkalRotの値によらず常に
// 保たれたまま、完成した模様全体だけが一体としてゆっくり回転する。
// 表示範囲拡大（■11〜■14）に伴い、円全体が上部48px・画面左右・下端を越えて
// 大きくはみ出すようになったため、従来の「頂点のどれかがKAL_TOPより上なら
// 三角形ごと描画をやめる」判定は廃止した。三角形の一部だけがy<48へかかる
// ケースが常態化し、丸ごと間引くと逆に模様の途切れ（黒い切れ込み）を作って
// しまうため。代わりに呼び出し側(vizRenderKaleidoscope)で描画範囲全体に対して
// 既存の GFX.setClipRect(0, KAL_TOP, 320, 240-KAL_TOP) を1回だけ設定し、
// scenePush()と同じ仕組み（LovyanGFXのクリップ矩形）でハードウェア側に
// y<48・画面外の部分だけを正しく切り捨てさせる。これにより上部2行の情報表示は
// 一切汚されず、かつ画面内に見えている部分は三角形の一部だけでも正しく描かれる。
static void kalDrawWedgeTri(float x0, float y0, float x1, float y1, float x2, float y2,
                             float cosR, float sinR, uint16_t color) {
  for (uint8_t k = 0; k < KAL_SECTORS; k++) {
    float ck = KAL_SEC_COS[k], sk = KAL_SEC_SIN[k];
    {
      float tx0 = x0*ck - y0*sk, ty0 = x0*sk + y0*ck;
      float tx1 = x1*ck - y1*sk, ty1 = x1*sk + y1*ck;
      float tx2 = x2*ck - y2*sk, ty2 = x2*sk + y2*ck;
      float fx0 = tx0*cosR - ty0*sinR, fy0 = tx0*sinR + ty0*cosR;   // 展開「後」に全体回転
      float fx1 = tx1*cosR - ty1*sinR, fy1 = tx1*sinR + ty1*cosR;
      float fx2 = tx2*cosR - ty2*sinR, fy2 = tx2*sinR + ty2*cosR;
      int dx0 = KAL_CX+(int)lroundf(fx0), dy0 = KAL_CY+(int)lroundf(fy0);
      int dx1 = KAL_CX+(int)lroundf(fx1), dy1 = KAL_CY+(int)lroundf(fy1);
      int dx2 = KAL_CX+(int)lroundf(fx2), dy2 = KAL_CY+(int)lroundf(fy2);
      GFX.fillTriangle(dx0,dy0,dx1,dy1,dx2,dy2,color);   // クリップ矩形適用中なので画面外座標でも安全
    }
    {
      float mx0 = x0*ck + y0*sk, my0 = x0*sk - y0*ck;
      float mx1 = x1*ck + y1*sk, my1 = x1*sk - y1*ck;
      float mx2 = x2*ck + y2*sk, my2 = x2*sk - y2*ck;
      float fx0 = mx0*cosR - my0*sinR, fy0 = mx0*sinR + my0*cosR;   // 展開「後」に全体回転
      float fx1 = mx1*cosR - my1*sinR, fy1 = mx1*sinR + my1*cosR;
      float fx2 = mx2*cosR - my2*sinR, fy2 = mx2*sinR + my2*cosR;
      int dx0 = KAL_CX+(int)lroundf(fx0), dy0 = KAL_CY+(int)lroundf(fy0);
      int dx1 = KAL_CX+(int)lroundf(fx1), dy1 = KAL_CY+(int)lroundf(fy1);
      int dx2 = KAL_CX+(int)lroundf(fx2), dy2 = KAL_CY+(int)lroundf(fy2);
      GFX.fillTriangle(dx0,dy0,dx1,dy1,dx2,dy2,color);   // クリップ矩形適用中なので画面外座標でも安全
    }
  }
}

// 指定bandの相対形状値（符号付き、bandMeanに対する「比率」ベース）を返す。
// 絶対値の差ではなく比率で正規化するため、同じ曲を音量を変えて再生しても
// この値はほぼ一定に保たれる（＝どのリングが強く動くかという曲の特徴が
// 音量に依存しない）。bandMeanが無音付近で小さい場合はKAL_REL_EPSILONが
// 分母の下限として効き、比率自体もKAL_REL_RATIO_CLAMPで安全に制限した上で
// sqrt圧縮・最終±1.3クランプを適用する（呼び出し側でさらにkalGateを掛けて
// 無音では最終出力自体を0にする＝二重の安全策）。
static float kalBandRelShape(float bandGainedVal, float bandMean) {
  float denom = (bandMean > KAL_REL_EPSILON) ? bandMean : KAL_REL_EPSILON;
  float ratio = (bandGainedVal - bandMean) / denom;
  if (ratio >  KAL_REL_RATIO_CLAMP) ratio =  KAL_REL_RATIO_CLAMP;
  if (ratio < -KAL_REL_RATIO_CLAMP) ratio = -KAL_REL_RATIO_CLAMP;
  float mag = sqrtf(fabsf(ratio) * KAL_REL_GAIN);
  float shape = (ratio >= 0.0f) ? mag : -mag;
  if (shape >  1.3f) shape =  1.3f;
  if (shape < -1.3f) shape = -1.3f;
  return shape;
}

void vizRenderKaleidoscope(bool needsInit) {
  if (needsInit) kalInitRings();
  const AudioVizState& s = gViz;
  uint8_t bandN = s.bandCount;
  if (bandN == 0 || bandN > VIZ_SRC_BAND_COUNT) bandN = VIZ_SRC_BAND_COUNT;

  // ── 背景：黒（Lighting併用時は既存Lighting背景をそのまま活かし何も塗らない）──
  if (!gLightingActive) {
    GFX.fillRect(0, KAL_TOP, 320, 240 - KAL_TOP, BLACK);
  }

  // ■14：表示範囲を大きく拡大した巨大な円を描くため、円自体は上部48pxの上・
  // 画面左右・下端を越えて大きくはみ出す。既存scenePush()と同じ考え方
  // （LovyanGFXのクリップ矩形）で、この関数の描画範囲だけをx=0..319/y=48..239
  // に限定する。これによりkalDrawWedgeTri()側の個別クランプ判定なしに、上部2行
  // の情報表示は一切汚れず、画面内に見える部分だけが正しく描かれる。共通の
  // scenePush()やCoreS3.Display.setClipRect()自体は変更していない。
  GFX.setClipRect(0, KAL_TOP, 320, 240 - KAL_TOP);

  // ── 回転：今回も脇役のまま。音量では速度を変えない（■13：回転は音反応の主役にしない）──
  const float KAL_ROT_BASE = 0.014f;   // 無音時の基準回転速度(rad/frame)。70ms周期で約0.2rad/s＝1周約31秒
  kalRot += KAL_ROT_BASE;              // 音量による上乗せはしない（回転は常に一定速度）
  if (kalRot > 6.2831853f) kalRot -= 6.2831853f;
  float cosR = cosf(kalRot), sinR = sinf(kalRot);   // 全リング共通・1フレーム1回だけ

  // ── ■8：全体の「今どれだけ大きい音か」を表す高速包絡線。既存の低速
  //    kalEnergySmooth(0.05)を廃止し、音反応の絶対音量ゲインとして使う ──
  if (s.level > kalLevelFast) kalLevelFast += (s.level - kalLevelFast) * KAL_LEVEL_FAST_ATTACK;
  else                        kalLevelFast += (s.level - kalLevelFast) * KAL_LEVEL_FAST_RELEASE;
  if (kalLevelFast > 1.4f) kalLevelFast = 1.4f;

  // ── 低音の急な立ち上がり検出 → 万華鏡全体が一体として即座に「ドクン」と
  //    脈動・発光する複合反応（膨張＋全リング共通の半径キック＋発光）。
  //    中心から何かを発射する演出には絶対に戻さない（■9）。自動モーフの速度は
  //    音に一切連動させない（■3・■10：2つの時間軸を分離する）──
  kalBassAvg += (s.bass - kalBassAvg) * 0.15f;
  unsigned long nowMs = millis();
  if (s.bass > kalBassAvg * 1.3f + 0.04f && nowMs >= kalBeatCooldownUntil) {
    kalBassPulse = 1.0f;              // 検出した瞬間に最大値へ＝「ドン」に即同期
    kalBeatCooldownUntil = nowMs + 160;   // 2026-07-29: 220→160msにして速い曲の連打も拾う
  }
  kalBassPulse *= 0.65f;   // 2026-07-29: 0.80→0.65にしてリズムがぼやけないよう短時間で減衰
  float pulseScale = 1.0f + kalBassPulse * 0.10f;   // 全体膨張は最大10%

  // ── Kaleidoscope専用の8バンド包絡線を更新。attackはかなり高速・releaseはそれより
  //    遅く追従させ、FFT値の細かい震えを抑えつつ立ち上がりには素早く反応する。
  //    gViz.band[]を読むだけでFFT本体・vizUpdateState()には一切触れない ──
  for (uint8_t b = 0; b < VIZ_SRC_BAND_COUNT && b < bandN; b++) {
    float raw = s.band[b];
    if (raw > kalBandEnv[b]) kalBandEnv[b] += (raw - kalBandEnv[b]) * KAL_ENV_ATTACK;
    else                     kalBandEnv[b] += (raw - kalBandEnv[b]) * KAL_ENV_RELEASE;
  }

  // ── 絶対音量ゲート＋派手さ補助（■2・■3・■7：Aの役割）。形そのものの有無は
  //    このkalGateが決め、kalLoudTrimは「同じ曲がどれだけ派手に見えるか」の
  //    穏やかな微調整に留める。無音では両方ゼロになり音反応が完全に消える ──
  float kalGate = (kalLevelFast - KAL_NOISE_FLOOR) / KAL_GATE_RANGE;
  if (kalGate < 0.0f) kalGate = 0.0f;
  if (kalGate > 1.0f) kalGate = 1.0f;
  float kalLevelClamped = (kalLevelFast > 1.2f) ? 1.2f : kalLevelFast;
  float kalLoudTrim = 0.80f + 0.35f * (kalLevelClamped / 1.2f);   // 0.80(小音量)〜1.15(大音量)
  float kalAmp = kalGate * kalLoudTrim;

  // ── 8バンド間の相対バランス（■3・■4・■5：曲の違いを作る主役）。band0..7の
  //    平均に対して各bandが強いか弱いかを求め、平均より強いband→＋方向、
  //    弱いband→－方向という符号付きの変形量へ変換する。絶対値ではなく相対値を
  //    使うため、全帯域が一様に強い/弱い曲でも8本すべてが同じ方向へ押されない ──
  float bandGained[VIZ_SRC_BAND_COUNT];
  float bandMean = 0.0f;
  uint8_t bandGainedN = (bandN > VIZ_SRC_BAND_COUNT) ? VIZ_SRC_BAND_COUNT : bandN;
  for (uint8_t b = 0; b < bandGainedN; b++) {
    bandGained[b] = kalBandEnv[b] * vizBandGain(b, bandN);   // 既存EQゲイン適用後の値で相対化する
    bandMean += bandGained[b];
  }
  if (bandGainedN > 0) bandMean /= (float)bandGainedN;

  // ── リングごとにA→Bのモーフ（遅い基礎アニメーション）を進めつつ、8バンドに
  //    よる速い音反応レイヤーを独立に加算し、境界固定4頂点＋可動頂点で
  //    4枚の三角形を描く ──
  float rPrev = 0.0f;   // 内周からの厚みの累積和＝リング半径（正のgapのみ加算するため単調増加＝重なり順は常に保たれる）
  for (uint8_t i = 0; i < KAL_RING_COUNT; i++) {
    KalRing& R = kalRing[i];

    // このリングに割り当てた2バンド（band[2i]=半径方向／band[2i+1]=角度方向）を
    // 平均せず別々の変形方向へ使う。8バンド全てが独立した意味を持つ。
    // 2026-07-29 二次修正：「8バンド平均からの相対差」を絶対値の差ではなく
    // bandMeanに対する比率（kalBandRelShape()）で正規化する。＋なら外側/＋角度、
    // －なら内側/－角度へ変形する（曲のスペクトル構成そのものを形へ変換）。
    // 比率ベースなので音量を下げても同じ曲なら同じ傾向の符号・大きさになる。
    uint8_t bandRadIdx = (uint8_t)(i * 2), bandAngIdx = (uint8_t)(i * 2 + 1);
    float shapeRad = 0.0f, shapeAng = 0.0f;   // 符号付き：＋＝平均より強い、－＝平均より弱い
    if (bandRadIdx < bandGainedN) shapeRad = kalBandRelShape(bandGained[bandRadIdx], bandMean);
    if (bandAngIdx < bandGainedN) shapeAng = kalBandRelShape(bandGained[bandAngIdx], bandMean);
    // 絶対音量（kalAmp＝ゲート×派手さ補助）はここでだけ掛ける。形の有無・方向は
    // 常に相対差（shapeRad/shapeAng）が決め、kalAmpは「今音が有効か・どれだけ
    // 派手に見せるか」だけを担う（小音量でも曲の特徴が消えない＝■2の核心）。
    float audioAvg = (fabsf(shapeRad) + fabsf(shapeAng)) * 0.5f * kalAmp;   // このリングの音の強さ（色の演出に使う。常に正）

    // A→Bモーフを進める。到達したら現在のBをAへ確定し、新しいB・新しい周期を抽選して
    // 無限に次の形へ向かい続ける（sin()による往復ではなく、常に新しい目標へのモーフ）。
    // ■3・■10：モーフ速度は常に一定（音には一切連動させない）。優先順位は
    // 音反応＞自動モーフ＞回転なので、自動モーフ自体は控えめな一定ペースで進む。
    R.morphT += 1.0f / (R.morphPeriod * 14.2857f);   // 14.2857 ≈ 1000ms/70ms（1秒あたりのフレーム数）
    if (R.morphT >= 1.0f) kalPickNewTarget(i);
    float mt = R.morphT;

    // ■10：base shape＝A/Bモーフだけで決まる現在形状（音声値では一切書き換えない）。
    float gapBase  = R.gapA  + (R.gapB  - R.gapA)  * mt;   // リング厚み(px)。常にKAL_GAP_MIN..MAXの範囲
    float angBase  = R.angA  + (R.angB  - R.angA)  * mt;   // 可動頂点の角度(rad)。境界(0°/30°)には触れない
    float fracBase = R.fracA + (R.fracB - R.fracA) * mt;   // 可動頂点の半径比
    float hueBase  = R.hueA  + (R.hueB  - R.hueA)  * mt;   // 色相位置（大きく飛ぶことも許容し、色の重なりが変わる）

    float gap  = gapBase;   // リング厚みはA/Bモーフのみで決める（半径の単調増加を音で崩さないため）
    float rIn  = rPrev * pulseScale;
    float rOut = (rPrev + gap) * pulseScale;
    rPrev += gap;   // 次のリングの内周半径＝このリングの外周半径（隙間なく連続）

    // 境界固定4頂点（θ=0°とθ=30°上。半径のみA/Bモーフ・bassパルスで変化＝角度は絶対に動かない）
    float pIn0x  = rIn  * KAL_A0_COS,  pIn0y  = rIn  * KAL_A0_SIN;
    float pIn1x  = rIn  * KAL_A30_COS, pIn1y  = rIn  * KAL_A30_SIN;
    float pOut0x = rOut * KAL_A0_COS,  pOut0y = rOut * KAL_A0_SIN;
    float pOut1x = rOut * KAL_A30_COS, pOut1y = rOut * KAL_A30_SIN;

    // ■4・■5・■10：final = base(A/Bモーフの位置, px単位) + audio deformation
    // (band由来のpx/radオフセット, 無音時ゼロ) + bassキック。frac(0..1)という
    // 共有レンジの奪い合いを避けるため、音の効果は最初からpx/rad単位の独立した
    // オフセットとしてbase位置へ加算し、最後にリング内へ収まるようクランプする
    // （通常入力では滑らかな可動域を使い、クランプは安全のための保険に留まる）。
    float rFBase = rIn + (rOut - rIn) * fracBase;
    float rF = rFBase + shapeRad * kalAmp * KAL_AUD_RADIUS_PX + kalBassPulse * KAL_BASS_KICK_PX;
    float marginPx = (rOut - rIn) * 0.12f; if (marginPx < 3.0f) marginPx = 3.0f;
    if (rF < rIn + marginPx) rF = rIn + marginPx;
    if (rF > rOut - marginPx) rF = rOut - marginPx;

    float ang = angBase + shapeAng * kalAmp * KAL_AUD_ANGLE_RAD;
    if (ang < KAL_ANG_MIN) ang = KAL_ANG_MIN;
    if (ang > KAL_ANG_MAX) ang = KAL_ANG_MAX;

    float hue = hueBase;   // 色相もA/Bモーフのみ。音の強さは後段でファセット間の色差・明るさへ反映する

    // 可動頂点：リング内部（境界には触れない）に1つだけ置く。A/Bモーフ（遅い）＋
    // 音の変形（速い）の合算で位置が決まる。
    float pfx = rF * cosf(ang), pfy = rF * sinf(ang);   // リング1個につきsin/cos1回だけ

    // 注意：ここではkalRotをまだ適用しない。θ=0°/30°の固定軸に対するdirect/mirror
    // 展開をkalDrawWedgeTri()内で先に行い、その後にcosR/sinRで全体回転を一括適用する
    // （理由はkalDrawWedgeTri()直前のコメント参照。反射変換の前後でkalRotを掛けると
    // 隣接コピーの境界がずれるため、必ず「展開後」に回転させる）。
    // ■12：音が鳴った領域が鮮やかになるよう、明るさに加えファセット間の色差
    // （KAL_FACET_HUE_OFSの効き）もこのリングの音の強さで広げる。
    int whiteBoost = 8 + (int)(audioAvg * 42.0f) + (int)(kalBassPulse * 55.0f);
    if (whiteBoost > 92) whiteBoost = 92;
    float hueSpread = 1.0f + audioAvg * 0.9f;   // 音が強いほどファセット同士の色差が広がる

    // 4枚のファセット：内周辺・外周辺・両側の境界辺をそれぞれ可動頂点と結ぶ。
    // 境界頂点（pIn0/pIn1/pOut0/pOut1）は全ファセット・全リングで一切動かないため、
    // 12方向複製後も隣接コピー・隣接リングと辺が正確に連結する。
    uint16_t col0 = light565Lerp(kalHueColor(hue + KAL_FACET_HUE_OFS[0] * hueSpread), WHITE, whiteBoost);
    uint16_t col1 = light565Lerp(kalHueColor(hue + KAL_FACET_HUE_OFS[1] * hueSpread), WHITE, whiteBoost);
    uint16_t col2 = light565Lerp(kalHueColor(hue + KAL_FACET_HUE_OFS[2] * hueSpread), WHITE, whiteBoost);
    uint16_t col3 = light565Lerp(kalHueColor(hue + KAL_FACET_HUE_OFS[3] * hueSpread), WHITE, whiteBoost);

    kalDrawWedgeTri(pIn0x,  pIn0y,  pIn1x,  pIn1y,  pfx, pfy, cosR, sinR, col0);   // 内周辺
    kalDrawWedgeTri(pIn1x,  pIn1y,  pOut1x, pOut1y, pfx, pfy, cosR, sinR, col1);   // θ=30°側の境界辺
    kalDrawWedgeTri(pOut1x, pOut1y, pOut0x, pOut0y, pfx, pfy, cosR, sinR, col2);   // 外周辺
    kalDrawWedgeTri(pOut0x, pOut0y, pIn0x,  pIn0y,  pfx, pfy, cosR, sinR, col3);   // θ=0°側の境界辺
  }

  GFX.clearClipRect();   // 顔描画・他の描画に影響しないよう必ず解除する

  // ── 顔（単体表示時のみ自前で描く。既存drawVisualizerFaceParts()は無変更）──
  if (!gLightingActive) {
    // Kaleidoscopeは黒背景のため、白背景前提の白リム省略（gLightingActive=false分岐）
    // のままだと黒地に黒目・黒鼻が埋もれる。既存関数は変更せず、直前に同じ固定
    // 座標（既存関数がgLightingActive時に描く縁と同一サイズ）へ白い下地だけ
    // 自前で敷いて対応する（Eye Slot使用中は目を敷かない）。この描画はウェッジの
    // 鏡映複製処理（kalDrawWedgeTri）を一切経由しない固定座標の単発描画である。
    if (!gEyeSlotActive) {
      GFX.fillCircle(90  + eyeOffsetX, 90 + eyeOffsetY, 22, WHITE);
      GFX.fillCircle(230 + eyeOffsetX, 90 + eyeOffsetY, 22, WHITE);
    }
    GFX.fillEllipse(noseX, noseY, 20, 14, WHITE);
    GFX.fillRect(135, 149, 51, 36, WHITE);   // 既存drawVisualizerFaceParts()の口消去矩形と同一範囲
    drawVisualizerFaceParts(needsInit);
  }
}

// ============================================================================
// 🎚 Analog VU（8連アナログ・スペクトラムメーター / v1.0 2026-07-29）
//
// ■ コンセプト
//   1970〜80年代のオーディオアンプ／ミキサーに並んでいたアナログVUメーターを、
//   「左右2chの音量計」ではなく「8バンドFFTそれぞれに1個ずつ」割り当てた
//   8連アナログ・スペクトラムメーターとして描く。
//     上段（左→右）：band0 / band1 / band2 / band3
//     下段（左→右）：band4 / band5 / band6 / band7
//   ＝ 左上から右下へ低域→高域。見ただけで周波数分布が分かる配置。
//
// ■ 既存構造からそのまま流用するもの（新規実装しない）
//   ・入力            … gViz.band[0..7]（FFT解析本体・8バンド生成には一切触れない）
//   ・バンド補正      … vizBandGain(i, n)（Graphic EQ / 8-Lane Rhythm と同一の式）
//   ・描画領域        … SCENE_TOP(48)〜SCENE_H-1。上部48pxの2行表示には描かない
//   ・顔             … Kaleidoscopeと同じ「黒背景用の白下地＋drawVisualizerFaceParts()」
//   ・ON/OFF判定      … 通常Visualizerと完全に同じ（独自graceは持たない）
//
// ■ レイアウト（320×240 / 上部48pxを除く 320×192 を 4列×2段へ等分）
//   セル       … 80 × 96（AVU_CELL_W × AVU_CELL_H）
//   盤面       … セル内 (+3, +8) から 74 × 76
//   針の回転軸 … セル内 (+40, +74)＝盤面下端の10px上
//   目盛り半径 … 内 34px / 外 42px、赤ゾーンは 38〜42px
//   針の長さ   … 38px（目盛りアークの内側で止まる）
//   針の角度   … -55°(最小) 〜 +55°(最大)。左下→上→右下の扇形。
//   ※ Rを42pxに抑えているのは、±55°での水平方向の張り出し
//      42×sin55°≒34.4px が盤面半幅37pxに収まる上限だから
//      （4×2配置で「一目でアナログメーター」と分かる最大サイズ）。
//
// ■ 8本の針は完全に独立（最重要）
//   avuNeedle[0..7] という8本分の独立したneedle stateを持ち、
//   band0〜band7それぞれの値だけから各針の目標角度を作る。
//   「音を鳴らすと8本が同じ角度になる」ことは構造上起こらない。
//
// ■ 小音量でも帯域差を残す（絶対値＋相対値のハイブリッド）
//   target = gate × ( AVU_W_ABS × 絶対成分 + AVU_W_REL × 相対成分 )
//     絶対成分 … gained[b] / AVU_ABS_FULL（音量を上げれば全体の振れ幅も増える）
//     相対成分 … (gained[b]/8band平均) を AVU_REL_LO..HI で正規化
//                 分子・分母が同じ音量係数で割れるため音量に依存しない
//                 ＝小音量でも「どの帯域が強い曲か」が針の差として必ず残る
//                 （Kaleidoscopeで採用した比率正規化と同じ考え方）
//   gate … 全体音量の高速包絡線に対する無音ゲート。無音時のノイズは拡大しない。
//
// ■ アナログ針らしいattack / release
//   FFT値を直接角度にせず、8本分の状態を非対称平滑化する。
//     上昇 … AVU_ATTACK  = 0.60（リズムが分かる速さ）
//     下降 … AVU_RELEASE = 0.14（機械式メーターの慣性）
//   描画周期60msでは attack は約3フレーム(180ms)で94%到達、
//   release は半減に約4.6フレーム(280ms)＝「もっさり」しない範囲の慣性。
//
// ■ 描画負荷・RAM
//   固定部分（目盛り7点・アーク13点・赤ゾーン16本の角度）のsin/cosは初回1回だけ
//   計算して静的テーブルに保持し、毎フレームのsinf/cosfは針8本分＝8回だけ。
//   新規Canvas・大きなLUTは追加しない。
//   追加RAM＝針8本(32B)＋包絡線(4B)＋初期化フラグ(1B)＋角度テーブル(7+13+16)×2×4B(288B)
//          ≒ 325byte（すべて静的。動的確保なし）。
// ============================================================================
#define AVU_TOP        SCENE_TOP                        // 48（上部48pxの2行表示は絶対に侵さない）
#define AVU_COLS       4
#define AVU_ROWS       2
#define AVU_CELL_W     (SCENE_W / AVU_COLS)             // 80
#define AVU_CELL_H     ((SCENE_H - AVU_TOP) / AVU_ROWS) // 96

#define AVU_PANEL_DX   3     // セル左端 → 盤面左端
#define AVU_PANEL_DY   8     // セル上端 → 盤面上端
#define AVU_PANEL_W    74
#define AVU_PANEL_H    76
#define AVU_PIVOT_DX   40    // セル左端 → 針の回転軸X（盤面中央）
#define AVU_PIVOT_DY   74    // セル上端 → 針の回転軸Y（盤面下端の10px上）
#define AVU_R_TICK_IN  34    // 主目盛りの内側半径
#define AVU_R_TICK_OUT 42    // 主目盛りの外側半径＝目盛りアークの半径
#define AVU_R_RED_IN   38    // 赤ピークゾーンの内側半径（外側はAVU_R_TICK_OUT）
#define AVU_R_NEEDLE   38    // 針の長さ
#define AVU_HUB_R      3     // 針の根元の軸の半径
#define AVU_NEEDLE_HW  1     // 針の根元の半幅(px)＝根元3px・先端1pxの細い三角形

#define AVU_TICK_N     7     // 主目盛りの本数（-55°〜+55°を6等分）
#define AVU_ARC_N      13    // 目盛りアークの折れ線分割点数（12セグメント＝小画面では滑らかな円弧に見える）
#define AVU_RED_N      16    // 赤ゾーンを塗る放射線の本数（間隔約1.2pxで隙間なく帯に見える）

static const float AVU_ANG_MIN_DEG = -55.0f;   // 針の最小角度（左下）
static const float AVU_ANG_MAX_DEG =  55.0f;   // 針の最大角度（右下）
static const float AVU_RED_START   = 0.75f;    // スケール比。ここから右がピーク（赤）領域＝主目盛り2本分

// 配色（RGB565）。盤面は明るいクリーム、背景は黒い機器パネル。
static const uint16_t AVU_C_FACE   = 0xF739;   // 盤面：クリーム／アイボリー #F5E6C8相当
static const uint16_t AVU_C_FRAME  = 0x2124;   // 外枠・軸：濃いグレー
static const uint16_t AVU_C_BEZEL  = 0x52AA;   // 外周ベゼル・軸のハイライト：中間グレー
static const uint16_t AVU_C_INK    = 0x2124;   // 目盛り・文字：濃色
static const uint16_t AVU_C_RED    = 0xC000;   // ピーク領域：やや暗い赤（明るい針と分離する）
static const uint16_t AVU_C_NEEDLE = 0xF800;   // 針：鮮やかな赤（クリーム上でも赤ゾーン上でも目立つ）
static const uint16_t AVU_C_BG     = 0x0000;   // メーター間の背景：黒

// 針の動特性（Analog VU専用。他Visualizerの平滑化には影響しない）
static const float AVU_ATTACK      = 0.60f;    // 立ち上がりは速く
static const float AVU_RELEASE     = 0.14f;    // 下降は機械式メーターらしく遅れて戻る
static const float AVU_LVL_ATTACK  = 0.50f;    // 全体音量包絡線（ゲート用）
static const float AVU_LVL_RELEASE = 0.18f;

// Analog VU専用のnoise floor / gate（無音時のFFTノイズで針を振らせないため）
static const float AVU_NOISE_FLOOR = 0.012f;   // これ未満の全体音量は完全な無音として扱う
static const float AVU_GATE_RANGE  = 0.040f;   // floorからこれだけ上がるとゲート全開＝小音量でも即座に帯域差が見える
static const float AVU_BAND_FLOOR  = 0.020f;   // 単一バンドのノイズ下限（gain適用後）

// 絶対成分／相対成分の変換パラメータ
static const float AVU_ABS_FULL    = 1.30f;    // gain適用後この値でフルスケール（絶対音量側）
static const float AVU_REL_EPS     = 0.060f;   // 相対比の分母下限。無音付近での比率暴走を防ぐ
static const float AVU_REL_LO      = 0.35f;    // 8band平均のこの倍率以下 → 相対成分0
static const float AVU_REL_HI      = 1.90f;    // 8band平均のこの倍率以上 → 相対成分1
static const float AVU_W_ABS       = 0.55f;    // 絶対成分の重み
static const float AVU_W_REL       = 0.60f;    // 相対成分の重み（合計1.15＝大音量かつ突出した帯域はピーク域へ振り切る）

// 周波数帯の識別表示（左上→右下＝低域→高域）
static const char* const AVU_LABEL[VIZ_SRC_BAND_COUNT] = {
  "L1", "L2", "M1", "M2", "M3", "M4", "H1", "H2"
};

// 8本分の独立した針の状態（0.0=最小角 / 1.0=最大角）
static float avuNeedle[VIZ_SRC_BAND_COUNT] = {0,0,0,0,0,0,0,0};
static float avuLevelEnv = 0.0f;    // 全体音量の高速包絡線（ゲート専用）

// 固定角度のsin/cosは初回1回だけ計算する（毎フレームの三角関数を針8本だけに絞る）
static bool  avuTabReady = false;
static float avuTickSin[AVU_TICK_N], avuTickCos[AVU_TICK_N];
static float avuArcSin[AVU_ARC_N],   avuArcCos[AVU_ARC_N];
static float avuRedSin[AVU_RED_N],   avuRedCos[AVU_RED_N];

static void avuInitTables() {
  const float D2R = 0.0174532925f;
  for (uint8_t k = 0; k < AVU_TICK_N; k++) {
    float t = (float)k / (float)(AVU_TICK_N - 1);
    float a = (AVU_ANG_MIN_DEG + t * (AVU_ANG_MAX_DEG - AVU_ANG_MIN_DEG)) * D2R;
    avuTickSin[k] = sinf(a);
    avuTickCos[k] = cosf(a);
  }
  for (uint8_t k = 0; k < AVU_ARC_N; k++) {
    float t = (float)k / (float)(AVU_ARC_N - 1);
    float a = (AVU_ANG_MIN_DEG + t * (AVU_ANG_MAX_DEG - AVU_ANG_MIN_DEG)) * D2R;
    avuArcSin[k] = sinf(a);
    avuArcCos[k] = cosf(a);
  }
  for (uint8_t k = 0; k < AVU_RED_N; k++) {
    float t = AVU_RED_START + (1.0f - AVU_RED_START) * ((float)k / (float)(AVU_RED_N - 1));
    float a = (AVU_ANG_MIN_DEG + t * (AVU_ANG_MAX_DEG - AVU_ANG_MIN_DEG)) * D2R;
    avuRedSin[k] = sinf(a);
    avuRedCos[k] = cosf(a);
  }
  avuTabReady = true;
}

void vizRenderAnalogVu(bool needsInit) {
  const AudioVizState& s = gViz;
  uint8_t bandN = s.bandCount;
  if (bandN == 0) return;
  if (bandN > VIZ_SRC_BAND_COUNT) bandN = VIZ_SRC_BAND_COUNT;

  if (!avuTabReady) avuInitTables();
  if (needsInit) {
    for (uint8_t b = 0; b < VIZ_SRC_BAND_COUNT; b++) avuNeedle[b] = 0.0f;
    avuLevelEnv = 0.0f;
  }

  // ── 1. 入力：既存の8バンド補正(vizBandGain)を適用した値だけを使う ──
  //    FFT解析本体・8バンド生成・vizUpdateState()には一切触れない。
  float gained[VIZ_SRC_BAND_COUNT];
  float mean = 0.0f;
  for (uint8_t b = 0; b < bandN; b++) {
    float g = s.band[b] * vizBandGain(b, bandN);   // Graphic EQ / Rhythmと同一のゲイン
    if (g < 0.0f) g = 0.0f;
    gained[b] = g;
    mean += g;
  }
  mean /= (float)bandN;

  // ── 2. 無音ゲート（Analog VU専用のnoise floor）──
  if (s.level > avuLevelEnv) avuLevelEnv += (s.level - avuLevelEnv) * AVU_LVL_ATTACK;
  else                       avuLevelEnv += (s.level - avuLevelEnv) * AVU_LVL_RELEASE;
  float gate = (avuLevelEnv - AVU_NOISE_FLOOR) / AVU_GATE_RANGE;
  if (gate < 0.0f) gate = 0.0f;
  if (gate > 1.0f) gate = 1.0f;

  float relDen = (mean > AVU_REL_EPS) ? mean : AVU_REL_EPS;   // 相対比の分母（下限つき）

  // ── 3. 8本の針を独立に更新（絶対成分＋相対成分 → attack/release）──
  for (uint8_t b = 0; b < VIZ_SRC_BAND_COUNT; b++) {
    float target = 0.0f;
    if (b < bandN && gained[b] >= AVU_BAND_FLOOR) {
      float aAbs = gained[b] / AVU_ABS_FULL;                  // 絶対音量（音量を上げれば全体が振れる）
      if (aAbs > 1.0f) aAbs = 1.0f;
      float ratio = gained[b] / relDen;                        // 8band平均に対する比率（音量に依存しない）
      float aRel = (ratio - AVU_REL_LO) / (AVU_REL_HI - AVU_REL_LO);
      if (aRel < 0.0f) aRel = 0.0f;
      if (aRel > 1.0f) aRel = 1.0f;
      target = gate * (AVU_W_ABS * aAbs + AVU_W_REL * aRel);
      if (target > 1.0f) target = 1.0f;
    }
    if (target > avuNeedle[b]) avuNeedle[b] += (target - avuNeedle[b]) * AVU_ATTACK;
    else                       avuNeedle[b] += (target - avuNeedle[b]) * AVU_RELEASE;
    if (avuNeedle[b] < 0.0f) avuNeedle[b] = 0.0f;
    if (avuNeedle[b] > 1.0f) avuNeedle[b] = 1.0f;
  }

  // ── 4. 背景：暗い機器パネル（Lighting併用時は既存Lighting背景をそのまま活かす）──
  if (!gLightingActive) {
    GFX.fillRect(0, AVU_TOP, SCENE_W, SCENE_H - AVU_TOP, AVU_C_BG);
  }

  GFX.setTextSize(1);
  GFX.setTextDatum(MC_DATUM);
  GFX.setTextColor(AVU_C_INK);

  const float D2R = 0.0174532925f;

  // ── 5. 8個のメーターを描画（band0..7 → 上段左から / 下段左から）──
  for (uint8_t b = 0; b < VIZ_SRC_BAND_COUNT; b++) {
    int cx0 = (int)(b % AVU_COLS) * AVU_CELL_W;                 // セル左端X
    int cy0 = AVU_TOP + (int)(b / AVU_COLS) * AVU_CELL_H;       // セル上端Y（必ず48以上）
    int px  = cx0 + AVU_PANEL_DX;
    int py  = cy0 + AVU_PANEL_DY;
    int vx  = cx0 + AVU_PIVOT_DX;                               // 針の回転軸X
    int vy  = cy0 + AVU_PIVOT_DY;                               // 針の回転軸Y

    // 盤面（クリーム）＋濃色の外枠＋外周ベゼル
    GFX.fillRect(px, py, AVU_PANEL_W, AVU_PANEL_H, AVU_C_FACE);
    GFX.drawRect(px, py, AVU_PANEL_W, AVU_PANEL_H, AVU_C_FRAME);
    GFX.drawRect(px - 1, py - 1, AVU_PANEL_W + 2, AVU_PANEL_H + 2, AVU_C_BEZEL);

    // 右端の赤いピーク領域（放射状の短い線を密に並べて帯に見せる）
    for (uint8_t k = 0; k < AVU_RED_N; k++) {
      GFX.drawLine(vx + (int)lroundf((float)AVU_R_RED_IN   * avuRedSin[k]),
                   vy - (int)lroundf((float)AVU_R_RED_IN   * avuRedCos[k]),
                   vx + (int)lroundf((float)AVU_R_TICK_OUT * avuRedSin[k]),
                   vy - (int)lroundf((float)AVU_R_TICK_OUT * avuRedCos[k]),
                   AVU_C_RED);
    }

    // 目盛りアーク（外周を12セグメントの折れ線で描く）
    int prevX = 0, prevY = 0;
    for (uint8_t k = 0; k < AVU_ARC_N; k++) {
      int ax = vx + (int)lroundf((float)AVU_R_TICK_OUT * avuArcSin[k]);
      int ay = vy - (int)lroundf((float)AVU_R_TICK_OUT * avuArcCos[k]);
      if (k > 0) GFX.drawLine(prevX, prevY, ax, ay, AVU_C_INK);
      prevX = ax; prevY = ay;
    }

    // 主目盛り7本（右端2本はピーク領域なので赤）
    for (uint8_t k = 0; k < AVU_TICK_N; k++) {
      int ox = vx + (int)lroundf((float)AVU_R_TICK_OUT * avuTickSin[k]);
      int oy = vy - (int)lroundf((float)AVU_R_TICK_OUT * avuTickCos[k]);
      int ix = vx + (int)lroundf((float)AVU_R_TICK_IN  * avuTickSin[k]);
      int iy = vy - (int)lroundf((float)AVU_R_TICK_IN  * avuTickCos[k]);
      float tPos = (float)k / (float)(AVU_TICK_N - 1);
      GFX.drawLine(ix, iy, ox, oy, (tPos >= AVU_RED_START) ? AVU_C_RED : AVU_C_INK);
    }

    // 識別表示（盤面下端の左＝帯域ラベル／右＝VU）。針の可動域は軸より上のみなので
    // 下端の左右コーナーは針に隠れない。
    GFX.drawString(AVU_LABEL[b], cx0 + 22, cy0 + 77);
    GFX.drawString("VU",         cx0 + 58, cy0 + 77);

    // 針（根元が太く先端が細い三角形）＋軸。毎フレームのsinf/cosfはここだけ。
    float ang = (AVU_ANG_MIN_DEG + avuNeedle[b] * (AVU_ANG_MAX_DEG - AVU_ANG_MIN_DEG)) * D2R;
    float sa  = sinf(ang), ca = cosf(ang);
    int tipX = vx + (int)lroundf((float)AVU_R_NEEDLE * sa);
    int tipY = vy - (int)lroundf((float)AVU_R_NEEDLE * ca);
    int b1X  = vx + (int)lroundf((float)AVU_NEEDLE_HW * ca);   // 針方向(sa,-ca)に直交する(ca,sa)
    int b1Y  = vy + (int)lroundf((float)AVU_NEEDLE_HW * sa);
    int b2X  = vx - (int)lroundf((float)AVU_NEEDLE_HW * ca);
    int b2Y  = vy - (int)lroundf((float)AVU_NEEDLE_HW * sa);
    GFX.fillTriangle(b1X, b1Y, b2X, b2Y, tipX, tipY, AVU_C_NEEDLE);
    GFX.fillCircle(vx, vy, AVU_HUB_R, AVU_C_FRAME);
    GFX.fillCircle(vx, vy, 1, AVU_C_BEZEL);
  }

  GFX.setTextDatum(TL_DATUM);   // 既定へ戻す（他の描画へ影響させない）

  // ── 6. 顔（Visualizer → 顔 の描画順。目・鼻・口が最前面）──
  //    Kaleidoscopeと同じく黒背景のため、既存drawVisualizerFaceParts()は変更せず
  //    直前に同じ固定座標へ白い下地だけ敷く（顔の位置・サイズは一切変更しない）。
  if (!gLightingActive) {
    if (!gEyeSlotActive) {
      GFX.fillCircle(90  + eyeOffsetX, 90 + eyeOffsetY, 22, WHITE);
      GFX.fillCircle(230 + eyeOffsetX, 90 + eyeOffsetY, 22, WHITE);
    }
    GFX.fillEllipse(noseX, noseY, 20, 14, WHITE);
    GFX.fillRect(135, 149, 51, 36, WHITE);   // 既存drawVisualizerFaceParts()の口消去矩形と同一範囲
    drawVisualizerFaceParts(needsInit);
  }
}

// ============================================================================
// Visualizer #7 : Tetromino Dance（旧仮称 Mega Blocks）── 大型落ち物ブロック
//                 v2.0 / 2026-07-31（実機確認後の修正版）
//
// ■ 位置づけ（8-Lane Rhythmとは無関係の独立モード）
//   8-Lane Rhythmが「固定8レーン×時間履歴」という音ゲー的な流れる譜面なのに対し、
//   本Visualizerは「少数（最大BLK_MAX_PIECES個）の大型ブロックが画面内を自由に
//   落下・回転・移動する」という別系統の表現にする。FFTの8バンドを画面のX座標へ
//   直接対応させる方式（列マッピング）は採用しない。ブロックの色は「どのバンドが
//   立ち上がったか」から決めるが、X座標そのものはバンド番号と無関係
//   （random()による疎な配置）にすることで、低音が強い曲でも画面左に偏らない
//   ようにしている。
//
// ■ v2.0での変更点（実機確認結果を反映）
//   1) 背景を黒→白に変更。他の白背景Visualizer（Graphic EQ/8-Lane Rhythm/
//      Mirror Wave）と同じ扱いにし、黒背景専用だった「顔まわりの白下地」処理
//      （黒地でないと不要）を撤去した。
//   2) 回転を「瞬間切替」から「連続アニメーション」へ変更。ピースを外接矩形の
//      左上ではなく中心(cx,cy)で管理し、現在角度angleが目標角度targetAngle
//      （常に90°=PI/2単位）へBLK_ROT_STEPずつ毎フレーム近づく。描画時は
//      各セルの中心・4頂点をangleぶん回転行列で変換してfillTriangle×2枚
//      （矩形1枚）で塗るため、0/90/180/270度の4テーブルは不要になり、
//      形状は「回転前の基準姿勢」を1つ持つだけでよい。中心(cx,cy)は回転で
//      動かないため、ブロックが飛ばずその場でクルッと回って見える。
//   3) 「落下途中の真横移動」を、常時ドリフトとは別の離散イベントとして追加。
//      普段cxは変化せず純粋に落下のみ。ときどき（タイマー or bass立ち上がり
//      抽選）1〜2セル分の目標Xを決め、BLK_SLIDE_DUR_MS程度かけて
//      smoothstep補間でcxを移動、到達したら再び純粋な落下に戻る。
//      常時斜めに流れる古いdriftDirは廃止した。
//   4) 名称を仮称「Mega Blocks」から正式名称「Tetromino Dance」へ変更
//      （VIZ_MODESのlabel/noteのみ変更。内部id "blocks" は互換性のため維持）。
//
// ■ サイズの基準（このファイルの既存定数から算出。数値の決め打ちはしない）
//   ・目（黒目）の半径 EYE_RADIUS = 20px → 直径40px
//   ・口の外接ボックス FACE_MOUTH_W=90 / FACE_MOUTH_H=105
//   目と口のおおよそ中間に位置する40pxを1セルの一辺（BLK_CELL）として採用し、
//   ブロックは3〜4セルで構成する。8-Lane Rhythmのバー（34×16px）よりも
//   明確に大きく、口(90〜105px)を大きく超えない範囲に収まる。
//
// ■ 形状
//   O（2×2セル全埋め）／I（3セル一直線）／L（2×2セルの1隅を欠いたトロミノ）の
//   3種類。回転は基準姿勢1つに対する連続回転行列で表現するため、形状テーブル
//   自体は4方向ぶん持たない（v1.0からの簡略化）。
//
// ■ 音への反応（既存処理を再利用。新しい解析は追加しない）
//   ・落下速度        … gViz.level（全帯域平均）で加速。Kaleidoscope等と同じ
//                       ゆっくりした追従（減衰付き平滑）を用いる。
//   ・出現            … gViz.band[i]の立ち上がり検出は8-Lane Rhythm/
//                       Kaleidoscopeと同じ「直前値からの急上昇＋クールダウン」
//                       方式をバンドごとに判定し、立ち上がったバンドの色
//                       （vizSpectrumColor）をそのブロックの色として使う
//                       （列位置には使わない）。
//   ・回転／真横移動の開始 … bassの立ち上がり（KaleidoscopeのkalBassAvgと
//                       同じ考え方の局所平均比較）またはタイマーをきっかけに、
//                       既存ブロックのどれか1つへランダムに適用する。
//
// ■ 積み上げなし
//   下端（SCENE_H）を完全に通過したブロックはそのまま非アクティブ化して消え、
//   次の出現までは何も描かれない。盤面を保持する目的の配列は持たない。
//
// ■ 顔（維持）
//   白背景→ブロック→drawVisualizerFaceParts() の順で最後に顔を描くため、
//   ブロックが顔の手前に重なって顔を隠すことはない。顔の描画仕様・表情処理は
//   無変更。
// ============================================================================
#define BLK_TOP          SCENE_TOP        // 48。他Visualizerと同じ保護ライン
#define BLK_CELL         40                // 1セル(px)。EYE_RADIUS(20)の直径と同じ大きさを基準にする
#define BLK_MAX_PIECES   4                 // 少数のみ同時表示
#define BLK_MAX_CELLS    4                 // 1ピース最大セル数（O=4）
// どの形状・どの回転角でも、ピース中心からセル最遠角までの距離はこれを超えない
// （I字形×連続回転時の最大値 約63pxを切り上げ）。画面外はみ出し防止の共通マージンに使う。
#define BLK_HALF_EXTENT  64
#define BLK_ROT_STEP     0.2618f           // 1フレームあたりの回転角(rad)。(PI/2)/6＝6フレームで90°
#define BLK_SLIDE_MIN_MS 300
#define BLK_SLIDE_MAX_MS 500

// 形状定義：回転前の基準姿勢のみを持つ（0/90/180/270の4テーブルは不要）。
// (col,row)はセル単位のグリッド座標。
static const int8_t BLK_SHAPE_O[4][2] = { {0,0},{1,0},{0,1},{1,1} };   // 2×2
static const int8_t BLK_SHAPE_I[3][2] = { {0,0},{1,0},{2,0} };          // 3×1（横向き基準）
static const int8_t BLK_SHAPE_L[3][2] = { {0,0},{1,0},{0,1} };          // 2×2の右下(1,1)が欠け

struct BlkPiece {
  bool     active;
  float    cx, cy;        // ピース中心のピクセル座標（回転軸）
  float    vy;            // 落下速度(px/frame)
  uint8_t  shape;         // 0=O,1=I,2=L
  float    angle;         // 現在の描画角度(rad)。連続値
  float    targetAngle;   // 回転アニメーションの目標角度(rad)
  uint16_t color;
  unsigned long nextRotMs;     // 次に回転開始を検討する時刻
  bool     sliding;             // 真横移動アニメーション中か
  float    slideStartX, slideTargetX;
  unsigned long slideStartMs, slideDurMs;
  unsigned long nextSlideMs;    // 次に真横移動の開始を検討する時刻
};
static BlkPiece blkPieces[BLK_MAX_PIECES];
static bool     blkReady = false;

static float    blkLevelSmooth = 0.0f;     // gViz.levelの緩やかな平滑値（落下速度に使用）
static float    blkBassAvg     = 0.0f;     // bassの局所平均（立ち上がり検出の基準）
static unsigned long blkBeatCooldownUntil = 0;
static int      blkPrevBandLvl[VIZ_SRC_BAND_COUNT] = {0,0,0,0,0,0,0,0};
static unsigned long blkBandCooldownUntil[VIZ_SRC_BAND_COUNT] = {0,0,0,0,0,0,0,0};
static unsigned long blkLastSpawnMs = 0;

// ピースが占めるセル数（O=4、I/L=3）。
static inline uint8_t blkCellCount(uint8_t shape) { return (shape == 0) ? 4 : 3; }

// 基準姿勢でのセル(col,row)を取得。
// BLK_SHAPE_O/I/L はいずれも [N][2] 型なので、どれも const int8_t(*)[2] へ
// 同じように decay する（キャスト不要）。
static inline void blkGetCell(uint8_t shape, uint8_t idx, int8_t& col, int8_t& row) {
  const int8_t (*o)[2] = (shape == 0) ? BLK_SHAPE_O : (shape == 1) ? BLK_SHAPE_I : BLK_SHAPE_L;
  col = o[idx][0];
  row = o[idx][1];
}

// 基準姿勢での外接ボックスセル数（O=2×2、I=3×1、L=2×2）。
static inline void blkShapeDims(uint8_t shape, uint8_t& wCells, uint8_t& hCells) {
  if (shape == 1) { wCells = 3; hCells = 1; }
  else            { wCells = 2; hCells = 2; }   // O・L は共通の2×2箱
}

// Lighting併用時だけ、回転済みセル四角形へ簡易輪郭（白1本線）を付ける。
// 既存vizOutlineRect（軸並行矩形専用・白外/黒内の二重輪郭）は回転四角形に
// そのまま使えないため、視認性確保のための簡略版として白線のみにしている。
static inline void blkOutlineQuad(int x0,int y0,int x1,int y1,int x2,int y2,int x3,int y3) {
  if (!gLightingActive) return;
  GFX.drawLine(x0,y0,x1,y1,WHITE);
  GFX.drawLine(x1,y1,x2,y2,WHITE);
  GFX.drawLine(x2,y2,x3,y3,WHITE);
  GFX.drawLine(x3,y3,x0,y0,WHITE);
}

// ピースを空いているスロットへ新規生成する。x/形状/回転初期値はrandom()、
// 色だけ「立ち上がったバンド」から決める（列位置には使わない＝8列直結を避ける）。
static void blkSpawn(uint16_t color) {
  int slot = -1;
  for (int i = 0; i < BLK_MAX_PIECES; i++) { if (!blkPieces[i].active) { slot = i; break; } }
  if (slot < 0) return;   // 空きが無ければ今回は諦める（強制的な入れ替えはしない）

  uint8_t shape = (uint8_t)random(0, 3);   // 0=O,1=I,2=L
  unsigned long nowMs = millis();

  BlkPiece& p = blkPieces[slot];
  p.active      = true;
  p.cx          = (float)random(BLK_HALF_EXTENT, SCENE_W - BLK_HALF_EXTENT + 1);
  p.cy          = (float)(BLK_TOP - BLK_HALF_EXTENT);   // 画面上端のすぐ外から落ちてくる
  p.vy          = 0.6f;
  p.shape       = shape;
  p.angle       = 0.0f;
  p.targetAngle = 0.0f;
  p.color       = color;
  p.nextRotMs   = nowMs + (unsigned long)random(900, 1700);
  p.sliding     = false;
  p.slideStartX = p.slideTargetX = p.cx;
  p.slideStartMs = p.slideDurMs = 0;
  p.nextSlideMs  = nowMs + (unsigned long)random(1200, 2200);
}

void vizRenderMegaBlocks(bool needsInit) {
  const AudioVizState& s = gViz;
  uint8_t n = s.bandCount;

  if (!blkReady || needsInit) {
    for (int i = 0; i < BLK_MAX_PIECES; i++) blkPieces[i].active = false;
    blkLevelSmooth = 0.0f;
    blkBassAvg     = s.bass;
    blkBeatCooldownUntil = 0;
    for (int i = 0; i < VIZ_SRC_BAND_COUNT; i++) { blkPrevBandLvl[i] = 0; blkBandCooldownUntil[i] = 0; }
    blkLastSpawnMs = 0;
    blkReady = true;
  }

  unsigned long nowMs = millis();

  // ── 背景：白（他の白背景Visualizerと同じ扱い。Lighting併用時は既存Lighting
  //    背景をそのまま活かし何も塗らない）。ブロックが毎フレーム動くため、
  //    Kaleidoscopeと同様に「差分ではなく毎フレーム全面を塗り直す」方式にする。──
  if (!gLightingActive) GFX.fillRect(0, BLK_TOP, SCENE_W, SCENE_H - BLK_TOP, WHITE);
  GFX.setClipRect(0, BLK_TOP, SCENE_W, SCENE_H - BLK_TOP);   // 上部48pxの情報表示は汚さない

  // ── 落下速度：level（全帯域平均）の緩やかな追従 ──
  blkLevelSmooth += (s.level - blkLevelSmooth) * 0.10f;

  // ── バンドごとの立ち上がり検出 → 出現（色にのみ使用。列位置には使わない）──
  for (uint8_t i = 0; i < n && i < VIZ_SRC_BAND_COUNT; i++) {
    int lvl  = (int)lroundf(s.band[i] * 100.0f * vizBandGain(i, n));
    if (lvl > 100) lvl = 100;
    int rise = lvl - blkPrevBandLvl[i];
    if (lvl >= 15 && rise >= 12 && nowMs >= blkBandCooldownUntil[i] && nowMs - blkLastSpawnMs >= 220) {
      blkSpawn(vizSpectrumColor((VIZ_SRC_BAND_COUNT > 1) ? (float)i / (float)(VIZ_SRC_BAND_COUNT - 1) : 0.0f));
      blkBandCooldownUntil[i] = nowMs + 500;
      blkLastSpawnMs = nowMs;
    }
    blkPrevBandLvl[i] = lvl;
  }
  // 無音が続いて画面が空のままにならないよう、一定間隔で最低限の出現を保証する。
  {
    int activeCount = 0;
    for (int i = 0; i < BLK_MAX_PIECES; i++) if (blkPieces[i].active) activeCount++;
    if (activeCount == 0 && nowMs - blkLastSpawnMs >= 2500) {
      blkSpawn(vizSpectrumColor(0.5f));
      blkLastSpawnMs = nowMs;
    }
  }

  // ── bassの立ち上がり検出（Kaleidoscopeと同じ局所平均比較）──
  blkBassAvg += (s.bass - blkBassAvg) * 0.15f;
  bool bassHit = (s.bass > blkBassAvg * 1.3f + 0.04f && nowMs >= blkBeatCooldownUntil);
  if (bassHit) blkBeatCooldownUntil = nowMs + 200;

  // ── 各ピースの更新・描画 ──
  for (int i = 0; i < BLK_MAX_PIECES; i++) {
    BlkPiece& p = blkPieces[i];
    if (!p.active) continue;

    // 1) 落下（縦方向のみ。真横移動は下のスライド処理が別途cxを動かす）
    p.cy += p.vy * (1.0f + blkLevelSmooth * 2.2f);

    // 2) 回転アニメーション：目標角度へBLK_ROT_STEPずつ近づく（中心固定）
    if (p.angle != p.targetAngle) {
      float diff = p.targetAngle - p.angle;
      if (fabsf(diff) <= BLK_ROT_STEP) p.angle = p.targetAngle;
      else p.angle += (diff > 0 ? BLK_ROT_STEP : -BLK_ROT_STEP);
      if (p.angle == p.targetAngle) {
        // 浮動小数の際限ない増加を防ぐため、一致したタイミングで2πぶん折り返す
        while (p.angle >= 6.2831853f) { p.angle -= 6.2831853f; p.targetAngle -= 6.2831853f; }
      }
    } else {
      bool rotTimeDue = (nowMs >= p.nextRotMs);
      // Oは回転しても見た目がほぼ変わらないため、新規回転の対象からは外す
      // （既に回転中なら上のアニメーションはそのまま最後まで完了させる）。
      if (p.shape != 0 && (rotTimeDue || (bassHit && random(0, BLK_MAX_PIECES) == 0))) {
        p.targetAngle = p.angle + 1.5707963f;   // +90°
        p.nextRotMs   = nowMs + (unsigned long)random(900, 1700);
      }
    }

    // 3) 真横移動（スライド）：普段は横に動かず、ときどき1〜2セル分だけ
    //    smoothstep補間で移動し、終わったらまた純粋な落下へ戻る。
    if (p.sliding) {
      unsigned long elapsed = nowMs - p.slideStartMs;
      float t = (p.slideDurMs > 0) ? (float)elapsed / (float)p.slideDurMs : 1.0f;
      if (t >= 1.0f) { p.cx = p.slideTargetX; p.sliding = false; }
      else {
        float e = t * t * (3.0f - 2.0f * t);   // smoothstep
        p.cx = p.slideStartX + (p.slideTargetX - p.slideStartX) * e;
      }
    } else if (nowMs >= p.nextSlideMs) {
      // 毎回スライドするわけではなく、常時漂う表現にならないよう半分は見送る
      if (random(0, 2) == 0) {
        int   dir    = (random(0, 2) == 0) ? -1 : 1;
        float dist   = (float)random(1, 3) * BLK_CELL;   // 1〜2セル分
        float target = p.cx + (float)dir * dist;
        if (target < BLK_HALF_EXTENT)              target = (float)BLK_HALF_EXTENT;
        if (target > SCENE_W - BLK_HALF_EXTENT)    target = (float)(SCENE_W - BLK_HALF_EXTENT);
        if (fabsf(target - p.cx) > 4.0f) {   // 画面端で動けない場合はスキップ
          p.sliding      = true;
          p.slideStartX  = p.cx;
          p.slideTargetX = target;
          p.slideStartMs = nowMs;
          p.slideDurMs   = (unsigned long)random(BLK_SLIDE_MIN_MS, BLK_SLIDE_MAX_MS);
        }
      }
      p.nextSlideMs = nowMs + (unsigned long)random(1200, 2200);
    }

    // 4) 下端を完全に通過したら消える（積み上げない）
    if (p.cy - BLK_HALF_EXTENT > (float)SCENE_H) { p.active = false; continue; }

    // 5) 描画：各セルを中心(cx,cy)まわりにangleぶん回転させた四角形として塗る
    float cosA = cosf(p.angle), sinA = sinf(p.angle);
    uint8_t wCells, hCells;
    blkShapeDims(p.shape, wCells, hCells);
    float halfCellPx = (float)(BLK_CELL - 2) / 2.0f;   // セル間に1px隙間を残す

    for (uint8_t c = 0; c < blkCellCount(p.shape); c++) {
      int8_t col, row; blkGetCell(p.shape, c, col, row);
      // セル中心の「回転前・ピース中心基準」ローカル座標
      float lcx = ((float)col - (float)(wCells - 1) / 2.0f) * (float)BLK_CELL;
      float lcy = ((float)row - (float)(hCells - 1) / 2.0f) * (float)BLK_CELL;

      // セルの4頂点（ローカル座標）を求め、まとめて回転→平行移動する。
      float lx[4] = { lcx - halfCellPx, lcx + halfCellPx, lcx + halfCellPx, lcx - halfCellPx };
      float ly[4] = { lcy - halfCellPx, lcy - halfCellPx, lcy + halfCellPx, lcy + halfCellPx };
      int wx[4], wy[4];
      for (int k = 0; k < 4; k++) {
        float rx = lx[k] * cosA - ly[k] * sinA;
        float ry = lx[k] * sinA + ly[k] * cosA;
        wx[k] = (int)lroundf(p.cx + rx);
        wy[k] = (int)lroundf(p.cy + ry);
      }
      GFX.fillTriangle(wx[0], wy[0], wx[1], wy[1], wx[2], wy[2], p.color);
      GFX.fillTriangle(wx[0], wy[0], wx[2], wy[2], wx[3], wy[3], p.color);
      blkOutlineQuad(wx[0], wy[0], wx[1], wy[1], wx[2], wy[2], wx[3], wy[3]);
    }
  }

  GFX.clearClipRect();   // 顔描画・他の描画に影響しないよう必ず解除する（Kaleidoscopeと同じ作法）

  // ── 顔（ブロック→顔 の描画順で最後に描く。ブロックが顔を覆い隠さないように
  //    する。白背景のためKaleidoscope/Analog VUのような黒背景用の白下地は不要）──
  if (!gLightingActive) drawVisualizerFaceParts(needsInit);
}

// ============================================================================
// Visualizer Manager
//
// 【Visualizerを追加する手順（このファイル内で完結）】
//   1. void vizRenderXxx(bool needsInit) を上に実装（入力は共通ステート gViz を読む）
//      （needsInit==true のとき fillScreen(WHITE) と内部状態のリセットを行う）
//   2. ファイル前方の enum VisualizerMode に VIZ_MODE_XXX を追加
//   3. ファイル前方の VIZ_MODES[] に {"xxx", "表示名", "説明"} を追加
//   4. 下の VIZ_RENDER_FN[] / VIZ_INTERVAL_MS[] へ同じ並び順で追加
//   → WebUI・NVS・入力元の切替処理は一切変更不要。
// ============================================================================
typedef void (*VizRenderFn)(bool needsInit);

// enum VisualizerMode と同じ並び順（OFFはNULL）
const VizRenderFn VIZ_RENDER_FN[VIZ_MODE_COUNT] = {
  NULL,                 // VIZ_MODE_OFF
  vizRenderGraphicEq,   // VIZ_MODE_EQ
  vizRenderAudioHalo,   // VIZ_MODE_HALO
  vizRenderMirrorWave,  // VIZ_MODE_MIRROR
  vizRenderRhythm,      // VIZ_MODE_RHYTHM
  vizRenderKaleidoscope,// VIZ_MODE_KALEIDO
  vizRenderAnalogVu,    // VIZ_MODE_AVU
  vizRenderMegaBlocks,  // VIZ_MODE_BLOCKS
};

// 各Visualizerの描画周期(ms)。既存EQは従来どおり80ms。
// 8-Lane Rhythmはノートのスクロールを滑らかに見せるため40ms（25fps相当）とする。
// Kaleidoscopeは種図形28個×最大12コピー程度の軽量描画のためHalo/Mirrorと同じ70msとする。
// Analog VUは針の動きを機械式メーターらしく滑らかに見せるため60ms（約16.7fps）とする。
// 1フレームの描画は8メーター×約30プリミティブ＝約240プリミティブで、
// Mirror Wave（81列×最大3回≒243回）と同程度のオーダーに収まる。
const uint16_t VIZ_INTERVAL_MS[VIZ_MODE_COUNT] = { 0, 80, 70, 70, 40, 70, 60, 70 };

// ====================================================
// Visualizer 有効判定と切替
//
// 有効条件（すべて満たす時だけ描画する）：
//   1. FFT_DISPLAY_TEST が有効（このブロック自体が #ifdef 内）
//   2. cfg_visualizerMode != OFF
//   3. 音声ソースがUDPまたはLINE IN
//      → 内蔵マイクモード(AUDIO_SRC_MIC)では絶対に表示されない
//   4. 直近 FFT_FACE_ACTIVE_MS 以内に有効なFFTパケットを受信している
//   5. 睡眠中でない・PNG顔（Face Gallery/独り言）表示中でない
//
// loop()の最後から毎回呼ばれる（早期returnはしない）。
// 条件が崩れた瞬間に drawFace() で通常顔へ復帰し、
// 再び成立した瞬間に白背景から描き直す。
// ====================================================
const unsigned long FFT_FACE_ACTIVE_MS = 500;
// visualizerFaceActive（現在Visualizer表示中か）は Unified Scene Canvas ブロックで宣言済み。
static VisualizerMode vizRenderedMode = VIZ_MODE_OFF;  // 直前フレームで描いたモード
static bool           vizNeedsInit    = true;          // 次フレームで全面初期化するか

bool isVisualizerFaceEnabled() {
  return cfg_visualizerMode != VIZ_MODE_OFF &&
         (audioSource == AUDIO_SRC_UDP || audioSource == AUDIO_SRC_LINEIN) &&
         lastFftPacketTime != 0 &&
         (millis() - lastFftPacketTime) <= FFT_FACE_ACTIVE_MS;
}

// 8-Lane Rhythm限定の「一時途絶グレース」。
// isVisualizerFaceEnabled()はFFTパケット受信からFFT_FACE_ACTIVE_MS(500ms)を
// 過ぎると即座にfalseを返す（この判定・定数はGraphic EQ/Halo/Mirrorも共有しており
// ここでは変更しない）。実機では約30秒に1回程度、何らかの理由でFFT受信が
// 500msを僅かに超えて途切れることがあり、その瞬間だけ isVisualizerFaceEnabled()が
// falseになる→ active が false→true をまたぐ→ 下のOFF遷移で drawFace()（白背景の
// 通常顔）が呼ばれる、という経路で「一瞬だけ白画面が見える」現象になっていた
// （履歴(rhyHist)自体は前回修正済みの通り消えない。あくまで「一瞬別の画面に
// 切り替わって見える」問題）。
// 8-Lane Rhythmだけは、直前まで表示できていた場合に限り、rawActiveがfalseに
// なってからRHYTHM_OFF_GRACE_MSの間は active を true のまま維持する。これにより
// visualizerFaceActive・drawFace()呼び出し・vizNeedsInitのいずれも動かないため
// 画面は前回描画されたRhythmの絵のままになる（新しいFFTが来ないのでgViz.bandは
// vizUpdateState()側の既存の減衰仕様に従って静かになるだけで、履歴のスクロール自体は
// 続く）。猶予を超えて本当に途絶した場合は、他モードと同じくOFF遷移する
// （＝音声入力が本当に終了した場合の復帰仕様はここでは変更していない）。
// Graphic EQ/Halo/Mirror、および isVisualizerFaceEnabled()/FFT_FACE_ACTIVE_MS
// そのものには一切手を入れていない。
const unsigned long RHYTHM_OFF_GRACE_MS = 3000;  // 500ms級のジッタを十分に飲み込む猶予

// ============================================================================
// isVisualizerFaceEnabledWithGrace()（2026-07-29 追加）
//
// 【背景】Kaleidoscope専用の一時途絶グレースは、Lighting OFF時に呼ばれる
// updateVisualizerFace()の中にしか実装していなかった。Lighting ON時は
// updateScreenEffects()が別経路でisVisualizerFaceEnabled()を直接呼んでおり
// （bool vizOn = isVisualizerFaceEnabled();）、このgraceを一切通らないまま
// sceneDrawVisualizerLayer(vizOn, ...)へ渡していた。そのためLighting ON中は
// FFTがFFT_FACE_ACTIVE_MS(500ms)をわずかに超えた瞬間にvizOn=falseとなり、
// その1フレームだけKaleidoscopeレイヤーが描かれず一瞬消えていた
// （Lighting OFF側は前回追加したgraceで既に解消済みだった）。
//
// 【方針】「このフレームでVisualizerを表示継続してよいか」の判定を、
// Lighting OFF経路(updateVisualizerFace)・Lighting ON経路(updateScreenEffects)の
// 両方から呼ばれる、この関数1か所へ統一する。
//   ・Kaleidoscope以外のモード … isVisualizerFaceEnabled()の結果をそのまま返す
//     （8-Lane RhythmのRHYTHM_OFF_GRACE_MSは、updateVisualizerFace()内の
//     従来どおり独立したロジックのままで、この関数には一切関与しない）。
//   ・Kaleidoscope … FFTがFFT_FACE_ACTIVE_MSを超えても、直前まで表示できて
//     いた場合に限りKAL_OFF_GRACE_MS(1200ms・今回は変更しない)の間はtrueを
//     維持する。猶予を超えて本当に途絶した場合、またはモードが切り替わった
//     場合は即座に通常判定へ戻る（次のisVisualizerFaceEnabled()＝falseを返す）。
// FFT_FACE_ACTIVE_MS・isVisualizerFaceEnabled()自体・RHYTHM_OFF_GRACE_MSの
// 値や適用条件は一切変更していない。
// ============================================================================
const unsigned long KAL_OFF_GRACE_MS = 1200;  // 実測の超過幅(11〜58ms)に対し十分な余裕を持たせつつ、本当の停止からの復帰を長時間遅らせない値（変更していない）

bool isVisualizerFaceEnabledWithGrace(uint8_t mode) {
  bool fftEligible = isVisualizerFaceEnabled();

  // モードが切り替わった直後は、以前のKaleidoscope猶予状態を持ち越さない
  // （■8：手動切替・Random切替でも猶予が誤って延命しないようにするため）。
  static uint8_t lastGraceMode = 255;
  static bool kalWasVisible = false;
  static unsigned long kalInactiveSinceMs = 0;
  if (mode != lastGraceMode) {
    lastGraceMode = mode;
    kalWasVisible = false;
    kalInactiveSinceMs = 0;
  }

  if (mode != (uint8_t)VIZ_MODE_KALEIDO) return fftEligible;

  unsigned long nowMs = millis();
  if (fftEligible) {
    kalInactiveSinceMs = 0;
    kalWasVisible = true;
    return true;
  }
  if (kalWasVisible) {
    if (kalInactiveSinceMs == 0) kalInactiveSinceMs = nowMs;
    bool withinGrace = (nowMs - kalInactiveSinceMs) < KAL_OFF_GRACE_MS;
    if (!withinGrace) kalWasVisible = false;   // 猶予切れ＝以後は通常判定に戻す
    return withinGrace;
  }
  return false;
}

void updateVisualizerFace() {
  bool rawActive = isVisualizerFaceEnabled() && !sleepMode && !imageFaceMode;
  bool active = rawActive;

  // グレースは「FFT受信側の一時的な途絶」だけに適用する。
  // sleepMode/imageFaceModeへの遷移が理由でrawActiveがfalseになった場合は、
  // 猶予せず既存仕様どおり即座にOFFへ従う（睡眠画面等、Canvas統合対象外の
  // 特殊画面への遷移を遅らせないため）。
  bool fftEligible = isVisualizerFaceEnabled();
  if (cfg_visualizerMode == VIZ_MODE_RHYTHM && !sleepMode && !imageFaceMode) {
    static unsigned long rhythmInactiveSinceMs = 0;
    unsigned long nowMs = millis();
    if (fftEligible) {
      rhythmInactiveSinceMs = 0;
      active = true;
    } else if (visualizerFaceActive) {
      // 直前まで表示中だった → 猶予時間内は「まだアクティブ」として扱い、
      // 前回描画されたRhythm画面をそのまま維持する。
      if (rhythmInactiveSinceMs == 0) rhythmInactiveSinceMs = nowMs;
      active = (nowMs - rhythmInactiveSinceMs) < RHYTHM_OFF_GRACE_MS;
    } else {
      active = false;
    }
  }

  // Kaleidoscope専用の一時途絶グレース（2026-07-29追加・2026-07-29二次修正で
  // Lighting ON経路と共通のisVisualizerFaceEnabledWithGrace()へ統一）。
  // Lighting ON時のupdateScreenEffects()からも同じ関数を呼ぶことで、
  // Lighting OFF/ONのどちらでもKaleidoscopeの一時途絶グレースが一貫して効く。
  // 8-Lane Rhythmの上のブロックには一切触れていない。
  if (cfg_visualizerMode == VIZ_MODE_KALEIDO && !sleepMode && !imageFaceMode) {
    active = isVisualizerFaceEnabledWithGrace((uint8_t)cfg_visualizerMode);
  }

  if (active != visualizerFaceActive) {
    visualizerFaceActive = active;
    if (active) {
      // ON遷移：白背景から描き直す（通常顔も白背景のため遷移は自然）
      vizNeedsInit    = true;
      vizRenderedMode = cfg_visualizerMode;
      addLog(String("VISUALIZER ON: ") + String(VIZ_MODES[cfg_visualizerMode].id));
    } else {
      // OFF遷移：通常顔へ自然に復帰する。
      // PNG顔・睡眠顔が表示中の場合はその画面を壊さない。
      if (!imageFaceMode && !sleepMode) {
        drawFace();
      }
      addLog("VISUALIZER OFF");
    }
  }

  if (!active) return;

  // 表示中にWebUIからモードが変わった場合：全面初期化して旧表示のゴミを残さない
  if (cfg_visualizerMode != vizRenderedMode) {
    vizRenderedMode = cfg_visualizerMode;
    vizNeedsInit    = true;
    addLog(String("VISUALIZER MODE: ") + String(VIZ_MODES[cfg_visualizerMode].id));
  }

  uint8_t m = (uint8_t)cfg_visualizerMode;
  if (m >= (uint8_t)VIZ_MODE_COUNT || VIZ_RENDER_FN[m] == NULL) return;

  // 描画周期の制限（既存処理のリアルタイム性を守る）
  static unsigned long lastVizFrameMs = 0;
  unsigned long now = millis();
  if (!vizNeedsInit && (now - lastVizFrameMs) < VIZ_INTERVAL_MS[m]) return;
  lastVizFrameMs = now;

  vizUpdateState((uint32_t)now);

  bool init    = vizNeedsInit;
  vizNeedsInit = false;

  // ── 統合描画パイプライン（Lighting時・通常表示時と同一経路）──
  //   Lightingは非表示（gLightingActive=false）なのでLayer0は描かれず、
  //   白背景の上にVisualizer→顔が従来どおりの順序で載る。
  sceneComposeAndPush(false, false, true, m, init,
                      0, SCENE_TOP, SCENE_W, SCENE_H - SCENE_TOP);
}
// ############################################################################
// #  KariPom Lighting Framework  v1.0  背景照明レイヤー（2026-07-22）        #
// ############################################################################
//
//   Layer0 Lighting → Layer1 Visualizer → Layer2 顔
//
// ・Lighting が1つでもONのとき、描画は「毎フレーム・レイヤー合成」へ切替わる。
//   Lighting OFF のときは従来どおり（updateVisualizerFace）で、Visualizerの
//   描画・見た目は 1バイトも変わらない。
// ・共存の要は gLightingActive フラグ。true のとき Visualizer と顔パーツは
//   「白で消さない・毎フレーム全描画」のオーバーレイモードで動く。
// ============================================================================

// ── 共通ヘルパ（RGB565を明るさ0..255で暗くする／白へ寄せる）──
static inline uint16_t light565Scale(uint16_t c, int b) {
  if (b >= 255) return c;
  if (b <= 0)   return 0x0000;
  int r = ((c >> 11) & 0x1F) * b / 255;
  int g = ((c >>  5) & 0x3F) * b / 255;
  int bl= ( c        & 0x1F) * b / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}
static inline uint16_t light565Lerp(uint16_t a, uint16_t c, int t) {  // t:0..255 で a→c
  if (t <= 0)   return a;
  if (t >= 255) return c;
  int ar=(a>>11)&0x1F, ag=(a>>5)&0x3F, ab=a&0x1F;
  int cr=(c>>11)&0x1F, cg=(c>>5)&0x3F, cb=c&0x1F;
  int r=ar+(cr-ar)*t/255, g=ag+(cg-ag)*t/255, bl=ab+(cb-ab)*t/255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// ── Lighting 共通の明るさ適用 ＋ 描画プリミティブ ──────────────
// gLightBriQ8（0..256）で RGB各chを同率スケール＝色味を変えず明るさだけ変更。
// 【重要】すべてのLightingは背景の描画に lightFillRect / lightDrawLine を使うこと。
//   これにより Framework共通のBrightnessが一括で効き、新しいLightingを追加しても
//   個別対応なしで自動適用される（＝Brightnessは各Lighting個別ではなく共通機能）。
static inline uint16_t lightBright(uint16_t c) {
  uint16_t q = gLightBriQ8;
  if (q >= 256) return c;
  int r = (((c >> 11) & 0x1F) * q) >> 8;
  int g = (((c >>  5) & 0x3F) * q) >> 8;
  int b = ((  c        & 0x1F) * q) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
static inline void lightFillRect(int x, int y, int w, int h, uint16_t c) {
  GFX.fillRect(x, y, w, h, lightBright(c));
}
static inline void lightDrawLine(int x0, int y0, int x1, int y1, uint16_t c) {
  GFX.drawLine(x0, y0, x1, y1, lightBright(c));
}

// ============================================================================
// Lighting #1 : Disco Floor
//
// ■ コンセプト
//   画面全体を 8×6 の大きなLEDタイル（40×40px）で敷き詰め、
//   虹色パレットが斜めに流れ（チェイス）、ビートでフロア全体がフラッシュする。
//   細かいドットではなく「一つ一つが存在感を持つ照明」。
//
// ■ 音との関係（測定器ではない）
//   ・全体レベルをオートゲイン(AGC)で持ち上げ、小音量でもフロアが大きく踊る
//   ・低音(bass)の立ち上がりを簡易ビート検出に使い、フラッシュとパレット送りの
//     “きっかけ”にする（FFTを忠実に表示はしない）
//   ・チェイスは時間ベースでも常に流れるので、静かな場面でも照明は生きている
//
// ■ 顔の可読性
//   目・鼻・口が乗るタイル(discoFaceTile[])は明るさに下限を設け、やや白へ寄せる。
//   ＝「演者へのスポットライト」。黒い目・鼻・口がどの色の上でも読める。
//   さらに顔タイルは毎フレーム再描画するので、口パクの旧形状が残らない。
//
// ■ 描画方式
//   ・Visualizer併用時(fullRepaint)は全48タイルを毎フレーム塗る
//     （Visualizerの残像をタイル塗りで消すため）
//   ・Disco単体(fullRepaint=false)は色が変わったタイル＋顔タイルだけ塗る（軽い）
//   ・sinf はタイル数ぶん(48)/フレームのみ。動的確保なし
// ============================================================================
#define DISCO_COLS   8
#define DISCO_ROWS   6
#define DISCO_TW     40
#define DISCO_TH     40
#define DISCO_TILES  (DISCO_COLS * DISCO_ROWS)   // 48
#define DISCO_NPAL   4                            // パレットセット数

// パレット（8色×4セット・8bit RGB。buildで565へ変換）
// ── カラーパレット（v1.4：Toy-box 原色）──────────────────
// 「クラブ照明」ではなく「LEGO / Nintendo / ゲームセンターのおもちゃ箱」を目指す。
// リアルさより“見た瞬間に楽しい！”を優先し、茶・オリーブ・暗青などの中間色を排除。
// 真っ赤 / 真黄 / 真緑 / 真青 / マゼンタ / シアン / 白 を中心とした高彩度・高明度。
// 色数を絞ってでも原色中心（中間色は増やさない）。
static const uint8_t DISCO_PAL_RGB[DISCO_NPAL][8][3] = {
  // 0: Toybox Rainbow（真っ赤/オレンジ/真黄/真緑/シアン/真青/マゼンタ/白）
  { {255,0,0},{255,120,0},{255,240,0},{0,220,0},{0,230,255},{0,90,255},{255,0,230},{255,255,255} },
  // 1: LEGO Bricks（赤・黄・青・緑の4原色ブロックを反復＝おもちゃブロック感）
  { {255,0,0},{255,240,0},{0,90,255},{0,220,0},{255,0,0},{255,240,0},{0,90,255},{0,220,0} },
  // 2: Arcade Neon（マゼンタ/シアン/黄/緑/青/白＝ゲームセンター）
  { {255,0,230},{0,230,255},{255,240,0},{0,220,0},{0,90,255},{255,0,230},{0,230,255},{255,255,255} },
  // 3: Nintendo Pop（赤/白/青/黄/緑/マゼンタ/シアン/オレンジ）
  { {255,0,0},{255,255,255},{0,90,255},{255,240,0},{0,220,0},{255,0,230},{0,230,255},{255,120,0} },
};

// ── 演出パターン（v1.1 で追加）──────────────────────────
// 明るさの「波」が各方向へ流れる。色パレットは別途ゆっくり回転して常に虹色。
// 20〜30秒ごとに自動で切り替わる（疑似ランダム・同じ物が連続しない）。
#define DISCO_NPAT   11
enum DiscoPattern : uint8_t {
  DISCO_PAT_TLBR = 0,   // 1 左上→右下
  DISCO_PAT_TRBL,       // 2 右上→左下
  DISCO_PAT_LR,         // 3 左→右
  DISCO_PAT_RL,         // 4 右→左
  DISCO_PAT_BT,         // 5 下→上
  DISCO_PAT_TB,         // 6 上→下
  DISCO_PAT_OUT,        // 7 中心→外
  DISCO_PAT_IN,         // 8 外→中心
  DISCO_PAT_CHECKER,    // 9 市松交互
  DISCO_PAT_PULSE,      // 10 全面パルス
  DISCO_PAT_SPARK       // 11 ランダムスパーク
};
static const char* DISCO_PAT_NAME[DISCO_NPAT] = {
  "TL-BR","TR-BL","L-R","R-L","B-T","T-B","OUT","IN","CHECKER","PULSE","SPARK"
};
#define DISCO_TOP          48       // 上端の情報パネル高さ（ここにはタイルを描かない）
#define DISCO_SWITCH_MIN   20000    // 自動切替の最短間隔(ms)
#define DISCO_SWITCH_MAX   30000    // 自動切替の最長間隔(ms)
static const float DISCO_TRAVEL_BASE = 0.05f;  // 無音時でも進む速度（ゆっくり呼吸）
static const float DISCO_TRAVEL_K    = 0.42f;  // 音量→波の速度
static const float DISCO_HUE_BASE    = 0.015f; // 色回転の最低速度
static const float DISCO_HUE_K       = 0.10f;  // 音量→色回転速度

static bool     discoReady = false;
static uint16_t discoPal[DISCO_NPAL][8];         // 565化パレット
static uint16_t discoPrev[DISCO_TILES];          // 前回描画色（差分描画用）
static bool     discoFaceTile[DISCO_TILES];      // 顔（目・鼻・口）が乗るタイル
static float    discoArg[DISCO_NPAT][DISCO_TILES]; // パターン別・タイル別の波の位相ベース
static float    discoTravel  = 0.0f;             // 明るさの波の位置
static float    discoHue     = 0.0f;             // 色回転の位置
static uint8_t  discoPat     = 0;                // 現在のパターン
static unsigned long discoPatMs = 0;             // 次にパターンを切り替える時刻
static uint32_t discoPatRng  = 0x9E3779B9u;      // パターン抽選用
static uint8_t  discoPalSet  = 0;
static uint8_t  discoPalOff  = 0;
static float    discoEnergy  = 0.0f;
static float    discoAgcPeak = 0.0f;
static float    discoBassAvg = 0.0f;
static float    discoFlash   = 0.0f;
static unsigned long discoBeatCd = 0;
static uint32_t discoRng = 0x1234ABCD;

// 顔パーツ（目・鼻・口）の外接矩形。タイルと交差すれば“顔タイル”＝スポットライト。
static bool discoRectHitsTile(int rx, int ry, int rw, int rh, int tx, int ty) {
  return !(rx >= tx + DISCO_TW || rx + rw <= tx || ry >= ty + DISCO_TH || ry + rh <= ty);
}

void buildDiscoTable() {
  for (int p = 0; p < DISCO_NPAL; p++) {
    for (int i = 0; i < 8; i++) {
      uint8_t r = DISCO_PAL_RGB[p][i][0], g = DISCO_PAL_RGB[p][i][1], b = DISCO_PAL_RGB[p][i][2];
      discoPal[p][i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
  }
  // 顔パーツの外接矩形（drawVisualizerFaceParts と同じ座標）
  //   左目(90,90)r20 / 右目(230,90)r20 / 鼻(160,145)18x12 / 口(y153〜185)
  for (int r = 0; r < DISCO_ROWS; r++) {
    for (int c = 0; c < DISCO_COLS; c++) {
      int tx = c * DISCO_TW, ty = r * DISCO_TH;
      bool hit =
        discoRectHitsTile( 66,  66, 48, 48, tx, ty) ||   // 左目
        discoRectHitsTile(206,  66, 48, 48, tx, ty) ||   // 右目
        discoRectHitsTile(138, 131, 44, 28, tx, ty) ||   // 鼻
        discoRectHitsTile(132, 150, 56, 40, tx, ty);     // 口
      discoFaceTile[r * DISCO_COLS + c] = hit;
    }
  }
  // パターンごとの波の位相ベースを事前計算（cosの引数）。
  //   明るさ = 0.5 + 0.5*cos(discoArg[pat][idx] - discoTravel)
  //   → discoTravel を増やすと明部がそのパターンの方向へ流れる。
  // 注: TWO_PI / PI は Arduino.h のマクロと衝突するため、独自名の定数を使う。
  const float DISCO_2PI = 6.2831853f;
  const float CYC       = 1.5f;              // フロアを横切る波の本数（方向パターン用）
  const float cx = (DISCO_COLS - 1) * 0.5f;  // 3.5
  const float cy = (DISCO_ROWS - 1) * 0.5f;  // 2.5
  const float maxd = sqrtf(cx * cx + cy * cy);
  for (int r = 0; r < DISCO_ROWS; r++) {
    for (int c = 0; c < DISCO_COLS; c++) {
      int idx = r * DISCO_COLS + c;
      float dist = sqrtf((c - cx) * (c - cx) + (r - cy) * (r - cy));
      float q[8];
      q[DISCO_PAT_TLBR] = (float)(c + r) / (float)((DISCO_COLS - 1) + (DISCO_ROWS - 1));
      q[DISCO_PAT_TRBL] = (float)((DISCO_COLS - 1 - c) + r) / (float)((DISCO_COLS - 1) + (DISCO_ROWS - 1));
      q[DISCO_PAT_LR]   = (float)c / (float)(DISCO_COLS - 1);
      q[DISCO_PAT_RL]   = (float)(DISCO_COLS - 1 - c) / (float)(DISCO_COLS - 1);
      q[DISCO_PAT_BT]   = (float)(DISCO_ROWS - 1 - r) / (float)(DISCO_ROWS - 1);
      q[DISCO_PAT_TB]   = (float)r / (float)(DISCO_ROWS - 1);
      q[DISCO_PAT_OUT]  = dist / maxd;
      q[DISCO_PAT_IN]   = 1.0f - dist / maxd;
      for (int p = 0; p < 8; p++) discoArg[p][idx] = q[p] * DISCO_2PI * CYC;
      discoArg[DISCO_PAT_CHECKER][idx] = (float)((c + r) & 1) * 3.14159265f; // 位相0/π＝交互
      discoArg[DISCO_PAT_PULSE][idx]   = 0.0f;                                // 全タイル同位相
      discoArg[DISCO_PAT_SPARK][idx]   = 0.0f;                                // 未使用（特殊処理）
    }
  }
  for (int i = 0; i < DISCO_TILES; i++) discoPrev[i] = 0xF81F;  // 無効値（初回強制描画）
  discoReady = true;
}

static inline uint32_t discoRand() { discoRng = discoRng * 1664525u + 1013904223u; return discoRng; }

void lightRenderDisco(bool needsInit, bool fullRepaint) {
  if (!discoReady) buildDiscoTable();
  unsigned long now = millis();

  if (needsInit) {
    discoTravel = 0; discoHue = 0; discoPalOff = 0; discoFlash = 0;
    discoEnergy = 0; discoAgcPeak = 0; discoBassAvg = 0;
    if (discoPatMs == 0) { discoPat = DISCO_PAT_TLBR; discoPatMs = now + DISCO_SWITCH_MIN; }
    for (int i = 0; i < DISCO_TILES; i++) discoPrev[i] = 0xF81F;
    fullRepaint = true;
  }

  // ── 全体レベル → AGC（小音量でもフロアが大きく踊る）──
  float lvl = gViz.level;
  if (lvl > discoAgcPeak) discoAgcPeak = lvl;
  else discoAgcPeak += (lvl - discoAgcPeak) * 0.03f;
  float ref = (discoAgcPeak < 0.14f) ? 0.14f : discoAgcPeak;
  float e = lvl / ref; if (e > 1.0f) e = 1.0f; if (e < 0.0f) e = 0.0f;
  discoEnergy += (e - discoEnergy) * 0.50f;

  // ── 高域の量 → ランダムなきらめきの強さ（照明としての演出）──
  float treble = 0.0f;
  {
    uint8_t n = gViz.bandCount; if (n == 0) n = 1;
    uint8_t s = (uint8_t)(n * 3 / 4); if (s >= n) s = (uint8_t)(n - 1);
    int cnt = 0;
    for (uint8_t i = s; i < n; i++) { treble += gViz.band[i]; cnt++; }
    if (cnt) treble /= (float)cnt;
  }

  // ── 低音の立ち上がり → 簡易ビート検出（フラッシュとパレット送りのアクセント）──
  float bass = gViz.bass;
  discoBassAvg += (bass - discoBassAvg) * 0.15f;
  if (bass > discoBassAvg * 1.35f + 0.05f && now >= discoBeatCd) {
    discoFlash = 1.0f;
    discoPalOff++;
    if ((discoPalOff & 7) == 0) discoPalSet = (discoPalSet + 1) % DISCO_NPAL;
    discoBeatCd = now + 170;
  }
  discoFlash *= 0.70f;

  // ── パターン自動切替（20〜30秒・疑似ランダム・同じ物が連続しない）──
  if (now >= discoPatMs) {
    uint8_t nx;
    do {
      discoPatRng = discoPatRng * 1664525u + 1013904223u;
      nx = (uint8_t)((discoPatRng >> 24) % DISCO_NPAT);
    } while (nx == discoPat);
    discoPat = nx;
    discoPatRng = discoPatRng * 1664525u + 1013904223u;
    discoPatMs = now + DISCO_SWITCH_MIN + (discoPatRng >> 18) % (DISCO_SWITCH_MAX - DISCO_SWITCH_MIN);
    if (discoFlash < 0.85f) discoFlash = 0.85f;                 // 切替の一瞬フラッシュ（トランジション）
    discoPalSet = (discoPalSet + 1) % DISCO_NPAL;               // 色も切り替える
    fullRepaint = true;                                        // 残像なく切り替える
    addLog(String("DISCO PATTERN: ") + String(DISCO_PAT_NAME[discoPat]));
  }

  // ── 波と色の前進（無音時も DISCO_TRAVEL_BASE で進み続ける＝待機演出）──
  discoTravel += DISCO_TRAVEL_BASE + discoEnergy * DISCO_TRAVEL_K;
  discoHue    += DISCO_HUE_BASE + discoEnergy * DISCO_HUE_K;
  int hueI = (int)discoHue;
  const uint16_t* pal = discoPal[discoPalSet];
  int   flashT  = (int)(discoFlash * 150.0f);       // 白へ寄せる量
  int   sparkTh = 3 + (int)(treble * 45.0f);        // 高域が多いほど、きらめき増
  bool  spark   = (discoPat == DISCO_PAT_SPARK);
  const float* arg = discoArg[discoPat];

  for (int r = 0; r < DISCO_ROWS; r++) {
    int ty  = r * DISCO_TH;
    int dy0 = (ty < DISCO_TOP) ? DISCO_TOP : ty;    // 上端情報パネルにはタイルを描かない
    int dh  = ty + DISCO_TH - dy0;
    if (dh <= 0) continue;                           // 完全にパネル内の行はスキップ
    for (int c = 0; c < DISCO_COLS; c++) {
      int idx = r * DISCO_COLS + c;
      int hue = (c + r + hueI + discoPalOff) & 7;
      uint16_t base = pal[hue];

      // v1.3: 「暗いクラブ」→「LEDステージ」へ。明度フロアと係数を引き上げ、
      // 波の谷でも暗く沈まないようにする（高彩度・高明度）。全体の明るさは
      // Framework共通Brightness（lightFillRect）で別途一括調整する。
      // ── v1.4 Toy-box 描画モデル ─────────────────────────
      // 【方針転換】従来は「明るさの波」で動きを表現していたが、原色を暗くすると
      //   黄→オリーブ / 橙→茶 に濁ってしまう。そこで
      //     ・タイルは常に高明度(220〜255)の原色を保つ（濁らせない）
      //     ・動きは「白い光沢(shine)が波状に流れる」ことで表現する
      //   ＝ LEDウォール／おもちゃ箱のように、鮮やかな原色の上を光が走る見え方。
      int b, shine;
      if (spark) {
        b = (int)(190.0f + discoEnergy * 45.0f);
        shine = 0;
        if ((discoRand() & 0xFF) < (uint32_t)(40 + (int)(treble * 80.0f))) { b = 255; shine = 120; }
      } else {
        float m = 0.5f + 0.5f * cosf(arg[idx] - discoTravel);     // 波 0..1
        b = (int)(220.0f + discoEnergy * 35.0f);                  // 220〜255：常に鮮やか
        shine = (int)(m * m * (60.0f + discoEnergy * 70.0f));     // 波の頂＝白い光沢が流れる
        if ((discoRand() & 0xFF) < (uint32_t)sparkTh) { b = 255; shine = 130; }  // 高域のきらめき
      }

      bool faceTile = discoFaceTile[idx];
      if (faceTile && b < 235) b = 235;               // 顔スポットライト（さらに明るく）
      if (b > 255) b = 255;

      uint16_t col = light565Scale(base, b);          // 高明度の原色
      if (shine  > 0) col = light565Lerp(col, 0xFFFF, shine);   // 白い光沢（動き）
      if (flashT > 0) col = light565Lerp(col, 0xFFFF, flashT);  // ビートフラッシュ
      if (faceTile)   col = light565Lerp(col, 0xFFFF, 55);      // 顔タイルはやや白へ

      // 顔タイルは毎フレーム塗り直し（口パクの旧形状を残さない）
      // lightFillRect で Framework共通Brightnessが自動適用される。
      // 差分判定は Brightness適用前の col で行う（明るさ変更時は全面initで反映）。
      if (fullRepaint || faceTile || col != discoPrev[idx]) {
        lightFillRect(c * DISCO_TW, dy0, DISCO_TW, dh, col);
        discoPrev[idx] = col;
      }
    }
  }
}

// ============================================================================
// Lighting #2 : Laser Show（第二弾 / v1.2）
//
// ■ コンセプト
//   暗い会場を横切るレーザービーム。四隅からの交差・扇状走査・左右すれ違い・
//   ビートで現れる大きなX字などが、音に合わせて動き交差する。
//
// ■ レイヤー
//   Disco Floor（背面）→ Laser Show（中間）→ Visualizer → 顔 → 上端パネル
//   ・Laser単体のときは自前で暗い背景(y>=48)を敷く＝会場の暗さ
//   ・Disco併用のときは Disco が背景を塗るので、その上へビームを重ねる
//   ・上端48pxの情報パネルには描かない（LASER_YMIN=51 で構造的に回避）
//
// ■ 残像なしの秘訣（合成）
//   Laserが有効な合成フレームは fullRepaint を強制する（コンポジタ側）。
//   → Disco/暗背景が毎フレーム全面を塗り直すので前フレームのビームは自動で消える。
//   その上で「1フレーム前のビームを淡く」描いて、意図した短い残像だけを残す。
//
// ■ 疑似グロー（CoreS3は半透明が使いにくい）
//   1本のビームを平行にずらして重ね描き：
//     外側(±2px)=暗い同系色 / 中間(±1px)=明るい同系色 / 中心(0px)=白or淡色
//   合計視認幅 約5px。細い針にはしない。
//
// ■ 音との連動（測定器ではない）
//   ・全体音量(AGC) → 本数・明るさ・走査速度
//   ・低音の立ち上がり → X Burst / 全ビーム一瞬増光
//   ・高音 → 細いフリッカービーム追加・瞬き
//   ・無音 → ゆっくり走査する待機演出
//
// ■ 負荷対策
//   ・drawLine ベース（fillCircle連打の drawThickLine は使わない）
//   ・本数上限 LASER_MAX、重い組合せでは本数を自動削減（コンポジタから指示）
//   ・sin/cos はビーム数ぶんだけ／フレーム。動的確保なし
// ============================================================================
enum LaserPattern : uint8_t {
  LASER_CROSS = 0,   // 1 Corner Cross：四隅→中央へ交差
  LASER_FAN,         // 2 Fan Sweep：一点から扇状・角度往復
  LASER_DUAL,        // 3 Dual Scan：左右から中央へすれ違い
  LASER_XBURST,      // 4 X Burst：ビートで大きなX字が一瞬
  LASER_RANDOM,      // 5 Random Club：発射位置・角度・色・本数が変化
  LASER_NPAT
};
static const char* LASER_PAT_NAME[LASER_NPAT] = { "CROSS","FAN","DUAL","XBURST","RANDOM" };

#define LASER_MAX      14      // 同時ビーム数の上限
#define LASER_YMIN     51      // 上端パネル(48px)を侵さない下限Y（±グロー込みで48以上）
#define LASER_YMAX    236
#define LASER_XMIN      3
#define LASER_XMAX    316
#define LASER_SWITCH_MIN 18000 // 自動切替の最短(ms)。Discoの20-30sとは別値で同期を避ける
#define LASER_SWITCH_MAX 27000

// レーザー色：第一弾は【蛍光グリーン1色限定】。
// クラシックなライブ/ディスコの緑レーザー(532nm)のイメージ。
// Disco Floorが十分カラフルなので、レーザーは単色にすることで
// 逆に“レーザーらしさ”を際立たせる意図。
// 疑似グローは 外側=暗い緑 / 中間=明るい緑 / 中心=白 の重ね描きで表現するため、
// 単色でも「白く光る芯＋緑の後光」で十分レーザーらしく見える。
// （将来ここを配列に戻せば多色レーザーへ拡張可能）
static const uint16_t LASER_GREEN = 0x07E0;   // 蛍光グリーン

struct LaserBeam { float x0, y0, x1, y1; uint16_t hue; uint8_t bright; };
static LaserBeam laserBeam[LASER_MAX];
static LaserBeam laserPrev[LASER_MAX];   // 直前フレーム（淡い残像用）
static uint8_t   laserCount = 0, laserPrevCount = 0;

static uint8_t  laserPat = 0;
static unsigned long laserPatMs = 0;
static float    laserPhase = 0.0f;
static float    laserEnergy = 0.0f;
static float    laserAgcPeak = 0.0f;
static float    laserBassAvg = 0.0f;
static float    laserFlash = 0.0f;       // X Burst / 増光
static unsigned long laserBeatCd = 0;
static uint32_t laserRng = 0xB5297A4Du;
static unsigned long laserRandMs = 0;    // Random Club の内部再抽選タイマ
static int      laserMaxBeams = LASER_MAX;  // コンポジタからの本数上限（重い組合せで削減）

static inline uint32_t laserRand() { laserRng = laserRng * 1664525u + 1013904223u; return laserRng; }
static inline float laserFrand() { return (float)(laserRand() >> 8) / 16777216.0f; }  // 0..1

// 線分を情報パネル下の矩形 [XMIN..XMAX]×[YMIN..YMAX] へクリップ（Liang-Barsky）。
// クリップ結果が空なら false。
static bool laserClip(float& x0, float& y0, float& x1, float& y1) {
  float dx = x1 - x0, dy = y1 - y0;
  float t0 = 0.0f, t1 = 1.0f;
  float p[4] = { -dx, dx, -dy, dy };
  float q[4] = { x0 - LASER_XMIN, LASER_XMAX - x0, y0 - LASER_YMIN, LASER_YMAX - y0 };
  for (int i = 0; i < 4; i++) {
    if (fabsf(p[i]) < 1e-6f) { if (q[i] < 0) return false; }
    else {
      float r = q[i] / p[i];
      if (p[i] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
      else          { if (r < t0) return false; if (r < t1) t1 = r; }
    }
  }
  float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
  float nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
  x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
  return true;
}

// クランプ付きの直線描画（上端パネル y<48 と画面外を絶対に侵さない）
//
// 【重要・v1.3.1】レーザーは Framework共通Brightness の影響を受けない。
//   ご要望「レーザーは常に鮮やか」に合わせ、あえて lightDrawLine ではなく
//   GFX.drawLine（＝生の色）で描く。
//   ・Brightnessを20%等に下げると Disco Floor は暗くなるが、緑レーザーは鮮やかなまま
//     → 暗い床＋鮮烈な緑ビームで、より“ライブ会場のレーザー”らしくなる。
//   ・レーザー自身の明るさは baseBright / laserFlash（音量・ビート連動）で決まる。
//   将来 RGB版 / Neon版 を追加する場合も、同じく生の色で描けば独立を保てる。
static inline void laserDrawSeg(int x0, int y0, int x1, int y1, uint16_t color) {
  if (x0 < 0) x0 = 0; if (x0 > 319) x0 = 319;
  if (x1 < 0) x1 = 0; if (x1 > 319) x1 = 319;
  if (y0 < 48) y0 = 48; if (y0 > 239) y0 = 239;
  if (y1 < 48) y1 = 48; if (y1 > 239) y1 = 239;
  GFX.drawLine(x0, y0, x1, y1, color);   // 共通Brightnessを通さない（常に鮮やか）
}

// 疑似グロー付きでビーム1本を描く（bright:0..255・全体の明るさ）
static void laserDrawBeam(float fx0, float fy0, float fx1, float fy1, uint16_t hue, int bright, bool withCore) {
  if (!laserClip(fx0, fy0, fx1, fy1)) return;
  int x0 = (int)fx0, y0 = (int)fy0, x1 = (int)fx1, y1 = (int)fy1;
  float len = sqrtf((float)((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)));
  if (len < 1.0f) return;
  float nx = -(y1 - y0) / len, ny = (x1 - x0) / len;   // 単位法線
  uint16_t cOut = light565Scale(hue, bright * 28 / 100);
  uint16_t cMid = light565Scale(hue, bright * 70 / 100);
  uint16_t cCore = (bright > 200) ? WHITE : light565Scale(hue, bright);
  // グロー用ラインを引くヘルパ。オフセット後の座標を必ず画面内かつ上端パネル外
  // （y>=LASER_YMIN-2）へクランプし、情報パネル(y<48)を絶対に侵さない。
  // 外側グロー（±2px）
  for (int s = -2; s <= 2; s += 4) {
    int ox = (int)(nx * s), oy = (int)(ny * s);
    laserDrawSeg(x0 + ox, y0 + oy, x1 + ox, y1 + oy, cOut);
  }
  // 中間（±1px）
  for (int s = -1; s <= 1; s += 2) {
    int ox = (int)(nx * s), oy = (int)(ny * s);
    laserDrawSeg(x0 + ox, y0 + oy, x1 + ox, y1 + oy, cMid);
  }
  // 中心（白）
  if (withCore) laserDrawSeg(x0, y0, x1, y1, cCore);
}

// 原点(ox,oy)から角度aで十分に伸ばした線分をビーム配列へ積む
static void laserPush(float ox, float oy, float a, uint16_t hue, int bright) {
  if (laserCount >= (uint8_t)laserMaxBeams) return;
  float ex = ox + cosf(a) * 520.0f;
  float ey = oy + sinf(a) * 520.0f;
  LaserBeam& b = laserBeam[laserCount++];
  b.x0 = ox; b.y0 = oy; b.x1 = ex; b.y1 = ey; b.hue = hue; b.bright = (uint8_t)bright;
}

void lightRenderLaser(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  unsigned long now = millis();

  if (needsInit) {
    laserPhase = 0; laserFlash = 0; laserEnergy = 0; laserAgcPeak = 0; laserBassAvg = 0;
    laserPrevCount = 0; laserCount = 0;
    if (laserPatMs == 0) { laserPat = LASER_CROSS; laserPatMs = now + LASER_SWITCH_MIN; }
  }

  // ── AGC（小音量でも派手に）──
  float lvl = gViz.level;
  if (lvl > laserAgcPeak) laserAgcPeak = lvl;
  else laserAgcPeak += (lvl - laserAgcPeak) * 0.03f;
  float ref = (laserAgcPeak < 0.14f) ? 0.14f : laserAgcPeak;
  float e = lvl / ref; if (e > 1.0f) e = 1.0f; if (e < 0.0f) e = 0.0f;
  laserEnergy += (e - laserEnergy) * 0.5f;

  // ── 高音 ──
  float treble = 0.0f;
  { uint8_t n = gViz.bandCount; if (n == 0) n = 1;
    uint8_t s = (uint8_t)(n * 3 / 4); if (s >= n) s = (uint8_t)(n - 1);
    int cnt = 0; for (uint8_t i = s; i < n; i++) { treble += gViz.band[i]; cnt++; }
    if (cnt) treble /= (float)cnt; }

  // ── ビート（X Burst / 増光のきっかけ）──
  float bass = gViz.bass;
  laserBassAvg += (bass - laserBassAvg) * 0.15f;
  if (bass > laserBassAvg * 1.35f + 0.05f && now >= laserBeatCd) {
    laserFlash = 1.0f; laserBeatCd = now + 160;   // ビート → X Burst / 全ビーム増光
  }
  laserFlash *= 0.78f;

  // ── パターン自動切替（Discoと別タイミング）──
  if (now >= laserPatMs) {
    uint8_t nx; do { laserRng = laserRng * 1664525u + 1013904223u; nx = (uint8_t)((laserRng >> 25) % LASER_NPAT); } while (nx == laserPat);
    laserPat = nx;
    laserRng = laserRng * 1664525u + 1013904223u;
    laserPatMs = now + LASER_SWITCH_MIN + (laserRng >> 18) % (LASER_SWITCH_MAX - LASER_SWITCH_MIN);
    laserRandMs = 0;
    addLog(String("LASER PATTERN: ") + String(LASER_PAT_NAME[laserPat]));
  }

  // ── 走査位相の前進（無音でも進む＝待機演出）──
  laserPhase += 0.03f + laserEnergy * 0.11f;

  // ── 背景：Disco非併用時のみ、暗い会場をy>=48へ敷く（前フレームのビーム消去も兼ねる）──
  // 背景(面)がこのフレームで塗られていなければ、Laserが自前で暗い会場を敷く。
  if (!gLightBgFilled) {
    GFX.fillRect(0, 48, 320, 240 - 48, 0x0000);
  }

  // ── 直前フレームのビームを淡い残像として先に描く（意図した短い残像）──
  for (uint8_t i = 0; i < laserPrevCount; i++) {
    LaserBeam& b = laserPrev[i];
    laserDrawBeam(b.x0, b.y0, b.x1, b.y1, b.hue, b.bright * 30 / 100, false);
  }

  // ── 現在フレームのビームを構築 ──
  laserCount = 0;
  int baseBright = 150 + (int)(laserEnergy * 90.0f);
  if (baseBright > 245) baseBright = 245;
  uint16_t col = LASER_GREEN;
  float cx = 160.0f, cyC = 144.0f;

  switch (laserPat) {
    case LASER_CROSS: {
      // 四隅→中央（＋わずかに揺れる）。中央で交差
      const float ox[4] = { LASER_XMIN, LASER_XMAX, LASER_XMIN, LASER_XMAX };
      const float oy[4] = { LASER_YMIN, LASER_YMIN, LASER_YMAX, LASER_YMAX };
      float wob = sinf(laserPhase) * 0.10f;
      for (int k = 0; k < 4; k++) {
        float a = atan2f(cyC - oy[k], cx - ox[k]) + wob * ((k & 1) ? 1 : -1);
        laserPush(ox[k], oy[k], a, col, baseBright);
      }
      break;
    }
    case LASER_FAN: {
      // 下端中央の一点から扇状。中心角が左右へ往復
      float base = -1.5708f + sinf(laserPhase * 0.8f) * 0.55f;   // 上向き±
      int n = 5 + (int)(laserEnergy * 3.0f);
      for (int k = 0; k < n; k++) {
        float a = base + (k - (n - 1) * 0.5f) * 0.22f;
        laserPush(cx, LASER_YMAX, a, col, baseBright);
      }
      break;
    }
    case LASER_DUAL: {
      // 左右の縁から複数、中央ですれ違い（yが位相でスクロール）
      int n = 3;
      for (int k = 0; k < n; k++) {
        float ph = laserPhase + k * 2.1f;
        float lY = LASER_YMIN + (0.5f + 0.5f * sinf(ph)) * (LASER_YMAX - LASER_YMIN);
        float rY = LASER_YMIN + (0.5f + 0.5f * sinf(ph + 3.14159f)) * (LASER_YMAX - LASER_YMIN);
        laserPush(LASER_XMIN, lY, atan2f(cyC - lY, cx - LASER_XMIN) * 0.6f, col, baseBright);
        laserPush(LASER_XMAX, rY, 3.14159f - atan2f(cyC - rY, cx - LASER_XMIN) * 0.6f, LASER_GREEN, baseBright);
      }
      break;
    }
    case LASER_XBURST: {
      // 通常は薄い2本のX。ビートで一気に増光＋太く（laserFlashで表現）
      int br = 70 + (int)(laserFlash * 185.0f);
      laserPush(LASER_XMIN, LASER_YMIN, atan2f(LASER_YMAX - LASER_YMIN, LASER_XMAX - LASER_XMIN), col, br);
      laserPush(LASER_XMAX, LASER_YMIN, atan2f(LASER_YMAX - LASER_YMIN, LASER_XMIN - LASER_XMAX), col, br);
      if (laserFlash > 0.5f) {   // 強ビート時は横断ビームも一瞬
        laserPush(LASER_XMIN, cyC, 0.0f, LASER_GREEN, br);
        laserPush(cx, LASER_YMIN, 1.5708f, LASER_GREEN, br);
      }
      break;
    }
    default: {  // LASER_RANDOM
      // 一定時間ごとに本数・原点・角度・色を再抽選。間はゆっくりドリフト
      if (now >= laserRandMs) {
        laserRandMs = now + 1600 + (laserRand() % 1400);
      }
      int n = 4 + (int)(laserEnergy * 4.0f);
      for (int k = 0; k < n; k++) {
        // 疑似ランダムだが k と時間帯で安定（毎フレーム同じ配置＋ゆっくり回転）
        float seed = (float)((laserRandMs >> 5) + k * 97);
        float ox = LASER_XMIN + fmodf(seed * 0.61803f, 1.0f) * (LASER_XMAX - LASER_XMIN);
        float oy = LASER_YMIN + fmodf(seed * 0.31831f, 1.0f) * (LASER_YMAX - LASER_YMIN);
        float a  = seed + laserPhase * 0.5f;
        laserPush(ox, oy, a, LASER_GREEN, baseBright);
      }
      break;
    }
  }

  // ── 高音で細いフリッカービームを1本追加 ──
  if (treble > 0.35f && laserCount < (uint8_t)laserMaxBeams) {
    float a = laserFrand() * 6.2831853f;
    laserPush(cx, cyC, a, LASER_GREEN, 120 + (int)(treble * 120.0f));
  }

  // ── ビート時は全ビーム増光 ──
  int flashAdd = (int)(laserFlash * 90.0f);

  // ── 描画（中心コアあり）──
  for (uint8_t i = 0; i < laserCount; i++) {
    LaserBeam& b = laserBeam[i];
    int br = b.bright + flashAdd; if (br > 255) br = 255;
    laserDrawBeam(b.x0, b.y0, b.x1, b.y1, b.hue, br, true);
  }

  // ── 残像用に今フレームを保存 ──
  laserPrevCount = laserCount;
  for (uint8_t i = 0; i < laserCount; i++) laserPrev[i] = laserBeam[i];
}

// ============================================================================
// Lighting #3 : Aurora（第四弾 / v1.5）
//
// ■ コンセプト
//   画面全体に、やわらかいオーロラの光幕。青緑〜シアン〜紫ピンクの
//   カーテンが横に波打ちながらゆっくり流れる。Disco の派手さと対照的な
//   “癒し系”。無音時でもゆっくり漂う待機演出になる。
//
// ■ レイヤー種別＝【背景(面)】
//   Disco と同じ「面」演出。Laser（ビーム）はこの上に重なる（Aurora + Laser 可）。
//   複数の背景（Disco と Aurora 両方ON）が選ばれた場合は、最後の1つだけ表示する
//   （コンポジタが背景を1つに集約）。
//
// ■ 共通ルール遵守
//   ・背景の描画は lightFillRect を使う → Framework共通Brightnessが自動で効く
//   ・上端48px（情報パネル）には描かない（AURORA_TOP=48でクリップ）
//   ・顔はコンポジタが最前面に描く（Aurora は顔の背面）
//   ・動的メモリ確保なし。sinf は列数ぶん/フレームのみ
// ============================================================================
#define AURORA_NCOL   32                 // 縦カーテンの列数
#define AURORA_CW     (320 / AURORA_NCOL) // 1列の幅(=10px)
#define AURORA_TOP    48                  // 情報パネル下限
#define AURORA_NPAL   6

// オーロラ色（鮮やかな青緑〜シアン〜紫ピンク）
static const uint8_t AURORA_PAL_RGB[AURORA_NPAL][3] = {
  {0,255,120},{80,255,170},{0,230,255},{0,200,210},{150,90,255},{255,90,210}
};
static const uint16_t AURORA_SKY = (uint16_t)(((0 & 0xF8) << 8) | ((14 & 0xFC) << 3) | (44 >> 3)); // 深い夜空

static bool     auroraReady = false;
static uint16_t auroraPal[AURORA_NPAL];
static float    auroraColBase[AURORA_NCOL];  // 列ごとの位相ベース
static float    auroraT = 0.0f;
static float    auroraEnergy = 0.0f;
static float    auroraAgcPeak = 0.0f;
static float    auroraBassAvg = 0.0f;
static float    auroraFlash = 0.0f;
static unsigned long auroraBeatCd = 0;

void buildAuroraTable() {
  for (int i = 0; i < AURORA_NPAL; i++) {
    uint8_t r = AURORA_PAL_RGB[i][0], g = AURORA_PAL_RGB[i][1], b = AURORA_PAL_RGB[i][2];
    auroraPal[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  for (int c = 0; c < AURORA_NCOL; c++) auroraColBase[c] = (float)c * 0.55f;
  auroraReady = true;
}

// 1列を、中心yc・半高hのやわらかい縦グラデーション（三角プロファイル）で描く。
static void auroraColumn(int cx, float yc, float h, uint16_t col, int inten) {
  const int NSEG = 5;
  float segH = (2.0f * h) / (float)NSEG;
  if (segH < 1.0f) segH = 1.0f;
  for (int s = 0; s < NSEG; s++) {
    int y0 = (int)(yc - h + segH * s);
    int ht = (int)segH + 1;
    if (y0 < AURORA_TOP) { ht -= (AURORA_TOP - y0); y0 = AURORA_TOP; }
    if (y0 > 239 || ht <= 0) continue;
    if (y0 + ht > 240) ht = 240 - y0;
    float prof = 1.0f - fabsf((float)s - (NSEG - 1) * 0.5f) / ((NSEG - 1) * 0.5f); // 0..1三角
    int bb = (int)(inten * (0.22f + 0.78f * prof));
    if (bb > 255) bb = 255;
    lightFillRect(cx, y0, AURORA_CW, ht, light565Scale(col, bb));
  }
}

void lightRenderAurora(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!auroraReady) buildAuroraTable();
  unsigned long now = millis();
  if (needsInit) { auroraT = 0; auroraEnergy = 0; auroraAgcPeak = 0; auroraBassAvg = 0; auroraFlash = 0; }

  // AGC（小音量でもやわらかく動く）
  float lvl = gViz.level;
  if (lvl > auroraAgcPeak) auroraAgcPeak = lvl;
  else auroraAgcPeak += (lvl - auroraAgcPeak) * 0.03f;
  float ref = (auroraAgcPeak < 0.14f) ? 0.14f : auroraAgcPeak;
  float e = lvl / ref; if (e > 1.0f) e = 1.0f; if (e < 0.0f) e = 0.0f;
  auroraEnergy += (e - auroraEnergy) * 0.35f;   // ゆっくり追従＝癒し系

  // 低音でふわっと増光
  float bass = gViz.bass;
  auroraBassAvg += (bass - auroraBassAvg) * 0.15f;
  if (bass > auroraBassAvg * 1.4f + 0.05f && now >= auroraBeatCd) { auroraFlash = 1.0f; auroraBeatCd = now + 220; }
  auroraFlash *= 0.86f;

  // 位相前進（無音でも進む＝待機演出）
  auroraT += 0.020f + auroraEnergy * 0.060f;

  // 夜空を1枚で塗る（前フレームのカーテン消去も兼ねる）
  lightFillRect(0, AURORA_TOP, 320, 240 - AURORA_TOP, AURORA_SKY);

  int baseInten = 150 + (int)(auroraEnergy * 80.0f) + (int)(auroraFlash * 60.0f);
  if (baseInten > 255) baseInten = 255;
  int hueShift = (int)(auroraT * 0.7f);

  // カーテン2層（奥＝ゆっくり広め、手前＝速く細め）でオーロラの奥行きを出す
  for (int layer = 0; layer < 2; layer++) {
    float freq  = (layer == 0) ? 0.9f : 1.6f;
    float spd   = (layer == 0) ? 1.0f : 1.7f;
    float amp   = (layer == 0) ? 34.0f : 22.0f;
    float cen   = (layer == 0) ? 130.0f : 108.0f;
    float hbase = (layer == 0) ? 30.0f : 22.0f;
    int   dim   = (layer == 0) ? 0 : 30;   // 手前層は少し淡く
    for (int c = 0; c < AURORA_NCOL; c++) {
      float ph = auroraColBase[c] * freq + auroraT * spd + layer * 2.3f;
      float yc = cen + amp * sinf(ph);
      float h  = hbase * (0.6f + 0.4f * sinf(ph * 1.7f + c));
      uint16_t col = auroraPal[(c + hueShift + layer * 2) % AURORA_NPAL];
      int inten = baseInten - dim;
      if (inten < 40) inten = 40;
      auroraColumn(c * AURORA_CW, yc, h, col, inten);
    }
  }
}

// ============================================================================
// Lighting #4 : Matrix（v1.7）
//
// ■ コンセプト（映画の再現ではなく“未来感・サイバー・かっこよさ”）
//   黒背景に蛍光グリーンの太い縦カラムが落下。細かい文字ではなく、
//   320×240でも存在感のある大きな発光セルで構成する。
//   先頭セルは白緑で明るく、後方へグリーンが減衰する。
//   低音ビートで全体が一瞬加速し、シアンの横スキャンラインが上から一閃する。
//
// ■ レイヤー＝背景(面) / HEADER_DARK
//   Laser（緑ビーム・Brightness非依存）を重ねると、Matrixの雨をレーザーが
//   切り裂く未来的演出になる（Background→Overlay→Face の順は既存のまま）。
//
// ■ 共通ルール
//   ・背景は lightFillRect で描く → Framework共通Brightnessが自動で効く
//   ・上端48px（情報パネル）には描かない（MATRIX_TOP=48でクリップ）
//   ・顔はコンポジタが最前面に描く
//   ・無音でもゆっくり落下し続ける（待機演出）。動的メモリ確保なし
// ============================================================================
#define MATRIX_TOP    48
#define MATRIX_COLW   20                    // 太い縦カラム幅
#define MATRIX_NCOL   (320 / MATRIX_COLW)   // 16列
#define MATRIX_CELLH  22                    // 発光セルの高さ

static bool     matrixReady = false;
static float    matrixHeadY[MATRIX_NCOL];   // 各列の先頭セルのY
static uint8_t  matrixLen[MATRIX_NCOL];     // 各列の尾の長さ（セル数）
static float    matrixSpeedVar[MATRIX_NCOL];// 各列の速度ばらつき
static float    matrixEnergy = 0.0f;
static float    matrixAgcPeak = 0.0f;
static float    matrixBassAvg = 0.0f;
static float    matrixBurst = 0.0f;         // 低音での一瞬加速
static unsigned long matrixBeatCd = 0;
static uint32_t matrixRng = 0x51ED2A3Bu;
static int      matrixScanY = -1;           // シアン横スキャン線のY（-1=非表示）

static inline uint32_t matrixRand() { matrixRng = matrixRng * 1664525u + 1013904223u; return matrixRng; }

void buildMatrixTable() {
  for (int c = 0; c < MATRIX_NCOL; c++) {
    matrixHeadY[c]    = (float)MATRIX_TOP + (float)((int)(matrixRand() % 400) - 200); // 画面上下にばらけて開始
    matrixLen[c]      = (uint8_t)(4 + (matrixRand() % 6));
    matrixSpeedVar[c] = 0.7f + (float)(matrixRand() % 60) / 100.0f;
  }
  matrixReady = true;
}

void lightRenderMatrix(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!matrixReady) buildMatrixTable();
  unsigned long now = millis();
  if (needsInit) { matrixEnergy = 0; matrixAgcPeak = 0; matrixBassAvg = 0; matrixBurst = 0; matrixScanY = -1; }

  // AGC（小音量でもよく流れる）
  float lvl = gViz.level;
  if (lvl > matrixAgcPeak) matrixAgcPeak = lvl;
  else matrixAgcPeak += (lvl - matrixAgcPeak) * 0.03f;
  float ref = (matrixAgcPeak < 0.14f) ? 0.14f : matrixAgcPeak;
  float e = lvl / ref; if (e > 1.0f) e = 1.0f; if (e < 0.0f) e = 0.0f;
  matrixEnergy += (e - matrixEnergy) * 0.40f;

  // 高音 → ランダムなスパークセル
  float treble = 0.0f;
  { uint8_t n = gViz.bandCount; if (n == 0) n = 1;
    uint8_t s = (uint8_t)(n * 3 / 4); if (s >= n) s = (uint8_t)(n - 1);
    int cnt = 0; for (uint8_t i = s; i < n; i++) { treble += gViz.band[i]; cnt++; }
    if (cnt) treble /= (float)cnt; }

  // 低音の立ち上がり → 全体加速 ＋ シアン横スキャン一閃
  float bass = gViz.bass;
  matrixBassAvg += (bass - matrixBassAvg) * 0.15f;
  if (bass > matrixBassAvg * 1.35f + 0.05f && now >= matrixBeatCd) {
    matrixBurst = 1.0f; matrixBeatCd = now + 170; matrixScanY = MATRIX_TOP + 4;
  }
  matrixBurst *= 0.80f;

  // 黒背景（前フレームのセル消去も兼ねる）
  lightFillRect(0, MATRIX_TOP, 320, 240 - MATRIX_TOP, 0x0000);

  const uint16_t GREEN = 0x07E0;                  // 蛍光グリーン
  float fall = 1.2f + matrixEnergy * 4.5f + matrixBurst * 7.0f;
  int   sparkTh = 2 + (int)(treble * 40.0f);

  for (int c = 0; c < MATRIX_NCOL; c++) {
    matrixHeadY[c] += fall * matrixSpeedVar[c];
    // 尾まで完全に画面下へ抜けたら、上へ再スポーン
    if (matrixHeadY[c] - (float)matrixLen[c] * MATRIX_CELLH > 240.0f) {
      matrixHeadY[c]    = (float)MATRIX_TOP - (float)(matrixRand() % 120);
      matrixLen[c]      = (uint8_t)(4 + (matrixRand() % 6));
      matrixSpeedVar[c] = 0.7f + (float)(matrixRand() % 60) / 100.0f;
    }
    int cx = c * MATRIX_COLW;
    for (int k = 0; k < matrixLen[c]; k++) {
      float y = matrixHeadY[c] - (float)k * MATRIX_CELLH;
      int y0 = (int)y;
      int y1 = y0 + MATRIX_CELLH - 3;
      if (y1 < MATRIX_TOP || y0 > 239) continue;
      if (y0 < MATRIX_TOP) y0 = MATRIX_TOP;
      if (y1 > 239) y1 = 239;

      uint16_t col;
      if (k == 0)                 col = light565Lerp(GREEN, 0xFFFF, 150);   // 先頭＝白緑で明るい
      else {
        float b = 1.0f - (float)k * 0.15f; if (b < 0.12f) b = 0.12f;
        col = light565Scale(GREEN, (int)(b * 255.0f));
      }
      if ((matrixRand() & 0xFF) < (uint32_t)sparkTh) col = light565Lerp(GREEN, 0xFFFF, 200); // スパーク
      lightFillRect(cx + 2, y0, MATRIX_COLW - 4, y1 - y0 + 1, col);
    }
  }

  // シアン横スキャン線（ビート時のみ、上から下へ速く一閃）
  if (matrixScanY >= MATRIX_TOP) {
    lightFillRect(0, matrixScanY, 320, 3, 0x07FF);   // cyan
    matrixScanY += 28;
    if (matrixScanY > 236) matrixScanY = -1;
  }
}

// ============================================================================
// Lighting #5 : Retro Race（Pole Position風スクリーンセーバー）（v1.7）
//
// ■ コンセプト
//   1980年代のMSXやPC-6001へ無理に移植したような、カクカクした低解像度の
//   疑似3Dレースデモ。プレイヤー操作は無く、ゲームオーバーも無い。
//   自車は道路のカーブと前方車両を自動で避けながら永遠に走り続ける、
//   「眺めて楽しむスクリーンセーバー」に徹する。
//
// ■ 自動運転ロジック（厳密な物理は不要、雑駁な近似で十分）
//   ・raceCurveSmooth … 道路のカーブ方向(-1〜+1)。数秒おきにランダムな
//     目標値へゆっくり遷移する（直線区間も混ぜる）。
//   ・racePlayerX     … 自車の車線内位置(-1〜+1)。前方車両が近づくと
//     車ごとにランダムに決めた回避側（raceCarAvoidSide）へ寄り、
//     危険が無ければカーブへ軽く寄りつつゆっくり漂う。
//   ・raceCarZ[]      … 対向車の奥行き進行度(0=最遠, 1=通過)。通過後は
//     車線・色・速度・追い越し方向を再抽選し、ランダムな間隔を空けて
//     再出現する（＝毎回すこし違う展開になる）。事故・衝突判定は行わない。
//
// ■ かりポムの目線（今回の要件の核）
//   Lighting中に setEyeDirection() を呼ぶと内部で drawOpenEyes()（白背景の
//   通常顔用）が呼ばれてしまい、レース画面が白く塗りつぶれるため、ここでは
//   eyeOffsetX/eyeOffsetY を直接更新するだけに留める。実際の描画は
//   drawVisualizerFaceParts()（Layer2・本Framework共通の顔描画）側で
//   オフセットを反映するよう対応済み。カーブ方向・自車の操舵・すれ違う
//   車両へ向けて、黒目がなめらかに追従する。
//
// ■ 共通ルール遵守
//   ・背景の描画は lightFillRect を使う → Framework共通Brightnessが自動で効く
//   ・上端48px（情報パネル）には描かない（RACE_TOP=48でクリップ）
//   ・顔はコンポジタが最前面に描く
//   ・動的メモリ確保なし
// ============================================================================
#define RACE_TOP      48
#define RACE_HORIZON  104                 // 地平線のY座標
#define RACE_ROW_H    4                   // 帯の高さ（低解像度感を出す）
#define RACE_MAX_CARS 3

static bool     raceReady = false;
static uint32_t raceRng   = 0x1234ABCDu;

static float raceCurveSmooth      = 0.0f;   // 現在のカーブ方向(-1〜+1)
static float raceCurveTarget      = 0.0f;
static unsigned long raceCurveChangeAt = 0;

static float racePlayerX       = 0.0f;      // 自車の車線内位置(-1〜+1)
static float racePlayerXTarget = 0.0f;

static float raceSpeed           = 1.0f;    // 現在の走行速度(倍率)
static float raceSpeedTarget     = 1.0f;
static unsigned long raceSpeedChangeAt = 0;

static float raceScrollZ = 0.0f;            // 路面テクスチャのスクロール量

static float   raceCarZ[RACE_MAX_CARS];
static float   raceCarLane[RACE_MAX_CARS];
static float   raceCarSpeedMul[RACE_MAX_CARS];
static int8_t  raceCarAvoidSide[RACE_MAX_CARS];
static uint8_t raceCarColorIdx[RACE_MAX_CARS];

static const uint8_t RACE_CAR_RGB[4][3] = {
  {230,60,60}, {70,110,230}, {235,200,40}, {235,235,235}
};
static uint16_t raceCarCol[4];

// ── 道路脇の看板（2026-07-23追加／2026-07-23調整）──────────────
// 本家Pole Positionのように、常に1枚だけを左右どちらかへランダムに出す
// （枠を1つにすることで「左右同時」「同じ奥行きに2枚」を構造的に防止）。
// 低解像度のドット表現に留めるため、文字は「KARI/POM/GO!」の3語のみ・
// 矢印・抽象ロゴの3種類。
#define RACE_MAX_SIGNS 1
static float   raceSignZ[RACE_MAX_SIGNS];
static int8_t  raceSignSide[RACE_MAX_SIGNS];        // -1=左 / +1=右
static uint8_t raceSignType[RACE_MAX_SIGNS];        // 0=矢印 1=ロゴ 2=文字
static uint8_t raceSignColorIdx[RACE_MAX_SIGNS];
static uint8_t raceSignTextIdx[RACE_MAX_SIGNS];     // RACE_SIGN_TEXT[] のどれか
static float   raceSignScale[RACE_MAX_SIGNS];       // 大きさのばらつき
static float   raceSignSpeedMul[RACE_MAX_SIGNS];    // 奥行き方向速度のばらつき

static const char*   RACE_SIGN_TEXT[3] = { "KARI", "POM", "GO!" };
static const uint8_t  RACE_SIGN_RGB[4][3] = {
  {230,140,40}, {40,180,170}, {150,90,220}, {250,250,235}
};
static const bool RACE_SIGN_DARKTEXT[4] = { true, true, false, true };  // 板の色に対する文字/柄の色（true=黒）
static uint16_t raceSignCol[4];

static inline uint32_t raceRand()   { raceRng = raceRng * 1664525u + 1013904223u; return raceRng; }
static inline float    raceRand01() { return (float)(raceRand() & 0xFFFF) / 65535.0f; }

static void raceSpawnCar(int i, bool initialSpread) {
  raceCarZ[i]         = initialSpread ? -raceRand01() * 1.4f : -(0.25f + raceRand01() * 0.9f);
  raceCarLane[i]       = (raceRand01() * 2.0f - 1.0f) * 0.55f;
  raceCarSpeedMul[i]   = 0.85f + raceRand01() * 0.30f;
  raceCarAvoidSide[i]  = (raceRand() & 1u) ? 1 : -1;
  raceCarColorIdx[i]   = (uint8_t)(raceRand() % 4);
}

// 看板の再抽選（出現間隔・左右・種類・色・文言・大きさ・速度のすべてをランダム化）。
//   raceSignZ の負値は「まだ見えていない待機時間」。看板は毎フレーム
//   0.0017*raceSpeed*raceSignSpeedMul ずつ進む（≒0.02/秒が目安）ため、
//   -0.05〜-0.30 は実時間でおよそ数秒〜十数秒のランダムな待ちに相当する。
static void raceSpawnSign(int i, bool initialSpread) {
  raceSignZ[i]        = initialSpread ? -raceRand01() * 0.30f : -(0.05f + raceRand01() * 0.25f);
  raceSignSide[i]      = (raceRand() & 1u) ? 1 : -1;
  raceSignType[i]      = (uint8_t)(raceRand() % 3);
  raceSignColorIdx[i]  = (uint8_t)(raceRand() % 4);
  raceSignTextIdx[i]   = (uint8_t)(raceRand() % 3);
  raceSignScale[i]     = 0.85f + raceRand01() * 0.5f;
  raceSignSpeedMul[i]  = 0.85f + raceRand01() * 0.4f;
}

// 矢印の抽象グリフ（5列の帯を先細りにするだけの低解像度表現）。dir>0で右、dir<0で左を指す。
static void raceDrawArrowGlyph(int leftX, int cy, int w, int h, int dir, uint16_t col) {
  const int steps = 5;
  int colW = w / steps; if (colW < 1) colW = 1;
  for (int k = 0; k < steps; k++) {
    int frac = (dir > 0) ? k : (steps - 1 - k);
    int rowH = h - (frac * h) / steps;
    if (rowH < 1) rowH = 1;
    int x = leftX + k * colW;
    lightFillRect(x, cy - rowH / 2, colW + 1, rowH, col);
  }
}

// 抽象ロゴのグリフ（市松模様。文字が読み取りづらい低解像度でも図案として成立する）。
static void raceDrawLogoGlyph(int x0, int y0, int w, int h, uint16_t colA, uint16_t colB) {
  int cw = w / 2; if (cw < 1) cw = 1;
  int ch = h / 2; if (ch < 1) ch = 1;
  lightFillRect(x0,      y0,      cw,     ch,     colA);
  lightFillRect(x0 + cw, y0,      w - cw, ch,     colB);
  lightFillRect(x0,      y0 + ch, cw,     h - ch, colB);
  lightFillRect(x0 + cw, y0 + ch, w - cw, h - ch, colA);
}

void buildRaceTable() {
  for (int i = 0; i < 4; i++) {
    uint8_t r = RACE_CAR_RGB[i][0], g = RACE_CAR_RGB[i][1], b = RACE_CAR_RGB[i][2];
    raceCarCol[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  for (int i = 0; i < 4; i++) {
    uint8_t r = RACE_SIGN_RGB[i][0], g = RACE_SIGN_RGB[i][1], b = RACE_SIGN_RGB[i][2];
    raceSignCol[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  raceCurveTarget = raceCurveSmooth = 0.0f;
  racePlayerX = racePlayerXTarget = 0.0f;
  raceSpeed = raceSpeedTarget = 1.0f;
  raceScrollZ = 0.0f;
  for (int i = 0; i < RACE_MAX_CARS;  i++) raceSpawnCar(i, true);
  for (int i = 0; i < RACE_MAX_SIGNS; i++) raceSpawnSign(i, true);
  raceReady = true;
}

void lightRenderRace(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!raceReady) buildRaceTable();
  unsigned long now = millis();
  if (needsInit) {
    raceCurveChangeAt = 0;
    raceSpeedChangeAt = 0;
  }

  // ── カーブ目標を数秒おきに更新（直線区間も混ぜて単調にしない）──
  if (now >= raceCurveChangeAt) {
    float r = raceRand01();
    if (r < 0.30f)      raceCurveTarget = 0.0f;
    else if (r < 0.65f) raceCurveTarget =  0.35f + raceRand01() * 0.65f;   // 右カーブ
    else                raceCurveTarget = -(0.35f + raceRand01() * 0.65f); // 左カーブ
    raceCurveChangeAt = now + 3500 + (unsigned long)(raceRand01() * 5000.0f);
  }
  raceCurveSmooth += (raceCurveTarget - raceCurveSmooth) * 0.02f;

  // ── 走行速度もゆるやかに変化。きついカーブでは少し減速し、音楽が鳴っていると少し加速 ──
  if (now >= raceSpeedChangeAt) {
    raceSpeedTarget = 0.80f + raceRand01() * 0.55f;
    raceSpeedChangeAt = now + 2500 + (unsigned long)(raceRand01() * 3500.0f);
  }
  float curveDrag  = 1.0f - fabsf(raceCurveSmooth) * 0.25f;
  float audioBoost = 1.0f + gViz.level * 0.35f;
  raceSpeed += ((raceSpeedTarget * curveDrag * audioBoost) - raceSpeed) * 0.03f;
  if (raceSpeed < 0.35f) raceSpeed = 0.35f;
  raceScrollZ += raceSpeed * 3.2f;

  // ── 自動操舵：回避すべき前方車両を探す（一番近いものだけを見る）──
  int   dangerIdx = -1;
  float dangerT    = -1.0f;
  for (int i = 0; i < RACE_MAX_CARS; i++) {
    if (raceCarZ[i] < 0.45f || raceCarZ[i] > 0.97f) continue;
    if (raceCarZ[i] > dangerT) { dangerT = raceCarZ[i]; dangerIdx = i; }
  }
  if (dangerIdx >= 0) {
    float avoid = raceCarLane[dangerIdx] + (float)raceCarAvoidSide[dangerIdx] * 0.55f;
    racePlayerXTarget = constrain(avoid, -0.85f, 0.85f);
  } else {
    float wander = sinf((float)now * 0.0007f) * 0.12f;
    racePlayerXTarget = constrain(-raceCurveSmooth * 0.30f + wander, -0.6f, 0.6f);
  }
  racePlayerX += (racePlayerXTarget - racePlayerX) * 0.05f;

  // ── かりポムの黒目：手前の車や自車位置ではなく、地平線付近＝道路の進行方向
  //    （カーブの先）を見る。可動範囲・追従の勢いは新しい値を作らず、既存の
  //    ジョイスティック黒目制御と同じ EYE_SHIFT_PIXELS を上限にそのまま流用する。
  //    raceCurveSmooth 自体が数秒がかりでゆっくり変化する値（0.02/frameで平滑化済み）
  //    なので、ここで追加のフィルタを重ねなくても急な動きにはならない
  //    （＝ジョイスティックの「現在値をそのまま反映」という制御と同じ考え方）。
  int eyeTargetX = (int)lroundf(raceCurveSmooth * (float)EYE_SHIFT_PIXELS);
  eyeOffsetX = constrain(eyeTargetX, -EYE_SHIFT_PIXELS, EYE_SHIFT_PIXELS);
  eyeOffsetY = 0;

  // ── 空（地平線の上）──
  const uint16_t SKY_TOP     = (uint16_t)(((120 & 0xF8) << 8) | ((190 & 0xFC) << 3) | (235 >> 3));
  const uint16_t SKY_HORIZON = (uint16_t)(((235 & 0xF8) << 8) | ((235 & 0xFC) << 3) | (215 >> 3));
  const uint16_t SUN_COL     = (uint16_t)(((255 & 0xF8) << 8) | ((235 & 0xFC) << 3) | (120 >> 3));
  lightFillRect(0, RACE_TOP, 320, (RACE_HORIZON - 18) - RACE_TOP, SKY_TOP);
  lightFillRect(0, RACE_HORIZON - 18, 320, 18, SKY_HORIZON);
  GFX.fillCircle(258, RACE_TOP + 20, 12, lightBright(SUN_COL));

  // ── 道路（地平線〜下端。帯ごとに塗ってカクカクした低解像度感を出す）──
  const uint16_t GRASS_A = (uint16_t)(((40  & 0xF8) << 8) | ((150 & 0xFC) << 3) | (50  >> 3));
  const uint16_t GRASS_B = (uint16_t)(((30  & 0xF8) << 8) | ((125 & 0xFC) << 3) | (40  >> 3));
  const uint16_t ROAD_A  = (uint16_t)(((90  & 0xF8) << 8) | ((90  & 0xFC) << 3) | (95  >> 3));
  const uint16_t ROAD_B  = (uint16_t)(((78  & 0xF8) << 8) | ((78  & 0xFC) << 3) | (83  >> 3));
  const uint16_t CURB_A  = (uint16_t)(((235 & 0xF8) << 8) | ((60  & 0xFC) << 3) | (55  >> 3));
  const uint16_t CURB_B  = (uint16_t)(((245 & 0xF8) << 8) | ((245 & 0xFC) << 3) | (245 >> 3));
  const uint16_t LINE_COL= (uint16_t)(((250 & 0xF8) << 8) | ((235 & 0xFC) << 3) | (110 >> 3));

  const float ROAD_MIN_HW  = 8.0f, ROAD_MAX_HW = 152.0f;
  const float CURVE_MAX_PX = 132.0f;
  const float STEER_MAX_PX = 46.0f;
  float steerOffset = -racePlayerX * STEER_MAX_PX;

  for (int y = RACE_HORIZON; y < 240; y += RACE_ROW_H) {
    float t = (float)(y - RACE_HORIZON) / (float)(239 - RACE_HORIZON);
    float rowOffset = raceCurveSmooth * CURVE_MAX_PX * (1.0f - t) * (1.0f - t);
    float centerX = 160.0f + rowOffset + steerOffset;
    float halfW   = ROAD_MIN_HW + (ROAD_MAX_HW - ROAD_MIN_HW) * t * t;

    int rowH = (240 - y < RACE_ROW_H) ? (240 - y) : RACE_ROW_H;
    int cxL  = (int)(centerX - halfW);
    int cxR  = (int)(centerX + halfW);

    long stripeIdx = (long)((raceScrollZ * (0.4f + t * 2.2f)) / 10.0f) + (y / RACE_ROW_H);

    // 芝生（縞・スクロールで前進感を出す）
    uint16_t grass = (stripeIdx & 1) ? GRASS_A : GRASS_B;
    if (cxL > 0)   lightFillRect(0, y, cxL, rowH, grass);
    if (cxR < 320) lightFillRect(cxR, y, 320 - cxR, rowH, grass);

    // 縁石（カーブがきついほど太く、交互に色が変わる）
    int curbW = 3 + (int)(fabsf(raceCurveSmooth) * 5.0f);
    if (curbW > (int)halfW) curbW = (int)halfW;
    if (curbW < 1) curbW = 1;
    uint16_t curb = (stripeIdx & 1) ? CURB_A : CURB_B;
    lightFillRect(cxL, y, curbW, rowH, curb);
    lightFillRect(cxR - curbW, y, curbW, rowH, curb);

    // 路面（破線のセンターライン付き）
    uint16_t road = (stripeIdx & 1) ? ROAD_A : ROAD_B;
    int roadW = (cxR - curbW) - (cxL + curbW);
    if (roadW > 0) lightFillRect(cxL + curbW, y, roadW, rowH, road);
    if (((stripeIdx / 2) & 1) == 0) {
      int lineW = 2 + (int)(t * 4.0f);
      lightFillRect((int)(centerX - lineW / 2), y, lineW, rowH, LINE_COL);
    }
  }

  // ── 道路脇の看板：ときどき左右どちらかに現れ、手前へ流れてくる ──
  //   カーブと同じ centerX/halfW を使うので、道路の曲がりに沿って自然に流れる。
  //   遠景は板の色だけ、近づいたものだけ矢印／ロゴ／文字の中身を描く。
  for (int i = 0; i < RACE_MAX_SIGNS; i++) {
    raceSignZ[i] += 0.0017f * raceSpeed * raceSignSpeedMul[i];
    if (raceSignZ[i] > 1.05f) { raceSpawnSign(i, false); continue; }
    if (raceSignZ[i] < 0.0f || raceSignZ[i] > 1.0f) continue;   // 出現待ち or 描画範囲外

    float t = raceSignZ[i];
    int   y = RACE_HORIZON + (int)(t * (239 - RACE_HORIZON));
    float rowOffset = raceCurveSmooth * CURVE_MAX_PX * (1.0f - t) * (1.0f - t);
    float centerX = 160.0f + rowOffset + steerOffset;
    float halfW   = ROAD_MIN_HW + (ROAD_MAX_HW - ROAD_MIN_HW) * t * t;

    float scale = (0.45f + t * t * 2.1f) * raceSignScale[i];
    int plateW = (int)(16.0f * scale); if (plateW < 6) plateW = 6;
    int plateH = (int)(11.0f * scale); if (plateH < 5) plateH = 5;
    int postH  = (int)(6.0f  * scale) + 2;

    int side   = raceSignSide[i];
    int edgeX  = (int)(centerX + (float)side * (halfW + 8.0f));
    int postX  = edgeX + side * (plateW / 2 + 3);
    int postY1 = y;
    int postY0 = postY1 - postH;
    int plateX0 = postX - plateW / 2;
    int plateY0 = postY0 - plateH;

    uint16_t plateCol = raceSignCol[raceSignColorIdx[i]];
    lightFillRect(postX - 1, postY0, 2, postH, 0x0000);        // 支柱
    lightFillRect(plateX0, plateY0, plateW, plateH, plateCol); // 板

    if (t > 0.30f) {  // 遠景は板だけ。近づいたものだけ中身を描く（低解像度なりの視認性配慮）
      bool     darkText = RACE_SIGN_DARKTEXT[raceSignColorIdx[i]];
      uint16_t glyphCol = darkText ? 0x0000 : 0xFFFF;
      uint8_t  type = raceSignType[i];
      if (type == 0) {
        int dir = (raceCurveSmooth >= 0.0f) ? 1 : -1;  // カーブの向きと同じ方向を指す
        raceDrawArrowGlyph(plateX0 + 2, plateY0 + plateH / 2, plateW - 4, plateH - 4, dir, glyphCol);
      } else if (type == 1) {
        uint16_t colB = darkText ? 0xFFFF : 0x0000;
        raceDrawLogoGlyph(plateX0 + 2, plateY0 + 2, plateW - 4, plateH - 4, glyphCol, colB);
      } else {
        // drawString は lightFillRect を経由しないため、Brightness共通ルールに合わせて
        // 文字色だけ lightBright() を手動で適用する（Laserの輝度非依存と同じ考え方の例外）。
        GFX.setTextDatum(MC_DATUM);
        GFX.setTextColor(lightBright(glyphCol));
        GFX.setTextSize(1);
        GFX.drawString(RACE_SIGN_TEXT[raceSignTextIdx[i]], plateX0 + plateW / 2, plateY0 + plateH / 2);
        GFX.setTextDatum(TL_DATUM);  // 他描画に影響しないよう既定へ戻す
      }
    }
  }

  // ── 対向車：奥→手前の順に描く（3台だけなので単純な並べ替えで十分）──
  int order[RACE_MAX_CARS];
  for (int i = 0; i < RACE_MAX_CARS; i++) order[i] = i;
  for (int a = 0; a < RACE_MAX_CARS - 1; a++) {
    for (int b = a + 1; b < RACE_MAX_CARS; b++) {
      if (raceCarZ[order[b]] < raceCarZ[order[a]]) { int tmp = order[a]; order[a] = order[b]; order[b] = tmp; }
    }
  }

  for (int oi = 0; oi < RACE_MAX_CARS; oi++) {
    int i = order[oi];
    raceCarZ[i] += 0.0026f * raceSpeed * raceCarSpeedMul[i];
    if (raceCarZ[i] > 1.05f) { raceSpawnCar(i, false); continue; }
    if (raceCarZ[i] < 0.0f || raceCarZ[i] > 1.0f) continue;   // 出現待ち or 描画範囲外

    float t = raceCarZ[i];
    int   y = RACE_HORIZON + (int)(t * (239 - RACE_HORIZON));
    float rowOffset = raceCurveSmooth * CURVE_MAX_PX * (1.0f - t) * (1.0f - t);
    float centerX = 160.0f + rowOffset + steerOffset;
    float halfW   = ROAD_MIN_HW + (ROAD_MAX_HW - ROAD_MIN_HW) * t * t;
    float carX    = centerX + raceCarLane[i] * (halfW * 0.75f);

    int carW = (int)(10.0f + t * t * 34.0f);
    int carH = (int)(7.0f  + t * t * 22.0f);
    int cx0  = (int)carX - carW / 2;
    int cy0  = y - carH;

    uint16_t body = raceCarCol[raceCarColorIdx[i]];
    lightFillRect(cx0, cy0, carW, carH, body);
    lightFillRect(cx0 + carW / 5, cy0 - carH / 3, carW - carW * 2 / 5, carH / 3 + 1, 0x0000);  // キャビン
    int wheelH = (carH > 6) ? (carH / 4) : 1;
    lightFillRect(cx0, cy0 + carH - wheelH, carW, wheelH, 0x0000);  // タイヤ
  }
}

// ============================================================================
// Lighting #6 : Sky Raid（Xevious風スクリーンセーバー／Retro Arcadeシリーズ第2弾）（v1.7）
//
// ■ コンセプト
//   1980年代前半のXeviousをイメージした、MSXやPC-6001へ無理に移植したような
//   カクカクした低解像度の縦スクロールデモ。ゲームの忠実再現ではなく、
//   「昔のゲームセンターでアトラクトデモを眺めている雰囲気」を重視する。
//   プレイヤー操作は無く、ゲームオーバーの概念も無い（衝突判定は敵と自機の
//   間では行わず、自動ショットと敵の当たり判定のみを扱う）。
//
// ■ 自動飛行ロジック（Retro Raceと同じく厳密な物理は不要）
//   ・xevShipX      … 自機のX位置。普段はゆったり左右へ漂い、接近する敵が
//     近ければ反対側へ寄って自然に避ける（衝突判定ではなく距離判定のみ）。
//   ・xevEnemyY/X[] … 敵は画面上方からランダムに出現し、自機へゆるやかに
//     ホーミングしつつ左右へ揺れながら下降する。編隊は組まず、各スロットが
//     独立したタイミングで再出現する。
//   ・xevShotY/X[]  … 自機が一定間隔で自動発射。敵と重なると簡易な爆発
//     （ドットの飛散）を発生させ、その敵を再出現させる。
//   ・背景は森・草地・道路・川・基地・滑走路をワールド座標のハッシュ値から
//     手続き的に決めるため、配列を持たずに毎回・毎区間ちがう地形になる。
//
// ■ かりポムの目線（今回の要件の核）
//   Retro Raceと同じ理由（Lighting中の setEyeDirection() は白背景で塗り
//   潰してしまう）で、ここでも eyeOffsetX/eyeOffsetY を直接更新するだけに
//   留める。手前の敵ではなく「画面上部＝飛行方向」を見せたいため、
//   eyeOffsetY は既存のジョイスティック黒目制御（UP方向）と同じ
//   -EYE_SHIFT_PIXELS に固定し、eyeOffsetX だけ自機の操舵に応じてゆるやかに
//   動く（xevShipX 自体が既に平滑化済みの値なので、追加のフィルタは重ねない）。
//
// ■ 共通ルール遵守
//   ・背景の描画は lightFillRect を使う → Framework共通Brightnessが自動で効く
//   ・上端48px（情報パネル）には描かない（XEV_TOP=48でクリップ）
//   ・顔はコンポジタが最前面に描く
//   ・動的メモリ確保なし。すべて固定長の静的配列／スカラー変数のみ
// ============================================================================
#define XEV_TOP         48
#define XEV_ROW_H       6                 // 帯の高さ（低解像度感を出す）
#define XEV_SHIP_Y      206
#define XEV_MAX_ENEMIES 4
#define XEV_MAX_SHOTS   3
#define XEV_MAX_EXPL    3

static bool     xevReady   = false;
static uint32_t xevRng     = 0x9E3779B9u;
static uint32_t xevSeed    = 0;           // 背景ハッシュの種。有効化のたびに引き直す

static float xevScrollY       = 0.0f;     // 地形のスクロール量（ワールド座標）
static float xevSpeed         = 1.0f;
static float xevSpeedTarget   = 1.0f;
static unsigned long xevSpeedChangeAt = 0;

static float xevShipX       = 160.0f;
static float xevShipXTarget = 160.0f;

static unsigned long xevNextShotAt = 0;

// 黒目専用の追加平滑化（2026-07-23調整）。自機の操舵にそのまま連動させると
// 実機ではチラチラ動きすぎたため、目線だけは独立してさらにゆっくり追従させる。
static float xevEyeXf = 0.0f;

static float   xevEnemyY[XEV_MAX_ENEMIES];
static float   xevEnemyX[XEV_MAX_ENEMIES];
static float   xevEnemySpeed[XEV_MAX_ENEMIES];
static float   xevEnemyWobPhase[XEV_MAX_ENEMIES];
static uint8_t xevEnemyColorIdx[XEV_MAX_ENEMIES];

static float   xevShotX[XEV_MAX_SHOTS];
static float   xevShotY[XEV_MAX_SHOTS];
static uint8_t xevShotActive[XEV_MAX_SHOTS];

static float    xevExplX[XEV_MAX_EXPL];
static float    xevExplY[XEV_MAX_EXPL];
static uint8_t  xevExplAge[XEV_MAX_EXPL];    // 0=非アクティブ／1から加齢して寿命で0へ戻る
static uint32_t xevExplSeed[XEV_MAX_EXPL];

static const uint8_t XEV_ENEMY_RGB[3][3] = { {230,70,70}, {230,150,60}, {200,90,220} };
static uint16_t xevEnemyCol[3];

static inline uint32_t xevRand()   { xevRng = xevRng * 1664525u + 1013904223u; return xevRng; }
static inline float    xevRand01() { return (float)(xevRand() & 0xFFFF) / 65535.0f; }

// セグメント番号から地形の見た目を決める（配列を持たず手続き的に生成するハッシュ）。
static inline uint32_t xevHash(int32_t seg) {
  uint32_t h = (uint32_t)seg * 2654435761u + xevSeed;
  h ^= h >> 13; h *= 0x85ebca6bu; h ^= h >> 16;
  return h;
}

static void xevSpawnEnemy(int i, bool initialSpread) {
  xevEnemyY[i]         = initialSpread ? (float)XEV_TOP - xevRand01() * 260.0f
                                        : (float)XEV_TOP - (20.0f + xevRand01() * 170.0f);
  xevEnemyX[i]         = 50.0f + xevRand01() * 220.0f;
  xevEnemySpeed[i]     = 0.55f + xevRand01() * 0.55f;
  xevEnemyWobPhase[i]  = xevRand01() * 6.2832f;
  xevEnemyColorIdx[i]  = (uint8_t)(xevRand() % 3);
}

static void xevSpawnExplosion(float x, float y) {
  for (int i = 0; i < XEV_MAX_EXPL; i++) {
    if (xevExplAge[i] != 0) continue;
    xevExplX[i] = x; xevExplY[i] = y;
    xevExplAge[i] = 1;
    xevExplSeed[i] = xevRand();
    return;
  }
  // 空きが無ければ何もしない（演出を1つ諦めるだけで、事故やクラッシュにはしない）
}

void buildSkyRaidTable() {
  for (int i = 0; i < 3; i++) {
    uint8_t r = XEV_ENEMY_RGB[i][0], g = XEV_ENEMY_RGB[i][1], b = XEV_ENEMY_RGB[i][2];
    xevEnemyCol[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  xevSeed = xevRand();  // 起動のたびに背景の組み合わせを変える
  xevScrollY = 0.0f;
  xevSpeed = xevSpeedTarget = 1.0f;
  xevShipX = xevShipXTarget = 160.0f;
  xevNextShotAt = 0;
  for (int i = 0; i < XEV_MAX_ENEMIES; i++) xevSpawnEnemy(i, true);
  for (int i = 0; i < XEV_MAX_SHOTS;   i++) xevShotActive[i] = 0;
  for (int i = 0; i < XEV_MAX_EXPL;    i++) xevExplAge[i] = 0;
  xevReady = true;
}

static void xevDrawShip(int cx, int cy, uint16_t col) {
  lightFillRect(cx - 2, cy - 10, 4, 10, col);       // 胴体
  lightFillRect(cx - 8, cy - 2,  16, 4, col);        // 主翼
  lightFillRect(cx - 2, cy - 14, 4, 4, 0xFFFF);      // コックピット
}

static void xevDrawEnemy(int cx, int cy, int size, uint16_t col) {
  if (size < 3) size = 3;
  lightFillRect(cx - size / 2, cy - size / 2, size, size, col);
  lightFillRect(cx - size / 4, cy - size / 4, size / 2 + 1, size / 2 + 1, 0x0000);
}

// 爆発のドット飛散（xevHashを流用し、爆発ごと・経過フレームごとに違う配置にする）。
static void xevDrawExplosion(int cx, int cy, uint8_t age, uint32_t seed) {
  uint16_t col = (age <= 2) ? 0xFFFF : (uint16_t)(((255 & 0xF8) << 8) | ((150 & 0xFC) << 3) | (40 >> 3));
  const int n = 6;
  for (int k = 0; k < n; k++) {
    uint32_t h = xevHash((int32_t)(seed + (uint32_t)k * 97u + (uint32_t)age * 131u));
    float ang  = (float)(h % 360u) * 3.14159f / 180.0f;
    float dist = 2.0f + (float)age * 2.1f + (float)((h >> 8) % 5u);
    int dx = (int)(cosf(ang) * dist);
    int dy = (int)(sinf(ang) * dist);
    int sz = (age <= 3) ? 2 : 1;
    lightFillRect(cx + dx, cy + dy, sz, sz, col);
  }
}

void lightRenderSkyRaid(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!xevReady) buildSkyRaidTable();
  unsigned long now = millis();
  if (needsInit) {
    xevSpeedChangeAt = 0;
    xevNextShotAt    = 0;
    xevEyeXf         = 0.0f;
  }

  // ── スクロール速度もRetro Race同様ゆるやかに変化（音楽が鳴っていると少し加速）──
  if (now >= xevSpeedChangeAt) {
    xevSpeedTarget = 0.80f + xevRand01() * 0.55f;
    xevSpeedChangeAt = now + 2500 + (unsigned long)(xevRand01() * 3500.0f);
  }
  float audioBoost = 1.0f + gViz.level * 0.35f;
  xevSpeed += ((xevSpeedTarget * audioBoost) - xevSpeed) * 0.03f;
  if (xevSpeed < 0.35f) xevSpeed = 0.35f;
  xevScrollY += xevSpeed * 2.6f;

  // ── 自機の自動操舵：接近する敵を避けつつ、普段はゆったり左右に漂う ──
  int   dangerIdx = -1;
  float dangerY    = -1.0e9f;
  for (int i = 0; i < XEV_MAX_ENEMIES; i++) {
    if (xevEnemyY[i] < XEV_SHIP_Y - 100.0f || xevEnemyY[i] > XEV_SHIP_Y - 5.0f) continue;
    if (fabsf(xevEnemyX[i] - xevShipX) > 70.0f) continue;
    if (xevEnemyY[i] > dangerY) { dangerY = xevEnemyY[i]; dangerIdx = i; }
  }
  if (dangerIdx >= 0) {
    float dir = (xevEnemyX[dangerIdx] >= xevShipX) ? -1.0f : 1.0f;
    xevShipXTarget = constrain(xevShipX + dir * 70.0f, 50.0f, 270.0f);
  } else {
    float wander = sinf((float)now * 0.0009f) * 70.0f;
    xevShipXTarget = constrain(160.0f + wander, 60.0f, 260.0f);
  }
  xevShipX += (xevShipXTarget - xevShipX) * 0.03f;

  // ── かりポムの黒目：手前の敵や自機の細かな操舵ではなく、画面上部＝飛行方向を
  //    落ち着いて見ているように見せる（2026-07-23調整）。
  //    実機で「上目遣いでチラチラする」と分かったため、以下を変更した：
  //      ・Yは常時最大上向き(-EYE_SHIFT_PIXELS=-14)ではなく、中央より少し上
  //        （-4〜-7px）に留める。ゆっくりとした周期でこの範囲内だけ揺らぐ。
  //      ・Xは自機の操舵(xevShipX)へ直接連動させず、専用の変数 xevEyeXf を
  //        自機よりさらにゆっくり(0.015/frame)追従させたうえで ±4〜6px に
  //        収める。自機や敵が細かく動いても黒目までは即座に伝わらない。
  const float XEV_EYE_X_MAX = 5.0f;    // 左右4〜6px程度の範囲に収める
  const float XEV_EYE_Y_MIN = -7.0f;   // 上へ4〜7px程度の範囲に収める
  const float XEV_EYE_Y_MAX = -4.0f;
  float eyeDriftTargetX = ((xevShipX - 160.0f) / 110.0f) * XEV_EYE_X_MAX;
  xevEyeXf += (eyeDriftTargetX - xevEyeXf) * 0.015f;
  eyeOffsetX = constrain((int)lroundf(xevEyeXf), (int)-XEV_EYE_X_MAX, (int)XEV_EYE_X_MAX);
  float eyeY = (XEV_EYE_Y_MIN + XEV_EYE_Y_MAX) * 0.5f
               + sinf((float)now * 0.00025f) * ((XEV_EYE_Y_MAX - XEV_EYE_Y_MIN) * 0.5f);
  eyeOffsetY = (int)lroundf(eyeY);

  // ── 背景：森・草地・道路・川・基地・滑走路をハッシュから手続き的に生成 ──
  const uint16_t GRASS_A  = (uint16_t)(((60  & 0xF8) << 8) | ((150 & 0xFC) << 3) | (70  >> 3));
  const uint16_t GRASS_B  = (uint16_t)(((45  & 0xF8) << 8) | ((130 & 0xFC) << 3) | (58  >> 3));
  const uint16_t FOREST   = (uint16_t)(((25  & 0xF8) << 8) | ((90  & 0xFC) << 3) | (40  >> 3));
  const uint16_t FOREST_D = (uint16_t)(((15  & 0xF8) << 8) | ((70  & 0xFC) << 3) | (30  >> 3));
  const uint16_t ROAD     = (uint16_t)(((95  & 0xF8) << 8) | ((95  & 0xFC) << 3) | (100 >> 3));
  const uint16_t ROAD_LN  = (uint16_t)(((230 & 0xF8) << 8) | ((220 & 0xFC) << 3) | (120 >> 3));
  const uint16_t RIVER    = (uint16_t)(((50  & 0xF8) << 8) | ((110 & 0xFC) << 3) | (210 >> 3));
  const uint16_t RIVER_HI = (uint16_t)(((120 & 0xF8) << 8) | ((180 & 0xFC) << 3) | (240 >> 3));
  const uint16_t BASE     = (uint16_t)(((90  & 0xF8) << 8) | ((90  & 0xFC) << 3) | (100 >> 3));
  const uint16_t BASE_AC  = (uint16_t)(((200 & 0xF8) << 8) | ((60  & 0xFC) << 3) | (60  >> 3));
  const uint16_t RUNWAY   = (uint16_t)(((150 & 0xF8) << 8) | ((150 & 0xFC) << 3) | (155 >> 3));
  const uint16_t RUNWAY_LN= (uint16_t)(((240 & 0xF8) << 8) | ((240 & 0xFC) << 3) | (240 >> 3));

  // 2026-07-23調整：地形の単位を「44pxの区間ごとに独立抽選」から「約260px
  // （画面高さ以上）続く1つのラン」へ変更した。ラン内部は sinf() による
  // なめらかな蛇行だけで形が変わるため、川や道路が区間の切れ目でブツ切れの
  // 色付き四角に見えていた問題が解消し、一本につながった地形として流れる。
  // ラン境界（約260pxごと）でのみ種類・中心位置・幅・蛇行量を再抽選するため、
  // 「曲がり方・幅・出現順のランダム性」は変更前と同じく維持している。
  const float XEV_RUN_PX = 260.0f;   // 1ランのワールド距離（画面高さ以上＝連続して見える）
  for (int y = XEV_TOP; y < 240; y += XEV_ROW_H) {
    int rowH = (240 - y < XEV_ROW_H) ? (240 - y) : XEV_ROW_H;
    float wy = xevScrollY + (float)(y - XEV_TOP);
    int32_t run = (int32_t)floorf(wy / XEV_RUN_PX);
    uint32_t hR = xevHash(run);

    uint16_t grass = ((run + (int32_t)(wy / 6.0f)) & 1) ? GRASS_A : GRASS_B;
    lightFillRect(0, y, 320, rowH, grass);

    if ((hR % 20u) < 9u) {   // ランの約45%だけ地形フィーチャーを乗せる（変更前と同じ比率）
      uint8_t ftype       = (uint8_t)((hR >> 5) % 5u);
      float   centerBase  = 60.0f + (float)((hR >> 10) % 190u);
      float   widthBase   = 40.0f + (float)((hR >> 18) % 80u);
      float   wiggleAmp   = 8.0f  + (float)((hR >> 24) % 20u);
      float   wigglePhase = (float)(hR % 1000u) * 0.00628f;

      // 種類ごとに蛇行の強さを変える：川・森は大きく曲がり、道路・滑走路・基地は
      // ほぼ真っ直ぐ（現実の地形らしさのための最小限の作り分け）。
      float wiggleScale;
      switch (ftype) {
        case 0: wiggleScale = 1.00f; break;  // 森
        case 1: wiggleScale = 0.15f; break;  // 道路
        case 2: wiggleScale = 0.90f; break;  // 川
        case 3: wiggleScale = 0.05f; break;  // 基地
        default: wiggleScale = 0.12f; break; // 滑走路
      }

      float wiggle  = sinf(wy * 0.02f + wigglePhase) * wiggleAmp * wiggleScale;
      float fCenter = centerBase + wiggle;
      float fWidth  = widthBase + sinf(wy * 0.013f + wigglePhase * 1.7f) * (widthBase * 0.15f);
      int fx = (int)(fCenter - fWidth * 0.5f);
      int fw = (int)fWidth;
      if (fx < 0)          { fw += fx; fx = 0; }
      if (fx + fw > 320)   fw = 320 - fx;

      if (fw > 0) {
        switch (ftype) {
          case 0:  // 森（まとまりのある森林。内部に濃淡の木立を点在させる）
            lightFillRect(fx, y, fw, rowH, FOREST);
            if ((((y / XEV_ROW_H) ^ run) & 3) == 0) lightFillRect(fx + fw / 3, y, fw / 4 + 2, rowH, FOREST_D);
            break;
          case 1:  // 道路（縦につながる一本道。中央に破線）
            lightFillRect(fx, y, fw, rowH, ROAD);
            if (((y / XEV_ROW_H) & 1) == 0) lightFillRect(fx + fw / 2 - 1, y, 2, rowH, ROAD_LN);
            break;
          case 2:  // 川（連続してうねる一本の川。中央にハイライトの流れ）
            lightFillRect(fx, y, fw, rowH, RIVER);
            lightFillRect((int)(fCenter + sinf(wy * 0.05f) * (fWidth * 0.15f)) - 2, y, 4, rowH, RIVER_HI);
            break;
          case 3:  // 基地（複数の建物区画を持つ意味のある一区画）
            lightFillRect(fx, y, fw, rowH, BASE);
            if (((y / XEV_ROW_H) % 5) == 0) lightFillRect(fx + 4, y, (fw > 8 ? fw - 8 : fw), rowH, BASE_AC);
            break;
          default: // 滑走路（直線的な一本の帯に中央線）
            lightFillRect(fx, y, fw, rowH, RUNWAY);
            if (((y / XEV_ROW_H) & 3) == 0) lightFillRect(fx + fw / 2 - 1, y, 2, rowH, RUNWAY_LN);
            break;
        }
      }
    }
  }

  // ── 敵：下降・左右のゆらぎ・自機への緩やかなホーミング。画面外へ抜けたら再出現 ──
  for (int i = 0; i < XEV_MAX_ENEMIES; i++) {
    xevEnemyY[i] += (1.4f + xevEnemySpeed[i] * 1.6f) * xevSpeed;
    xevEnemyX[i] += (xevShipX - xevEnemyX[i]) * 0.0025f;
    xevEnemyX[i] += sinf((float)now * 0.005f + xevEnemyWobPhase[i]) * 0.6f;
    xevEnemyX[i]  = constrain(xevEnemyX[i], 20.0f, 300.0f);
    if (xevEnemyY[i] > 255.0f) { xevSpawnEnemy(i, false); continue; }
    if (xevEnemyY[i] < (float)XEV_TOP) continue;   // まだ画面外（上方）
    int size = (int)(6.0f + (xevEnemyY[i] - (float)XEV_TOP) * 0.03f);
    xevDrawEnemy((int)xevEnemyX[i], (int)xevEnemyY[i], size, xevEnemyCol[xevEnemyColorIdx[i]]);
  }

  // ── 自機の自動ショット：一定間隔（ランダム幅つき）で発射 ──
  if (now >= xevNextShotAt) {
    for (int i = 0; i < XEV_MAX_SHOTS; i++) {
      if (xevShotActive[i]) continue;
      xevShotX[i] = xevShipX;
      xevShotY[i] = (float)XEV_SHIP_Y - 14.0f;
      xevShotActive[i] = 1;
      break;
    }
    xevNextShotAt = now + 260 + (unsigned long)(xevRand01() * 260.0f);
  }

  // ── ショットの移動と、敵との簡易当たり判定（当たったら小さな爆発） ──
  for (int i = 0; i < XEV_MAX_SHOTS; i++) {
    if (!xevShotActive[i]) continue;
    xevShotY[i] -= (5.0f + xevSpeed * 2.0f);
    if (xevShotY[i] < (float)XEV_TOP - 6.0f) { xevShotActive[i] = 0; continue; }

    for (int e = 0; e < XEV_MAX_ENEMIES; e++) {
      if (xevEnemyY[e] < (float)XEV_TOP) continue;
      if (fabsf(xevShotX[i] - xevEnemyX[e]) < 10.0f && fabsf(xevShotY[i] - xevEnemyY[e]) < 9.0f) {
        xevSpawnExplosion(xevEnemyX[e], xevEnemyY[e]);
        xevSpawnEnemy(e, false);
        xevShotActive[i] = 0;
        break;
      }
    }
    if (xevShotActive[i]) lightFillRect((int)xevShotX[i] - 1, (int)xevShotY[i] - 3, 2, 5, 0xFFE0);
  }

  // ── 自機（最前面寄り）──
  xevDrawShip((int)xevShipX, XEV_SHIP_Y, 0x07FF);

  // ── 爆発：ドットが飛散して短時間で消える。毎回すこし違う配置にする ──
  for (int i = 0; i < XEV_MAX_EXPL; i++) {
    if (xevExplAge[i] == 0) continue;
    xevDrawExplosion((int)xevExplX[i], (int)xevExplY[i], xevExplAge[i], xevExplSeed[i]);
    xevExplAge[i]++;
    if (xevExplAge[i] > 6) xevExplAge[i] = 0;
  }
}

// ============================================================================
// Lighting #7 : Eye Slot（お目々スロット）（v1.7）
//
// ■ コンセプト
//   通常の顔（眉・鼻・口）はそのまま維持し、左右の黒目だけを2リールの
//   スロットマシンに置き換える。ゲームではなく「眺めて楽しむ」演出。
//   プレイヤー操作は無く、回転→減速→停止→結果表示を自動で延々と繰り返す。
//
// ■ リール構成
//   左右の目＝2リール（3リールではない。目が2つなのでそのまま活かす）。
//   絵柄は 7 / BAR / Cherry / ● / ¥ / $ / ❤️ / 🥝 の8種類（2026-07-23追加分含む）。
//   さらに増やす場合は ESLOT_SYMBOL_COUNT を増やして eslotDrawSymbol() に
//   case を足すだけでよい。左右のリールは固定順の配列 ESLOT_STRIP_L[] /
//   ESLOT_STRIP_R[]（順序はあえて左右で変えている）を毎フレーム参照するだけで、
//   絵柄をランダムに切り替えるような処理はしていない。
//
// ■ 絵柄の見え方（2026-07-23調整：黒目と同じ大きさで表示）
//   絵柄を縮小して3つ収めるのではなく、黒目(直径約40px)と同等の大きさで
//   描いたうえで、表示窓を約2絵柄分の高さ(ESLOT_ROW_H*2)だけに
//   setClipRect()/clearClipRect() でクリッピングする。結果として停止時は
//   「中央：絵柄1個を完全表示／上端：前の絵柄の下半分／下端：次の絵柄の上半分」
//   という、実物のスロットの窓を覗いているような見え方になる。
//
// ■ 回転の見せ方（今回の最優先事項）
//   単純な絵柄の切り替えではなく、pos_k(S)=S-k という1本の式で
//   「絵柄kが画面上のどの高さにいるか」を連続的に計算し、上から流れて
//   中央を通り下へ抜けるスクロールを表現する（Sは連続増加する回転量、
//   kは絵柄のインデックス）。停止時は S の値をあらかじめ決めた整数
//   （eslotDecelTo）へイーズアウトで正確に着地させることで、
//   「中央ラインへ吸着するように止まる」動きを実現している。
//   停止順は 左 → 少し間を置いて右（ESLOT_ST_GAPで期待感を作る）。
//
// ■ かりポムの黒目との関係
//   drawVisualizerFaceParts() 側に gEyeSlotActive ガードを追加し、Eye Slotが
//   背景として採用されている間だけ通常の黒目描画をスキップしてもらう
//   （このファイルの先頭付近で gEyeSlotActive = false 済み、本関数の先頭で
//   true に戻す＝「今フレームの背景として実際に採用された時だけ」正しく働く）。
//   リールは eyeOffsetX/Y を中心に置くため、黒目が元々あった位置＝
//   90+eyeOffsetX,90+eyeOffsetY（左）/230+eyeOffsetX,90+eyeOffsetY（右）に表示される。
//
// ■ 共通ルール遵守
//   ・背景の描画は lightFillRect を使う → Framework共通Brightnessが自動で効く
//   ・上端48px（情報パネル）には描かない（ESLOT_TOP=48でクリップ）
//   ・毎フレーム白で全面塗り直し＝通常の顔と同じ背景を維持（鼻・口・まゆ毛は
//     Layer2＝drawVisualizerFaceParts()がそのまま描く）
//   ・動的メモリ確保なし。すべて固定長のスカラー変数のみ（配列すら不要な規模）
// ============================================================================
#define ESLOT_TOP           48
#define ESLOT_ROW_H         40               // リール1コマぶんの高さ(px)。黒目(直径約40px)と同等
#define ESLOT_WIN_HALF_W    20               // 表示窓の半幅(px)
#define ESLOT_SYM_R         17               // 各絵柄の基準半径/半サイズ(px)
#define ESLOT_SYMBOL_COUNT  8                // 7/BAR/Cherry/●/¥/$/❤️/🥝
#define ESLOT_MAX_SPEED     0.55f            // 全開回転時のスクロール速度（絵柄/フレーム）

// 絵柄インデックス：0=7 1=BAR 2=Cherry 3=● 4=¥ 5=$ 6=❤️ 7=🥝
// 左右のリールは固定順。順序はあえて左右で少し変えている（要望どおり）。
static const uint8_t ESLOT_STRIP_L[ESLOT_SYMBOL_COUNT] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const uint8_t ESLOT_STRIP_R[ESLOT_SYMBOL_COUNT] = { 2, 5, 7, 1, 4, 0, 6, 3 };

#define ESLOT_ST_ACCEL    0   // 加速：0→全開まで速度を上げる
#define ESLOT_ST_SPIN     1   // 全開回転（両リール）
#define ESLOT_ST_DECEL_L  2   // 左リールだけ減速→停止。右は全開のまま
#define ESLOT_ST_GAP      3   // 左停止後の「間」。右はまだ回っている（期待感）
#define ESLOT_ST_DECEL_R  4   // 右リールを減速→停止
#define ESLOT_ST_RESULT   5   // 両方停止。結果表示（揃っていればフラッシュ）

static uint8_t      eslotState   = ESLOT_ST_ACCEL;
static unsigned long eslotStateAt  = 0;
static unsigned long eslotStateDur = 0;

static float         eslotReelPos[2]   = { 0.0f, 0.0f };   // 連続増加する回転量S（左=0/右=1）
static float         eslotDecelFrom[2] = { 0.0f, 0.0f };
static float         eslotDecelTo[2]   = { 0.0f, 0.0f };
static unsigned long eslotDecelAt[2]   = { 0, 0 };
static unsigned long eslotDecelDur[2]  = { 0, 0 };

static bool     eslotMatch      = false;   // 直近の結果が揃ったか
static unsigned long eslotResultAt = 0;    // RESULT状態に入った時刻（フラッシュのタイミング用）

static uint32_t eslotRng = 0x51A5E1D1u;
static inline uint32_t eslotRand()   { eslotRng = eslotRng * 1664525u + 1013904223u; return eslotRng; }
static inline float    eslotRand01() { return (float)(eslotRand() & 0xFFFF) / 65535.0f; }

// 絵柄1つを描く。黒目(直径約40px)と同等の大きさ＝ESLOT_SYM_R基準で描画する。
// 文字列表示に依存すると機種のフォントに絵柄が無い場合があるため、7とBAR以外は
// すべて線・矩形・円などの描画プリミティブだけで表現している（絵文字フォント不使用）。
static void eslotDrawSymbol(uint8_t sym, int cx, int cy) {
  const int R = ESLOT_SYM_R;
  switch (sym) {
    case 0: {  // 7（赤・太字）
      GFX.setTextDatum(MC_DATUM);
      GFX.setTextColor(lightBright(RED));
      GFX.setTextSize(4);
      GFX.drawString("7", cx, cy);
      GFX.setTextDatum(TL_DATUM);
      break;
    }
    case 1: {  // BAR（黒いプレート＋白い太字BAR）
      lightFillRect(cx - R, cy - R + 4, R * 2, R * 2 - 8, 0x0000);
      GFX.setTextDatum(MC_DATUM);
      GFX.setTextColor(lightBright(WHITE));
      GFX.setTextSize(2);
      GFX.drawString("BAR", cx, cy);
      GFX.setTextDatum(TL_DATUM);
      break;
    }
    case 2: {  // Cherry（赤い実2つ＋緑の軸）
      const uint16_t stem = (uint16_t)(((40 & 0xF8) << 8) | ((150 & 0xFC) << 3) | (60 >> 3));
      lightFillRect(cx - 2, cy - R, 4, R, stem);
      GFX.fillCircle(cx - R / 2, cy + R / 3, R / 2, lightBright(RED));
      GFX.fillCircle(cx + R / 2, cy + R / 3, R / 2, lightBright(RED));
      break;
    }
    case 3: {  // ●（紺）
      const uint16_t navy = (uint16_t)(((30 & 0xF8) << 8) | ((60 & 0xFC) << 3) | (150 >> 3));
      GFX.fillCircle(cx, cy, R - 1, navy);
      break;
    }
    case 4: {  // ¥（太めのY型＋横線2本・黄色系）
      const uint16_t yen = (uint16_t)(((255 & 0xF8) << 8) | ((210 & 0xFC) << 3) | (40 >> 3));
      const int th = 4;
      int legX = (R * 55) / 100;
      drawThickLine(cx - legX, cy - R, cx, cy, th, yen);
      drawThickLine(cx + legX, cy - R, cx, cy, th, yen);
      drawThickLine(cx, cy, cx, cy + R, th, yen);
      lightFillRect(cx - R / 2, cy + 2,          R, th, yen);
      lightFillRect(cx - R / 2, cy + 2 + th * 2, R, th, yen);
      break;
    }
    case 5: {  // $（太めのS＋貫通する縦線・緑系）
      const uint16_t dollar = (uint16_t)(((60 & 0xF8) << 8) | ((190 & 0xFC) << 3) | (80 >> 3));
      const int th = 4;
      int hw = (R * 60) / 100;   // 横方向の半幅
      int vh = (R * 60) / 100;   // 上下ブロックの縦の高さ
      lightFillRect(cx - hw, cy - R,          hw * 2, th, dollar);         // 上の横線
      lightFillRect(cx - hw, cy - R,          th,     vh, dollar);         // 上の左縦
      lightFillRect(cx - hw, cy - th / 2,     hw * 2, th, dollar);         // 中央の横線
      lightFillRect(cx + hw - th, cy,         th,     vh, dollar);         // 下の右縦
      lightFillRect(cx - hw, cy + R - th,     hw * 2, th, dollar);         // 下の横線
      lightFillRect(cx - th / 2, cy - R - 2,  th,     R * 2 + 4, dollar);  // Sを貫通する縦線
      break;
    }
    case 6: {  // ❤️（赤い塗りつぶしハート・左右対称）
      uint16_t heart = lightBright(RED);
      GFX.fillCircle(cx - R / 2, cy - R / 4, R / 2, heart);
      GFX.fillCircle(cx + R / 2, cy - R / 4, R / 2, heart);
      for (int i = 0; i < R; i++) {
        int rowW = R * 2 - i * 2;
        if (rowW < 2) break;
        lightFillRect(cx - rowW / 2, cy - R / 4 + i, rowW, 2, heart);
      }
      break;
    }
    default: {  // 🥝（緑の断面：外周やや濃く・中心明るく・種を放射状に）
      const uint16_t outer = (uint16_t)(((70  & 0xF8) << 8) | ((150 & 0xFC) << 3) | (50  >> 3));
      const uint16_t inner = (uint16_t)(((190 & 0xF8) << 8) | ((225 & 0xFC) << 3) | (140 >> 3));
      GFX.fillCircle(cx, cy, R - 1, outer);
      GFX.fillCircle(cx, cy, R - 5, inner);
      for (int i = 0; i < 8; i++) {
        float a = (float)i * 0.7854f;   // 45°刻みで放射状に配置
        int sx = cx + (int)(cosf(a) * (float)(R - 8));
        int sy = cy + (int)(sinf(a) * (float)(R - 8));
        lightFillRect(sx - 1, sy - 1, 2, 2, 0x0000);
      }
      break;
    }
  }
}

// 左右どちらか1リールぶんの描画。centerX/centerYはそのリールの中心（黒目のあった位置）。
// strip[] はそのリール固有の固定絵柄配列（ESLOT_STRIP_L/ESLOT_STRIP_R）。
// 2026-07-23調整：絵柄を等倍で描いたうえで、表示窓（中央±ESLOT_ROW_H＝
// 約2絵柄分の高さ）だけをクリッピングする方式に変更。これにより「中央は
// 完全表示、上下は隣の絵柄が半分だけ見える」窓のぞき込み効果になる。
static void eslotDrawReel(float reelPos, int centerX, int centerY, const uint8_t* strip) {
  int winX = centerX - ESLOT_WIN_HALF_W;
  int winY = centerY - ESLOT_ROW_H;
  int winW = ESLOT_WIN_HALF_W * 2;
  int winH = ESLOT_ROW_H * 2;

  GFX.setClipRect(winX, winY, winW, winH);
  int k0 = (int)floorf(reelPos);
  for (int k = k0 - 1; k <= k0 + 1; k++) {
    float pos = reelPos - (float)k;   // -1.x .. +1.x（0=中央）
    int sym = strip[((k % ESLOT_SYMBOL_COUNT) + ESLOT_SYMBOL_COUNT) % ESLOT_SYMBOL_COUNT];
    int rowY = centerY + (int)roundf(pos * (float)ESLOT_ROW_H);
    eslotDrawSymbol((uint8_t)sym, centerX, rowY);
  }
  GFX.clearClipRect();   // 他の描画に影響しないよう必ず解除する

  // ペイライン（中央ラインの目印）。派手にしすぎず細い2本線に留める。
  const uint16_t lineCol = (uint16_t)(((180 & 0xF8) << 8) | ((180 & 0xFC) << 3) | (180 >> 3));
  lightFillRect(winX, centerY - ESLOT_ROW_H / 2 - 1, winW, 1, lineCol);
  lightFillRect(winX, centerY + ESLOT_ROW_H / 2,     winW, 1, lineCol);
}

// Eye Slotの状態更新＋リール本体の描画だけを行う（背景の白塗りを含まない）。
// 通常Lighting（lightRenderEyeSlot、下記）とSleep Lighting Carousel（目としての再利用）の
// 両方から呼べるよう、状態遷移・フラッシュ演出・リール描画をここへ一本化した。
// ロジックは従来のlightRenderEyeSlot()と完全に同一（本体をそのまま移動しただけ）。
void eslotUpdateAndDrawReels(bool needsInit) {
  gEyeSlotActive = true;   // このフレームでEye Slotが目として採用されたことを他関数へ伝える
  unsigned long now = millis();

  if (needsInit) {
    eslotState = ESLOT_ST_ACCEL;
    eslotStateAt = now;
    eslotStateDur = 500 + (unsigned long)(eslotRand01() * 300.0f);
  }

  // ── 状態遷移：加速→全開→左減速→間→右減速→結果表示→(最初へ戻る) ──
  switch (eslotState) {
    case ESLOT_ST_ACCEL: {
      float p = (float)(now - eslotStateAt) / (float)eslotStateDur; if (p > 1.0f) p = 1.0f;
      float sp = ESLOT_MAX_SPEED * p;
      eslotReelPos[0] += sp;
      eslotReelPos[1] += sp;
      if (p >= 1.0f) {
        eslotState = ESLOT_ST_SPIN;
        eslotStateAt = now;
        eslotStateDur = 900 + (unsigned long)(eslotRand01() * 900.0f);
      }
      break;
    }
    case ESLOT_ST_SPIN: {
      eslotReelPos[0] += ESLOT_MAX_SPEED;
      eslotReelPos[1] += ESLOT_MAX_SPEED;
      if (now - eslotStateAt >= eslotStateDur) {
        eslotDecelFrom[0] = eslotReelPos[0];
        eslotDecelAt[0]   = now;
        eslotDecelDur[0]  = 650 + (unsigned long)(eslotRand01() * 250.0f);
        eslotDecelTo[0]   = ceilf(eslotReelPos[0]) + (float)(2 + (eslotRand() % 3));
        eslotState = ESLOT_ST_DECEL_L;
        eslotStateAt = now;
      }
      break;
    }
    case ESLOT_ST_DECEL_L: {
      eslotReelPos[1] += ESLOT_MAX_SPEED;   // 右は全開のまま回り続ける
      float p = (float)(now - eslotDecelAt[0]) / (float)eslotDecelDur[0]; if (p > 1.0f) p = 1.0f;
      float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);   // イーズアウト（3乗）
      eslotReelPos[0] = eslotDecelFrom[0] + (eslotDecelTo[0] - eslotDecelFrom[0]) * e;
      if (p >= 1.0f) {
        eslotReelPos[0] = eslotDecelTo[0];   // 整数値へ正確に吸着
        eslotState = ESLOT_ST_GAP;
        eslotStateAt = now;
        eslotStateDur = 350 + (unsigned long)(eslotRand01() * 250.0f);
      }
      break;
    }
    case ESLOT_ST_GAP: {
      eslotReelPos[1] += ESLOT_MAX_SPEED;   // 右はまだ回り続け、期待感の間を作る
      if (now - eslotStateAt >= eslotStateDur) {
        eslotDecelFrom[1] = eslotReelPos[1];
        eslotDecelAt[1]   = now;
        eslotDecelDur[1]  = 650 + (unsigned long)(eslotRand01() * 250.0f);
        eslotDecelTo[1]   = ceilf(eslotReelPos[1]) + (float)(2 + (eslotRand() % 3));
        eslotState = ESLOT_ST_DECEL_R;
        eslotStateAt = now;
      }
      break;
    }
    case ESLOT_ST_DECEL_R: {
      float p = (float)(now - eslotDecelAt[1]) / (float)eslotDecelDur[1]; if (p > 1.0f) p = 1.0f;
      float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
      eslotReelPos[1] = eslotDecelFrom[1] + (eslotDecelTo[1] - eslotDecelFrom[1]) * e;
      if (p >= 1.0f) {
        eslotReelPos[1] = eslotDecelTo[1];
        int kL = (int)roundf(eslotReelPos[0]);
        int kR = (int)roundf(eslotReelPos[1]);
        int symL = ESLOT_STRIP_L[((kL % ESLOT_SYMBOL_COUNT) + ESLOT_SYMBOL_COUNT) % ESLOT_SYMBOL_COUNT];
        int symR = ESLOT_STRIP_R[((kR % ESLOT_SYMBOL_COUNT) + ESLOT_SYMBOL_COUNT) % ESLOT_SYMBOL_COUNT];
        eslotMatch = (symL == symR);
        eslotResultAt = now;
        eslotState = ESLOT_ST_RESULT;
        eslotStateAt = now;
        eslotStateDur = 1600 + (unsigned long)(eslotRand01() * 900.0f);
      }
      break;
    }
    case ESLOT_ST_RESULT: {
      if (now - eslotStateAt >= eslotStateDur) {
        eslotState = ESLOT_ST_ACCEL;
        eslotStateAt = now;
        eslotStateDur = 500 + (unsigned long)(eslotRand01() * 300.0f);
      }
      break;
    }
  }

  eslotDrawReelsFrame();
}

// リール本体（＋揃った時の結果フラッシュ）を「現在の状態のまま」描くだけの関数。
// 状態遷移・アニメーション更新（eslotState/eslotReelPos等）は一切行わない。
// 2026-08-07追加：eslotUpdateAndDrawReels()末尾の描画部分をそのままこちらへ移設し、
// eslotUpdateAndDrawReels()側からは本関数を呼ぶだけにした（描画コードの複製はしていない）。
// 用途は2つ：
//   ① eslotUpdateAndDrawReels()から呼ばれる通常経路（状態更新の直後に1回）
//   ② Eye Slotが背景として選ばれている状態でVisualizerが全画面描画を行った直後、
//      統合描画パイプライン（sceneComposeAndPush、後述）から「目だけ」を最前面へ
//      描き直すための再呼び出し。状態を進めないため、1フレーム内で②が余分に
//      呼ばれてもリールの回転速度やフラッシュのタイミングは変化しない。
void eslotDrawReelsFrame() {
  unsigned long now = millis();

  // ── 揃った場合だけ、結果表示の最初の一瞬だけ控えめなフラッシュを入れる ──
  if (eslotState == ESLOT_ST_RESULT && eslotMatch) {
    unsigned long since = now - eslotResultAt;
    if (since < 700) {
      float t = (float)since / 700.0f;
      float pulse = sinf(t * 3.14159f);   // 0→1→0 と一度だけ明滅
      if (pulse > 0.05f) {
        // レインボー風に色相を回しつつ、両目の周りへ薄いリングを一瞬だけ重ねる
        float hue = t * 6.0f;
        uint8_t seg = (uint8_t)hue % 6;
        float   f   = hue - (float)((int)hue);
        uint8_t rC, gC, bC;
        switch (seg) {
          case 0: rC=255; gC=(uint8_t)(255*f);       bC=0;   break;
          case 1: rC=(uint8_t)(255*(1-f)); gC=255;    bC=0;   break;
          case 2: rC=0;   gC=255;    bC=(uint8_t)(255*f);     break;
          case 3: rC=0;   gC=(uint8_t)(255*(1-f)); bC=255;    break;
          case 4: rC=(uint8_t)(255*f); gC=0; bC=255;          break;
          default: rC=255; gC=0; bC=(uint8_t)(255*(1-f));     break;
        }
        uint16_t rainbow = (uint16_t)(((rC & 0xF8) << 8) | ((gC & 0xFC) << 3) | (bC >> 3));
        int lx = 90 + eyeOffsetX, ly = 90 + eyeOffsetY, rx = 230 + eyeOffsetX, ry = 90 + eyeOffsetY;
        int ringR = 26;
        lightFillRect(lx - ringR, ly - 1, ringR * 2, 2, rainbow);
        lightFillRect(rx - ringR, ry - 1, ringR * 2, 2, rainbow);
      }
    }
  }

  // ── リール本体：黒目のあった位置（eyeOffsetX/Yを反映）を中心に描く ──
  eslotDrawReel(eslotReelPos[0], 90  + eyeOffsetX, 90 + eyeOffsetY, ESLOT_STRIP_L);
  eslotDrawReel(eslotReelPos[1], 230 + eyeOffsetX, 90 + eyeOffsetY, ESLOT_STRIP_R);
}

// リール窓（左右2枚）の背後だけを不透明な白で塗る。
// 2026-08-07追加：もともとSleep Lighting Carousel（sleepComposeEyeLightFrame、後述）が
// 「窓の背後は背景Lighting／Brightnessのテーマに関わらず常に不透明な白にする」ために
// 個別に持っていた処理（GFX.fillRectを直接2回呼ぶだけ）をそのままこちらへ切り出し、
// Sleep Carousel側もこの関数を呼ぶよう統一した（実装の重複をなくし、Sleep Carousel発の
// 既存ロジックを再利用する）。lightFillRect（Framework共通Brightness適用）ではなく
// あえてGFX.fillRectを使う点もSleep Carousel側の元実装を踏襲している＝Eye Slotの窓は
// 常にBrightnessの影響を受けない不透明な白、という既存仕様を変えていない。
//
// 用途：
//   ① Sleep Lighting Carousel（sleepComposeEyeLightFrame）－ 従来どおり
//   ② 通常LightingでEye Slot選択中、Visualizerが全画面描画した直後の再描画
//     （sceneComposeAndPush、後述）－ Visualizerの色がリールの絵柄の隙間から
//     透けて見えないよう、絵柄を描く前に窓を不透明化する。
static inline void eslotFillWindowsWhite() {
  int lx = 90 + eyeOffsetX, rx = 230 + eyeOffsetX, cy = 90 + eyeOffsetY;
  GFX.fillRect(lx - ESLOT_WIN_HALF_W, cy - ESLOT_ROW_H, ESLOT_WIN_HALF_W * 2, ESLOT_ROW_H * 2, WHITE);
  GFX.fillRect(rx - ESLOT_WIN_HALF_W, cy - ESLOT_ROW_H, ESLOT_WIN_HALF_W * 2, ESLOT_ROW_H * 2, WHITE);
}

void lightRenderEyeSlot(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  // ── 背景：通常の顔と同じ白背景を毎フレーム塗り直す（鼻・口・まゆ毛はLayer2が描く）──
  lightFillRect(0, ESLOT_TOP, 320, 240 - ESLOT_TOP, WHITE);
  eslotUpdateAndDrawReels(needsInit);
}

// ============================================================================
// Lighting #8 : Classic Race（1970年代後半風トップビューレースデモ）（v1.7）
//
// ■ コンセプト
//   TAITO「SPEED RACE」(1978) 程度の超レトロな雰囲気を狙った、真上視点
//   （トップビュー）のレースデモ。Retro Race（疑似3D視点）とは方向性を
//   完全に分け、遠近感・地平線・道幅の変化を一切使わない。プレイヤー操作・
//   ゲームオーバー・スコア・周回数・クラッシュ・爆発は無く、ただ永遠に
//   走り続けるだけの「眺めて楽しむ」スクリーンセーバー。情報量を増やさず、
//   道路・自車・敵車・最小限の装飾だけのミニマル構成に留める。
//
// ■ 視点・道路生成方法
//   道路は固定幅（CRACE_ROAD_HW）。中心Xは「スクロール量に応じたsin波」＋
//   「時間経過でゆっくり変化する振幅(crCurveSmooth)」で連続的に蛇行させる。
//   区間ごとに独立抽選するSky Raid方式ではなく、1本の連続した滑らかな
//   カーブ（Retro Raceの疑似3D遠近ではなく、真上視点なのでrowごとの拡幅も
//   無い）として生成することで、常に自然な一本道に見える。
//
// ■ AI走行方法
//   ・自車(crPlayerX)は普段は車線中央付近をゆったり漂い、前方の危険域
//     （CRACE_PLAYER_Y手前）に敵車が近づくと、敵ごとに決めた回避側へ
//     軽く車線変更する（Retro Raceの回避ロジックと同じ考え方）。
//   ・敵車(crEnemyY/Lane[])は上から出現し、各自の速度で下降。画面外へ
//     抜けたら車線・速度・色を再抽選して再出現する（衝突判定なし＝
//     安全運転。ぶつかる/クラッシュする概念自体を持たない）。
//
// ■ かりポムの黒目
//   Retro Raceと同じく setEyeDirection() は使わず eyeOffsetX/Y を直接
//   更新する。「道路の先」ではなく「次のコーナー」を見せるため、自車の
//   少し先（CRACE_EYE_LOOKAHEAD_PX）の位置で道路がどちらへ曲がるかを
//   先読みして方向だけを取り出し、Sky Raid調整時の教訓を踏まえて
//   非常に小さい範囲（±CRACE_EYE_MAX_PX）に収め、専用の緩やかな
//   平滑化フィルタ(crEyeXf)を通す。Y方向は動かさない（常に0＝落ち着いた
//   目線）。
//
// ■ 共通ルール遵守
//   ・背景の描画は lightFillRect を使う → Framework共通Brightnessが自動で効く
//   ・上端48px（情報パネル）には描かない（CRACE_TOP=48でクリップ）
//   ・顔はコンポジタが最前面に描く
//   ・動的メモリ確保なし。すべて固定長の静的配列／スカラー変数のみ
// ============================================================================
#define CRACE_TOP          48
#define CRACE_ROW_H        4                  // 帯の高さ（低解像度感を出す）
#define CRACE_ROAD_HW      75                 // 道路の半幅(px)。トップビューなので固定
#define CRACE_PLAYER_Y     206                // 自車のY座標（画面下側に常時表示）
#define CRACE_MAX_ENEMIES  3
#define CRACE_EYE_MAX_PX   5                  // 黒目の可動範囲（Sky Raid調整と同じく非常に小さく）
#define CRACE_EYE_LOOKAHEAD_PX 70.0f          // 「次のコーナー」を見るための先読み距離(px)

static bool     crReady = false;
static uint32_t crRng   = 0x8B6A5CDEu;

static float crCurveSmooth = 0.0f;   // 蛇行の強さ（0..1程度）
static float crCurveTarget = 0.0f;
static unsigned long crCurveChangeAt = 0;

static float crPlayerX       = 0.0f;   // 自車の車線内位置(-1〜+1)
static float crPlayerXTarget = 0.0f;

static float crSpeed         = 1.0f;
static float crSpeedTarget   = 1.0f;
static unsigned long crSpeedChangeAt = 0;

static float crScrollZ = 0.0f;
static float crEyeXf   = 0.0f;   // 黒目専用の追加平滑化

static float   crEnemyY[CRACE_MAX_ENEMIES];
static float   crEnemyLane[CRACE_MAX_ENEMIES];      // -1〜+1
static float   crEnemySpeedMul[CRACE_MAX_ENEMIES];
static int8_t  crEnemyAvoidSide[CRACE_MAX_ENEMIES];
static uint8_t crEnemyColorIdx[CRACE_MAX_ENEMIES];

static float crOilY   = -9999.0f;   // オイル染み（1つだけ・時々流れる）
static float crOilLane = 0.0f;
static float crCheckerY = -9999.0f; // チェッカーライン（1本だけ・時々流れる）

static const uint8_t CRACE_ENEMY_RGB[3][3] = { {60,110,230}, {235,200,40}, {240,240,240} };
static uint16_t crEnemyCol[3];

static inline uint32_t crRand()   { crRng = crRng * 1664525u + 1013904223u; return crRng; }
static inline float    crRand01() { return (float)(crRand() & 0xFFFF) / 65535.0f; }

// 道路中心Xを求める：連続sin波（区間独立抽選ではなく、常に一本道として繋がる）。
static inline float craceRoadCenterX(float wy) {
  return 160.0f + sinf(wy * 0.006f) * (crCurveSmooth * 34.0f);
}

static void craceSpawnEnemy(int i, bool initialSpread) {
  crEnemyY[i]         = initialSpread ? (float)CRACE_TOP - crRand01() * 260.0f
                                       : (float)CRACE_TOP - (20.0f + crRand01() * 170.0f);
  crEnemyLane[i]       = (crRand01() * 2.0f - 1.0f) * 0.6f;
  crEnemySpeedMul[i]   = 0.85f + crRand01() * 0.35f;
  crEnemyAvoidSide[i]  = (crRand() & 1u) ? 1 : -1;
  crEnemyColorIdx[i]   = (uint8_t)(crRand() % 3);
}

void buildClassicRaceTable() {
  for (int i = 0; i < 3; i++) {
    uint8_t r = CRACE_ENEMY_RGB[i][0], g = CRACE_ENEMY_RGB[i][1], b = CRACE_ENEMY_RGB[i][2];
    crEnemyCol[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  crCurveTarget = crCurveSmooth = 0.0f;
  crPlayerX = crPlayerXTarget = 0.0f;
  crSpeed = crSpeedTarget = 1.0f;
  crScrollZ = 0.0f;
  crOilY = -(300.0f + crRand01() * 400.0f);
  crOilLane = (crRand01() * 2.0f - 1.0f) * 0.6f;
  crCheckerY = -(900.0f + crRand01() * 1100.0f);
  for (int i = 0; i < CRACE_MAX_ENEMIES; i++) craceSpawnEnemy(i, true);
  crReady = true;
}

static void craceDrawPlayerCar(int cx, int cy, uint16_t col) {
  lightFillRect(cx - 6, cy - 14, 12, 22, col);        // 車体
  lightFillRect(cx - 9, cy - 3,  18, 5,  col);         // ウイング/サイドポッド
  lightFillRect(cx - 3, cy - 16, 6,  5,  0x0000);      // コックピット
  lightFillRect(cx - 7, cy - 16, 3,  3,  0x0000);      // タイヤ×4（簡易）
  lightFillRect(cx + 4, cy - 16, 3,  3,  0x0000);
  lightFillRect(cx - 7, cy + 6,  3,  3,  0x0000);
  lightFillRect(cx + 4, cy + 6,  3,  3,  0x0000);
}

static void craceDrawEnemyCar(int cx, int cy, uint16_t col) {
  lightFillRect(cx - 6, cy - 10, 12, 18, col);
  lightFillRect(cx - 3, cy - 11, 6,  4,  0x0000);
}

void lightRenderClassicRace(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!crReady) buildClassicRaceTable();
  unsigned long now = millis();
  if (needsInit) {
    crCurveChangeAt = 0;
    crSpeedChangeAt = 0;
    crEyeXf = 0.0f;
  }

  // ── 蛇行の強さを数秒おきにゆるやかに変更（直線に近い区間も混ぜる）──
  if (now >= crCurveChangeAt) {
    crCurveTarget = 0.25f + crRand01() * 0.75f;
    crCurveChangeAt = now + 4000 + (unsigned long)(crRand01() * 5000.0f);
  }
  crCurveSmooth += (crCurveTarget - crCurveSmooth) * 0.015f;

  // ── 走行速度もゆるやかに変化。音楽が鳴っていると少し加速 ──
  if (now >= crSpeedChangeAt) {
    crSpeedTarget = 0.80f + crRand01() * 0.5f;
    crSpeedChangeAt = now + 2500 + (unsigned long)(crRand01() * 3500.0f);
  }
  float audioBoost = 1.0f + gViz.level * 0.30f;
  crSpeed += ((crSpeedTarget * audioBoost) - crSpeed) * 0.03f;
  if (crSpeed < 0.35f) crSpeed = 0.35f;
  crScrollZ += crSpeed * 3.0f;

  // ── 自動操舵：接近する敵車を避けつつ、普段は車線中央をゆったり漂う ──
  int   dangerIdx = -1;
  float dangerY    = -1.0e9f;
  for (int i = 0; i < CRACE_MAX_ENEMIES; i++) {
    if (crEnemyY[i] < CRACE_PLAYER_Y - 90.0f || crEnemyY[i] > CRACE_PLAYER_Y - 8.0f) continue;
    if (fabsf(crEnemyLane[i] - crPlayerX) > 0.55f) continue;
    if (crEnemyY[i] > dangerY) { dangerY = crEnemyY[i]; dangerIdx = i; }
  }
  if (dangerIdx >= 0) {
    crPlayerXTarget = constrain(crPlayerX + (float)crEnemyAvoidSide[dangerIdx] * 0.55f, -0.85f, 0.85f);
  } else {
    float wander = sinf((float)now * 0.0006f) * 0.20f;
    crPlayerXTarget = constrain(wander, -0.5f, 0.5f);
  }
  crPlayerX += (crPlayerXTarget - crPlayerX) * 0.03f;

  // ── かりポムの黒目：道路の先ではなく「次のコーナー」を、非常に小さく落ち着いて見る ──
  float wyAhead = crScrollZ + (float)(CRACE_PLAYER_Y - CRACE_TOP) + CRACE_EYE_LOOKAHEAD_PX;
  float nextCornerDir = sinf(wyAhead * 0.006f) * crCurveSmooth;   // -1..1程度。符号=方向
  float eyeTargetX = constrain(nextCornerDir * (float)CRACE_EYE_MAX_PX,
                                (float)-CRACE_EYE_MAX_PX, (float)CRACE_EYE_MAX_PX);
  crEyeXf += (eyeTargetX - crEyeXf) * 0.02f;   // 専用のゆっくりしたフィルタ（Sky Raid調整と同じ考え方）
  eyeOffsetX = constrain((int)lroundf(crEyeXf), -CRACE_EYE_MAX_PX, CRACE_EYE_MAX_PX);
  eyeOffsetY = 0;   // Y方向は動かさない（落ち着いた視線）

  // ── 道路（画面中央を固定幅のまま、なめらかに蛇行）──
  const uint16_t GRASS_A = (uint16_t)(((55  & 0xF8) << 8) | ((150 & 0xFC) << 3) | (65  >> 3));
  const uint16_t GRASS_B = (uint16_t)(((45  & 0xF8) << 8) | ((135 & 0xFC) << 3) | (55  >> 3));
  const uint16_t ROAD_A  = (uint16_t)(((25  & 0xF8) << 8) | ((25  & 0xFC) << 3) | (28  >> 3));
  const uint16_t ROAD_B  = (uint16_t)(((20  & 0xF8) << 8) | ((20  & 0xFC) << 3) | (22  >> 3));
  const uint16_t EDGE_W  = (uint16_t)(((245 & 0xF8) << 8) | ((245 & 0xFC) << 3) | (240 >> 3));
  const uint16_t OIL_COL = (uint16_t)(((15  & 0xF8) << 8) | ((15  & 0xFC) << 3) | (18  >> 3));

  for (int y = CRACE_TOP; y < 240; y += CRACE_ROW_H) {
    int rowH = (240 - y < CRACE_ROW_H) ? (240 - y) : CRACE_ROW_H;
    float wy = crScrollZ + (float)(y - CRACE_TOP);
    float centerX = craceRoadCenterX(wy);
    int cxL = (int)(centerX - CRACE_ROAD_HW);
    int cxR = (int)(centerX + CRACE_ROAD_HW);

    long stripeIdx = (long)(wy / 10.0f);

    // 芝生（縞・スクロールで前進感を出す。装飾はここまでに留める＝情報量を増やさない）
    uint16_t grass = (stripeIdx & 1) ? GRASS_A : GRASS_B;
    if (cxL > 0)   lightFillRect(0, y, cxL, rowH, grass);
    if (cxR < 320) lightFillRect(cxR, y, 320 - cxR, rowH, grass);

    // 白い路肩
    lightFillRect(cxL, y, 3, rowH, EDGE_W);
    lightFillRect(cxR - 3, y, 3, rowH, EDGE_W);

    // アスファルト（わずかな2トーンで「路面色の変化」を兼ねる）
    uint16_t road = (stripeIdx & 1) ? ROAD_A : ROAD_B;
    int roadW = (cxR - 3) - (cxL + 3);
    if (roadW > 0) lightFillRect(cxL + 3, y, roadW, rowH, road);

    // 白いセンターライン（破線）
    if (((stripeIdx / 2) & 1) == 0) {
      lightFillRect((int)centerX - 2, y, 4, rowH, EDGE_W);
    }

    // チェッカーライン（時々だけ、道路幅ぶんの市松模様を横断させる）
    if (wy >= crCheckerY && wy < crCheckerY + 10.0f) {
      int cxx = cxL + 3;
      int cw = 8;
      int idx2 = ((cxL / cw) + (y / CRACE_ROW_H)) & 1;
      while (cxx < cxR - 3) {
        int w2 = (cxx + cw > cxR - 3) ? (cxR - 3 - cxx) : cw;
        lightFillRect(cxx, y, w2, rowH, (idx2 & 1) ? 0x0000 : EDGE_W);
        idx2++;
        cxx += cw;
      }
    }

    // オイル染み（時々だけ、道路上に小さな黒っぽい染みが流れる）
    if (wy >= crOilY && wy < crOilY + 14.0f) {
      float oilCx = centerX + crOilLane * (CRACE_ROAD_HW - 14.0f);
      float d = wy - crOilY - 7.0f;
      int halfW = (int)(7.0f - fabsf(d) * 0.7f);
      if (halfW > 0) lightFillRect((int)oilCx - halfW, y, halfW * 2, rowH, OIL_COL);
    }
  }

  // チェッカー/オイルの再抽選（画面を抜けたら次の出現までランダムな間隔を空ける）
  if (crCheckerY > 260.0f) crCheckerY = crScrollZ + 900.0f + crRand01() * 1400.0f;
  if (crOilY     > 260.0f) { crOilY = crScrollZ + 300.0f + crRand01() * 500.0f; crOilLane = (crRand01() * 2.0f - 1.0f) * 0.6f; }

  // ── 敵車：下降のみ（横方向の細かな揺れは持たせない＝ミニマル）。画面外で再出現 ──
  for (int i = 0; i < CRACE_MAX_ENEMIES; i++) {
    crEnemyY[i] += (1.2f + crEnemySpeedMul[i] * 1.4f) * crSpeed;
    if (crEnemyY[i] > 255.0f) { craceSpawnEnemy(i, false); continue; }
    if (crEnemyY[i] < (float)CRACE_TOP) continue;
    float wyE = crScrollZ + (crEnemyY[i] - (float)CRACE_TOP);
    float centerAtE = craceRoadCenterX(wyE);
    int ex = (int)(centerAtE + crEnemyLane[i] * (CRACE_ROAD_HW - 14.0f));
    craceDrawEnemyCar(ex, (int)crEnemyY[i], crEnemyCol[crEnemyColorIdx[i]]);
  }

  // ── 自車（最前面）──
  float wyP = crScrollZ + (float)(CRACE_PLAYER_Y - CRACE_TOP);
  float centerAtP = craceRoadCenterX(wyP);
  int px = (int)(centerAtP + crPlayerX * (CRACE_ROAD_HW - 14.0f));
  const uint16_t PLAYER_COL = (uint16_t)(((230 & 0xF8) << 8) | ((30 & 0xFC) << 3) | (30 >> 3));
  craceDrawPlayerCar(px, CRACE_PLAYER_Y, PLAYER_COL);
}

// ============================================================================
// Lighting #9 : Asteroid Field（ワイヤーフレーム隕石が漂う宇宙空間演出）（v1.7）
//
// ■ コンセプト
//   1980年前後のベクターゲーム（Asteroids等）の画面を眺めているような、
//   癒し・レトロ・見飽きない、を重視したアート系スクリーンセーバー。
//   ゲームではないため、自機・弾・スコア・衝突判定は一切持たない。
//   10〜15個の不規則なワイヤーフレーム多角形が、それぞれ異なる方向へ
//   ゆっくり漂う「小さな宇宙空間」として画面全体に配置する
//   （全隕石を同じ方向へ流さない＝ユーザー提案の設計方針）。
//
// ■ 隕石の生成方法
//   各隕石は、頂点数(5〜9)・各頂点の半径比率(0.6〜1.0)を出現時に一度だけ
//   乱数で決め、以降は同じ形のまま自転させる（毎フレーム再生成しない＝
//   軽量化）。位置・速度・回転速度・色（ネオンピンク/グリーン/イエロー）も
//   出現時に決定し、以後はその値で等速直線運動＋等速回転するだけの
//   単純な物理なので計算負荷が低い。
//
// ■ 動き
//   2026-07-25調整: 「宇宙空間を慣性で飛んでいる」躍動感を優先し、移動・回転
//   速度とも初版の約2.5倍に引き上げ（シューティングゲームほど速くはしない
//   範囲・Lighting Galleryとして見苦しくならない範囲を目安に調整）。
//   画面外（上下左右）へ完全に出たら反対側から現れるラップアラウンド。
//
// ■ 星（背景装飾）
//   本数を少なく抑えた固定位置の点のみ。隕石より目立たないよう暗めの
//   グレーで描画し、主役はあくまで隕石とする。
//
// ■ かりポムの顔・黒目
//   2026-07-25追記：自機（KariPomが操縦する小さな機体）を追加し、黒目は
//   その自機の現在位置を追従するように変更した。岩石・星の生成/速度/色/
//   遠近感には一切影響しない（自機は既存ループの後に追加描画するのみ）。
//   黒目の追従フィルタは自機自身の操舵フィルタより速い係数を使い、
//   「黒目の方が少し機敏」という要望どおりの体感差を出している。
//   モード終了時のeyeOffsetX/Yリセットは updateScreenEffects() 側の
//   共通処理（bgMode振り分け直前）で行うため、本関数側では扱わない。
//
// ■ パフォーマンス配慮
//   ・動的メモリ確保なし（すべて固定長静的配列。自機の状態もスカラー変数のみ）
//   ・頂点数は最大9に制限し、1隕石あたりの描画は9本の直線のみ
//   ・毎フレームの黒塗りつぶしは1回のfillRectのみ（個別クリアより軽い）
//   ・星は18個の固定ドットのみで、複雑な演算を行わない
//   ・自機の描画は3本の直線のみ（三角形のワイヤーフレーム）
// ============================================================================
#define ASTR_TOP         48
#define ASTR_COUNT       13     // 10〜15個の中央値
#define ASTR_MAX_VERTS   9
#define ASTR_STAR_COUNT  16     // 主役を隕石にするため控えめ

// 自機（岩石とは独立した状態。黒目追従の対象）
#define ASTR_CRAFT_MARGIN  16.0f   // 自機の目標点を画面端から離すマージン
#define ASTR_CRAFT_SPEED   0.30f   // 自機の巡航速度(px/フレーム)。岩石の最遅速度より控えめ
#define ASTR_EYE_MAX_X     12.0f   // 黒目追従の可動域(px、EYE_SHIFT_PIXELS=14以内)
#define ASTR_EYE_MAX_Y     10.0f

// 弾（演出専用のベクターミサイル。2026-07-25追記。当たり判定・破壊・爆発は無し）
#define ASTR_BULLET_COUNT           2      // 同時に存在できる弾数（固定スロット）
#define ASTR_BULLET_SPEED           4.0f   // px/フレーム（自機・岩石よりはっきり速い）
#define ASTR_BULLET_RANGE           55.0f  // 発射地点からこの距離を超えたら消滅
#define ASTR_BULLET_DETECT_R        60.0f  // この距離以内に岩石が来たら優先発射
#define ASTR_BULLET_MIN_INTERVAL_MS 900     // 優先発射も含めた最短発射間隔（連射防止）

static bool     astrReady = false;
static uint32_t astrRng   = 0x2F19E7A3u;

static float   astrX[ASTR_COUNT], astrY[ASTR_COUNT];
static float   astrVX[ASTR_COUNT], astrVY[ASTR_COUNT];
static float   astrSize[ASTR_COUNT];              // 基準半径(px)
static float   astrAngle[ASTR_COUNT], astrAngVel[ASTR_COUNT];
static uint8_t astrVertCount[ASTR_COUNT];
static float   astrVertR[ASTR_COUNT][ASTR_MAX_VERTS];  // 頂点ごとの半径比率(0.6〜1.0)
static uint16_t astrColor[ASTR_COUNT];

static int16_t astrStarX[ASTR_STAR_COUNT], astrStarY[ASTR_STAR_COUNT];
static uint16_t astrStarCol[ASTR_STAR_COUNT];

// 2026-08-09追加：最背面のカラフルな1ドット星（ギャラクシアン／ムーンクレスタ等の
// 80年代アーケードゲームを思わせる宇宙背景へのオマージュ）。既存の控えめな灰色の
// 星（astrStarX等）や隕石・自機・弾とは別要素で、黒背景の直後・他の要素より必ず
// 先に描く（最背面）。位置・色は起動時に一度だけ決め、以後は固定のまま使い回す。
#define ASTR_CSTAR_COUNT 20
static int16_t  astrCStarX[ASTR_CSTAR_COUNT], astrCStarY[ASTR_CSTAR_COUNT];
static uint16_t astrCStarCol[ASTR_CSTAR_COUNT];

static float astrCraftX, astrCraftY;
static float astrCraftVX = 0.0f, astrCraftVY = 0.0f;
static float astrCraftTargetX, astrCraftTargetY;
static float astrCraftAngle = 0.0f;             // 進行方向（描画の向き）
static unsigned long astrCraftRetargetAt = 0;
static float astrEyeXf = 0.0f, astrEyeYf = 0.0f;  // 黒目専用の平滑化フィルタ

static bool  astrBulletActive[ASTR_BULLET_COUNT];
static float astrBulletX[ASTR_BULLET_COUNT], astrBulletY[ASTR_BULLET_COUNT];
static float astrBulletDX[ASTR_BULLET_COUNT], astrBulletDY[ASTR_BULLET_COUNT];  // 正規化方向
static float astrBulletDist[ASTR_BULLET_COUNT];   // 発射地点からの移動距離
static unsigned long astrNextFireAt = 0;
static unsigned long astrLastFireAt = 0;

static inline uint32_t astrRand()   { astrRng = astrRng * 1664525u + 1013904223u; return astrRng; }
static inline float    astrRand01() { return (float)(astrRand() & 0xFFFF) / 65535.0f; }

// ネオン3色（背景が黒のため、あえて彩度・輝度とも最大級にする）
static const uint8_t ASTR_NEON_RGB[3][3] = {
  { 255, 20, 147 },   // ネオンピンク
  { 57,  255, 20  },  // ネオングリーン
  { 255, 245, 10  },  // ネオンイエロー
};

static void astrSpawn(int i) {
  astrX[i] = astrRand01() * 320.0f;
  astrY[i] = (float)ASTR_TOP + astrRand01() * (240.0f - (float)ASTR_TOP);
  float ang = astrRand01() * 6.2831853f;
  // 2026-07-25: 「慣性で宇宙空間を漂う」躍動感を出すため、移動・回転速度とも
  // 元の約2.5倍に引き上げ（シューティングゲームほど速くはしない範囲で調整）。
  float spd = 0.25f + astrRand01() * 0.55f;         // 慣性で漂う速度感（旧: 0.10〜0.32 → 新: 0.25〜0.80）
  astrVX[i] = cosf(ang) * spd;
  astrVY[i] = sinf(ang) * spd;
  astrSize[i]  = 10.0f + astrRand01() * 16.0f;       // 大きさをランダム化(10〜26px)
  astrAngle[i] = astrRand01() * 6.2831853f;
  astrAngVel[i] = (astrRand01() * 2.0f - 1.0f) * 0.030f;  // 回転速度（旧: ±0.012 → 新: ±0.030）
  if (fabsf(astrAngVel[i]) < 0.008f) astrAngVel[i] = (astrAngVel[i] < 0) ? -0.008f : 0.008f;

  uint8_t vc = 5 + (uint8_t)(astrRand() % 5);        // 5〜9角形
  astrVertCount[i] = vc;
  for (uint8_t v = 0; v < vc; v++) {
    astrVertR[i][v] = 0.6f + astrRand01() * 0.4f;    // 不規則な歪み（円にしない）
  }

  uint8_t ci = (uint8_t)(astrRand() % 3);
  uint8_t r = ASTR_NEON_RGB[ci][0], g = ASTR_NEON_RGB[ci][1], b = ASTR_NEON_RGB[ci][2];
  astrColor[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void buildAsteroidTable() {
  for (int i = 0; i < ASTR_COUNT; i++) astrSpawn(i);

  // 星（控えめ・固定位置。暗めのグレーで隕石より目立たせない）
  const uint16_t STAR_COL_DIM  = (uint16_t)(((90  & 0xF8) << 8) | ((90  & 0xFC) << 3) | (95  >> 3));
  const uint16_t STAR_COL_FAINT = (uint16_t)(((55 & 0xF8) << 8) | ((55  & 0xFC) << 3) | (60  >> 3));
  for (int i = 0; i < ASTR_STAR_COUNT; i++) {
    astrStarX[i] = (int16_t)(astrRand01() * 320.0f);
    astrStarY[i] = (int16_t)((float)ASTR_TOP + astrRand01() * (240.0f - (float)ASTR_TOP));
    astrStarCol[i] = (astrRand() & 1u) ? STAR_COL_DIM : STAR_COL_FAINT;
  }

  // 最背面のカラフルな星（80年代アーケード風。赤・白・黄・緑の4色を基本とする）
  {
    const uint16_t ASTR_CSTAR_PALETTE[4] = { RED, WHITE, YELLOW, GREEN };
    for (int i = 0; i < ASTR_CSTAR_COUNT; i++) {
      astrCStarX[i]   = (int16_t)(astrRand01() * 320.0f);
      astrCStarY[i]   = (int16_t)((float)ASTR_TOP + astrRand01() * (240.0f - (float)ASTR_TOP));
      astrCStarCol[i] = ASTR_CSTAR_PALETTE[astrRand() % 4];
    }
  }

  // 自機の初期化（画面中央付近からスタートし、すぐに新しい目標点を選ぶ）
  astrCraftX = 160.0f;
  astrCraftY = 144.0f;
  astrCraftVX = 0.0f;
  astrCraftVY = 0.0f;
  astrCraftTargetX = astrCraftX;
  astrCraftTargetY = astrCraftY;
  astrCraftAngle = 0.0f;
  astrCraftRetargetAt = 0;
  astrEyeXf = 0.0f;
  astrEyeYf = 0.0f;

  // 弾の初期化（全スロット非アクティブ、発射タイマーはリセット）
  for (int b = 0; b < ASTR_BULLET_COUNT; b++) astrBulletActive[b] = false;
  astrNextFireAt = 0;
  astrLastFireAt = 0;

  astrReady = true;
}

// 自機のワイヤーフレーム（進行方向を向く小さな三角形。岩石とは別色で見分けやすくする）
static const uint16_t ASTR_CRAFT_COL = (uint16_t)(((210 & 0xF8) << 8) | ((240 & 0xFC) << 3) | (255 >> 3));  // 明るいシアン白

// 弾を1発発射する（空きスロットが無ければ何もしない＝弾数は固定上限を超えない）
static void astrFireBullet() {
  for (int b = 0; b < ASTR_BULLET_COUNT; b++) {
    if (astrBulletActive[b]) continue;
    astrBulletActive[b] = true;
    astrBulletX[b] = astrCraftX;
    astrBulletY[b] = astrCraftY;
    astrBulletDX[b] = cosf(astrCraftAngle);
    astrBulletDY[b] = sinf(astrCraftAngle);
    astrBulletDist[b] = 0.0f;
    return;
  }
}

static void astrDrawCraft(int cx, int cy, float angle) {
  const float tipR = 9.0f, backR = 6.0f, spread = 2.4f;
  int tipX = cx + (int)lroundf(cosf(angle) * tipR);
  int tipY = cy + (int)lroundf(sinf(angle) * tipR);
  int blX  = cx + (int)lroundf(cosf(angle + spread) * backR);
  int blY  = cy + (int)lroundf(sinf(angle + spread) * backR);
  int brX  = cx + (int)lroundf(cosf(angle - spread) * backR);
  int brY  = cy + (int)lroundf(sinf(angle - spread) * backR);
  lightDrawLine(tipX, tipY, blX, blY, ASTR_CRAFT_COL);
  lightDrawLine(tipX, tipY, brX, brY, ASTR_CRAFT_COL);
  lightDrawLine(blX,  blY,  brX, brY, ASTR_CRAFT_COL);
}

static void astrDrawWireframe(int i) {
  uint8_t vc = astrVertCount[i];
  float cx = astrX[i], cy = astrY[i], size = astrSize[i], ang = astrAngle[i];
  int px0 = 0, py0 = 0, pxFirst = 0, pyFirst = 0;
  for (uint8_t v = 0; v < vc; v++) {
    float a = ang + (6.2831853f * (float)v / (float)vc);
    float r = size * astrVertR[i][v];
    int px = (int)lroundf(cx + cosf(a) * r);
    int py = (int)lroundf(cy + sinf(a) * r);
    if (v == 0) { pxFirst = px; pyFirst = py; }
    else        { lightDrawLine(px0, py0, px, py, astrColor[i]); }
    px0 = px; py0 = py;
  }
  lightDrawLine(px0, py0, pxFirst, pyFirst, astrColor[i]);   // 最後の頂点→最初の頂点で閉じる
}

void lightRenderAsteroid(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!astrReady) buildAsteroidTable();
  if (needsInit) { /* 特に再初期化は不要（隕石はモード切替をまたいで漂い続けて良い） */ }

  // ── 背景：完全な黒で塗りつぶし（Brightness適用のため lightFillRect を使用）──
  lightFillRect(0, ASTR_TOP, 320, 240 - ASTR_TOP, 0x0000);

  // ── 最背面のカラフルな星（80年代アーケード風。既存の隕石・自機・弾・灰色の星より必ず先に描く）──
  for (int i = 0; i < ASTR_CSTAR_COUNT; i++) {
    lightFillRect(astrCStarX[i], astrCStarY[i], 1, 1, astrCStarCol[i]);
  }

  // ── 星（控えめ・主役ではない）──
  for (int i = 0; i < ASTR_STAR_COUNT; i++) {
    lightFillRect(astrStarX[i], astrStarY[i], 1, 1, astrStarCol[i]);
  }

  // ── 隕石：移動・回転・ラップアラウンド・描画 ──
  for (int i = 0; i < ASTR_COUNT; i++) {
    astrX[i] += astrVX[i];
    astrY[i] += astrVY[i];
    astrAngle[i] += astrAngVel[i];
    if (astrAngle[i] > 6.2831853f) astrAngle[i] -= 6.2831853f;
    if (astrAngle[i] < 0.0f)       astrAngle[i] += 6.2831853f;

    float m = astrSize[i] + 4.0f;
    if (astrX[i] < -m)        astrX[i] = 320.0f + m;
    else if (astrX[i] > 320.0f + m) astrX[i] = -m;
    if (astrY[i] < (float)ASTR_TOP - m)   astrY[i] = 240.0f + m;
    else if (astrY[i] > 240.0f + m)       astrY[i] = (float)ASTR_TOP - m;

    astrDrawWireframe(i);
  }

  // ── 自機：岩石とは独立して、数秒おきに選ぶ目標点へゆるやかに操舵しながら漂う ──
  unsigned long now = millis();
  if (now >= astrCraftRetargetAt) {
    astrCraftTargetX = ASTR_CRAFT_MARGIN + astrRand01() * (320.0f - ASTR_CRAFT_MARGIN * 2.0f);
    astrCraftTargetY = (float)ASTR_TOP + ASTR_CRAFT_MARGIN
                      + astrRand01() * (240.0f - (float)ASTR_TOP - ASTR_CRAFT_MARGIN * 2.0f);
    astrCraftRetargetAt = now + 3000 + (unsigned long)(astrRand01() * 3000.0f);
  }
  {
    float dx = astrCraftTargetX - astrCraftX;
    float dy = astrCraftTargetY - astrCraftY;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > 1.0f) {
      float desiredVX = (dx / dist) * ASTR_CRAFT_SPEED;
      float desiredVY = (dy / dist) * ASTR_CRAFT_SPEED;
      astrCraftVX += (desiredVX - astrCraftVX) * 0.03f;   // 岩石より慣性が強い＝ゆっくり操舵
      astrCraftVY += (desiredVY - astrCraftVY) * 0.03f;
    } else {
      astrCraftVX *= 0.95f;
      astrCraftVY *= 0.95f;
    }
    astrCraftX += astrCraftVX;
    astrCraftY += astrCraftVY;
    if (fabsf(astrCraftVX) > 0.02f || fabsf(astrCraftVY) > 0.02f) {
      astrCraftAngle = atan2f(astrCraftVY, astrCraftVX);
    }
  }
  astrDrawCraft((int)lroundf(astrCraftX), (int)lroundf(astrCraftY), astrCraftAngle);

  // ── 弾：時々自機から発射する演出用ベクターミサイル ──
  // 当たり判定・岩石の破壊・爆発・スコアは一切持たない、純粋な演出。
  // 通常はランダム間隔で発射するが、岩石が自機の近くまで来た瞬間は
  // 「敵を見つけて撃っているように見える」よう優先的に発射する
  // （優先発射も最短発射間隔ASTR_BULLET_MIN_INTERVAL_MSの対象＝連射はしない）。
  {
    bool priorityFired = false;
    if (now - astrLastFireAt >= ASTR_BULLET_MIN_INTERVAL_MS) {
      for (int i = 0; i < ASTR_COUNT; i++) {
        float dx = astrX[i] - astrCraftX;
        float dy = astrY[i] - astrCraftY;
        if (dx * dx + dy * dy <= ASTR_BULLET_DETECT_R * ASTR_BULLET_DETECT_R) {
          astrFireBullet();
          astrLastFireAt = now;
          astrNextFireAt = now + 2500 + (unsigned long)(astrRand01() * 2500.0f);
          priorityFired = true;
          break;
        }
      }
    }
    if (!priorityFired && now >= astrNextFireAt) {
      astrFireBullet();
      astrLastFireAt = now;
      astrNextFireAt = now + 2500 + (unsigned long)(astrRand01() * 2500.0f);
    }

    for (int b = 0; b < ASTR_BULLET_COUNT; b++) {
      if (!astrBulletActive[b]) continue;
      astrBulletX[b] += astrBulletDX[b] * ASTR_BULLET_SPEED;
      astrBulletY[b] += astrBulletDY[b] * ASTR_BULLET_SPEED;
      astrBulletDist[b] += ASTR_BULLET_SPEED;
      if (astrBulletDist[b] >= ASTR_BULLET_RANGE) { astrBulletActive[b] = false; continue; }
      int tailX = (int)lroundf(astrBulletX[b] - astrBulletDX[b] * 6.0f);
      int tailY = (int)lroundf(astrBulletY[b] - astrBulletDY[b] * 6.0f);
      lightDrawLine(tailX, tailY, (int)lroundf(astrBulletX[b]), (int)lroundf(astrBulletY[b]), ASTR_CRAFT_COL);
    }
  }

  // ── かりポムの黒目：自機の現在位置を追従する（隕石は追わない）──
  // 自機自身の操舵フィルタ(0.03)より速い係数(0.07)を使い、「黒目の方が少し機敏」という
  // 要望どおりの体感差を出す。可動域はEYE_SHIFT_PIXELS(=14)以内に収めている。
  {
    float craftNormX = constrain((astrCraftX - 160.0f) / 160.0f, -1.0f, 1.0f);
    float craftNormY = constrain((astrCraftY - 144.0f) / 96.0f,  -1.0f, 1.0f);
    float eyeTargetX = craftNormX * ASTR_EYE_MAX_X;
    float eyeTargetY = craftNormY * ASTR_EYE_MAX_Y;
    astrEyeXf += (eyeTargetX - astrEyeXf) * 0.07f;
    astrEyeYf += (eyeTargetY - astrEyeYf) * 0.07f;
    eyeOffsetX = constrain((int)lroundf(astrEyeXf), -(int)ASTR_EYE_MAX_X, (int)ASTR_EYE_MAX_X);
    eyeOffsetY = constrain((int)lroundf(astrEyeYf), -(int)ASTR_EYE_MAX_Y, (int)ASTR_EYE_MAX_Y);
  }
}

// ============================================================================
// Lighting #10 : Tempest Tunnel（ワイヤーフレームトンネルのベクターアート演出）（v1.7）
//
// ■ コンセプト
//   1981年のアーケードゲーム『Tempest』のワイヤーフレームトンネルだけを
//   モチーフにした、ゲームではないベクターアート系スクリーンセーバー。
//   敵・自機・弾・スコアなどのゲーム要素は一切持たない。「Tempestを知らない
//   人が見てもかっこいい」ことを最優先にしつつ、多角形リング＋放射状の
//   スポーク線という構成で、知っている人には一目でオマージュと分かる程度の
//   要素だけを残す。
//
// ■ 描画方式（トンネルの作り方）
//   TUN_RINGS本の多角形リングを常に同時に存在させ、各リングへ「深度」を
//   割り当てる。深度は tunFlowPhase（連続的に増加する値）を土台に
//   d[i] = (tunFlowPhase + i) mod TUN_RINGS として計算し、深度が大きいほど
//   半径を小さく（画面中央に近く）、小さいほど半径を大きく（画面端に近く）
//   することで「奥へ吸い込まれていく」遠近感を表現する。tunFlowPhaseが
//   増加し続けることで各リングの深度が連続的に進み、TUN_RINGS周期で
//   自然にループする（＝リングを使い回すだけで無限にトンネルが続いて見える。
//   動的メモリ確保も配列の伸び縮みも不要）。
//   半径は三乗カーブ（(1-t)^3）で圧縮し、Tempest特有の「中央への吸い込まれ」
//   を強調している。隣接する深度のリング同士は各頂点を線でつなぎ、トンネルの
//   「壁」を表現する（深度が周回して戻る境界の1組だけは接続をスキップし、
//   中心から端へ一瞬で飛ぶような不自然な線が出ないようにしている）。
//
// ■ アニメーション（すべて経過時間dtベースで駆動）
//   ・回転：tunRotAngleがdtに比例して増加し、トンネル全体をゆっくり回転させる。
//     さらにリングごとに微小なひねり（TUN_RING_TWIST）を加えることで、
//     放射状のスポーク線がまっすぐでなく緩やかならせん状に見え、
//     「未来的な万華鏡」のような印象を強める。
//   ・脈動：tunPulsePhaseによるsin波で全リング半径を±6%程度ゆっくり
//     拡縮させ、呼吸するような柔らかい躍動感を出す。
//   ・色：tunHueDegが連続的に回転し、各リングにも深度に応じた色相差を
//     付けることで、ネオンカラーの帯がトンネルの奥行きに沿って
//     グラデーションして見える。
//   本フレームワークの合成周期は約90ms（≒11fps、LIGHT_COMPOSITE_MS参照）
//   であるため、実機の実描画フレームレートはそれに準ずるが、内部の状態は
//   固定フレーム刻みではなく実経過時間(dt=ms)に比例させて更新しているため、
//   フレーム間隔が多少ばらついても速度が変わらず滑らかに見える設計とした。
//
// ■ かりポムの黒目
//   2026-07-25追記：自機（KariPomが操縦する小さな機体）をトンネル手前の
//   外周に追加し、黒目は基本的にその自機の位置を追従する。発光体が最前列
//   間近まで来た瞬間だけ、短時間だけ視線をその発光体へ切り替える（優先度：
//   間近の発光体への一時注視 ＞ 自機追従）。トンネル本体（リング・スポーク・
//   回転・奥への流れ・脈動・色相変化）のロジックには一切影響しない。
//
// ■ 自機・発光体（2026-07-25追記）
//   自機はトンネル手前外周（半径ほぼ固定）を、画面下寄りの弧の範囲内で
//   ゆっくり左右へ操舵する。発光体は固定3体を使い回し、奥(depth=TUN_RINGS)
//   から手前(depth=0)へ連続的に流れて最前列で再び最奥から出現する。
//   自機・発光体とも既存のリング/スポーク描画の後に追加描画するのみで、
//   トンネル本体の生成・アニメーションには一切手を加えていない。
//
// ■ パフォーマンス配慮
//   ・動的メモリ確保なし（すべて固定長静的配列。頂点数は最大16に制限）
//   ・線の描画本数はリング6本×頂点分＋スポーク5組×頂点分（周回1組はスキップ）
//     ＋自機3本＋発光体最大3体×4本 程度に収まるよう調整し、
//     Asteroid Fieldと同程度の負荷に留めている
//   ・色計算はHSV→RGB565の軽量整数演算のみ（浮動小数除算を多用しない）
//   ・発光体は固定3スロットの使い回し（動的な増減なし）
// ============================================================================
#define TUN_TOP        48
#define TUN_RINGS      6
#define TUN_MAX_VERTS  16
#define TUN_MIN_VERTS  8
#define TUN_MIN_R      6.0f
#define TUN_MAX_R      92.0f     // 中心(160,144)から上下48pxの表示領域内に収まる半径
#define TUN_CENTER_X   160
#define TUN_CENTER_Y   144       // (TUN_TOP=48 〜 240) の中央
#define TUN_RING_TWIST 0.12f     // リングごとの追加ひねり(rad)。らせん感を出す
#define TUN_PULSE_AMPL 0.06f     // 脈動の振幅（半径の±6%）
#define TUN_HUE_STEP   24.0f     // 隣接リング間の色相差(度)

// 自機・発光体（2026-07-25追記）
#define TUN_CRAFT_RADIUS      (TUN_MAX_R - 6.0f)   // 自機を置く半径（最前列のすぐ内側）
#define TUN_CRAFT_ARC_CENTER  1.5707963f           // 弧の中心角(rad)＝画面下方向(90°)
#define TUN_CRAFT_ARC_HALF    1.0471976f           // 弧の半幅(rad)＝±60°
#define TUN_ENEMY_COUNT       3
#define TUN_ENEMY_NEAR_T      0.85f   // 「最前列間近」と判定するt(0..1、1が最前列)の閾値
#define TUN_EYE_MAX_X         12.0f   // 黒目可動域(px、EYE_SHIFT_PIXELS=14以内)
#define TUN_EYE_MAX_Y         10.0f

static bool  tunReady    = false;
static uint32_t tunRng   = 0x51A7C33Du;
static uint8_t  tunVerts = 12;          // 8〜16角形の中から起動時に1回だけ決定
static int   tunRotDir   = 1;           // 回転方向（起動時にランダム決定）

static float tunRotAngle   = 0.0f;      // 現在の回転角(rad)
static float tunFlowPhase  = 0.0f;      // 連続的に増加する「奥への流れ」位相（0〜TUN_RINGS換算）
static float tunPulsePhase = 0.0f;
static float tunHueDeg     = 0.0f;
static unsigned long tunLastMs = 0;     // dt計算用（前回フレーム時刻）

// 2026-08-09追加：最背面のカラフルな1ドット星（ギャラクシアン／ムーンクレスタ等の
// 80年代アーケードゲームを思わせる宇宙背景へのオマージュ）。トンネル本体（リング・
// スポーク・自機・発光体・弾）とは別要素で、黒背景の直後・他の要素より必ず先に描く
// （最背面）。位置・色は起動時に一度だけ決め、以後は固定のまま使い回す。
#define TUN_CSTAR_COUNT 20
static int16_t  tunCStarX[TUN_CSTAR_COUNT], tunCStarY[TUN_CSTAR_COUNT];
static uint16_t tunCStarCol[TUN_CSTAR_COUNT];

static const float TUN_ROT_SPEED   = 6.2831853f / 15000.0f;  // 1周 約15秒
static const float TUN_FLOW_SPEED  = (float)TUN_RINGS / 4500.0f;  // 1周期 約4.5秒
static const float TUN_PULSE_SPEED = 6.2831853f / 3200.0f;   // 1周期 約3.2秒
static const float TUN_HUE_SPEED   = 360.0f / 20000.0f;      // 1周 約20秒

// 自機の状態
static float tunCraftAngle       = TUN_CRAFT_ARC_CENTER;  // 現在の角度(rad)
static float tunCraftTargetAngle = TUN_CRAFT_ARC_CENTER;  // 操舵目標角度
static float tunCraftAvoidBias   = 0.0f;                  // 発光体接近時の一時的な角度バイアス
static unsigned long tunCraftRetargetAt = 0;
static const float TUN_CRAFT_STEER_RATE = 0.00035f;  // 目標角度への操舵の速さ(1/ms相当)

// 発光体の状態（固定3スロットの使い回し。0=ひし形の発光体 / 1=三角の追跡体）
static float   tunEnemyDepth[TUN_ENEMY_COUNT];   // TUN_RINGS(奥)〜0(手前)
static float   tunEnemyAngle[TUN_ENEMY_COUNT];
static float   tunEnemySpeed[TUN_ENEMY_COUNT];   // depthを減らす速さ(rings/ms)
static uint8_t tunEnemyType[TUN_ENEMY_COUNT];
static const uint16_t TUN_ENEMY_COL =
    (uint16_t)(((255 & 0xF8) << 8) | ((70 & 0xFC) << 3) | (40 >> 3));  // アラート系ネオン（赤橙）
static const uint16_t TUN_CRAFT_COL =
    (uint16_t)(((220 & 0xF8) << 8) | ((245 & 0xFC) << 3) | (255 >> 3));  // 明るいシアン白

// 黒目の状態
static float tunEyeXf = 0.0f, tunEyeYf = 0.0f;   // 黒目専用の平滑化フィルタ
static int   tunGlanceEnemyIdx = -1;             // 一時的に注視中の発光体（-1=なし＝自機を追従）
static unsigned long tunGlanceUntil = 0;

// 弾（演出専用のベクターミサイル。2026-07-25追記。当たり判定・破壊・爆発は無し）
#define TUN_BULLET_COUNT           2       // 同時に存在できる弾数（固定スロット）
#define TUN_BULLET_SPEED_PX_MS     0.13f   // 半径を縮める速さ(px/ms)。自機半径→中心付近まで約0.6秒
#define TUN_BULLET_DETECT_T        0.55f   // この近さ(t)以上の発光体が現れたら優先発射の対象にする
#define TUN_BULLET_DETECT_ANGLE    0.6f    // 自機との角度差(rad)がこれ未満なら「近い」とみなす
#define TUN_BULLET_MIN_INTERVAL_MS 900      // 優先発射も含めた最短発射間隔（連射防止）

static bool  tunBulletActive[TUN_BULLET_COUNT];
static float tunBulletAngle[TUN_BULLET_COUNT];
static float tunBulletRadius[TUN_BULLET_COUNT];
static unsigned long tunNextFireAt = 0;
static unsigned long tunLastFireAt = 0;

static inline uint32_t tunRand()   { tunRng = tunRng * 1664525u + 1013904223u; return tunRng; }
static inline float    tunRand01() { return (float)(tunRand() & 0xFFFF) / 65535.0f; }

// HSV(彩度・明度とも最大)→RGB565。ネオンカラーを軽量な整数演算で求める。
static uint16_t tunHueToRgb565(float hueDeg) {
  hueDeg = fmodf(hueDeg, 360.0f);
  if (hueDeg < 0.0f) hueDeg += 360.0f;
  float h = hueDeg / 60.0f;
  int   hi = ((int)h) % 6;
  float f = h - (float)(int)h;
  uint8_t q = (uint8_t)(255.0f * (1.0f - f));
  uint8_t t = (uint8_t)(255.0f * f);
  uint8_t r, g, b;
  switch (hi) {
    case 0: r = 255; g = t;   b = 0;   break;
    case 1: r = q;   g = 255; b = 0;   break;
    case 2: r = 0;   g = 255; b = t;   break;
    case 3: r = 0;   g = q;   b = 255; break;
    case 4: r = t;   g = 0;   b = 255; break;
    default: r = 255; g = 0;  b = q;   break;
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void buildTunnelTable() {
  tunVerts  = TUN_MIN_VERTS + (uint8_t)(tunRand() % (TUN_MAX_VERTS - TUN_MIN_VERTS + 1));  // 8〜16角形
  tunRotDir = (tunRand() & 1u) ? 1 : -1;
  tunRotAngle   = tunRand01() * 6.2831853f;
  tunFlowPhase  = tunRand01() * (float)TUN_RINGS;
  tunPulsePhase = tunRand01() * 6.2831853f;
  tunHueDeg     = tunRand01() * 360.0f;
  tunLastMs = 0;

  // 最背面のカラフルな星（80年代アーケード風。赤・白・黄・緑の4色を基本とする）
  {
    const uint16_t TUN_CSTAR_PALETTE[4] = { RED, WHITE, YELLOW, GREEN };
    for (int i = 0; i < TUN_CSTAR_COUNT; i++) {
      tunCStarX[i]   = (int16_t)(tunRand01() * 320.0f);
      tunCStarY[i]   = (int16_t)((float)TUN_TOP + tunRand01() * (240.0f - (float)TUN_TOP));
      tunCStarCol[i] = TUN_CSTAR_PALETTE[tunRand() % 4];
    }
  }

  // 自機の初期化
  tunCraftAngle       = TUN_CRAFT_ARC_CENTER;
  tunCraftTargetAngle = TUN_CRAFT_ARC_CENTER;
  tunCraftAvoidBias   = 0.0f;
  tunCraftRetargetAt  = 0;

  // 発光体の初期化（奥行きをばらけさせ、同時に出現しないようにする）
  for (int e = 0; e < TUN_ENEMY_COUNT; e++) {
    tunEnemyDepth[e] = tunRand01() * (float)TUN_RINGS;
    tunEnemyAngle[e] = tunRand01() * 6.2831853f;
    tunEnemySpeed[e] = (float)TUN_RINGS / (2500.0f + tunRand01() * 2000.0f);  // 1周 約2.5〜4.5秒
    tunEnemyType[e]  = (uint8_t)(tunRand() % 2);
  }

  tunEyeXf = 0.0f;
  tunEyeYf = 0.0f;
  tunGlanceEnemyIdx = -1;
  tunGlanceUntil = 0;

  // 弾の初期化（全スロット非アクティブ、発射タイマーはリセット）
  for (int b = 0; b < TUN_BULLET_COUNT; b++) tunBulletActive[b] = false;
  tunNextFireAt = 0;
  tunLastFireAt = 0;

  tunReady  = true;
}

// 自機のワイヤーフレーム（中心＝トンネルの奥を向く小さな三角形）
static void tunDrawCraft(int cx, int cy, float angleToCenter, uint16_t col) {
  const float tipR = 9.0f, backR = 6.0f, spread = 2.3f;
  int tipX = cx + (int)lroundf(cosf(angleToCenter) * tipR);
  int tipY = cy + (int)lroundf(sinf(angleToCenter) * tipR);
  int blX  = cx + (int)lroundf(cosf(angleToCenter + spread) * backR);
  int blY  = cy + (int)lroundf(sinf(angleToCenter + spread) * backR);
  int brX  = cx + (int)lroundf(cosf(angleToCenter - spread) * backR);
  int brY  = cy + (int)lroundf(sinf(angleToCenter - spread) * backR);
  lightDrawLine(tipX, tipY, blX, blY, col);
  lightDrawLine(tipX, tipY, brX, brY, col);
  lightDrawLine(blX,  blY,  brX, brY, col);
}

// 発光体のワイヤーフレーム（0=ひし形／1=三角の2種類のみ。単純な固定形状）
static void tunDrawEnemy(int cx, int cy, uint8_t type, uint16_t col) {
  if (type == 0) {
    lightDrawLine(cx,     cy - 5, cx + 5, cy,     col);
    lightDrawLine(cx + 5, cy,     cx,     cy + 5, col);
    lightDrawLine(cx,     cy + 5, cx - 5, cy,     col);
    lightDrawLine(cx - 5, cy,     cx,     cy - 5, col);
  } else {
    lightDrawLine(cx,     cy - 6, cx + 5, cy + 4, col);
    lightDrawLine(cx + 5, cy + 4, cx - 5, cy + 4, col);
    lightDrawLine(cx - 5, cy + 4, cx,     cy - 6, col);
  }
}

// 弾を1発発射する（空きスロットが無ければ何もしない＝弾数は固定上限を超えない）
static void tunFireBullet(float angle) {
  for (int b = 0; b < TUN_BULLET_COUNT; b++) {
    if (tunBulletActive[b]) continue;
    tunBulletActive[b] = true;
    tunBulletAngle[b]  = angle;
    tunBulletRadius[b] = TUN_CRAFT_RADIUS;
    return;
  }
}

void lightRenderTunnel(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!tunReady) buildTunnelTable();
  unsigned long now = millis();
  float dt = (tunLastMs == 0 || needsInit) ? 0.0f : (float)(now - tunLastMs);
  if (dt > 200.0f) dt = 200.0f;   // モード切替・一時停止直後の大ジャンプを防止
  tunLastMs = now;

  tunRotAngle   += TUN_ROT_SPEED * (float)tunRotDir * dt;
  tunFlowPhase  += TUN_FLOW_SPEED * dt;
  if (tunFlowPhase >= (float)TUN_RINGS) tunFlowPhase = fmodf(tunFlowPhase, (float)TUN_RINGS);
  tunPulsePhase += TUN_PULSE_SPEED * dt;
  if (tunPulsePhase > 6.2831853f) tunPulsePhase -= 6.2831853f;
  tunHueDeg     += TUN_HUE_SPEED * dt;
  if (tunHueDeg >= 360.0f) tunHueDeg = fmodf(tunHueDeg, 360.0f);

  // ── 背景：完全な黒で塗りつぶし ──
  lightFillRect(0, TUN_TOP, 320, 240 - TUN_TOP, 0x0000);

  // ── 最背面のカラフルな星（80年代アーケード風。既存のリング・自機・発光体・弾より必ず先に描く）──
  for (int i = 0; i < TUN_CSTAR_COUNT; i++) {
    lightFillRect(tunCStarX[i], tunCStarY[i], 1, 1, tunCStarCol[i]);
  }

  float pulse = 1.0f + sinf(tunPulsePhase) * TUN_PULSE_AMPL;

  // 各リングの深度・半径・色・頂点座標をまとめて計算
  float   d[TUN_RINGS];
  float   radius[TUN_RINGS];
  uint16_t col[TUN_RINGS];
  int16_t vx[TUN_RINGS][TUN_MAX_VERTS];
  int16_t vy[TUN_RINGS][TUN_MAX_VERTS];

  for (int i = 0; i < TUN_RINGS; i++) {
    float depth = fmodf(tunFlowPhase + (float)i, (float)TUN_RINGS);
    float t = depth / (float)TUN_RINGS;              // 0(手前)〜1(奥)
    float far3 = (1.0f - t) * (1.0f - t) * (1.0f - t);
    d[i]      = depth;
    radius[i] = (TUN_MIN_R + (TUN_MAX_R - TUN_MIN_R) * far3) * pulse;
    col[i]    = tunHueToRgb565(tunHueDeg + (float)i * TUN_HUE_STEP);

    float ringAngle = tunRotAngle + (float)i * TUN_RING_TWIST;
    for (uint8_t v = 0; v < tunVerts; v++) {
      float a = ringAngle + 6.2831853f * (float)v / (float)tunVerts;
      vx[i][v] = (int16_t)lroundf((float)TUN_CENTER_X + cosf(a) * radius[i]);
      vy[i][v] = (int16_t)lroundf((float)TUN_CENTER_Y + sinf(a) * radius[i]);
    }
  }

  // ── リング本体（多角形の輪郭線）を描画 ──
  for (int i = 0; i < TUN_RINGS; i++) {
    for (uint8_t v = 0; v < tunVerts; v++) {
      uint8_t vn = (v + 1 == tunVerts) ? 0 : (v + 1);
      lightDrawLine(vx[i][v], vy[i][v], vx[i][vn], vy[i][vn], col[i]);
    }
  }

  // ── 隣接する深度のリング同士をつなぐ放射状スポーク（トンネルの「壁」）──
  // 深度が周回して戻る1組（d[j] < d[i]になる境界）だけは接続をスキップする。
  for (int i = 0; i < TUN_RINGS; i++) {
    int j = (i + 1 == TUN_RINGS) ? 0 : (i + 1);
    if (d[j] < d[i]) continue;   // 周回の境界＝中心↔端の不自然な線を防ぐ
    for (uint8_t v = 0; v < tunVerts; v++) {
      lightDrawLine(vx[i][v], vy[i][v], vx[j][v], vy[j][v], col[i]);
    }
  }

  // ── 発光体：奥(depth=TUN_RINGS)から手前(depth=0)へ流れ、最前列で最奥から再出現 ──
  float enemyScreenX[TUN_ENEMY_COUNT];
  float enemyScreenY[TUN_ENEMY_COUNT];
  float enemyT[TUN_ENEMY_COUNT];   // 0(奥)〜1(手前)
  for (int e = 0; e < TUN_ENEMY_COUNT; e++) {
    tunEnemyDepth[e] -= tunEnemySpeed[e] * dt;
    if (tunEnemyDepth[e] <= 0.0f) {
      tunEnemyDepth[e] = (float)TUN_RINGS;               // 最奥から再出現
      tunEnemyAngle[e] = tunRand01() * 6.2831853f;
      tunEnemySpeed[e] = (float)TUN_RINGS / (2500.0f + tunRand01() * 2000.0f);
      tunEnemyType[e]  = (uint8_t)(tunRand() % 2);
    }
    float t = 1.0f - (tunEnemyDepth[e] / (float)TUN_RINGS);      // 0(奥)〜1(手前)
    float far3 = (1.0f - t) * (1.0f - t) * (1.0f - t);
    float er = TUN_MIN_R + (TUN_MAX_R - TUN_MIN_R) * far3;
    enemyT[e] = t;
    enemyScreenX[e] = (float)TUN_CENTER_X + cosf(tunEnemyAngle[e]) * er;
    enemyScreenY[e] = (float)TUN_CENTER_Y + sinf(tunEnemyAngle[e]) * er;
    tunDrawEnemy((int)lroundf(enemyScreenX[e]), (int)lroundf(enemyScreenY[e]), tunEnemyType[e], TUN_ENEMY_COL);
  }

  // ── 自機：画面下寄りの弧の中で、数秒おきに選ぶ目標角度へゆっくり操舵する ──
  if (now >= tunCraftRetargetAt) {
    tunCraftTargetAngle = TUN_CRAFT_ARC_CENTER + (tunRand01() * 2.0f - 1.0f) * TUN_CRAFT_ARC_HALF;
    tunCraftRetargetAt = now + 3000 + (unsigned long)(tunRand01() * 3000.0f);
  }
  {
    float steer = constrain(dt * TUN_CRAFT_STEER_RATE, 0.0f, 1.0f);
    tunCraftAngle += (tunCraftTargetAngle - tunCraftAngle) * steer;
  }

  // ── 自機の回避バイアス：間近の発光体が自機と近い角度にいる時だけ、わずかに位置をずらす（任意演出）──
  {
    float avoidTarget = 0.0f;
    for (int e = 0; e < TUN_ENEMY_COUNT; e++) {
      if (enemyT[e] < TUN_ENEMY_NEAR_T) continue;
      float diff = tunCraftAngle - tunEnemyAngle[e];
      while (diff >  3.1415927f) diff -= 6.2831853f;
      while (diff < -3.1415927f) diff += 6.2831853f;
      if (fabsf(diff) < 0.5f) avoidTarget += (diff >= 0.0f ? 1.0f : -1.0f) * 0.18f;
    }
    avoidTarget = constrain(avoidTarget, -0.25f, 0.25f);
    float biasSteer = constrain(dt * 0.0015f, 0.0f, 1.0f);
    tunCraftAvoidBias += (avoidTarget - tunCraftAvoidBias) * biasSteer;
  }

  float craftEffAngle = tunCraftAngle + tunCraftAvoidBias;
  float craftX = (float)TUN_CENTER_X + cosf(craftEffAngle) * TUN_CRAFT_RADIUS;
  float craftY = (float)TUN_CENTER_Y + sinf(craftEffAngle) * TUN_CRAFT_RADIUS;
  tunDrawCraft((int)lroundf(craftX), (int)lroundf(craftY), craftEffAngle + 3.1415927f, TUN_CRAFT_COL);

  // ── 弾：時々自機から発射する演出用ベクターミサイル ──
  // 当たり判定・発光体の破壊・爆発・スコアは一切持たない、純粋な演出。
  // 通常はランダム間隔で発射するが、発光体が自機の近く（角度が近い最前列間近）
  // まで来た瞬間は「敵を見つけて撃っているように見える」よう優先的に発射する
  // （優先発射も最短発射間隔TUN_BULLET_MIN_INTERVAL_MSの対象＝連射はしない）。
  {
    bool priorityFired = false;
    if (now - tunLastFireAt >= TUN_BULLET_MIN_INTERVAL_MS) {
      for (int e = 0; e < TUN_ENEMY_COUNT; e++) {
        if (enemyT[e] < TUN_BULLET_DETECT_T) continue;
        float diff = craftEffAngle - tunEnemyAngle[e];
        while (diff >  3.1415927f) diff -= 6.2831853f;
        while (diff < -3.1415927f) diff += 6.2831853f;
        if (fabsf(diff) < TUN_BULLET_DETECT_ANGLE) {
          tunFireBullet(craftEffAngle);
          tunLastFireAt = now;
          tunNextFireAt = now + 2500 + (unsigned long)(tunRand01() * 2500.0f);
          priorityFired = true;
          break;
        }
      }
    }
    if (!priorityFired && now >= tunNextFireAt) {
      tunFireBullet(craftEffAngle);
      tunLastFireAt = now;
      tunNextFireAt = now + 2500 + (unsigned long)(tunRand01() * 2500.0f);
    }

    for (int b = 0; b < TUN_BULLET_COUNT; b++) {
      if (!tunBulletActive[b]) continue;
      tunBulletRadius[b] -= TUN_BULLET_SPEED_PX_MS * dt;
      if (tunBulletRadius[b] <= TUN_MIN_R) { tunBulletActive[b] = false; continue; }
      float bx = (float)TUN_CENTER_X + cosf(tunBulletAngle[b]) * tunBulletRadius[b];
      float by = (float)TUN_CENTER_Y + sinf(tunBulletAngle[b]) * tunBulletRadius[b];
      float tailR = tunBulletRadius[b] + 8.0f;
      if (tailR > TUN_CRAFT_RADIUS) tailR = TUN_CRAFT_RADIUS;
      float tx = (float)TUN_CENTER_X + cosf(tunBulletAngle[b]) * tailR;
      float ty = (float)TUN_CENTER_Y + sinf(tunBulletAngle[b]) * tailR;
      lightDrawLine((int)lroundf(tx), (int)lroundf(ty), (int)lroundf(bx), (int)lroundf(by), TUN_CRAFT_COL);
    }
  }

  // ── かりポムの黒目：基本は自機を追従。発光体が最前列間近まで来た瞬間だけ、
  //    短時間そちらへ視線を切り替えてから自機追従へ戻る（優先度：間近の発光体 ＞ 自機）──
  if (now >= tunGlanceUntil) tunGlanceEnemyIdx = -1;
  if (tunGlanceEnemyIdx < 0) {
    for (int e = 0; e < TUN_ENEMY_COUNT; e++) {
      if (enemyT[e] >= TUN_ENEMY_NEAR_T) {
        tunGlanceEnemyIdx = e;
        tunGlanceUntil = now + 500;   // 約0.5秒だけ注視
        break;
      }
    }
  }
  float gazeX = (tunGlanceEnemyIdx >= 0) ? enemyScreenX[tunGlanceEnemyIdx] : craftX;
  float gazeY = (tunGlanceEnemyIdx >= 0) ? enemyScreenY[tunGlanceEnemyIdx] : craftY;

  float eyeTargetX = constrain((gazeX - (float)TUN_CENTER_X) / TUN_MAX_R, -1.0f, 1.0f) * TUN_EYE_MAX_X;
  float eyeTargetY = constrain((gazeY - (float)TUN_CENTER_Y) / TUN_MAX_R, -1.0f, 1.0f) * TUN_EYE_MAX_Y;
  float eyeSteer = constrain(dt * 0.0009f, 0.0f, 1.0f);
  tunEyeXf += (eyeTargetX - tunEyeXf) * eyeSteer;
  tunEyeYf += (eyeTargetY - tunEyeYf) * eyeSteer;
  eyeOffsetX = constrain((int)lroundf(tunEyeXf), -(int)TUN_EYE_MAX_X, (int)TUN_EYE_MAX_X);
  eyeOffsetY = constrain((int)lroundf(tunEyeYf), -(int)TUN_EYE_MAX_Y, (int)TUN_EYE_MAX_Y);
}

// ============================================================================
// Lighting #11 : PAC-MAN Arcade（Retro Game Lighting 第1弾 1/3）
//
// ■ コンセプト
//   1980年代のドットイート迷路アクションを、実在ゲームのマップ・画像・
//   スプライトデータを一切参照せず、独自デザインの迷路・自機・敵で
//   再現したLighting背景デモです。プレイヤー操作・当たり判定・スコア・
//   ゲームオーバーは無く、「勝手にパックマンが迷路を巡回し続けている」
//   画面が目的です。
//
// ■ 構成
//   ・迷路：20×12セル（1セル16px＝320×192＝ちょうどSCENE_TOP〜画面下端）。
//     外周ループ通路＋中央の横断通路＋装飾用の壁ブロックのみのシンプル構成。
//     壁の配置は本ファイル独自にデザインしたもの。
//   ・自機：外周ループを一定速度で周回。進行方向に応じて口が開閉する
//     アニメーションのみでパックマンらしさを表現（経路探索なし）。
//   ・ドット／パワーエサ：通路上に配置。自機が通過すると消え、全消化
//     または一定時間ごとに自動で復活し、無限ループを維持する。
//   ・敵（オバケ）：3体。あらかじめ決めた安全な経路（外周ループの別位相
//     ／中央通路の往復）だけを移動し、経路探索は行わない。
//
// ■ 音との関係（測定器ではない）
//   ・gViz.level（全体音量）で自機・敵の移動速度がわずかに上下する程度。
//
// ■ 共通ルール遵守
//   ・背景の矩形描画は lightFillRect を使用（Framework共通Brightnessが自動適用）
//   ・円／三角形はGFX.fillCircle等＋色を手動でlightBright()に通す
//     （既存Retro Race/Sky Raid等と同じ扱い）
//   ・上端48px（情報パネル）には描かない（PAC_TOP=SCENE_TOP）
//   ・顔は他のLightingと同じくコンポジタが最前面に描く（本モード専用の
//     顔非表示処理はしない＝ユーザー確認済み仕様）
//   ・動的メモリ確保なし。固定長の静的配列／スカラー変数のみ
// ============================================================================
#define PAC_CELL   16
#define PAC_COLS   20
#define PAC_ROWS   12                 // 20*16=320=SCENE_W, 12*16=192=SCENE_H-SCENE_TOP
#define PAC_TOP    SCENE_TOP
#define PAC_C0     1
#define PAC_C1     18
#define PAC_R0     1
#define PAC_R1     10
#define PAC_CROSS_ROW  5
#define PAC_PATH_MAX   64
#define PAC_CROSS_LEN  16             // 中央通路の内側セル数（col2..17）
#define PAC_GHOSTS     3

static bool   pacReady = false;
static int8_t pacPathCol[PAC_PATH_MAX];
static int8_t pacPathRow[PAC_PATH_MAX];
static int    pacPathLen = 0;
static bool   pacDotEaten[PAC_PATH_MAX];
static bool   pacCrossEaten[PAC_CROSS_LEN];

static float  pacPos   = 0.0f;        // 自機の経路上の連続位置（0..pacPathLen）
static const float PAC_SPEED = 0.10f; // 1フレームあたりの進行セル数（needsInit以外では固定）

static float  pacGhostPos[PAC_GHOSTS];  // [0][1]は外周ループ基準、[2]は中央通路の0..1往復
static int    pacGhostDir[PAC_GHOSTS];
static const uint16_t PAC_GHOST_COL[PAC_GHOSTS] = {
  (uint16_t)(((230 & 0xF8) << 8) | ((40  & 0xFC) << 3) | (40  >> 3)),  // 赤
  (uint16_t)(((240 & 0xF8) << 8) | ((130 & 0xFC) << 3) | (210 >> 3)),  // ピンク
  (uint16_t)(((70  & 0xF8) << 8) | ((220 & 0xFC) << 3) | (230 >> 3)),  // シアン
};

static unsigned long pacResetAt = 0;

static inline void pacCellXY(int col, int row, int* px, int* py) {
  *px = col * PAC_CELL + PAC_CELL / 2;
  *py = PAC_TOP + row * PAC_CELL + PAC_CELL / 2;
}

// 外周ループ通路のセル列を起動時に一度だけ生成する（時計回り）。
static void pacBuildPath() {
  pacPathLen = 0;
  for (int c = PAC_C0; c <= PAC_C1; c++)              { pacPathCol[pacPathLen] = c;      pacPathRow[pacPathLen] = PAC_R0; pacPathLen++; }
  for (int r = PAC_R0 + 1; r <= PAC_R1; r++)          { pacPathCol[pacPathLen] = PAC_C1; pacPathRow[pacPathLen] = r;      pacPathLen++; }
  for (int c = PAC_C1 - 1; c >= PAC_C0; c--)          { pacPathCol[pacPathLen] = c;      pacPathRow[pacPathLen] = PAC_R1; pacPathLen++; }
  for (int r = PAC_R1 - 1; r >= PAC_R0 + 1; r--)      { pacPathCol[pacPathLen] = PAC_C0; pacPathRow[pacPathLen] = r;      pacPathLen++; }
}

static void pacResetDots() {
  for (int i = 0; i < pacPathLen; i++) pacDotEaten[i] = false;
  for (int i = 0; i < PAC_CROSS_LEN; i++) pacCrossEaten[i] = false;
}

static void pacResetActors() {
  pacPos = 0.0f;
  pacGhostPos[0] = (float)pacPathLen * 0.33f;
  pacGhostPos[1] = (float)pacPathLen * 0.66f;
  pacGhostPos[2] = 0.5f;              // 中央通路は0..1の往復パラメータ
  pacGhostDir[0] = 1;
  pacGhostDir[1] = -1;
  pacGhostDir[2] = 1;
}

static void pacInitAll() {
  pacBuildPath();
  pacResetDots();
  pacResetActors();
  pacResetAt = 0;
  pacReady = true;
}

// 装飾用の壁ブロック（見た目だけで、実際の通路判定＝移動経路には使わない）。
// PAC-MAN風の非対称な迷路に見せるため、上下左右で異なる形の壁ブロック
// （T字・角・袋小路・ゴーストハウス・左右トンネル風の開口）を配置する。
// 実際にキャラクターが通る経路（外周ループ＝PAC_C0/PAC_C1/PAC_R0/PAC_R1と
// 中央横断通路＝PAC_CROSS_ROW）は、以下のガードで必ず壁にならないようにしている。
static bool pacIsWallCell(int col, int row) {
  // 上下端は常に壁
  if (row == 0 || row == PAC_ROWS - 1) return true;
  // 左右端：中央通路の行だけ開口し、PAC-MANらしいワープトンネル風の抜けを演出
  if (col == 0 || col == PAC_COLS - 1) return (row != PAC_CROSS_ROW);

  // 実際の移動経路（外周ループ＋中央横断通路）は最優先で通路のまま維持
  if (col == PAC_C0 || col == PAC_C1)        return false; // 外周ループの縦通路帯
  if (row == PAC_R0 || row == PAC_R1)        return false; // 外周ループの横通路帯
  if (row == PAC_CROSS_ROW)                  return false; // 中央横断通路

  // ── ここから内部の装飾壁（通路判定には無関係）──
  // 上段（row2-4）：非対称なT字・角ブロックで「田」の字を崩す
  if (row >= 2 && row <= 3 && col >= 2  && col <= 3)  return true; // 左上の角ブロック
  if (row == 2              && col >= 5  && col <= 8) return true; // 横バー
  if (row >= 3 && row <= 4  && col >= 6  && col <= 7) return true; // ↑から垂れるT字の柄
  if (row >= 2 && row <= 4  && col >= 10 && col <= 11) return true; // 中央上の縦長ブロック
  if (row == 2              && col >= 13 && col <= 17) return true; // 右上の長い横バー
  if (row >= 3 && row <= 4  && col >= 14 && col <= 15) return true; // 右側T字の柄（左とは位置をずらす）

  // 中央：ゴーストハウスを連想させる小部屋（中央横断通路のすぐ下＝入口的な位置）
  if (row == 6               && col >= 8  && col <= 11) return true; // 屋根
  if (row == 7               && (col == 8 || col == 11)) return true; // 側柱（内側col9-10は入口として開放）

  // 下段（row6-9）：上段とは高さ・大きさを変えた非対称配置
  if (row >= 6 && row <= 7  && col >= 2  && col <= 3)  return true; // 左の角ブロック（上段より低い位置）
  if (row >= 8 && row <= 9  && col >= 4  && col <= 6)  return true; // 左下の横長ブロック
  if (row == 9               && col >= 13 && col <= 16) return true; // 右下の横バー
  if (row >= 6 && row <= 8  && col >= 14 && col <= 15) return true; // 右の縦長ブロック
  if (row >= 6 && row <= 7  && col == 17)              return true; // 右端の細い柱（袋小路）

  return false;
}

static void pacDrawGhost(int cx, int cy, uint16_t col, int dirX) {
  const int r = 7;
  const uint16_t bodyCol = lightBright(col);
  lightFillRect(cx - r, cy - 2, r * 2, r + 4, col);            // 胴体（角ばった下半分・Brightness自動適用）
  GFX.fillCircle(cx, cy - 1, r, bodyCol);                       // 丸い頭
  // すそのギザギザ（背景の黒で三角に切り欠く）
  for (int k = -2; k <= 2; k++) {
    int nx = cx + k * (r / 2);
    GFX.fillTriangle(nx - r / 4, cy + r + 2, nx + r / 4, cy + r + 2, nx, cy + r - 2, BLACK);
  }
  // 目（進行方向へ寄せる）
  int ex = dirX * 2;
  const uint16_t pupil = (uint16_t)(((20 & 0xF8) << 8) | ((30 & 0xFC) << 3) | (140 >> 3));
  GFX.fillCircle(cx - 3 + ex, cy - 2, 3, lightBright(WHITE));
  GFX.fillCircle(cx + 3 + ex, cy - 2, 3, lightBright(WHITE));
  GFX.fillCircle(cx - 3 + ex * 2, cy - 2, 1, lightBright(pupil));
  GFX.fillCircle(cx + 3 + ex * 2, cy - 2, 1, lightBright(pupil));
}

void lightRenderPacman(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!pacReady) pacInitAll();
  if (needsInit) pacResetActors();   // 短時間の再有効化では迷路・ドット進捗は消さず、動きだけ揃え直す
  unsigned long now = millis();
  float audioBoost = 1.0f + gViz.level * 0.30f;

  const uint16_t WALL_COL  = (uint16_t)(((33  & 0xF8) << 8) | ((150 & 0xFC) << 3) | (243 >> 3));
  const uint16_t WALL_EDGE = (uint16_t)(((15  & 0xF8) << 8) | ((90  & 0xFC) << 3) | (200 >> 3));
  const uint16_t DOT_COL   = (uint16_t)(((255 & 0xF8) << 8) | ((255 & 0xFC) << 3) | (190 >> 3));
  const uint16_t PACMAN_COL= (uint16_t)(((255 & 0xF8) << 8) | ((225 & 0xFC) << 3) | (0   >> 3));

  // ── 背景を黒で塗りつぶす（毎フレーム全面・Sky Raid等と同じ連続アニメ方式）──
  lightFillRect(0, PAC_TOP, SCENE_W, SCENE_H - PAC_TOP, BLACK);

  // ── 壁タイル ──
  for (int row = 0; row < PAC_ROWS; row++) {
    for (int col = 0; col < PAC_COLS; col++) {
      if (!pacIsWallCell(col, row)) continue;
      int x = col * PAC_CELL, y = PAC_TOP + row * PAC_CELL;
      lightFillRect(x + 1, y + 1, PAC_CELL - 2, PAC_CELL - 2, WALL_EDGE);
      lightFillRect(x + 2, y + 2, PAC_CELL - 6, PAC_CELL - 6, WALL_COL);
    }
  }

  // ── ドット／パワーエサ ──
  bool blink = ((now / 260) % 2) == 0;
  for (int i = 0; i < pacPathLen; i++) {
    int col = pacPathCol[i], row = pacPathRow[i];
    bool isPellet = (col == PAC_C0 && row == PAC_R0) || (col == PAC_C1 && row == PAC_R0) ||
                    (col == PAC_C0 && row == PAC_R1) || (col == PAC_C1 && row == PAC_R1);
    int cx, cy; pacCellXY(col, row, &cx, &cy);
    if (isPellet) {
      if (!blink) continue;
      GFX.fillCircle(cx, cy, 4, lightBright(DOT_COL));
    } else {
      if (pacDotEaten[i]) continue;
      GFX.fillCircle(cx, cy, 2, lightBright(DOT_COL));
    }
  }
  for (int k = 0; k < PAC_CROSS_LEN; k++) {
    if (pacCrossEaten[k]) continue;
    int col = PAC_C0 + 1 + k;
    int cx, cy; pacCellXY(col, PAC_CROSS_ROW, &cx, &cy);
    GFX.fillCircle(cx, cy, 2, lightBright(DOT_COL));
  }

  // ── 自機（外周ループを周回）──
  pacPos += PAC_SPEED * audioBoost;
  while (pacPos >= (float)pacPathLen) pacPos -= (float)pacPathLen;
  int   pi  = (int)pacPos;
  int   pi2 = (pi + 1) % pacPathLen;
  float pf  = pacPos - (float)pi;
  int x0, y0, x1, y1;
  pacCellXY(pacPathCol[pi],  pacPathRow[pi],  &x0, &y0);
  pacCellXY(pacPathCol[pi2], pacPathRow[pi2], &x1, &y1);
  int pacX = x0 + (int)((x1 - x0) * pf);
  int pacY = y0 + (int)((y1 - y0) * pf);
  pacDotEaten[pi] = true;

  int dirX = (x1 > x0) ? 1 : (x1 < x0 ? -1 : 0);
  int dirY = (y1 > y0) ? 1 : (y1 < y0 ? -1 : 0);
  float mouthT = (sinf((float)now * 0.012f) + 1.0f) * 0.5f;  // 0..1
  int mouthDeg = 8 + (int)(mouthT * 30.0f);                  // 8〜38度

  GFX.fillCircle(pacX, pacY, 7, lightBright(PACMAN_COL));
  if (dirX != 0 || dirY != 0) {
    float baseAng = atan2f((float)dirY, (float)dirX);
    float half = mouthDeg * 3.14159f / 180.0f;
    float ax = pacX + cosf(baseAng + half) * 10.0f;
    float ay = pacY + sinf(baseAng + half) * 10.0f;
    float bx = pacX + cosf(baseAng - half) * 10.0f;
    float by = pacY + sinf(baseAng - half) * 10.0f;
    GFX.fillTriangle(pacX, pacY, (int)ax, (int)ay, (int)bx, (int)by, BLACK);
  }

  // ── 敵（オバケ）3体：外周ループの別位相2体＋中央通路往復1体 ──
  for (int g = 0; g < PAC_GHOSTS; g++) {
    int gx, gy, gdirX;
    if (g < 2) {
      pacGhostPos[g] += 0.085f * audioBoost * (float)pacGhostDir[g];
      while (pacGhostPos[g] < 0.0f)               pacGhostPos[g] += (float)pacPathLen;
      while (pacGhostPos[g] >= (float)pacPathLen)  pacGhostPos[g] -= (float)pacPathLen;
      int gi  = (int)pacGhostPos[g];
      int gi2 = (gi + pacGhostDir[g] + pacPathLen) % pacPathLen;
      float gf = pacGhostPos[g] - (float)gi;
      int gx0, gy0, gx1, gy1;
      pacCellXY(pacPathCol[gi],  pacPathRow[gi],  &gx0, &gy0);
      pacCellXY(pacPathCol[gi2], pacPathRow[gi2], &gx1, &gy1);
      gx = gx0 + (int)((gx1 - gx0) * gf);
      gy = gy0 + (int)((gy1 - gy0) * gf);
      gdirX = (gx1 >= gx0) ? 1 : -1;
      if (gi >= 0 && gi < pacPathLen) pacDotEaten[gi] = true;
    } else {
      pacGhostPos[g] += 0.02f * audioBoost * (float)pacGhostDir[g];
      if (pacGhostPos[g] >= 1.0f) { pacGhostPos[g] = 1.0f; pacGhostDir[g] = -1; }
      if (pacGhostPos[g] <= 0.0f) { pacGhostPos[g] = 0.0f; pacGhostDir[g] = 1; }
      float t = pacGhostPos[g];
      int cx0, cy0, cx1, cy1;
      pacCellXY(PAC_C0, PAC_CROSS_ROW, &cx0, &cy0);
      pacCellXY(PAC_C1, PAC_CROSS_ROW, &cx1, &cy1);
      gx = cx0 + (int)((cx1 - cx0) * t);
      gy = cy0;
      gdirX = pacGhostDir[g];
      int idx = (int)(t * (PAC_CROSS_LEN - 1));
      if (idx >= 0 && idx < PAC_CROSS_LEN) pacCrossEaten[idx] = true;
    }
    pacDrawGhost(gx, gy, PAC_GHOST_COL[g], gdirX);
  }

  // ── ドット全消化 or 一定時間経過で無限ループのため復活 ──
  bool allEaten = true;
  for (int i = 0; i < pacPathLen && allEaten; i++) if (!pacDotEaten[i]) allEaten = false;
  for (int k = 0; k < PAC_CROSS_LEN && allEaten; k++) if (!pacCrossEaten[k]) allEaten = false;
  if (pacResetAt == 0) pacResetAt = now + 45000;   // 45秒ごとにも強制リセット（見た目の新鮮さ維持）
  if (allEaten || now >= pacResetAt) {
    pacResetDots();
    pacResetAt = now + 45000;
  }
}

// ============================================================================
// Lighting #12 : Fighter Duel（Retro Game Lighting 第1弾 2/3）
//
// ■ コンセプト
//   1990年代前半の対戦格闘ゲームをモチーフにした自動デモです。頭・胴・腕・
//   脚を持つブロック体型の2人のファイター（棒人間ではない）が横向きの
//   ステージ上で、構え→前後移動→パンチ／キック→ジャンプ／しゃがみ→
//   被弾のけぞりを自動サイクルし、時おり飛び道具を撃ち合います。体力
//   ゲージは時間経過＋被弾で減り続け、0になると大きく『K.O.』を表示して
//   数秒後に体力・位置をリセットし次のラウンドへ進みます。勝者は毎回変わり、
//   固定の勝者はいません。プレイヤー操作・厳密な当たり判定・成績記録は
//   行いません。
//
// ■ 音との関係（測定器ではない）
//   ・攻撃ヒット時のフラッシュ・K.O.演出の明るさが gViz.level でわずかに変化。
//
// ■ 共通ルール遵守
//   ・背景の矩形描画は lightFillRect を使用
//   ・上端48px（情報パネル）には描かない（SF_TOP=SCENE_TOP）
//   ・顔は他のLightingと同じくコンポジタが最前面に描く
//   ・動的メモリ確保なし。固定長の静的配列／スカラーのみ
// ============================================================================
#define SF_TOP        SCENE_TOP
#define SF_GROUND_Y   (SCENE_H - 40)
#define SF_STAGE_L    30
#define SF_STAGE_R    290

#define SF_IDLE     0
#define SF_WALK_F   1
#define SF_WALK_B   2
#define SF_CROUCH   3
#define SF_PUNCH    4
#define SF_KICK     5
#define SF_JUMP     6
#define SF_SPECIAL  7
#define SF_HIT      8

struct SFFighter {
  float x;
  int   facing;        // +1: 右向き, -1: 左向き
  int   state;
  unsigned long stateUntil;
  bool  impactDone;     // このstate中に攻撃判定を処理済みか
  float health;         // 0..100
  int   hitFlash;       // 残りフラッシュフレーム数
  float jumpPhase;      // 0..1（ジャンプの弧）
};

// Arduino IDEの自動プロトタイプ生成（ctags）は、struct SFFighter の定義より前の
// 位置へ sfDrawFighter() のプロトタイプを機械的に挿入してしまい、その時点では
// SFFighter がまだ未定義のため "'SFFighter' does not name a type" でビルド失敗する。
// ここで手動プロトタイプを明示することで自動生成対象から外し、回避する。
static void sfDrawFighter(const SFFighter& f, uint16_t giColRaw, uint16_t skinColRaw, bool isFighterA);

static bool      sfReady    = false;
static SFFighter  sfF[2];
static bool       sfProjActive[2];
static float      sfProjX[2];
static int        sfProjDir[2];
static int        sfRound     = 1;
static int        sfTimer     = 60;
static unsigned long sfTimerTickAt = 0;
static bool       sfKoActive  = false;
static unsigned long sfKoUntil = 0;
static uint32_t   sfRng = 0x2545F491u;

static inline uint32_t sfRand() { sfRng = sfRng * 1664525u + 1013904223u; return sfRng; }
static inline int      sfRandRange(int lo, int hi) { return lo + (int)(sfRand() % (uint32_t)(hi - lo + 1)); }

static void sfResetFighter(int i, float x, int facing) {
  sfF[i].x          = x;
  sfF[i].facing     = facing;
  sfF[i].state      = SF_IDLE;
  sfF[i].stateUntil = millis() + 400;
  sfF[i].impactDone = false;
  sfF[i].health     = 100.0f;
  sfF[i].hitFlash   = 0;
  sfF[i].jumpPhase  = 0.0f;
  sfProjActive[i]   = false;
}

static void sfInitAll() {
  sfResetFighter(0, (float)SF_STAGE_L + 40.0f, 1);
  sfResetFighter(1, (float)SF_STAGE_R - 40.0f, -1);
  sfRound       = 1;
  sfTimer       = 60;
  sfTimerTickAt = millis() + 1000;
  sfKoActive    = false;
  sfReady       = true;
}

static void sfNewRound() {
  sfResetFighter(0, (float)SF_STAGE_L + 40.0f, 1);
  sfResetFighter(1, (float)SF_STAGE_R - 40.0f, -1);
  sfRound++;
  sfTimer       = 60;
  sfTimerTickAt = millis() + 1000;
  sfKoActive    = false;
}

static void sfApplyHit(int target, float dmg, int fromDir) {
  sfF[target].health -= dmg;
  sfF[target].hitFlash = 6;
  sfF[target].state = SF_HIT;
  sfF[target].stateUntil = millis() + 260;
  sfF[target].x += (float)fromDir * 10.0f;
  if (sfF[target].x < SF_STAGE_L) sfF[target].x = SF_STAGE_L;
  if (sfF[target].x > SF_STAGE_R) sfF[target].x = SF_STAGE_R;
  if (sfF[target].health <= 0.0f) {
    sfF[target].health = 0.0f;
    sfKoActive = true;
    sfKoUntil  = millis() + 2600;
  }
}

// 次の行動を距離ベースの重み付け抽選で決める（厳密な当たり判定・経路探索は行わない）
static void sfDecideNext(int i, float dist) {
  unsigned long now = millis();
  int r = sfRandRange(0, 99);
  int st;
  unsigned long dur;
  if (dist > 110) {
    st = (r < 80) ? SF_WALK_F : (r < 92 ? SF_SPECIAL : SF_JUMP);
  } else if (dist > 60) {
    st = (r < 45) ? SF_WALK_F : (r < 65) ? SF_PUNCH : (r < 82) ? SF_KICK : (r < 92) ? SF_SPECIAL : SF_JUMP;
  } else {
    st = (r < 35) ? SF_PUNCH : (r < 60) ? SF_KICK : (r < 78) ? SF_WALK_B : (r < 90) ? SF_CROUCH : SF_JUMP;
  }
  switch (st) {
    case SF_WALK_F:  dur = 260; break;
    case SF_WALK_B:  dur = 240; break;
    case SF_CROUCH:  dur = 380; break;
    case SF_PUNCH:   dur = 260; break;
    case SF_KICK:    dur = 320; break;
    case SF_JUMP:    dur = 480; break;
    case SF_SPECIAL: dur = 300; break;
    default:         dur = 300; break;
  }
  sfF[i].state      = st;
  sfF[i].stateUntil = now + dur;
  sfF[i].impactDone = false;
}

// 2026/07/27 改訂：Fighter Duelの見た目改善依頼への対応。
// 既存のゲーム状態(SFFighter.state)・攻撃判定・タイミング・体力・位置計算は
// 一切変更せず、状態ごとの描画（ポーズ）だけを作り直した。
// 「サラリーマン2人の喧嘩」に見えないよう、常時：腰を落とし前後に脚を
// ずらしたファイティングスタンス／道着＋帯／拳を顔の高さに構える、を基本とし、
// Fighter A＝白系の道着＋赤い鉢巻、Fighter B＝濃色の道着＋とげ状の黒髪シルエット
// で頭部・体格・構えの3点から一目で区別できるようにしている。
static void sfDrawFighter(const SFFighter& f, uint16_t giColRaw, uint16_t skinColRaw, bool isFighterA) {
  int cx = (int)lroundf(f.x);
  int baseY = SF_GROUND_Y;
  int crouchExtra = (f.state == SF_CROUCH) ? 8 : 0;
  int jumpY  = (f.state == SF_JUMP) ? (int)(sinf(f.jumpPhase * 3.14159f) * 34.0f) : 0;
  int cy = baseY - jumpY;
  uint16_t giCol = (f.hitFlash > 0) ? WHITE : giColRaw;   // 被弾中は白フラッシュ（既存仕様のまま）
  uint16_t skin  = lightBright(skinColRaw);
  int fwd = f.facing;   // +1: 相手が右, -1: 相手が左（既存のfacing符号のまま使用）

  const uint16_t BELT_COL     = (uint16_t)(((25  & 0xF8) << 8) | ((22  & 0xFC) << 3) | (20  >> 3));  // 黒帯
  const uint16_t HEADBAND_COL = (uint16_t)(((225 & 0xF8) << 8) | ((35  & 0xFC) << 3) | (35  >> 3));  // 赤い鉢巻（Fighter A）
  const uint16_t HAIR_COL     = (uint16_t)(((35  & 0xF8) << 8) | ((28  & 0xFC) << 3) | (24  >> 3));  // 黒髪（Fighter B）

  // 常時：腰を落とし前後に脚をずらしたファイティングスタンス（棒立ちにしない）。
  int legH    = 16 - crouchExtra / 2;
  int torsoH  = (isFighterA ? 22 : 24) - crouchExtra;   // Fighter Bはやや体格を大きく＝体格差
  int torsoW  = isFighterA ? 16 : 18;
  int headR   = 8;

  int frontLegX = cx + fwd * 5;   // 相手側へ出した前脚
  int backLegX  = cx - fwd * 6;   // 後ろへ引いた軸脚
  int footY     = cy - 3;

  // ── 脚・素足 ──
  if (f.state == SF_KICK) {
    int reach = fwd * 26;
    lightFillRect(backLegX - 3, cy - legH, 7, legH, giCol);            // 軸足（道着の裾）
    lightFillRect(backLegX - 4, footY, 8, 3, skin);                     // 軸足の素足
    lightFillRect(cx + reach - 5, cy - legH - 6, 11, 6, skin);          // 相手側へ伸ばした蹴り脚（素足）
  } else if (f.state == SF_JUMP) {
    // ジャンプ中：両脚を折り曲げて引き上げ、地面から離れているのを明確にする
    lightFillRect(cx - 8, cy - legH + 5, 7, legH - 5, giCol);
    lightFillRect(cx + 1, cy - legH + 5, 7, legH - 5, giCol);
    lightFillRect(cx - 8, cy - 1, 7, 3, skin);
    lightFillRect(cx + 1, cy - 1, 7, 3, skin);
  } else {
    lightFillRect(backLegX  - 3, cy - legH, 7, legH, giCol);
    lightFillRect(frontLegX - 3, cy - legH, 7, legH, giCol);
    lightFillRect(backLegX  - 4, footY, 8, 3, skin);
    lightFillRect(frontLegX - 4, footY, 8, 3, skin);
  }

  // ── 胴（道着）＋帯 ──：相手側へわずかに前傾させたオフセットを付ける
  int torsoY = cy - legH - torsoH;
  int torsoX = cx - torsoW / 2 + fwd * 2;
  lightFillRect(torsoX, torsoY, torsoW, torsoH, giCol);
  lightFillRect(torsoX - 1, torsoY + torsoH - 5, torsoW + 2, 5, BELT_COL);   // 帯（腰位置）
  lightFillRect(torsoX + torsoW / 2 - 3, torsoY, 6, 4, skin);                 // 襟元（肌色のV字）

  int torsoCenterX = torsoX + torsoW / 2;
  int guardY = torsoY - 2;

  // ── 腕：常に拳を顔の高さへ構え、状態に応じてポーズを変える ──
  if (f.state == SF_PUNCH) {
    int reach = fwd * 24;
    lightFillRect(torsoCenterX + reach - 5, guardY + 2, 10, 7, skin);         // 突き出した拳
    lightFillRect(torsoCenterX - fwd * 8 - 3, guardY + 8, 7, 8, giCol);       // 引いた腕（そで）
  } else if (f.state == SF_SPECIAL) {
    // 飛び道具：両手を相手側へ突き出し「技を出している」ことが分かる姿勢にする
    int reach = fwd * 16;
    lightFillRect(torsoCenterX + reach - 5, guardY,     10, 6, skin);
    lightFillRect(torsoCenterX + reach - 5, guardY + 8, 10, 6, skin);
  } else if (f.state == SF_HIT) {
    // 被弾：両腕を開き、上体・頭をのけぞらせる
    lightFillRect(torsoCenterX - fwd * 14 - 4, guardY + 4, 8, 8, skin);
    lightFillRect(torsoCenterX + fwd * 14 - 4, guardY + 2, 8, 8, skin);
  } else {
    // 通常構え：前拳を顔の高さ、後ろ拳をあご元に
    lightFillRect(torsoCenterX + fwd * 7 - 3, guardY,     7, 8, skin);
    lightFillRect(torsoCenterX - fwd * 7 - 3, guardY + 5, 7, 8, skin);
  }

  // ── 頭 ──（被弾時は後ろへのけぞる）
  int headX = torsoCenterX + (f.state == SF_HIT ? -fwd * 6 : 0);
  int headY = torsoY - headR + 1;
  GFX.fillCircle(headX, headY, headR, skin);

  if (isFighterA) {
    // Fighter A：赤い鉢巻（後方へ流れる端付き）
    lightFillRect(headX - headR, headY - headR + 2, headR * 2, 4, HEADBAND_COL);
    lightFillRect(headX - fwd * (headR + 5), headY - headR + 2, 6, 3, HEADBAND_COL);
  } else {
    // Fighter B：とげ状の黒髪シルエットでAと区別
    GFX.fillTriangle(headX - 6, headY - headR + 3, headX,     headY - headR - 6, headX + 2, headY - headR + 2, HAIR_COL);
    GFX.fillTriangle(headX - 1, headY - headR + 2, headX + 5, headY - headR - 6, headX + 7, headY - headR + 3, HAIR_COL);
  }
}

static void sfUpdateProjectile(int i) {
  int other = 1 - i;
  if (!sfProjActive[i]) return;
  sfProjX[i] += (float)sfProjDir[i] * 6.5f;
  if (fabsf(sfProjX[i] - sfF[other].x) < 14.0f && !sfKoActive) {
    sfApplyHit(other, 9.0f + (float)sfRandRange(0, 4), sfProjDir[i]);
    sfProjActive[i] = false;
    return;
  }
  if (sfProjX[i] < SF_STAGE_L - 10 || sfProjX[i] > SF_STAGE_R + 10) sfProjActive[i] = false;
}

static void sfDrawBar(int x, float pct, bool rightAlign) {
  uint16_t col = (pct > 50) ? GREEN : (pct > 20 ? ORANGE : RED);
  int w = 110;
  GFX.drawRect(x, SF_TOP + 6, w, 12, lightBright(WHITE));
  int fw = (int)(w * (pct / 100.0f));
  if (fw > 0) {
    int fx = rightAlign ? (x + w - fw) : x;
    lightFillRect(fx + 1, SF_TOP + 7, fw - 2, 10, col);
  }
}

void lightRenderStreetFighter(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!sfReady) sfInitAll();
  (void)needsInit;   // 短時間の再有効化ではラウンド進行を消さない（Rhythm教訓の踏襲）
  unsigned long now = millis();
  float audioBoost = 1.0f + gViz.level * 0.25f;

  // ── 背景 ──
  const uint16_t SKY_A      = (uint16_t)(((120 & 0xF8) << 8) | ((190 & 0xFC) << 3) | (230 >> 3));
  const uint16_t SKY_B      = (uint16_t)(((170 & 0xF8) << 8) | ((215 & 0xFC) << 3) | (235 >> 3));
  const uint16_t HILL_COL   = (uint16_t)(((90  & 0xF8) << 8) | ((150 & 0xFC) << 3) | (110 >> 3));
  const uint16_t GROUND_COL = (uint16_t)(((200 & 0xF8) << 8) | ((160 & 0xFC) << 3) | (100 >> 3));
  const uint16_t GROUND_LN  = (uint16_t)(((150 & 0xF8) << 8) | ((110 & 0xFC) << 3) | (60  >> 3));

  lightFillRect(0, SF_TOP,      SCENE_W, (SF_GROUND_Y - SF_TOP),      SKY_A);
  lightFillRect(0, SF_TOP + 40, SCENE_W, (SF_GROUND_Y - SF_TOP - 40), SKY_B);
  for (int k = 0; k < 3; k++) {
    int hx = 60 + k * 110;
    lightFillRect(hx - 40, SF_GROUND_Y - 26, 80, 26, HILL_COL);
  }
  lightFillRect(0, SF_GROUND_Y, SCENE_W, SCENE_H - SF_GROUND_Y, GROUND_COL);
  lightFillRect(0, SF_GROUND_Y, SCENE_W, 3, GROUND_LN);

  // ── 体力ゲージ・ラウンド表示 ──
  sfDrawBar(14, sfF[0].health, false);
  sfDrawBar(SCENE_W - 14 - 110, sfF[1].health, true);
  GFX.setTextSize(1);
  GFX.setTextColor(lightBright(WHITE));
  GFX.drawString("ROUND " + String(sfRound), 122, SF_TOP + 22);
  GFX.drawString(String(sfTimer), 152, SF_TOP + 4);

  // ── ラウンドタイマー ──
  if (!sfKoActive && now >= sfTimerTickAt) {
    sfTimerTickAt = now + 1000;
    if (sfTimer > 0) sfTimer--;
    if (sfTimer <= 0) { sfKoActive = true; sfKoUntil = now + 2200; }
  }

  if (sfKoActive) {
    // ── K.O. 表示・ラウンドリセット ──
    GFX.setTextSize(3);
    GFX.setTextColor(lightBright(RED));
    GFX.drawString("K.O.", 118, 120);
    GFX.setTextSize(1);
    if (now >= sfKoUntil) sfNewRound();
  } else {
    float dist = fabsf(sfF[0].x - sfF[1].x);
    for (int i = 0; i < 2; i++) {
      SFFighter& f = sfF[i];
      int other = 1 - i;
      f.facing = (sfF[other].x >= f.x) ? 1 : -1;
      if (f.hitFlash > 0) f.hitFlash--;

      if (now >= f.stateUntil) {
        if (f.state == SF_SPECIAL && !f.impactDone) {
          sfProjActive[i] = true;
          sfProjX[i]      = f.x + f.facing * 16.0f;
          sfProjDir[i]    = f.facing;
        }
        sfDecideNext(i, dist);
      }

      switch (f.state) {
        case SF_WALK_F: f.x += f.facing * 0.7f * audioBoost; break;
        case SF_WALK_B: f.x -= f.facing * 0.6f * audioBoost; break;
        case SF_JUMP:   f.jumpPhase += 0.03f; if (f.jumpPhase > 1.0f) f.jumpPhase = 0.0f; break;
        default: break;
      }
      if (f.x < SF_STAGE_L) f.x = SF_STAGE_L;
      if (f.x > SF_STAGE_R) f.x = SF_STAGE_R;

      // 攻撃判定（stateの中間地点で一度だけ）
      if (!f.impactDone && (f.state == SF_PUNCH || f.state == SF_KICK)) {
        unsigned long total = (f.state == SF_PUNCH) ? 260UL : 320UL;
        if ((long)(f.stateUntil - now) <= (long)(total / 2)) {
          f.impactDone = true;
          float reach = (f.state == SF_PUNCH) ? 46.0f : 58.0f;
          if (fabsf(f.x - sfF[other].x) <= reach && sfRandRange(0, 99) < 55) {
            float dmg = (f.state == SF_PUNCH) ? (float)sfRandRange(4, 7) : (float)sfRandRange(6, 10);
            sfApplyHit(other, dmg, f.facing);
          }
        }
      }
    }
    // 体力は時間経過でもごくわずかに減る（決着を保証する自然減衰）
    sfF[0].health -= 0.015f;
    sfF[1].health -= 0.015f;
    if (sfF[0].health <= 0.0f || sfF[1].health <= 0.0f) {
      sfKoActive = true;
      sfKoUntil  = now + 2600;
    }
    sfUpdateProjectile(0);
    sfUpdateProjectile(1);
  }

  // ── 描画 ──
  // 2026/07/27 改訂：Fighter A＝白系の道着、Fighter B＝濃い赤系の道着へ変更
  // （見た目改善依頼対応。健康ゲージ・ラウンド進行・攻撃判定等は上記のとおり無変更）。
  const uint16_t P1_COL   = (uint16_t)(((235 & 0xF8) << 8) | ((228 & 0xFC) << 3) | (205 >> 3));  // Fighter A：白系の道着
  const uint16_t P2_COL   = (uint16_t)(((150 & 0xF8) << 8) | ((25  & 0xFC) << 3) | (25  >> 3));  // Fighter B：濃い赤の道着
  const uint16_t SKIN_COL = (uint16_t)(((235 & 0xF8) << 8) | ((190 & 0xFC) << 3) | (150 >> 3));
  sfDrawFighter(sfF[0], P1_COL, SKIN_COL, true);    // Fighter A
  sfDrawFighter(sfF[1], P2_COL, SKIN_COL, false);   // Fighter B

  for (int i = 0; i < 2; i++) {
    if (!sfProjActive[i]) continue;
    uint16_t pc = (i == 0) ? P1_COL : P2_COL;
    GFX.fillCircle((int)lroundf(sfProjX[i]),                    SF_GROUND_Y - 26, 6, lightBright(pc));
    GFX.fillCircle((int)lroundf(sfProjX[i] - sfProjDir[i] * 8),  SF_GROUND_Y - 26, 3, lightBright(pc));
  }
}

// ============================================================================
// Lighting #13 : 8-Bit Runner（Retro Game Lighting 第1弾 3/3）
//
// ■ コンセプト
//   横スクロールアクションをモチーフにした自動デモです（実在ゲームの画像・
//   ロゴ・キャラクターデータは使用せず、赤い帽子＋青いオーバーオールの
//   ジェネリックな8bit風ランナーです）。ランナー自身は画面上の定位置で
//   走り続け、代わりに空・地面・ブロック・土管・雲がスクロールして流れる
//   ことで前進感を出します。ランナーは自動で敵や土管を飛び越え、目印の
//   アイテムブロックを下から叩いてコインを獲得しながら進み、一定距離ごとに
//   ステージが自然にリセットされます。
//
// ■ 音との関係（測定器ではない）
//   ・gViz.level でスクロール速度がわずかに上下する程度。
//
// ■ 共通ルール遵守
//   ・背景の矩形描画は lightFillRect を使用
//   ・上端48px（情報パネル）には描かない（MAR_TOP=SCENE_TOP）
//   ・顔は他のLightingと同じくコンポジタが最前面に描く
//   ・動的メモリ確保なし。固定長の静的配列／スカラーのみ
// ============================================================================
#define MAR_TOP        SCENE_TOP
#define MAR_GROUND_Y   (SCENE_H - 34)
#define MAR_BLOCK_Y    (MAR_GROUND_Y - 70)
#define MAR_PLAYER_X   70
#define MAR_OBST_MAX   8
#define MAR_LOOP_DIST  3000.0f
#define MAR_CLOUD_COUNT 5
#define MAR_JUMP_MS    480.0f
#define MAR_JUMP_H     30.0f

#define MAR_T_PIPE   0
#define MAR_T_ENEMY  1
#define MAR_T_BLOCK  2
#define MAR_T_ITEM   3
#define MAR_T_COIN   4

struct MarObst {
  float   worldX;
  uint8_t type;
  bool    triggered;   // ジャンプ誘発／コイン回収済みか（一度だけ処理）
  int     bumpTimer;   // アイテムブロックのバウンド演出用
};

static bool     marReady = false;
static float    marScrollX = 0.0f;
static uint32_t marRng = 0x87654321u;
static MarObst  marObst[MAR_OBST_MAX];
static float    marNextSpawnWorldX = 0.0f;
static int      marSpawnIdx = 0;
static int      marCoins = 0;
static bool     marPlayerJumping = false;
static unsigned long marJumpStartAt = 0;
static float    marRunPhase = 0.0f;

static inline uint32_t marRand()   { marRng = marRng * 1664525u + 1013904223u; return marRng; }
static inline float    marRand01() { return (float)(marRand() & 0xFFFF) / 65535.0f; }

static void marSpawnNext() {
  float gap = 90.0f + marRand01() * 110.0f;
  marNextSpawnWorldX += gap;
  int slot = marSpawnIdx % MAR_OBST_MAX;
  marSpawnIdx++;
  uint32_t r = marRand() % 100;
  uint8_t type = (r < 25) ? MAR_T_PIPE : (r < 50) ? MAR_T_ENEMY : (r < 65) ? MAR_T_ITEM : (r < 80) ? MAR_T_BLOCK : MAR_T_COIN;
  marObst[slot].worldX    = marNextSpawnWorldX;
  marObst[slot].type      = type;
  marObst[slot].triggered = false;
  marObst[slot].bumpTimer = 0;
}

static void marInitAll() {
  marScrollX         = 0.0f;
  marCoins           = 0;
  marPlayerJumping   = false;
  marRunPhase        = 0.0f;
  marNextSpawnWorldX = 200.0f;
  marSpawnIdx        = 0;
  for (int i = 0; i < MAR_OBST_MAX; i++) { marObst[i].worldX = -9999.0f; marObst[i].type = MAR_T_COIN; marObst[i].triggered = true; marObst[i].bumpTimer = 0; }
  for (int i = 0; i < MAR_OBST_MAX; i++) marSpawnNext();
  marReady = true;
}

static void marStartJump() { marPlayerJumping = true; marJumpStartAt = millis(); }

// 2026/07/27 改訂：8-Bit Runner主人公の見た目改善依頼への対応。
// 背景・横スクロール・ゲーム進行・呼び出しシグネチャ(px,py,legPhase,jumping)は
// 一切変更せず、キャラクター描画だけを「赤帽子＋黒髭＋肌色の顔＋赤いシャツ＋
// 青いオーバーオール＋濃色の靴」が低解像度でも見分けられるよう再構成した。
// 帽子とシャツを同系赤にする点は原作の配色どおりだが、間に肌色の顔（頭）が
// 挟まるため「同じ赤の塊」には見えない構図になっている。
static void marDrawPlayer(int px, int py, float legPhase, bool jumping) {
  const uint16_t RED_COL  = (uint16_t)(((216 & 0xF8) << 8) | ((30  & 0xFC) << 3) | (30  >> 3));   // 帽子・シャツ共通の赤
  const uint16_t OVER_COL = (uint16_t)(((40  & 0xF8) << 8) | ((80  & 0xFC) << 3) | (190 >> 3));   // 青いオーバーオール
  const uint16_t SKIN_COL = (uint16_t)(((250 & 0xF8) << 8) | ((205 & 0xFC) << 3) | (160 >> 3));   // 肌色
  const uint16_t SHOE_COL = (uint16_t)(((70  & 0xF8) << 8) | ((45  & 0xFC) << 3) | (25  >> 3));   // 濃茶の靴

  int legOff = jumping ? 4 : (int)(legPhase * 5.0f);
  int legAY  = py - 14 + (jumping ? 0 : legOff);
  int legBY  = py - 14 - (jumping ? 0 : legOff);

  // 靴（脚の下端4px分。走行モーションにそのまま追従）
  lightFillRect(px - 7, legAY + 10, 6, 4, SHOE_COL);
  lightFillRect(px + 1, legBY + 10, 6, 4, SHOE_COL);
  // 脚（青いオーバーオール。靴の上10px分）
  lightFillRect(px - 7, legAY, 6, 10, OVER_COL);
  lightFillRect(px + 1, legBY, 6, 10, OVER_COL);

  // 胴：赤いシャツを土台にし、中央へ青いオーバーオールのビブ＋サスペンダーを重ねる
  int torsoY = py - 30;
  int torsoH = 17;
  lightFillRect(px - 9, torsoY, 18, torsoH, RED_COL);
  lightFillRect(px - 5, torsoY + 4, 10, torsoH - 4, OVER_COL);   // ビブ（胸当て）
  lightFillRect(px - 5, torsoY,     2, 5, OVER_COL);              // サスペンダー左
  lightFillRect(px + 3, torsoY,     2, 5, OVER_COL);              // サスペンダー右

  // 腕：赤いシャツの袖＋肌色の手。ジャンプ中は右腕を高く突き上げるポーズにする。
  if (jumping) {
    lightFillRect(px - 13, torsoY - 2, 5, 10, RED_COL);
    lightFillRect(px + 8,  torsoY - 10, 5, 12, RED_COL);
    GFX.fillCircle(px - 11, torsoY + 8,  3, lightBright(SKIN_COL));
    GFX.fillCircle(px + 10, torsoY - 11, 3, lightBright(SKIN_COL));
  } else {
    lightFillRect(px - 13, torsoY + 2, 5, 10, RED_COL);
    lightFillRect(px + 8,  torsoY + 2, 5, 10, RED_COL);
    GFX.fillCircle(px - 11, torsoY + 12, 3, lightBright(SKIN_COL));
    GFX.fillCircle(px + 10, torsoY + 12, 3, lightBright(SKIN_COL));
  }

  // 顔
  GFX.fillCircle(px, py - 36, 9, lightBright(SKIN_COL));
  // 黒い髭（顔の下側）
  lightFillRect(px - 4, py - 34, 8, 3, BLACK);
  // 帽子（つば付き）
  lightFillRect(px - 10, py - 46, 20, 6, RED_COL);
  lightFillRect(px - 3,  py - 42, 13, 4, RED_COL);
}

void lightRenderMario(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!marReady) marInitAll();
  if (needsInit) marPlayerJumping = false;   // 短時間の再有効化ではワールド進行を消さない
  unsigned long now = millis();
  float audioBoost = 1.0f + gViz.level * 0.20f;
  float speed = 1.4f * audioBoost;

  const uint16_t SKY_COL     = (uint16_t)(((110 & 0xF8) << 8) | ((190 & 0xFC) << 3) | (250 >> 3));
  const uint16_t CLOUD_COL   = lightBright(WHITE);
  const uint16_t GROUND_TOP  = (uint16_t)(((70  & 0xF8) << 8) | ((200 & 0xFC) << 3) | (70  >> 3));
  const uint16_t GROUND_BODY = (uint16_t)(((170 & 0xF8) << 8) | ((110 & 0xFC) << 3) | (60  >> 3));
  const uint16_t GROUND_LINE = (uint16_t)(((120 & 0xF8) << 8) | ((75  & 0xFC) << 3) | (35  >> 3));
  const uint16_t PIPE_COL    = (uint16_t)(((50  & 0xF8) << 8) | ((180 & 0xFC) << 3) | (70  >> 3));
  const uint16_t PIPE_EDGE   = (uint16_t)(((25  & 0xF8) << 8) | ((120 & 0xFC) << 3) | (45  >> 3));
  const uint16_t BLOCK_COL   = (uint16_t)(((180 & 0xF8) << 8) | ((110 & 0xFC) << 3) | (60  >> 3));
  const uint16_t ITEM_COL    = (uint16_t)(((250 & 0xF8) << 8) | ((190 & 0xFC) << 3) | (30  >> 3));
  const uint16_t COIN_COL    = (uint16_t)(((255 & 0xF8) << 8) | ((215 & 0xFC) << 3) | (0   >> 3));
  const uint16_t ENEMY_COL   = (uint16_t)(((150 & 0xF8) << 8) | ((90  & 0xFC) << 3) | (40  >> 3));

  // ── 空 ──
  lightFillRect(0, MAR_TOP, SCENE_W, MAR_GROUND_Y - MAR_TOP, SKY_COL);

  // ── 雲（時間ベースの緩いパララックス。純粋に装飾）──
  for (int k = 0; k < MAR_CLOUD_COUNT; k++) {
    float sx = fmodf((float)now * 0.018f * (0.6f + 0.15f * (float)k) + (float)k * 150.0f, 420.0f) - 100.0f;
    int cy = MAR_TOP + 14 + (k % 3) * 16;
    GFX.fillCircle((int)sx,      cy,     10, CLOUD_COL);
    GFX.fillCircle((int)sx + 10, cy - 4, 8,  CLOUD_COL);
    GFX.fillCircle((int)sx - 10, cy - 3, 8,  CLOUD_COL);
  }

  // ── 地面（横縞がスクロールして流れる）──
  lightFillRect(0, MAR_GROUND_Y, SCENE_W, 6, GROUND_TOP);
  lightFillRect(0, MAR_GROUND_Y + 6, SCENE_W, SCENE_H - (MAR_GROUND_Y + 6), GROUND_BODY);
  int stripeOffset = ((int)marScrollX) % 20;
  for (int x = -stripeOffset; x < SCENE_W; x += 20) {
    lightFillRect(x, MAR_GROUND_Y + 6, 2, SCENE_H - (MAR_GROUND_Y + 6), GROUND_LINE);
  }

  // ── スクロール進行 ──
  marScrollX += speed;
  if (marScrollX >= MAR_LOOP_DIST) marInitAll();
  while (marNextSpawnWorldX - marScrollX < 400.0f) marSpawnNext();

  // ── 障害物／収集物の描画とトリガー判定 ──
  for (int i = 0; i < MAR_OBST_MAX; i++) {
    MarObst& o = marObst[i];
    float screenXf = o.worldX - marScrollX;
    if (screenXf < -60.0f || screenXf > 380.0f) continue;
    int sx = (int)lroundf(screenXf);
    bool groundObst = (o.type == MAR_T_PIPE || o.type == MAR_T_ENEMY);

    if (groundObst) {
      if (o.type == MAR_T_PIPE) {
        lightFillRect(sx - 16, MAR_GROUND_Y - 34, 32, 34, PIPE_COL);
        lightFillRect(sx - 19, MAR_GROUND_Y - 40, 38, 8,  PIPE_EDGE);
      } else {
        GFX.fillCircle(sx, MAR_GROUND_Y - 6, 8, lightBright(ENEMY_COL));
        lightFillRect(sx - 8, MAR_GROUND_Y - 2, 6, 4, ENEMY_COL);
        lightFillRect(sx + 2, MAR_GROUND_Y - 2, 6, 4, ENEMY_COL);
        GFX.fillCircle(sx - 3, MAR_GROUND_Y - 8, 2, lightBright(WHITE));
        GFX.fillCircle(sx + 3, MAR_GROUND_Y - 8, 2, lightBright(WHITE));
      }
    } else if (o.type == MAR_T_BLOCK) {
      lightFillRect(sx - 12, MAR_BLOCK_Y - 12, 24, 24, BLOCK_COL);
    } else if (o.type == MAR_T_ITEM) {
      int by = MAR_BLOCK_Y - 12 - (o.bumpTimer > 0 ? (o.bumpTimer > 4 ? 4 : o.bumpTimer) : 0);
      if (o.bumpTimer > 0) o.bumpTimer--;
      lightFillRect(sx - 12, by, 24, 24, o.triggered ? BLOCK_COL : ITEM_COL);
      if (!o.triggered) {
        GFX.setTextSize(2);
        GFX.setTextColor(lightBright(BLACK));
        GFX.drawString("?", sx - 6, by + 3);
      }
    } else if (o.type == MAR_T_COIN) {
      if (!o.triggered) GFX.fillCircle(sx, MAR_BLOCK_Y - 4, 6, lightBright(COIN_COL));
    }

    // ── トリガー判定：プレイヤー手前の一定区間に入ったら自動ジャンプ／回収 ──
    if (!o.triggered && sx > MAR_PLAYER_X + 26 && sx < MAR_PLAYER_X + 54 && !marPlayerJumping) {
      o.triggered = true;
      marStartJump();
      if (o.type == MAR_T_ITEM) { o.bumpTimer = 8; marCoins++; }
      if (o.type == MAR_T_COIN) { marCoins++; }
    }
  }

  // ── プレイヤー（自動ジャンプ＋走行アニメ）──
  float jumpOffset = 0.0f;
  if (marPlayerJumping) {
    float t = (float)(now - marJumpStartAt) / MAR_JUMP_MS;
    if (t >= 1.0f) { marPlayerJumping = false; }
    else            jumpOffset = sinf(t * 3.14159f) * MAR_JUMP_H;
  }
  marRunPhase += 0.35f * audioBoost;
  int py = MAR_GROUND_Y - (int)jumpOffset;
  marDrawPlayer(MAR_PLAYER_X, py, sinf(marRunPhase), marPlayerJumping);

  // ── コインカウンタ ──
  GFX.setTextSize(1);
  GFX.setTextColor(lightBright((uint16_t)(((255 & 0xF8) << 8) | ((215 & 0xFC) << 3) | (0 >> 3))));
  GFX.drawString("COIN x" + String(marCoins), 8, MAR_TOP + 4);
}

// ============================================================================
// Lighting #14 : Missile Defense（v1.0 / 2026-07-27）
//
// ■ コンセプト
//   1980年代のアーケードゲーム『Missile Command』を連想させる自動迎撃デモです。
//   実在ゲームの再現ではなく、Laser Show（第二弾）の発展形という位置づけの
//   Retro Game Lighting。プレイヤー操作・弾薬数・勝敗・ゲームオーバーは持たず、
//   敵ミサイル（常に2〜3本）が飛来 → 照準が追尾 → ロックオン → 地上3基地の
//   いずれかから迎撃レーザー → 命中・爆発・煙 → 次の標的へ、というサイクルを
//   無限に繰り返します。迎撃しても総数は減らさず、都度補充して演出を継続します。
//
// ■ レイヤー種別＝【背景(面)】
//   Asteroid Field / Tempest Tunnel と同じく、黒背景の上へワイヤーフレーム調の
//   要素を自前で毎フレーム全面描画する完結型の背景演出です。Laser Show
//   （オーバーレイ）とは独立した別モードで、Laser Show自体のコード・状態
//   （laserBeam[]・laserCount 等）には一切触れません（見た目の着想のみ流用）。
//
// ■ 軌跡の表現
//   各敵ミサイルは「発射点(x0,y0)→現在位置(x,y)」の直線を毎フレーム引き直す
//   だけで「先端が伸びていく軌跡」を表現する（点の履歴配列は持たない）。
//   迎撃で敵ミサイルを非アクティブにした瞬間、この直線ごと描画されなくなる
//   ＝ミサイル本体だけでなく軌跡も同時に消える。
//
// ■ 照準
//   1つのクロスヘアが「探索中→追尾中→ロックオン後のクールダウン」の3状態を
//   遷移する。追尾中は現在の標的の位置へ一定係数で滑らかに近づき（瞬間移動
//   しない）、距離が閾値以下になった瞬間にロックオンして即座に迎撃レーザーを
//   発射する。標的は「最も地面に近い（＝最も危険な）敵」を優先して選ぶ。
//
// ■ 迎撃・命中・爆発煙
//   ロックオンした瞬間、3基地のうち標的のx位置に最も近い基地から
//   ターゲットへ向かう短寿命の迎撃ラインを描画し、同時にその敵ミサイルを
//   非アクティブ化（＝本体と軌跡を消す）して命中地点で爆発を開始する。
//   爆発は共有プール(MSL_BURST_COUNT)で管理し、経過時間で
//     0〜MSL_BURST_FLASH_MS   … 明るい爆発（拡大する円）
//     以降〜MSL_BURST_LIFE_MS … 煙（背景色へ寄せながら縮小して消える）
//   の2段階に遷移する。敵ミサイルが迎撃されず地上へ到達した場合も同じ爆発
//   シーケンスで静かに消え、新しい敵ミサイルに置き換わる（ゲームオーバー扱い
//   にはしない）。
// ============================================================================
#define MSL_TOP              48     // 上端の情報パネル高さ（ここには描かない）
#define MSL_GROUND_Y        206     // 地面の上端Y
#define MSL_MAX_ENEMY         3     // 同時に存在する敵ミサイルの最大数（2〜3本）
#define MSL_BURST_COUNT       4     // 爆発／煙の同時プール数
// 2026-07-28: 実機確認で爆発がほぼ視認できなかったため、表示だけを調整（迎撃・追尾・
//   ロックオン等のロジックは無変更）。詳細は mslStartBurst()/mslDrawBurst() 直上のコメント参照。
//   Lighting合成周期は LIGHT_COMPOSITE_MS(=90ms) のため、旧値(140ms/650ms)では
//   明るい爆発フェーズが1フレームも描かれずに終わることがあった。90msの数倍の
//   長さを確保し、確実に複数フレーム描画されるようにする。
#define MSL_BURST_FLASH_MS  260     // 爆発（明るい円が拡大する）フェーズの時間
#define MSL_BURST_LIFE_MS   900     // 爆発＋煙をあわせた寿命
#define MSL_BURST_MAX_R       34    // 爆発の最大半径（px）。かりポムの目(半径22)より
                                     // ひとまわり大きくし、顔と重なっても輪郭がはみ出て見えるようにする
#define MSL_LOCK_DIST        9.0f   // 照準がこの距離まで近づいたらロックオン
#define MSL_SHOT_DURATION_MS 130    // 迎撃レーザーの表示時間
#define MSL_REACQUIRE_MIN_MS 350    // ロックオン後、次の標的探索を始めるまでの間
#define MSL_REACQUIRE_MAX_MS 900

// 黒い空・黄色い地面・青い敵ミサイル/軌跡・緑の基地・白いクロスヘア・迎撃レーザー・爆発色
static const uint16_t MSL_SKY_COL     = 0x0000;                                                              // 黒
static const uint16_t MSL_GROUND_COL  = (uint16_t)(((255 & 0xF8) << 8) | ((210 & 0xFC) << 3) | (0   >> 3));  // 黄
static const uint16_t MSL_MISSILE_COL = (uint16_t)((( 60 & 0xF8) << 8) | ((160 & 0xFC) << 3) | (255 >> 3));  // 青
static const uint16_t MSL_BASE_COL    = (uint16_t)((( 40 & 0xF8) << 8) | ((220 & 0xFC) << 3) | (90  >> 3));  // 緑
static const uint16_t MSL_AIM_COL     = 0xFFFF;                                                              // 白
static const uint16_t MSL_SHOT_COL    = (uint16_t)(((200 & 0xF8) << 8) | ((255 & 0xFC) << 3) | (255 >> 3));  // 明るい水色
static const uint16_t MSL_FLASH_COL   = 0xFFFF;                                                              // 爆発フラッシュ＝白
static const uint16_t MSL_FLASH_COL2  = (uint16_t)(((255 & 0xF8) << 8) | ((160 & 0xFC) << 3) | (0   >> 3));  // 橙
// 2026-07-28: 煙の色を「橙→黒」から「橙→明るいグレー」へ変更。
//   黒い空の上では橙→黒でも見えるが、黄色い地面（地面到達／地面付近での迎撃）の上では
//   橙が地面の黄色に近く、縮小しながら埋もれて見えなくなっていた。明るいグレーなら
//   黒い空・黄色い地面のどちらの上でも最後まで輪郭がはっきり見える。
static const uint16_t MSL_SMOKE_COL   = (uint16_t)(((190 & 0xF8) << 8) | ((190 & 0xFC) << 3) | (195 >> 3));  // 明るいグレー

struct MslEnemy {
  bool  active;
  float x0, y0;     // 発射点（上空）
  float tx, ty;     // 目標地点（地面）
  float x, y;       // 現在位置（軌跡の先端）
  float progress;   // 0.0（発射点）〜1.0（目標地点）
  float speed;      // 1フレームあたりのprogress増分
};
struct MslBurst {
  bool active;
  float x, y;
  unsigned long startMs;
};

static MslEnemy mslEnemy[MSL_MAX_ENEMY];
static MslBurst mslBurst[MSL_BURST_COUNT];
static uint32_t mslRng = 0xC001D00Du;

// 2026-08-09追加：最背面のカラフルな1ドット星（ギャラクシアン／ムーンクレスタ等の
// 80年代アーケードゲームを思わせる宇宙背景へのオマージュ）。黒い空(MSL_SKY_COL)の
// 範囲だけに配置し、既存の基地・敵ミサイル・照準・迎撃・爆発より必ず先に描く
// （最背面）。位置・色は一度だけ決め、以後は固定のまま使い回す（mslResetState()の
// 対象には含めない＝Lighting再開のたびに位置が変わらず安定して見える）。
#define MSL_CSTAR_COUNT 16
static int16_t  mslCStarX[MSL_CSTAR_COUNT], mslCStarY[MSL_CSTAR_COUNT];
static uint16_t mslCStarCol[MSL_CSTAR_COUNT];
static bool     mslStarReady = false;

// 地上基地（左・中央・右）。原作を連想させる程度の小さな印なので座標は固定でよい。
static const int MSL_BASE_X[3] = { 54, 160, 266 };

// 照準の状態
enum MslAimState : uint8_t { MSL_AIM_SEARCH = 0, MSL_AIM_TRACK, MSL_AIM_COOLDOWN };
static uint8_t       mslAimState    = MSL_AIM_SEARCH;
static float         mslAimX        = 160.0f, mslAimY = 90.0f;
static int8_t        mslAimTarget   = -1;     // 追尾中のmslEnemyインデックス（-1=なし）
static unsigned long mslReacquireAt = 0;      // COOLDOWN終了予定時刻

// 迎撃レーザー（ロックオン即発射のため、同時1本ぶんの短寿命状態だけ保持すればよい）
static bool          mslShotActive   = false;
static float         mslShotX0, mslShotY0, mslShotX1, mslShotY1;
static unsigned long mslShotStartMs  = 0;

static inline uint32_t mslRand()   { mslRng = mslRng * 1664525u + 1013904223u; return mslRng; }
static inline float    mslRand01() { return (float)(mslRand() >> 8) / 16777216.0f; }  // 0..1

// 敵ミサイル1本を新規発射する（発射点・目標点・速度をランダムに決める）。
static void mslSpawnEnemy(uint8_t i) {
  MslEnemy& e = mslEnemy[i];
  e.x0 = 20.0f + mslRand01() * 280.0f;
  e.y0 = (float)MSL_TOP + 4.0f + mslRand01() * 18.0f;   // 上空の発射点（情報パネル直下〜少し下）
  e.tx = 20.0f + mslRand01() * 280.0f;
  e.ty = (float)MSL_GROUND_Y;
  e.x  = e.x0; e.y = e.y0;
  e.progress = 0.0f;
  e.speed = 0.0028f + mslRand01() * 0.0022f;   // 個体差のある飛来速度
  e.active = true;
}

// 最背面のカラフルな星を一度だけ生成する（黒い空の範囲のみ。赤・白・黄・緑の4色）。
// mslResetState()（Lighting再開のたびに呼ばれる）とは独立させ、星の位置が
// 毎回変わらず安定して見えるようにしている。
static void mslBuildStars() {
  const uint16_t MSL_CSTAR_PALETTE[4] = { RED, WHITE, YELLOW, GREEN };
  for (int i = 0; i < MSL_CSTAR_COUNT; i++) {
    mslCStarX[i]   = (int16_t)(mslRand01() * 320.0f);
    mslCStarY[i]   = (int16_t)((float)MSL_TOP + mslRand01() * (float)(MSL_GROUND_Y - MSL_TOP));
    mslCStarCol[i] = MSL_CSTAR_PALETTE[mslRand() % 4];
  }
  mslStarReady = true;
}

static void mslResetState() {
  for (uint8_t i = 0; i < MSL_MAX_ENEMY; i++) {
    mslSpawnEnemy(i);
    // 起動直後に全機が同時に発射点へ揃わないよう、飛来の進み具合を分散させる。
    mslEnemy[i].progress = mslRand01() * 0.5f;
    mslEnemy[i].x = mslEnemy[i].x0 + (mslEnemy[i].tx - mslEnemy[i].x0) * mslEnemy[i].progress;
    mslEnemy[i].y = mslEnemy[i].y0 + (mslEnemy[i].ty - mslEnemy[i].y0) * mslEnemy[i].progress;
  }
  for (uint8_t i = 0; i < MSL_BURST_COUNT; i++) mslBurst[i].active = false;
  mslAimState    = MSL_AIM_SEARCH;
  mslAimTarget   = -1;
  mslAimX = 160.0f; mslAimY = 90.0f;
  mslReacquireAt = 0;
  mslShotActive  = false;
}

// 爆発／煙を1つ起動する（空きスロットが無ければ最も古いものを上書きし表示不整合を避ける）。
// 2026-07-28: 呼び出し元（lightRenderMissile）がそのフレームで既に取得済みの now を
//   そのまま受け取るよう変更した。以前はここで millis() を再度呼んでいたため、
//   ごく稀に呼び出し元の now よりわずかに後の時刻が startMs に入り、同じフレームの
//   爆発描画ループで (now - startMs) が符号なし整数の負方向オーバーフローを起こし、
//   生成した瞬間の爆発が1フレーム分描かれずに終わる場合があった（表示のみの不具合、
//   命中判定やロックオン等のロジックには影響していなかった）。
static void mslStartBurst(float x, float y, unsigned long now) {
  int slot = -1;
  unsigned long oldest = 0xFFFFFFFFu;
  for (uint8_t i = 0; i < MSL_BURST_COUNT; i++) {
    if (!mslBurst[i].active) { slot = (int)i; break; }
    if (mslBurst[i].startMs < oldest) { oldest = mslBurst[i].startMs; slot = (int)i; }
  }
  mslBurst[slot].active  = true;
  mslBurst[slot].x       = x;
  mslBurst[slot].y       = y;
  mslBurst[slot].startMs = now;
}

// 基地3ヶ所のうち、標的のx位置に最も近い基地を選ぶ（毎回中央固定にしない）。
static uint8_t mslPickBase(float targetX) {
  uint8_t best = 0;
  float bestD = fabsf(targetX - (float)MSL_BASE_X[0]);
  for (uint8_t k = 1; k < 3; k++) {
    float d = fabsf(targetX - (float)MSL_BASE_X[k]);
    if (d < bestD) { bestD = d; best = k; }
  }
  return best;
}

// 大きく描かない：黄色い地面の上に小さな三角の印が見える程度でよい。
static void mslDrawBase(int x) {
  lightDrawLine(x - 6, MSL_GROUND_Y, x + 6, MSL_GROUND_Y, MSL_BASE_COL);
  lightDrawLine(x - 6, MSL_GROUND_Y, x,     MSL_GROUND_Y - 7, MSL_BASE_COL);
  lightDrawLine(x + 6, MSL_GROUND_Y, x,     MSL_GROUND_Y - 7, MSL_BASE_COL);
}

static void mslDrawCrosshair(int x, int y) {
  lightDrawLine(x - 7, y, x - 2, y, MSL_AIM_COL);
  lightDrawLine(x + 2, y, x + 7, y, MSL_AIM_COL);
  lightDrawLine(x, y - 7, x, y - 2, MSL_AIM_COL);
  lightDrawLine(x, y + 2, x, y + 7, MSL_AIM_COL);
  GFX.drawRect(x - 3, y - 3, 6, 6, lightBright(MSL_AIM_COL));
}

// 爆発→煙の1フレーム分を描く。経過時間で見た目を段階的に変える。
// 【重要】Arduino IDEの自動プロトタイプ生成は、struct定義より前（ファイル先頭）に
//   関数プロトタイプを挿入する。引数にカスタムstruct（MslBurst）を直接使うと、
//   その自動生成プロトコルの時点ではまだ struct MslBurst が定義されておらず
//   "MslBurst does not name a type" でコンパイルエラーになる（実機で確認済みの不具合）。
//   そのため他のLighting（例：astrDrawWireframe(int i)）と同じく、struct参照ではなく
//   配列インデックスを引数に取る形にする。
// 2026-07-28: 実機で「爆発がほぼ見えない」問題を受けて表示だけを見直した。
//   ・半径を最大34pxまで拡大（かりポムの目=半径22より大きくし、顔パーツの上に
//     命中しても輪郭がはみ出て視認できるようにする。顔は合成の最前面に来る既存仕様
//     のため完全な重なりは解消できないが、視認性は大きく改善する）。
//   ・煙の色を「橙→黒」から「橙→明るいグレー」へ変更（黄色い地面の上で橙が
//     埋もれていた問題への対処。詳細は MSL_SMOKE_COL 定義のコメント参照）。
//   ・上端の情報パネル(y<48)へ絶対にはみ出さないよう、Laser Show等と同様に
//     描画半径を明示的にクランプする（既存の保護領域は変更しない）。
static void mslDrawBurst(uint8_t i, unsigned long now) {
  MslBurst& b = mslBurst[i];
  unsigned long el = now - b.startMs;
  int by = (int)lroundf(b.y);
  if (el < MSL_BURST_FLASH_MS) {
    // 明るい爆発：白→橙で急拡大する円
    float t = (float)el / (float)MSL_BURST_FLASH_MS;   // 0..1
    int r = 3 + (int)(t * (float)(MSL_BURST_MAX_R - 3));
    if (by - r < MSL_TOP) r = by - MSL_TOP;   // 上端パネルへはみ出さないようクランプ
    if (r < 1) return;
    uint16_t col = light565Lerp(MSL_FLASH_COL, MSL_FLASH_COL2, (int)(t * 255.0f));
    GFX.fillCircle((int)b.x, by, r, lightBright(col));
  } else if (el < MSL_BURST_LIFE_MS) {
    // 煙：橙→明るいグレーへ徐々に寄せながら、じわっと縮む
    float t = (float)(el - MSL_BURST_FLASH_MS) / (float)(MSL_BURST_LIFE_MS - MSL_BURST_FLASH_MS); // 0..1
    int r = MSL_BURST_MAX_R - (int)(t * (float)(MSL_BURST_MAX_R - 6));
    if (r < 1) r = 1;
    if (by - r < MSL_TOP) r = by - MSL_TOP;   // 上端パネルへはみ出さないようクランプ
    if (r < 1) return;
    uint16_t col = light565Lerp(MSL_FLASH_COL2, MSL_SMOKE_COL, (int)(t * 255.0f));
    GFX.fillCircle((int)b.x, by, r, lightBright(col));
  }
}

void lightRenderMissile(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!mslStarReady) mslBuildStars();
  if (needsInit) mslResetState();

  unsigned long now = millis();

  // ── 背景：黒い空＋黄色い地面（原作の第一印象を再現するシンプルな2色構成）──
  lightFillRect(0, MSL_TOP, 320, MSL_GROUND_Y - MSL_TOP, MSL_SKY_COL);
  lightFillRect(0, MSL_GROUND_Y, 320, 240 - MSL_GROUND_Y, MSL_GROUND_COL);

  // ── 最背面のカラフルな星（80年代アーケード風。黒い空にのみ配置し、既存の基地・
  //     敵ミサイル・照準・迎撃・爆発より必ず先に描く）──
  for (int i = 0; i < MSL_CSTAR_COUNT; i++) {
    lightFillRect(mslCStarX[i], mslCStarY[i], 1, 1, mslCStarCol[i]);
  }

  // ── 地上基地（左・中央・右）──
  for (uint8_t k = 0; k < 3; k++) mslDrawBase(MSL_BASE_X[k]);

  // ── 敵ミサイル：移動・軌跡描画・地面到達判定 ──
  for (uint8_t i = 0; i < MSL_MAX_ENEMY; i++) {
    MslEnemy& e = mslEnemy[i];
    if (!e.active) continue;
    e.progress += e.speed;
    if (e.progress >= 1.0f) {
      // 迎撃されずに地面へ到達：ゲームオーバーにはせず、静かに爆発だけ残して補充する。
      mslStartBurst(e.tx, e.ty, now);
      e.active = false;
      if (mslAimTarget == (int8_t)i) { mslAimTarget = -1; mslAimState = MSL_AIM_SEARCH; }
      continue;
    }
    e.x = e.x0 + (e.tx - e.x0) * e.progress;
    e.y = e.y0 + (e.ty - e.y0) * e.progress;
    // 軌跡＝発射点→現在位置の直線。敵ミサイルが消えれば同時にこの線も描かれなくなる。
    lightDrawLine((int)lroundf(e.x0), (int)lroundf(e.y0), (int)lroundf(e.x), (int)lroundf(e.y), MSL_MISSILE_COL);
  }

  // ── 敵ミサイルの補充（常に2〜3本が飛来している状態を保つ）──
  for (uint8_t i = 0; i < MSL_MAX_ENEMY; i++) {
    if (!mslEnemy[i].active) mslSpawnEnemy(i);
  }

  // ── 照準：探索中→追尾中→ロックオン(即発射)→再探索までのクールダウン ──
  if (mslAimState == MSL_AIM_COOLDOWN) {
    if (now >= mslReacquireAt) mslAimState = MSL_AIM_SEARCH;
  }
  if (mslAimState == MSL_AIM_SEARCH) {
    // 最も地面に近い（progressが大きい）敵を優先的に狙う＝一番危険な標的から迎撃する。
    int8_t best = -1;
    float bestProg = -1.0f;
    for (uint8_t i = 0; i < MSL_MAX_ENEMY; i++) {
      if (mslEnemy[i].active && mslEnemy[i].progress > bestProg) { bestProg = mslEnemy[i].progress; best = (int8_t)i; }
    }
    if (best >= 0) { mslAimTarget = best; mslAimState = MSL_AIM_TRACK; }
  }
  if (mslAimState == MSL_AIM_TRACK) {
    if (mslAimTarget < 0 || !mslEnemy[mslAimTarget].active) {
      mslAimTarget = -1; mslAimState = MSL_AIM_SEARCH;
    } else {
      MslEnemy& t = mslEnemy[mslAimTarget];
      // 瞬間移動させず、標的へ滑らかに近づける（追尾していると分かる動き）。
      mslAimX += (t.x - mslAimX) * 0.12f;
      mslAimY += (t.y - mslAimY) * 0.12f;
      float dx = t.x - mslAimX, dy = t.y - mslAimY;
      if ((dx * dx + dy * dy) <= (MSL_LOCK_DIST * MSL_LOCK_DIST)) {
        // ロックオン→即座に迎撃レーザー発射（標的に最も近い基地を選ぶ）
        uint8_t base = mslPickBase(t.x);
        mslShotX0 = (float)MSL_BASE_X[base]; mslShotY0 = (float)(MSL_GROUND_Y - 7);
        mslShotX1 = t.x; mslShotY1 = t.y;
        mslShotActive   = true;
        mslShotStartMs  = now;
        // 命中：敵ミサイルを非アクティブ化＝本体と軌跡が同時に消え、命中地点に爆発を残す。
        mslStartBurst(t.x, t.y, now);
        t.active = false;
        mslAimTarget = -1;
        mslAimState  = MSL_AIM_COOLDOWN;
        mslReacquireAt = now + MSL_REACQUIRE_MIN_MS
                        + (unsigned long)(mslRand01() * (float)(MSL_REACQUIRE_MAX_MS - MSL_REACQUIRE_MIN_MS));
      }
    }
  }
  mslDrawCrosshair((int)lroundf(mslAimX), (int)lroundf(mslAimY));

  // ── 迎撃レーザー（短寿命の1本だけ・常時発射にはしない）──
  if (mslShotActive) {
    if (now - mslShotStartMs < MSL_SHOT_DURATION_MS) {
      lightDrawLine((int)lroundf(mslShotX0), (int)lroundf(mslShotY0),
                    (int)lroundf(mslShotX1), (int)lroundf(mslShotY1), MSL_SHOT_COL);
    } else {
      mslShotActive = false;
    }
  }

  // ── 爆発／煙 ──
  for (uint8_t i = 0; i < MSL_BURST_COUNT; i++) {
    if (!mslBurst[i].active) continue;
    unsigned long el = now - mslBurst[i].startMs;
    if (el >= MSL_BURST_LIFE_MS) { mslBurst[i].active = false; continue; }
    mslDrawBurst(i, now);
  }
}

// ============================================================================
// Lighting #15 : Psychedelic / Trance（v1.0 / 2026-07-30）
//
// ■ コンセプト
//   「綺麗」より「強烈」／「連続性」より「予測不能」／「眺める」より「目に飛び込んでくる」。
//   1つの模様を眺める演出ではなく、視覚世界そのものを次々に切り替えるモンタージュ。
//   Kaleidoscope（対称・秩序・連続モーフ）とは意図的に真逆へ振ってある。
//
// ■ 三層構造（本Lightingの背骨）
//   ┌─ MOTION 8〜16フレーム(0.72〜1.44s) ─┐  ┌ FLASH連射 ┐  ┌ MOTION ┐
//   │渦渦渦[割込]渦渦[反転]渦渦渦渦渦渦渦  │→│格子 点 縞 │→│輪輪輪…│
//   └──────────────────────────────────────┘  └───────────┘  └────────┘
//     ・MOTION … 動きを目で追える主役。5種をシャッフルバッグで回すため、
//                約10秒あれば5つの視覚世界すべてが必ず1回以上登場する。
//     ・FLASH  … 静止の一撃。1〜3フレーム(90〜270ms)を2〜5連射し、
//                そのたびに配色世界が丸ごと入れ替わる。
//     ・ACCENT … 1〜2フレーム(90〜180ms)の割り込み。MOTIONの途中へ突然入る。
//                【重要】割り込み中もMOTIONの状態は裏で進み続けるため、
//                戻ってきたときには渦が「跳んで」いる＝ジャンプカットになる。
//     ・MOTIONは終盤ほど加速する（speedRamp）。最も盛り上がった瞬間に断ち切られ、
//       次のFLASH連射へ落ちる（トランスのビルドアップ→ドロップに相当）。
//
// ■ 音声非依存（設計要件）
//   gViz.band[] / gViz.level / gViz.bass を【一切参照しない】。
//   音が無くても演出単体で完全に成立する。将来の音同期は
//   「ショット切替のトリガに低音の立ち上がりを混ぜる」だけで追加できる。
//
// ■ 上部48px・顔
//   全描画を GFX.setClipRect(0, PSY_TOP, 320, 240-PSY_TOP) で囲む
//   （Kaleidoscopeと同じ手法。共通の scenePush()/合成パイプラインは無変更）。
//   顔（目・まつ毛・鼻・口）は従来どおりコンポジタが最後に描くので常に最前面。
//   Faceショット中も、かりポムの目鼻口が上に重なる＝「顔の上に顔」の不穏な二重像
//   として意図的に利用する（Faceは必ず拡大・位置ずらしして別スケールにする）。
//
// ■ 負荷
//   全画素ループでの sqrt/atan2/sin/cos は一切なし。
//   毎フレームの三角関数は「少数の頂点」に対してのみ（最大でも約400回）。
//   新規の大型Canvasは Faceキャッシュ(160x120x4枚/PSRAM)のみ。
// ============================================================================
#define PSY_TOP  SCENE_TOP     // 48。上部の情報パネルには絶対に描かない

// ── 層の種別 ───────────────────────────────────────────────
enum : uint8_t { PSY_LAYER_FLASH = 0, PSY_LAYER_MOTION = 1 };
// MOTION 5種（シャッフルバッグで回す）
enum : uint8_t { PSY_M_RINGS = 0, PSY_M_SPIRAL, PSY_M_MOIRE, PSY_M_WEDGE, PSY_M_SHARD, PSY_M_COUNT };
// FLASH 4種（静止の一撃）
enum : uint8_t { PSY_F_OPGRID = 0, PSY_F_DOTS, PSY_F_HATCH, PSY_F_WOBBLE, PSY_F_COUNT };
// ACCENT 5種（割り込み）
enum : uint8_t { PSY_A_FACE = 0, PSY_A_INVERT, PSY_A_STAB, PSY_A_GIANT, PSY_A_SCAN, PSY_A_COUNT };

// ── 各層の寿命（フレーム数。1フレーム = LIGHT_COMPOSITE_MS = 90ms）────────
// LIGHT_COMPOSITE_MS は【変更していない】。ここはこのLighting内部の寿命管理のみ。
#define PSY_FLASH_MIN_F    1     //  90ms
#define PSY_FLASH_MAX_F    3     // 270ms
#define PSY_MOTION_MIN_F   8     // 720ms
#define PSY_MOTION_MAX_F  16     // 1440ms（実測で1.44秒を超えると単調に感じたため上限）
#define PSY_SHARD_MAX_F   11     // 破片は画面が空になる前に断ち切る
#define PSY_ACCENT_MIN_F   1     //  90ms
#define PSY_ACCENT_MAX_F   2     // 180ms
#define PSY_BURST_MIN      2     // FLASH連射の本数
#define PSY_BURST_MAX      5
#define PSY_INTR_PCT      11     // MOTION中にACCENTが割り込む確率(%)
#define PSY_INVERT_PCT     7     // MOTION中に補色反転が入る確率(%)
#define PSY_RAMP        1.25f    // 終盤の加速量。ramp = 1 + PSY_RAMP * (idx/frames)^2.2
#define PSY_FACE_MIN_GAP   3     // Faceショットの最低間隔（ショット数）＝連続表示の禁止
#define PSY_FACE_FORCE_GAP 8     // これだけ空いたら強制的にFaceを出す

// ── 12パレット（BG / MAIN / ACCENT / 暗いか）──────────────────────
// 原色・補色・高彩度のみ。パステルで綺麗にまとめない（設計要件）。
//
// 【重要】struct PsyPal の定義は【ファイル先頭（logFirmwareInfo() の直前）】にある。
//   Arduino IDE が自動生成する関数プロトタイプは「最初の関数定義の直前」へ挿入されるため、
//   PsyPal を引数に取る関数（psyFlashOpGrid 等）のプロトタイプがそこへ差し込まれた時点で
//   型が既知でないと 'PsyPal' does not name a type でコンパイルが止まる。
//   詳細な理由はファイル先頭の対策コメントを参照。ここには配色テーブルの実体だけを置く。
static const PsyPal PSY_PAL[12] = {
  { 0x0000, 0xF800, 0xFFE0, 0 },   //  0 赤×黄
  { 0x001F, 0xFD20, 0xFFFF, 0 },   //  1 青×橙
  { 0x2806, 0xF81F, 0x07FF, 0 },   //  2 マゼンタ×シアン
  { 0x0000, 0x07E0, 0x780F, 0 },   //  3 緑×紫
  { 0xF800, 0x0000, 0xFFFF, 1 },   //  4 赤×黒
  { 0xFFE0, 0x001F, 0xF800, 0 },   //  5 黄×青
  { 0xFFFF, 0x0000, 0xF800, 0 },   //  6 白×黒
  { 0xFC00, 0x300A, 0x07FF, 1 },   //  7 橙×濃紫
  { 0xAFE5, 0xF816, 0x0000, 0 },   //  8 ライム×マゼンタ
  { 0x07FF, 0x000C, 0xFFE0, 1 },   //  9 シアン×濃青
  { 0xF992, 0x01C3, 0xFFE0, 0 },   // 10 ピンク×深緑
  { 0x0007, 0xFFFF, 0xF800, 1 },   // 11 黒×白×赤
};

// Expanding Rings 用の色スペクトル（12色・高彩度）。
// パレットの3色だけだと単調になるため、ショットごとにこの環から
// 連続する5〜7色の「窓」を切り出して色プールを作る（＝複数色が不均一に流れ込む）。
static const uint16_t PSY_SPECTRUM[12] = {
  0xF800, 0xFB00, 0xFD20, 0xFFE0, 0xAFE5, 0x07E0,
  0x07F5, 0x07FF, 0x041F, 0x300F, 0xA81F, 0xF81F
};

// ════════════════════════════════════════════════════════════════════════
// ★ Expanding Rings 実機チューニング用定数（ここだけ触れば絵の濃さを調整できる）
//
//   「多重円の洪水」をもっと濃くしたい → PSY_RING_MAX を 12→14、
//                                        PSY_RING_SPAWN_LOW を 3→4、
//                                        PSY_RING_KILL_R を 215→190
//   描画が重い（90msに収まらない）     → PSY_RING_MAX を 12→8、
//                                        PSY_RING_W_FAT_MAX を 15→10、
//                                        PSY_RING_KILL_R を 215→180
//   ※ PSY_RING_MAX を増やすときは PsyRing 配列サイズ(=PSY_RING_MAX)も自動追従する。
// ════════════════════════════════════════════════════════════════════════
#define PSY_RING_MAX         12     // 同時に存在できる輪の本数（8〜14で調整可）
#define PSY_RING_PRELOAD     10     // 初期化時に仕込む本数（1コマ目から画面を埋める）
#define PSY_RING_SPAWN_LOW    3     // 本数が少ないとき1フレームに生む本数
#define PSY_RING_SPAWN_HI     2     // 本数が足りているとき1フレームに生む本数
#define PSY_RING_LOW_COUNT    7     // この本数未満なら SPAWN_LOW を使う
#define PSY_RING_KILL_R     215.0f  // この半径を超えた輪は破棄（画面外まで生かさない＝スロットを空ける）
#define PSY_RING_R0           3.5f  // 生まれたときの半径
#define PSY_RING_V0           5.5f  // 生まれたときの拡大速度(px/frame)
#define PSY_RING_GRO_MIN      1.030f// 1本ごとに違う加速率（迫ってくる遠近感）
#define PSY_RING_GRO_MAX      1.075f
#define PSY_RING_V_CLAMP     34.0f  // 拡大速度の上限（一瞬で飛び去るのを防ぐ）
#define PSY_RING_JITTER      30.0f  // 「生まれる場所」のばらつき(px)＝同心円にしないための要
#define PSY_RING_DRIFT_X      1.8f  // 拡大しながら輪自身の中心が流れる速度
#define PSY_RING_DRIFT_Y      1.3f
#define PSY_RING_ELL_MIN      0.62f // 楕円率（真円にしない）
#define PSY_RING_ELL_MAX      1.48f
#define PSY_RING_BIRTH_VX     2.6f  // 発生中心そのもののドリフト速度
#define PSY_RING_BIRTH_VY     1.8f
// 線幅は3クラスを不均一に混ぜる（細い輪／普通の輪／極端に太い輪）
#define PSY_RING_W_THIN_MIN   1
#define PSY_RING_W_THIN_MAX   2
#define PSY_RING_W_MID_MIN    3
#define PSY_RING_W_MID_MAX    6
#define PSY_RING_W_FAT_MIN    9
#define PSY_RING_W_FAT_MAX   15
#define PSY_RING_W_THIN_PCT  30     // 出現率(%)：細30 / 普通45 / 極太25
#define PSY_RING_W_MID_PCT   45
#define PSY_RING_POOL_MAX     9     // 1ショットあたりの色プール上限
#define PSY_RING_RUN_PCT     35     // 同じ色が2〜3本続く確率(%)＝色が“流れ込む”ムラ

// ── Faceキャッシュ ────────────────────────────────────────
#define PSY_FACE_ENABLE       1     // 0 にすると Faceキャッシュを完全に無効化（ベクター顔のみになる）
#define PSY_FACE_MAX          4     // PSRAMへ持つ顔の枚数（1〜4）。1枚 = 38,400byte
#define PSY_FACE_W          160     // 160x120x16bit = 38,400byte/枚
#define PSY_FACE_H          120
#define PSY_FACE_PNG_MAX  (256UL * 1024UL)   // これより大きいPNGはキャッシュ対象外

// ════════════════════════════════════════════════════════════════════════
// 乱数：既存の Arduino random() のみを使う（新しい乱数源は追加しない）
// ════════════════════════════════════════════════════════════════════════
static inline float psyRndF(float lo, float hi) {
  return lo + (hi - lo) * ((float)random(0, 10001) * 0.0001f);
}
static inline int psyRndI(int lo, int hi) { return (int)random(lo, (long)hi + 1); }
static inline bool psyChance(int pct)      { return (int)random(0, 100) < pct; }

// FLASHは1〜3フレームのあいだ「同じ絵」を出す必要がある。
// Canvasは毎フレーム背景から作り直されるため、ショットごとの種から
// 決定的に同じ図形列を再生成する（配列に絵を溜め込まない＝RAM節約）。
static uint32_t psyLcg;
static inline void     psyLcgSeed(uint32_t s) { psyLcg = s ? s : 1u; }
static inline uint32_t psyLcgNext() {
  psyLcg ^= psyLcg << 13; psyLcg ^= psyLcg >> 17; psyLcg ^= psyLcg << 5; return psyLcg;
}
static inline float psyLcgF(float lo, float hi) {
  return lo + (hi - lo) * ((float)(psyLcgNext() & 0xFFFF) / 65535.0f);
}
static inline int psyLcgI(int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int)(psyLcgNext() % (uint32_t)(hi - lo + 1));
}

// ── PsyPal を引数に取る全関数の明示的な前方宣言 ────────────────────
// Arduino IDE(ctags) は「既にプロトタイプがある関数」への自動生成をスキップする。
// 既存の sfDrawFighter(const SFFighter&, ...) と同じ流儀。
// ファイル先頭の PsyPal 前方定義と合わせて二重の保険になっている。
static void psyFlashOpGrid(const PsyPal& P);
static void psyFlashDots(const PsyPal& P);
static void psyFlashHatch(const PsyPal& P);
static void psyFlashWobble(const PsyPal& P);
static void psyAccentFaceVector(const PsyPal& P, float zoom);
static void psyAccentFace(const PsyPal& P);
static void psyAccentStab(const PsyPal& P);
static void psyAccentGiant(const PsyPal& P);

// ── 描画ラッパ（すべて lightBright() を通すのでBrightness設定が自動で効く）──
static inline void psyFillCircle(int x, int y, int r, uint16_t c)             { GFX.fillCircle(x, y, r, lightBright(c)); }
static inline void psyDrawEllipse(int x, int y, int rx, int ry, uint16_t c)   { GFX.drawEllipse(x, y, rx, ry, lightBright(c)); }
static inline void psyFillTri(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c) { GFX.fillTriangle(x0,y0,x1,y1,x2,y2, lightBright(c)); }

// ════════════════════════════════════════════════════════════════════════
// ショット管理の状態
// ════════════════════════════════════════════════════════════════════════
static uint8_t  psyLayer      = PSY_LAYER_MOTION;
static uint8_t  psyKind       = PSY_M_RINGS;
static uint8_t  psyFrames     = PSY_MOTION_MIN_F;
static uint8_t  psyIdx        = 0;
static uint8_t  psyPal        = 0;
static uint32_t psyShotSeed   = 1;
static uint8_t  psyBurstLeft  = 0;      // FLASH連射の残り本数
static uint8_t  psyLastFlash  = 255;    // 直前のFLASH種別（連続禁止）
static uint8_t  psyIntrLeft   = 0;      // ACCENT割り込みの残りフレーム
static uint8_t  psyIntrKind   = PSY_A_STAB;
static uint8_t  psyBag[PSY_M_COUNT];    // MOTIONシャッフルバッグ
static uint8_t  psyBagN       = 0;
static uint8_t  psyShotsSinceFace = 99;
static int8_t   psyLastFaceIdx    = -1;

// ── Face ACCENT 専用state ────────────────────────────────────
// Face ACCENT は最大2コマ(180ms)続き、psyAccentFace() が2回呼ばれる。
// 描画のたびに抽選すると 1コマ目と2コマ目で別の顔・別の角度になってしまうため、
// 【ACCENT開始時に一度だけ】抽選してここへ保存し、描画中は再抽選しない。
// 2コマ目は zoom だけを PSY_FACE_ZOOM_STEP 倍する
// ＝「同じ顔が、同じ向き・同じ位置のまま一段こちらへ迫る」動きになる。
#define PSY_FACE_ZOOM_STEP 1.15f     // 2コマ目の拡大率（+15%）
static int8_t  psyFaceShotIdx  = -1;      // 使用する顔（-1 = ベクター顔フォールバック）
static float   psyFaceShotZoom = 2.0f;    // 1コマ目のズーム
static bool    psyFaceShotFlip = false;   // 左右反転
static float   psyFaceShotAng  = 0.0f;    // 回転角(度)
static float   psyFaceShotDx   = 160.0f;  // 表示中心X
static float   psyFaceShotDy   = 144.0f;  // 表示中心Y
static uint8_t psyFaceShotOv   = 3;       // 上乗せ加工（0=色スキャンライン/1=太枠/それ以外=なし）
static uint8_t psyFaceShotStep = 0;       // 0=1コマ目 / 1以上=2コマ目

// ── Faceキャッシュ ──
// 親を持たない既定コンストラクタで確保する（LGFX_Spriteのコピー構築を要求しないため、
// Arduino-ESP32 の C++11 / C++17 どちらのツールチェーンでも確実にコンパイルできる）。
// 転送先は pushRotateZoom() の第1引数で毎回明示するので親は不要。
static M5Canvas psyFace[PSY_FACE_MAX];
static uint8_t psyFaceN     = 0;        // 実際にデコードできた枚数
static bool    psyFaceTried = false;    // 1回でも読み込みを試したか（毎回SDを叩かない）

// ════════════════════════════════════════════════════════════════════════
// MOTION-A : Expanding Rings（代表MOTION）
//   輪が偏心した位置で次々生まれ、加速しながら拡大して画面外へ抜ける。
//   Tempest Tunnel（中心へ吸い込まれる／全リング共通の中心／等速／輪郭線のみ）
//   とは、進行方向・中心・速度・太さ・色数のすべてが逆方向。
// ════════════════════════════════════════════════════════════════════════
struct PsyRing {
  float    r, v, cx, cy, dx, dy, ell, gro;
  uint16_t col;
  uint8_t  wid;
};
static PsyRing  psyRing[PSY_RING_MAX];
static uint8_t  psyRingN   = 0;
static float    psyRingBx, psyRingBy, psyRingBvx, psyRingBvy;   // 発生中心とそのドリフト
static uint16_t psyRingPool[PSY_RING_POOL_MAX];
static uint8_t  psyRingPoolN = 0;
static uint8_t  psyRingRun   = 0;       // 同色を続ける残り本数
static uint16_t psyRingRunCol = 0;

// 色プール：スペクトル環から連続する5〜7色の窓を切り出し、パレットのMAIN/ACCENT/白を足す。
// → ショットごとに「色の家族」が変わるが、常に複数色が混ざる。
static void psyRingBuildPool() {
  const PsyPal& P = PSY_PAL[psyPal];
  uint8_t start = (uint8_t)psyRndI(0, 11);
  uint8_t span  = (uint8_t)psyRndI(5, 7);
  psyRingPoolN = 0;
  for (uint8_t i = 0; i < span && psyRingPoolN < PSY_RING_POOL_MAX; i++) {
    uint8_t step = (uint8_t)psyRndI(1, 2);   // 環を1つ飛ばしで拾うこともある＝色差が均一にならない
    psyRingPool[psyRingPoolN++] = PSY_SPECTRUM[(start + i * step) % 12];
  }
  if (psyRingPoolN < PSY_RING_POOL_MAX) psyRingPool[psyRingPoolN++] = P.mn;
  if (psyRingPoolN < PSY_RING_POOL_MAX) psyRingPool[psyRingPoolN++] = P.ac;
  if (psyRingPoolN < PSY_RING_POOL_MAX) psyRingPool[psyRingPoolN++] = 0xFFFF;
}

// 先頭ほど出やすい不均一な重み付き抽選＋ときどき同色が2〜3本続く
static uint16_t psyRingPickColor() {
  if (psyRingRun > 0) { psyRingRun--; return psyRingRunCol; }
  if (psyRingPoolN == 0) return 0xFFFF;
  int total = 0;
  for (uint8_t i = 0; i < psyRingPoolN; i++) total += (int)(psyRingPoolN - i) * 3 + 2;
  int pick = (int)random(0, total), acc = 0;
  uint16_t col = psyRingPool[0];
  for (uint8_t i = 0; i < psyRingPoolN; i++) {
    acc += (int)(psyRingPoolN - i) * 3 + 2;
    if (pick < acc) { col = psyRingPool[i]; break; }
  }
  if (psyChance(PSY_RING_RUN_PCT)) { psyRingRun = (uint8_t)psyRndI(1, 2); psyRingRunCol = col; }
  return col;
}

// 線幅：細い／普通／極端に太い の3クラスを不均一に混ぜる（全部同じ太さにしない）
static uint8_t psyRingPickWidth() {
  int p = (int)random(0, 100);
  if (p < PSY_RING_W_THIN_PCT)                      return (uint8_t)psyRndI(PSY_RING_W_THIN_MIN, PSY_RING_W_THIN_MAX);
  if (p < PSY_RING_W_THIN_PCT + PSY_RING_W_MID_PCT) return (uint8_t)psyRndI(PSY_RING_W_MID_MIN,  PSY_RING_W_MID_MAX);
  return (uint8_t)psyRndI(PSY_RING_W_FAT_MIN, PSY_RING_W_FAT_MAX);
}

static void psyRingSpawn(float r0, float v0) {
  if (psyRingN >= PSY_RING_MAX) return;
  PsyRing& R = psyRing[psyRingN++];
  R.r   = r0;
  R.v   = v0;
  R.cx  = psyRingBx + psyRndF(-PSY_RING_JITTER, PSY_RING_JITTER);   // 生まれる場所がばらつく＝同心にならない
  R.cy  = psyRingBy + psyRndF(-PSY_RING_JITTER, PSY_RING_JITTER);
  R.dx  = psyRndF(-PSY_RING_DRIFT_X, PSY_RING_DRIFT_X);             // 拡大しながら中心も流れる
  R.dy  = psyRndF(-PSY_RING_DRIFT_Y, PSY_RING_DRIFT_Y);
  R.ell = psyRndF(PSY_RING_ELL_MIN, PSY_RING_ELL_MAX);              // 楕円率も1本ずつ違う
  R.gro = psyRndF(PSY_RING_GRO_MIN, PSY_RING_GRO_MAX);              // 拡大速度も1本ずつ違う
  R.col = psyRingPickColor();
  R.wid = psyRingPickWidth();
}

static void psyRingsInit() {
  psyRingBx  = psyRndF(100.0f, 220.0f);
  psyRingBy  = psyRndF(100.0f, 190.0f);
  psyRingBvx = psyRndF(-PSY_RING_BIRTH_VX, PSY_RING_BIRTH_VX);
  psyRingBvy = psyRndF(-PSY_RING_BIRTH_VY, PSY_RING_BIRTH_VY);
  psyRingN   = 0;
  psyRingRun = 0;
  psyRingBuildPool();
  // 1コマ目からすでに輪が飛んでいる状態にする（立ち上がりが空にならない）
  float r = 4.5f, v = 4.5f;
  for (uint8_t i = 0; i < PSY_RING_PRELOAD && i < PSY_RING_MAX; i++) {
    psyRingSpawn(r, v);
    r *= 1.115f * 1.115f;   // 半径を階段状に散らす
    v *= 1.05f  * 1.05f;
  }
}

static void psyRingsStep(float ramp) {
  float rs = sqrtf(ramp);   // 半径への加速は緩めに掛ける（クライマックスで一気に飛び去らせない）
  uint8_t w = 0;
  for (uint8_t i = 0; i < psyRingN; i++) {
    PsyRing& R = psyRing[i];
    R.v = R.v * R.gro; if (R.v > PSY_RING_V_CLAMP) R.v = PSY_RING_V_CLAMP;
    R.r  += R.v * rs;
    R.cx += R.dx * ramp;
    R.cy += R.dy * ramp;
    if (R.r < PSY_RING_KILL_R) { if (w != i) psyRing[w] = R; w++; }   // 画面外まで生かさない
  }
  psyRingN = w;
  psyRingBx += psyRingBvx * ramp;
  psyRingBy += psyRingBvy * ramp;
  if (psyRingBx <  80.0f || psyRingBx > 240.0f) psyRingBvx = -psyRingBvx;
  if (psyRingBy <  95.0f || psyRingBy > 195.0f) psyRingBvy = -psyRingBvy;
  uint8_t spawn = (psyRingN < PSY_RING_LOW_COUNT) ? PSY_RING_SPAWN_LOW : PSY_RING_SPAWN_HI;
  for (uint8_t k = 0; k < spawn; k++) psyRingSpawn(PSY_RING_R0, PSY_RING_V0);
}

static void psyRingsDraw(bool inv) {
  const PsyPal& P = PSY_PAL[psyPal];
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.bg);
  // 大きい輪（＝近い輪）から描き、生まれたての小さい輪が最前面に来るようにする
  for (int pass = 0; pass < 2; pass++) {
    for (uint8_t i = 0; i < psyRingN; i++) {
      const PsyRing& R = psyRing[i];
      bool big = (R.r >= 90.0f);
      if ((pass == 0) != big) continue;
      int rx = (int)R.r, ry = (int)(R.r * R.ell);
      if (rx < 1) rx = 1;
      if (ry < 1) ry = 1;
      int th = (int)(R.wid * (0.45f + R.r / PSY_RING_KILL_R));   // 近い輪ほど太く見える
      if (th < 1) th = 1;
      uint16_t col = inv ? (uint16_t)~R.col : R.col;
      int cx = (int)R.cx, cy = (int)R.cy;
      for (int k = 0; k < th; k++) psyDrawEllipse(cx, cy, rx + k, ry + k, col);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════
// MOTION-B : 偏心スパイラル（回転＋中心移動＋拡大＋途中で突然の逆回転）
// ════════════════════════════════════════════════════════════════════════
static float   psySpPh, psySpSpin, psySpTwist, psySpReach, psySpCx, psySpCy, psySpEll;
static uint8_t psySpArms, psySpSkip, psySpFlipAt, psySpF;

static void psySpiralInit() {
  psySpCx    = psyRndF(70.0f, 250.0f);
  psySpCy    = psyRndF(90.0f, 200.0f);
  psySpArms  = (uint8_t)psyRndI(6, 9);
  psySpTwist = psyRndF(0.012f, 0.018f);
  psySpEll   = psyRndF(0.70f, 1.30f);
  psySpPh    = psyRndF(0.0f, 6.2831853f);
  psySpSpin  = (psyChance(50) ? 1.0f : -1.0f) * 0.155f;
  psySpReach = 180.0f;
  psySpSkip  = (uint8_t)psyRndI(0, psySpArms - 1);   // 1本だけ腕を欠けさせる＝規則の局所破壊
  psySpFlipAt= (uint8_t)psyRndI(6, 12);
  psySpF     = 0;
}
static void psySpiralStep(float ramp) {
  psySpF++;
  if (psySpF == psySpFlipAt) psySpSpin *= -1.35f;   // 突然の逆回転＋加速
  psySpPh    += psySpSpin * ramp;
  psySpCx    += 1.7f * ramp;
  psySpCy    += 0.85f * ramp;
  psySpTwist += 0.00022f * ramp;
  psySpReach *= (1.0f + 0.017f * ramp);
  if (psySpReach > 300.0f) psySpReach = 300.0f;
  if (psySpPh >  62.83f) psySpPh -= 62.83f;
  if (psySpPh < -62.83f) psySpPh += 62.83f;
}
static void psySpiralDraw(bool inv) {
  const PsyPal& P = PSY_PAL[psyPal];
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.bg);
  uint16_t cA = inv ? P.ac : P.mn, cB = inv ? P.mn : P.ac;
  int N = 18 + (int)(psySpReach / 11.0f);           // 伸びたぶんスタンプ数も増やす（腕が途切れない）
  if (N > 45) N = 45;                               // 描画負荷の上限
  for (uint8_t a = 0; a < psySpArms; a++) {
    if (a == psySpSkip) continue;
    uint16_t col = (a & 1) ? cB : cA;
    float base = psySpPh + (float)a * 6.2831853f / (float)psySpArms;
    for (int i = 0; i < N; i++) {
      float t  = (float)i / (float)(N - 1);
      float r  = 6.0f + powf(t, 0.85f) * psySpReach;
      float th = base + r * psySpTwist;
      int x = (int)(psySpCx + r * cosf(th));
      int y = (int)(psySpCy + r * sinf(th) * psySpEll);
      int w = (int)(3.0f + 13.0f * t);              // 根元は細く先端ほど太い
      psyFillCircle(x, y, w, col);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════
// MOTION-C : モアレ干渉（2つの線束がすれ違い、線密度も変化して縞が崩壊・再生）
// ════════════════════════════════════════════════════════════════════════
static float psyMoAx, psyMoAy, psyMoBx, psyMoBy, psyMoBvx, psyMoBvy, psyMoSkew, psyMoDsk, psyMoNa, psyMoNb;

static void psyMoireInit() {
  psyMoAx  = psyRndF(120.0f, 190.0f);
  psyMoAy  = psyRndF(110.0f, 170.0f);
  bool fromLeft = psyChance(50);
  psyMoBx  = fromLeft ? -30.0f : 350.0f;
  psyMoBy  = psyRndF(95.0f, 190.0f);
  psyMoBvx = fromLeft ? 40.0f : -40.0f;             // 約13フレームで画面を横断＝束Aとすれ違う
  psyMoBvy = psyRndF(-4.0f, 4.0f);
  psyMoSkew= 0.0f;
  psyMoDsk = psyRndF(0.030f, 0.048f);
  psyMoNa  = 44.0f;
  psyMoNb  = 40.0f;
}
static void psyMoireStep(float ramp) {
  psyMoBx   += psyMoBvx * ramp;
  psyMoBy   += psyMoBvy * ramp;
  psyMoSkew += psyMoDsk * ramp;
  psyMoAx   += 1.4f * ramp;
  psyMoNa -= 1.1f * ramp; if (psyMoNa < 18.0f) psyMoNa = 18.0f;   // 線密度が変わる＝縞の太さが変わる
  psyMoNb -= 0.9f * ramp; if (psyMoNb < 16.0f) psyMoNb = 16.0f;
}
static void psyMoireDraw(bool inv) {
  const PsyPal& P = PSY_PAL[psyPal];
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.bg);
  uint16_t cA = inv ? P.ac : P.mn, cB = inv ? P.mn : P.ac;
  int na = (int)psyMoNa, nb = (int)psyMoNb;
  for (int i = 0; i < na; i++) {
    float a = (float)i * 3.14159265f / (float)na;
    float c = cosf(a) * 560.0f, s = sinf(a) * 560.0f;
    lightDrawLine((int)(psyMoAx - c), (int)(psyMoAy - s), (int)(psyMoAx + c), (int)(psyMoAy + s), cA);
  }
  for (int i = 0; i < nb; i++) {
    float a = (float)i * 3.14159265f / (float)nb + psyMoSkew;
    float c = cosf(a) * 560.0f, s = sinf(a) * 560.0f;
    lightDrawLine((int)(psyMoBx - c), (int)(psyMoBy - s), (int)(psyMoBx + c), (int)(psyMoBy + s), cB);
  }
}

// ════════════════════════════════════════════════════════════════════════
// MOTION-D : 不規則放射ウェッジ（頂点が画面を横断しながら高速回転）
// ════════════════════════════════════════════════════════════════════════
#define PSY_WD_MAX 20
static float   psyWdPh, psyWdSpin, psyWdX, psyWdY, psyWdVx, psyWdVy, psyWdBreathe;
static float   psyWdRaw[PSY_WD_MAX];
static uint8_t psyWdN;

static void psyWedgeInit() {
  psyWdX    = psyRndF(30.0f, 290.0f);
  psyWdY    = psyRndF(70.0f, 220.0f);
  psyWdN    = (uint8_t)psyRndI(13, 19);
  for (uint8_t i = 0; i < psyWdN; i++) psyWdRaw[i] = psyRndF(0.5f, 1.6f);  // 角度幅が非等間隔
  psyWdPh   = psyRndF(0.0f, 6.2831853f);
  psyWdSpin = (psyChance(50) ? 1.0f : -1.0f) * psyRndF(0.17f, 0.27f);
  psyWdVx   = psyRndF(-9.0f, 9.0f);
  psyWdVy   = psyRndF(-6.0f, 6.0f);
  psyWdBreathe = 0.0f;
}
static void psyWedgeStep(float ramp) {
  psyWdPh += psyWdSpin * ramp;
  psyWdX  += psyWdVx * ramp;
  psyWdY  += psyWdVy * ramp;
  if (psyWdX <  10.0f || psyWdX > 310.0f) psyWdVx = -psyWdVx;
  if (psyWdY <  60.0f || psyWdY > 230.0f) psyWdVy = -psyWdVy;
  psyWdBreathe += 0.55f * ramp;
  uint8_t k = (uint8_t)((int)psyWdBreathe % (int)psyWdN);   // 1枚ずつ順番に肥大していく
  psyWdRaw[k] *= 1.16f;
  if (psyWdRaw[k] > 2.2f) psyWdRaw[k] = 2.2f;
  if (psyWdPh >  62.83f) psyWdPh -= 62.83f;
  if (psyWdPh < -62.83f) psyWdPh += 62.83f;
}
static void psyWedgeDraw(bool inv) {
  const PsyPal& P = PSY_PAL[psyPal];
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.bg);
  uint16_t cA = inv ? P.ac : P.mn, cB = inv ? P.mn : P.ac;
  float total = 0.0f;
  for (uint8_t i = 0; i < psyWdN; i++) total += psyWdRaw[i];
  if (total <= 0.0f) return;
  float a = psyWdPh;
  int ax = (int)psyWdX, ay = (int)psyWdY;
  for (uint8_t i = 0; i < psyWdN; i++) {
    float a1 = a;
    a += psyWdRaw[i] / total * 6.2831853f;
    float a2 = a;
    uint16_t col = (i % 9 == 4) ? P.bg : ((i & 1) ? cB : cA);   // 9枚に1枚は背景色で抜く
    // 扇を3枚の三角形で近似する（fillTriangleの呼び出し数を抑える）
    for (int s = 0; s < 3; s++) {
      float t0 = a1 + (a2 - a1) * (float)s / 3.0f;
      float t1 = a1 + (a2 - a1) * (float)(s + 1) / 3.0f;
      psyFillTri(ax, ay,
                 ax + (int)(cosf(t0) * 500.0f), ay + (int)(sinf(t0) * 500.0f),
                 ax + (int)(cosf(t1) * 500.0f), ay + (int)(sinf(t1) * 500.0f), col);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════
// MOTION-E : 破片三角（回転しながら加速して飛び散る）
// ════════════════════════════════════════════════════════════════════════
#define PSY_TRI_N 15
struct PsyTri { float x[3], y[3], vx, vy; uint8_t col; };
static PsyTri psyTri[PSY_TRI_N];

static void psyShardInit() {
  for (int i = 0; i < 3; i++) {          // 画面を横切る巨大片（重なりを抑えるため3枚）
    psyTri[i].x[0] = psyRndF(-120, 0);   psyTri[i].y[0] = psyRndF(-60, 300);
    psyTri[i].x[1] = psyRndF(320, 440);  psyTri[i].y[1] = psyRndF(-60, 300);
    psyTri[i].x[2] = psyRndF(0, 320);    psyTri[i].y[2] = psyRndF(-120, 420);
    psyTri[i].vx = psyRndF(-7, 7);       psyTri[i].vy = psyRndF(-5, 5);
    psyTri[i].col = (uint8_t)(i % 3);
  }
  for (int i = 3; i < PSY_TRI_N; i++) {  // 小片
    float cx = psyRndF(0, 320), cy = psyRndF(PSY_TOP, 240), sz = psyRndF(14, 52);
    for (int k = 0; k < 3; k++) {
      psyTri[i].x[k] = cx + psyRndF(-sz, sz);
      psyTri[i].y[k] = cy + psyRndF(-sz, sz);
    }
    psyTri[i].vx = psyRndF(-11, 11);     psyTri[i].vy = psyRndF(-8, 8);
    psyTri[i].col = (uint8_t)((i + 1) % 3);
  }
}
static void psyShardStep(float ramp) {
  const float th = 0.09f;
  float cs = cosf(th * ramp), sn = sinf(th * ramp);
  for (int i = 0; i < PSY_TRI_N; i++) {
    PsyTri& T = psyTri[i];
    T.vx *= 1.06f; T.vy *= 1.06f;        // 加速して飛び散る
    float cx = (T.x[0] + T.x[1] + T.x[2]) / 3.0f;
    float cy = (T.y[0] + T.y[1] + T.y[2]) / 3.0f;
    for (int k = 0; k < 3; k++) {        // 自分の重心まわりに回転しながら移動
      float dx = T.x[k] - cx, dy = T.y[k] - cy;
      T.x[k] = cx + dx * cs - dy * sn + T.vx * ramp;
      T.y[k] = cy + dx * sn + dy * cs + T.vy * ramp;
    }
  }
}
static void psyShardDraw(bool inv) {
  const PsyPal& P = PSY_PAL[psyPal];
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.bg);
  uint16_t c0 = inv ? P.ac : P.mn, c1 = inv ? P.mn : P.ac;
  for (int i = 0; i < PSY_TRI_N; i++) {
    const PsyTri& T = psyTri[i];
    uint16_t col = (T.col == 0) ? c0 : (T.col == 1) ? c1 : 0xFFFF;
    psyFillTri((int)T.x[0], (int)T.y[0], (int)T.x[1], (int)T.y[1], (int)T.x[2], (int)T.y[2], col);
  }
}

// ── MOTION共通ディスパッチ ───────────────────────────────────
static void psyMotionInit(uint8_t k) {
  switch (k) {
    case PSY_M_RINGS:  psyRingsInit();  break;
    case PSY_M_SPIRAL: psySpiralInit(); break;
    case PSY_M_MOIRE:  psyMoireInit();  break;
    case PSY_M_WEDGE:  psyWedgeInit();  break;
    default:           psyShardInit();  break;
  }
}
static void psyMotionStep(uint8_t k, float ramp) {
  switch (k) {
    case PSY_M_RINGS:  psyRingsStep(ramp);  break;
    case PSY_M_SPIRAL: psySpiralStep(ramp); break;
    case PSY_M_MOIRE:  psyMoireStep(ramp);  break;
    case PSY_M_WEDGE:  psyWedgeStep(ramp);  break;
    default:           psyShardStep(ramp);  break;
  }
}
static void psyMotionDraw(uint8_t k, bool inv) {
  switch (k) {
    case PSY_M_RINGS:  psyRingsDraw(inv);  break;
    case PSY_M_SPIRAL: psySpiralDraw(inv); break;
    case PSY_M_MOIRE:  psyMoireDraw(inv);  break;
    case PSY_M_WEDGE:  psyWedgeDraw(inv);  break;
    default:           psyShardDraw(inv);  break;
  }
}

// ════════════════════════════════════════════════════════════════════════
// FLASH（静止の一撃）。psyShotSeed から決定的に再生成するので
// 1〜3フレームのあいだ「まったく同じ絵」が出る。
// ════════════════════════════════════════════════════════════════════════
static void psyFlashOpGrid(const PsyPal& P) {
  int  cols = psyLcgI(9, 14), rows = psyLcgI(6, 9);
  float amp = (float)psyLcgI(10, 26), ph = psyLcgF(0.0f, 6.28f);
  float cw[14], total = 0.0f;
  for (int i = 0; i < cols; i++) { cw[i] = 6.0f + 34.0f * fabsf(sinf((float)i * 0.9f + ph)); total += cw[i]; }
  float y = (float)PSY_TOP;
  for (int j = 0; j < rows; j++) {
    float rh = (float)(240 - PSY_TOP) / (float)rows * (0.6f + 0.8f * fabsf(sinf((float)j * 1.1f + ph * 0.7f)));
    float x  = -40.0f + amp * sinf((float)j * 0.8f + ph);   // 行ごとに横シフト＝布のように歪む
    int i = 0;
    while (x < 320.0f && i < 64) {
      float wd = cw[i % cols] * (320.0f / total * 1.15f);
      lightFillRect((int)x, (int)y, (int)wd + 1, (int)rh + 1, ((i + j) & 1) ? P.ac : P.mn);
      x += wd; i++;
    }
    y += rh;
  }
}
static void psyFlashDots(const PsyPal& P) {
  // 背景は psyFlashDraw() が塗り済み（ここで二重に塗らない＝1画面分の無駄を省く）
  int nBig = psyLcgI(1, 2), nMid = psyLcgI(2, 3), nSml = psyLcgI(3, 5);
  int qx = psyLcgI(0, 1) * 160, qy = PSY_TOP + psyLcgI(0, 1) * 96;   // 小円を1象限に密集させる
  for (int i = 0; i < nBig; i++) {   // 巨大円は中心を画面外に置いて縁で切る
    int x = psyLcgI(-60, 380), y = psyLcgI(20, 260), r = psyLcgI(85, 150);
    psyFillCircle(x, y, r, (i & 1) ? P.ac : P.mn);
    int ir = r * 48 / 100;
    psyFillCircle(x + psyLcgI(-r / 3, r / 3), y + psyLcgI(-r / 3, r / 3), ir, (i & 1) ? P.mn : P.bg);
  }
  for (int i = 0; i < nMid; i++) {
    int x = psyLcgI(0, 320), y = psyLcgI(PSY_TOP, 240), r = psyLcgI(25, 50);
    psyFillCircle(x, y, r, (i & 1) ? P.mn : P.ac);
    psyFillCircle(x + psyLcgI(-r / 3, r / 3), y + psyLcgI(-r / 3, r / 3), r * 55 / 100, (i & 1) ? P.bg : P.mn);
  }
  for (int i = 0; i < nSml; i++) {
    psyFillCircle(qx + psyLcgI(0, 160), qy + psyLcgI(0, 96), psyLcgI(6, 14), (i & 1) ? P.ac : P.mn);
  }
}
static void psyFlashHatch(const PsyPal& P) {
  // 背景は psyFlashDraw() が塗り済み
  float ang = (float)psyLcgI(0, 170) * 0.0174533f;
  float dd  = (float)psyLcgI(3, 7)   * 0.0174533f;   // わずかな角度差＝干渉帯が発生
  for (int f = 0; f < 2; f++) {
    float a = (f == 0) ? ang : ang + dd;
    int   sp = (f == 0) ? 7 : 9;                     // 7pxと9px＝非整数比
    float dx = cosf(a), dy = sinf(a);
    for (int k = -60; k < 60; k++) {
      float px = 160.0f - dy * (float)(k * sp), py = 144.0f + dx * (float)(k * sp);
      lightDrawLine((int)(px - dx * 500.0f), (int)(py - dy * 500.0f),
                    (int)(px + dx * 500.0f), (int)(py + dy * 500.0f), (f == 0) ? P.mn : P.ac);
    }
  }
}
static void psyFlashWobble(const PsyPal& P) {
  const uint16_t cols[3] = { P.mn, P.ac, P.bg };
  int   bands = psyLcgI(4, 6);
  float ph    = psyLcgF(0.0f, 6.28f);
  for (int x = 0; x < 320; x += 8) {
    for (int b = 0; b < bands; b++) {
      // 周期 47/61/93px の3正弦合成＝画面内で決して同じ形が繰り返さない
      float k0 = (float)b * 0.9f, k1 = (float)(b + 1) * 0.9f;
      float o0 = 26.0f * sinf(((float)x + ph * 40.0f) / 47.0f * 6.2831853f + k0)
               + 16.0f * sinf((float)x / 61.0f * 6.2831853f + k0 * 1.7f)
               + 10.0f * sinf((float)x / 93.0f * 6.2831853f);
      float o1 = 26.0f * sinf(((float)x + ph * 40.0f) / 47.0f * 6.2831853f + k1)
               + 16.0f * sinf((float)x / 61.0f * 6.2831853f + k1 * 1.7f)
               + 10.0f * sinf((float)x / 93.0f * 6.2831853f);
      int y0 = (int)((float)PSY_TOP + (float)(240 - PSY_TOP) * (float)b / (float)bands + o0);
      int y1 = (int)((float)PSY_TOP + (float)(240 - PSY_TOP) * (float)(b + 1) / (float)bands + o1);
      if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
      lightFillRect(x, y0, 8, y1 - y0 + 1, cols[b % 3]);
    }
  }
}
static void psyFlashDraw(uint8_t k) {
  const PsyPal& P = PSY_PAL[psyPal];
  psyLcgSeed(psyShotSeed);                 // ← 毎フレーム同じ種＝同じ絵
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.bg);
  switch (k) {
    case PSY_F_OPGRID: psyFlashOpGrid(P); break;
    case PSY_F_DOTS:   psyFlashDots(P);   break;
    case PSY_F_HATCH:  psyFlashHatch(P);  break;
    default:           psyFlashWobble(P); break;
  }
}

// ════════════════════════════════════════════════════════════════════════
// Faceキャッシュ（既存 Face Gallery の /faces/*.png を再利用する。
// 既存の drawFaceImage() / tryShowMutterFace() / showSleepFace() / imageFaceMode は
// 一切呼ばず・一切変更しない。ここは「PNGを読んでスプライトへデコードする」だけ。）
// ════════════════════════════════════════════════════════════════════════
static bool psyFaceLoadAll() {
#if !PSY_FACE_ENABLE
  return false;
#else
  if (!psramFound()) return false;          // PSRAMが無ければキャッシュしない

  File dir = SD.open("/faces");
  if (!dir) return false;

  const int MAXN = 32;
  static char names[MAXN][64];              // 一時的な名前リスト（静的にしてスタックを使わない）
  int n = 0;
  File entry = dir.openNextFile();
  while (entry && n < MAXN) {
    String nm = String(entry.name());
    if (!entry.isDirectory() && !nm.startsWith(".") && nm.endsWith(".png")) {
      nm.toCharArray(names[n], 64);
      n++;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  if (n == 0) return false;

  int start = (int)random(0, n);            // 起動ごとに違う顔が選ばれる
  psyFaceN = 0;
  for (int s = 0; s < n && psyFaceN < PSY_FACE_MAX; s++) {
    int idx = (start + s) % n;
    char path[80];
    snprintf(path, sizeof(path), "/faces/%s", names[idx]);

    File f = SD.open(path);
    if (!f) continue;
    size_t sz = f.size();
    if (sz == 0 || sz > PSY_FACE_PNG_MAX) { f.close(); continue; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { f.close(); break; }         // ヒープが足りない → ここで打ち切り
    size_t rd = f.read(buf, sz);
    f.close();

    bool ok = false;
    if (rd == sz) {
      M5Canvas& C = psyFace[psyFaceN];
      C.setPsram(true);
      C.setColorDepth(16);
      if (C.createSprite(PSY_FACE_W, PSY_FACE_H)) {
        C.fillSprite(0x0000);
        // 320x240のPNGを 0.5倍でデコード＝160x120へ直接展開（変換用の中間バッファ不要）
        ok = C.drawPng(buf, sz, 0, 0, PSY_FACE_W, PSY_FACE_H, 0, 0, 0.5f, 0.5f);
        if (!ok) C.deleteSprite();
      }
    }
    free(buf);
    if (ok) psyFaceN++;
  }
  return (psyFaceN > 0);
#endif
}

// 初回のPsychedelic起動時に1回だけ試す。以後SDへは一切アクセスしない。
static void psyFaceEnsure() {
  if (psyFaceTried) return;
  psyFaceTried = true;
  unsigned long t0 = millis();
  bool ok = psyFaceLoadAll();
  addLog(String("PSYCHE FACE CACHE: ") + (ok ? "OK " : "FALLBACK ")
         + String(psyFaceN) + "/" + String((int)PSY_FACE_MAX)
         + " " + String(millis() - t0) + "ms psram=" + String((unsigned)ESP.getFreePsram()));
}
static inline bool psyFaceUsable() { return psyFaceN > 0; }

// ════════════════════════════════════════════════════════════════════════
// ACCENT（割り込み）
// ════════════════════════════════════════════════════════════════════════
// キャッシュが使えないときの代替：かりポムの顔プリミティブを巨大化・偏心して描く。
// 追加RAMゼロで必ず動くため、SDなし／PNG 0枚／PSRAM確保失敗でも演出は止まらない。
static void psyAccentFaceVector(const PsyPal& P, float zoom) {
  // 位置は Face ACCENT 開始時に保存した値をそのまま使う（2コマ間で動かさない）。
  // zoom だけが 2コマ目で 1.15倍になるので、ベクター顔でも「一段迫る」動きになる。
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.bg);
  int cx = (int)psyFaceShotDx, cy = (int)psyFaceShotDy;
  int s  = (int)(12.0f * zoom);                     // 拡大率（通常顔の約3倍）
  psyFillCircle(cx - s * 2, cy - s, (int)(s * 1.3f), P.mn);
  psyFillCircle(cx + s * 2, cy - s, (int)(s * 1.3f), P.mn);
  psyFillCircle(cx,         cy + s / 2, s, P.ac);
  lightFillRect(cx - s * 2, cy + s * 2, s * 4, s / 2, P.ac);
}
// Face ACCENT の開始。ここで【一度だけ】すべての見た目を抽選して保存する。
// 以降 psyAccentFace() は保存した値を読むだけで、一切再抽選しない。
static void psyFaceShotBegin() {
  static const float ZOOM[4] = { 1.7f, 2.0f, 2.3f, 2.6f };
  if (psyFaceUsable()) {
    int idx = (int)random(0, psyFaceN);
    if (psyFaceN > 1 && idx == psyLastFaceIdx) idx = (idx + 1) % psyFaceN;   // 直前と同じ顔を避ける
    psyFaceShotIdx = (int8_t)idx;
    psyLastFaceIdx = (int8_t)idx;
  } else {
    psyFaceShotIdx = -1;                             // ベクター顔フォールバック
  }
  // かりポムの目・鼻・口はこの後コンポジタが最前面に描く。ぴったり重なると
  // 「ズレた」ように見えるので、必ず拡大＋位置ずらしして別スケールの二重像にする。
  psyFaceShotZoom = ZOOM[random(0, 4)];
  psyFaceShotFlip = psyChance(50);                   // 負のズーム＝左右反転
  psyFaceShotAng  = (float)psyRndI(-22, 22);
  psyFaceShotDx   = 160.0f + psyRndF(-45.0f, 45.0f);
  psyFaceShotDy   = 144.0f + psyRndF(-30.0f, 30.0f);
  psyFaceShotOv   = (uint8_t)psyRndI(0, 3);
  psyFaceShotStep = 0;
  psyShotsSinceFace = 0;
}

static void psyAccentFace(const PsyPal& P) {
  // 2コマ目は zoom だけ +15%。faceIdx / flip / angle / dx / dy / overlay は
  // psyFaceShotBegin() で保存した値をそのまま使う（＝ここでは絶対に再抽選しない）。
  float z = psyFaceShotZoom * ((psyFaceShotStep == 0) ? 1.0f : PSY_FACE_ZOOM_STEP);
  if (psyFaceShotStep < 255) psyFaceShotStep++;

  if (psyFaceShotIdx < 0 || !psyFaceUsable()) { psyAccentFaceVector(P, z); return; }

  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, 0x0000);
  float zx = psyFaceShotFlip ? -z : z;
  psyFace[psyFaceShotIdx].pushRotateZoom(gGfx, psyFaceShotDx, psyFaceShotDy, psyFaceShotAng, zx, z);

  // 上乗せ加工（画素変換はしない。矩形を重ねるだけ）
  if (psyFaceShotOv == 0) {                          // 色スキャンライン
    for (int y = PSY_TOP; y < 240; y += 10) lightFillRect(0, y, 320, 4, P.mn);
  } else if (psyFaceShotOv == 1) {                   // 太枠4辺
    for (int k = 0; k < 3; k++) {
      uint16_t c = (k & 1) ? P.ac : P.mn;
      lightFillRect(k * 4, PSY_TOP + k * 4, 320 - k * 8, 4, c);
      lightFillRect(k * 4, 236 - k * 4, 320 - k * 8, 4, c);
      lightFillRect(k * 4, PSY_TOP + k * 4, 4, 240 - PSY_TOP - k * 8, c);
      lightFillRect(316 - k * 4, PSY_TOP + k * 4, 4, 240 - PSY_TOP - k * 8, c);
    }
  }
}
static void psyAccentStab(const PsyPal& P) {         // 高彩度単色＋巨大な黒バー（全面白黒点滅にしない）
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.mn);
  int n = psyRndI(2, 3);
  for (int i = 0; i < n; i++) {
    int y = psyRndI(PSY_TOP, 220);
    lightFillRect(0, y, 320, psyRndI(14, 34), 0x0000);
  }
}
static void psyAccentGiant(const PsyPal& P) {        // 巨大図形が画面の70%を占有
  lightFillRect(0, PSY_TOP, 320, 240 - PSY_TOP, P.bg);
  if (psyChance(50)) {
    psyFillCircle(psyRndI(60, 260), psyRndI(80, 200), psyRndI(120, 190), P.ac);
  } else {
    psyFillTri(psyRndI(-100, 80), psyRndI(-40, 120),
               psyRndI(240, 420), psyRndI(60, 200),
               psyRndI(40, 280),  psyRndI(200, 340), P.ac);
  }
}
static void psyAccentScan() {                        // 現在の絵の上に黒バーを重ねる（映像が壊れたような不穏さ）
  for (int y = PSY_TOP; y < 240; y += 10) lightFillRect(0, y, 320, 4, 0x0000);
}

// ACCENT種別の抽選。Faceだけは最低間隔・強制発火・連続禁止のルールを持つ。
static uint8_t psyPickAccent() {
  bool faceOk = (psyShotsSinceFace >= PSY_FACE_MIN_GAP);
  if (faceOk && psyShotsSinceFace >= PSY_FACE_FORCE_GAP) return PSY_A_FACE;
  uint8_t cand[4]; uint8_t n = 0;
  if (faceOk) cand[n++] = PSY_A_FACE;
  cand[n++] = PSY_A_STAB;
  cand[n++] = PSY_A_GIANT;
  cand[n++] = PSY_A_SCAN;
  return cand[random(0, n)];
}

// ════════════════════════════════════════════════════════════════════════
// ショット遷移
// ════════════════════════════════════════════════════════════════════════
// パレット：直前と同じは選ばない。暗いパレットの直後は必ず明るいパレットにする
// （明→暗の等間隔反復にならず、コントラスト自体を演出に使える）。
static void psyPickPalette() {
  uint8_t prev = psyPal;
  bool needBright = (PSY_PAL[prev].dark != 0);
  uint8_t cand[12]; uint8_t n = 0;
  for (uint8_t i = 0; i < 12; i++) {
    if (i == prev) continue;
    if (needBright && PSY_PAL[i].dark) continue;
    cand[n++] = i;
  }
  psyPal = (n > 0) ? cand[random(0, n)] : (uint8_t)((prev + 1) % 12);
}

// MOTIONはシャッフルバッグで回す。5種を使い切るまで同じ種類を再抽選しないので、
// 約10秒あれば5つの視覚世界すべてが必ず登場する（乱数の偏りで出ない事故を排除）。
static uint8_t psyPickMotion() {
  if (psyBagN == 0) {
    for (uint8_t i = 0; i < PSY_M_COUNT; i++) psyBag[i] = i;
    for (uint8_t i = PSY_M_COUNT - 1; i > 0; i--) {   // Fisher-Yates
      uint8_t j = (uint8_t)random(0, i + 1);
      uint8_t t = psyBag[i]; psyBag[i] = psyBag[j]; psyBag[j] = t;
    }
    psyBagN = PSY_M_COUNT;
  }
  return psyBag[--psyBagN];
}

static void psyStartMotionShot() {
  psyPickPalette();
  psyLayer  = PSY_LAYER_MOTION;
  psyKind   = psyPickMotion();
  psyFrames = (psyKind == PSY_M_SHARD)
              ? (uint8_t)psyRndI(PSY_MOTION_MIN_F, PSY_SHARD_MAX_F)
              : (uint8_t)psyRndI(PSY_MOTION_MIN_F, PSY_MOTION_MAX_F);
  psyIdx    = 0;
  psyMotionInit(psyKind);
  psyShotsSinceFace++;
}
static void psyStartFlashShot() {
  psyPickPalette();
  psyLayer = PSY_LAYER_FLASH;
  uint8_t k;
  do { k = (uint8_t)random(0, PSY_F_COUNT); } while (PSY_F_COUNT > 1 && k == psyLastFlash);
  psyLastFlash = k;
  psyKind      = k;
  psyFrames    = (uint8_t)psyRndI(PSY_FLASH_MIN_F, PSY_FLASH_MAX_F);
  psyIdx       = 0;
  psyShotSeed  = (uint32_t)random(1, 0x7FFFFFFF);
  psyShotsSinceFace++;
}
static void psyNextShot() {
  if (psyLayer == PSY_LAYER_MOTION) {
    psyBurstLeft = (uint8_t)psyRndI(PSY_BURST_MIN, PSY_BURST_MAX);   // MOTIONの後はFLASH連射へ
    psyStartFlashShot();
  } else {
    if (psyBurstLeft > 0) psyBurstLeft--;
    if (psyBurstLeft > 0) psyStartFlashShot();
    else                  psyStartMotionShot();
  }
}

// ════════════════════════════════════════════════════════════════════════
// 本体
// ════════════════════════════════════════════════════════════════════════
void lightRenderPsychedelic(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;   // 毎フレーム背景から作り直すため差分描画はしない

  if (needsInit) {
    psyFaceEnsure();                 // 初回だけSDからFaceをPSRAMへ先読み（以後アクセスなし）
    psyBagN           = 0;
    psyBurstLeft      = 0;
    psyIntrLeft       = 0;
    psyLastFlash      = 255;
    psyShotsSinceFace = 99;
    psyLastFaceIdx    = -1;
    psyPal            = (uint8_t)random(0, 12);
    psyStartMotionShot();
  }

  // ── 描画範囲を y=48..239 に限定する（Kaleidoscopeと同じ手法）──
  //    P1/P2/P5/P7 は幾何的に画面外へ大きくはみ出すが、上部48pxの情報パネルは
  //    このクリップで完全に保護される。共通の scenePush() には手を触れていない。
  GFX.setClipRect(0, PSY_TOP, 320, 240 - PSY_TOP);

  // ── ACCENT割り込み中：MOTIONの状態は【裏で進み続ける】──
  //    戻ってきたときには渦が「跳んで」いる＝ジャンプカットが自然に生まれる。
  if (psyIntrLeft > 0) {
    psyIntrLeft--;
    const PsyPal& P = PSY_PAL[psyPal];
    if (psyLayer == PSY_LAYER_MOTION) {
      float t    = (float)(psyIdx + 1) / (float)psyFrames;
      float ramp = 1.0f + PSY_RAMP * powf(t, 2.2f);
      psyMotionStep(psyKind, ramp);                       // ← 割り込み中も進める
      if (psyIntrKind == PSY_A_SCAN) psyMotionDraw(psyKind, false);   // 下地としてMOTIONを描く
    }
    switch (psyIntrKind) {
      case PSY_A_FACE:   psyAccentFace(P);  break;
      case PSY_A_INVERT: psyMotionDraw(psyKind, true); break;
      case PSY_A_STAB:   psyAccentStab(P);  break;
      case PSY_A_GIANT:  psyAccentGiant(P); break;
      default:           psyAccentScan();   break;
    }
    GFX.clearClipRect();
    return;
  }

  // ── 通常のショット進行 ──
  psyIdx++;
  if (psyIdx >= psyFrames) psyNextShot();

  if (psyLayer == PSY_LAYER_MOTION) {
    // 終盤ほど加速する（ビルドアップ）。頂点で断ち切られて次の世界へ落ちる。
    float t    = (float)psyIdx / (float)psyFrames;
    float ramp = 1.0f + PSY_RAMP * powf(t, 2.2f);
    psyMotionStep(psyKind, ramp);

    // 立ち上がりと終端は割り込ませない（MOTIONの導入と山場を潰さないため）
    if (psyIdx > 2 && psyIdx + 1 < psyFrames) {
      if (psyChance(PSY_INTR_PCT)) {
        psyIntrKind = psyPickAccent();
        psyIntrLeft = (uint8_t)psyRndI(PSY_ACCENT_MIN_F, PSY_ACCENT_MAX_F) - 1;
        // Face ACCENT はここで一度だけ見た目を確定させる（2コマ目は zoom のみ +15%）
        if (psyIntrKind == PSY_A_FACE) psyFaceShotBegin();
        const PsyPal& P = PSY_PAL[psyPal];
        if (psyIntrKind == PSY_A_SCAN) psyMotionDraw(psyKind, false);
        switch (psyIntrKind) {
          case PSY_A_FACE:   psyAccentFace(P);  break;
          case PSY_A_INVERT: psyMotionDraw(psyKind, true); break;
          case PSY_A_STAB:   psyAccentStab(P);  break;
          case PSY_A_GIANT:  psyAccentGiant(P); break;
          default:           psyAccentScan();   break;
        }
        GFX.clearClipRect();
        return;
      }
      if (psyChance(PSY_INVERT_PCT)) {         // 構図はそのまま色だけ1コマ反転
        psyMotionDraw(psyKind, true);
        GFX.clearClipRect();
        return;
      }
    }
    psyMotionDraw(psyKind, false);
  } else {
    psyFlashDraw(psyKind);
  }

  GFX.clearClipRect();   // 顔描画・他の描画に影響しないよう必ず解除する
}

// ############################################################################
// #  Hypnotic Vortex Lighting（2026-07-30 追加／2026-07-31 実機評価で正式採用）#
// ############################################################################
//
// ■ 経緯
//   当初はOptical Illusion（錯視）系Lightingとして Expanding Hole /
//   Moire Breathing / Hypnotic Vortex の3種を試作したが、実機評価の結果
//   「説明なしで見た瞬間に面白い・強烈」という基準に達したのは
//   Hypnotic Vortexのみだった。Expanding Hole（狙った奥行き感が出ず）・
//   Moire Breathing（テレビの砂嵐のような見え方になった）は不採用とし、
//   関連コード（enum・テーブル・render関数・定数・WebUI説明文）は
//   全て削除した。以下はHypnotic Vortex単体の説明。
//
// ■ 共通方針
//   ・配色は白黒のみ（Psychedelic/Tranceのような多原色ランダムにはしない）。
//   ・上部48px（情報パネル）には一切描画しない。SCENE_TOP(=48)を下限にし、
//     Psychedelic/Eye Slot/Kaleidoscopeと同じ GFX.setClipRect() による
//     防御も併用する。
//   ・cfg_lightingMask==0（Lighting全OFF）経路・既存Lighting・既存
//     Visualizer・顔の座標や黒本体の形状には一切手を入れていない。
// ============================================================================

// ============================================================================
// Lighting #16 : Hypnotic Vortex
//
// ■ コンセプト（2026-07-31 修正：実機評価を反映し6本化。脈動は削除し
//   確認済みの「回転版」を忠実に実装。
//   以前の実装は5本（奇数）の境界を単純に交互色分けしていたため、円周の
//   継ぎ目で同色（アーム0とアーム4）が隣接し、「黒2本＋白2本＋太さが
//   倍に見える黒1本」という意図しない見え方になっていた。VTX_ARMSを
//   6（偶数）にすることで、境界を一周しても黒→白→黒→白→黒→白→(黒に
//   戻る)ときれいに交互接続し、6本すべてが同じ角度幅（等幅）になる。
//   腕の本数が増えた分、1本あたりの太さは以前よりやや細くなる（意図した変更）。
//
//   中心へ向かってテーパーする太い螺旋アーム6本がゆっくり実回転する。
//   既存 Tempest Tunnel（LIGHT_TUNNEL）とは描画原理・見た目とも別物：
//     ・Tempest Tunnel … 8〜16角形の【輪郭線のみのリング】の奥行き位相
//       (tunFlowPhase)が進むことで手前へ流れてくるベクターアート。
//     ・Hypnotic Vortex … 画面全体を6分割する【塗りつぶした太いウェッジ】
//       （扇形が中心で1点に集まる＝自然にテーパーする）で、画面の大部分
//       を占有する。実際の回転速度は遅いが、渦の分厚さで吸引感を強める。
//   形状・動き・画面占有・体感のいずれもTempest Tunnelと重ならないため、
//   排他制御は追加していない（両方を同時にONにすることも可能）。
//
// ■ 負荷対策
//   ・半径・らせんの捻れ量はどれも回転角に依存しないため、
//     buildVortexTable() で起動後（モード初回のみ）1回だけ powfで計算し
//     static配列へキャッシュする。
//   ・毎フレームは「キャッシュ値＋現在の回転角」から境界7本（6区画+折返し）
//     ぶんの頂点のcosf/sinfを求めて GFX.fillTriangle で塗るだけ（頂点単位
//     の三角関数のみ。画面全ピクセルに対するatan2等は一切使わない）。
//   ・境界座標を入れる配列は固定サイズのstatic配列（動的malloc/freeなし）。
// ============================================================================
#define VTX_TOP        SCENE_TOP
#define VTX_CX         160
#define VTX_CY         ((VTX_TOP + 240) / 2)
#define VTX_ARMS       6           // 偶数にすることで円周一周後も黒/白が正しく交互接続する
#define VTX_STEPS      24
#define VTX_MAXR       196.0f
#define VTX_TWIST      2.6f        // アーム1本が中心→外周で捻れる角度(rad)
#define VTX_ROT_SPEED  0.15f       // rad/秒（1周 約42秒のゆっくりした実回転。脈動なし＝確認済みの回転版のみ）

static float vtxRadiusTab[VTX_STEPS];
static float vtxTwistTab[VTX_STEPS];
static bool  vtxReady = false;

void buildVortexTable() {
  for (int s = 0; s < VTX_STEPS; s++) {
    float t = (float)s / (float)(VTX_STEPS - 1);
    vtxRadiusTab[s] = VTX_MAXR * powf(t, 0.88f);
    vtxTwistTab[s]  = t * VTX_TWIST;
  }
  vtxReady = true;
}

void lightRenderVortex(bool needsInit, bool fullRepaint) {
  if (!vtxReady) buildVortexTable();

  static float          vtxRot    = 0.0f;
  static unsigned long  vtxPrevMs = 0;
  unsigned long now = millis();
  if (needsInit || vtxPrevMs == 0) { vtxRot = 0.0f; vtxPrevMs = now; }
  float dt = (now - vtxPrevMs) / 1000.0f;
  if (dt > 0.5f) dt = 0.5f;
  vtxPrevMs = now;
  vtxRot += dt * VTX_ROT_SPEED;

  GFX.setClipRect(0, VTX_TOP, 320, 240 - VTX_TOP);
  lightFillRect(0, VTX_TOP, 320, 240 - VTX_TOP, WHITE);

  const float twoPiOverArms = 6.2831853f / VTX_ARMS;
  uint16_t colBlack = lightBright(BLACK);
  uint16_t colWhite = lightBright(WHITE);

  // 境界曲線（0〜VTX_ARMSの7本＝6区画+折返し）の頂点を先に全部求めておく。
  static float bx[VTX_ARMS + 1][VTX_STEPS];
  static float by[VTX_ARMS + 1][VTX_STEPS];
  for (int k = 0; k <= VTX_ARMS; k++) {
    float baseAng = (float)(k % VTX_ARMS) * twoPiOverArms + vtxRot;
    for (int s = 0; s < VTX_STEPS; s++) {
      float ang = baseAng + vtxTwistTab[s];
      float r   = vtxRadiusTab[s];
      bx[k][s] = VTX_CX + cosf(ang) * r;
      by[k][s] = VTX_CY + sinf(ang) * r;
    }
  }

  // 隣り合う境界曲線どうしの間（区画すべて）を交互に黒／白で塗る。
  // 区画は中心(半径0)で1点に集まるため、扇形の形状そのものが
  // 「中心へ向かってテーパーする」形になる（別途すぼめる処理は不要）。
  for (int k = 0; k < VTX_ARMS; k++) {
    uint16_t col = (k & 1) ? colWhite : colBlack;
    for (int s = 0; s < VTX_STEPS - 1; s++) {
      GFX.fillTriangle((int)bx[k][s],   (int)by[k][s],   (int)bx[k+1][s],   (int)by[k+1][s],   (int)bx[k][s+1],   (int)by[k][s+1],   col);
      GFX.fillTriangle((int)bx[k+1][s], (int)by[k+1][s], (int)bx[k+1][s+1], (int)by[k+1][s+1], (int)bx[k][s+1],   (int)by[k][s+1],   col);
    }
  }
  GFX.clearClipRect();
}

// ============================================================================
// Lighting #17 : Aquarium
//
// ■ コンセプト
//   往年の水槽スクリーンセーバー「Marine Aquarium」を思い出させる、眺めていて
//   楽しい水中演出。ただし配色・魚のデザイン・背景要素はすべてKariPom独自に
//   起こしたもので、元ソフトウェアの画像・データは一切使用していない。
//
// ■ 2026-08-09 実機フィードバックによる改訂（v1.1）
//   実機評価で「魚が小さすぎてミジンコのように見える」という指摘を受け、
//   魚を大幅に大型化する方向へ変更した。多数の小さな魚を泳がせるのではなく、
//   AQU_FISH_COUNT=3（同時に見えるのは2〜3匹程度）に絞り、他のLighting
//   （Fighter Duelのキャラクター）程度の存在感を目安に1匹ごとを丁寧に作り込む。
//   背景（グラデーション・光の筋・岩・水草・砂地・気泡）は好評だったため
//   ほぼ無改造のまま維持し、今回は魚の大きさとグラフィックのみを変更した。
//
// ■ 背景
//   上（水面に近い）ほど淡く、下（深い）ほど沈んだ青緑になる帯状グラデーション
//   で「深い水中」を表現する。加えてごくわずかに明るい光の筋を2本、ゆっくり
//   左右に揺らしながら重ねることで水の揺らぎ感を出す。下部には岩・水草・
//   砂地を控えめに配置し、主役である魚の視認性を妨げないよう彩度・明度を抑える。
//
// ■ 魚
//   体形・ヒレ・模様が一目で違うと分かる4種の魚種（縦に高いエンゼルフィッシュ風、
//   横長で側線の通る回遊魚風、大きな扇尾を持つベタ／グッピー風、丸胴に横帯の
//   クマノミ風）をあらかじめ定義し、出現のたびにその中から1種＋個体差（色相・
//   大きさ・速度・泳ぐ高さ・左右どちらへ泳ぐか）を乱数で決める。頭から尾へ
//   すぼまる胴の輪郭・エラ・2トーン以上の体色・ハイライトと陰影・種ごとの
//   模様を持たせ、単純な楕円＋三角形に見えないよう作り込む。尾びれは常に
//   左右へはためき、背びれ・胸びれもわずかに揺れるため静止画的に見えない。
//   画面外へ完全に出たら即座に反対側へワープさせるのではなく、いったん
//   非表示にしてランダムな待ち時間（1〜4秒程度）を置いてから再登場させることで、
//   全個体が一斉に湧き直す不自然さを避けている。
//
// ■ 気泡
//   下から上へゆっくり上昇する小さな円をハイライト付きで描き、水面
//   （画面上端＝SCENE_TOP）へ到達したら底からランダムなX位置で再スタートする。
//
// ■ 負荷対策
//   ・魚の形状は種プリセット4種＋個体差の乱数（出現時に1度だけ決定）で作り、
//     毎フレームは位置・角度・はためき位相の更新と1匹あたり十数個の
//     プリミティブ描画のみ。同時に泳ぐ魚を3匹に絞ったことで、1匹の描画が
//     増えても全体の描画コストは従来と同程度に収まる。
//   ・動的メモリ確保なし（すべて固定長static配列）。
//   ・背景グラデーションは12本のfillRectのみ（1ピクセル単位の演算はしない）。
//   ・上部48px（情報パネル）には一切描画しない。SCENE_TOP(=48)を下限にし、
//     他のLightingと同じGFX.setClipRect()による防御も併用する。
// ============================================================================
#define AQU_TOP            SCENE_TOP
#define AQU_FISH_COUNT     3              // 大型化に伴い、同時表示は2〜3匹程度に絞った
#define AQU_SPECIES_COUNT  4
#define AQU_BUBBLE_COUNT   10
#define AQU_WEED_COUNT     3
#define AQU_WEED_BLADES    3
#define AQU_WEED_SEGS      5
#define AQU_ROCK_COUNT     2
#define AQU_SAND_DOT_COUNT 14

static uint32_t aquRng = 0x9E3779B9u;
static inline uint32_t aquRand()   { aquRng = aquRng * 1664525u + 1013904223u; return aquRng; }
static inline float    aquRand01() { return (float)(aquRand() & 0xFFFF) / 65535.0f; }
static inline uint16_t aquRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint8_t aquClamp255(int v) { return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v)); }

static bool aquReady = false;

// 魚種プリセット（体形比率・尾びれサイズ・模様タイプ・ヒレの構え・泳ぐ速さが種ごとに異なる。
// サイズはscale=1.0基準の値で、実際の描画サイズはaquFishScale[]（1.05〜1.90）を掛けたもの）。
//   pattern    … 0=無地(2トーン) 1=縦縞 2=斑点 3=横帯1本(白フチ付き) 4=側線(頭から尾の一直線)
//   longFins   … true: 背びれ・尻びれが体から大きくトレイン状に伸びる（エンゼルフィッシュ風）
//   tailStyle  … 0=フォーク型（通常） 1=大きな扇型（ベタ／グッピー風）
//   speedMul   … 種による泳ぐ速さの違い（1.0基準）
struct AquSpecies { float bodyLen, bodyHi, tailSize; uint8_t pattern; bool longFins; uint8_t tailStyle; float speedMul; };
static const AquSpecies AQU_SPECIES[AQU_SPECIES_COUNT] = {
  // 0: 縦に高い熱帯魚（エンゼルフィッシュ風）… 体高が高く、背びれ・尻びれが長く尾を引く
  { 26.0f, 32.0f, 16.0f, 1, true,  0, 0.85f },
  // 1: 横長の魚（回遊魚風）… 細長い胴で頭が尖り、側線が頭から尾まで一直線に通る
  { 36.0f, 15.0f, 15.0f, 4, false, 0, 1.15f },
  // 2: 大きな尾びれを持つ魚（ベタ／グッピー風）… 胴は小さめだが尾が体より大きく優雅に広がる
  { 20.0f, 14.0f, 28.0f, 0, false, 1, 0.75f },
  // 3: 丸みの強い魚（クマノミ風）… ずんぐりした丸胴に太い白フチ付き横帯
  { 21.0f, 21.0f, 11.0f, 3, false, 0, 0.95f },
};

// 魚の状態（固定長static配列。動的確保なし）
static bool     aquFishActive[AQU_FISH_COUNT];
static uint8_t  aquFishSpecies[AQU_FISH_COUNT];
static float    aquFishX[AQU_FISH_COUNT], aquFishBaseY[AQU_FISH_COUNT];
static int8_t   aquFishDir[AQU_FISH_COUNT];            // +1=右向き / -1=左向き
static float    aquFishSpeed[AQU_FISH_COUNT];          // px/フレーム
static float    aquFishScale[AQU_FISH_COUNT];          // 1.05〜1.90（大型化後の個体差）
static float    aquFishBobPhase[AQU_FISH_COUNT], aquFishBobFreq[AQU_FISH_COUNT], aquFishBobAmp[AQU_FISH_COUNT];
static float    aquFishTailPhase[AQU_FISH_COUNT];
static uint16_t aquFishHue[AQU_FISH_COUNT];             // 個体差の色相シード
static unsigned long aquFishRespawnAt[AQU_FISH_COUNT];

// 気泡
static float aquBubbleX[AQU_BUBBLE_COUNT], aquBubbleY[AQU_BUBBLE_COUNT];
static float aquBubbleR[AQU_BUBBLE_COUNT], aquBubbleSpeed[AQU_BUBBLE_COUNT], aquBubbleSway[AQU_BUBBLE_COUNT];

// 水草（AQU_WEED_COUNT株 × AQU_WEED_BLADES本）
static float    aquWeedBaseX[AQU_WEED_COUNT];
static float    aquWeedHeight[AQU_WEED_COUNT][AQU_WEED_BLADES];
static float    aquWeedPhase[AQU_WEED_COUNT][AQU_WEED_BLADES];
static uint16_t aquWeedColor[AQU_WEED_COUNT][AQU_WEED_BLADES];

// 岩
static float aquRockX[AQU_ROCK_COUNT], aquRockY[AQU_ROCK_COUNT], aquRockR[AQU_ROCK_COUNT];

// 砂地のテクスチャ用固定ドット
static int16_t aquSandX[AQU_SAND_DOT_COUNT], aquSandY[AQU_SAND_DOT_COUNT];

static void aquSpawnFish(int i) {
  aquFishSpecies[i] = (uint8_t)(aquRand() % AQU_SPECIES_COUNT);
  const AquSpecies &sp = AQU_SPECIES[aquFishSpecies[i]];

  aquFishDir[i]   = (aquRand() & 1u) ? 1 : -1;
  aquFishScale[i] = 1.05f + aquRand01() * 0.85f;                          // 1.05〜1.90：Fighter Duel程度の存在感を狙った大型化
  aquFishSpeed[i] = (0.20f + aquRand01() * 0.16f) * sp.speedMul;          // ぼんやり眺められる速度（種ごとに差をつける）

  float scale = aquFishScale[i];
  // 縦方向の必要マージンは種の特徴（トレイン状のヒレ／大きな扇尾）によって大きく異なるため、
  // 種ごとに見積もって、画面上下でヒレが極端に切れないようにする。
  float finVert   = sp.longFins ? sp.bodyHi * 0.9f : (sp.tailStyle == 1 ? sp.tailSize * 0.85f : sp.tailSize * 0.25f);
  float vertHalf  = (sp.bodyHi * 0.5f + finVert) * scale;
  float availY    = (240.0f - (float)AQU_TOP) - 2.0f * vertHalf - 12.0f;
  if (availY < 0.0f) availY = 0.0f;
  aquFishBaseY[i] = (float)AQU_TOP + vertHalf + 6.0f + aquRand01() * availY;

  aquFishBobPhase[i]  = aquRand01() * 6.2831853f;
  aquFishBobFreq[i]   = 0.7f + aquRand01() * 0.5f;
  aquFishBobAmp[i]    = 3.0f + aquRand01() * 4.0f;
  aquFishTailPhase[i] = aquRand01() * 6.2831853f;
  aquFishHue[i]       = (uint16_t)(aquRand() % 256);

  float margin = (sp.bodyLen * 0.5f + sp.tailSize + 16.0f) * scale;
  aquFishX[i] = (aquFishDir[i] > 0) ? -margin : (320.0f + margin);
  aquFishActive[i] = true;
}

void buildAquariumTable() {
  for (int i = 0; i < AQU_FISH_COUNT; i++) {
    aquSpawnFish(i);
    aquFishX[i] = aquRand01() * 320.0f;   // 起動直後は既に画面内にばらけさせる（一斉出現を避ける）
    aquFishRespawnAt[i] = 0;
  }
  for (int i = 0; i < AQU_BUBBLE_COUNT; i++) {
    aquBubbleX[i]     = aquRand01() * 320.0f;
    aquBubbleY[i]     = (float)AQU_TOP + aquRand01() * (240.0f - (float)AQU_TOP);
    aquBubbleR[i]     = 1.0f + aquRand01() * 2.0f;
    aquBubbleSpeed[i] = 0.25f + aquRand01() * 0.35f;
    aquBubbleSway[i]  = aquRand01() * 6.2831853f;
  }
  for (int w = 0; w < AQU_WEED_COUNT; w++) {
    aquWeedBaseX[w] = 30.0f + (float)w * (260.0f / (float)AQU_WEED_COUNT) + aquRand01() * 20.0f;
    for (int b = 0; b < AQU_WEED_BLADES; b++) {
      aquWeedHeight[w][b] = 26.0f + aquRand01() * 20.0f;
      aquWeedPhase[w][b]  = aquRand01() * 6.2831853f;
      aquWeedColor[w][b]  = (b & 1) ? aquRgb(34, 110, 70) : aquRgb(24, 84, 54);
    }
  }
  for (int r = 0; r < AQU_ROCK_COUNT; r++) {
    aquRockX[r] = 60.0f + (float)r * (200.0f / (float)AQU_ROCK_COUNT) + aquRand01() * 30.0f;
    aquRockY[r] = 236.0f;
    aquRockR[r] = 14.0f + aquRand01() * 8.0f;
  }
  for (int s = 0; s < AQU_SAND_DOT_COUNT; s++) {
    aquSandX[s] = (int16_t)(aquRand01() * 320.0f);
    aquSandY[s] = (int16_t)(234.0f + aquRand01() * 5.0f);
  }
  aquReady = true;
}

static void aquDrawBackground(unsigned long now) {
  const int BANDS = 12;
  int total = 240 - AQU_TOP;
  int bandH = total / BANDS;
  for (int b = 0; b < BANDS; b++) {
    float t = (float)b / (float)(BANDS - 1);      // 0(上・淡い)〜1(下・深い)
    uint8_t r  = aquClamp255((int)(10 + (1.0f - t) * 12.0f));
    uint8_t g  = aquClamp255((int)(70 - t * 40.0f));
    uint8_t bl = aquClamp255((int)(95 - t * 45.0f));
    int y0 = AQU_TOP + b * bandH;
    int h  = (b == BANDS - 1) ? (240 - y0) : bandH;
    lightFillRect(0, y0, 320, h, aquRgb(r, g, bl));
  }
  // わずかに明るい光の筋を2本、ゆっくり左右へ揺らして重ねる（水の揺らぎ）
  float t1 = (float)now / 1000.0f;
  for (int k = 0; k < 2; k++) {
    float sway = sinf(t1 * 0.35f + (float)k * 2.4f) * 24.0f;
    float baseX = 80.0f + (float)k * 150.0f + sway;
    uint16_t col = aquRgb(46, 108, 118);
    GFX.fillTriangle((int)baseX, AQU_TOP, (int)(baseX + 20), AQU_TOP, (int)(baseX - 34), 240, lightBright(col));
  }
}

static void aquDrawRocksAndSand() {
  for (int r = 0; r < AQU_ROCK_COUNT; r++) {
    float cx = aquRockX[r], cy = aquRockY[r], rad = aquRockR[r];
    uint16_t base = aquRgb(58, 66, 78);
    uint16_t hi   = aquRgb(88, 98, 112);
    GFX.fillCircle((int)cx,               (int)cy,               (int)rad,         lightBright(base));
    GFX.fillCircle((int)(cx - rad*0.6f),  (int)(cy - rad*0.1f),  (int)(rad*0.7f),  lightBright(base));
    GFX.fillCircle((int)(cx + rad*0.55f), (int)(cy - rad*0.05f), (int)(rad*0.65f), lightBright(base));
    GFX.fillCircle((int)(cx - rad*0.2f),  (int)(cy - rad*0.75f), (int)(rad*0.35f), lightBright(hi));
  }
  lightFillRect(0, 234, 320, 240 - 234, aquRgb(70, 78, 60));
  for (int s = 0; s < AQU_SAND_DOT_COUNT; s++) {
    lightFillRect(aquSandX[s], aquSandY[s], 1, 1, aquRgb(90, 96, 74));
  }
}

static void aquDrawWeeds(unsigned long now) {
  float t = (float)now / 1000.0f;
  for (int w = 0; w < AQU_WEED_COUNT; w++) {
    for (int b = 0; b < AQU_WEED_BLADES; b++) {
      float baseX = aquWeedBaseX[w] + (float)(b - 1) * 6.0f;
      float h      = aquWeedHeight[w][b];
      float phase  = aquWeedPhase[w][b];
      int px = (int)baseX, py = 238;
      for (int s = 1; s <= AQU_WEED_SEGS; s++) {
        float ft   = (float)s / (float)AQU_WEED_SEGS;
        float sway = sinf(t * 0.9f + phase + ft * 2.2f) * (4.0f * ft);
        int nx = (int)(baseX + sway);
        int ny = (int)(238.0f - h * ft);
        lightDrawLine(px, py, nx, ny, aquWeedColor[w][b]);
        px = nx; py = ny;
      }
    }
  }
}

static void aquUpdateAndDrawBubbles(unsigned long now) {
  float t = (float)now / 1000.0f;
  for (int i = 0; i < AQU_BUBBLE_COUNT; i++) {
    aquBubbleY[i] -= aquBubbleSpeed[i];
    if (aquBubbleY[i] < (float)AQU_TOP - 4.0f) {
      aquBubbleY[i]     = 238.0f;
      aquBubbleX[i]     = aquRand01() * 320.0f;
      aquBubbleR[i]     = 1.0f + aquRand01() * 2.0f;
      aquBubbleSpeed[i] = 0.25f + aquRand01() * 0.35f;
    }
    float sway = sinf(t * 1.4f + aquBubbleSway[i]) * 3.0f;
    int bx = (int)(aquBubbleX[i] + sway);
    int by = (int)aquBubbleY[i];
    int r  = (int)aquBubbleR[i];
    GFX.fillCircle(bx, by, r, lightBright(aquRgb(190, 222, 228)));
    GFX.fillCircle(bx - (r > 1 ? 1 : 0), by - (r > 1 ? 1 : 0), 1, lightBright(WHITE));
  }
}

// 1匹分の魚を描く（体色・体形・ヒレ・模様は種プリセット＋個体差の乱数で決まる）。
// 2026-08-09改訂：実機評価で「小さすぎて魚に見えない」との指摘を受け、種ごとの
// シルエットの違い・胴の自然な輪郭（頭の鼻先〜尾へのすぼまり）・エラ・ハイライト／
// 陰影・種別の模様を持たせ、単純な楕円＋三角形に見えないよう作り込んだ。
static void aquDrawFish(int i) {
  const AquSpecies &sp = AQU_SPECIES[aquFishSpecies[i]];
  float scale = aquFishScale[i];
  int   dir   = aquFishDir[i];
  float x = aquFishX[i];
  float y = aquFishBaseY[i] + sinf(aquFishBobPhase[i]) * aquFishBobAmp[i];

  float bodyLen = sp.bodyLen  * scale;
  float bodyHi  = sp.bodyHi   * scale;
  float tailLen = sp.tailSize * scale;

  uint16_t seed = aquFishHue[i];
  uint8_t rr = aquClamp255(110 + (int)(seed % 110));
  uint8_t gg = aquClamp255(70  + (int)((seed >> 2) % 140));
  uint8_t bb = aquClamp255(130 + (int)((seed >> 4) % 110));
  uint16_t bodyTop    = aquRgb(rr, gg, bb);                                                     // 背側（濃いめ）
  uint16_t bodyBelly  = aquRgb(aquClamp255(rr + 70), aquClamp255(gg + 60), aquClamp255(bb + 50)); // 腹側（明るい）
  uint16_t bodyShadow = aquRgb(aquClamp255(rr - 50), aquClamp255(gg - 40), aquClamp255(bb - 40)); // 陰影
  uint16_t finCol     = aquRgb((uint8_t)(rr * 0.55f), (uint8_t)(gg * 0.55f), (uint8_t)(bb * 0.65f));
  uint16_t gillCol    = aquRgb(aquClamp255(rr - 35), aquClamp255(gg - 25), aquClamp255(bb - 25));
  uint16_t markCol    = (sp.pattern == 3) ? aquRgb(25, 20, 20) : aquRgb(250, 248, 240);
  uint16_t hiliteCol  = aquRgb(aquClamp255(rr + 110), aquClamp255(gg + 110), aquClamp255(bb + 100));

  float tailWag = sinf(aquFishTailPhase[i]) * 0.6f;
  float finWag  = sinf(aquFishTailPhase[i] * 0.75f + 1.0f) * 0.2f;

  float headX     = x + (bodyLen * 0.5f) * dir;
  float tailBaseX = x - (bodyLen * 0.5f) * dir;

  // ── 尾びれ（種によりフォーク型／大きな扇型を切り替える。体より先に描き付け根に隠す）──
  if (sp.tailStyle == 1) {
    // 大きな扇型（3枚の重ね三角で優雅な広がりを表現。ベタ／グッピー風）
    for (int lobe = -1; lobe <= 1; lobe++) {
      float lobeAng = (float)lobe * 0.42f + tailWag * 0.5f;
      float tx = tailBaseX - dir * tailLen * (1.05f + fabsf(tailWag) * 0.15f);
      float ty = y + sinf(lobeAng) * tailLen * 0.85f;
      GFX.fillTriangle((int)tailBaseX, (int)(y - 3), (int)tailBaseX, (int)(y + 3), (int)tx, (int)ty,
                        lightBright(lobe == 0 ? finCol : bodyShadow));
    }
  } else {
    float tTipX = tailBaseX - dir * tailLen * (1.0f + fabsf(tailWag) * 0.25f);
    float tTopY = y - tailLen * 0.55f + tailWag * tailLen * 0.55f;
    float tBotY = y + tailLen * 0.55f + tailWag * tailLen * 0.55f;
    GFX.fillTriangle((int)tailBaseX, (int)y, (int)tTipX, (int)tTopY, (int)tailBaseX, (int)(y - bodyHi * 0.12f), lightBright(finCol));
    GFX.fillTriangle((int)tailBaseX, (int)y, (int)tTipX, (int)tBotY, (int)tailBaseX, (int)(y + bodyHi * 0.12f), lightBright(finCol));
  }

  // ── 尻びれ／トレイン尻びれ（longFins種は体下から大きく尾を引く。エンゼルフィッシュ風）──
  if (sp.longFins) {
    int analX    = (int)(x - dir * bodyLen * 0.05f);
    int analBotY = (int)(y + bodyHi * 0.5f + bodyHi * 0.85f + finWag * 8.0f);
    GFX.fillTriangle(analX - (int)(bodyLen * 0.16f), (int)(y + bodyHi * 0.42f),
                      analX + (int)(bodyLen * 0.10f), (int)(y + bodyHi * 0.42f),
                      analX, analBotY, lightBright(finCol));
  }

  // ── 尾への付け根（体から尾びれへなだらかにすぼめ、自然な輪郭にする）──
  GFX.fillTriangle((int)tailBaseX, (int)(y - bodyHi * 0.38f), (int)tailBaseX, (int)(y + bodyHi * 0.38f),
                    (int)(tailBaseX - dir * bodyLen * 0.22f), (int)y, lightBright(bodyTop));

  // ── 体（背側は濃く、腹側は明るい2トーン）──
  GFX.fillEllipse((int)x, (int)y, (int)(bodyLen * 0.5f), (int)(bodyHi * 0.5f), lightBright(bodyTop));
  GFX.fillEllipse((int)x, (int)(y + bodyHi * 0.20f), (int)(bodyLen * 0.44f), (int)(bodyHi * 0.30f), lightBright(bodyBelly));

  // ── 鼻先（頭側を少しすぼめて自然な顔つきにする）──
  GFX.fillTriangle((int)headX, (int)(y - bodyHi * 0.30f), (int)headX, (int)(y + bodyHi * 0.30f),
                    (int)(headX + dir * bodyLen * 0.16f), (int)y, lightBright(bodyTop));

  // ── 背側のハイライト（艶）と腹側の陰影の細い筋 ──
  lightDrawLine((int)(x - bodyLen * 0.28f), (int)(y - bodyHi * 0.30f), (int)(x + bodyLen * 0.18f), (int)(y - bodyHi * 0.36f), hiliteCol);
  lightDrawLine((int)(x - bodyLen * 0.24f), (int)(y + bodyHi * 0.40f), (int)(x + bodyLen * 0.20f), (int)(y + bodyHi * 0.42f), bodyShadow);

  // ── エラ（頭の付け根に弧状の線を1本、やや濃い色で）──
  int gillX = (int)(headX - dir * bodyLen * 0.22f);
  lightDrawLine(gillX, (int)(y - bodyHi * 0.38f), gillX - dir * (int)(bodyHi * 0.10f), (int)(y + bodyHi * 0.38f), gillCol);

  // ── 模様（種ごとに縦縞／側線／横帯／斑点）──
  if (sp.pattern == 1) {          // 縦縞（3〜4本）
    for (int s = -1; s <= 2; s++) {
      int sx = (int)(x + ((float)s - 0.5f) * bodyLen * 0.20f);
      lightDrawLine(sx, (int)(y - bodyHi * 0.46f), sx, (int)(y + bodyHi * 0.46f), markCol);
    }
  } else if (sp.pattern == 2) {   // 斑点
    for (int s = 0; s < 5; s++) {
      float ang = (float)s * 1.25f + (float)seed;
      int sx = (int)(x + cosf(ang) * bodyLen * 0.24f);
      int sy = (int)(y + sinf(ang) * bodyHi * 0.24f);
      GFX.fillCircle(sx, sy, (int)(2.2f * scale) + 1, lightBright(markCol));
    }
  } else if (sp.pattern == 3) {   // 横帯1本（クマノミ風。白フチ付き）
    int sx = (int)x;
    lightFillRect(sx - (int)(3 * scale) - 1, (int)(y - bodyHi * 0.48f), (int)(6 * scale) + 2, (int)(bodyHi * 0.96f), aquRgb(250, 248, 240));
    lightFillRect(sx - (int)(3 * scale),     (int)(y - bodyHi * 0.46f), (int)(6 * scale),     (int)(bodyHi * 0.92f), markCol);
  } else if (sp.pattern == 4) {   // 側線（頭から尾まで通る一直線）
    lightDrawLine((int)headX, (int)y, (int)tailBaseX, (int)y, markCol);
  }

  // ── 背びれ（longFins種はさらに大きく体から伸びる）──
  int dorsalX    = (int)(x - dir * bodyLen * 0.08f);
  float dorsalH  = bodyHi * (sp.longFins ? 1.35f : 0.75f);
  int dorsalTopY = (int)(y - bodyHi * 0.5f - dorsalH + finWag * 10.0f);
  GFX.fillTriangle(dorsalX - (int)(bodyLen * 0.16f), (int)(y - bodyHi * 0.40f),
                    dorsalX + (int)(bodyLen * 0.10f), (int)(y - bodyHi * 0.40f),
                    dorsalX, dorsalTopY, lightBright(finCol));

  // ── 胸びれ（体側面。はっきり視認できる大きさにする）──
  int pecX = (int)(headX - dir * bodyLen * 0.22f);
  int pecY = (int)(y + bodyHi * 0.12f);
  GFX.fillTriangle(pecX, pecY,
                    pecX - dir * (int)(bodyLen * 0.20f), pecY + (int)(bodyHi * 0.55f + finWag * 8.0f),
                    pecX - dir * (int)(bodyLen * 0.06f), pecY, lightBright(finCol));

  // ── 目（小さな黒い目のみ。2026-08-09再々改訂：Aquariumはキャラクター表現ではなく
  //     リアルな熱帯魚・観賞魚を目標とする方針のため、白目・白フチは完全に廃止し、
  //     黒目＋任意の1ドットハイライトのみのシンプルな表現へ変更した。サイズは
  //     直前の縮小済みサイズを基準に維持している）──
  int eyeX = (int)(headX - dir * bodyLen * 0.10f);
  int eyeY = (int)(y - bodyHi * 0.10f);
  int eyeRFull = (int)(bodyHi * 0.17f) + 1;
  int eyeR = eyeRFull / 2;
  if (eyeR < 1) eyeR = 1;
  GFX.fillCircle(eyeX, eyeY, eyeR, lightBright(BLACK));         // 黒目のみ
  GFX.fillCircle(eyeX + (dir > 0 ? 1 : -1), eyeY - 1, 1, lightBright(WHITE));  // ごく小さなハイライト（1ドット）
}

static void aquUpdateFish(int i, unsigned long now, float dt) {
  if (!aquFishActive[i]) {
    if (aquFishRespawnAt[i] != 0 && (long)(now - aquFishRespawnAt[i]) >= 0) {
      aquSpawnFish(i);
      aquFishRespawnAt[i] = 0;
    }
    return;
  }
  aquFishX[i]         += aquFishSpeed[i] * aquFishDir[i];
  aquFishBobPhase[i]  += dt * aquFishBobFreq[i];
  aquFishTailPhase[i] += dt * 3.0f;   // 尾びれは体より速くはためく

  const AquSpecies &sp = AQU_SPECIES[aquFishSpecies[i]];
  float margin  = (sp.bodyLen * 0.5f + sp.tailSize + 16.0f) * aquFishScale[i];  // aquSpawnFish()と同じ見積もり
  bool offRight = (aquFishDir[i] > 0 && aquFishX[i] > 320.0f + margin);
  bool offLeft  = (aquFishDir[i] < 0 && aquFishX[i] < -margin);
  if (offRight || offLeft) {
    aquFishActive[i]     = false;
    aquFishRespawnAt[i]  = now + 1200UL + (unsigned long)(aquRand01() * 3200.0f);  // 1〜4秒程度で自然な再登場
  }
}

void lightRenderAquarium(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!aquReady) buildAquariumTable();

  static unsigned long aquPrevMs = 0;
  unsigned long now = millis();
  if (needsInit || aquPrevMs == 0) aquPrevMs = now;
  float dt = (now - aquPrevMs) / 1000.0f;
  if (dt > 0.5f) dt = 0.5f;
  aquPrevMs = now;

  GFX.setClipRect(0, AQU_TOP, 320, 240 - AQU_TOP);

  aquDrawBackground(now);
  aquDrawRocksAndSand();
  aquDrawWeeds(now);
  aquUpdateAndDrawBubbles(now);

  for (int i = 0; i < AQU_FISH_COUNT; i++) {
    aquUpdateFish(i, now, dt);
    if (aquFishActive[i]) aquDrawFish(i);
  }

  GFX.clearClipRect();   // 顔描画・他の描画に影響しないよう必ず解除する
}

// ============================================================================
// Lighting #18 : Flying Pompadour
//
// ■ コンセプト
//   1990年代Macの名作スクリーンセーバー「Flying Toasters」を思い出させる
//   オマージュ演出。トースターの代わりに「羽の生えたCoreS3風かりポム」が
//   飛ぶという、KariPom独自のオマージュにしている。元作品の画像・データは
//   一切使わないが、右上→左下への飛行・左右の大きな羽・羽ばたき・複数体の
//   流れ・遠近感という原作の構図と動きは意識的に踏襲している。
//
// ■ 2026-08-09 実機フィードバックによる改訂（v1.1）
//   実機評価で「本体が何か分かりにくい」「羽が頭上から生えて見える」
//   「飛行方向がFlying Toastersと逆」との指摘を受け、以下を変更した。
//     ・本体をCoreS3を連想させる小型直方体デバイス（正面に黒いディスプレイ・
//       ベゼル・側面/底面の厚み）へ変更し、ディスプレイにKariPomらしい顔を表示。
//     ・羽を本体の左右側面から生やす配置に修正（頭上からではない）。
//     ・飛行方向を右上→左下へ反転（原作Flying Toastersと同じ向き）。
//     ・遠景/中景/前景の3段階で大きさ・速度をはっきり分け、前景個体は本体の
//       ディテール（画面の顔・ベゼル・左右の羽）がしっかり視認できる大きさにした。
//   背景（夕暮れ〜夜空のグラデーション・星・雲）は変更していない。
//
// ■ 背景
//   夕暮れから夜へ沈んでいく空を帯状グラデーションで表現し、控えめに瞬く星、
//   ゆっくり左へ流れる雲のシルエットを重ねて奥行きを出す。
//
// ■ 飛行体
//   最大FLYP_SLOT_COUNT体を同時に管理し、非アクティブなスロットはランダムな
//   待ち時間の後に個別に出現する（一定間隔ではなくランダム感のある出現）。
//   遠景/中景/前景の3層を持ち、奥ほど小さく・遅く、手前ほど大きく・速いことで
//   奥行きを表現する。画面右上外側から現れ、斜め左下へ横切って画面外（左または
//   下）へ抜ける（Flying Toastersと同じ向き）。羽は4コマの羽ばたきアニメーション
//   を持ち、本体は正面・側面・底面の3面と陰影・ハイライトで立体感を出す。
//   パレットは4種のKariPomらしいパステルカラーからランダムに選ぶ。
//
// ■ 負荷対策
//   ・雲・星は出現時に1度だけ乱数で決めた固定長static配列を使い回す。
//   ・1体あたりの描画は本体3面・ディスプレイ・顔・羽2枚・航跡の
//     二十プリミティブ前後のみで、ピクセル単位の演算はしない。
//   ・動的メモリ確保なし。
//   ・上部48px（情報パネル）には一切描画しない。SCENE_TOP(=48)を下限にし、
//     他のLightingと同じGFX.setClipRect()による防御も併用する。
// ============================================================================
#define FLYP_TOP          SCENE_TOP
#define FLYP_SLOT_COUNT   6
#define FLYP_LAYER_COUNT  3     // 0=遠景(小・遅) / 1=中景 / 2=前景(大・速)
#define FLYP_PALETTE_COUNT 4
#define FLYP_STAR_COUNT   10
#define FLYP_CLOUD_COUNT  3

static uint32_t flypRng = 0x51ED270Bu;
static inline uint32_t flypRand()   { flypRng = flypRng * 1664525u + 1013904223u; return flypRng; }
static inline float    flypRand01() { return (float)(flypRand() & 0xFFFF) / 65535.0f; }
static inline uint16_t flypRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static bool flypReady = false;

// 羽ばたき4コマ（羽先の上下オフセット倍率。0=最下点…1=最上点の往復）
static const float FLYP_WING_FRAMES[4] = { -1.0f, -0.25f, 0.5f, 1.0f };

// KariPomらしいパステルカラーパレット（本体シェル／側面・底面の陰影／ディスプレイの
// ベゼル／羽／羽の陰影）。CoreS3風本体のシェルカラーとして使う。
struct FlypPalette { uint16_t body, bodyShade, bezel, wing, wingShade; };
static FlypPalette flypPaletteRt[FLYP_PALETTE_COUNT];   // 実行時（起動後1回）にflypBuildPalette()が確定するパレット本体

// 飛行体スロット
static bool     flypActive[FLYP_SLOT_COUNT];
static uint8_t  flypLayer[FLYP_SLOT_COUNT];      // 0=遠景(奥・小・遅) / 1=中景 / 2=前景(手前・大・速)
static uint8_t  flypPaletteIdx[FLYP_SLOT_COUNT];
static float    flypX[FLYP_SLOT_COUNT], flypY[FLYP_SLOT_COUNT];
static float    flypVX[FLYP_SLOT_COUNT], flypVY[FLYP_SLOT_COUNT];
static float    flypScale[FLYP_SLOT_COUNT];
static float    flypWingPhase[FLYP_SLOT_COUNT];
static unsigned long flypNextSpawnAt[FLYP_SLOT_COUNT];

// 星・雲（背景装飾）
static int16_t  flypStarX[FLYP_STAR_COUNT], flypStarY[FLYP_STAR_COUNT];
static unsigned long flypStarNextBlinkAt[FLYP_STAR_COUNT];
static bool     flypStarBright[FLYP_STAR_COUNT];
static float    flypCloudX[FLYP_CLOUD_COUNT], flypCloudY[FLYP_CLOUD_COUNT], flypCloudR[FLYP_CLOUD_COUNT];
static float    flypCloudSpeed[FLYP_CLOUD_COUNT];

static void flypBuildPalette() {
  // 4色ともKariPomの世界観に合う淡いパステルの本体シェル＋やや濃い側面／ベゼル／羽の配色。
  flypPaletteRt[0] = { flypRgb(238, 232, 222), flypRgb(180, 172, 160), flypRgb(120, 108, 118), flypRgb(255, 196, 206), flypRgb(206, 150, 160) }; // アイボリー
  flypPaletteRt[1] = { flypRgb(214, 232, 248), flypRgb(158, 182, 206), flypRgb(96,  108, 130), flypRgb(190, 222, 240), flypRgb(140, 176, 198) }; // ペールブルー
  flypPaletteRt[2] = { flypRgb(224, 246, 224), flypRgb(166, 202, 172), flypRgb(100, 128, 110), flypRgb(210, 236, 190), flypRgb(158, 196, 140) }; // ミント
  flypPaletteRt[3] = { flypRgb(236, 224, 246), flypRgb(180, 164, 206), flypRgb(122, 108, 138), flypRgb(224, 200, 240), flypRgb(176, 152, 202) }; // ラベンダー
}

static void flypSpawn(int i) {
  uint8_t layer = (uint8_t)(flypRand() % FLYP_LAYER_COUNT);   // 遠景/中景/前景をほぼ均等に抽選
  flypLayer[i]      = layer;
  flypPaletteIdx[i] = (uint8_t)(flypRand() % FLYP_PALETTE_COUNT);

  static const float SCALE_LO[FLYP_LAYER_COUNT] = { 0.55f, 0.95f, 1.45f };
  static const float SCALE_HI[FLYP_LAYER_COUNT] = { 0.80f, 1.30f, 1.95f };
  // 2026-08-09: 実機評価で「前回より速く感じる」との指摘を受け、全層一律で約0.67倍
  // （1/1.5）に減速した。層ごとの速度差の比率自体は変更していない。
  static const float SPEED_LO[FLYP_LAYER_COUNT] = { 0.37f, 0.57f, 0.77f };
  static const float SPEED_HI[FLYP_LAYER_COUNT] = { 0.53f, 0.77f, 1.03f };
  flypScale[i] = SCALE_LO[layer] + flypRand01() * (SCALE_HI[layer] - SCALE_LO[layer]);
  float baseSpeed = SPEED_LO[layer] + flypRand01() * (SPEED_HI[layer] - SPEED_LO[layer]);

  // 右上→左下（Flying Toastersへのオマージュとして重要な向き）。水平からやや下向きの角度。
  float angDeg = 25.0f + flypRand01() * 25.0f;    // 25°〜50°
  float ang = angDeg * 3.14159265f / 180.0f;
  flypVX[i] = -cosf(ang) * baseSpeed;
  flypVY[i] =  sinf(ang) * baseSpeed;

  float margin = 30.0f * flypScale[i];
  flypX[i] = 320.0f + margin + flypRand01() * 40.0f;             // 右上外側から出現
  flypY[i] = (float)FLYP_TOP + 6.0f + flypRand01() * 70.0f;
  flypWingPhase[i] = flypRand01() * 6.2831853f;
  flypActive[i] = true;
}

void buildFlyingPompadourTable() {
  flypBuildPalette();
  for (int i = 0; i < FLYP_SLOT_COUNT; i++) {
    flypActive[i] = false;
    // 起動直後は全体が一斉に湧かないよう、出現タイミングをそれぞれずらす。
    flypNextSpawnAt[i] = millis() + (unsigned long)(flypRand01() * 3500.0f);
  }
  for (int s = 0; s < FLYP_STAR_COUNT; s++) {
    flypStarX[s] = (int16_t)(flypRand01() * 320.0f);
    flypStarY[s] = (int16_t)((float)FLYP_TOP + flypRand01() * 90.0f);   // 空の上寄りだけに置く
    flypStarBright[s] = (flypRand() & 1u) != 0;
    flypStarNextBlinkAt[s] = millis() + 400UL + (unsigned long)(flypRand01() * 2200.0f);
  }
  for (int c = 0; c < FLYP_CLOUD_COUNT; c++) {
    flypCloudX[c]     = flypRand01() * 320.0f;
    flypCloudY[c]     = (float)FLYP_TOP + 70.0f + flypRand01() * 90.0f;
    flypCloudR[c]      = 20.0f + flypRand01() * 16.0f;
    flypCloudSpeed[c] = 0.08f + flypRand01() * 0.10f;
  }
  flypReady = true;
}

static void flypDrawBackground(unsigned long now) {
  const int BANDS = 10;
  int total = 240 - FLYP_TOP;
  int bandH = total / BANDS;
  for (int b = 0; b < BANDS; b++) {
    float t = (float)b / (float)(BANDS - 1);   // 0(上・夕焼け)〜1(下・夜)
    uint8_t r = (uint8_t)(60  + (1.0f - t) * 70.0f);
    uint8_t g = (uint8_t)(40  + (1.0f - t) * 35.0f);
    uint8_t bl= (uint8_t)(70  + (1.0f - t) * 40.0f + t * 20.0f);
    int y0 = FLYP_TOP + b * bandH;
    int h  = (b == BANDS - 1) ? (240 - y0) : bandH;
    lightFillRect(0, y0, 320, h, flypRgb(r, g, bl));
  }

  // 星（控えめに瞬く）
  for (int s = 0; s < FLYP_STAR_COUNT; s++) {
    if ((long)(now - flypStarNextBlinkAt[s]) >= 0) {
      flypStarBright[s] = !flypStarBright[s];
      flypStarNextBlinkAt[s] = now + 400UL + (unsigned long)(flypRand01() * 2200.0f);
    }
    uint16_t col = flypStarBright[s] ? flypRgb(255, 250, 230) : flypRgb(140, 130, 150);
    lightFillRect(flypStarX[s], flypStarY[s], 1, 1, col);
  }

  // 雲のシルエット（ゆっくり左へ流れ、画面端でラップアラウンド）
  for (int c = 0; c < FLYP_CLOUD_COUNT; c++) {
    flypCloudX[c] -= flypCloudSpeed[c];
    if (flypCloudX[c] < -flypCloudR[c] * 2.2f) flypCloudX[c] = 320.0f + flypCloudR[c];
    float cx = flypCloudX[c], cy = flypCloudY[c], r = flypCloudR[c];
    uint16_t col = flypRgb(90, 70, 100);
    GFX.fillCircle((int)cx,              (int)cy, (int)(r*0.7f), lightBright(col));
    GFX.fillCircle((int)(cx - r*0.8f),   (int)(cy + r*0.15f), (int)(r*0.55f), lightBright(col));
    GFX.fillCircle((int)(cx + r*0.85f),  (int)(cy + r*0.1f),  (int)(r*0.5f), lightBright(col));
  }
}

// 1体分の飛行体を描く（CoreS3風の直方体本体・正面ディスプレイの顔・左右の羽・航跡）。
// 2026-08-09改訂：実機評価で「本体が何か分かりにくい／羽が頭上から生えて見える」との
// 指摘を受け、本体をCoreS3を連想させる小型直方体デバイスへ変更し、羽は本体の左右
// 側面から生やす配置に修正した（頭上からではない）。ディスプレイにはKariPomらしい
// 顔（表情バリエーション＋時々まばたき）を表示する。
static void flypDrawFlyer(int i) {
  const FlypPalette &pal = flypPaletteRt[flypPaletteIdx[i]];
  float scale = flypScale[i];
  float x = flypX[i], y = flypY[i];

  int bw    = (int)(20.0f * scale);
  int bh    = (int)(15.0f * scale);
  int depth = (int)(6.0f * scale) + 1;   // 側面・底面の厚み表現

  int bodyL = (int)(x - bw * 0.5f), bodyR = bodyL + bw;
  int bodyT = (int)(y - bh * 0.5f), bodyB = bodyT + bh;

  // ── 航跡（速度方向の後方へ、フェードする短い線を数本）──
  float spd = sqrtf(flypVX[i]*flypVX[i] + flypVY[i]*flypVY[i]);
  float dx = (spd > 0.0001f) ? flypVX[i] / spd : 1.0f;
  float dy = (spd > 0.0001f) ? flypVY[i] / spd : -1.0f;
  uint16_t trailCol = flypRgb(210, 200, 210);
  for (int k = 1; k <= 3; k++) {
    float tx = x - dx * (bw * 0.9f + k * 6.0f * scale);
    float ty = y - dy * (bw * 0.9f + k * 6.0f * scale);
    lightDrawLine((int)tx, (int)ty, (int)(tx - dx * 3.0f), (int)(ty - dy * 3.0f), trailCol);
  }

  // ── 羽ばたき量（4コマ）。羽を小型化したため振幅も従来よりやや控えめにする ──
  int frame = (int)(fmodf(flypWingPhase[i], 6.2831853f) / 6.2831853f * 4.0f) & 3;
  float wingLift = FLYP_WING_FRAMES[frame] * 5.0f * scale;

  // ── 羽（本体の左右側面から生やす、鳩のような丸みと厚みのある翼）──
  // 2026-08-09改訂：実機評価で「羽が本体に対して大きすぎる」「カモメのように細長い」
  // との指摘を受け、サイズを従来のおよそ半分に縮小し、根元が最も大きく先端へ向かって
  // 小さくなる3枚の羽根（丸いブロブ）を重ねる方式へ変更した。色を1枚おきに変えることで
  // 「複数の羽根が分かれて見える」丸みのある鳥の翼らしい質感を出している。
  float wingSpan = bw * 0.72f;   // 従来（bw*1.4）のおよそ半分
  {   // 左羽（奥側。やや小さく暗め）
    float ws = wingSpan * 0.92f;
    float lift0 = wingLift * 0.22f, lift1 = wingLift * 0.55f, lift2 = wingLift * 0.85f;
    int rx0 = bodyL - (int)(ws * 0.30f), ry0 = (int)(y - bh * 0.05f - lift0);
    int rx1 = bodyL - (int)(ws * 0.62f), ry1 = (int)(y - bh * 0.18f - lift1);
    int rx2 = bodyL - (int)(ws * 0.94f), ry2 = (int)(y - bh * 0.28f - lift2);
    GFX.fillEllipse(rx0, ry0, (int)(bw * 0.32f) + 1, (int)(bh * 0.42f) + 1, lightBright(pal.wingShade));
    GFX.fillEllipse(rx1, ry1, (int)(bw * 0.25f) + 1, (int)(bh * 0.31f) + 1, lightBright(pal.wing));
    GFX.fillEllipse(rx2, ry2, (int)(bw * 0.17f) + 1, (int)(bh * 0.21f) + 1, lightBright(pal.wingShade));
  }
  {   // 右羽（手前側。標準色でやや大きい）
    float ws = wingSpan;
    float lift0 = wingLift * 0.25f, lift1 = wingLift * 0.65f, lift2 = wingLift * 1.0f;
    int rx0 = bodyR + (int)(ws * 0.30f), ry0 = (int)(y - bh * 0.05f - lift0);
    int rx1 = bodyR + (int)(ws * 0.62f), ry1 = (int)(y - bh * 0.18f - lift1);
    int rx2 = bodyR + (int)(ws * 0.94f), ry2 = (int)(y - bh * 0.28f - lift2);
    GFX.fillEllipse(rx0, ry0, (int)(bw * 0.36f) + 1, (int)(bh * 0.46f) + 1, lightBright(pal.wing));
    GFX.fillEllipse(rx1, ry1, (int)(bw * 0.28f) + 1, (int)(bh * 0.34f) + 1, lightBright(pal.wingShade));
    GFX.fillEllipse(rx2, ry2, (int)(bw * 0.19f) + 1, (int)(bh * 0.23f) + 1, lightBright(pal.wing));
  }

  // ── ウサ耳（2026-08-09追加、2026-08-10位置を再修正、2026-08-10色味を再調整。
  //     位置・形・サイズは変更しない。実機確認で「背面にある耳が正面の白い画面と
  //     ほぼ同じ明るさで奥行き感が弱い」との指摘を受け、耳の主色を本体正面シェル
  //     （pal.body）より1段暗い pal.bodyShade（本体の側面・底面と同じ色。真っ黒には
  //     しない）へ変更し、影の縞にはさらに濃い pal.bezel を使うことで「背面側にある
  //     ため少し影になって見える」奥行きを強調した。中景・前景の先端には pal.body の
  //     ごく軽いハイライトを1点だけ残している。デザイン・位置・サイズは変更していない）──
  {
    int cxBack   = (bodyL + bodyR) / 2 + depth;   // 側面・底面と同じ奥行き分だけ後方へずらした中心
    int backTopY = bodyT - depth;                 // 本体上面の奥側（背面）の高さ
    if (flypLayer[i] == 0) {
      // 遠景：単純なシルエットのみ（軽量・簡略）。正面シェルより1段暗い色で奥行きを示す
      int earW    = (int)(bw * 0.12f) + 1;
      // 2026-08-10: 実機確認で耳が長すぎるとの指摘を受け、根元位置(earBaseY)は
      // 変えず、高さのみ従来のおよそ2/3へ短縮した（先端側だけが根元に近づく）。
      int earHFull = (int)(bh * 0.55f) + 1;
      int earH = (int)(earHFull * 0.667f);
      if (earH < 1) earH = 1;
      int earGapX = (int)(bw * 0.18f) + 1;
      int earBaseY = backTopY + 1;
      lightFillRect(cxBack - earGapX - earW / 2, earBaseY - earH, earW, earH, pal.bodyShade);
      lightFillRect(cxBack + earGapX - earW / 2, earBaseY - earH, earW, earH, pal.bodyShade);
    } else {
      // 中景・前景：先端に丸みを付け、片側に濃いめの影、反対側にごく軽いハイライトを入れる
      int earW    = (int)(bw * 0.15f) + 1;
      // 2026-08-10: 実機確認で耳が長すぎるとの指摘を受け、根元位置(earBaseY)は
      // 変えず、高さのみ従来のおよそ2/3へ短縮した（先端側だけが根元に近づく）。
      int earHFull = (int)(bh * 0.8f) + 2;
      int earH = (int)(earHFull * 0.667f);
      if (earH < 1) earH = 1;
      int earGapX = (int)(bw * 0.20f) + 1;
      int earBaseY = backTopY + 1;
      int earTipY  = earBaseY - earH;
      int shadeW   = 1;
      for (int s = -1; s <= 1; s += 2) {
        int ex = cxBack + s * earGapX;
        lightFillRect(ex - earW / 2, earTipY, earW, earH, pal.bodyShade);
        GFX.fillCircle(ex, earTipY, earW / 2, lightBright(pal.bodyShade));
        lightFillRect(ex + s * (earW / 2 - shadeW), earTipY, shadeW, earH, pal.bezel);
        GFX.fillCircle(ex - s * (earW / 2 - shadeW), earTipY, 1, lightBright(pal.body));
      }
    }
  }

  // ── 本体の側面・底面（厚みを表現する陰影パーツ。正面より先に描く）──
  GFX.fillTriangle(bodyL, bodyB, bodyR, bodyB, bodyR + depth, bodyB - depth, lightBright(pal.bodyShade));
  GFX.fillTriangle(bodyL, bodyB, bodyR + depth, bodyB - depth, bodyL + depth, bodyB - depth, lightBright(pal.bodyShade));
  GFX.fillTriangle(bodyR, bodyT, bodyR, bodyB, bodyR + depth, bodyB - depth, lightBright(pal.bezel));
  GFX.fillTriangle(bodyR, bodyT, bodyR + depth, bodyB - depth, bodyR + depth, bodyT - depth, lightBright(pal.bezel));

  // ── 本体正面（CoreS3を連想させる小型直方体のシェル。上端・左端にエッジハイライト）──
  // 耳の根元はこのfillRectより後方（画面奥）にあるため、ここで正面シェルを描くことで
  // 耳の根元付近が自然に隠れ、耳が本体背面から生えているように見える。
  GFX.fillRect(bodyL, bodyT, bw, bh, lightBright(pal.body));
  lightDrawLine(bodyL, bodyT, bodyR, bodyT, WHITE);
  lightDrawLine(bodyL, bodyT, bodyL, bodyB, WHITE);

  // ── 正面ディスプレイ（ベゼル＋白画面＋KariPom本来の顔）──
  // 2026-08-09改訂：実機評価で「独自の表情ではなく既存KariPomの顔にしてほしい」との
  // 指摘を受け、黒画面＋白い目という独自デザインをやめ、白画面＋黒い目2つ・鼻・
  // 鼻から口への縦線・口という、既存KariPomの基本的な顔構成をそのまま小型画面向けに
  // 簡略化して再現した（表情バリエーション・まばたきは持たせない）。
  int dispW = (int)(bw * 0.66f), dispH = (int)(bh * 0.66f);
  int dispX = (bodyL + bodyR) / 2 - dispW / 2;
  int dispY = (bodyT + bodyB) / 2 - dispH / 2;
  GFX.fillRect(dispX - 1, dispY - 1, dispW + 2, dispH + 2, lightBright(pal.bezel));
  GFX.fillRect(dispX, dispY, dispW, dispH, lightBright(WHITE));

  // 2026-08-09改訂：実機評価で「目と鼻が大きく、顔パーツが中央に密集して見える」との
  // 指摘を受け、目・鼻をおおむね半分の大きさへ縮小した。単純に顔全体を中央へ縮小
  // するのではなく、白画面の余白を活かして目の間隔（eyeDx）は広めに保ち、各パーツの
  // 縦の間隔も調整して、小さな目・鼻・縦線・口が自然に配置されるようにしている。
  int eyeCx = dispX + dispW / 2;
  int eyeCy = dispY + (int)(dispH * 0.32f);
  int eyeDx = (int)(dispW * 0.30f) + 1;
  int eyeR  = (int)(dispH * 0.075f) + 1;
  GFX.fillCircle(eyeCx - eyeDx, eyeCy, eyeR, lightBright(BLACK));
  GFX.fillCircle(eyeCx + eyeDx, eyeCy, eyeR, lightBright(BLACK));

  // 遠景の小さな個体は「白い画面＋黒い目」だけで十分認識できるため、鼻・鼻口線・口は
  // 潰れやすい遠景（layer==0）では省略し、中景・前景（layer>=1）でのみ描き足す。
  if (flypLayer[i] >= 1) {
    int noseCy  = eyeCy + (int)(dispH * 0.24f);
    int noseRW  = (int)(dispW * 0.055f) + 1, noseRH = (int)(dispH * 0.05f) + 1;
    GFX.fillEllipse(eyeCx, noseCy, noseRW, noseRH, lightBright(BLACK));
    int lineBotY = noseCy + noseRH + (int)(dispH * 0.16f);
    lightDrawLine(eyeCx, noseCy + noseRH, eyeCx, lineBotY, BLACK);
    int mouthHalfW = (int)(dispW * 0.13f) + 1;
    int mouthDropY = (int)(dispH * 0.09f) + 1;
    lightDrawLine(eyeCx, lineBotY, eyeCx - mouthHalfW, lineBotY + mouthDropY, BLACK);
    lightDrawLine(eyeCx, lineBotY, eyeCx + mouthHalfW, lineBotY + mouthDropY, BLACK);
  }

  // ── 小さなポート／インジケーター（電子機器らしいディテール）──
  lightFillRect(bodyL + 2, bodyB - 3, (int)(bw * 0.18f) + 1, 2, flypRgb(120, 200, 140));
  GFX.fillCircle(bodyR - 3, bodyT + 3, 1, lightBright(flypRgb(230, 210, 140)));
}

static void flypUpdateSlot(int i, unsigned long now, float dt) {
  if (!flypActive[i]) {
    if ((long)(now - flypNextSpawnAt[i]) >= 0) flypSpawn(i);
    return;
  }
  flypX[i] += flypVX[i] * dt * 60.0f;
  flypY[i] += flypVY[i] * dt * 60.0f;
  flypWingPhase[i] += dt * (3.6f + (float)flypLayer[i] * 0.8f);   // 前景ほど羽ばたきも心持ち速く

  // 右上→左下へ飛ぶため、画面左または画面下へ抜けたら非アクティブ化する。
  float margin = 34.0f * flypScale[i];
  bool offScreen = (flypX[i] < -margin) || (flypY[i] > 240.0f + margin);
  if (offScreen) {
    flypActive[i] = false;
    // 一定間隔ではなくランダムな待ち時間の後に再出現させる
    flypNextSpawnAt[i] = now + 300UL + (unsigned long)(flypRand01() * 2600.0f);
  }
}

void lightRenderFlyingPompadour(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!flypReady) buildFlyingPompadourTable();

  static unsigned long flypPrevMs = 0;
  unsigned long now = millis();
  if (needsInit || flypPrevMs == 0) flypPrevMs = now;
  float dt = (now - flypPrevMs) / 1000.0f;
  if (dt > 0.5f) dt = 0.5f;
  flypPrevMs = now;

  GFX.setClipRect(0, FLYP_TOP, 320, 240 - FLYP_TOP);

  flypDrawBackground(now);

  // 状態更新は1回だけ行い、描画だけを遠景→中景→前景の順にして奥行きの重なりを自然にする。
  for (int i = 0; i < FLYP_SLOT_COUNT; i++) flypUpdateSlot(i, now, dt);
  for (int pass = 0; pass < FLYP_LAYER_COUNT; pass++) {
    for (int i = 0; i < FLYP_SLOT_COUNT; i++) {
      if (flypActive[i] && (int)flypLayer[i] == pass) flypDrawFlyer(i);
    }
  }

  GFX.clearClipRect();   // 顔描画・他の描画に影響しないよう必ず解除する
}

// ============================================================================
// Lighting #19 : Rainbow Washing Machine（v1.1 / 2026-08-10）
//
// ■ コンセプト（2026-08-10 実機評価を反映し全面改訂）
//   v1.0は「固定形状のリングがその場で回転するだけ」で奥行きが無く、実機評価で
//   トランス感・奥行き感が乏しいとの指摘を受けた。v1.1では、色片そのものが
//   中心付近の小さな点として生まれ、外側へ向かって実際に移動しながら
//   だんだん大きくなり、最外周で消えてまた中心付近から生まれ直す──という
//   循環運動を主役にし、「中央の消失点から色のトンネルがこちらへ迫ってくる」
//   奥行きを作っている（詳細は buildRainbowWashingMachineTable() /
//   lightRenderRainbowWashingMachine() 内のコメント参照）。
//
//   さらに、半径によって回転方向・速度が変わる「帯（RWM_BAND_*）」を設け、
//   内側から外側へ「時計回り→反時計回り→時計回り→…」と隣接する帯が
//   互いに逆方向へ滑るようにしてある。単一の剛体として同じ角速度で回る
//   Hypnotic Vortexとは、色片の数・色の割り当て・中心→外周への移動・
//   帯ごとの逆回転のいずれの点でも明確に別物になっている。
//
//   また、v1.0にあった「加速→高速巡航→減速→一時停止→（時々）反転」という
//   洗濯機の脱水サイクル的な速度変化・停止フェーズは、実機評価で「回転が
//   主役に見えすぎる」との指摘を受けて廃止した。回転は帯ごとに常時継続し、
//   停止・完全な逆転サイクルは行わない（トランス感を優先）。
//
//   既存 Psychedelic / Trance（LIGHT_PSYCHE）とも別物：Psychedelicは
//   「輪・螺旋・モアレ…」といった複数の視覚モチーフを1〜2秒ごとに丸ごと
//   切り替え、フラッシュ・顔差し込みなど「予測不能な切替」が主役のモンタージュ
//   型。本Lightingはモチーフの切替や静止フラッシュを行わず、単一の連続した
//   「中心から湧き出て外へ流れるトンネル」を持続させる。
//
// ■ 負荷対策
//   色片は固定数（RWM_COUNT個）のプールを使い回す（生成・消滅のたびに
//   new/deleteやmalloc/freeは行わない）。毎フレームは各色片について
//   「現在の半径から属する帯を求める→帯の角速度で角度を進める→半径に比例した
//   速度で外側へ進める→最外周を超えたら中心付近の値へ書き戻す」という
//   O(1)の更新と、頂点3点ぶんのcosf/sinfのみで、画面全ピクセルに対する
//   atan2等の重い処理は行わない。
// ============================================================================
#define RWM_TOP    SCENE_TOP
#define RWM_CX     160
#define RWM_CY     ((RWM_TOP + 240) / 2)
#define RWM_MAX_R  200.0f   // この半径で画面四隅まで完全に覆う（トンネルの最遠端）
#define RWM_COUNT  510      // 常時プールしておく色片の総数（2026-08-10: 実機評価で密度不足の
                             // 指摘を受け170→510へ変更。アニメーションロジックは無変更）

// 9色・高彩度パレット（赤・橙・黄・黄緑・緑・シアン・青・紫・マゼンタ）。
// このLightingは終始このパレットのみを使い、パステル寄りの色は混ぜない。
static const uint16_t RWM_PALETTE[9] = {
  0xF800, 0xFD20, 0xFFE0, 0xAFE5, 0x07E0, 0x07FF, 0x001F, 0x780F, 0xF81F,
};
#define RWM_PALETTE_COUNT 9

// 半径を RWM_BAND_COUNT個の帯に分け、帯ごとに異なる回転方向・速度を与える。
// 符号を隣り合う帯どうしで交互にすることで「内側:時計回り／中間:反時計回り／
// 外側:時計回り…」のように半径方向へ逆回転する層が並ぶ（画面座標はy下方向が
// 正のため、cosf/sinfで角度を増やす方向＝符号+が時計回りになる）。速度の大きさ
// も帯ごとに変えて「適度な速度差」を出している。
#define RWM_BAND_COUNT 5
static const float RWM_BAND_ANGVEL[RWM_BAND_COUNT] = { 1.05f, -1.55f, 0.80f, -1.35f, 1.15f };
static const float RWM_BAND_WOBBLE_FREQ[RWM_BAND_COUNT] = { 0.35f, 0.50f, 0.28f, 0.60f, 0.40f };
#define RWM_BAND_WOBBLE_AMP 0.15f   // 速度がゆっくり揺らぐ幅（符号は反転させない＝停止・逆転はしない）

// 半径の成長：中心付近はゆっくり・外側ほど速く広がる（半径に比例する成長速度＋
// 常に一定以上の最低速度）ことで、「消失点から迫ってくる」加速感を出す。
#define RWM_GROWTH_RATE     0.50f  // 半径に比例した成長速度（1/秒）
#define RWM_GROWTH_MIN      5.0f   // 中心付近でも動きが止まって見えないための最低速度(px/秒)
#define RWM_RESPAWN_R       2.0f   // 再出現時の半径（中心付近）
#define RWM_RESPAWN_JITTER  6.0f   // 再出現位置の半径ジッター（同時に生まれた点が重ならないように）

struct RwmShard {
  float   r;            // 中心からの現在の距離（半径）。時間とともに増加し続ける
  float   ang;           // 現在の角度（rad）。半径が属する帯の角速度で回り続ける
  float   angWidthL;     // 外側左頂点の角度オフセット幅
  float   angWidthR;     // 外側右頂点の角度オフセット幅
  float   apexAngOff;    // 中心側頂点の角度オフセット（三角形を歪ませ不揃いに見せる）
  float   growthMul;     // この色片固有の成長速度倍率（個体差を出す）
  uint8_t colorIdx;
};

// Arduino IDEの自動プロトタイプ生成（ctags）は、struct RwmShard の定義より前の
// 位置へ rwmSpawnShard() のプロトタイプを機械的に挿入してしまい、その時点では
// RwmShard がまだ未定義のため "'RwmShard' was not declared in this scope" 等で
// ビルド失敗する（Fighter Duel の sfDrawFighter(const SFFighter&, ...) と同じ
// 既知の問題。詳細はそちらの前方宣言コメントを参照）。
// ここで手動プロトタイプを明示することで自動生成対象から外し、回避する。
static void rwmSpawnShard(RwmShard& s, float rStart);

static RwmShard rwmShard[RWM_COUNT];
static bool     rwmReady = false;

// 乱数：このLighting専用の小さなLCG（既存の他Lighting同様、共通化はしない）
static uint32_t rwmLcg = 1;
static inline uint32_t rwmRand() {
  rwmLcg ^= rwmLcg << 13; rwmLcg ^= rwmLcg >> 17; rwmLcg ^= rwmLcg << 5; return rwmLcg;
}
static inline float rwmRand01() { return (float)(rwmRand() & 0xFFFF) / 65535.0f; }

// 色片1個を「中心付近の新しい点」として（再）生成する。形・色・成長速度の
// 個体差はここで毎回ランダムに決め直すため、同じ場所から生まれ直しても
// 見た目が単調にならない。
static void rwmSpawnShard(RwmShard& s, float rStart) {
  s.r   = rStart;
  s.ang = rwmRand01() * 6.2831853f;
  float halfW  = 0.045f + rwmRand01() * 0.10f;  // 角度幅は概ね一定（rad）。絶対幅は半径とともに自動的に広がる
  s.angWidthL  = halfW * (0.7f + rwmRand01() * 0.6f);
  s.angWidthR  = halfW * (0.7f + rwmRand01() * 0.6f);
  s.apexAngOff = (rwmRand01() - 0.5f) * halfW * 0.6f;
  s.growthMul  = 0.82f + rwmRand01() * 0.36f;
  s.colorIdx   = (uint8_t)(rwmRand() % RWM_PALETTE_COUNT);
}

void buildRainbowWashingMachineTable() {
  rwmLcg = 20260810u;
  for (uint16_t i = 0; i < RWM_COUNT; i++) {
    // 起動直後から中心〜最外周までトンネルの奥行き全体に色片が満ちて見えるよう、
    // 初期半径だけは均等にばらまいておく（以後の再出現は必ず中心付近から、
    // という循環ルールは rwmSpawnShard() 側で保証する）。
    float rStart = ((float)i / (float)RWM_COUNT) * RWM_MAX_R;
    rwmSpawnShard(rwmShard[i], rStart);
  }
  rwmReady = true;
}

void lightRenderRainbowWashingMachine(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  if (!rwmReady) buildRainbowWashingMachineTable();

  static unsigned long rwmPrevMs   = 0;
  static float         rwmTimeSec  = 0.0f;
  unsigned long now = millis();
  if (needsInit || rwmPrevMs == 0) { rwmPrevMs = now; rwmTimeSec = 0.0f; }
  float dt = (now - rwmPrevMs) / 1000.0f;
  if (dt > 0.5f) dt = 0.5f;
  rwmPrevMs = now;
  rwmTimeSec += dt;

  GFX.setClipRect(0, RWM_TOP, 320, 240 - RWM_TOP);
  lightFillRect(0, RWM_TOP, 320, 240 - RWM_TOP, BLACK);

  for (uint16_t i = 0; i < RWM_COUNT; i++) {
    RwmShard& s = rwmShard[i];

    // ① 半径からこの色片が属する帯を求め、帯ごとの角速度（符号が違えば逆回転）
    //    で角度を進める。速度にはゆっくりした揺らぎを重ねるが符号は変えない
    //    ＝回転が止まったり反転したりはしない（トランス感を優先）。
    int band = (int)(s.r / RWM_MAX_R * (float)RWM_BAND_COUNT);
    if (band < 0) band = 0;
    if (band >= RWM_BAND_COUNT) band = RWM_BAND_COUNT - 1;
    float wobble = 1.0f + RWM_BAND_WOBBLE_AMP
                 * sinf(rwmTimeSec * RWM_BAND_WOBBLE_FREQ[band] + (float)band * 1.7f);
    s.ang += RWM_BAND_ANGVEL[band] * wobble * dt;

    // ② 半径そのものを時間とともに増やし、色片を中心から外側へ実際に移動させる。
    //    半径に比例した速度＋最低速度により、中心付近はゆっくり・外側ほど速く
    //    広がる（消失点から迫ってくるような加速感）。
    s.r += (RWM_GROWTH_RATE * s.r + RWM_GROWTH_MIN) * s.growthMul * dt;

    // ③ 最外周を超えたら消し、中心付近から新しい色片として生まれ直させる
    //    （この循環を止めずに連続させることで「トンネル」に見せる）。
    if (s.r > RWM_MAX_R + 24.0f) {
      rwmSpawnShard(s, RWM_RESPAWN_R + rwmRand01() * RWM_RESPAWN_JITTER);
      continue;   // 生まれ直した直後の1フレームは点として小さすぎるため描画を省略
    }

    // 半径が大きいほど色片自体も太く・大きく見えるようにする（外側ほど大きい構成）。
    float depthHalf = 3.0f + s.r * 0.09f;
    float apexR = s.r - depthHalf; if (apexR < 0.0f) apexR = 0.0f;
    float baseR = s.r + depthHalf;

    float apexAng = s.ang + s.apexAngOff;
    float ax = RWM_CX + cosf(apexAng) * apexR;
    float ay = RWM_CY + sinf(apexAng) * apexR;
    float lx = RWM_CX + cosf(s.ang - s.angWidthL) * baseR;
    float ly = RWM_CY + sinf(s.ang - s.angWidthL) * baseR;
    float rx = RWM_CX + cosf(s.ang + s.angWidthR) * baseR;
    float ry = RWM_CY + sinf(s.ang + s.angWidthR) * baseR;
    uint16_t col = lightBright(RWM_PALETTE[s.colorIdx]);
    GFX.fillTriangle((int)ax, (int)ay, (int)lx, (int)ly, (int)rx, (int)ry, col);
  }

  // 中心に小さな光点を置き、トンネルの奥（消失点）をはっきり感じさせる。
  GFX.fillCircle(RWM_CX, RWM_CY, 2, lightBright(WHITE));

  GFX.clearClipRect();
}

// ============================================================================
// Lighting #20 : Pixel Invasion（v1.0 / 2026-08-10）
//
// ■ コンセプト
//   1970年代末〜1980年代初頭のカラー化された固定画面シューティングゲーム
//   （特にSpace Invaders Part IIの雰囲気）から着想を得た、レトロアーケード風の
//   自動アニメーションLightingです。実在ゲームのスプライトデータ・マップ・
//   画像は一切使用せず、敵編隊・自機・UFOはすべて本ブロック内で新規に
//   デザインしたオリジナルのドット絵（8x8等の小さな2値ビットマップ）です。
//   背景は要件どおり完全な黒一色のみで、星・瞬き・流れ星・パーティクル・
//   背景スクロールの類は一切描かない（Asteroid Field/Tempest Tunnel/Missile
//   Defenseに追加したカラフルな1ドット星とは意図的に切り離してあり、本
//   Lightingには一切持ち込んでいない）。
//
// ■ Arduino IDE 自動プロトタイプ生成対策（設計方針）
//   Rainbow Washing Machine追加時に、ユーザー定義structを引数に取る関数で
//   ctagsの自動プロトタイプ生成がstruct定義より前へプロトタイプを挿入して
//   しまい、ビルドエラーになる問題が発生した（対策はrwmSpawnShard()直上の
//   コメント、および本ファイル冒頭のPsyPal・17444行目付近のSFFighterの
//   コメントを参照）。本Lightingでは同じ問題を最初から避けるため、
//   【カスタムstruct/classを一切定義しない】設計にしてある。敵編隊・弾・
//   シールドはすべてグローバルなプリミティブ型の配列（bool/uint8_t/float/
//   uint16_t/unsigned long）で保持し、操作関数はすべて添字(int)や座標(float)
//   などのプリミティブ引数のみを取る（Missile DefenseのmSpawnEnemy(uint8_t i)
//   と同じ流儀）。これにより自動生成プロトタイプが未知の型を参照すること
//   自体が起こり得ない。
//
// ■ 構成
//   ・敵編隊：8列×5段。1段目＝マゼンタ、2〜3段目＝ターコイズ、4〜5段目＝
//     グリーンの3色に塗り分け、段のグループごとに異なる自作ドット絵（各2
//     フレームの脚／触手アニメーション）を割り当てている。編隊全体は一定
//     ピクセル単位でしか移動しない「ステップ移動」で、画面端付近に達すると
//     反転して少し下降する。
//   ・自機：画面下部をステップ移動で自動的に左右往復し、一定間隔でランダム
//     性を持たせつつ弾を発射する。
//   ・シールド：4個。赤いドット絵の防壁で、弾が当たったセルだけが個別に
//     消えていき、徐々に崩れて見える。編隊リセット時に元の形へ復元する。
//   ・弾：自機弾（水色系・上方向）、敵弾（赤系・下方向）。同時数を少数
//     （自機1発・敵最大3発）に制限し、昔の固定画面シューティングらしい
//     簡潔な画面を維持する。
//   ・UFO：ランダムな間隔（約9〜21秒）で出現し、画面最上部付近を左右
//     どちらかへ横切って自然に消える。
//   ・永久ループ：敵が少数まで減る／編隊が下降しすぎる／一定時間経過、の
//     いずれかで新しい5段編隊とシールドへ自然に切り替わる。スコア・残機・
//     GAME OVER・ユーザー操作は一切無く、アニメーションは無限に継続する。
//
// ■ 描画方針
//   すべて lightFillRect（Framework共通Brightness適用済み）による矩形の
//   ベタ塗りのみで構成し、アンチエイリアス・グラデーション・発光・
//   パーティクル・大量の爆発演出は使わない（要件どおり）。撃墜演出も
//   数フレームの白いピクセル的な点滅のみ。
// ============================================================================
#define PIX_TOP           SCENE_TOP
#define PIX_COLS          8
#define PIX_ROWS          5
#define PIX_INV_CELL      2      // 敵1体＝8x8ドット絵 × 2px = 16x16画面px
#define PIX_INV_HALF      8
#define PIX_FORM_LEFT_X0  32
#define PIX_FORM_COL_DX   34
#define PIX_FORM_TOP_Y    (PIX_TOP + 4)
#define PIX_FORM_ROW_DY   20
#define PIX_STEP_PX       6      // 1ステップあたりの水平移動量（「カッ、カッ、カッ」の刻み）
#define PIX_DROP_PX       4      // 反転時に下降する量（下降できる余地を確保し、1ラウンドが
                                  // 数秒で終わってしまわないようにする）
#define PIX_FORM_MARGIN   16     // 編隊が反転する画面端からの余白
#define PIX_RESET_ALIVE_MIN 3    // 生存数がこれ以下になったら新しい編隊へ
#define PIX_ROUND_MAX_MS  70000UL // 安全策：この時間が経過したら強制的に新しい編隊へ

#define PIX_SHIELD_COUNT  4
#define PIX_SHIELD_ROWS   6
#define PIX_SHIELD_COLS   10
#define PIX_SHIELD_CELL   3
#define PIX_SHIELD_Y      178

#define PIX_PLAYER_Y        224
#define PIX_PLAYER_CELL     2
#define PIX_PLAYER_MARGIN   16
#define PIX_PLAYER_STEP_MS  40UL
#define PIX_PLAYER_STEP_PX  2.0f

#define PIX_UFO_CELL        2
#define PIX_UFO_Y           (PIX_TOP + 12)
#define PIX_UFO_SPEED       55.0f
#define PIX_UFO_MIN_GAP_MS  9000UL
#define PIX_UFO_GAP_JITTER_MS 12000UL

#define PIX_PBULLET_MAX     1
#define PIX_EBULLET_MAX     3
#define PIX_PBULLET_SPEED   170.0f
#define PIX_EBULLET_SPEED   115.0f
#define PIX_PLAYER_FIRE_MIN_MS  1200UL
#define PIX_PLAYER_FIRE_JITTER_MS 1300UL
#define PIX_ENEMY_FIRE_MIN_MS   500UL
#define PIX_ENEMY_FIRE_JITTER_MS 900UL
#define PIX_KILL_FLASH_MS   150UL

// 黒背景・マゼンタ／紫・ターコイズ・グリーン・自機水色・敵弾赤・シールド赤・UFO橙黄
static const uint16_t PIX_COL_MAGENTA   = (uint16_t)(((230 & 0xF8) << 8) | ((40  & 0xFC) << 3) | (220 >> 3));
static const uint16_t PIX_COL_TURQUOISE = (uint16_t)((( 40 & 0xF8) << 8) | ((220 & 0xFC) << 3) | (210 >> 3));
static const uint16_t PIX_COL_GREEN     = (uint16_t)((( 60 & 0xF8) << 8) | ((220 & 0xFC) << 3) | (90  >> 3));
static const uint16_t PIX_COL_PLAYER    = (uint16_t)((( 80 & 0xF8) << 8) | ((200 & 0xFC) << 3) | (255 >> 3));
static const uint16_t PIX_COL_PBULLET   = (uint16_t)(((160 & 0xF8) << 8) | ((235 & 0xFC) << 3) | (255 >> 3));
static const uint16_t PIX_COL_EBULLET   = (uint16_t)(((255 & 0xF8) << 8) | ((70  & 0xFC) << 3) | (70  >> 3));
static const uint16_t PIX_COL_SHIELD    = (uint16_t)(((220 & 0xF8) << 8) | ((30  & 0xFC) << 3) | (30  >> 3));
static const uint16_t PIX_COL_UFO       = (uint16_t)(((255 & 0xF8) << 8) | ((195 & 0xFC) << 3) | (40  >> 3));

// ── 敵編隊のドット絵（8x8、1バイト=1行。bit7が左端、bit0が右端）──────────
// 実在ゲームのスプライトは参照せず、本ファイル用に新規デザインした抽象的な
// 宇宙生物のシルエット。段グループごとに2フレーム（脚／触手のポーズ違い）。
static const uint8_t PIX_SPR_TOP[2][8] = {
  { 0x3C, 0x7E, 0xFF, 0xDB, 0xFF, 0x24, 0x42, 0x81 },
  { 0x3C, 0x7E, 0xFF, 0xDB, 0xFF, 0x5A, 0x24, 0x5A },
};
static const uint8_t PIX_SPR_MID[2][8] = {
  { 0x18, 0x3C, 0x7E, 0xFF, 0x66, 0xDB, 0x5A, 0xA5 },
  { 0x18, 0x3C, 0x7E, 0xFF, 0x66, 0xDB, 0xA5, 0x5A },
};
static const uint8_t PIX_SPR_BOT[2][8] = {
  { 0x5A, 0x3C, 0x7E, 0xDB, 0xFF, 0x42, 0xA5, 0x42 },
  { 0x5A, 0x3C, 0x7E, 0xDB, 0xFF, 0xA5, 0x42, 0xA5 },
};
// 自機（8x6、水色／ターコイズ系。オリジナルの小さな砲台シルエット）
static const uint8_t PIX_SPR_PLAYER[6] = { 0x18, 0x3C, 0x7E, 0xFF, 0xDB, 0xA5 };
// UFO（8x5、既存ゲームのスプライトを参照しないオリジナルの円盤シルエット）
static const uint8_t PIX_SPR_UFO[5] = { 0x3C, 0x7E, 0xFF, 0x5A, 0x24 };
// シールドの初期形状（6行×10列、ドーム状＋下部にアーチの切り欠き）
static const uint16_t PIX_SHIELD_BASE[PIX_SHIELD_ROWS] = { 0x0FC, 0x1FE, 0x3FF, 0x3FF, 0x387, 0x303 };

// ── 状態（すべてプリミティブ型のグローバル配列。カスタムstruct/classは使わない）──
static bool          pixReady = false;
static bool          pixAlive[PIX_ROWS][PIX_COLS];
static unsigned long pixKillFlashUntil[PIX_ROWS][PIX_COLS];
static int           pixFormOffsetX = 0, pixFormOffsetY = 0;
static int8_t         pixFormDir = 1;
static uint8_t        pixAnimFrame = 0;
static unsigned long pixNextStepAt = 0;
static unsigned long pixRoundStartMs = 0;

static uint16_t       pixShieldRowMask[PIX_SHIELD_COUNT][PIX_SHIELD_ROWS];

static float          pixPlayerX = 160.0f;
static int8_t         pixPlayerDir = 1;
static unsigned long pixPlayerNextStepAt = 0;
static unsigned long pixPlayerNextFireAt = 0;

static bool           pixUfoActive = false;
static float          pixUfoX = 0.0f;
static int8_t          pixUfoDir = 1;
static unsigned long  pixUfoNextAt = 0;

static bool            pixPBulletActive[PIX_PBULLET_MAX];
static float           pixPBulletX[PIX_PBULLET_MAX], pixPBulletY[PIX_PBULLET_MAX];
static bool            pixEBulletActive[PIX_EBULLET_MAX];
static float           pixEBulletX[PIX_EBULLET_MAX], pixEBulletY[PIX_EBULLET_MAX];
static unsigned long  pixNextEnemyFireAt = 0;

// 乱数：このLighting専用の小さなLCG（既存の他Lighting同様、共通化はしない）
static uint32_t pixRng = 20260810u;
static inline uint32_t pixRand() {
  pixRng ^= pixRng << 13; pixRng ^= pixRng >> 17; pixRng ^= pixRng << 5; return pixRng;
}
static inline float pixRand01() { return (float)(pixRand() & 0xFFFF) / 65535.0f; }

static inline int pixInvaderX(int c) { return PIX_FORM_LEFT_X0 + c * PIX_FORM_COL_DX + pixFormOffsetX; }
static inline int pixInvaderY(int r) { return PIX_FORM_TOP_Y   + r * PIX_FORM_ROW_DY  + pixFormOffsetY; }
static inline uint16_t pixRowColor(int r) {
  if (r == 0) return PIX_COL_MAGENTA;
  if (r <= 2) return PIX_COL_TURQUOISE;
  return PIX_COL_GREEN;
}
static inline const uint8_t* pixRowSprite(int r, int frame) {
  if (r == 0) return PIX_SPR_TOP[frame];
  if (r <= 2) return PIX_SPR_MID[frame];
  return PIX_SPR_BOT[frame];
}
static inline int pixShieldX(int i) { return 40 + i * 70; }

static int pixAliveCount() {
  int n = 0;
  for (int r = 0; r < PIX_ROWS; r++)
    for (int c = 0; c < PIX_COLS; c++)
      if (pixAlive[r][c]) n++;
  return n;
}

// 8列幅のドット絵を、1論理ピクセル=px画面pxのベタ塗り矩形として描く。
// rows/rowCountは「上から何行あるか」だけを表す単純なポインタ渡しのため、
// 自動プロトタイプ生成の対象になっても引数はすべて既知の組み込み型のみ。
static void pixDrawSprite(int cx, int cy, const uint8_t* rows, int rowCount, uint16_t color, int px) {
  int w = 8 * px, h = rowCount * px;
  int x0 = cx - w / 2, y0 = cy - h / 2;
  for (int r = 0; r < rowCount; r++) {
    uint8_t bits = rows[r];
    for (int c = 0; c < 8; c++) {
      if (bits & (0x80 >> c)) {
        lightFillRect(x0 + c * px, y0 + r * px, px, px, color);
      }
    }
  }
}

static void pixResetShields() {
  for (int s = 0; s < PIX_SHIELD_COUNT; s++)
    for (int r = 0; r < PIX_SHIELD_ROWS; r++)
      pixShieldRowMask[s][r] = PIX_SHIELD_BASE[r];
}

static void pixResetFormation() {
  for (int r = 0; r < PIX_ROWS; r++) {
    for (int c = 0; c < PIX_COLS; c++) {
      pixAlive[r][c] = true;
      pixKillFlashUntil[r][c] = 0;
    }
  }
  pixFormOffsetX = 0;
  pixFormOffsetY = 0;
  pixFormDir = 1;
  pixAnimFrame = 0;
}

static void pixResetRound(unsigned long now) {
  pixResetFormation();
  pixResetShields();
  for (int i = 0; i < PIX_PBULLET_MAX; i++) pixPBulletActive[i] = false;
  for (int i = 0; i < PIX_EBULLET_MAX; i++) pixEBulletActive[i] = false;
  pixRoundStartMs = now;
  pixNextStepAt = now + 200;
  pixNextEnemyFireAt = now + PIX_ENEMY_FIRE_MIN_MS + (unsigned long)(pixRand01() * PIX_ENEMY_FIRE_JITTER_MS);
}

static void pixInitAll(unsigned long now) {
  pixPlayerX = 160.0f;
  pixPlayerDir = 1;
  pixPlayerNextStepAt = now;
  pixPlayerNextFireAt = now + PIX_PLAYER_FIRE_MIN_MS;
  pixUfoActive = false;
  pixUfoNextAt = now + PIX_UFO_MIN_GAP_MS + (unsigned long)(pixRand01() * PIX_UFO_GAP_JITTER_MS);
  pixResetRound(now);
  pixReady = true;
}

// 編隊のステップ移動（一定ピクセルずつ進む「カッ、カッ、カッ」という刻み）。
// 端に達したら移動せず反転＋下降だけを行う（連続スクロールにしない＝要件どおり）。
static void pixStepFormation(unsigned long now) {
  if (now < pixNextStepAt) return;
  int aliveN = pixAliveCount();
  unsigned long stepMs = 180UL + (unsigned long)aliveN * 10UL;
  if (stepMs > 560UL) stepMs = 560UL;
  pixNextStepAt = now + stepMs;

  int newOffsetX = pixFormOffsetX + (int)pixFormDir * PIX_STEP_PX;
  int leftEdge  = PIX_FORM_LEFT_X0 + 0 * PIX_FORM_COL_DX + newOffsetX - PIX_INV_HALF;
  int rightEdge = PIX_FORM_LEFT_X0 + (PIX_COLS - 1) * PIX_FORM_COL_DX + newOffsetX + PIX_INV_HALF;
  if (leftEdge < PIX_FORM_MARGIN || rightEdge > SCENE_W - PIX_FORM_MARGIN) {
    pixFormDir = (int8_t)(-pixFormDir);
    pixFormOffsetY += PIX_DROP_PX;
  } else {
    pixFormOffsetX = newOffsetX;
  }
  pixAnimFrame ^= 1;
}

// 生存数が少ない／編隊が下降しすぎた／時間経過、のいずれかで新しい編隊へ。
static void pixCheckRoundReset(unsigned long now) {
  int aliveN = pixAliveCount();
  bool tooLow    = aliveN <= PIX_RESET_ALIVE_MIN;
  bool descended = pixInvaderY(PIX_ROWS - 1) >= (PIX_SHIELD_Y - 10);
  bool timedOut  = (now - pixRoundStartMs) >= PIX_ROUND_MAX_MS;
  if (tooLow || descended || timedOut) pixResetRound(now);
}

static void pixUpdatePlayer(unsigned long now) {
  if (now < pixPlayerNextStepAt) return;
  pixPlayerNextStepAt = now + PIX_PLAYER_STEP_MS;
  pixPlayerX += (float)pixPlayerDir * PIX_PLAYER_STEP_PX;
  if (pixPlayerX < PIX_PLAYER_MARGIN)              { pixPlayerX = PIX_PLAYER_MARGIN;              pixPlayerDir = 1;  }
  if (pixPlayerX > SCENE_W - PIX_PLAYER_MARGIN)    { pixPlayerX = SCENE_W - PIX_PLAYER_MARGIN;     pixPlayerDir = -1; }
}

static void pixUpdatePlayerFire(unsigned long now) {
  if (now < pixPlayerNextFireAt) return;
  pixPlayerNextFireAt = now + PIX_PLAYER_FIRE_MIN_MS + (unsigned long)(pixRand01() * PIX_PLAYER_FIRE_JITTER_MS);
  if (!pixPBulletActive[0]) {
    pixPBulletActive[0] = true;
    pixPBulletX[0] = pixPlayerX;
    pixPBulletY[0] = (float)PIX_PLAYER_Y - 10.0f;
  }
}

static void pixUpdateUfo(unsigned long now, float dt) {
  if (!pixUfoActive) {
    if (now >= pixUfoNextAt) {
      pixUfoActive = true;
      pixUfoDir = (pixRand() & 1u) ? 1 : -1;
      pixUfoX = (pixUfoDir > 0) ? -20.0f : (float)SCENE_W + 20.0f;
    }
    return;
  }
  pixUfoX += (float)pixUfoDir * PIX_UFO_SPEED * dt;
  if ((pixUfoDir > 0 && pixUfoX > (float)SCENE_W + 20.0f) ||
      (pixUfoDir < 0 && pixUfoX < -20.0f)) {
    pixUfoActive = false;
    pixUfoNextAt = now + PIX_UFO_MIN_GAP_MS + (unsigned long)(pixRand01() * PIX_UFO_GAP_JITTER_MS);
  }
}

// 生存している列からランダムに1つ選び、その列でもっとも手前（下側）の
// 敵から弾を発射する（古典的な固定画面シューティングの定石どおり）。
static void pixSpawnEnemyBullet(unsigned long now) {
  if (now < pixNextEnemyFireAt) return;
  pixNextEnemyFireAt = now + PIX_ENEMY_FIRE_MIN_MS + (unsigned long)(pixRand01() * PIX_ENEMY_FIRE_JITTER_MS);

  int slot = -1;
  for (int i = 0; i < PIX_EBULLET_MAX; i++) { if (!pixEBulletActive[i]) { slot = i; break; } }
  if (slot < 0) return;

  int aliveCols[PIX_COLS];
  int aliveColN = 0;
  for (int c = 0; c < PIX_COLS; c++) {
    bool any = false;
    for (int r = 0; r < PIX_ROWS; r++) if (pixAlive[r][c]) { any = true; break; }
    if (any) aliveCols[aliveColN++] = c;
  }
  if (aliveColN == 0) return;
  int c = aliveCols[pixRand() % (uint32_t)aliveColN];
  int shooterRow = -1;
  for (int r = PIX_ROWS - 1; r >= 0; r--) { if (pixAlive[r][c]) { shooterRow = r; break; } }
  if (shooterRow < 0) return;

  pixEBulletActive[slot] = true;
  pixEBulletX[slot] = (float)pixInvaderX(c);
  pixEBulletY[slot] = (float)pixInvaderY(shooterRow) + (float)PIX_INV_HALF;
}

// シールドの1セルに弾が当たったかを判定し、当たっていればそのセルだけを
// 消して true を返す（既に空のセルなら弾はそのまま素通りする＝false）。
static bool pixShieldHit(int s, float wx, float wy) {
  int localX = (int)(wx - (float)pixShieldX(s));
  int localY = (int)(wy - (float)PIX_SHIELD_Y);
  if (localX < 0 || localY < 0) return false;
  int c = localX / PIX_SHIELD_CELL;
  int r = localY / PIX_SHIELD_CELL;
  if (c < 0 || c >= PIX_SHIELD_COLS || r < 0 || r >= PIX_SHIELD_ROWS) return false;
  uint16_t bit = (uint16_t)(1u << c);
  if (!(pixShieldRowMask[s][r] & bit)) return false;
  pixShieldRowMask[s][r] &= (uint16_t)~bit;
  return true;
}

static void pixUpdateBullets(unsigned long now, float dt) {
  (void)now;
  // ── 自機弾：上へ進み、シールド／敵編隊との当たりを判定 ──
  for (int i = 0; i < PIX_PBULLET_MAX; i++) {
    if (!pixPBulletActive[i]) continue;
    pixPBulletY[i] -= PIX_PBULLET_SPEED * dt;
    if (pixPBulletY[i] < (float)PIX_TOP) { pixPBulletActive[i] = false; continue; }

    bool absorbed = false;
    for (int s = 0; s < PIX_SHIELD_COUNT && !absorbed; s++) {
      if (pixShieldHit(s, pixPBulletX[i], pixPBulletY[i])) absorbed = true;
    }
    if (absorbed) { pixPBulletActive[i] = false; continue; }

    for (int r = 0; r < PIX_ROWS && pixPBulletActive[i]; r++) {
      for (int c = 0; c < PIX_COLS && pixPBulletActive[i]; c++) {
        if (!pixAlive[r][c]) continue;
        int ix = pixInvaderX(c), iy = pixInvaderY(r);
        if (pixPBulletX[i] >= ix - PIX_INV_HALF && pixPBulletX[i] <= ix + PIX_INV_HALF &&
            pixPBulletY[i] >= iy - PIX_INV_HALF && pixPBulletY[i] <= iy + PIX_INV_HALF) {
          pixAlive[r][c] = false;
          pixKillFlashUntil[r][c] = now + PIX_KILL_FLASH_MS;
          pixPBulletActive[i] = false;
        }
      }
    }
  }

  // ── 敵弾：下へ進み、シールドとの当たりのみ判定（自機には当てない＝ゲームオーバー無し）──
  for (int i = 0; i < PIX_EBULLET_MAX; i++) {
    if (!pixEBulletActive[i]) continue;
    pixEBulletY[i] += PIX_EBULLET_SPEED * dt;
    if (pixEBulletY[i] > (float)PIX_PLAYER_Y - 4.0f) { pixEBulletActive[i] = false; continue; }

    for (int s = 0; s < PIX_SHIELD_COUNT; s++) {
      if (pixShieldHit(s, pixEBulletX[i], pixEBulletY[i])) { pixEBulletActive[i] = false; break; }
    }
  }
}

static void pixDrawShield(int s) {
  int baseX = pixShieldX(s);
  for (int r = 0; r < PIX_SHIELD_ROWS; r++) {
    uint16_t mask = pixShieldRowMask[s][r];
    for (int c = 0; c < PIX_SHIELD_COLS; c++) {
      if (mask & (uint16_t)(1u << c)) {
        lightFillRect(baseX + c * PIX_SHIELD_CELL, PIX_SHIELD_Y + r * PIX_SHIELD_CELL,
                      PIX_SHIELD_CELL, PIX_SHIELD_CELL, PIX_COL_SHIELD);
      }
    }
  }
}

static void pixDrawFormation(unsigned long now) {
  for (int r = 0; r < PIX_ROWS; r++) {
    for (int c = 0; c < PIX_COLS; c++) {
      int cx = pixInvaderX(c), cy = pixInvaderY(r);
      if (pixAlive[r][c]) {
        pixDrawSprite(cx, cy, pixRowSprite(r, pixAnimFrame), 8, pixRowColor(r), PIX_INV_CELL);
      } else if (now < pixKillFlashUntil[r][c]) {
        // ごく短い数フレームの撃墜フラッシュ（白いピクセルの点滅のみ。爆発演出は無し）
        lightFillRect(cx - PIX_INV_HALF, cy - PIX_INV_HALF, PIX_INV_HALF * 2, PIX_INV_HALF * 2, WHITE);
      }
    }
  }
}

static void pixDrawPlayer() {
  pixDrawSprite((int)pixPlayerX, PIX_PLAYER_Y, PIX_SPR_PLAYER, 6, PIX_COL_PLAYER, PIX_PLAYER_CELL);
}

static void pixDrawUfo() {
  if (!pixUfoActive) return;
  pixDrawSprite((int)pixUfoX, PIX_UFO_Y, PIX_SPR_UFO, 5, PIX_COL_UFO, PIX_UFO_CELL);
}

static void pixDrawBullets() {
  for (int i = 0; i < PIX_PBULLET_MAX; i++) {
    if (!pixPBulletActive[i]) continue;
    lightFillRect((int)pixPBulletX[i] - 1, (int)pixPBulletY[i] - 3, 2, 6, PIX_COL_PBULLET);
  }
  for (int i = 0; i < PIX_EBULLET_MAX; i++) {
    if (!pixEBulletActive[i]) continue;
    lightFillRect((int)pixEBulletX[i] - 1, (int)pixEBulletY[i] - 3, 2, 6, PIX_COL_EBULLET);
  }
}

void lightRenderPixelInvasion(bool needsInit, bool fullRepaint) {
  (void)fullRepaint;
  unsigned long now = millis();
  if (!pixReady) pixInitAll(now);

  static unsigned long pixPrevMs = 0;
  if (needsInit || pixPrevMs == 0) pixPrevMs = now;
  float dt = (now - pixPrevMs) / 1000.0f;
  if (dt > 0.5f) dt = 0.5f;
  pixPrevMs = now;

  pixStepFormation(now);
  pixUpdatePlayer(now);
  pixUpdatePlayerFire(now);
  pixUpdateUfo(now, dt);
  pixSpawnEnemyBullet(now);
  pixUpdateBullets(now, dt);
  pixCheckRoundReset(now);

  GFX.setClipRect(0, PIX_TOP, SCENE_W, SCENE_H - PIX_TOP);
  lightFillRect(0, PIX_TOP, SCENE_W, SCENE_H - PIX_TOP, BLACK);   // 完全な黒背景のみ（星・スクロール等は一切描かない）

  for (int s = 0; s < PIX_SHIELD_COUNT; s++) pixDrawShield(s);
  pixDrawFormation(now);
  pixDrawPlayer();
  pixDrawUfo();
  pixDrawBullets();

  GFX.clearClipRect();
}

// ============================================================================
// Lighting Manager（描画関数テーブル）
//
// enum LightingMode と同じ並び順。追加時はここへ関数を1行足すだけ。
// ============================================================================
typedef void (*LightRenderFn)(bool needsInit, bool fullRepaint);
const LightRenderFn LIGHT_RENDER_FN[LIGHT_MODE_COUNT] = {
  lightRenderDisco,   // LIGHT_DISCO
  lightRenderLaser,   // LIGHT_LASER
  lightRenderAurora,  // LIGHT_AURORA
  lightRenderMatrix,  // LIGHT_MATRIX
  lightRenderRace,    // LIGHT_RACE
  lightRenderSkyRaid, // LIGHT_SKYRAID
  lightRenderEyeSlot, // LIGHT_EYESLOT
  lightRenderClassicRace, // LIGHT_CLASSICRACE
  lightRenderAsteroid,    // LIGHT_ASTEROID
  lightRenderTunnel,      // LIGHT_TUNNEL
  lightRenderPacman,          // LIGHT_PACMAN
  lightRenderStreetFighter,   // LIGHT_STREETFIGHTER
  lightRenderMario,           // LIGHT_MARIO
  lightRenderMissile,         // LIGHT_MISSILE
  lightRenderPsychedelic,     // LIGHT_PSYCHE
  lightRenderVortex,          // LIGHT_VORTEX
  lightRenderAquarium,             // LIGHT_AQUARIUM
  lightRenderFlyingPompadour,      // LIGHT_FLYINGPOMPADOUR
  lightRenderRainbowWashingMachine, // LIGHT_RAINBOWWASHER
  lightRenderPixelInvasion,         // LIGHT_PIXELINVASION
};

// LIGHT_LAYER[] / LIGHT_HEADER[] / lightingHeaderDark() / LIGHT_LAYER_BG・OVL /
// HEADER_LIGHT・DARK は、showSensors()から参照するためファイル前半で定義済み。
// gLightBgFilled も同様（screenFxLighting付近）。

// Lighting有効判定（Visualizerと同じゲート：UDP/LINE IN かつ FFT鮮度あり）
// 2026-07-25: lightingScreenActive()と同じ理由でMIC選択時はFFT新鮮判定を免除。
//   詳細はlightingScreenActive()直上のコメント参照。UDP/LINE INの判定は不変。
bool isLightingEnabled() {
  if (cfg_lightingMask == 0) return false;
  if (audioSource == AUDIO_SRC_MIC) return true;
  return (audioSource == AUDIO_SRC_UDP || audioSource == AUDIO_SRC_LINEIN) &&
         lastFftPacketTime != 0 &&
         (millis() - lastFftPacketTime) <= FFT_FACE_ACTIVE_MS;
}

// ============================================================================
// lightingActiveWithGrace()（2026/07/27 追加）
//
// 【背景】実機の診断ログ（WHITE_FLASH_CAUSE）で、Retro Game Lighting表示中
//   約30秒に1回、FFT_FACE_ACTIVE_MS(500ms)をわずか11〜58ms超過した瞬間に
//   isLightingEnabled()がfalseとなり、exitLightingCompositeMode(true)経由で
//   一瞬だけ通常の白画面へ戻っていたことが確定した（音声入力自体は継続中で
//   EAR RXも増加し続けており、真の音声切断ではなかった）。
//
// 【方針】FFT_FACE_ACTIVE_MS自体やLighting開始条件は変更しない。
//   ・Lighting開始（screenFxLightingがまだfalse）: 従来どおり
//     isLightingEnabled()の500ms判定のみをそのまま使う。
//   ・Lighting継続中（screenFxLightingがtrue）: 一度有効になった後だけ、
//     FFTが途絶えてからLIGHTING_FFT_GRACE_MS(3秒)以内ならLighting継続を
//     許容し、一時的な受信間隔の揺らぎで白画面へ落ちないようにする。
//   ・sleep/imageFace等、即座に終了すべき条件には本グレースを適用しない
//     （updateScreenEffects()側で !sleepMode && !imageFaceMode を本関数の
//     戻り値に別途ANDしているため、そちらが真の終了条件として独立に効く）。
//
// 8-Lane RhythmのRHYTHM_OFF_GRACE_MS（Visualizer側・isVisualizerFaceEnabled()
// 用）とは別系統。今回はLighting側の状態遷移として同じ考え方を実装したもので、
// 重複を避けるため isLightingEnabled() 自体は変更せず、この関数だけを
// updateScreenEffects() の1呼び出し箇所から使う。
// ============================================================================
const unsigned long LIGHTING_FFT_GRACE_MS = 3000;

bool lightingActiveWithGrace() {
  bool raw = isLightingEnabled();
  static unsigned long lightingLostSinceMs = 0;
  unsigned long now = millis();

  if (raw) {
    lightingLostSinceMs = 0;   // FFTが新鮮に戻ったのでグレースタイマーを解除
    return true;
  }

  if (!screenFxLighting) {
    // まだLighting合成を開始していない＝これから始めようとしている状態。
    // 開始条件にはグレースを適用せず、従来どおり500ms判定のみに従う。
    lightingLostSinceMs = 0;
    return false;
  }

  // 継続中にFFTが途絶えた瞬間を記録し、猶予時間内かどうかだけを見る。
  if (lightingLostSinceMs == 0) lightingLostSinceMs = now;
  return (now - lightingLostSinceMs) < LIGHTING_FFT_GRACE_MS;
}

// ============================================================================
// Screen Effects コンポジタ（loop末尾から呼ばれる唯一の入口）
//
//   Lighting OFF  → updateVisualizerFace()（従来パス・完全に元のまま）
//   Lighting ON   → Layer0 Lighting → Layer1 Visualizer(overlay) → Layer2 顔
// ============================================================================
const uint16_t LIGHT_COMPOSITE_MS = 90;      // 合成周期（約11fps）。負荷が高い時はここを大きく（100〜140）
// screenFxLighting はファイル前半（cfg_lightingMask付近）でグローバル宣言済み。
static bool lightNeedsInit   = true;

// ============================================================================
// exitLightingCompositeMode() — Lighting合成モードの【統一終了処理】
//
// スリープ／起床／画像顔／アートギャラリー／alert／音停止／WebUI OFF など、
// あらゆる離脱経路でここ1か所を呼べば、状態フラグと画面が一貫して戻る。
// 個別箇所でフラグを散発的に操作して不整合が起きるのを防ぐ。
//
//   ・gLightingActive   = false   （Visualizer/顔パーツのオーバーレイ解除）
//   ・screenFxLighting  = false   （上端パネルを白帯へ戻す条件）
//   ・lightNeedsInit    = true    （次に有効化した時に全面初期化）
//   ・Visualizer再初期化（visualizerFaceActive=false / vizNeedsInit=true）
//   ・redrawTopPanel=true なら通常顔＋showSensors()（白帯）を即再描画
// ============================================================================
void exitLightingCompositeMode(bool redrawTopPanel) {
  bool wasActive = screenFxLighting;
  gLightingActive      = false;
  screenFxLighting     = false;
  lightNeedsInit       = true;
  visualizerFaceActive = false;
  vizNeedsInit         = true;
  gEyeSlotActive       = false;   // Eye Slot中の「黒目を描かない」状態を持ち越さない
  // Retro Race（Pole Position風デモ）が黒目オフセットを動かしていた場合に備え、
  // 通常顔へ戻る際は必ず中央へ戻す（他のLightingは目オフセットを操作しないため無害）。
  eyeOffsetX = 0;
  eyeOffsetY = 0;
  sceneInvalidate();   // 次のCanvas転送を全面にして、Lightingの残像を確実に消す
  if (redrawTopPanel && !imageFaceMode && !sleepMode) {
    drawFace();        // 通常の白背景・顔へ（統合Canvasで全面再構築）
    showSensors();     // lightingScreenActive()=false になったので白帯＋通常文字色で描き直す
  }
  if (wasActive) addLog("LIGHTING EXIT");
}

// ============================================================================
// 統合描画パイプラインの各レイヤー
//
// Lighting時・Visualizer単体時・通常表示（Lighting全OFF）時のすべてが
// sceneComposeAndPush() という同じ経路を通る。顔描画方式をLightingの有無で
// 分けないため、顔パーツ消去用の白い矩形が色付き背景の上に露出しなくなる。
// ============================================================================

// ── Layer0：Lighting ───────────────────────────────────────────────
// cfg_lightingMask == 0（Lighting全OFF）なら何も描かない＝背景は白のまま。
// Canvas描画用のLighting ON/OFFフラグは新設していない（判定は従来のマスクのみ）。
// 背景Lightingは最後に選択された1つ、その上にOverlay Lightingを全て重ねる
// （重ね順・演出内容は従来仕様のまま）。
void sceneDrawLightingLayer(bool init, bool fullRepaint) {
  if (cfg_lightingMask == 0) return;
  // Lighting を「背景(面)→オーバーレイ(ビーム)」の2パスで描く。
  //   ① 背景は最後に選ばれた1つだけ表示（Disco と Aurora 両方ONなら後勝ち）
  //   ② オーバーレイ(Laser)は全て、背景の上に重ねる
  gLightBgFilled = false;
  gLightActiveBgMode = 0xFF;
  gEyeSlotActive = false;   // Eye Slotが今回のbgModeでなければfalseのまま（lightRenderEyeSlot側でtrueにする）
  // 2026-07-25: 黒目制御を持つbgModeが選択されなくなった直後にeyeOffsetX/Yが
  //   前のモードの値のまま残ってしまわないよう、bgMode振り分け直前で毎フレーム
  //   いったんリセットする。Retro Race/Sky Raid/Classic Race/Asteroid Field/
  //   Tempest Tunnelは自分が実際に描画される全フレームでeyeOffsetX/Yを無条件に
  //   上書きするため、この行があってもそれらの挙動は一切変化しない。
  //   Disco/Aurora/Matrix/Eye Slot等、黒目を制御しないbgModeでは0,0の
  //   自然な状態が保証されるようになる（従来はここが未リセットだったため、
  //   直前に選ばれていた黒目制御モードの値が残ってしまう余地があった）。
  eyeOffsetX = 0;
  eyeOffsetY = 0;
  int bgMode = -1;
  for (uint8_t li = 0; li < (uint8_t)LIGHT_MODE_COUNT; li++) {
    if ((cfg_lightingMask & (1u << li)) && LIGHT_LAYER[li] == LIGHT_LAYER_BG) bgMode = (int)li;
  }
  if (bgMode >= 0) { LIGHT_RENDER_FN[bgMode](init, fullRepaint); gLightBgFilled = true; gLightActiveBgMode = (uint8_t)bgMode; }
  for (uint8_t li = 0; li < (uint8_t)LIGHT_MODE_COUNT; li++) {
    if ((cfg_lightingMask & (1u << li)) && LIGHT_LAYER[li] == LIGHT_LAYER_OVL) {
      LIGHT_RENDER_FN[li](init, fullRepaint);
    }
  }
}

// ── Layer1：Visualizer ─────────────────────────────────────────────
// 各Visualizerの種類・動き・色・感度・更新タイミングは従来のまま。
void sceneDrawVisualizerLayer(bool on, uint8_t m, bool init) {
  if (!on) return;
  if (m >= (uint8_t)VIZ_MODE_COUNT || VIZ_RENDER_FN[m] == NULL) return;
  VIZ_RENDER_FN[m](init);
}

// ── Layer2：顔 ─────────────────────────────────────────────────────
// レイヤー順は従来仕様を厳密に維持する：
//   ・Lighting合成中      … 顔が最前面（コンポジタが最後に描く）
//   ・Visualizer単体表示中 … 顔は各Visualizerが内部で描く（Graphic EQはバーが最前面）
//   ・通常表示            … 白背景の上に目・まつ毛・鼻・口
void sceneDrawFaceLayer() {
  if (gLightingActive) { drawVisualizerFaceParts(false); return; }
  if (visualizerFaceActive) return;
  sceneDrawNormalFace();
}

// ── 統合描画パイプライン（唯一の合成経路）─────────────────────────
//   1. Canvasへ背景を描画
//   2. Lightingを描画
//   3. Visualizerを描画
//   3.5. Eye Slot選択中のみ：窓を不透明な白で塗り直してから目（リール）を
//        Visualizerの上へ再描画
//   4. 目・まつ毛・鼻・口を描画
//   5. 完成したCanvasを液晶へ転送（y >= SCENE_TOP のみ／上端48pxは触らない）
// Canvas未確保（PSRAM確保失敗）時は従来どおり液晶へ直接描画される。
//
// 2026-08-07追加（Eye Slot × Visualizer全画面描画の重なり対策）：
//   Kaleidoscope／Analogue VU等、画面全体を塗りつぶすVisualizerがLayer3で
//   描かれると、Layer2（Lighting）で既に描いていたEye Slotのリールがその下に
//   埋もれて見えなくなってしまう。Visualizer個別に「目の領域だけ描かない」
//   例外処理を足すのではなく、Eye Slotが背景として選ばれている間は共通して
//   「Visualizer描画の直後・顔レイヤーの直前」にリールだけを最前面へ描き直す
//   構造にした（描画順：Lighting背景 → Visualizer → Eye Slot → 顔）。
//   gEyeSlotActiveはlightRenderEyeSlot()/eslotUpdateAndDrawReels()がEye Slot
//   選択時にのみtrueにするフラグなので、Eye Slot以外のLighting・通常表示・
//   Visualizer単体表示（Lighting OFF）ではfalseのまま＝一切影響しない。
//   条件はgEyeSlotActive && vizOnのみで判定しており、7種類あるVisualizerの
//   どれが選ばれていても同じ1行が共通に効く（Visualizerモード別の分岐は
//   一切追加していない）。
//
// 2026-08-07追記（実機確認：リール背景が透けてVisualizerの色が見える不具合の修正）：
//   最初の対策ではeslotDrawReelsFrame()（絵柄本体の描画のみ）しか呼んでおらず、
//   絵柄と絵柄の隙間からVisualizerの描画がそのまま透けて見えてしまっていた。
//   Sleep Lighting Carousel（sleepComposeEyeLightFrame、後述）が以前から「窓の
//   背後を不透明な白で塗ってから絵柄を描く」処理を持っていたため、その実装を
//   eslotFillWindowsWhite()として切り出し、Sleep Carousel側もこちらを呼ぶよう
//   統一したうえで、Visualizer描画直後にも同じ関数を再利用する。
//   eslotDrawReelsFrame()と同様、eslotFillWindowsWhite()も状態は一切進めない
//   （単純な塗りつぶしのみ）ため、1フレームに2回呼んでも副作用は無い。
//   再描画にはeslotFillWindowsWhite()＋eslotDrawReelsFrame()（どちらも状態を
//   進めない純粋な描画のみの関数）を使うため、1フレームに2回呼んでもリールの
//   アニメーション速度やフラッシュのタイミングは変化しない（＝新しい重複描画
//   コードを増やさず、既存のEye Slot描画ロジックをそのまま再利用している）。
//   顔レイヤー（鼻・口・まつ毛。時刻・バッテリー等の上端パネルはCanvas対象外で
//   別経路のまま）は従来どおりLayer4として最後に描かれるため、他パーツの
//   描画順は変わらない。
void sceneComposeAndPush(bool lightInit, bool lightFull,
                         bool vizOn, uint8_t vizMode, bool vizInit,
                         int px, int py, int pw, int ph) {
  bool onCanvas = sceneBeginCompose();                         // 1
  if (cfg_lightingMask != 0 && gLightingActive) {              // 2
    sceneDrawLightingLayer(lightInit, lightFull);
  }
  sceneDrawVisualizerLayer(vizOn, vizMode, vizInit);           // 3
  if (gEyeSlotActive && vizOn) {                               // 3.5
    eslotFillWindowsWhite();   // 窓の背後を不透明な白で塗り直す（Visualizerの色が透けないように）
    eslotDrawReelsFrame();     // その上へ絵柄本体を描く（従来どおり）
  }
  sceneDrawFaceLayer();                                        // 4
  sceneEndCompose(onCanvas);
  if (onCanvas) scenePush(px, py, pw, ph);                     // 5
}

void updateScreenEffects() {
  bool lightingOn = lightingActiveWithGrace() && !sleepMode && !imageFaceMode;

  if (!lightingOn) {
    if (screenFxLighting) {
      // 合成モードを抜ける：統一終了処理で通常顔・白帯へ戻し、Visualizerも再init。
      exitLightingCompositeMode(true);
    }
    updateVisualizerFace();   // ← Lighting非表示時の駆動（Visualizer単体／通常表示）。
                              //   内部でも同じ統合Canvasパイプラインを通る。
    return;
  }

  // 2026-07-29修正：Lighting ON時もisVisualizerFaceEnabled()を直に見るのではなく、
  // Lighting OFF経路(updateVisualizerFace)と共通のisVisualizerFaceEnabledWithGrace()
  // を通す。これによりKaleidoscope選択中はLighting ON/OFFどちらでも短時間FFT途絶で
  // 一瞬消えなくなる（Kaleidoscope以外のモードでは従来のisVisualizerFaceEnabled()と
  // 完全に同じ値を返すため、他モードの挙動は一切変わらない）。
  uint8_t vm = (uint8_t)cfg_visualizerMode;
  bool vizOn = isVisualizerFaceEnabledWithGrace(vm);

  static bool prevVizOn = false;
  bool vizChanged = (vizOn != prevVizOn);
  prevVizOn = vizOn;

  if (!screenFxLighting) {
    screenFxLighting = true;
    lightNeedsInit   = true;
    visualizerFaceActive = false;   // 通常Visualizerパスの状態をリセット
    addLog("LIGHTING ON");
  }

  // Brightnessが変更されたら全面initして即時反映（差分描画でも取りこぼさない）
  if (gLightNeedReinit) { lightNeedsInit = true; gLightNeedReinit = false; }

  // 描画周期制限（既存処理のリアルタイム性を守る）
  static unsigned long lastFx = 0;
  unsigned long now = millis();
  if (!lightNeedsInit && (now - lastFx) < LIGHT_COMPOSITE_MS) return;
  lastFx = now;

  vizUpdateState((uint32_t)now);

  gLightingActive = true;   // Visualizer/顔パーツをオーバーレイモードにする

  bool init = lightNeedsInit;
  lightNeedsInit = false;

  // 合成モード開始時：上端の情報パネルを「採用中の背景テーマの色」で先に塗る
  // （showSensors()が文字を描くまでのちらつき防止）。以降 Lighting は上端48pxを
  // 描かないので、この帯は showSensors() が保持する。
  //   Disco系＝白 / Laser・Aurora系＝黒。
  if (init) CoreS3.Display.fillRect(0, 0, 320, 48, lightingHeaderDark() ? BLACK : WHITE);

  // Layer0：Lighting の描画パラメータを決める（実際の描画は統合パイプライン内）
  //   Visualizer併用時・viz切替時・init時は全面塗りで残像を消す
  // Laserが有効な合成フレームは全面塗りを強制（前フレームのビーム残像を消すため）。
  bool laserOn  = (cfg_lightingMask & (1u << LIGHT_LASER)) != 0;
  bool discoOn  = (cfg_lightingMask & (1u << LIGHT_DISCO)) != 0;
  bool auroraOn = (cfg_lightingMask & (1u << LIGHT_AURORA)) != 0;
  bool matrixOn = (cfg_lightingMask & (1u << LIGHT_MATRIX)) != 0;
  bool raceOn    = (cfg_lightingMask & (1u << LIGHT_RACE)) != 0;
  bool skyraidOn = (cfg_lightingMask & (1u << LIGHT_SKYRAID)) != 0;
  bool eyeslotOn = (cfg_lightingMask & (1u << LIGHT_EYESLOT)) != 0;
  bool classicraceOn = (cfg_lightingMask & (1u << LIGHT_CLASSICRACE)) != 0;
  bool asteroidOn = (cfg_lightingMask & (1u << LIGHT_ASTEROID)) != 0;
  bool tunnelOn = (cfg_lightingMask & (1u << LIGHT_TUNNEL)) != 0;
  bool pacmanOn = (cfg_lightingMask & (1u << LIGHT_PACMAN)) != 0;
  bool streetfighterOn = (cfg_lightingMask & (1u << LIGHT_STREETFIGHTER)) != 0;
  bool marioOn = (cfg_lightingMask & (1u << LIGHT_MARIO)) != 0;
  bool missileOn = (cfg_lightingMask & (1u << LIGHT_MISSILE)) != 0;
  bool fullRepaint = vizOn || vizChanged || init || laserOn || auroraOn || matrixOn || raceOn || skyraidOn || eyeslotOn || classicraceOn || asteroidOn || tunnelOn || pacmanOn || streetfighterOn || marioOn || missileOn;

  // 重い組合せではレーザー本数を自動削減（サーボ・通信・口パクを優先）。
  //   背景 + Laser + Visualizer = 最重 → 8本 / (背景+Laser) or (Laser+Viz) → 10本 / それ以外 14本
  bool bgOn = discoOn || auroraOn || matrixOn || raceOn || skyraidOn || eyeslotOn || classicraceOn || asteroidOn || tunnelOn || pacmanOn || streetfighterOn || marioOn || missileOn;
  if (bgOn && laserOn && vizOn)         laserMaxBeams = 8;
  else if ((bgOn && laserOn) || (laserOn && vizOn)) laserMaxBeams = 10;
  else                                  laserMaxBeams = LASER_MAX;
  // ── 統合描画パイプライン ──
  //   Layer0 Lighting → Layer1 Visualizer → Layer2 顔 をCanvas上で合成し、
  //   完成後に y=48〜239 だけを液晶へ一括転送する。
  sceneComposeAndPush(init, fullRepaint, vizOn, vm, false,
                      0, SCENE_TOP, SCENE_W, SCENE_H - SCENE_TOP);

  gLightingActive = false;

  // 背景テーマ（白/黒）が切り替わった瞬間は、上端パネルを即座に描き直す
  // （例: Web操作で Disco→Aurora に変えた時。次の300ms周期を待たずに配色を更新）。
  static bool prevHdrDark = false;
  static bool prevFxOn    = false;
  bool hd = lightingHeaderDark();
  if (hd != prevHdrDark || !prevFxOn) { prevHdrDark = hd; showSensors(); }
  prevFxOn = true;
}

// ============================================================================
// Sleep Lighting Carousel（眺めて楽しいSleep表示）実装本体
//
// handleSleepMode() 側（SLEEP_FACE_DURATION経過後）から
// updateSleepLightingCarousel() を毎ループ呼ぶことで、
//   ・SLEEP_FACE_ROTATE_MS（3分）ごとに「Face Gallery」か「目×Lighting」かを抽選
//   ・「目×Lighting」の間は LIGHT_COMPOSITE_MS（既存90ms、通常Lighting合成と同じ周期）で
//     背景Lightingと目・鼻口を統合Canvasへ合成して液晶へ転送する（Zzzは出さない。
//     Zzzは入眠直後3分間の静止閉じ目でのみ表示する既存仕様のまま）
// を行う。cfg_lightingMask 等の共有Lighting設定・Visualizer状態は一切変更しない。
// ============================================================================

// 背景Lighting候補を1つ選ぶ（Eye Slotは目として使うため除外／直前と同じ値は避ける）。
uint8_t sleepPickBgLightMode() {
  uint8_t pick;
  do {
    pick = (uint8_t)random(0, (long)LIGHT_MODE_COUNT);
  } while (pick == LIGHT_EYESLOT || (int)pick == sleepLastBgLightMode);
  sleepLastBgLightMode = (int8_t)pick;
  return pick;
}

// 目の種類を1つ選ぶ（直前と同じ値は避ける）。
// 時刻が一度も同期できていない端末では「時計の目」を候補から除外する
// （既存のSleep時計フォールバック仕様＝未同期時は時計を表示しない、を踏襲）。
uint8_t sleepPickEyeKind() {
  uint8_t pick;
  do {
    pick = (uint8_t)random(0, (long)SLEEP_EYE_KIND_COUNT);
  } while ((int)pick == sleepLastEyeKind || (pick == SLEEP_EYE_CLOCK && !timeEverSynced));
  sleepLastEyeKind = (int8_t)pick;
  return pick;
}

// 2026-08-04 実機確認により、Sleep Lighting Carousel（Face Gallery／目×Lighting）中は
// Zzzを表示しない仕様に変更した（Zzzは入眠直後3分間の静止閉じ目でのみ表示する）。
// そのため本カルーセル専用のZzz描画関数はここでは持たない。

// 上端48pxの情報パネル（IP・電池）をSleep Lighting Carousel用に描く。
// V/A/M等のセンサー値は既存Sleep表示（drawIpStatusOnly系）と同様に出さない
// （showSensors()は使わない＝既存Sleepの見た目の方針を踏襲）。
// 電池アイコンは既存drawBattery()をそのまま再利用する
// （sleepLightingComposeActive経由で本カルーセルのテーマに追従する。drawBattery()参照）。
void drawSleepTopPanel(bool dark) {
  CoreS3.Display.fillRect(0, 0, 320, 48, dark ? BLACK : WHITE);

  String ipText;
  if (WiFi.getMode() == WIFI_AP) {
    ipText = "IP " + WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    ipText = "IP " + WiFi.localIP().toString();
  } else {
    ipText = "IP OFFLINE";
  }

  CoreS3.Display.setTextSize(2);
  CoreS3.Display.setTextColor(dark ? WHITE : PURPLE);
  CoreS3.Display.drawString(ipText, 5, 26);

  drawBattery();
}

// 目×背景Lighting の1フレーム分を統合Canvasへ合成して液晶へ転送する。
// 既存の sceneComposeAndPush() と同じ「Canvas未確保なら直接液晶へ」フォールバックを
// sceneBeginCompose()/sceneEndCompose()/scenePush() でそのまま踏襲する。
// isLightingEnabled()等の音声セッション判定は経由しない（LIGHT_RENDER_FN[]を直接呼ぶ）。
//
// 顔パーツ（時計の目／閉じ目／鼻／鼻口線／口）は、背景Lightingの明暗によらず
// 常に「黒本体＋白縁取り」で統一する（実機確認 2026-08-04。旧版は暗背景時だけ
// 白反転していたが、背景色によって視認性に差が出たため統一した）。
// Eye Slotの目だけは例外で、リール表示領域を常に白背景にする（既存Eye Slotの
// リール内容・アニメーション自体は無改造）。
// Zzzはここでは描かない（入眠直後3分間の静止閉じ目でのみ表示する既存仕様のまま）。
void sleepComposeEyeLightFrame(bool bgNeedsInit) {
  bool onCanvas = sceneBeginCompose();

  // gViz（音声可視化の共通ステート）を無音方向へ自然減衰させるためだけに呼ぶ。
  // Visualizerそのものは使わない。FFT受信・音声入力そのものには一切触れない。
  vizUpdateState((uint32_t)millis());

  // ── Layer0：背景Lighting（Eye Slotを除く既存Lightingのいずれか1つ）──
  gEyeSlotActive = false;
  eyeOffsetX = 0;
  eyeOffsetY = 0;
  LIGHT_RENDER_FN[sleepBgLightMode](bgNeedsInit, true);

  // 上端48pxパネルの配色判定にはテーマ（黒/白）を引き続き使う（顔パーツの色には使わない）。
  bool dark = (LIGHT_HEADER[sleepBgLightMode] == HEADER_DARK);
  sleepPanelDark = dark;

  // ── Layer1：目（3種のいずれか。背景の上に重ねて描く）──
  switch (sleepEyeKind) {
    case SLEEP_EYE_EYESLOT: {
      // Eye Slotは例外：リール領域は背景Lightingのテーマに関わらず常に白背景にする
      // （既存Eye Slotの見た目・アニメーション自体は無改造。窓の背後だけ白く敷く）。
      // 2026-08-07: 窓の白塗り処理はeslotFillWindowsWhite()へ共通化（実装は元のまま移設）。
      eslotFillWindowsWhite();
      eslotUpdateAndDrawReels(bgNeedsInit);
      break;
    }
    case SLEEP_EYE_CLOSED:
      drawClosedEyeLinesOutlined();
      break;
    case SLEEP_EYE_CLOCK:
    default:
      drawClockEyeDigitsOutlined();
      break;
  }

  // ── 鼻・鼻口線・口（黒＋白縁取りで統一。既存faceShapeNoseAndVMouth()は無改造のまま利用）──
  drawNoseAndMouthOutlined();

  sceneEndCompose(onCanvas);
  if (onCanvas) scenePush(0, SCENE_TOP, SCENE_W, SCENE_H - SCENE_TOP);

  // 上端48pxはCanvas対象外の別領域なので、通常のLightingと同様ここで直接描く。
  drawSleepTopPanel(dark);
}

// handleSleepMode() から毎ループ呼ぶ入口。
//   ・SLEEP_FACE_ROTATE_MS（3分）ごとに「Face Gallery」か「目×Lighting」かを抽選
//   ・パターン自体の連続回避はしない（Face Galleryと目×Lightingが連続することは許容する。
//     目・背景Lightingそれぞれの直前重複だけを避ける＝既存Lighting Randomと同じ粒度）
//   ・「目×Lighting」表示中は LIGHT_COMPOSITE_MS 周期で背景Lightingごと再描画する
void updateSleepLightingCarousel() {
  unsigned long now = millis();

  if (sleepCarouselNextSwitchMs == 0 || (long)(now - sleepCarouselNextSwitchMs) >= 0) {
    sleepCarouselNextSwitchMs = now + SLEEP_FACE_ROTATE_MS;

    uint8_t pattern = (uint8_t)random(0, 2);   // 0=Face Gallery, 1=目×Lighting
    sleepLastPattern = (int8_t)pattern;

    if (pattern == 0) {
      // パターン1：Face Gallery（既存showSleepFace()をそのまま利用。無改造）
      sleepLightingComposeActive = false;
      showSleepFace();
      addLog("SLEEP CAROUSEL: gallery");
    } else {
      // パターン2：ランダムな目 × ランダムな背景Lighting
      imageFaceMode = false;      // Face Gallery（画像表示）状態から抜ける
      sleepFaceActive = false;
      sceneInvalidate();          // 直前がPNG等の直接描画だった場合に備え、次のscenePushを全面にする
      sleepEyeKind        = sleepPickEyeKind();
      sleepBgLightMode    = sleepPickBgLightMode();
      sleepLightNeedsInit = true;
      sleepLightingComposeActive = true;
      addLog("SLEEP CAROUSEL: eye=" + String((int)sleepEyeKind)
           + " light=" + String(LIGHT_MODES[sleepBgLightMode].id));
    }
  }

  if (sleepLightingComposeActive) {
    static unsigned long lastDrawMs = 0;
    bool needInit = sleepLightNeedsInit;
    sleepLightNeedsInit = false;
    if (needInit || (now - lastDrawMs) >= LIGHT_COMPOSITE_MS) {
      lastDrawMs = now;
      sleepComposeEyeLightFrame(needInit);
    }
  }
}

#endif  // FFT_DISPLAY_TEST

void loop() {

  // loop周回カウンタ（PWR SNAP の loops= に載せる）。
  // オーバーフローしても差分だけ見れば足りるので飽和処理はしない。
  loopCounter++;

  CoreS3.update();

  // LRサーボ・フェイルセーフ（sleepの早期returnより前に必ず実行）
  handleLrFailsafe();

  // Webからの発話リクエスト処理
  handleSpeakRequest();

  // Web / UDP / Mac音声連動 / WiFi監視
  handleCommunication();

  // 自動スリープ中の起床判定・処理ゲート
  if (handleStandbyGate()) {
    return;
  }

  // 頭タッチ判定
  bool touchedHead = isHeadTouched();

  // ====================================================
  // 常時マイク入力（updateVolume）は停止中
  //
  // CoreS3.Mic.record() と Speaker.playWav() の同時使用は
  // 激しく音割れする（2026/06/07確認済。soundBusyでも改善せず）。
  //
  // マイクを使う場合はWebの【Karipom Ear】で「内蔵マイク」を選択する。
  // 内蔵マイクモードはスピーカーを完全停止した専用モードとして
  // handleCommunication() → updateAudioInput() 側で動作する。
  // ====================================================

  // 音量更新
  int volume = updateVolume();

  // センサー表示更新
  handleSensorDisplay();

  // カメラ動体検知（睡眠中と覚醒中で判定条件を切替）
  bool cameraStimulus = updateCameraStimulus();

  // IMU動体検知（揺れ・持ち上げ）
  bool imuStimulus = updateImuStimulus();

  // 音検知
  bool soundStimulus = updateSoundStimulus(volume);

  // 音がある環境では眠らせない
  handleSoundActivity(volume);

  // 尻尾ジョイスティック操作
  handleJoystick();

  // 鼻ヒクヒク（覚醒中・睡眠中問わず常時実行）
  handleNoseMotion();

  // 睡眠中の処理（起床判定を含む）
  if (handleSleepMode(touchedHead, cameraStimulus, imuStimulus, soundStimulus)) {
    return;
  }

  // 一定時間無操作なら睡眠へ移行
  if (handleSleepTransition()) {
    return;
  }

  // 待機中のランダムな首振り
  handleIdleServoMotion();

  // 頭なで
  handlePetTouch(touchedHead);

  // 大きな音で警戒
  handleSoundAlert(soundStimulus);

  // 瞬き
  handleBlinkMotion();

  // 警戒解除
  handleAlertRelease();

  // ごくたまに独り言
  handleRandomMutter();

  // 低バッテリー通知
  handleBatteryLow();

  // LRサーボ自動 detach（動作終了から LR_DETACH_DELAY_MS 後にトルクフリーへ）
  handleLrAutoDetach();

#ifdef FFT_DISPLAY_TEST
  // Visualizer Face：早期returnせず、loop()の全処理（タッチ・ジョイスティック・
  // サーボ・睡眠・アイドル等）を通した後に「顔描画だけ」を差し替える。
  // ここはloop()末尾＝覚醒中のみ到達する位置のため、睡眠中は
  // 自動的に描画されない。有効条件の判定は updateVisualizerFace() 内。

  // 🎲 Lighting Random / Audio Visualizer Random の切替判定。
  // 実際の描画(updateScreenEffects)より前に呼び、選択が決まった状態で
  // このフレームの描画へ反映させる。両者は完全に独立している。
  updateLightingRandomTick();
  updateVisualizerRandomTick();

  updateScreenEffects();
#endif
}
