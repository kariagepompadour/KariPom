// ============================================================
// Karipom_Ear_Pico_Step2_UART_Echo.ino   v1.0
// Karipom Ear v2 - Phase 0 Step 2：UART送信内容のUSBエコー検証（Pico単体）
//
// スコープ：CoreS3には**まだ接続しない**。Grove未接続のまま、
//   Pico単体で「UART0へ送るテキストプロトコル」を組み立て、
//   同じ内容をUSBシリアルへエコーして書式・レート・挙動を確認する。
//
// Step1から維持：I2Sスレーブ受信／起動時同期プローブ＋再同期／
//   fmt自動判定（A/B）／mis監視／n・ovf・タイムアウト監視
// Step2で追加：
//   - モノラル化 → 96kHz→16kHzデシメーション（6サンプル平均）
//   - FFT 512点（arduinoFFT v2.x・Hann窓）→ 8バンド（オクターブ割り）0〜100
//   - 簡易レベルゲート（RMS閾値+ヒステリシス+ハングオーバ）※VADではない
//   - UART0（Serial1, GP0=TX/GP1=RX, 115200bps 8N1）へ既存互換プロトコル送信
//       FFT:a,b,c,d,e,f,g,h\n   （25Hz）
//       SPEAK_START\n           （ゲート開時＋開中1秒毎ハートビート）
//       SPEAK_STOP\n            （ゲート閉時）
//   - 送信と同一内容をUSBへ「TX> 」プレフィクス付きでエコー
//   - 任意：GP0-GP1をジャンパ直結すると自動でループバック検出（lb=OK）
//       → CoreS3なしでUART電気層まで自己検証できる
//
// 必要ライブラリ：arduinoFFT v2.x（ライブラリマネージャで「arduinoFFT」
//   kosme版をインストール。v1系とはAPI非互換のためv2.0以上を選ぶこと）
//
// 安全条件（Step1と同一）：
//   - PicoはBCLK/LRCK/MCLKを出力しない（setSlave受信のみ）
//   - setMCLK()/setBCLK()不使用、setSlave()はbegin()前
//   - 配線変更なし：DATA→GP2/BCLK→GP3/LRCK→GP4/MCLK未接続
//   - UARTはGP0(TX)/GP1(RX)のみ。CoreS3未接続時は開放でよい
// ============================================================

#include <I2S.h>
#include <arduinoFFT.h>

// ---- I2S（Step1と同一・変更禁止） ----
static const int      PIN_DIN   = 2;
static const long     FS_HZ     = 96000;   // M-96K。M-48K移行時は48000（DECIM_Nも変更：下記）
static const int      BITS      = 32;
static const size_t   BUF_COUNT = 8;
static const size_t   BUF_WORDS = 256;

// ---- 信号処理 ----
static const int   DECIM_N   = 6;          // 96k/6=16k（48k運用時は3にして16k維持）
static const long  FS_DECIM  = FS_HZ / DECIM_N;   // 16000
static const int   FFT_N     = 512;        // 512@16k = 32ms/フレーム, 分解能31.25Hz
static const float FS24      = 8388607.0f; // 24bitフルスケール

// 8バンドのビン境界（31.25Hz/bin）：オクターブ割り 31..8000Hz
// band k = bin[EDGE[k]] .. bin[EDGE[k+1]-1]
static const int BAND_EDGE[9] = {1, 2, 4, 8, 16, 32, 64, 128, 256};

// 表示スケール（要調整ノブ：Step2成功判定は相対挙動で行う）
static const float FFT_FLOOR_DB = -63.0f;  // これ以下を0
static const float FFT_TOP_DB   = -3.0f;   // これ以上を100

// ---- レベルゲート（簡易・VADではない） ----
static const float GATE_OPEN_DB  = -45.0f; // 開く閾値（dBFS, 要調整）
static const float GATE_CLOSE_DB = -50.0f; // 閉じる閾値（ヒステリシス）
static const uint32_t GATE_HANG_MS      = 500;   // 閉じるまでの保持
static const uint32_t SPEAK_HEARTBEAT_MS = 1000; // 開中の再送間隔

// ---- 送信 ----
static const uint32_t FFT_SEND_MS   = 40;  // 25Hz
static const int      ECHO_EVERY_N  = 1;   // FFT行のUSBエコー間引き（1=全行）
static const uint32_t REPORT_MS     = 1000;
static const uint32_t NO_DATA_TIMEOUT_MS = 2000;

// ---- 同期検査（Step1と同一） ----
static const int ALIGN_MAX_ATTEMPTS = 25;
static const int PROBE_WORDS        = 2048;
static const int PROBE_MIN_NONZERO  = 16;
static const int BAD_PERMILLE       = 20;

I2S i2s(INPUT);

enum FmtModel { MODEL_UNKNOWN, MODEL_A, MODEL_B };
FmtModel fmtModel = MODEL_UNKNOWN;
bool alignVerified = false;
int  alignAttempts = 0;

static inline int32_t toS24(int32_t raw) {
  if (fmtModel == MODEL_B) return (int32_t)((uint32_t)raw << 1) >> 8;
  return raw >> 8;
}
static inline bool fitsModel(uint32_t w, FmtModel m) {
  if (m == MODEL_A) return (w & 0x000000FFu) == 0;
  else              return (w & 0x8000007Fu) == 0;
}

// ---- FFT ----
float vReal[FFT_N];
float vImag[FFT_N];
ArduinoFFT<float> FFT(vReal, vImag, (uint_fast16_t)FFT_N, (float)FS_DECIM);

// ---- 状態 ----
int32_t  decimAcc = 0;      // デシメーション用累積
int      decimCnt = 0;
float    fftBuf[FFT_N];     // 16kHzモノラル蓄積
int      fftFill = 0;
uint8_t  band[8] = {0};     // 最新の8バンド(0-100)
bool     bandsValid = false;
float    frameRmsDb = -120; // 直近FFTフレームのRMS(dBFS)

bool     gateOpen = false;
uint32_t gateLastLoudMs = 0;
uint32_t lastHeartbeatMs = 0;

uint32_t lastFftSendMs = 0;
uint32_t fftSentCount = 0;   // 今周期の送信FFT行数
uint32_t fftEchoSkip = 0;

// ループバック検出（GP0-GP1直結時のみ機能。未接続なら "--"）
uint32_t lbRxBytes = 0;
bool     lbSeen = false;

uint32_t misCount = 0, overflowCount = 0, noDataPeriods = 0;
uint32_t sampleCount = 0;    // 今周期のL/Rペア数
uint32_t lastReportMs = 0, lastDataMs = 0;
bool     i2sOk = false;

// ============================================================
// 同期プローブ（Step1 v1.1と同一）
// ============================================================
int alignmentProbe() {
  uint32_t t0 = millis();
  int got = 0, nz = 0, fitA = 0, fitB = 0;
  while (got < PROBE_WORDS && millis() - t0 < 400) {
    if (i2s.available() >= 4) {
      int32_t w;
      if (i2s.read(&w, true) == 0) break;
      got++;
      if (w != 0) {
        nz++;
        if (fitsModel((uint32_t)w, MODEL_A)) fitA++;
        if (fitsModel((uint32_t)w, MODEL_B)) fitB++;
      }
    }
  }
  if (got & 1) { int32_t w; if (i2s.available() >= 4) i2s.read(&w, true); }
  if (got < PROBE_WORDS / 2) return -2;
  if (nz  < PROBE_MIN_NONZERO) return -1;
  if ((nz - fitA) * 1000 <= nz * BAD_PERMILLE) return 2;
  if ((nz - fitB) * 1000 <= nz * BAD_PERMILLE) return 1;
  return 0;
}

bool tryAlign() {
  for (alignAttempts = 1; alignAttempts <= ALIGN_MAX_ATTEMPTS; alignAttempts++) {
    uint32_t t0 = millis();
    while (millis() - t0 < 20) {
      while (i2s.available() >= 4) { int32_t w; i2s.read(&w, true); }
    }
    int r = alignmentProbe();
    if (r == 2) { fmtModel = MODEL_A; alignVerified = true;  return true; }
    if (r == 1) { fmtModel = MODEL_B; alignVerified = true;  return true; }
    if (r == -1){ fmtModel = MODEL_A; alignVerified = false; return true; }
    if (r == -2){ Serial.println(F("[FAIL] クロック未到達。配線・基板電源を確認。")); return false; }
    i2s.end();
    delayMicroseconds(700 + (micros() % 1300));
    if (!i2s.begin(FS_HZ)) { Serial.println(F("[FAIL] 再begin失敗。リセットしてください。")); return false; }
  }
  Serial.println(F("[FAIL] 25回試行しても同期不成立。ロジアナで波形確認を。"));
  return false;
}

// ============================================================
// 送信（UART0＋USBエコーを必ず同一関数経由にする＝内容の乖離を構造的に防止）
// ============================================================
void txLine(const char* line, bool echo) {
  Serial1.print(line);
  Serial1.print('\n');
  if (echo) {
    Serial.print(F("TX> "));
    Serial.println(line);
  }
}

void sendFft() {
  char buf[48];
  snprintf(buf, sizeof(buf), "FFT:%u,%u,%u,%u,%u,%u,%u,%u",
           band[0], band[1], band[2], band[3], band[4], band[5], band[6], band[7]);
  bool echo = (ECHO_EVERY_N <= 1) || (++fftEchoSkip % ECHO_EVERY_N == 0);
  txLine(buf, echo);
  fftSentCount++;
}

// ============================================================
// FFTフレーム処理（512サンプル貯まるごとに呼ばれる：約31回/秒）
// ============================================================
void processFrame() {
  // RMS（ゲート用）を窓掛け前の生データで計算
  double sumsq = 0;
  for (int i = 0; i < FFT_N; i++) { sumsq += (double)fftBuf[i] * fftBuf[i]; }
  float rms = sqrt(sumsq / FFT_N);
  frameRmsDb = (rms > 1) ? 20.0f * log10f(rms / FS24) : -120.0f;

  // FFT
  for (int i = 0; i < FFT_N; i++) { vReal[i] = fftBuf[i]; vImag[i] = 0; }
  FFT.dcRemoval();
  FFT.windowing(FFTWindow::Hann, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();   // 結果は vReal[0..FFT_N/2]

  // 8バンド化：バンド内の平均振幅 → dBFS → 0-100
  // 正規化基準：フルスケール正弦のHann窓FFTピーク ≈ FS24 * N/4
  const float REF = FS24 * (FFT_N / 4.0f);
  for (int b = 0; b < 8; b++) {
    float acc = 0; int cnt = 0;
    for (int i = BAND_EDGE[b]; i < BAND_EDGE[b + 1]; i++) { acc += vReal[i]; cnt++; }
    float avg = (cnt > 0) ? acc / cnt : 0;
    float db  = (avg > 1e-3f) ? 20.0f * log10f(avg / REF) : -120.0f;
    float lvl = (db - FFT_FLOOR_DB) * 100.0f / (FFT_TOP_DB - FFT_FLOOR_DB);
    band[b] = (uint8_t)constrain((int)lvl, 0, 100);
  }
  bandsValid = true;

  // ---- 簡易レベルゲート（ヒステリシス＋ハングオーバ） ----
  uint32_t now = millis();
  if (frameRmsDb >= GATE_OPEN_DB) { gateLastLoudMs = now; }
  if (!gateOpen && frameRmsDb >= GATE_OPEN_DB) {
    gateOpen = true;
    lastHeartbeatMs = now;
    txLine("SPEAK_START", true);
  } else if (gateOpen) {
    bool quietLongEnough = (frameRmsDb < GATE_CLOSE_DB) && (now - gateLastLoudMs >= GATE_HANG_MS);
    if (quietLongEnough) {
      gateOpen = false;
      txLine("SPEAK_STOP", true);
    } else if (now - lastHeartbeatMs >= SPEAK_HEARTBEAT_MS) {
      lastHeartbeatMs = now;
      txLine("SPEAK_START", true);   // ハートビート（既存CoreS3仕様と同じ）
    }
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }

  Serial.println(F("============================================================"));
  Serial.println(F("Karipom Ear v2 - Step 2 UART Echo v1.0 (build " __DATE__ " " __TIME__ ")"));
  Serial.println(F("I2S    : Step1 v1.1と同一（setSlave/GP2=DATA/GP3=BCLK/GP4=LRCK/32bit）"));
  Serial.print (F("DSP    : ")); Serial.print(FS_HZ); Serial.print(F("Hz -> /"));
  Serial.print (DECIM_N); Serial.print(F(" = ")); Serial.print(FS_DECIM);
  Serial.println(F("Hz, FFT512(Hann), 8バンド(オクターブ割り31..8000Hz)"));
  Serial.print (F("GATE   : open ")); Serial.print(GATE_OPEN_DB);
  Serial.print (F("dBFS / close ")); Serial.print(GATE_CLOSE_DB);
  Serial.print (F("dBFS / hang ")); Serial.print(GATE_HANG_MS); Serial.println(F("ms"));
  Serial.println(F("UART0  : Serial1 GP0(TX)/GP1(RX) 115200 8N1  ※CoreS3未接続でよい"));
  Serial.println(F("ECHO   : 送信全行を『TX> 』付きでUSBへ表示"));
  Serial.println(F("LOOPBK : GP0-GP1をジャンパ直結すると lb=OK 表示（任意の自己試験）"));
  Serial.println(F("============================================================"));

  // UART0（送信が主。RXはループバック検出専用）
  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial1.begin(115200);

  bool ok = true;
  ok &= i2s.setSlave();
  ok &= i2s.setDATA(PIN_DIN);
  ok &= i2s.setBitsPerSample(BITS);
  ok &= i2s.setBuffers(BUF_COUNT, BUF_WORDS);
  if (!ok) Serial.println(F("[FAIL] I2S設定拒否（呼び出し順を確認）"));

  i2sOk = ok && i2s.begin(FS_HZ);
  if (!i2sOk) { Serial.println(F("[FAIL] I2S begin失敗")); return; }
  Serial.println(F("[ OK ] I2S begin。同期プローブ開始..."));

  i2sOk = tryAlign();
  if (i2sOk) {
    Serial.print(F("[ OK ] ALIGN: attempt=")); Serial.print(alignAttempts);
    Serial.print(F(" fmt=")); Serial.print(fmtModel == MODEL_A ? F("A") : F("B"));
    Serial.println(alignVerified ? F(" 判定=確定") : F(" 判定=暫定（無音）"));
    Serial.println(F("ST> 行: rmsDb gate fft/s n mis ovf lb"));
  }
  lastReportMs = lastDataMs = millis();
}

void loop() {
  if (!i2sOk) {
    static uint32_t lastErr = 0;
    if (millis() - lastErr >= 2000) { lastErr = millis(); Serial.println(F("[FAIL] 未初期化のまま。リセット要。")); }
    return;
  }

  // ---- I2S受信 → デシメーション → FFTバッファ ----
  while (i2s.available() >= 8) {
    int32_t rawL, rawR;
    if (!i2s.read32(&rawL, &rawR)) break;
    if (rawL != 0 && !fitsModel((uint32_t)rawL, fmtModel)) misCount++;
    if (rawR != 0 && !fitsModel((uint32_t)rawR, fmtModel)) misCount++;
    int32_t mono = (toS24(rawL) >> 1) + (toS24(rawR) >> 1);  // (L+R)/2（オーバーフロー安全）
    sampleCount++;
    lastDataMs = millis();

    decimAcc += mono;
    if (++decimCnt >= DECIM_N) {
      float s = (float)decimAcc / DECIM_N;   // 6点平均＝簡易LPF＋間引き
      decimAcc = 0; decimCnt = 0;
      fftBuf[fftFill++] = s;
      if (fftFill >= FFT_N) {
        fftFill = 0;
        processFrame();
      }
    }
  }

  // ---- FFT行送信（25Hz・最新バンドを送る） ----
  uint32_t now = millis();
  if (bandsValid && now - lastFftSendMs >= FFT_SEND_MS) {
    lastFftSendMs = now;
    sendFft();
  }

  // ---- ループバック検出（GP0-GP1直結時のみ増える） ----
  while (Serial1.available()) { Serial1.read(); lbRxBytes++; lbSeen = true; }

  // ---- オーバーフロー（立上り計数） ----
  static bool prevOvf = false;
  bool ovf = i2s.getOverflow();
  if (ovf && !prevOvf) overflowCount++;
  prevOvf = ovf;

  // ---- 1秒ステータス行（プロトコル行と区別できる『ST>』プレフィクス） ----
  if (now - lastReportMs >= REPORT_MS) {
    lastReportMs = now;
    if (sampleCount == 0) {
      noDataPeriods++;
      Serial.println(F("[NG ] 1秒間 受信0サンプル → Step1の配線確認へ戻る"));
    } else {
      Serial.print(F("ST> rmsDb=")); Serial.print(frameRmsDb, 1);
      Serial.print(F(" gate=")); Serial.print(gateOpen ? F("OPEN") : F("closed"));
      Serial.print(F(" fft/s=")); Serial.print(fftSentCount);
      Serial.print(F(" n=")); Serial.print(sampleCount);
      Serial.print(F(" mis=")); Serial.print(misCount);
      Serial.print(F(" ovf=")); Serial.print(overflowCount);
      Serial.print(F(" lb=")); Serial.println(lbSeen ? (lbRxBytes > 0 ? "OK" : "--") : "--");
      if (misCount > sampleCount / 50) Serial.println(F("[WARN] mis多発＝境界ずれ再発。リセット要"));
      if (sampleCount < (uint32_t)(FS_HZ * 0.9)) Serial.println(F("[WARN] n低下：loop負荷かクロック異常"));
    }
    sampleCount = 0; misCount = 0; fftSentCount = 0; lbRxBytes = 0;
  }

  if (now - lastDataMs >= NO_DATA_TIMEOUT_MS) {
    lastDataMs = now;
    Serial.println(F("[TIMEOUT] 2秒以上サンプルなし。"));
  }
}
