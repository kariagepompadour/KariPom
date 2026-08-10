#!/usr/bin/env python3
# =============================================================================
# karipom_companion_v4.py
# KariPom Companion v4 — 1ウィンドウ4タブ統合版
#
# 統合元:
#   - karipom_companion_v2.py          (PyQt5 GUI: ログ取得・健康診断・サマリー等)
#   - karipom_talk_20260719_multiOS.py (PC音声連携: Loopback取得→FFT解析→UDP送信)
#
# v4正式候補の基本方針:
#   - 1ウィンドウ4タブ: KariPom Lab / Talk / Log・健康診断 / KariPom
#   - KariPom Labを初期表示。KariPom顔は最後のPC単独お試し/モニタータブ。
#   - PC音声解析はIP未設定でも常時利用でき、KariPom顔はCoreS3無しでも口パクする。
#   - IP設定時のみ同じ発話状態・FFTをCoreS3へUDP送信する。
#
# 統合方針:
#   - Companionの既存機能（GUI・ログ解析・健康診断・保存・検索・ログ管理等）は無変更。
#   - Talkの音声取得・FFT解析・UDP送信ロジックは実機確認済みの既存実装をそのまま維持し、
#     モジュールインポート時に自動実行されないよう run_talk_mode() 関数にまとめた。
#     （音声ライブラリ(numpy/sounddevice/soundcard)のimportもrun_talk_mode()内に遅延させ、
#       未導入環境でもCompanion GUIが問題なく起動できるようにしている）
#   - karipom_config.json のIP設定を "ip" キーに一本化。旧Talk形式の "m5_ip" は
#     初回読込時に自動移行し、他の設定値（port / chunk_lines 等）は上書きしない。
#   - karipom_config.json の正式な保存先は、本スクリプト(karipom_companion_v3.py)と
#     同じディレクトリ(SCRIPT_DIR)。プログラム一式（v3 + config）をフォルダごと
#     別PCへコピーする運用に統一するため。旧Companion(v2)が使っていた
#     ~/Documents/KariPom/karipom_config.json がSCRIPT_DIR側に無い場合のみ、
#     初回読込時にその内容を一度だけ引き継ぐ（以降はSCRIPT_DIR側が正式な保存先）。
#     ログ・サマリー・チャンク等の保存先(~/Documents/KariPom)は従来どおり変更しない。
#   - ポートは用途が異なるため共通化しない:
#       Companion HTTP : cfg["port"]（デフォルト80、既存のまま）
#       Talk    UDP    : M5_PORT = 12345（既存のまま固定）
#   - Companion GUI起動時、設定済みのCoreS3 IPでTalkを別プロセスとして自動起動する
#     （IPをGUIとTalkで二重入力させないため）。GUI上の「Talk：動作中/停止中」表示から
#     Talkだけを停止・再開でき、Companion終了時はTalkも自動的に停止する。
#     Talk本体のロジック(run_talk_mode()以下)には手を入れず、プロセスの起動・停止のみを
#     MainWindow側から管理する構成にしている。
#   - Companion GUIで「保存」を押すと、動作中のTalkもIP_RELOAD_INTERVAL秒以内に
#     karipom_config.jsonの"ip"を再読込し、UDP送信先をその場で切り替える
#     （_maybe_reload_ip()。停止→開始は不要）。IPの参照元はkaripom_config.jsonの
#     "ip"一本のままで、Talk側のM5_IPはその値を定期的に反映するだけの変数。
#
# 起動方法:
#   python karipom_companion_v3.py             → Companion GUI（設定済みIPでTalkも自動起動）
#   python karipom_companion_v3.py talk [opts]  → Talk単独起動（デバッグ・実機確認用。従来どおり
#                                                  IP入力プロンプトあり）
#       opts: --list-devices / --input <名前の一部> / --threshold <値> /
#             --no-keepalive / --fft / --no-fft / --fft-send / --no-fft-send /
#             --ip <値>（Companion GUIが自動起動時に使用する内部オプション。CLIから
#                       手動起動する場合は通常指定不要）
# =============================================================================

import sys, json, os, re, threading, traceback, subprocess, webbrowser, random, math
import socket, platform, time, signal
import requests
from datetime import datetime
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QTextEdit, QSplitter,
    QGroupBox, QStatusBar, QMessageBox, QFrame, QTabWidget
)
from PyQt5.QtCore import Qt, QThread, pyqtSignal, QTimer, QUrl, QPoint
from PyQt5.QtGui import QFont, QTextCursor, QColor, QPainter, QPen, QBrush

# KariPom LabをCompanion内タブへ埋め込む。PyQtWebEngineが無い環境でも
# Companion本体は起動できるよう、オプション依存として扱う。
try:
    from PyQt5.QtWebEngineWidgets import QWebEngineView
    WEBENGINE_AVAILABLE = True
except Exception:
    QWebEngineView = None
    WEBENGINE_AVAILABLE = False

# karipom_config.json の正式な保存先: v3スクリプト自身と同じディレクトリ。
# （プログラム一式をフォルダごと別PCへコピーする運用に合わせるため。macOS/Windows/Linux共通）
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
SCRIPT_PATH = os.path.abspath(__file__)  # Talk自動起動時に子プロセスへ渡す自スクリプトパス
CONFIG_PATH = os.path.join(SCRIPT_DIR, "karipom_config.json")

# ログ・サマリー・チャンク等の既存保存先（変更しない）。
BASE_DIR          = os.path.expanduser("~/Documents/KariPom")
# 旧Companion(v2)が使っていたconfigの場所。SCRIPT_DIR側にまだ設定ファイルが無い場合の
# 一度限りのフォールバック移行にのみ使用する（ログ等の保存先そのものは変更しない）。
LEGACY_CONFIG_PATH = os.path.join(BASE_DIR, "karipom_config.json")
CURRENT_LOG_PATH  = os.path.join(BASE_DIR, "karipom_current.log")
PREVIOUS_LOG_PATH = os.path.join(BASE_DIR, "karipom_previous.log")
SUMMARY_PATH      = os.path.join(BASE_DIR, "karipom_summary.txt")
CHUNK_SIZE        = 230
MIN_ANALYZABLE_LINES = 15  # これ未満の行数はLog Clear直後/Compress直後/新規ログとみなし解析を打ち切る

# "ip"  : CoreS3のIPアドレス（Companion HTTP・Talk UDPの両方が共通で参照する）
# "port": Companion HTTP通信のポート（デフォルト80、既存のまま）
# 旧Talk単体版が使っていた "m5_ip" は load_config() 内で "ip" へ自動移行する。
DEFAULT_CONFIG = {"ip": "", "port": 80, "chunk_lines": CHUNK_SIZE}

COUNT_KEYWORDS = {
    "Boot":                r"\bBoot\b|boot complete|BOOT",
    "ERROR":               r"\bERROR\b",
    "Guru Meditation":     r"Guru Meditation",
    "PANIC":               r"\bPANIC\b",
    "REBOOT":              r"\bREBOOT\b",
    "WDT":                 r"\bWDT\b",
    "MIC SPEAK START":     r"MIC SPEAK START",
    "MIC SPEAK STOP":      r"MIC SPEAK STOP",
    "MIC START CONFIRMED": r"MIC START CONFIRMED",
    "weak_short":          r"weak_short",
    "spike_cooldown":      r"spike_cooldown|MIC SPIKE IGNORED",
    "servo":               r"\bservo\b",
    "detach":              r"\bdetach\b",
    "mutter":              r"\bmutter\b",
    "PLAY WAV":            r"PLAY WAV",
    "RTC":                 r"\bRTC\b",
    "HEAP":                r"\bHEAP\b",
    "LR SERVO AUTO DETACH":r"LR SERVO AUTO DETACH",
    "IDLE SERVO":          r"IDLE SERVO",
    "suppress":            r"suppress|suppressMicStart",
    "STATE HEARTBEAT":     r"STATE HEARTBEAT",
}

IMPORTANT_PATTERNS = [
    ("Guru",        r"Guru Meditation.*"),
    ("ERROR",       r".*\bERROR\b.*"),
    ("PANIC",       r".*\bPANIC\b.*"),
    ("REBOOT",      r".*\bREBOOT\b.*"),
    ("WDT",         r".*\bWDT\b.*"),
    ("MIC START",   r".*MIC START CONFIRMED.*"),
    ("MIC STOP",    r".*MIC SPEAK STOP.*"),
    ("PLAY WAV",    r".*PLAY WAV.*"),
    ("MUTTER",      r".*mutter.*"),
    ("RTC",         r".*(?:RTC WRITE|LOCALTIME AFTER RTC WRITE|RTC TIME OK|RTC TIME INVALID|RTC settimeofday|RTC getLocalTime|RTC RAW|RTC UPDATED|using RTC).*"),
    ("HEAP",        r".*\bHEAP\b.*"),
    ("AUTO DETACH", r".*AUTO DETACH.*"),
    ("SUPPRESS",    r".*suppress.*"),
    ("Boot",        r".*[Bb]oot complete.*|.*BOOT.*"),
]

# ─── 電源・リセット診断ロジック（stable_2.3 対応） ─────
#
# 実機ログで確認された形式（2回目のフィードバックで実ログ確認済み）:
#   BOOT_RESET_POWERON (1)                        … 今回起動のリセット理由。名前+括弧内番号。
#   BOOT_RESET_USB (11) / BOOT_RESET_WDT (..) / BOOT_RESET_PANIC (..) など
#   PREV RESET REASON = 11                        … 前回起動時に記録されたリセット理由（数値のみ）
#   PWR BOOT IRQ48=0x10 IRQ49=0x03 IRQ4A=0x00 STAT00=0x28 STAT01=0x14   … 起動ブロックの起点の一つ
#   PWR BOOT CAUSE BATFET_OCP BAT_REMOVE VBUS_REMOVE SOC_WARN_LV2 vbus=1 bat=1 chg=CHARGE
#       … CAUSEコードは複数を空白区切りで列挙。'vbus='以降は状態値でCAUSEには含めない。
#   PWR BOOT INFO CHG_START BAT_INSERT VBUS_INSERT GAUGE_NEW_SOC
#       … INFOコードも複数・空白区切り。
#   PWR BOOT IRQEN want=80/54/64 got=FF/FC/7F result=OK lib=0   … 参考情報・現状未使用
#   PWR SNAP vbus=1 bat=1 chg=1 soc=87 vbat=4021 vbus_mv=5083 vmin=3984 vmax=4040 loops=... load=...
#       … 定期スナップショット。vbus/bat/chgは状態コード、soc=Battery%、vbat=現在電圧、
#          vbus_mv=VBUS電圧、vmin/vmax=最低・最高電圧（mV）。
#   PWR VBUS LOST / PWR VBUS OK                   … VBUS切断・復帰イベント
#   PWR BAT REMOVED / PWR BAT PRESENT             … バッテリー切断・認識イベント
#   PWR VSAG ...                                  … 電圧降下(Vsag)検出イベント
#
# 起動をまたいだ情報混在の防止:
#   ログに複数回起動分が含まれる場合、RESET/CAUSE/INFOを単純に「最後の1件」ずつ拾うと
#   異なる起動の情報が混ざる恐れがあるため、"BOOT_RESET_..." 行または "PWR BOOT IRQ48=" 行を
#   起動ブロックの起点とみなしてログを分割し、直近の起動ブロックの範囲内からまとめて取得する。

CAUSE_JP = {
    "BATFET_OCP":    "バッテリー保護回路（BATFET）の過電流保護",
    "VBUS_REMOVE":   "USB給電喪失",
    "BAT_REMOVE":    "バッテリー切断",
    "THERMAL":       "PMIC温度保護",
    "VBUS_OCP":      "USB給電の過電流保護",
    "VBUS_OVP":      "USB給電の過電圧保護",
    "BAT_OVP":       "バッテリー過電圧保護",
    "VSYS_OVP":      "システム電圧(VSYS)の過電圧保護",
    "VSYS_UVP":      "システム電圧(VSYS)の低電圧保護",
    "CHG_OVER_TEMP": "充電時の温度異常保護",
    "SOFT_RESET":    "ソフトウェアリセット",
    "CHIP_RESET":    "PMICチップリセット",
    "POWERON":       "通常の電源投入",
    "SOC_WARN_LV1":  "バッテリー残量低下警告（レベル1）",
    "SOC_WARN_LV2":  "バッテリー残量低下警告（レベル2）",
    "SOC_WARN_LV3":  "バッテリー残量低下警告（レベル3）",
    "NONE":          "特記事項なし",
}

INFO_JP = {
    "GAUGE_NEW_SOC": "残量ゲージ更新通知",
    "CHG_START":     "充電開始",
    "CHG_DONE":      "充電完了",
    "CHG_STOP":      "充電停止",
    "BAT_INSERT":    "バッテリー認識",
    "BAT_REMOVE":    "バッテリー切断検知",
    "VBUS_INSERT":   "USB給電認識",
    "VBUS_REMOVE":   "USB給電切断検知",
    "PEK_POS_EDGE":  "電源ボタン押下",
    "PEK_NEG_EDGE":  "電源ボタン解放",
    "PEK_LONG":      "電源ボタン長押し",
    "PEK_SHORT":     "電源ボタン短押し",
    "GAUGE_LOW_SOC": "バッテリー残量低下通知",
}

# 電源関連イベント（サーボ等の既存 IMPORTANT_PATTERNS とは別カテゴリで表示する）
POWER_EVENT_PATTERNS = [
    ("PWR VBUS", r".*PWR VBUS\b.*"),
    ("PWR BAT",  r".*PWR BAT\b.*"),
    ("PWR VSAG", r".*PWR VSAG\b.*"),
]

# RESET理由: "BOOT_RESET_POWERON (1)" のように名前+括弧内番号で記録される。
_RESET_LINE_RE = re.compile(r"\bBOOT_RESET_([A-Z0-9_]+)\s*\((\d+)\)")
# PREV RESET REASONは "PREV RESET REASON = 11" のように '=' の後ろに数値のみが続く。
# '=' 自体を値として拾わないよう、'=' はオプションの区切り文字として明示的に読み飛ばす。
_PREV_RESET_RE = re.compile(r"PREV RESET REASON\s*[:=]?\s*(-?\d+)")
_BOOT_CAUSE_LINE_RE = re.compile(r"PWR BOOT CAUSE\s+(.*)")
_BOOT_INFO_LINE_RE  = re.compile(r"PWR BOOT INFO\s+(.*)")
# 起動ブロックの起点: "BOOT_RESET_..." 行、または "PWR BOOT IRQ48=" 行。
_BOOT_BLOCK_MARKER_RE = re.compile(r"BOOT_RESET_[A-Z0-9_]+\s*\(\d+\)|PWR BOOT IRQ48=")


def _extract_codes(rest):
    """CAUSE/INFO行の残り部分から、'key=value' 形式の状態値が始まる前までを
    コード一覧として取り出す（例: "BATFET_OCP BAT_REMOVE vbus=1 bat=1" → ["BATFET_OCP","BAT_REMOVE"]）。"""
    codes = []
    for tok in rest.split():
        if "=" in tok:
            break
        codes.append(tok)
    return codes


def _split_boot_blocks(log_text):
    """BOOT_RESET_ 行 または PWR BOOT IRQ48= 行を起動ブロックの起点とみなし、
    ログを起動単位のブロックに分割する（起動をまたいだ情報混在を防ぐため）。
    両行は同じ起動の冒頭で数行以内に連続して現れる想定のため、近接する起点候補は
    最初のものだけを境界として採用しクラスタリングする。"""
    lines = log_text.splitlines()
    boundaries = [i for i, l in enumerate(lines) if _BOOT_BLOCK_MARKER_RE.search(l)]
    merged = []
    for idx in boundaries:
        if merged and idx - merged[-1] <= 5:
            continue
        merged.append(idx)
    if not merged:
        return [log_text]  # 起動ブロックの目印が見つからない場合は全体を1ブロックとして扱う
    blocks = []
    for i, start in enumerate(merged):
        end = merged[i + 1] if i + 1 < len(merged) else len(lines)
        blocks.append("\n".join(lines[start:end]))
    return blocks


def _build_reset_code_map(log_text):
    """"BOOT_RESET_NAME (番号)" の組をログ全体から集め、番号→名称の対応表を作る。
    PREV RESET REASON が数値のみで記録される場合に、名称へ変換して表示するために使う。"""
    return {num: name for name, num in _RESET_LINE_RE.findall(log_text)}


def parse_power_diagnosis(log_text):
    """電源・リセット診断情報を抽出する（RESET / PREV RESET REASON / PWR BOOT CAUSE・INFO）。
    複数回起動分のログが混在していても、直近の起動ブロックの範囲内からまとめて取得することで、
    起動をまたいだ情報混在を防ぐ。"""
    blocks = _split_boot_blocks(log_text)
    last_block = blocks[-1]

    reset_matches = list(_RESET_LINE_RE.finditer(last_block))
    if reset_matches:
        rm = reset_matches[-1]
        reset_reason = f"BOOT_RESET_{rm.group(1)} ({rm.group(2)})"
    else:
        reset_reason = None

    prev_matches = list(_PREV_RESET_RE.finditer(last_block)) or list(_PREV_RESET_RE.finditer(log_text))
    prev_reset_reason = None
    if prev_matches:
        prev_num = prev_matches[-1].group(1)
        name = _build_reset_code_map(log_text).get(prev_num)
        prev_reset_reason = f"BOOT_RESET_{name} ({prev_num})" if name else f"コード {prev_num}"

    cause_matches = list(_BOOT_CAUSE_LINE_RE.finditer(last_block))
    boot_cause = _extract_codes(cause_matches[-1].group(1)) if cause_matches else []

    info_matches = list(_BOOT_INFO_LINE_RE.finditer(last_block))
    boot_info = _extract_codes(info_matches[-1].group(1)) if info_matches else []

    return {
        "reset_reason":      reset_reason,
        "prev_reset_reason": prev_reset_reason,
        "boot_cause":        boot_cause,  # 常にリスト（複数CAUSEに対応）
        "boot_info":         boot_info,   # 常にリスト
    }


def _snap_state(value, mapping):
    if value is None:
        return None
    return mapping.get(value, value)


_VBUS_STATE_JP = {"1": "ON", "0": "OFF"}
_BAT_STATE_JP  = {"1": "認識あり", "0": "認識なし"}
_CHG_STATE_JP  = {"1": "充電中", "0": "待機", "-1": "放電中"}


def parse_pwr_snap(log_text):
    """定期出力される PWR SNAP 行を集計する。
    実ログの形式: PWR SNAP vbus=1 bat=1 chg=1 soc=87 vbat=4021 vbus_mv=5083 vmin=3984 vmax=4040 ...
    直近（最後）のスナップショットの状態を採用する。vmin/vmax は起動後の実測レンジとして
    ファームウェア側が更新し続けている値をそのまま表示する。"""
    snaps = []
    for m in re.finditer(r"PWR SNAP\b(.*)", log_text):
        kv = dict(re.findall(r"(\w+)\s*=\s*(-?[\w.]+)", m.group(1)))
        if kv:
            snaps.append(kv)
    if not snaps:
        return None

    def get(d, key):
        for k in d:
            if k.lower() == key:
                return d[k]
        return None

    last = snaps[-1]
    return {
        "count":       len(snaps),
        "battery_pct": get(last, "soc"),
        "vbus_state":  _snap_state(get(last, "vbus"), _VBUS_STATE_JP),
        "bat_state":   _snap_state(get(last, "bat"), _BAT_STATE_JP),
        "chg_state":   _snap_state(get(last, "chg"), _CHG_STATE_JP),
        "vbat":        get(last, "vbat"),
        "vbus_mv":     get(last, "vbus_mv"),
        "vmin":        get(last, "vmin"),
        "vmax":        get(last, "vmax"),
    }


def extract_power_events(log_text):
    """PWR VBUS / PWR BAT / PWR VSAG を、Servoなど既存の重要イベントとは別枠で抽出する。"""
    events = []
    for line in log_text.splitlines():
        for label, pat in POWER_EVENT_PATTERNS:
            if re.search(pat, line, re.IGNORECASE):
                events.append((label, line.strip()))
                break
    return events


def make_power_reset_block(power):
    out = ["■ 電源・リセット診断", "-"*40]
    out.append(f"RESET          : {power.get('reset_reason') or '不明'}")
    prev = power.get("prev_reset_reason")
    out.append(f"PREV RESET     : {prev if prev else 'なし'}")

    causes = power.get("boot_cause") or []
    info   = power.get("boot_info") or []
    real_causes = [c for c in causes if c.upper() != "NONE"]

    if causes or info:
        out += ["", "AXP2101"]
        if causes:
            jp_list = " / ".join(CAUSE_JP.get(c, f"{c}（不明なCAUSEコード）") for c in causes)
            out.append(f"CAUSE          : {' '.join(causes)}  ({jp_list})")
        if info:
            info_disp = "  ".join(f"{tok}({INFO_JP.get(tok, '不明なINFOコード')})" for tok in info)
            out.append(f"INFO           : {info_disp}")

    out += ["", "判定"]
    judge = []
    for c in real_causes:
        jp = CAUSE_JP.get(c, f"{c}（未登録のCAUSEコード）")
        judge.append(f"・{jp}が記録されています")
    if "VBUS_INSERT" in info:
        judge.append("・VBUSは認識されています")
    if "BAT_INSERT" in info:
        judge.append("・Batteryは認識されています")
    if "VBUS_REMOVE" in info:
        judge.append("・VBUS切断が記録されています")
    if "BAT_REMOVE" in info:
        judge.append("・Battery切断が記録されています")
    if not judge:
        judge.append("・特記すべき電源異常は記録されていません")
    out += judge
    return "\n".join(out)


def make_pwr_snap_block(snap):
    out = ["■ PWR SNAP 集計", "-"*40]
    if not snap:
        out.append("PWR SNAPは記録されていません。")
        return "\n".join(out)

    def mv(v):
        return f"{v}mV" if v is not None else "不明"

    out.append(f"サンプル数     : {snap['count']} 件")
    out.append(f"Battery(SOC)   : {snap['battery_pct'] + '%' if snap['battery_pct'] is not None else '不明'}")
    out.append(f"VBUS状態       : {snap['vbus_state'] or '不明'}")
    out.append(f"Battery認識    : {snap['bat_state'] or '不明'}")
    out.append(f"Charge状態     : {snap['chg_state'] or '不明'}")
    out.append(f"現在電圧(vbat) : {mv(snap['vbat'])}")
    out.append(f"VBUS電圧       : {mv(snap['vbus_mv'])}")
    out.append(f"最低電圧       : {mv(snap['vmin'])}")
    out.append(f"最大電圧       : {mv(snap['vmax'])}")
    return "\n".join(out)


def make_power_events_block(events):
    out = ["■ 電源イベント抽出（PWR VBUS / PWR BAT / PWR VSAG）", "-"*40]
    if events:
        for label, line in events:
            out.append(f"  [{label:<8}] {line[:100]}")
    else:
        out.append("  （電源イベントなし）")
    return "\n".join(out)


def make_power_diagnosis_summary(power, events):
    out = ["■ 電源診断まとめ", "-"*40]
    causes = [c for c in (power.get("boot_cause") or []) if c.upper() != "NONE"]
    if causes:
        primary = causes[0]
        jp = CAUSE_JP.get(primary, f"{primary}（不明なCAUSE）")
        if len(causes) > 1:
            others = " / ".join(CAUSE_JP.get(c, c) for c in causes[1:])
            out.append(f"・電源断原因として{jp}が最も疑われます（他に {others} も記録されています）。")
        else:
            out.append(f"・電源断原因として{jp}が最も疑われます。")
    else:
        out.append("・電源断原因となるCAUSEは記録されていません（通常起動と考えられます）。")

    # VBUS異常: 実ログでは "PWR VBUS LOST"（切断）/ "PWR VBUS OK"（復帰）で記録される。
    # 旧仕様の "REMOVE" 表記にも念のため対応しておく。
    vbus_bad_re = re.compile(r"\b(LOST|REMOVE)\b", re.IGNORECASE)
    vbus_bad_n = sum(1 for label, line in events if label == "PWR VBUS" and vbus_bad_re.search(line))
    if vbus_bad_n:
        out.append(f"・VBUS異常（切断）が{vbus_bad_n}件検出されました。")
    else:
        out.append("・VBUS異常は検出されませんでした。")

    vsag_n = sum(1 for label, line in events if label == "PWR VSAG")
    if vsag_n:
        out.append(f"・低電圧(VSAG)イベントが{vsag_n}件検出されました。")
    else:
        out.append("・低電圧イベントは確認されませんでした。")
    return "\n".join(out)


def make_power_report(power, snap, events):
    """①〜⑥をまとめた電源診断レポート全体を組み立てる。"""
    return "\n\n".join([
        make_power_reset_block(power),
        make_pwr_snap_block(snap),
        make_power_events_block(events),
        make_power_diagnosis_summary(power, events),
    ])

# ─── ユーティリティ ───────────────────────────

def ensure_base_dir():
    os.makedirs(BASE_DIR, exist_ok=True)

def load_config():
    """karipom_config.json を読み込む。

    正式な保存先は v3スクリプトと同じディレクトリ(CONFIG_PATH = SCRIPT_DIR)。
    そこにまだ設定ファイルが無く、旧Companion(v2)が使っていた
    ~/Documents/KariPom/karipom_config.json (LEGACY_CONFIG_PATH) が存在する場合は、
    その内容を一度だけ読み込んでSCRIPT_DIR側へ書き出す（一度限りのフォールバック移行）。
    以降はCONFIG_PATH（SCRIPT_DIR側）が正式な保存先になる。
    ログ・サマリー・チャンク等の保存先(BASE_DIR)はこの移行の対象外で、変更しない。

    旧Talk単体版が使っていた "m5_ip" キーが残っている場合、"ip" が未設定であれば
    その値を "ip" へ引き継いだ上で "m5_ip" は削除する（IP設定を "ip" に一本化するため）。
    それ以外の既存キー（port / chunk_lines 等）はそのまま保持する。"""
    ensure_base_dir()
    cfg = dict(DEFAULT_CONFIG)

    # CONFIG_PATH(SCRIPT_DIR側)が無く、旧場所(LEGACY_CONFIG_PATH)にだけ設定がある場合は
    # そちらを読み込み元にする（一度限りのフォールバック移行）。
    source_path = CONFIG_PATH
    migrate_from_legacy = False
    if not os.path.exists(CONFIG_PATH) and os.path.exists(LEGACY_CONFIG_PATH):
        source_path = LEGACY_CONFIG_PATH
        migrate_from_legacy = True

    if os.path.exists(source_path):
        try:
            with open(source_path, "r", encoding="utf-8") as f:
                loaded = json.load(f)
            if isinstance(loaded, dict):
                cfg.update(loaded)
        except Exception:
            pass

    if "m5_ip" in cfg:
        if not cfg.get("ip"):
            cfg["ip"] = cfg["m5_ip"]
        del cfg["m5_ip"]

    for k, v in DEFAULT_CONFIG.items():
        cfg.setdefault(k, v)

    if migrate_from_legacy:
        # 旧場所(~/Documents/KariPom)の設定をSCRIPT_DIR側へ一度だけ書き出す。
        # 失敗しても致命的ではないため（次回起動時に再度移行を試みるだけ）例外は握りつぶす。
        try:
            save_config(cfg)
        except Exception:
            pass

    return cfg

def save_config(cfg):
    """karipom_config.json (SCRIPT_DIR側、CONFIG_PATH) を書き込む。
    渡された辞書全体をそのまま書き出すだけで、IPだけを保存する目的で他の設定値
    （port / chunk_lines 等）を消してはならない。
    呼び出し側は load_config() で読み込んだ辞書を書き換えてから渡すこと。"""
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)


# ─── 健康診断ロジック ─────────────────────────
# ※ しきい値は簡易判定用の暫定値です。運用実績を見ながら調整してください。

HEALTH_ITEMS = ["Heap", "PSRAM", "RTC", "Servo", "WAV", "Boot", "NVS保存", "IDLE"]
MARK_OK, MARK_WARN, MARK_BAD, MARK_NA = "◎", "△", "×", "－"

# 診断の確信度が何を意味するかの一言補足（表示用）
CONFIDENCE_NOTES = {
    "高": "十分な判定材料が揃っています",
    "中": "一部の項目は判定材料が不足しています",
    "低": "判定材料が少なく、参考程度にご覧ください",
}

def _heap_values(log_text):
    # 健康診断の推移判定には「BOOT HEAP: free=」「HEAP MONITOR: free=」の値のみを使う。
    # 「PLAY WAV PRE: heap=」のような再生直前の一時値は変動が大きく誤判定を招くため対象外。
    return [int(v) for v in re.findall(
        r"(?:BOOT HEAP|HEAP MONITOR):\s*free=(\d+)", log_text, re.IGNORECASE)]

def analyze_health(log_text, next_bucket_hint=None):
    """既存のdo_kenben()集計とは独立に、生ログを軽くスキャンして
    KariPomの状態を◎/△/×で簡易判定する。
    next_bucket_hint: 時間帯別解析で、次の時間帯冒頭の数行を渡すと、
    末尾で途切れたWAV再生のEND確認に補足として使う（省略可）。"""
    lines = log_text.splitlines()
    total = len(lines)
    marks = {}
    notes = {}
    findings = []

    def has(pat):
        return re.search(pat, log_text, re.IGNORECASE) is not None

    def count(pat):
        rx = re.compile(pat, re.IGNORECASE)
        return sum(1 for l in lines if rx.search(l))

    # Heap: 開始〜終了の増減で判定
    heap_vals = _heap_values(log_text)
    if len(heap_vals) >= 2:
        start, end = heap_vals[0], heap_vals[-1]
        drop_ratio = (start - end) / start if start else 0
        if drop_ratio >= 0.5:
            marks["Heap"] = MARK_BAD
            findings.append(f"Heapが開始時から大きく低下しています（{start}→{end}）")
        elif drop_ratio >= 0.15:
            marks["Heap"] = MARK_WARN
            findings.append(f"Heapが継続的に減少しています（{start}→{end}）")
        else:
            marks["Heap"] = MARK_OK
    else:
        marks["Heap"] = MARK_NA

    # PSRAM: エラー言及の有無
    if has(r"PSRAM"):
        if has(r"PSRAM[^\n]*(fail|error|ERROR|エラー)"):
            marks["PSRAM"] = MARK_BAD
            findings.append("PSRAM関連のエラーが記録されています")
        else:
            marks["PSRAM"] = MARK_OK
    else:
        marks["PSRAM"] = MARK_NA

    # RTC: RTC WRITE INPUT → WRITE -> READ BACK → LOCALTIME AFTER RTC WRITE → NTP TIME OK -> RTC UPDATED
    # 同期処理の「開始記録」は RTC WRITE INPUT のみを根拠とする（"RTC "を必須にすることで、
    # 将来他機能に "WRITE INPUT" という文言が追加されても誤認しないようにする）。
    # 「[RTC] STATE HEARTBEAT」のようなRTCという文字を含むだけの行は開始記録として扱わない。
    rtc_started = has(r"RTC WRITE INPUT")
    rtc_fail = count(r"RTC[^\n]*(fail|error|ERROR|失敗)") + count(r"NTP[^\n]*(fail|error|ERROR|失敗)")
    if rtc_fail:
        marks["RTC"] = MARK_BAD
        findings.append("RTC同期に失敗しています")
    elif not rtc_started:
        marks["RTC"] = MARK_NA
    else:
        seq_ok = has(
            r"RTC WRITE INPUT[\s\S]{0,500}?"
            r"WRITE\s*->\s*READ BACK[\s\S]{0,500}?"
            r"LOCALTIME AFTER RTC WRITE[\s\S]{0,500}?"
            r"NTP TIME OK\s*->\s*RTC UPDATED"
        )
        if seq_ok:
            marks["RTC"] = MARK_OK
        else:
            marks["RTC"] = MARK_WARN
            findings.append("RTC同期シーケンス（WRITE INPUT→WRITE->READ BACK→LOCALTIME AFTER RTC WRITE→NTP TIME OK->RTC UPDATED）が完全には確認できません")

    # Servo: detach件数の比率
    detach_n = count(r"\bdetach\b")
    if total > 0 and has(r"\bservo\b"):
        ratio = detach_n / total
        if ratio >= 0.08:
            marks["Servo"] = MARK_WARN
            findings.append(f"Servo detachが通常より多い（{detach_n}件 / 全{total}行）")
        else:
            marks["Servo"] = MARK_OK
    else:
        marks["Servo"] = MARK_NA

    # WAV: PRE→PLAY→ENDが揃っているか
    # 実ログでは「PLAY WAV PRE」「PLAY WAV: <path>」「PLAY WAV END」の3種の行があり、
    # 実際の再生開始は「PLAY WAV:」のみ。PRE/ENDは開始件数に含めない。
    if has(r"WAV"):
        pre_n  = count(r"PLAY WAV PRE")
        play_n = count(r"PLAY WAV:")
        end_n  = count(r"PLAY WAV END")

        # 件数差だけでは「区間途中でENDが欠落し、その後に別の正常再生があった」
        # ケースも差1件になり誤判定するため、PLAY/ENDを出現順に追跡し、
        # 「区間末尾に残った最後の未完了再生だけ」である場合に限って境界切れとみなす。
        # 途中で未解決のまま次のPLAYが来た場合（mid_gap）は実際の異常としてWARNを維持する。
        mid_gap = False        # 区間途中でPLAYが未解決のまま次のPLAYが来た（＝本当の異常）
        trailing_open = False  # 区間末尾がPLAYで終わり、ENDが来ていない（＝境界切れ候補）
        for l in lines:
            if re.search(r"PLAY WAV END", l, re.IGNORECASE):
                trailing_open = False
            elif re.search(r"PLAY WAV:", l, re.IGNORECASE):
                if trailing_open:
                    mid_gap = True
                trailing_open = True
        boundary_cut = trailing_open and not mid_gap

        boundary_note = "区間末尾のWAV再生が時間帯の境界で打ち切られた可能性のため判定保留"
        if boundary_cut and next_bucket_hint and re.search(r"PLAY WAV END", next_bucket_hint, re.IGNORECASE):
            boundary_note = "区間末尾のWAV再生は次の時間帯冒頭でENDを確認できたため、境界での打ち切りと判断し判定保留"

        if play_n == 0:
            marks["WAV"] = MARK_NA
        elif end_n < play_n and boundary_cut:
            marks["WAV"] = MARK_NA
            notes["WAV"] = boundary_note
        elif end_n < play_n:
            marks["WAV"] = MARK_WARN
            findings.append(f"WAV再生の開始({play_n}件)に対し終了({end_n}件)が不足しています")
        elif pre_n < play_n:
            marks["WAV"] = MARK_WARN
            findings.append(f"WAV再生の開始({play_n}件)に対しPRE({pre_n}件)が不足しています")
        else:
            marks["WAV"] = MARK_OK
    else:
        marks["WAV"] = MARK_NA

    # Boot: 正常起動シーケンス（PANIC/REBOOT/WDT/Guru Meditationが無ければ正常）
    # 起動の有無・回数は「BOOT HEAP:」（setup()末尾で起動ごとに1回だけ記録）を根拠にする。
    # COUNT_KEYWORDSの緩い"Boot"判定（[BOOT]タイムスタンプタグ等も拾う）はマーク判定には使わない。
    # ただしPANIC/WDT/Guru Meditationは、起動記録(boot_n)の有無にかかわらず異常として扱う。
    boot_n = count(r"BOOT HEAP:")
    trouble = count(r"\bPANIC\b") + count(r"Guru Meditation") + count(r"\bWDT\b")
    reboot_n = count(r"\bREBOOT\b")
    if trouble:
        marks["Boot"] = MARK_BAD
        findings.append("PANIC / Guru Meditation / WDTが検出されました")
    elif boot_n == 0:
        marks["Boot"] = MARK_NA
    elif reboot_n > 1:
        marks["Boot"] = MARK_WARN
        findings.append(f"REBOOTが複数回検出されています（{reboot_n}件）")
    else:
        marks["Boot"] = MARK_OK
    if boot_n >= 1:
        notes["Boot"] = f"起動{boot_n}回"

    # NVS保存（実ログでは NVS ではなく CFG SAVED / CFG LOADED で記録される）
    save_ok   = has(r"CFG SAVED")
    save_fail = has(r"CFG SAVE[^\n]*(fail|error|ERROR|失敗)") or has(r"CFG[^\n]*SAVE[^\n]*(fail|error|ERROR|失敗)")
    load_ok   = has(r"CFG LOADED")
    if save_fail:
        marks["NVS保存"] = MARK_BAD
        findings.append("設定保存(CFG SAVE)に失敗しています")
    elif save_ok:
        marks["NVS保存"] = MARK_OK
    elif load_ok:
        # 保存操作そのものはこの区間に無いが、読込(CFG LOADED)は正常に確認できている。
        marks["NVS保存"] = MARK_OK
        notes["NVS保存"] = "保存操作なし・既存設定の読込(CFG LOADED)は正常"
    else:
        marks["NVS保存"] = MARK_NA

    # IDLE
    marks["IDLE"] = MARK_OK if has(r"\bIDLE\b") else MARK_NA

    bad_n  = sum(1 for m in marks.values() if m == MARK_BAD)
    warn_n = sum(1 for m in marks.values() if m == MARK_WARN)
    na_n   = sum(1 for m in marks.values() if m == MARK_NA)

    # 診断の確信度: 判定材料（◎△×が付いた項目）がどれだけ揃っているかの簡易目安。
    # ※各項目の◎△×－表示そのものには影響しない。
    if total < MIN_ANALYZABLE_LINES or na_n >= len(HEALTH_ITEMS) - 2:
        confidence = "低"
    elif na_n >= 3:
        confidence = "中"
    else:
        confidence = "高"

    if bad_n >= 2:
        stars = "★★☆☆☆"
    elif bad_n == 1:
        stars = "★★★☆☆"
    elif warn_n >= 3:
        stars = "★★★☆☆"
    elif warn_n >= 1:
        stars = "★★★★☆"
    else:
        stars = "★★★★★"

    if bad_n:
        recommend = "異常が検出されています。早めにログの詳細をご確認ください。"
    elif warn_n:
        recommend = "軽微な注意点があります。しばらく経過観察してください。"
    else:
        recommend = "そのまま運用して問題ありません。"

    return {"marks": marks, "notes": notes, "findings": findings, "stars": stars,
            "recommend": recommend, "confidence": confidence}

def make_kenshinsho(health, scope_label=None):
    out = ["="*60,
           "【KariPom 健康診断】",
           "="*60]
    if scope_label:
        out += ["対象：", scope_label, ""]
    confidence = health.get("confidence", "－")
    confidence_note = CONFIDENCE_NOTES.get(confidence, "")
    confidence_line = f"{confidence}（{confidence_note}）" if confidence_note else confidence
    out += ["総合判定",
           health["stars"],
           "",
           "診断の確信度",
           confidence_line,
           "",
           "システム状態",
           "-"*25]
    notes = health.get("notes", {})
    for item in HEALTH_ITEMS:
        line = f"{item:<12}{health['marks'].get(item, MARK_NA)}"
        if item in notes:
            line += f"　（{notes[item]}）"
        out.append(line)
    out += ["", "気になる点", "-"*25]
    if health["findings"]:
        for f in health["findings"]:
            out.append(f"・{f}")
    else:
        out.append("異常なし")
    out += ["", "推奨", "-"*25, health["recommend"]]
    return "\n".join(out)

def make_kenshinsho_na():
    """データ不足で判定できない場合の健康診断ブロック"""
    return "\n".join(["="*60, "【KariPom 健康診断】", "="*60,
                       "データ不足のため診断できません。"])

# ─── ユーティリティ（ログ解析） ───────────────

def do_kenben(log_text, next_bucket_hint=None):
    lines = log_text.splitlines()
    counts = {}
    for label, pat in COUNT_KEYWORDS.items():
        rx = re.compile(pat, re.IGNORECASE)
        counts[label] = sum(1 for l in lines if rx.search(l))
    important = []
    for line in lines:
        for label, pat in IMPORTANT_PATTERNS:
            if re.match(pat, line, re.IGNORECASE):
                important.append((label, line.strip()))
                break
    times = []
    trx = re.compile(r"\[(\d{2}:\d{2}:\d{2})\]|\b(\d{2}:\d{2}:\d{2})\b")
    for l in lines:
        m = trx.search(l)
        if m:
            times.append(m.group(1) or m.group(2))
    trange = f"{times[0]} ～ {times[-1]}" if len(times) >= 2 else "（不明）"
    return {"total": len(lines), "trange": trange,
            "counts": counts, "important": important,
            "health": analyze_health(log_text, next_bucket_hint),
            "power": parse_power_diagnosis(log_text),
            "pwr_snap": parse_pwr_snap(log_text),
            "power_events": extract_power_events(log_text)}

def make_summary(res, max_important=40):
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    out = []
    if res.get("power"):
        out += [make_power_report(res["power"], res.get("pwr_snap"), res.get("power_events", [])), ""]
    if res.get("health"):
        out += [make_kenshinsho(res["health"], "解析対象全体"), ""]
    out += ["="*60,
           f"【KariPom ログ解析レポート】  {now}",
           "="*60,
           f"総行数: {res['total']}行",
           f"時間範囲: {res['trange']}", "",
           "■ キーワード集計", "-"*40]
    for label, cnt in res["counts"].items():
        if cnt > 0:
            out.append(f"  {label:<28}: {cnt:>4} 件")
    out += ["", "■ 重要イベント抽出（先頭40件）", "-"*40]
    important = res["important"]
    if important:
        for label, line in important[:max_important]:
            out.append(f"  [{label}] {line}")
        if len(important) > max_important:
            out.append(f"  ... 他 {len(important)-max_important} 件省略")
    else:
        out.append("  （重要イベントなし）")
    out += ["", "このログを総合分析してください。"]
    return "\n".join(out)

def make_hourly_summaries(res, log_text):
    """ログを1時間ごとに分割してサマリーリストを返す"""
    lines = log_text.splitlines()
    trx = re.compile(r"\[(\d{4}/\d{2}/\d{2} (\d{2}):\d{2}:\d{2})\]")

    # 1時間ごとにラインを振り分け
    buckets = {}  # "YYYY/MM/DD HH" -> [lines]
    no_time = []
    for line in lines:
        m = trx.search(line)
        if m:
            key = m.group(1)[:13]  # "2026/07/09 03"
            buckets.setdefault(key, []).append(line)
        else:
            no_time.append(line)

    if not buckets:
        if res['total'] < MIN_ANALYZABLE_LINES:
            # タイムスタンプが1つも取れず、かつ総行数も少なすぎる場合は
            # Log Clear直後・新規ログの可能性が高いため通常サマリーを作らず打ち切る。
            out = [make_kenshinsho_na(), "",
                   "="*60,
                   "【KariPom ログ解析】",
                   "="*60,
                   f"行数: {res['total']}行", "",
                   "ログが短すぎるため解析できません。",
                   "Log Clear直後、Compress直後、または新規ログの可能性があります。"]
            return [("\n".join(out), "不明", "不明", "不明")]
        return [(make_summary(res), "不明", "不明", "不明")]  # タイムスタンプなければ通常サマリー

    summaries = []
    full_trx = re.compile(r"\[(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2})\]")
    sorted_keys = sorted(buckets.keys())
    for bucket_idx, key in enumerate(sorted_keys):
        hour_lines = buckets[key]
        hour_text = "\n".join(hour_lines)

        # 最初と最後のタイムスタンプを取得
        times_in_hour = []
        for l in hour_lines:
            m = full_trx.search(l)
            if m:
                times_in_hour.append(m.group(1))
        time_from = times_in_hour[0]  if times_in_hour else f"{key}:00:00"
        time_to   = times_in_hour[-1] if times_in_hour else f"{key}:59:59"

        if len(hour_lines) < MIN_ANALYZABLE_LINES:
            # 行数が少なすぎる時間帯はLog Clear直後・Compress直後・新規ログの可能性が高く、
            # キーワード集計や重要イベント抽出をしても意味のある結果にならないため解析を打ち切る。
            out = [make_kenshinsho_na(), "",
                   "="*60,
                   f"【KariPom ログ解析】{time_from} 〜 {time_to}  ({len(hour_lines)}行)",
                   "="*60,
                   f"期間: {time_from} 〜 {time_to}",
                   f"行数: {len(hour_lines)}行", "",
                   "ログが短すぎるため解析できません。",
                   "Log Clear直後、Compress直後、または新規ログの可能性があります。"]
            summaries.append(("\n".join(out), key, time_from, time_to))
            continue

        # 次の時間帯冒頭の数行をヒントとして渡す（末尾で途切れたWAV再生のEND確認用）
        next_hint = None
        if bucket_idx + 1 < len(sorted_keys):
            next_hint = "\n".join(buckets[sorted_keys[bucket_idx + 1]][:10])

        hour_res = do_kenben(hour_text, next_bucket_hint=next_hint)

        out = [make_power_report(hour_res["power"], hour_res.get("pwr_snap"), hour_res.get("power_events", [])), "",
               make_kenshinsho(hour_res["health"], f"{time_from} 〜 {time_to}"), "",
               "="*60,
               f"【KariPom ログ解析】{time_from} 〜 {time_to}  ({len(hour_lines)}行)",
               "="*60,
               f"期間: {time_from} 〜 {time_to}",
               f"行数: {len(hour_lines)}行", "",
               "■ キーワード集計", "-"*40]
        for label, cnt in hour_res["counts"].items():
            if cnt > 0:
                out.append(f"  {label:<28}: {cnt:>4} 件")
        out += ["", "■ 重要イベント抽出（先頭40件）", "-"*40]
        important = hour_res["important"]
        if important:
            for label, line in important[:40]:
                out.append(f"  [{label}] {line}")
            if len(important) > 40:
                out.append(f"  ... 他 {len(important)-40} 件省略")
        else:
            out.append("  （重要イベントなし）")
        out += ["", "この時間帯のログを分析してください。"]
        summaries.append(("\n".join(out), key, time_from, time_to))
    return summaries

def split_chunks(text, n):
    lines = text.splitlines()
    return ["\n".join(lines[i:i+n]) for i in range(0, len(lines), n)]

def pbcopy(text):
    QApplication.clipboard().setText(text)

# ─── 取得スレッド ─────────────────────────────

class FetchThread(QThread):
    done    = pyqtSignal(str)
    error   = pyqtSignal(str)

    def __init__(self, ip, port, file="current"):
        super().__init__()
        self.ip   = ip
        self.port = port
        self.file = file

    def run(self):
        try:
            url  = f"http://{self.ip}:{self.port}/logtoiletview?file={self.file}"
            resp = requests.get(url, timeout=10)
            # CoreS3側はファイルが無い（Clear直後・新規ログ等）場合404を返す。
            # これは通信エラーではないため、専用のシグナル値で区別して伝える。
            if resp.status_code == 404:
                self.error.emit("EMPTY_LOG")
                return
            resp.raise_for_status()
            self.done.emit(resp.text)
        except Exception as e:
            traceback.print_exc()
            self.error.emit(str(e))

# ─── Companion内蔵 Talk / KariPom モニター ─────────────────────────
# v5: Talkを別プロセスで起動せず、PC音声取得・発話判定・FFT・UDP送信を
# Companion自身のバックグラウンドスレッドへ統合する。
# これにより、IP未設定でもPC側KariPomは常に口パクでき、IP設定時のみ同じ状態を
# CoreS3へUDP送信する。Talkタブは処理の開始/停止を行う表示タブであり、別窓は作らない。

EMBED_THRESHOLD = 0.02
EMBED_SILENCE_TIMEOUT = 5.0
EMBED_HARD_STOP_RMS = 0.0005
EMBED_HARD_STOP_TIMEOUT = 0.8
EMBED_STATUS_SEND_INTERVAL = 2.0
EMBED_BLOCKSIZE = 2048
EMBED_SAMPLERATE = 44100
EMBED_M5_PORT = 12345
EMBED_FFT_UPDATE_INTERVAL = 0.08
EMBED_FFT_BANDS_HZ = [60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000]
EMBED_FFT_DB_RANGES = [
    (-60, -10), (-62, -12), (-63, -13), (-65, -15),
    (-65, -15), (-70, -20), (-78, -28), (-85, -35),
]
EMBED_FFT_ATTACK = 0.6
EMBED_FFT_DECAY = 0.18


class EmbeddedAudioState:
    """PC音声取得スレッドとGUIの共有状態。CoreS3/IPの有無とは独立。"""
    def __init__(self):
        self._lock = threading.Lock()
        self._speaking = False
        self._rms = 0.0
        self._error = ""
        self._fft = [0] * 8
        self._source = ""

    def set_audio(self, speaking, rms):
        with self._lock:
            self._speaking = bool(speaking)
            self._rms = float(rms)

    def set_fft(self, levels):
        with self._lock:
            self._fft = [int(v) for v in levels]

    def set_error(self, text):
        with self._lock:
            self._error = str(text or "")

    def set_source(self, text):
        with self._lock:
            self._source = str(text or "")

    def source(self):
        with self._lock:
            return self._source

    def snapshot(self):
        with self._lock:
            return self._speaking, self._rms, self._error, list(self._fft)


class EmbeddedSpeechDetector:
    """KariPom Desktop v0.2と同じ発話判定。"""
    def __init__(self, threshold, on_state):
        self.threshold = float(threshold)
        self.on_state = on_state
        self.speaking = False
        self.last_loud_time = 0.0
        self.hard_silence_start_time = None

    def process_block(self, mono_block):
        np = self._np
        block = np.asarray(mono_block, dtype=np.float32).reshape(-1)
        if block.size == 0:
            return
        rms = float(np.sqrt(np.mean(block ** 2)))
        now = time.time()

        if rms > self.threshold:
            self.last_loud_time = now
            self.hard_silence_start_time = None
            speaking_now = True
        else:
            if rms < EMBED_HARD_STOP_RMS:
                if self.hard_silence_start_time is None:
                    self.hard_silence_start_time = now
                hard_stop = (now - self.hard_silence_start_time) >= EMBED_HARD_STOP_TIMEOUT
                normal_stop = (now - self.last_loud_time) > EMBED_SILENCE_TIMEOUT
                speaking_now = not (hard_stop or normal_stop)
            else:
                self.hard_silence_start_time = None
                speaking_now = (now - self.last_loud_time) <= EMBED_SILENCE_TIMEOUT

        self.speaking = speaking_now
        self.on_state(speaking_now, rms)

    def bind_numpy(self, np_module):
        self._np = np_module

    def force_stop(self):
        self.speaking = False
        self.hard_silence_start_time = None
        self.on_state(False, 0.0)


class EmbeddedTalkEngine:
    """Desktop版の実績ある音声取得を基礎に、FFT/UDPだけを同一ブロックから分岐する。"""
    RESTART_WAIT = 2.0

    def __init__(self, state, ip_getter):
        self.state = state
        self.ip_getter = ip_getter
        self.stop_event = threading.Event()
        self.thread = None
        self.keepalive_thread = None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.np = None
        self.sd = None
        self.sc = None
        self.detector = EmbeddedSpeechDetector(EMBED_THRESHOLD, self._on_audio_state)
        self._last_udp_speaking = None
        self._last_status_sent = 0.0
        self._fft_last = 0.0
        self._fft_levels = None
        self._fft_window = None
        self._fft_bins = None

    def is_running(self):
        return self.thread is not None and self.thread.is_alive() and not self.stop_event.is_set()

    def start(self):
        if self.is_running():
            return
        self.stop_event.clear()
        self.state.set_error("")
        self.thread = threading.Thread(target=self._run, name="KariPomEmbeddedTalk", daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.detector.force_stop()
        self._send(b"SPEAK_STOP")

    def _current_ip(self):
        try:
            return str(self.ip_getter() or "").strip()
        except Exception:
            return ""

    def _send(self, payload):
        ip = self._current_ip()
        if not ip:
            return True
        try:
            self.sock.sendto(payload, (ip, EMBED_M5_PORT))
            return True
        except Exception:
            return False

    def _on_audio_state(self, speaking, rms):
        self.state.set_audio(speaking, rms)
        now = time.time()
        if self._last_udp_speaking is None or speaking != self._last_udp_speaking or (now - self._last_status_sent) >= EMBED_STATUS_SEND_INTERVAL:
            self._send(b"SPEAK_START" if speaking else b"SPEAK_STOP")
            self._last_udp_speaking = speaking
            self._last_status_sent = now

    def _prepare_fft(self):
        np = self.np
        self._fft_window = np.hanning(EMBED_BLOCKSIZE)
        freqs = np.fft.rfftfreq(EMBED_BLOCKSIZE, 1.0 / EMBED_SAMPLERATE)
        self._fft_bins = [
            np.where((freqs >= lo) & (freqs < hi))[0]
            for lo, hi in zip(EMBED_FFT_BANDS_HZ[:-1], EMBED_FFT_BANDS_HZ[1:])
        ]
        self._fft_levels = np.zeros(8, dtype=float)

    def _process_fft(self, mono_block):
        now = time.time()
        if now - self._fft_last < EMBED_FFT_UPDATE_INTERVAL:
            return
        self._fft_last = now
        np = self.np
        block = np.asarray(mono_block, dtype=np.float32).reshape(-1)
        if block.size != EMBED_BLOCKSIZE:
            return
        spec = np.abs(np.fft.rfft(block * self._fft_window)) / (EMBED_BLOCKSIZE / 2)
        band_db = np.array([
            20.0 * np.log10(np.sqrt(np.mean(spec[idx] ** 2)) + 1e-12)
            for idx in self._fft_bins
        ])
        db_min = np.array([r[0] for r in EMBED_FFT_DB_RANGES])
        db_max = np.array([r[1] for r in EMBED_FFT_DB_RANGES])
        target = np.clip((band_db - db_min) / (db_max - db_min), 0.0, 1.0) * 100.0
        coef = np.where(target > self._fft_levels, EMBED_FFT_ATTACK, EMBED_FFT_DECAY)
        self._fft_levels = self._fft_levels + coef * (target - self._fft_levels)
        levels = [int(v + 0.5) for v in self._fft_levels]
        self.state.set_fft(levels)
        if self._current_ip():
            self._send(("FFT:" + ",".join(str(v) for v in levels)).encode("ascii"))

    def _process_audio_block(self, mono_block):
        # Desktop版と同じ発話判定を最優先で処理し、同じブロックをFFTへ分岐する。
        self.detector.process_block(mono_block)
        self._process_fft(mono_block)

    def _load_audio_libs(self):
        import numpy as np
        self.np = np
        self.detector.bind_numpy(np)
        if platform.system() == "Darwin":
            import sounddevice as sd
            self.sd = sd
        else:
            import soundcard as sc
            self.sc = sc
        self._prepare_fft()

    def _find_blackhole_device(self):
        devices = self.sd.query_devices()
        for idx, dev in enumerate(devices):
            if "BlackHole" in dev["name"] and int(dev.get("max_input_channels", 0)) > 0:
                return idx, dev["name"]
        return None, None

    @staticmethod
    def _is_loopback_mic(mic):
        if getattr(mic, "isloopback", False):
            return True
        return str(getattr(mic, "id", "")).endswith(".monitor")

    def _find_loopback_mic(self):
        mics = self.sc.all_microphones(include_loopback=True)
        loopbacks = [mic for mic in mics if self._is_loopback_mic(mic)]
        try:
            speaker_name = self.sc.default_speaker().name
        except Exception:
            speaker_name = None
        if speaker_name:
            for mic in loopbacks:
                if speaker_name in mic.name:
                    return mic
        return loopbacks[0] if loopbacks else None

    def _run_mac(self):
        # KariPom Desktop v0.2と同じ callback InputStream方式。
        while not self.stop_event.is_set():
            device_id, device_name = self._find_blackhole_device()
            if device_id is None:
                msg = "BlackHole input device not found"
                self.state.set_source("")
                self.state.set_error(msg)
                self.detector.force_stop()
                self.stop_event.wait(self.RESTART_WAIT)
                continue

            self.state.set_source(device_name)
            self.state.set_error("")
            print(f"Detected BlackHole: {device_name} = {device_id}")

            def callback(indata, frames, time_info, status):  # noqa: ARG001
                if status:
                    print(f"Audio status: {status}", file=sys.stderr)
                try:
                    self._process_audio_block(indata[:, 0])
                except Exception as exc:
                    self.state.set_error(f"Audio callback error: {type(exc).__name__}: {exc}")

            try:
                with self.sd.InputStream(
                    device=device_id,
                    channels=1,
                    samplerate=EMBED_SAMPLERATE,
                    blocksize=EMBED_BLOCKSIZE,
                    callback=callback,
                ):
                    while not self.stop_event.wait(0.1):
                        pass
                    return
            except Exception as exc:
                self.state.set_error(f"InputStream error: {type(exc).__name__}: {exc}")
                self.detector.force_stop()
                self.stop_event.wait(self.RESTART_WAIT)

    def _keepalive_loop(self):
        np = self.np
        zeros = np.zeros((EMBED_BLOCKSIZE, 1), dtype="float32")
        while not self.stop_event.is_set():
            try:
                speaker = self.sc.default_speaker()
                with speaker.player(samplerate=EMBED_SAMPLERATE, blocksize=EMBED_BLOCKSIZE) as player:
                    while not self.stop_event.is_set():
                        player.play(zeros)
            except Exception:
                self.stop_event.wait(5.0)

    def _run_soundcard(self):
        if self.keepalive_thread is None or not self.keepalive_thread.is_alive():
            self.keepalive_thread = threading.Thread(target=self._keepalive_loop, name="KariPomKeepalive", daemon=True)
            self.keepalive_thread.start()
        while not self.stop_event.is_set():
            mic = self._find_loopback_mic()
            if mic is None:
                self.state.set_source("")
                self.state.set_error("PC再生音のループバック/モニター入力が見つかりません")
                self.detector.force_stop()
                self.stop_event.wait(self.RESTART_WAIT)
                continue
            self.state.set_source(mic.name)
            self.state.set_error("")
            print(f"Detected loopback: {mic.name}")
            try:
                with mic.recorder(samplerate=EMBED_SAMPLERATE, blocksize=EMBED_BLOCKSIZE) as recorder:
                    while not self.stop_event.is_set():
                        data = recorder.record(numframes=EMBED_BLOCKSIZE)
                        mono = data.mean(axis=1) if data.ndim > 1 and data.shape[1] > 1 else data.reshape(-1)
                        self._process_audio_block(mono)
            except Exception as exc:
                self.state.set_error(f"Audio capture error: {type(exc).__name__}: {exc}")
                self.detector.force_stop()
                self.stop_event.wait(self.RESTART_WAIT)

    def _run(self):
        try:
            self._load_audio_libs()
            if platform.system() == "Darwin":
                self._run_mac()
            else:
                self._run_soundcard()
        except Exception as exc:
            self.state.set_error(f"Talk engine error: {type(exc).__name__}: {exc}")
            self.detector.force_stop()

FACE_W = 320
FACE_H = 240
FACE_NOSE_X = 160
FACE_NOSE_Y = 145
FACE_MOUTH_PAKU_MS = 140
FACE_BLINK_CLOSED_MS = 80
FACE_DOUBLE_BLINK_GAP_MS = 180
FACE_BLINK_PROBABILITY = 0.45
FACE_DOUBLE_BLINK_PROBABILITY = 0.08
FACE_BLINK_INTERVAL_MS = (3000, 10000)
FACE_NOSE_INTERVAL_MS = (120, 280)


class KariPomFaceWidget(QWidget):
    """Companion内蔵音声に同期する、クリック切替対応KariPom BBX。"""

    VIS_MODES = (
        "Face",
        "EQ Classic",
        "Audio Halo",
        "Mirror Wave",
        "8-Lane Rhythm",
        "Kaleidoscope",
        "Analog VU",
        "Tetromino Dance",
    )
    EQ_GAIN8 = (1.6, 1.6, 1.6, 1.6, 1.7, 1.8, 2.0, 2.2)
    SPECTRUM_RGB = (
        (0, 0, 255), (0, 255, 255), (0, 255, 0), (173, 255, 41),
        (255, 255, 0), (255, 165, 0), (255, 0, 0), (255, 0, 255),
    )
    SCENE_TOP = 48

    def __init__(self, audio_state, scale=2, parent=None):
        super().__init__(parent)
        self.audio_state = audio_state
        self.scale_factor = scale
        self.speaking = False
        self.was_speaking = False
        self.eye_mode = "open"
        self.nose_offset = 0
        self.mouth_open = False
        self.mouth_mx = FACE_NOSE_X
        self.mouth_my = FACE_NOSE_Y + 26
        self.mouth_mw = 18
        self.mouth_mh = 12

        # Face -> 7 Visualizers -> Face
        self.display_mode = 0

        # Visualizer state. Keep these as Python lists so Companion GUI does not
        # gain a new mandatory numpy import at module startup.
        self.halo_agc_peak = 0.0
        self.mw_agc_peak = 0.0
        self.mw_phase = 0.0
        self.rhythm_hist = [[0.0] * 8 for _ in range(48)]
        self.rhythm_prev = [0.0] * 8
        self.rhythm_last_step = 0

        # 2026-08-10 adopted Kaleidoscope look:
        # compact rings, very slow auto rotation, audio deformation dominant.
        self.kal_rot = 0.0
        self.kal_pulse = 0.0
        self.kal_bass_avg = 0.0
        self.kal_level_fast = 0.0
        self.kal_phase = [0.0, 1.3, 2.7, 4.1]
        self.kal_period = [15.0, 18.0, 21.0, 13.0]

        # 2026-08-10 adopted Analog VU behavior/shape.
        self.avu_needle = [0.0] * 8
        self.avu_level_env = 0.0

        self.tetro_pieces = []
        self.tetro_last_spawn = 0
        self.tetro_bass_avg = 0.0
        self._tetro_prev = [0.0] * 8

        now = self._now_ms()
        self.next_nose_at = now + random.randrange(*FACE_NOSE_INTERVAL_MS)
        self.next_blink_check_at = now + random.randrange(*FACE_BLINK_INTERVAL_MS)
        self.next_mouth_at = now + FACE_MOUTH_PAKU_MS
        self.blink_events = []

        self.setFixedSize(FACE_W * scale, FACE_H * scale)
        self.setAttribute(Qt.WA_OpaquePaintEvent, True)
        self.setCursor(Qt.PointingHandCursor)
        self.setToolTip("画面をクリックすると Face → 7 Visualizers → Face の順に切り替わります")

        self.timer = QTimer(self)
        self.timer.setInterval(20)
        self.timer.timeout.connect(self._tick)
        self.timer.start()

    @staticmethod
    def _now_ms():
        return int(time.monotonic() * 1000)

    @staticmethod
    def _clamp(v, lo=0.0, hi=1.0):
        return lo if v < lo else hi if v > hi else v

    @classmethod
    def _spectrum_color(cls, p):
        p = cls._clamp(float(p)) * 7.0
        i = int(math.floor(p))
        j = min(7, i + 1)
        t = p - i
        a = cls.SPECTRUM_RGB[i]
        b = cls.SPECTRUM_RGB[j]
        return QColor(
            int(a[0] * (1.0 - t) + b[0] * t),
            int(a[1] * (1.0 - t) + b[1] * t),
            int(a[2] * (1.0 - t) + b[2] * t),
        )

    @staticmethod
    def _tint(color, white_pct):
        t = max(0.0, min(1.0, float(white_pct) / 100.0))
        return QColor(
            int(color.red() * (1.0 - t) + 255 * t),
            int(color.green() * (1.0 - t) + 255 * t),
            int(color.blue() * (1.0 - t) + 255 * t),
        )

    @staticmethod
    def _fft8(values):
        try:
            vals = [float(v) for v in list(values)[:8]]
        except Exception:
            vals = []
        if len(vals) < 8:
            vals.extend([0.0] * (8 - len(vals)))
        return [max(0.0, min(100.0, v)) for v in vals]

    @classmethod
    def _interp_band(cls, fft_levels, p):
        vals = [v / 100.0 for v in cls._fft8(fft_levels)]
        x = cls._clamp(p) * 7.0
        i = int(math.floor(x))
        j = min(7, i + 1)
        t = x - i
        return vals[i] * (1.0 - t) + vals[j] * t

    def set_speaking(self, speaking):
        self.speaking = bool(speaking)

    def mousePressEvent(self, event):
        """左クリックで Face → 7 Visualizers → Face を順送りする。"""
        if event.button() == Qt.LeftButton:
            self.display_mode = (self.display_mode + 1) % len(self.VIS_MODES)
            self.update()
            event.accept()
            return
        super().mousePressEvent(event)

    def _tick(self):
        now = self._now_ms()
        speaking, _rms, _error, fft_raw = self.audio_state.snapshot()
        fft = self._fft8(fft_raw)
        fft01 = [v / 100.0 for v in fft]
        level = sum(fft01) / 8.0
        bass = (fft01[0] + fft01[1]) * 0.5

        if speaking != self.was_speaking:
            self.was_speaking = speaking
            if speaking:
                self.mouth_open = False
                self.next_mouth_at = now
            else:
                self.mouth_open = False

        if speaking and now >= self.next_mouth_at:
            self.next_mouth_at = now + FACE_MOUTH_PAKU_MS
            self.mouth_open = not self.mouth_open
            if self.mouth_open:
                self._randomize_talk_mouth()

        if now >= self.next_nose_at:
            self.nose_offset = -1 if self.nose_offset >= 0 else 1
            self.next_nose_at = now + random.randrange(*FACE_NOSE_INTERVAL_MS)

        if now >= self.next_blink_check_at:
            self.next_blink_check_at = now + random.randrange(*FACE_BLINK_INTERVAL_MS)
            if random.random() < FACE_BLINK_PROBABILITY:
                self._schedule_blink(now)

        while self.blink_events and now >= self.blink_events[0][0]:
            _, mode = self.blink_events.pop(0)
            self.eye_mode = mode

        # Mirror Wave
        self.mw_phase = (self.mw_phase + 0.02 + level * 0.30) % 6.0

        # Latest Kaleidoscope: bass pulse + very slow rotation.
        self.kal_bass_avg += (bass - self.kal_bass_avg) * 0.15
        if bass > self.kal_bass_avg * 1.3 + 0.04:
            self.kal_pulse = 1.0
        self.kal_pulse *= 0.65
        self.kal_level_fast += (level - self.kal_level_fast) * (0.50 if level > self.kal_level_fast else 0.18)
        # CoreS3 uses 0.004 rad per ~70 ms frame. Companion ticks at 20 ms.
        self.kal_rot = (self.kal_rot + 0.004 * (20.0 / 70.0)) % (2.0 * math.pi)

        # Latest Analog VU: absolute + relative spectrum contribution, attack/release.
        gained = [min(1.5, fft01[i] * self.EQ_GAIN8[i]) for i in range(8)]
        mean = sum(gained) / 8.0
        self.avu_level_env += (level - self.avu_level_env) * (0.50 if level > self.avu_level_env else 0.18)
        gate = self._clamp((self.avu_level_env - 0.012) / 0.040)
        rel_den = max(mean, 0.060)
        for i in range(8):
            target = 0.0
            if gained[i] >= 0.020:
                a_abs = min(1.0, gained[i] / 1.30)
                ratio = gained[i] / rel_den
                a_rel = self._clamp((ratio - 0.35) / (1.90 - 0.35))
                target = min(1.0, gate * (0.55 * a_abs + 0.60 * a_rel))
            coef = 0.60 if target > self.avu_needle[i] else 0.14
            self.avu_needle[i] += (target - self.avu_needle[i]) * coef

        self._update_rhythm(now, fft)
        self._update_tetromino(now, fft01, level, bass)
        self.update()

    def _schedule_blink(self, now):
        self.blink_events.append((now, "blink"))
        self.blink_events.append((now + FACE_BLINK_CLOSED_MS, "open"))
        if random.random() < FACE_DOUBLE_BLINK_PROBABILITY:
            second_close = now + FACE_BLINK_CLOSED_MS + FACE_DOUBLE_BLINK_GAP_MS
            self.blink_events.append((second_close, "blink"))
            self.blink_events.append((second_close + FACE_BLINK_CLOSED_MS, "open"))
        self.blink_events.sort(key=lambda item: item[0])

    def _randomize_talk_mouth(self):
        self.mouth_mx = FACE_NOSE_X + random.choice((-2, 0, 2))
        self.mouth_my = FACE_NOSE_Y + 26 + random.choice((-2, 0, 2))
        self.mouth_mw = 18 + random.choice((-2, 0, 2))
        self.mouth_mh = 12 + random.choice((-2, 0, 2))

    @staticmethod
    def _draw_thick_line(painter, x0, y0, x1, y1, thickness, color):
        pen = QPen(color)
        pen.setWidth(thickness)
        pen.setCapStyle(Qt.RoundCap)
        painter.setPen(pen)
        painter.drawLine(int(x0), int(y0), int(x1), int(y1))

    @staticmethod
    def _fill_ellipse(painter, cx, cy, rx, ry, color):
        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(color))
        painter.drawEllipse(int(cx - rx), int(cy - ry), int(rx * 2), int(ry * 2))

    @classmethod
    def _fill_circle(cls, painter, cx, cy, radius, color):
        cls._fill_ellipse(painter, cx, cy, radius, radius, color)

    @staticmethod
    def _poly(painter, points, color):
        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(color))
        painter.drawPolygon(*[QPoint(int(round(x)), int(round(y))) for x, y in points])

    def _draw_face(self, painter):
        black = QColor("black")
        red = QColor("red")
        if self.eye_mode == "blink":
            self._draw_thick_line(painter, 72, 90, 108, 90, 6, black)
            self._draw_thick_line(painter, 212, 90, 248, 90, 6, black)
        else:
            self._fill_circle(painter, 90, 90, 20, black)
            self._fill_circle(painter, 230, 90, 20, black)

        self._fill_ellipse(painter, FACE_NOSE_X, FACE_NOSE_Y + self.nose_offset, 18, 12, black)

        if self.mouth_open and self.was_speaking:
            self._draw_thick_line(
                painter, FACE_NOSE_X, FACE_NOSE_Y + self.nose_offset + 8,
                FACE_NOSE_X, FACE_NOSE_Y + 25, 6, black
            )
            self._fill_ellipse(
                painter, self.mouth_mx, self.mouth_my,
                self.mouth_mw, self.mouth_mh, red
            )
        else:
            self._draw_thick_line(
                painter, FACE_NOSE_X, FACE_NOSE_Y + self.nose_offset + 8,
                FACE_NOSE_X, FACE_NOSE_Y + 22, 6, black
            )
            self._draw_thick_line(painter, FACE_NOSE_X, FACE_NOSE_Y + 22, FACE_NOSE_X - 20, FACE_NOSE_Y + 32, 6, black)
            self._draw_thick_line(painter, FACE_NOSE_X, FACE_NOSE_Y + 22, FACE_NOSE_X + 20, FACE_NOSE_Y + 32, 6, black)

    def _draw_face_with_rim(self, painter):
        white = QColor("white")
        if self.eye_mode == "blink":
            self._draw_thick_line(painter, 72, 90, 108, 90, 10, white)
            self._draw_thick_line(painter, 212, 90, 248, 90, 10, white)
        else:
            self._fill_circle(painter, 90, 90, 24, white)
            self._fill_circle(painter, 230, 90, 24, white)
        self._fill_ellipse(painter, FACE_NOSE_X, FACE_NOSE_Y + self.nose_offset, 22, 16, white)
        # Keep the visualizer background visible behind the mouth.
        # Eyes and nose retain their white rim, but no rectangular mouth underlay.
        self._draw_face(painter)

    def _draw_eq(self, painter, fft):
        painter.setPen(Qt.NoPen)
        for i, raw in enumerate(fft):
            disp = min(100.0, raw * self.EQ_GAIN8[i])
            h = int(round(disp * 160.0 / 100.0))
            if h > 0:
                painter.setBrush(QBrush(QColor(*self.SPECTRUM_RGB[i])))
                painter.drawRect(i * 40 + 3, 200 - h, 34, h)
        self._draw_face(painter)

    def _draw_halo(self, painter, fft):
        # 2026-08-10 adopted CoreS3 look: true circle, not the former flattened ellipse.
        vals = [v / 100.0 for v in fft]
        peak = max(vals) if vals else 0.0
        self.halo_agc_peak += (peak - self.halo_agc_peak) * (1.0 if peak > self.halo_agc_peak else 0.03)
        ref = max(0.21, self.halo_agc_peak)
        agc = 0.64 / ref
        cx, cy = 160.0, 143.0
        rin, rout = 62.0, 93.0
        for j in range(48):
            rel = j / 47.0
            fold = min(rel * 2.0, 2.0 - rel * 2.0)
            v = min(1.0, self._interp_band(fft, fold) * agc)
            if v < 0.02:
                v = 0.0
            elif v < 0.065:
                v *= (v - 0.02) / 0.045
            if v > 0.0:
                v = v ** 0.48
            a = math.radians(90.0 + j * 360.0 / 48.0)
            dx, dy = math.cos(a), math.sin(a)
            r2 = rin + (rout - rin) * v
            base = self._spectrum_color(fold)
            for frac, tint, width in ((0.45, 82, 8), (0.70, 58, 7), (0.88, 28, 6), (1.0, 0, 4)):
                rr = rin + (r2 - rin) * frac
                pen = QPen(self._tint(base, tint))
                pen.setWidth(width)
                pen.setCapStyle(Qt.RoundCap)
                painter.setPen(pen)
                painter.drawLine(
                    int(cx + dx * rin), int(cy + dy * rin),
                    int(cx + dx * rr), int(cy + dy * rr)
                )
        self._draw_face(painter)

    def _draw_mirror(self, painter, fft):
        vals = [v / 100.0 for v in fft]
        peak = max(vals) if vals else 0.0
        self.mw_agc_peak += (peak - self.mw_agc_peak) * (1.0 if peak > self.mw_agc_peak else 0.03)
        agc = 0.80 / max(0.20, self.mw_agc_peak)
        pal = (
            QColor(0,230,255), QColor(0,140,255), QColor(150,90,255),
            QColor(255,60,220), QColor(255,200,0), QColor(60,255,140),
        )
        cy, ymin, ymax, sx, edge = 143, 50, 236, 4, 6
        max_half = (ymax - ymin) // 2
        for c in range(81):
            x = min(319, c * sx)
            p = x / 319.0
            v = min(1.0, self._interp_band(fft, p) * agc)
            h = max(2, int((v ** 0.45) * max_half))
            top, bot = max(ymin, cy - h), min(ymax, cy + h)
            hue = pal[int(c * 0.10 + self.mw_phase) % len(pal)]
            body = self._tint(hue, 18)
            hi = self._tint(hue, 62)
            painter.setPen(Qt.NoPen)
            if bot - top > 2 * edge:
                painter.setBrush(QBrush(body))
                painter.drawRect(x, top + edge, sx, bot - top - 2 * edge)
            painter.setBrush(QBrush(hi))
            painter.drawRect(x, top, sx, edge)
            painter.drawRect(x, bot - edge, sx, edge)
        self._draw_face(painter)

    def _update_rhythm(self, now, fft):
        if now - self.rhythm_last_step < 40:
            return
        self.rhythm_last_step = now
        self.rhythm_hist[1:] = [row[:] for row in self.rhythm_hist[:-1]]
        self.rhythm_hist[0] = [0.0] * 8
        vals = [min(100.0, fft[i] * self.EQ_GAIN8[i]) for i in range(8)]
        rise = [vals[i] - self.rhythm_prev[i] for i in range(8)]
        for i in range(8):
            if vals[i] >= 12.0 and rise[i] >= 10.0:
                self.rhythm_hist[0][i] = vals[i]
        self.rhythm_prev = vals

    def _draw_rhythm(self, painter):
        painter.setPen(Qt.NoPen)
        for r, row in enumerate(self.rhythm_hist):
            if (r % 4) >= 3:
                continue
            y = self.SCENE_TOP + r * 4
            for i, disp in enumerate(row):
                if disp <= 0:
                    continue
                tint = max(0.0, min(88.0, 100.0 - disp))
                painter.setBrush(QBrush(self._tint(QColor(*self.SPECTRUM_RGB[i]), tint)))
                painter.drawRect(i * 40 + 3, y, 34, 16)
        self._draw_face(painter)

    def _draw_kaleido(self, painter, fft, now_sec):
        # 2026-08-10 adopted compact Kaleidoscope:
        # total radius ~106..136 px, slow auto morph/rotation, audio shape dominant.
        painter.fillRect(0, self.SCENE_TOP, FACE_W, FACE_H - self.SCENE_TOP, QColor("white"))
        painter.save()
        painter.setClipRect(0, self.SCENE_TOP, FACE_W, FACE_H - self.SCENE_TOP)

        vals = [v / 100.0 for v in fft]
        gained = [vals[i] * self.EQ_GAIN8[i] for i in range(8)]
        mean = sum(gained) / 8.0
        gate = self._clamp((self.kal_level_fast - 0.015) / 0.05)
        loud_trim = 0.55 + 0.45 * min(1.0, self.kal_level_fast / 0.35)
        amp = gate * loud_trim
        pulse = 1.0 + 0.10 * self.kal_pulse

        gaps_min = (20.0, 24.0, 28.0, 34.0)
        gaps_max = (26.0, 30.0, 36.0, 44.0)
        cx, cy = 160.0, 144.0
        r_prev = 0.0

        def rel_shape(v):
            denom = max(mean, 0.05)
            ratio = max(-2.5, min(2.5, (v - mean) / denom))
            mag = math.sqrt(abs(ratio) * 1.3)
            return max(-1.3, min(1.3, mag if ratio >= 0 else -mag))

        for ring in range(4):
            # 12..22s-scale base morph kept intentionally subtle.
            wave = 0.5 + 0.5 * math.sin((now_sec / self.kal_period[ring]) * 2.0 * math.pi + self.kal_phase[ring])
            gap = gaps_min[ring] + (gaps_max[ring] - gaps_min[ring]) * wave
            r_in = r_prev * pulse
            r_out = (r_prev + gap) * pulse
            r_prev += gap

            shape_r = rel_shape(gained[ring * 2])
            shape_a = rel_shape(gained[ring * 2 + 1])
            frac_base = 0.38 + 0.24 * (0.5 + 0.5 * math.sin((now_sec / (self.kal_period[ring] * 1.17)) * 2.0 * math.pi + self.kal_phase[ring]))
            r_f = r_in + (r_out - r_in) * frac_base + shape_r * amp * 45.0 + self.kal_pulse * 22.0
            margin = max(3.0, (r_out - r_in) * 0.12)
            r_f = max(r_in + margin, min(r_out - margin, r_f))
            base_ang = math.radians(10.0 + 10.0 * wave)
            ang = max(math.radians(2.0), min(math.radians(28.0), base_ang + shape_a * amp * 0.55))

            # One 30-degree seed wedge, mirrored into 12 copies.
            in0 = (r_in, 0.0)
            in1 = (r_in * math.cos(math.pi / 6), r_in * math.sin(math.pi / 6))
            out0 = (r_out, 0.0)
            out1 = (r_out * math.cos(math.pi / 6), r_out * math.sin(math.pi / 6))
            fp = (r_f * math.cos(ang), r_f * math.sin(ang))

            audio_avg = (abs(shape_r) + abs(shape_a)) * 0.5 * amp
            hue_spread = 1.0 + audio_avg * 0.9
            white_boost = min(92.0, 8.0 + audio_avg * 42.0 + self.kal_pulse * 55.0)
            colors = [
                self._tint(self._spectrum_color((ring * 0.22 + ofs * hue_spread) % 1.0), white_boost)
                for ofs in (0.00, 0.05, 0.10, 0.15)
            ]
            tris = (
                (in0, in1, fp),
                (in1, out1, fp),
                (out1, out0, fp),
                (out0, in0, fp),
            )

            for sec in range(6):
                sec_a = sec * math.pi / 3.0
                for mirrored in (False, True):
                    for tri, col in zip(tris, colors):
                        pts = []
                        for x, y in tri:
                            if mirrored:
                                y = -y
                            # sector rotation, then whole-pattern rotation
                            cs, ss = math.cos(sec_a), math.sin(sec_a)
                            tx, ty = x * cs - y * ss, x * ss + y * cs
                            cr, sr = math.cos(self.kal_rot), math.sin(self.kal_rot)
                            fx, fy = tx * cr - ty * sr, tx * sr + ty * cr
                            pts.append((cx + fx, cy + fy))
                        self._poly(painter, pts, col)

        painter.restore()
        self._draw_face(painter)

    def _draw_analog_vu(self, painter):
        # 2026-08-10 adopted arch-top meter faces:
        # meter/ticks/needle size retained; unused square top is removed.
        painter.fillRect(0, self.SCENE_TOP, FACE_W, FACE_H - self.SCENE_TOP, QColor("white"))
        cream = QColor(245, 230, 200)
        frame = QColor(33, 36, 36)
        bezel = QColor(82, 82, 82)
        red_zone = QColor(192, 0, 0)
        needle = QColor(248, 0, 0)
        labels = ("L1", "L2", "M1", "M2", "M3", "M4", "H1", "H2")

        for b in range(8):
            col, row = b % 4, b // 4
            cx0, cy0 = col * 80, self.SCENE_TOP + row * 96
            px, py = cx0 + 3, cy0 + 8
            vx, vy = cx0 + 40, cy0 + 74
            panel_w, panel_h = 74, 76
            face_r = 44  # tick outer radius 42 + requested 2 px
            panel_bottom = py + panel_h - 1

            # Fill only below the arch. This is the key adopted change.
            painter.setPen(Qt.NoPen)
            painter.setBrush(QBrush(cream))
            prev_top = None
            for lx in range(panel_w):
                sx = px + lx
                dx = sx - vx
                rr = max(0, face_r * face_r - dx * dx)
                top_y = vy - int(round(math.sqrt(rr)))
                painter.drawRect(sx, top_y, 1, panel_bottom - top_y + 1)
                if prev_top is not None:
                    painter.setPen(QPen(frame, 1))
                    painter.drawLine(prev_top[0], prev_top[1], sx, top_y)
                    painter.setPen(Qt.NoPen)
                prev_top = (sx, top_y)

            # Side/bottom frame.
            def arch_y(sx, radius):
                rr = max(0, radius * radius - (sx - vx) ** 2)
                return vy - int(round(math.sqrt(rr)))
            right_x = px + panel_w - 1
            painter.setPen(QPen(frame, 1))
            painter.drawLine(px, arch_y(px, face_r), px, panel_bottom)
            painter.drawLine(right_x, arch_y(right_x, face_r), right_x, panel_bottom)
            painter.drawLine(px, panel_bottom, right_x, panel_bottom)

            # 1px outer bezel following the arch.
            prev = None
            for lx in range(-1, panel_w + 1):
                sx = px + lx
                dx = sx - vx
                rr = (face_r + 1) ** 2 - dx * dx
                if rr < 0:
                    continue
                yy = vy - int(round(math.sqrt(rr)))
                if prev is not None:
                    painter.setPen(QPen(bezel, 1))
                    painter.drawLine(prev[0], prev[1], sx, yy)
                prev = (sx, yy)

            # Arc + ticks.
            arc_pts = []
            for k in range(13):
                deg = -55.0 + (110.0 * k / 12.0)
                a = math.radians(deg)
                arc_pts.append((vx + math.sin(a) * 42, vy - math.cos(a) * 42))
            painter.setPen(QPen(frame, 1))
            for p0, p1 in zip(arc_pts[:-1], arc_pts[1:]):
                painter.drawLine(int(p0[0]), int(p0[1]), int(p1[0]), int(p1[1]))

            # Dense red peak band.
            painter.setPen(QPen(red_zone, 2))
            for k in range(16):
                t = 0.75 + 0.25 * k / 15.0
                deg = -55.0 + 110.0 * t
                a = math.radians(deg)
                painter.drawLine(
                    int(vx + math.sin(a) * 38), int(vy - math.cos(a) * 38),
                    int(vx + math.sin(a) * 42), int(vy - math.cos(a) * 42),
                )

            for k in range(7):
                t = k / 6.0
                deg = -55.0 + 110.0 * t
                a = math.radians(deg)
                painter.setPen(QPen(red_zone if t >= 0.75 else frame, 1))
                painter.drawLine(
                    int(vx + math.sin(a) * 34), int(vy - math.cos(a) * 34),
                    int(vx + math.sin(a) * 42), int(vy - math.cos(a) * 42),
                )

            # Labels at lower corners, matching current CoreS3 design.
            painter.setPen(QPen(frame, 1))
            painter.drawText(cx0 + 17, cy0 + 80, labels[b])
            painter.drawText(cx0 + 53, cy0 + 80, "VU")

            # Needle + hub.
            a = math.radians(-55.0 + 110.0 * self.avu_needle[b])
            sx, sy = math.sin(a), -math.cos(a)
            pxp, pyp = math.cos(a), math.sin(a)
            tip = (vx + sx * 38.0, vy + sy * 38.0)
            b1 = (vx + pxp, vy + pyp)
            b2 = (vx - pxp, vy - pyp)
            self._poly(painter, (b1, b2, tip), needle)
            self._fill_circle(painter, vx, vy, 3, frame)
            self._fill_circle(painter, vx, vy, 1, bezel)

        self._draw_face(painter)

    def _spawn_tetromino(self, color, now):
        if len(self.tetro_pieces) >= 4:
            return
        shape = random.choice(("O", "I", "L"))
        self.tetro_pieces.append({
            "shape": shape,
            "cx": float(random.randint(64, 256)),
            "cy": float(self.SCENE_TOP - 64),
            "vy": 0.6,
            "angle": 0.0,
            "target": 0.0,
            "color": color,
            "next_rot": now + random.randint(900, 1700),
            "slide_to": None,
            "slide_from": 0.0,
            "slide_start": 0,
            "slide_dur": 0,
            "next_slide": now + random.randint(1200, 2200),
        })

    def _update_tetromino(self, now, fft01, level, bass):
        vals = [min(100.0, fft01[i] * self.EQ_GAIN8[i] * 100.0) for i in range(8)]
        for i in range(8):
            if vals[i] >= 15.0 and vals[i] - self._tetro_prev[i] >= 12.0 and now - self.tetro_last_spawn >= 220:
                self._spawn_tetromino(QColor(*self.SPECTRUM_RGB[i]), now)
                self.tetro_last_spawn = now
                break
        self._tetro_prev = vals

        if not self.tetro_pieces and now - self.tetro_last_spawn >= 2500:
            self._spawn_tetromino(QColor(*self.SPECTRUM_RGB[4]), now)
            self.tetro_last_spawn = now

        self.tetro_bass_avg += (bass - self.tetro_bass_avg) * 0.15
        bass_hit = bass > self.tetro_bass_avg * 1.3 + 0.04

        for p in list(self.tetro_pieces):
            p["cy"] += p["vy"] * (1.0 + level * 2.2)
            if abs(p["target"] - p["angle"]) > 1e-4:
                step = 0.2618 if p["target"] > p["angle"] else -0.2618
                if abs(p["target"] - p["angle"]) <= abs(step):
                    p["angle"] = p["target"]
                else:
                    p["angle"] += step
            elif p["shape"] != "O" and (now >= p["next_rot"] or (bass_hit and random.random() < 0.25)):
                p["target"] = p["angle"] + math.pi / 2.0
                p["next_rot"] = now + random.randint(900, 1700)

            if p["slide_to"] is not None:
                t = min(1.0, (now - p["slide_start"]) / max(1, p["slide_dur"]))
                e = t * t * (3.0 - 2.0 * t)
                p["cx"] = p["slide_from"] + (p["slide_to"] - p["slide_from"]) * e
                if t >= 1.0:
                    p["slide_to"] = None
            elif now >= p["next_slide"]:
                if random.random() < 0.5:
                    target = p["cx"] + random.choice((-1, 1)) * random.choice((40, 80))
                    target = max(64.0, min(256.0, target))
                    if abs(target - p["cx"]) > 4.0:
                        p["slide_from"] = p["cx"]
                        p["slide_to"] = target
                        p["slide_start"] = now
                        p["slide_dur"] = random.randint(300, 500)
                p["next_slide"] = now + random.randint(1200, 2200)

            if p["cy"] - 64 > FACE_H:
                self.tetro_pieces.remove(p)

    def _draw_tetromino(self, painter):
        painter.save()
        painter.setClipRect(0, self.SCENE_TOP, FACE_W, FACE_H - self.SCENE_TOP)
        shapes = {
            "O": ((0,0),(1,0),(0,1),(1,1)),
            "I": ((0,0),(1,0),(2,0)),
            "L": ((0,0),(1,0),(0,1)),
        }
        for p in self.tetro_pieces:
            cells = shapes[p["shape"]]
            w = 3 if p["shape"] == "I" else 2
            h = 1 if p["shape"] == "I" else 2
            ca, sa = math.cos(p["angle"]), math.sin(p["angle"])
            for col, row in cells:
                lcx = (col - (w - 1) / 2.0) * 40
                lcy = (row - (h - 1) / 2.0) * 40
                half = 19
                pts = []
                for lx, ly in (
                    (lcx-half,lcy-half),(lcx+half,lcy-half),
                    (lcx+half,lcy+half),(lcx-half,lcy+half)
                ):
                    rx, ry = lx * ca - ly * sa, lx * sa + ly * ca
                    pts.append((p["cx"] + rx, p["cy"] + ry))
                self._poly(painter, pts, p["color"])
        painter.restore()
        self._draw_face(painter)

    def paintEvent(self, event):  # noqa: ARG002
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, False)
        painter.fillRect(self.rect(), QColor("white"))
        painter.scale(self.scale_factor, self.scale_factor)

        _speaking, _rms, _error, fft_raw = self.audio_state.snapshot()
        fft = self._fft8(fft_raw)
        now_sec = time.monotonic()

        if self.display_mode == 0:
            self._draw_face(painter)
        elif self.display_mode == 1:
            self._draw_eq(painter, fft)
        elif self.display_mode == 2:
            self._draw_halo(painter, fft)
        elif self.display_mode == 3:
            self._draw_mirror(painter, fft)
        elif self.display_mode == 4:
            self._draw_rhythm(painter)
        elif self.display_mode == 5:
            self._draw_kaleido(painter, fft, now_sec)
        elif self.display_mode == 6:
            self._draw_analog_vu(painter)
        elif self.display_mode == 7:
            self._draw_tetromino(painter)

        painter.end()


# ─── メインウィンドウ ─────────────────────────

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("KariPom Companion")
        self.resize(1300, 820)

        self.cfg        = load_config()
        self.log_text   = ""
        self.kenben_res = None
        self.chunks          = []
        self.chunk_idx       = 0
        self.hourly_summaries = []
        self.hourly_idx       = 0
        self._fetch_thread = None
        self._displayed_log_path = None
        self._audio_state = EmbeddedAudioState()
        self._talk_engine = EmbeddedTalkEngine(
            self._audio_state,
            ip_getter=lambda: self.cfg.get("ip", ""),
        )

        mono = QFont("Courier", 11)

        # ══ 中央ウィジェット ══
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setSpacing(4)
        root.setContentsMargins(8, 8, 8, 4)

        # ── 共通設定バー（全タブ共通）──
        cfg_box = QGroupBox("設定")
        cfg_row = QHBoxLayout(cfg_box)
        cfg_row.setSpacing(6)

        cfg_row.addWidget(QLabel("CoreS3 IP:"))
        self.e_ip = QLineEdit()
        self.e_ip.setPlaceholderText("例: 192.168.1.100")
        self.e_ip.setFixedWidth(160)
        self.e_ip.setText(self.cfg.get("ip", ""))
        cfg_row.addWidget(self.e_ip)

        cfg_row.addWidget(QLabel("Port:"))
        self.e_port = QLineEdit(str(self.cfg.get("port", 80)))
        self.e_port.setFixedWidth(55)
        cfg_row.addWidget(self.e_port)

        btn_save_cfg = QPushButton("💾 保存")
        btn_save_cfg.clicked.connect(self._save_cfg)
        cfg_row.addWidget(btn_save_cfg)

        self.lbl_cfg = QLabel("← IPを入力して保存")
        cfg_row.addWidget(self.lbl_cfg)
        cfg_row.addStretch()
        root.addWidget(cfg_box)

        # ── 1ウィンドウ・3タブ構成 ──
        self.tabs = QTabWidget()
        root.addWidget(self.tabs, stretch=1)

        # === 🌐 KariPom Lab タブ ===
        lab_page = QWidget()
        lab_lay = QVBoxLayout(lab_page)
        lab_lay.setContentsMargins(8, 8, 8, 8)

        lab_toolbar = QHBoxLayout()
        self.lbl_lab_url = QLabel("CoreS3 IPを保存するとKariPom Labを表示します")
        lab_toolbar.addWidget(self.lbl_lab_url, stretch=1)
        self.btn_lab_reload = QPushButton("↻ 再読込")
        self.btn_lab_reload.clicked.connect(self._reload_lab)
        lab_toolbar.addWidget(self.btn_lab_reload)
        self.btn_lab_external = QPushButton("外部ブラウザで開く")
        self.btn_lab_external.clicked.connect(self._open_lab_external)
        lab_toolbar.addWidget(self.btn_lab_external)
        lab_lay.addLayout(lab_toolbar)

        if WEBENGINE_AVAILABLE:
            self.lab_view = QWebEngineView()
            lab_lay.addWidget(self.lab_view, stretch=1)
        else:
            self.lab_view = None
            no_web = QLabel(
                "KariPom Labのタブ内表示には PyQtWebEngine が必要です。\n"
                "pip install PyQtWebEngine\n\n"
                "未導入でも、右上の『外部ブラウザで開く』からKariPom Labを利用できます。"
            )
            no_web.setAlignment(Qt.AlignCenter)
            no_web.setStyleSheet("color: gray; font-size: 13px;")
            lab_lay.addWidget(no_web, stretch=1)

        self.tabs.addTab(lab_page, "🌐 KariPom Lab")

        # === 🐰 KariPom BBX タブ ===
        face_page = QWidget()
        face_lay = QVBoxLayout(face_page)
        face_lay.setContentsMargins(12, 12, 12, 12)
        face_lay.addStretch()
        self.face_widget = KariPomFaceWidget(self._audio_state, scale=2)
        face_row = QHBoxLayout()
        face_row.addStretch()
        face_row.addWidget(self.face_widget)
        face_row.addStretch()
        face_lay.addLayout(face_row)
        self.lbl_face_mode = QLabel("Face → 7 Visualizers → Face：画面クリック")
        self.lbl_face_mode.setAlignment(Qt.AlignCenter)
        self.lbl_face_mode.setStyleSheet("color: gray; font-size: 11px;")
        face_lay.addWidget(self.lbl_face_mode)
        self.lbl_face_status = QLabel("PC音声を待機中")
        self.lbl_face_status.setAlignment(Qt.AlignCenter)
        self.lbl_face_status.setStyleSheet("color: gray; font-size: 12px;")
        face_lay.addWidget(self.lbl_face_status)
        face_lay.addStretch()
        self.tabs.addTab(face_page, "🐰 KariPom BBX")

        # === 💬 Talk タブ ===
        talk_page = QWidget()
        talk_lay = QVBoxLayout(talk_page)
        talk_lay.setContentsMargins(12, 12, 12, 12)

        talk_row = QHBoxLayout()
        talk_row.setSpacing(6)
        self.lbl_talk_status = QLabel("Talk：確認中…")
        talk_row.addWidget(self.lbl_talk_status)
        self.btn_talk_toggle = QPushButton("…")
        self.btn_talk_toggle.clicked.connect(self._talk_toggle)
        talk_row.addWidget(self.btn_talk_toggle)
        talk_row.addStretch()
        talk_lay.addLayout(talk_row)

        lbl_talk_note = QLabel("※ TalkはIP未設定でもPC音声を解析します。IP保存後はCoreS3への送信先も自動反映されます。")
        lbl_talk_note.setStyleSheet("color: gray; font-size: 11px;")
        talk_lay.addWidget(lbl_talk_note)
        talk_lay.addStretch()
        self.tabs.addTab(talk_page, "💬 Talk")

        # === 🚽 Log / 健康診断 タブ ===
        log_page = QWidget()
        log_root = QVBoxLayout(log_page)
        log_root.setSpacing(4)
        log_root.setContentsMargins(8, 8, 8, 8)

        tb = QHBoxLayout()
        tb.setSpacing(6)
        for text, slot in [
            ("① Current取得",   self._fetch_current),
            ("① Prev取得",       self._fetch_prev),
            ("② 健康診断",       self._kenben),
            ("③ 時間帯別",        self._hourly_next),
            ("④ サマリーコピー", self._copy_summary),
            ("⑤ 保存",           self._save_summary),
        ]:
            b = QPushButton(text)
            b.clicked.connect(slot)
            tb.addWidget(b)
        tb.addStretch()
        log_root.addLayout(tb)

        lm = QHBoxLayout()
        lm.setSpacing(6)
        lm.addWidget(QLabel("ログ管理:"))
        for text, slot in [
            ("Compress Current",  lambda: self._compress_log(CURRENT_LOG_PATH)),
            ("Clear Current",     lambda: self._clear_log(CURRENT_LOG_PATH)),
            ("Compress Previous", lambda: self._compress_log(PREVIOUS_LOG_PATH)),
            ("Clear Previous",    lambda: self._clear_log(PREVIOUS_LOG_PATH)),
        ]:
            b = QPushButton(text)
            b.clicked.connect(slot)
            lm.addWidget(b)
        lm.addStretch()
        log_root.addLayout(lm)

        sb = QHBoxLayout()
        sb.addWidget(QLabel("Raw Log 検索:"))
        self.e_search = QLineEdit()
        self.e_search.setPlaceholderText("2文字以上で自動検索")
        self.e_search.setFixedWidth(260)
        self.e_search.textChanged.connect(self._search)
        sb.addWidget(self.e_search)
        sb.addStretch()
        log_root.addLayout(sb)

        splitter = QSplitter(Qt.Horizontal)

        left_box = QGroupBox("健診結果")
        left_lay = QVBoxLayout(left_box)
        self.kb_txt = QTextEdit()
        self.kb_txt.setReadOnly(True)
        self.kb_txt.setFont(mono)
        left_lay.addWidget(self.kb_txt)
        splitter.addWidget(left_box)

        right_box = QGroupBox("Raw Log")
        right_lay = QVBoxLayout(right_box)
        self.raw_txt = QTextEdit()
        self.raw_txt.setReadOnly(True)
        self.raw_txt.setFont(mono)
        right_lay.addWidget(self.raw_txt)
        splitter.addWidget(right_box)

        splitter.setSizes([400, 800])
        log_root.addWidget(splitter, stretch=1)

        cb = QHBoxLayout()
        b_prev = QPushButton("◀ 前の時間帯")
        b_prev.clicked.connect(self._hourly_prev)
        cb.addWidget(b_prev)
        self.lbl_chunk = QLabel("　")
        self.lbl_chunk.setAlignment(Qt.AlignCenter)
        cb.addWidget(self.lbl_chunk, stretch=1)
        b_next = QPushButton("次の時間帯 ▶")
        b_next.clicked.connect(self._hourly_next)
        cb.addWidget(b_next)
        log_root.addLayout(cb)

        self.tabs.addTab(log_page, "🚽 Log / 健康診断")

        # タブ順: Lab → Talk → Log → KariPom。KariPomはPC単独のお試し/モニター用途なので最後。
        # 現在の追加順は Lab, KariPom, Talk, Log のため、KariPomを末尾へ移動する。
        self.tabs.tabBar().moveTab(1, 3)
        self.tabs.setCurrentIndex(0)

        self.statusBar().showMessage("IPアドレスを入力して「保存」")

        # Talk自動起動・状態監視。タブを切り替えても処理は止めない。
        self._talk_status_timer = QTimer(self)
        self._talk_status_timer.timeout.connect(self._talk_poll_status)
        self._talk_status_timer.start(250)
        self._talk_autostart()
        QTimer.singleShot(300, lambda: self._load_lab_from_current_config(switch_to_lab=False))

    # ─── 設定 ────────────────────────────────

    def _save_cfg(self):
        ip = self.e_ip.text().strip()
        try:
            port = int(self.e_port.text().strip())
        except ValueError:
            self.lbl_cfg.setText("⚠ Portは数値で")
            return
        self.cfg["ip"]   = ip
        self.cfg["port"] = port
        save_config(self.cfg)
        self.lbl_cfg.setText(f"✅ 保存済み  IP={ip}")
        self.statusBar().showMessage(f"設定保存  IP={ip}  Port={port}")

        # IP保存のたびにCompanion内のKariPom Labタブを更新する。
        # 外部ブラウザは自動起動しない（必要な場合はLabタブのボタンから開く）。
        if ip:
            QTimer.singleShot(0, lambda: self._load_lab_from_current_config(switch_to_lab=True))

    def _current_lab_url(self):
        ip = str(self.cfg.get("ip", "") or "").strip()
        try:
            port = int(self.cfg.get("port", 80))
        except Exception:
            port = 80
        if not ip:
            return ""
        return f"http://{ip}:{port}/"

    def _load_lab_from_current_config(self, switch_to_lab=False):
        url = self._current_lab_url()
        if not url:
            self.lbl_lab_url.setText("CoreS3 IPを保存するとKariPom Labを表示します")
            return
        self.lbl_lab_url.setText(url)
        if self.lab_view is not None:
            self.lab_view.setUrl(QUrl(url))
        if switch_to_lab:
            self.tabs.setCurrentIndex(0)
        self.statusBar().showMessage(f"KariPom Lab: {url}")

    def _reload_lab(self):
        if self.lab_view is not None and self._current_lab_url():
            self.lab_view.reload()
        else:
            self._load_lab_from_current_config(switch_to_lab=False)

    def _open_lab_external(self):
        url = self._current_lab_url()
        if not url:
            QMessageBox.information(self, "KariPom Lab", "先にCoreS3 IPを入力して保存してください。")
            return
        try:
            if platform.system() == "Darwin":
                # ChromeでAP(192.168.4.1)へ到達できないmacOS環境が確認されたためSafariを使用。
                subprocess.Popen(["open", "-a", "Safari", url])
            else:
                webbrowser.open(url)
        except Exception as e:
            QMessageBox.warning(self, "ブラウザ起動エラー", f"ブラウザを開けませんでした。\n\n{e}")

    def _st(self, msg):
        self.statusBar().showMessage(msg)

    # ─── Talk連携（Companionへ完全内蔵）────────────────
    # 別プロセスは一切起動しない。PC音声取得・発話判定・FFT・UDP送信は
    # EmbeddedTalkEngineがバックグラウンドで担当する。

    def _talk_is_running(self):
        return self._talk_engine.is_running()

    def _talk_update_ui(self):
        running = self._talk_is_running()
        speaking, rms, error, _fft = self._audio_state.snapshot()
        if running:
            self.lbl_talk_status.setText("Talk：動作中")
            self.btn_talk_toggle.setText("停止")
            if error:
                self.lbl_face_status.setText(f"Audio Error: {error}")
            elif speaking:
                src = self._audio_state.source()
                self.lbl_face_status.setText(f"Talk：発話中  RMS={rms:.4f}" + (f"  [{src}]" if src else ""))
            else:
                src = self._audio_state.source()
                self.lbl_face_status.setText(f"Talk：待機中  RMS={rms:.4f}" + (f"  [{src}]" if src else ""))
        else:
            self.lbl_talk_status.setText("Talk：停止中")
            self.btn_talk_toggle.setText("開始")
            self.lbl_face_status.setText("Talk：停止中")

    def _talk_autostart(self):
        # IPの有無にかかわらず、PC音声解析とPC側KariPomは起動する。
        # IPが空ならUDP送信だけを行わない。
        self._talk_start(silent=True)
        self._talk_update_ui()

    def _talk_poll_status(self):
        self._talk_update_ui()

    def _talk_toggle(self):
        if self._talk_is_running():
            self._talk_stop()
        else:
            self._talk_start(silent=False)
        self._talk_update_ui()

    def _talk_start(self, silent=False):
        if self._talk_is_running():
            return
        try:
            self._talk_engine.start()
            self._st("✅ TalkをCompanion内で開始しました")
        except Exception as e:
            if not silent:
                QMessageBox.critical(self, "Talk起動エラー", f"Talkの起動に失敗しました。\n{e}")
            self._st(f"❌ Talk起動失敗: {e}")

    def _talk_stop(self):
        if self._talk_engine is not None:
            self._talk_engine.stop()
        self._st("Talkを停止しました")

    def closeEvent(self, event):
        self._talk_stop()
        event.accept()

    # ─── ① 取得 ─────────────────────────────

    def _fetch_current(self):
        self._fetch("current")

    def _fetch_prev(self):
        self._fetch("prev")

    def _fetch(self, file="current"):
        ip   = self.cfg.get("ip", "").strip()
        port = self.cfg.get("port", 80)
        if not ip:
            QMessageBox.warning(self, "IP未設定",
                "IPアドレスを入力して「保存」を押してから取得してください。")
            return
        self._st(f"取得中… http://{ip}:{port}/logtoiletview?file={file}")
        self._fetch_file = file
        self._fetch_thread = FetchThread(ip, port, file)
        self._fetch_thread.done.connect(self._on_fetch_done)
        self._fetch_thread.error.connect(self._on_fetch_error)
        self._fetch_thread.start()

    def _on_fetch_done(self, text):
        self.log_text = text
        if self._fetch_file == "prev":
            save_path = PREVIOUS_LOG_PATH
        else:
            if os.path.exists(CURRENT_LOG_PATH):
                import shutil
                shutil.copy2(CURRENT_LOG_PATH, PREVIOUS_LOG_PATH)
            save_path = CURRENT_LOG_PATH
        with open(save_path, "w", encoding="utf-8") as f:
            f.write(text)
        self._displayed_log_path = save_path
        self.raw_txt.setPlainText(text)
        self.hourly_summaries = []
        self.hourly_idx = 0

        lines = len(text.splitlines())

        # 自動で検便＆時間帯別サマリー生成
        self._kenben()
        self.hourly_summaries = make_hourly_summaries(self.kenben_res, self.log_text)
        self.hourly_idx = 0
        h = len(self.hourly_summaries)
        self._st(f"✅ 取得成功  {lines}行  {h}時間帯に分割  → 「③ 時間帯別」を押してChatGPTへ貼ってください")
        if self.hourly_summaries:
            _, _, t0, t1 = self.hourly_summaries[0]
            self.lbl_chunk.setText(f"  1/{h}:  {t0}  〜  {t1}  （「③ 時間帯別」を押してコピー）")

    def _on_fetch_error(self, err):
        if err == "EMPTY_LOG":
            self._st("ℹ️ ログはまだありません（Clear直後、または新規ログの可能性があります）")
            QMessageBox.information(self, "ログなし",
                "CoreS3側にログがまだありません。\n"
                "Clear直後、または新規ログの可能性があります。\n"
                "しばらく待ってから再度お試しください。")
            return
        self._st(f"❌ 取得失敗: {err}")
        QMessageBox.critical(self, "取得エラー", err)

    # ─── 検索 ────────────────────────────────

    def _search(self, q):
        cursor = self.raw_txt.textCursor()
        cursor.setPosition(0)
        self.raw_txt.setTextCursor(cursor)

        # 既存ハイライトをクリア
        fmt_clear = self.raw_txt.currentCharFormat()
        self.raw_txt.selectAll()
        plain = self.raw_txt.toPlainText()
        self.raw_txt.setPlainText(plain)

        if len(q) < 2:
            return

        from PyQt5.QtGui import QTextCharFormat
        hl_fmt = QTextCharFormat()
        hl_fmt.setBackground(QColor("yellow"))

        doc = self.raw_txt.document()
        cursor = doc.find(q)
        cnt = 0
        while not cursor.isNull():
            cursor.mergeCharFormat(hl_fmt)
            if cnt == 0:
                self.raw_txt.setTextCursor(cursor)
            cursor = doc.find(q, cursor)
            cnt += 1
        self._st(f"検索「{q}」: {cnt}件")

    # ─── ② 検便 ──────────────────────────────

    def _kenben(self):
        if not self.log_text:
            QMessageBox.warning(self, "", "先にCurrent Logを取得してください。")
            return
        self._st("解析中…")
        res = do_kenben(self.log_text)
        self.kenben_res = res
        out = [make_power_report(res["power"], res.get("pwr_snap"), res.get("power_events", [])), "",
               make_kenshinsho(res["health"], "解析対象全体"), ""]
        out += ["="*44,
               f" KariPom 健診結果  {datetime.now().strftime('%H:%M:%S')}",
               "="*44,
               f" 総行数  : {res['total']} 行",
               f" 時間範囲: {res['trange']}",
               "─"*44,
               " ■ キーワード集計（0件省略）"]
        for label, cnt in res["counts"].items():
            if cnt > 0:
                out.append(f"   {label:<28}: {cnt:>4}")
        out += ["─"*44, " ■ 重要イベント抽出"]
        if res["important"]:
            for label, line in res["important"]:
                out.append(f"  [{label:<12}] {line[:80]}")
        else:
            out.append("  （重要イベントなし）")
        out.append("="*44)
        self.kb_txt.setPlainText("\n".join(out))
        self._st(f"✅ 健康診断完了  重要イベント {len(res['important'])}件")

    # ─── ③ サマリーコピー ─────────────────────

    def _copy_summary(self):
        if not self.kenben_res:
            if self.log_text:
                self._kenben()
            else:
                QMessageBox.warning(self, "", "先にCurrent Logを取得してください。")
                return
        pbcopy(make_summary(self.kenben_res))
        self._st("✅ サマリーをクリップボードにコピーしました")

    # ─── 時間帯別サマリー ────────────────────

    def _hourly_prev(self):
        if not self.hourly_summaries:
            return
        if self.hourly_idx > 1:
            self.hourly_idx -= 2  # next で+1されるので2戻す
        else:
            self.hourly_idx = 0
        self._hourly_copy()

    def _hourly_next(self):
        if not self.log_text:
            QMessageBox.warning(self, "", "先にCurrent Logを取得してください。")
            return
        # 初回または再生成
        if not self.hourly_summaries:
            self.hourly_summaries = make_hourly_summaries(self.kenben_res, self.log_text)
            self.hourly_idx = 0
            self._st(f"時間帯別サマリー {len(self.hourly_summaries)}時間分を生成しました")

        if not self.hourly_summaries:
            return

        self._hourly_copy()
        if self.hourly_idx < len(self.hourly_summaries) - 1:
            self.hourly_idx += 1

    def _hourly_copy(self):
        if not self.hourly_summaries:
            return
        idx = min(self.hourly_idx, len(self.hourly_summaries)-1)
        text, key, time_from, time_to = self.hourly_summaries[idx]
        pbcopy(text)
        # 健診結果パネルにも、この時間帯だけを対象にした健康診断+ログ解析を表示する。
        # （これまでは② 健康診断で取得した「解析対象全体」の結果が表示されたままだった）
        self.kb_txt.setPlainText(text)
        current = idx + 1
        total   = len(self.hourly_summaries)
        self._st(f"✅ 時間帯 {key}xx ({current}/{total}) をコピーしました  → ChatGPTに貼ってください")
        self.lbl_chunk.setText(f"  {current}/{total}:  {time_from}  〜  {time_to}  ")

    # ─── ④ Chunk作成 ─────────────────────────

    def _make_chunks(self):
        if not self.log_text:
            QMessageBox.warning(self, "", "先にCurrent Logを取得してください。")
            return
        n = self.cfg.get("chunk_lines", CHUNK_SIZE)
        self.chunks    = split_chunks(self.log_text, n)
        self.chunk_idx = 0
        for i, chunk in enumerate(self.chunks, 1):
            path = os.path.join(BASE_DIR, f"karipom_chunk{i}.txt")
            with open(path, "w", encoding="utf-8") as f:
                if self.kenben_res:
                    f.write(make_summary(self.kenben_res) + "\n\n")
                    f.write(f"=== Chunk {i}/{len(self.chunks)} ===\n")
                f.write(chunk)
                if i == len(self.chunks):
                    f.write("\n\nこのログを総合分析してください。")
        self._upd_chunk_lbl()
        self._st(f"✅ {len(self.chunks)}チャンク作成  → ▶ 次+コピー で順番に貼ってください")

    def _upd_chunk_lbl(self):
        if not self.chunks:
            self.lbl_chunk.setText("")
            return
        self.lbl_chunk.setText(f"Chunk {self.chunk_idx+1} / {len(self.chunks)}")

    def _next_copy(self):
        if not self.chunks:
            QMessageBox.warning(self, "", "先にChunk作成してください。")
            return
        self._copy_chunk()
        if self.chunk_idx < len(self.chunks) - 1:
            self.chunk_idx += 1
        self._upd_chunk_lbl()

    def _prev_chunk(self):
        if not self.chunks:
            return
        if self.chunk_idx > 0:
            self.chunk_idx -= 1
        self._upd_chunk_lbl()

    def _copy_chunk(self):
        if not self.chunks:
            QMessageBox.warning(self, "", "先にChunk作成してください。")
            return
        idx = self.chunk_idx
        pre = (make_summary(self.kenben_res) + f"\n\n=== Chunk {idx+1}/{len(self.chunks)} ===\n"
               if self.kenben_res else "")
        suf = "\n\nこのログを総合分析してください。" if idx == len(self.chunks) - 1 else ""
        pbcopy(pre + self.chunks[idx] + suf)
        self._st(f"✅ Chunk {idx+1}/{len(self.chunks)} をコピーしました")

    # ─── ログ管理 ─────────────────────────────

    def _compress_log(self, path):
        name = os.path.basename(path)
        if not os.path.exists(path):
            self._st(f"⚠ ファイルなし: {name}")
            return
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        if len(lines) <= 500:
            self._st(f"⚠ {name} は500行以下のため圧縮不要（{len(lines)}行）")
            return
        kept = lines[-500:]
        with open(path, "w", encoding="utf-8") as f:
            f.writelines(kept)
        if self._displayed_log_path == path:
            text = "".join(kept)
            self.raw_txt.setPlainText(text)
            self.log_text = text
        self._st(f"✅ Compress完了: {name}  {len(lines)}行 → 500行")

    def _clear_log(self, path):
        # 従来はMac側の取得済みファイルと画面表示だけを空にしており、
        # CoreS3のSDカード上の元ログ（karipom.log / karipom_prev.log）は
        # 削除されていなかった（＝再取得すると同じ内容が復活する原因）。
        # CoreS3には既存の /clear_current_log・/clear_prev_log エンドポイントが
        # あり（Web Cockpitの🗑Clearボタンと同じ処理）、それを呼び出してから
        # Mac側を空にするよう変更する。
        name = os.path.basename(path)
        ans = QMessageBox.question(
            self, "確認", f"「{name}」の内容を空にします。よろしいですか？\n"
                          "CoreS3上のログも削除されます。",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No
        )
        if ans != QMessageBox.Yes:
            return

        is_prev  = (path == PREVIOUS_LOG_PATH)
        endpoint = "/clear_prev_log" if is_prev else "/clear_current_log"
        label    = "Previous" if is_prev else "Current"

        ip   = self.cfg.get("ip", "").strip()
        port = self.cfg.get("port", 80)
        if not ip:
            QMessageBox.warning(self, "IP未設定",
                "IPアドレスを入力して「保存」を押してから実行してください。")
            return

        try:
            resp = requests.get(f"http://{ip}:{port}{endpoint}",
                                 timeout=10, allow_redirects=False)
        except Exception as e:
            self._st(f"❌ CoreS3側のClearに失敗しました: {e}")
            QMessageBox.critical(self, "Clearエラー",
                f"CoreS3への削除リクエストに失敗しました。\n{e}")
            return

        # /clear_current_log・/clear_prev_log は成功時302リダイレクトを返す。
        if resp.status_code not in (200, 302):
            self._st(f"❌ CoreS3側のClearに失敗しました（HTTP {resp.status_code}）")
            QMessageBox.critical(self, "Clearエラー",
                f"CoreS3が削除に失敗しました（HTTP {resp.status_code}）。")
            return

        # CoreS3側の削除を確認できてから、Mac側の取得済みファイルと画面表示を空にする。
        open(path, "w", encoding="utf-8").close()
        if self._displayed_log_path == path:
            self.raw_txt.setPlainText("")
            self.log_text = ""
        self._st(f"✅ {label} log cleared on CoreS3")

    # ─── ⑤ 保存 ──────────────────────────────

    def _save_summary(self):
        if not self.kenben_res:
            QMessageBox.warning(self, "", "先に健康診断を実行してください。")
            return
        with open(SUMMARY_PATH, "w", encoding="utf-8") as f:
            f.write(make_summary(self.kenben_res) + "\n\n=== Raw Log ===\n" + self.log_text)
        self._st(f"✅ 保存完了 → {SUMMARY_PATH}")


# =============================================================================
# ここから Talk 統合部分
# 音声取得・RMS/FFT解析・SPEAK_START/STOP・HEARTBEAT・UDP送信のロジックは
# karipom_talk_20260719_multiOS.py の実装をそのまま維持している（挙動変更なし）。
# 唯一の変更点は、
#   ・IP設定を共通の load_config()/save_config()（"ip"キー）に統一したこと
#   ・音声ライブラリ(numpy/sounddevice/soundcard)のimportと、実際の起動処理を
#     run_talk_mode() 関数内に閉じ込め、Companion GUI起動時には一切実行されない
#     ようにしたこと（未導入環境でもGUIが問題なく起動できるようにするため）
# の2点のみ。
# =============================================================================

# ===== OS判定 =====
OS_NAME = platform.system()          # "Darwin" / "Windows" / "Linux"
IS_MAC     = (OS_NAME == "Darwin")
IS_WINDOWS = (OS_NAME == "Windows")
IS_LINUX   = (OS_NAME == "Linux")

# 音声ライブラリの実体は run_talk_mode() 内で遅延importし、ここへ代入する。
# （Companion GUI起動時にimportエラーで落ちないようにするため。モジュール定義側は
#  従来どおり "sd." "sc." という書き方のままにしてあり、ロジックは変更していない）
np = None
sd = None
sc = None

M5_PORT = 12345   # Talk UDP送信先ポート（既存のCoreS3側.ino仕様のため変更しない）

# Companion GUIで「保存」を押した際、Talk停止→開始をせずに送信先IPをその場で
# 切り替えるための定期再読込設定。karipom_config.json の "ip" だけを見に行く軽量な
# チェックで、音声取得・FFT・UDP送信そのものには手を入れない。
IP_RELOAD_INTERVAL = 1.0    # この秒数おきにkaripom_config.jsonの"ip"を確認する
_last_ip_reload_check = 0.0

THRESHOLD = 0.02            # 反応しすぎるなら上げる。反応しないなら下げる
SILENCE_TIMEOUT = 5.0       # 曲間・会話の間で止まらないための通常停止判定
HARD_STOP_RMS = 0.0005      # ほぼ完全無音とみなす音量。曲を手動停止した時の早期STOP用
HARD_STOP_TIMEOUT = 0.8     # ほぼ完全無音がこの秒数続いたら早めにSTOP
STATUS_SEND_INTERVAL = 2.0  # 現在状態を再送信する間隔
BLOCKSIZE = 2048
SAMPLERATE = 44100

# ===== FFT設定（Visualizer Face 第一段階：PC側での解析・表示） =====
# FFT_ENABLED = False にすると従来と完全に同じ動作・負荷になる。
# 起動オプション --fft / --no-fft でも切り替え可能（定数より優先。run_talk_mode()内で反映）。
FFT_ENABLED = True
FFT_UPDATE_INTERVAL = 0.08   # 表示・送信の更新間隔（約12.5回/秒）
FFT_BANDS_HZ = [60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000]  # 8バンドの境界
FFT_DB_RANGES = [
    (-60, -10),   # Band 0:  60- 120 Hz
    (-62, -12),   # Band 1: 120- 250 Hz
    (-63, -13),   # Band 2: 250- 500 Hz
    (-65, -15),   # Band 3: 500-1000 Hz
    (-65, -15),   # Band 4: 1k - 2k  Hz
    (-70, -20),   # Band 5: 2k - 4k  Hz
    (-78, -28),   # Band 6: 4k - 8k  Hz
    (-85, -35),   # Band 7: 8k -16k  Hz
]
FFT_ATTACK = 0.6             # バー上昇の平滑化係数（大きいほど速く追従）
FFT_DECAY = 0.18             # バー下降の平滑化係数（小さいほどゆっくり減衰）

# ===== FFT送信設定（Visualizer Face 第二段階：CoreS3への送信） =====
FFT_SEND_ENABLED = True      # FFT解析結果（8バンド・0〜100）をCoreS3へUDP送信するか
FFT_SEND_PREFIX = "FFT:"     # メッセージ識別子
FFT_SEND_RETRY_WAIT = 5.0    # 連続送信失敗時に送信を一時停止する秒数（自動再開する）

# --input <名前> で使用する入力デバイスヒント（Windows/Linux）。run_talk_mode()内で設定。
INPUT_HINT = None

M5_IP = None
sock = None

speaking = False
last_loud_time = 0.0
last_status_sent_time = 0.0
hard_silence_start_time = None

# ===== ストリーム自己復旧設定 =====
# 入力ストリームが曲切替などで silently 停止するケースを検出し、
# 自動的に再オープンする（Mac/Windows/Linux共通の考え方）。
STREAM_WATCHDOG_TIMEOUT = 10.0  # コールバックが来なくなってから再起動するまでの秒数
STREAM_RESTART_WAIT     = 2.0   # 再起動前の待機秒数（デバイス安定待ち）
_last_callback_time     = 0.0   # 音声ブロック最終受信時刻（time.time()）

# ===== FFT用の共有状態 =====
# 音声取得側は最新1ブロックをコピーして置くだけ。
# FFT・正規化・平滑化・描画・送信はすべてメインループ側で行う。
_fft_lock = threading.Lock()
_fft_latest_block = None
_fft_send_error_count = 0
_fft_send_pause_until = 0.0  # この時刻まで送信を一時停止（連続失敗時の自動リトライ用）

# FFT係数配列（_fft_window / _fft_freqs / _fft_band_bins / _fft_levels / _FFT_BAR_CHARS）は
# numpyの遅延importに合わせ、run_talk_mode() 内でFFT_ENABLED確定後に生成する。
_fft_window = None
_fft_freqs = None
_fft_band_bins = None
_fft_levels = None
_FFT_BAR_CHARS = None

_capture_generation = 0  # 世代番号。増えたら古い取得スレッドは自然終了する


# ===== 設定ファイルからのIP読み込み（Talk用・共通load_config/save_configを利用）=====
def ask_ip_for_talk():
    """Talk起動時のIP確認・入力。
    従来のTalk単体版 ask_m5_ip() と同じ挙動（前回IP表示 / Enterだけなら前回IPを使用 /
    新しいIPを入力した場合は保存）を維持しつつ、karipom_config.json全体を上書きせず
    "ip" キーだけを更新する（Companion側の port / chunk_lines 等は保持する）。"""
    cfg = load_config()
    last_ip = cfg.get("ip", "")

    print()
    print("=================================")
    print("かりポム接続設定")
    print("かりポムのおでこに表示されたIPアドレスを入力してください")
    print(f"前回IP: {last_ip}")
    print("Enterだけなら前回IPを使用します。")
    print("=================================")

    ip = input("IPアドレス: ").strip()

    if ip == "":
        ip = last_ip

    if ip == "":
        print("IPアドレスが入力されていません。")
        sys.exit(1)

    cfg["ip"] = ip
    save_config(cfg)
    return ip


# ===== デバイス検索（OS依存部・従来どおり）=====
def find_blackhole_device():
    # Mac専用（従来どおり）
    devices = sd.query_devices()

    for i, d in enumerate(devices):
        if "BlackHole" in d["name"]:
            return i, d["name"]

    return None, None


def _is_loopback_mic(m):
    # Windows: isloopback属性 / Linux: Pulseのモニターソース（id末尾 .monitor）
    if getattr(m, "isloopback", False):
        return True
    return str(getattr(m, "id", "")).endswith(".monitor")


def find_loopback_mic():
    """Windows/Linux専用。PC再生音を取得できる入力（ループバック/モニター）を探す。
    優先順位:
      1. --input で名前指定されたデバイス
      2. 既定スピーカーに対応するループバック/モニター
      3. 最初に見つかったループバック/モニター
    """
    mics = sc.all_microphones(include_loopback=True)

    if INPUT_HINT:
        for m in mics:
            if INPUT_HINT.lower() in m.name.lower():
                return m
        print(f"--input '{INPUT_HINT}' に一致するデバイスが見つかりません。")
        return None

    loopbacks = [m for m in mics if _is_loopback_mic(m)]

    try:
        spk_name = sc.default_speaker().name
    except Exception:
        spk_name = None

    if spk_name:
        for m in loopbacks:
            if spk_name in m.name:
                return m

    if loopbacks:
        return loopbacks[0]

    return None


def print_devices():
    if IS_MAC:
        print(sd.query_devices())
    else:
        print("=== 音声デバイス一覧（[loopback]がPC再生音の取得元候補）===")
        for m in sc.all_microphones(include_loopback=True):
            mark = "[loopback]" if _is_loopback_mic(m) else "[mic]     "
            print(f"{mark} {m.name}")
        try:
            print(f"既定スピーカー: {sc.default_speaker().name}")
        except Exception as e:
            print(f"既定スピーカー: 取得失敗 ({e})")


def send_message(message: bytes) -> bool:
    # CoreS3 IP未設定時はPC側の音声解析だけ継続し、UDP送信は行わない。
    # 成功時True / 未送信・失敗時False を返す。
    if not M5_IP:
        return False
    try:
        sock.sendto(message, (M5_IP, M5_PORT))
        return True
    except Exception as e:
        print("UDP send error:", e)
        return False


def send_status(is_speaking: bool, rms: float, reason: str = "CHANGE"):
    # reason: "CHANGE"=状態変化, "HEARTBEAT"=定期再送
    if is_speaking:
        send_message(b"SPEAK_START")
        print(f"▶ SPEAK_START  rms={rms:.5f} [{reason}]")
    else:
        send_message(b"SPEAK_STOP")
        print(f"■ SPEAK_STOP   rms={rms:.5f} [{reason}]")


def _maybe_reload_ip():
    """karipom_config.jsonの"ip"が変わっていないか定期的に確認し、変わっていれば
    UDP送信先(M5_IP)をその場で切り替える。send_message()は毎回この時点のM5_IPを
    参照するだけなので、ここでM5_IPを更新するだけでTalkの停止→開始なしに送信先が
    切り替わる（IPの参照元はkaripom_config.json一本のまま）。
    IP_RELOAD_INTERVAL秒に一度しかファイルを読まないため、監視ループ（Mac/Windows/
    Linux共通で毎ティック呼ばれる）に置いても負荷は小さい。音声取得・FFT・UDP送信
    そのものには手を入れていない。"""
    global M5_IP, _last_ip_reload_check

    now = time.time()
    if now - _last_ip_reload_check < IP_RELOAD_INTERVAL:
        return
    _last_ip_reload_check = now

    try:
        cfg = load_config()
    except Exception:
        return  # 読み込みに失敗しても致命的ではない。次回のチェックで再試行される。

    new_ip = cfg.get("ip", "").strip()
    if new_ip != M5_IP:
        before = M5_IP or "未設定"
        after = new_ip or "未設定"
        print(f"\n[IP] 送信先IPを更新しました: {before} → {after}", file=sys.stderr)
        M5_IP = new_ip


def fft_update_and_draw():
    # メインループ専用。音声取得スレッドからは呼ばない。
    global _fft_levels, _fft_send_error_count, _fft_send_pause_until

    with _fft_lock:
        block = _fft_latest_block

    if block is None:
        target = np.zeros(len(_fft_levels))
    else:
        spec = np.abs(np.fft.rfft(block * _fft_window)) / (BLOCKSIZE / 2)
        band_db = np.array([
            20.0 * np.log10(np.sqrt(np.mean(spec[idx] ** 2)) + 1e-12)
            for idx in _fft_band_bins
        ])
        db_min = np.array([r[0] for r in FFT_DB_RANGES])
        db_max = np.array([r[1] for r in FFT_DB_RANGES])
        target = np.clip((band_db - db_min) / (db_max - db_min), 0.0, 1.0) * 100.0

    # 上昇は速く、下降はゆっくり。無音時は自然に0へ減衰する。
    coef = np.where(target > _fft_levels, FFT_ATTACK, FFT_DECAY)
    _fft_levels = _fft_levels + coef * (target - _fft_levels)

    bars = "".join(
        _FFT_BAR_CHARS[min(8, int(v * 8.0 / 100.0 + 0.5))] for v in _fft_levels
    )
    nums = " ".join(f"{v:3.0f}" for v in _fft_levels)
    print(f"\rFFT|{bars}| {nums}  ", end="", flush=True)

    # FFT送信（表示と同じ約12.5回/秒）。失敗しても表示・口パクは継続する。
    # 連続失敗時はFFT_SEND_RETRY_WAIT秒だけ一時停止して自動的に再開する。
    if FFT_SEND_ENABLED and time.time() >= _fft_send_pause_until:
        try:
            msg = FFT_SEND_PREFIX + ",".join(
                str(int(v + 0.5)) for v in _fft_levels)
            ok = send_message(msg.encode("ascii"))
        except Exception as e:
            print(f"\nFFT send error: {e}", file=sys.stderr)
            ok = False

        if ok:
            _fft_send_error_count = 0
        else:
            _fft_send_error_count += 1
            if _fft_send_error_count >= 5:
                _fft_send_error_count = 0
                _fft_send_pause_until = time.time() + FFT_SEND_RETRY_WAIT
                print(f"\nFFT送信の連続失敗のため{FFT_SEND_RETRY_WAIT:.0f}秒間"
                      "一時停止します（自動再開・口パクとUDP送信は継続）",
                      file=sys.stderr)


# ===== OS共通の音声処理（中身はTalk単体版のcallbackと同一）=====
def process_block(mono_block):
    """1ブロック分のモノラル音声（float, -1.0〜1.0, 長さBLOCKSIZE）を処理する。
    音量判定・SPEAK_START/STOP・HEARTBEAT・FFT用ブロック共有を行う。
    Macではsounddeviceコールバックから、Windows/Linuxでは取得スレッドから呼ばれる。
    """
    global speaking, last_loud_time, last_status_sent_time, hard_silence_start_time
    global _fft_latest_block, _last_callback_time

    # ウォッチドッグ用：呼ばれるたびに更新。メインループがここを監視する。
    _last_callback_time = time.time()

    # FFT用に最新ブロックをコピーして共有するだけ（重い処理はメインループ側）
    if FFT_ENABLED:
        with _fft_lock:
            _fft_latest_block = mono_block.copy()

    rms = float(np.sqrt(np.mean(mono_block ** 2)))
    now = time.time()

    # 音がしきい値を超えたら即START扱い。ここは従来通り反応を速くする。
    if rms > THRESHOLD:
        last_loud_time = now
        hard_silence_start_time = None
        speaking_now = True

    else:
        # ほぼ完全無音なら、曲を止めたと判断して早めにSTOPする。
        # ただし小さな音・曲間の一瞬の無音では従来の5秒判定を使う。
        if rms < HARD_STOP_RMS:
            if hard_silence_start_time is None:
                hard_silence_start_time = now

            hard_silence_long_enough = (now - hard_silence_start_time) >= HARD_STOP_TIMEOUT
            normal_silence_long_enough = (now - last_loud_time) > SILENCE_TIMEOUT
            speaking_now = not (hard_silence_long_enough or normal_silence_long_enough)

        else:
            hard_silence_start_time = None
            speaking_now = (now - last_loud_time) <= SILENCE_TIMEOUT

    if speaking_now != speaking:
        send_status(speaking_now, rms)
        speaking = speaking_now
        last_status_sent_time = now
        return

    if now - last_status_sent_time >= STATUS_SEND_INTERVAL:
        send_status(speaking_now, rms, "HEARTBEAT")
        last_status_sent_time = now


def callback(indata, frames, time_info, status):
    # Mac専用（sounddevice InputStreamコールバック・従来どおり）
    if status:
        # input_overflow は BlackHole / macOS の曲切替時に発生しやすい。
        # 口パクは継続するが、ウォッチドッグが停止検出した場合は自動再起動する。
        print("Audio status:", status, file=sys.stderr)

    process_block(indata[:, 0])


# ===== Mac: sounddevice ストリーム（従来どおり）=====
def _run_stream(device_id):
    """
    InputStream を開いてメインループを回す。
    ストリームが停止（stream.active=False または STREAM_WATCHDOG_TIMEOUT 秒間
    コールバックが呼ばれない）を検出した場合は return して呼び元に再起動を促す。
    KeyboardInterrupt が来たら例外をそのまま上に伝える。
    """
    global _last_callback_time, FFT_ENABLED

    _last_callback_time = time.time()   # 起動直後はタイムアウトさせない

    with sd.InputStream(
        device=device_id,
        channels=1,
        samplerate=SAMPLERATE,
        blocksize=BLOCKSIZE,
        callback=callback
    ) as stream:
        fft_error_count = 0
        while True:
            # ---- ウォッチドッグ ----
            if not stream.active:
                print("\n[watchdog] InputStream が非アクティブになりました。"
                      "再起動します...", file=sys.stderr)
                send_message(b"SPEAK_STOP")
                return  # 呼び元のループで再起動

            elapsed = time.time() - _last_callback_time
            if elapsed > STREAM_WATCHDOG_TIMEOUT:
                print(f"\n[watchdog] コールバック停止を検出"
                      f"（{elapsed:.1f}秒間未受信）。再起動します...",
                      file=sys.stderr)
                send_message(b"SPEAK_STOP")
                return  # 呼び元のループで再起動

            # ---- IP再読込（Companion側で「保存」されたIPをその場で反映） ----
            _maybe_reload_ip()

            # ---- FFT 処理（既存ロジック） ----
            if FFT_ENABLED:
                try:
                    fft_update_and_draw()
                    fft_error_count = 0
                except Exception as e:
                    fft_error_count += 1
                    print(f"\nFFT error: {e}", file=sys.stderr)
                    if fft_error_count >= 5:
                        FFT_ENABLED = False
                        print("FFTエラーが続いたため無効化しました"
                              "（口パクとUDP送信は継続します）", file=sys.stderr)
                time.sleep(FFT_UPDATE_INTERVAL)
            else:
                time.sleep(0.1)


# ===== Windows/Linux: soundcard 取得スレッド＋監視ループ（従来どおり）=====
def _capture_loop(mic, generation):
    """取得スレッド本体。ループバック/モニターからBLOCKSIZEずつ読み、
    モノラル化してprocess_block()へ渡す。例外はスレッド終了として
    メインの監視ループに検出させる。"""
    try:
        with mic.recorder(samplerate=SAMPLERATE, blocksize=BLOCKSIZE) as rec:
            while _capture_generation == generation:
                data = rec.record(numframes=BLOCKSIZE)
                if data.ndim > 1 and data.shape[1] > 1:
                    mono = data.mean(axis=1)
                else:
                    mono = data.reshape(-1)
                process_block(mono)
    except Exception as e:
        print(f"\n[watchdog] capture エラー: {type(e).__name__}: {e}",
              file=sys.stderr)


def _keepalive_loop():
    """既定スピーカーへ無音を再生し続けるスレッド。
    WASAPI Loopback／Pulseモニターは「何も再生されていないとデータが
    届かない・止まる」ことがあるため、これでMac版と同様に無音中も
    ブロックが流れ続け、HEARTBEATが維持される。音は一切鳴らない。"""
    zeros = np.zeros((BLOCKSIZE, 1), dtype="float32")
    while True:
        try:
            spk = sc.default_speaker()
            with spk.player(samplerate=SAMPLERATE, blocksize=BLOCKSIZE) as p:
                while True:
                    p.play(zeros)
        except Exception as e:
            print(f"\n[keepalive] 出力エラー: {type(e).__name__}: {e} "
                  "→ 5秒後に再開します", file=sys.stderr)
            time.sleep(5)


def _run_stream_soundcard(mic):
    """取得スレッドを起動し、Mac版_run_stream()と同じ監視・FFTループを回す。
    停止検出時は return して呼び元に再起動を促す。"""
    global _last_callback_time, FFT_ENABLED, _capture_generation

    _capture_generation += 1
    generation = _capture_generation
    _last_callback_time = time.time()   # 起動直後はタイムアウトさせない

    th = threading.Thread(target=_capture_loop, args=(mic, generation),
                          daemon=True)
    th.start()

    fft_error_count = 0
    while True:
        # ---- ウォッチドッグ ----
        if not th.is_alive():
            print("\n[watchdog] 取得スレッドが停止しました。再起動します...",
                  file=sys.stderr)
            send_message(b"SPEAK_STOP")
            return  # 呼び元のループで再起動

        elapsed = time.time() - _last_callback_time
        if elapsed > STREAM_WATCHDOG_TIMEOUT:
            print(f"\n[watchdog] 音声ブロック停止を検出"
                  f"（{elapsed:.1f}秒間未受信）。再起動します...",
                  file=sys.stderr)
            _capture_generation += 1  # 取得スレッドへ終了を指示
            send_message(b"SPEAK_STOP")
            return  # 呼び元のループで再起動

        # ---- IP再読込（Companion側で「保存」されたIPをその場で反映） ----
        _maybe_reload_ip()

        # ---- FFT 処理（Mac版と同一） ----
        if FFT_ENABLED:
            try:
                fft_update_and_draw()
                fft_error_count = 0
            except Exception as e:
                fft_error_count += 1
                print(f"\nFFT error: {e}", file=sys.stderr)
                if fft_error_count >= 5:
                    FFT_ENABLED = False
                    print("FFTエラーが続いたため無効化しました"
                          "（口パクとUDP送信は継続します）", file=sys.stderr)
            time.sleep(FFT_UPDATE_INTERVAL)
        else:
            time.sleep(0.1)


def run_talk_mode():
    """Talk音声連携モードの本体。
    karipom_talk_20260719_multiOS.py のモジュールレベル実行部分を、そのままの順序・
    ロジックで関数化したもの。Companion GUIモードでは一切呼び出されない。
    """
    global np, sd, sc
    global THRESHOLD, FFT_ENABLED, FFT_SEND_ENABLED, INPUT_HINT
    global M5_IP, sock
    global _fft_window, _fft_freqs, _fft_band_bins, _fft_levels, _FFT_BAR_CHARS
    global _capture_generation
    global _last_ip_reload_check

    # ===== 音声ライブラリの読み込み（Talkモード起動時のみ実行）=====
    #   Mac          : sounddevice（従来どおり）
    #   Windows/Linux: soundcard（WASAPI Loopback / Pulseモニター対応）
    import numpy as _np
    np = _np

    if IS_MAC:
        import sounddevice as _sd
        sd = _sd
    else:
        try:
            import soundcard as _sc
            sc = _sc
        except Exception as e:
            print("音声ライブラリ soundcard が読み込めません。")
            print("  pip install soundcard numpy")
            if IS_LINUX:
                print("Linuxの場合は libpulse も必要です:")
                print("  sudo apt install libpulse0   (PipeWire環境は pipewire-pulse も)")
            print(f"詳細: {e}")
            sys.exit(1)

    # Windowsコンソール対策:
    # FFTバー表示（▁▂▃…）がcp932コンソールでUnicodeEncodeErrorになるのを防ぐ。
    # Macでは実行されないため従来動作に影響しない。
    if IS_WINDOWS:
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    # ===== 起動オプション（sys.argvから解析。従来のTalk単体版と同じ書式）=====
    if "--fft" in sys.argv:
        FFT_ENABLED = True
    if "--no-fft" in sys.argv:
        FFT_ENABLED = False
    if "--fft-send" in sys.argv:
        FFT_SEND_ENABLED = True
    if "--no-fft-send" in sys.argv:
        FFT_SEND_ENABLED = False

    # --list-devices : 音声デバイス一覧を表示して終了（IP入力の前に実行される）
    # --input <名前> : Windows/Linuxで使用する入力デバイスを名前の一部で指定
    # --threshold <値>: 口パク開始しきい値を上書き（システム音量の影響を受ける環境向け）
    # --no-keepalive : Windows/Linuxのキープアライブ無音出力を無効化
    # --ip <値>      : Companion GUIからの自動起動用オプション。指定時はIP確認プロンプトを
    #                  出さず、そのままこの値を使用する（CLIから手動で talk を起動する場合は
    #                  通常指定不要で、従来どおり ask_ip_for_talk() の対話式プロンプトになる）。
    keepalive_enabled = ("--no-keepalive" not in sys.argv)
    companion_mode = ("--companion" in sys.argv)
    INPUT_HINT = None
    ip_override = None
    for _i, _a in enumerate(sys.argv):
        if _a == "--input" and _i + 1 < len(sys.argv):
            INPUT_HINT = sys.argv[_i + 1]
        if _a == "--threshold" and _i + 1 < len(sys.argv):
            try:
                THRESHOLD = float(sys.argv[_i + 1])
            except ValueError:
                print(f"--threshold の値が不正です: {sys.argv[_i + 1]}")
                sys.exit(1)
        if _a == "--ip" and _i + 1 < len(sys.argv):
            ip_override = sys.argv[_i + 1]

    if "--list-devices" in sys.argv:
        print_devices()
        sys.exit(0)

    if ip_override is not None:
        M5_IP = ip_override.strip()
    elif companion_mode:
        # Companion内ではIP未設定でもPC音声解析を開始する。
        # 保存後は_maybe_reload_ip()がconfigから送信先を自動反映する。
        try:
            M5_IP = load_config().get("ip", "").strip()
        except Exception:
            M5_IP = ""
    else:
        M5_IP = ask_ip_for_talk()

    # 起動直後の重複読込を避けるため、IP再読込のタイマーをここで起点にする
    # （_maybe_reload_ip()はIP_RELOAD_INTERVAL秒に一度しかファイルを読まない）。
    _last_ip_reload_check = time.time()

    sock = None  # finally句でのNameError防止のため先にNoneで初期化
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if FFT_ENABLED:
        _fft_window = np.hanning(BLOCKSIZE)
        _fft_freqs = np.fft.rfftfreq(BLOCKSIZE, 1.0 / SAMPLERATE)
        _fft_band_bins = [
            np.where((_fft_freqs >= lo) & (_fft_freqs < hi))[0]
            for lo, hi in zip(FFT_BANDS_HZ[:-1], FFT_BANDS_HZ[1:])
        ]
        _fft_levels = np.zeros(len(_fft_band_bins))
        _FFT_BAR_CHARS = " ▁▂▃▄▅▆▇█"

    # ===== 起動メッセージ・デバイス選択 =====
    if IS_MAC:
        print("=== Karipom Mac Audio Link ===")
        found_id, found_name = find_blackhole_device()

        if found_id is not None:
            print(f"Detected BlackHole: {found_name} = {found_id}")
        else:
            print("Detected BlackHole: NOT FOUND")

        if found_id is None:
            raise RuntimeError("BlackHole input device not found")

        device_id = found_id
        print(f"Using device: {device_id}")
        mic = None

    else:
        print(f"=== Karipom PC Audio Link ({OS_NAME}) ===")
        mic = find_loopback_mic()

        if mic is not None:
            print(f"Detected loopback: {mic.name}")
        else:
            print("Detected loopback: NOT FOUND")
            print_devices()
            if IS_WINDOWS:
                print("PC再生音を取得できるデバイスが見つかりません。")
                print("--input オプションでデバイス名の一部を指定してみてください。")
            else:
                print("モニターソースが見つかりません。PulseAudio/PipeWireが動作しているか、")
                print("pipewire-pulse がインストールされているか確認してください。")
            sys.exit(1)
        device_id = None

    print(f"Send to: {(M5_IP + ':' + str(M5_PORT)) if M5_IP else '未設定（PC側のみ）'}")
    print(f"Threshold: {THRESHOLD}")
    print(f"Silence timeout: {SILENCE_TIMEOUT}s")
    print(f"Hard stop: rms < {HARD_STOP_RMS} for {HARD_STOP_TIMEOUT}s")
    print(f"Status send interval: {STATUS_SEND_INTERVAL}s")
    print(f"FFT visualizer: {'ON' if FFT_ENABLED else 'OFF'}")
    print(f"FFT send: {'ON' if (FFT_ENABLED and FFT_SEND_ENABLED) else 'OFF'}")
    if not IS_MAC:
        print(f"Keepalive silent output: {'ON' if keepalive_enabled else 'OFF'}")
    print("Ctrl+Cで停止します")

    # ===== メインループ =====
    try:
        if IS_MAC:
            # ---- Mac: 従来コードそのまま ----
            current_device_id = device_id
            while True:
                try:
                    # ストリームを開いて監視ループを回す。停止検出で return してくる。
                    _run_stream(current_device_id)

                except Exception as e:
                    # InputStreamオープン失敗・PortAudio例外・一時的なデバイス消失など
                    # あらゆる例外をここで受け止め、プロセスを終了させない。
                    print(f"\n[watchdog] InputStream エラー: {type(e).__name__}: {e}",
                          file=sys.stderr)
                    send_message(b"SPEAK_STOP")

                # ---- 自己復旧：デバイス再スキャン → 再オープン ----
                print(f"[watchdog] {STREAM_RESTART_WAIT}秒後に再起動します...",
                      file=sys.stderr)
                time.sleep(STREAM_RESTART_WAIT)

                new_id, new_name = find_blackhole_device()
                if new_id is not None:
                    if new_id != current_device_id:
                        print(f"[watchdog] デバイスID変更検出: "
                              f"{current_device_id} → {new_id} ({new_name})",
                              file=sys.stderr)
                    current_device_id = new_id
                    print(f"[watchdog] InputStream を再オープンします "
                          f"(device={current_device_id})", file=sys.stderr)
                else:
                    print("[watchdog] BlackHole が見つかりません。再試行します...",
                          file=sys.stderr)
                    time.sleep(STREAM_RESTART_WAIT)

        else:
            # ---- Windows / Linux ----
            if keepalive_enabled:
                threading.Thread(target=_keepalive_loop, daemon=True).start()

            current_mic = mic
            while True:
                try:
                    _run_stream_soundcard(current_mic)

                except Exception as e:
                    print(f"\n[watchdog] ストリームエラー: {type(e).__name__}: {e}",
                          file=sys.stderr)
                    send_message(b"SPEAK_STOP")

                # ---- 自己復旧：デバイス再スキャン → 再オープン ----
                # 既定の再生デバイスが切り替わった場合もここで追従する。
                print(f"[watchdog] {STREAM_RESTART_WAIT}秒後に再起動します...",
                      file=sys.stderr)
                time.sleep(STREAM_RESTART_WAIT)

                new_mic = find_loopback_mic()
                if new_mic is not None:
                    if new_mic.name != current_mic.name:
                        print(f"[watchdog] デバイス変更検出: "
                              f"{current_mic.name} → {new_mic.name}",
                              file=sys.stderr)
                    current_mic = new_mic
                    print(f"[watchdog] 取得スレッドを再起動します "
                          f"(device={current_mic.name})", file=sys.stderr)
                else:
                    print("[watchdog] ループバックデバイスが見つかりません。"
                          "再試行します...", file=sys.stderr)
                    time.sleep(STREAM_RESTART_WAIT)

    except KeyboardInterrupt:
        print("\n停止します")
        if not IS_MAC:
            _capture_generation += 1  # 取得スレッドへ終了を指示
        send_message(b"SPEAK_STOP")
        print("■ SPEAK_STOP sent on exit")

    finally:
        if sock is not None:
            sock.close()


# =============================================================================
# ── 起動エントリポイント ──
#   引数なし          : Companion GUI（従来どおり）
#   第1引数が "talk"  : Talk音声連携モード
# =============================================================================

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "talk":
        run_talk_mode()
    else:
        ensure_base_dir()
        app = QApplication(sys.argv)
        win = MainWindow()
        win.show()
        sys.exit(app.exec_())
