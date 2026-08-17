#!/usr/bin/env python3
# =============================================================================
# karipom_companion_v4.py
# KariPom Companion v4 — 1ウィンドウ4タブ統合版
# 2026-08-17: KariPom BBXを最新Desktopと同じ3ボタン/全Visualizer/全Lightingへ更新。
#             Visualizer開始時にOS別音声環境を確認し、不足時は案内後Faceへロールバック。
# 2026-08-17: run_talk_mode()のmacOS経路もBlackHoleからScreenCaptureKitへ統一。
#             helperは事前ビルド済みバイナリ（mac-companion/resources/）を同梱し、
#             実行時のswiftcビルドを廃止（一般ユーザーにXcode/Command Line Toolsは不要）。
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

import sys, json, os, re, threading, traceback, subprocess, webbrowser, random, math, shutil
import numpy as np
import socket, platform, time, signal
from pathlib import Path
import requests
from datetime import datetime
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QTextEdit, QSplitter,
    QGroupBox, QStatusBar, QMessageBox, QFrame, QTabWidget
)
from PyQt5.QtCore import Qt, QThread, pyqtSignal, QTimer, QUrl, QPoint, QPointF, QEvent
from PyQt5.QtGui import QFont, QFontMetrics, QTextCursor, QColor, QPainter, QPen, QBrush

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
        self._error_code = ""
        self._fft = np.zeros(8, dtype=np.float32)
        self._source = ""

    def set_audio(self, speaking, rms):
        with self._lock:
            self._speaking = bool(speaking)
            self._rms = float(rms)

    def set_fft(self, levels):
        arr = np.asarray(levels, dtype=np.float32).reshape(-1)
        if arr.size != 8:
            return
        with self._lock:
            self._fft = arr.copy()

    def set_error(self, text, code=""):
        with self._lock:
            self._error = str(text or "")
            self._error_code = str(code or "")

    def clear_error(self):
        with self._lock:
            self._error = ""
            self._error_code = ""

    def error_info(self):
        with self._lock:
            return self._error_code, self._error

    def set_source(self, text):
        with self._lock:
            self._source = str(text or "")

    def source(self):
        with self._lock:
            return self._source

    def snapshot(self):
        with self._lock:
            return self._speaking, self._rms, self._error, self._fft.copy()


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


def locate_mac_sck_helper():
    """mac-companion/resources/ に同梱された、事前ビルド済みScreenCaptureKit音声helper
    （Universal Binary: arm64/x86_64）の実行ファイルパスを返す。

    Swiftソースは resources/karipom_sck_helper.swift、ビルド手順は
    resources/build_helper.sh を参照。ビルドは開発者が一度だけ実行してバイナリを
    リポジトリへコミットするものであり、一般ユーザーの実行時にswiftc/Xcode
    Command Line Toolsを必要としない（helperは配布物に同梱済み）。
    将来の.app配布版では、この関数がアプリバンドル内Resourcesを指すよう
    差し替えるだけでよい。
    """
    helper_path = Path(__file__).resolve().parent / "resources" / "karipom_sck_helper"
    if not helper_path.exists():
        raise FileNotFoundError(
            f"ScreenCaptureKit helperが見つかりません: {helper_path}\n"
            "mac-companion/resources/karipom_sck_helper が配布物に含まれているか確認してください"
            "（resources/build_helper.sh で生成できます）。"
        )
    if not os.access(helper_path, os.X_OK):
        try:
            helper_path.chmod(helper_path.stat().st_mode | 0o111)
        except OSError:
            pass
    return helper_path


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
        # macOSはApple純正ScreenCaptureKitを使用し、BlackHole/sounddeviceは不要。
        if platform.system() != "Darwin":
            import soundcard as sc
            self.sc = sc
        self._prepare_fft()

    def inspect_audio_environment(self):
        """Visualizer開始前の同期チェック用。OSごとのPC再生音取得経路が
        利用可能かを副作用なしで確認し、(ok, code, detail) を返す。

        macOSはBlackHoleを使わずScreenCaptureKitを使用する。
        code:
          OK / UNSUPPORTED_MACOS / MISSING_HELPER_BINARY /
          MISSING_SOUNDCARD / MISSING_LOOPBACK / AUDIO_CHECK_ERROR
        """
        try:
            if platform.system() == "Darwin":
                version = tuple(
                    int(v) for v in platform.mac_ver()[0].split(".")[:2]
                    if v.isdigit()
                )
                if version < (13, 0):
                    return False, "UNSUPPORTED_MACOS", "ScreenCaptureKit audio requires macOS 13 or later"
                try:
                    helper_path = locate_mac_sck_helper()
                except Exception as exc:
                    return False, "MISSING_HELPER_BINARY", str(exc)
                return True, "OK", str(helper_path)

            try:
                import soundcard as sc
            except Exception as exc:
                return False, "MISSING_SOUNDCARD", str(exc)
            try:
                mics = sc.all_microphones(include_loopback=True)
                loopbacks = [mic for mic in mics if self._is_loopback_mic(mic)]
            except Exception as exc:
                return False, "AUDIO_CHECK_ERROR", str(exc)
            if loopbacks:
                return True, "OK", str(loopbacks[0].name)
            return False, "MISSING_LOOPBACK", "PC playback loopback/monitor source not found"
        except Exception as exc:
            return False, "AUDIO_CHECK_ERROR", f"{type(exc).__name__}: {exc}"

    @staticmethod
    def _is_sck_permission_denied(detail):
        s = (detail or "").lower()
        markers = (
            "tcc", "permission", "not authorized", "not authorised",
            "declined", "denied", "拒否", "許可",
        )
        return any(marker in s for marker in markers)

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
        """ScreenCaptureKit helperからFloat32 mono PCMを受け取り、既存の口パク/FFT/UDPへ渡す。
        権限拒否は再試行せず停止。スリープ等の一時停止は1→2→4秒で自動再接続する。
        """
        restart_delay = 1.0
        np = self.np

        while not self.stop_event.is_set():
            try:
                helper_path = locate_mac_sck_helper()
            except Exception as exc:
                msg = f"ScreenCaptureKit helper error: {type(exc).__name__}: {exc}"
                self.state.set_source("")
                self.state.set_error(msg, "SCK_HELPER_ERROR")
                self.detector.force_stop()
                print(f"[audio] {msg}", file=sys.stderr)
                return

            self.state.set_source("ScreenCaptureKit")
            self.state.clear_error()
            print("Audio: macOS ScreenCaptureKit (BlackHole不要)")

            proc = None
            stderr_lines = []
            try:
                proc = subprocess.Popen(
                    [str(helper_path)],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    bufsize=0,
                )

                def stderr_reader():
                    assert proc is not None and proc.stderr is not None
                    for raw in iter(proc.stderr.readline, b""):
                        line = raw.decode("utf-8", errors="replace").strip()
                        if line:
                            stderr_lines.append(line)
                            print(f"[ScreenCaptureKit] {line}", file=sys.stderr)

                threading.Thread(
                    target=stderr_reader,
                    name="KariPomCompanionSCKStderr",
                    daemon=True,
                ).start()

                assert proc.stdout is not None
                pending = bytearray()
                bytes_per_block = EMBED_BLOCKSIZE * 4  # Float32 mono

                while not self.stop_event.is_set():
                    chunk = proc.stdout.read(max(4096, bytes_per_block - len(pending)))
                    if not chunk:
                        break
                    restart_delay = 1.0
                    pending.extend(chunk)
                    while len(pending) >= bytes_per_block:
                        raw_block = bytes(pending[:bytes_per_block])
                        del pending[:bytes_per_block]
                        mono = np.frombuffer(raw_block, dtype=np.float32).copy()
                        if mono.size == EMBED_BLOCKSIZE:
                            self._process_audio_block(mono)

                if self.stop_event.is_set():
                    if proc.poll() is None:
                        proc.terminate()
                    try:
                        proc.wait(timeout=2.0)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    return

                rc = proc.wait(timeout=1.0) if proc.poll() is not None else None
                detail = stderr_lines[-1] if stderr_lines else f"helper exited ({rc})"
                self.detector.force_stop()
                self.state.set_fft(np.zeros(8, dtype=np.float32))

                if self._is_sck_permission_denied(detail):
                    msg = "macOSの『画面とシステムオーディオの録音』権限が許可されていません。"
                    self.state.set_source("")
                    self.state.set_error(msg, "SCK_PERMISSION_DENIED")
                    print(f"[audio] ScreenCaptureKit permission denied: {detail}", file=sys.stderr)
                    return

                msg = f"ScreenCaptureKit capture stopped: {detail}"
                self.state.set_error(msg, "SCK_STREAM_STOPPED")
                print(f"[audio] {msg}", file=sys.stderr)
                wait_s = restart_delay
                restart_delay = min(restart_delay * 2.0, 4.0)
                print(f"[audio] ScreenCaptureKitを{wait_s:.0f}秒後に再接続します。", file=sys.stderr)
                if self.stop_event.wait(wait_s):
                    return

            except Exception as exc:
                if proc is not None and proc.poll() is None:
                    proc.terminate()
                msg = f"ScreenCaptureKit capture error: {type(exc).__name__}: {exc}"
                self.state.set_error(msg, "SCK_CAPTURE_ERROR")
                print(f"[audio] {msg}", file=sys.stderr)
                self.detector.force_stop()
                self.state.set_fft(np.zeros(8, dtype=np.float32))
                wait_s = restart_delay
                restart_delay = min(restart_delay * 2.0, 4.0)
                if self.stop_event.wait(wait_s):
                    return

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


# =============================================================================
# KariPom BBX Desktop-parity preview constants (2026-08-17)
# Source of truth: latest KariPom Desktop supplied with this Companion update.
# =============================================================================
# ===== karipom_talk 最終版と同じ音声判定値 =====
DEFAULT_THRESHOLD = 0.02
SILENCE_TIMEOUT = 5.0
HARD_STOP_RMS = 0.0005
HARD_STOP_TIMEOUT = 0.8
BLOCKSIZE = 2048
SAMPLERATE = 44100

# ===== EQ Classic 試験移植：KariPom Talk/Companion と同じ8バンドFFT =====
FFT_UPDATE_INTERVAL = 0.08
FFT_BANDS_HZ = [60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000]
FFT_DB_RANGES = [
    (-60, -10),
    (-62, -12),
    (-63, -13),
    (-65, -15),
    (-65, -15),
    (-70, -20),
    (-78, -28),
    (-85, -35),
]
FFT_ATTACK = 0.6
FFT_DECAY = 0.18

# CoreS3 Graphic EQ (Classic) と同じ表示ゲイン・8色・レイアウト。
EQ_GAIN8 = [1.6, 1.6, 1.6, 1.6, 1.7, 1.8, 2.0, 2.2]
EQ_COLORS_RGB = [
    (0, 0, 255),       # 0x001F blue
    (0, 255, 255),     # 0x07FF cyan
    (0, 255, 0),       # 0x07E0 green
    (173, 255, 41),    # 0xAFE5 yellow-green (RGB565 -> RGB888)
    (255, 255, 0),     # 0xFFE0 yellow
    (255, 165, 0),     # 0xFD20 orange
    (255, 0, 0),       # 0xF800 red
    (255, 0, 255),     # 0xF81F magenta
]
EQ_COL_W = 40
EQ_BAR_MX = 3
EQ_BAR_W = 34
EQ_BAR_MAX_H = 160
EQ_BAR_BOTTOM_Y = 200

# ===== Audio Visualizer modes =====
# Face → 各Visualizer → (UI上は)Random → Face の順で3ボタンUIが循環する。
# "Random"はVIS_MODESには含めない（実際の描画対象ではなく、Random抽選中である
# ことを示すUI専用の疑似状態のため）。CoreS3側のVIZ_MODES[]と同じ並び順。
VIS_MODES = [
    "Face",
    "EQ Classic",
    "Audio Halo",
    "Mirror Wave",
    "8-Lane Rhythm",
    "Kaleidoscope",
    "Analog VU",
    "Tetromino Dance",
    "Flash Spotlight",
]

SCENE_TOP = 48

# ===== 3ボタンUIの配色・フォント（v0.5 Phase 3.1で追加）=====
# 左から Character(青地white文字) / Visualizer(白地黒文字) / Lighting(赤地white文字)の
# トリコロール。派手な蛍光色ではなく落ち着いたフランス国旗風の青・赤を使用する。
BTN_BG_CHARACTER = (0, 85, 164)     # 落ち着いたフランス国旗風の青
BTN_BG_VISUALIZER = (255, 255, 255)
BTN_BG_LIGHTING = (206, 17, 38)     # 落ち着いたフランス国旗風の赤
BTN_FG_ON_BLUE = (255, 255, 255)
BTN_FG_ON_WHITE = (0, 0, 0)
BTN_FG_ON_RED = (255, 255, 255)
# ラベル行・現在値行の2行太字表示。基準フォントサイズ（pt）はPhase 3.1以前の無指定
# QPushButtonが実際に使っていたデフォルトフォントのポイント数（プローブ値。
# KariPomWindow.__init__でプローブ用QPushButtonから読み取る）を土台にする。
# v0.5 Phase 4見た目修正：実機確認で「ボタンの大きさに対して文字がまだ小さい」との
# 指摘を受け、今回は「Qt標準フォントサイズに一致させる」ことを目的とせず、プローブ値へ
# BTN_FONT_PT_BOOSTを加算した値を基準サイズとする。boost量は、scale=2〜4であれば
# 通常名称（Character:/KariPom、Visualizer:/Face、Lighting:/Random 等）が縮小・折り返し
# 無しで収まり、かつボタン幅に対して十分な余白が残る範囲で選んだ（scale=1は元々ボタン幅が
# 320//3px しかなく、boost前の基準サイズでも短い名称一部で縮小が発生していたため、
# scale=1のみ従来通り(1)折り返し→(2)縮小のフォールバックに委ねる＝退行ではない）。
# scaleでは変えない＝通常の短い名称はscale=2〜4のどれでもこの基準サイズのまま表示される。
# 長い名称（Rainbow Washing Machine等）だけ、(1)現在値の折り返し→(2)それでも収まらない
# 場合のみBTN_FONT_PT_MINまでの縮小、の順で個別に対応する（_fit_button_text参照）。
BTN_FONT_PT_FALLBACK = 12  # プローブでポイントサイズが取得できない環境向けの保険値
BTN_FONT_PT_BOOST = 5      # v0.5 Phase 4見た目修正：プローブ値からの上乗せ量(pt)
BTN_FONT_PT_MIN = 6
BTN_HEIGHT_BASE = 26
BTN_HEIGHT_PER_SCALE = 12
SPECTRUM_COLORS = [QColor(*rgb) for rgb in EQ_COLORS_RGB]

# Mirror Wave
MW_CY, MW_YMIN, MW_YMAX, MW_SX, MW_EDGE = 143, 50, 236, 4, 6
MW_PAL = [
    QColor(0,230,255), QColor(0,140,255), QColor(150,90,255),
    QColor(255,60,220), QColor(255,200,0), QColor(60,255,140),
]

# 8-Lane Rhythm
RHY_ROW_H = 4
RHY_HIST_ROWS = (CANVAS_H - SCENE_TOP) // RHY_ROW_H if "CANVAS_H" in globals() else 48

# ===== 実機 .ino と同じ顔・モーション値 =====
CANVAS_W = 320
CANVAS_H = 240
NOSE_X = 160
NOSE_Y = 145
SLEEP_OUTLINE_PX = 4  # CoreS3 SLEEP_OUTLINE_PXと同値。Lighting中の顔白縁取りの太さに使う。
MOUTH_PAKU_MS = 140
BLINK_CLOSED_MS = 80
DOUBLE_BLINK_GAP_MS = 180
BLINK_PROBABILITY = 0.45
DOUBLE_BLINK_PROBABILITY = 0.08
BLINK_INTERVAL_MS = (3000, 10000)
NOSE_INTERVAL_MS = (120, 280)
RHY_HIST_ROWS = (CANVAS_H - SCENE_TOP) // RHY_ROW_H

# ===== Flash Spotlight Visualizer（CoreS3 vizRenderFlashSpotlight() 準拠）=====
# 8バンドFFTの各バンドを独立した半透明の色付き円として表現する。CoreS3側はM5GFXに
# アルファ付き円塗りAPIが無いため走査線ごとにfillRectAlpha()を呼ぶ自作ヘルパーで円を
# 組み立てているが、QPainterは標準でアルファ合成付きdrawEllipse()を持つため、
# その回避策は不要（QColor.setAlpha()+drawEllipse()で同等以上の見た目になる）。
SPOT_INTERVAL_MS = 120        # 新しいランダム配置（位置・色）へ切り替える周期
SPOT_BAND_FLOOR = 0.020       # バンド単位の無音閾値（CoreS3 SPOT_BAND_FLOOR/AVU_BAND_FLOORと同値）
SPOT_R_MIN = 20.0             # 閾値を超えて表示される円の最小半径
SPOT_R_MAX = 94.0             # FFTレベル最大時の半径
SPOT_ALPHA = 128              # 円の不透明度（0-255。約50%。CoreS3 SPOT_ALPHAと同値）
# 半径＝絶対成分＋相対成分ブレンド用パラメータ（CoreS3のSPOT_*＝AVU_*と同値）
SPOT_ABS_FULL = 1.30
SPOT_REL_LO = 0.35
SPOT_REL_HI = 1.90
SPOT_W_ABS = 0.55
SPOT_W_REL = 0.60
# CoreS3 SPOT_PALETTE[]（RGB565・14色）をRGB888へ変換した固定パレット。
SPOT_PALETTE_RGB = [
    (255, 0, 0),      # 0xF800
    (255, 109, 0),    # 0xFB60
    (255, 182, 0),    # 0xFDA0
    (255, 235, 0),    # 0xFF40
    (173, 255, 0),    # 0xAFE0
    (0, 223, 58),     # 0x06E7
    (0, 255, 140),    # 0x07F1
    (0, 223, 255),    # 0x06FF
    (0, 150, 255),    # 0x04BF
    (58, 89, 255),    # 0x3ADF
    (140, 61, 255),   # 0x89FF
    (255, 0, 222),    # 0xF81B
    (255, 61, 140),   # 0xF9F1
    (255, 20, 90),    # 0xF8AB
]

# ===== Character（顔）モード =====
# CoreS3 enum FaceMode（KariPom/Miss KariPom/Face Gallery）のうち、Desktopでは
# Face Galleryの代わりに「None（顔なし）」を直接3値目に持つ、独自の3状態にする。
# 循環順は KariPom → Ms. KariPom → None → KariPom。CharacterにRandomは付けない。
CHARACTER_MODES = ["KariPom", "Ms. KariPom", "None"]
CHAR_KARIPOM, CHAR_MISS_KARIPOM, CHAR_NONE = range(3)

# ===== Lighting モード =====
# CoreS3 enum LightingModeと同じ相対並び順で、実装が完了したものから追記していく
# （index 0 = "None" は固定。CoreS3には無いDesktop独自の状態で、Lighting Random
# の抽選対象にも含める）。Phase 3で低難度6種（Disco Floor/Laser Show/Aurora/Matrix/
# Hypnotic Vortex/Rainbow Washing Machine）を追加。Phase 4a（中難度）でEye Slot・
# Flower Clockを追加。Phase 4b（中難度・残り5種）でRetro Race/Sky Raid/Classic Race/
# Asteroid Field/Tempest Tunnelを追加し、CoreS3のLIGHT_RACE=4〜LIGHT_TUNNEL=9の
# 相対位置（LIGHT_EYESLOT=6を挟む前後関係）をそのまま維持した。Phase 4残り4種
# （8-Bit Runner/Psychedelic-Trance/Aquarium/Flying Pompadour）を追加し、CoreS3の
# LIGHT_MARIO=12・LIGHT_PSYCHE=14・LIGHT_AQUARIUM=16・LIGHT_FLYINGPOMPADOUR=17の
# 相対位置（Desktop未実装のPAC-MAN/Fighter Duel/Missile Defenseは詰めて省略しつつ、
# 実装済みの前後関係だけはCoreS3のenum順を維持）をそのまま踏襲した。Phase 5A で
# PAC-MAN Arcade（CoreS3 LIGHT_PACMAN=10。Tempest Tunnelの直後・8-Bit Runnerの直前へ
# 挿入）とPixel Invasion（CoreS3 LIGHT_PIXELINVASION=19。Rainbow Washing Machineの
# 直後・Flower Clockの直前へ挿入）を追加。Phase 5B でFighter Duel（CoreS3
# LIGHT_STREETFIGHTER=11。PAC-MANの直後・8-Bit Runnerの直前へ挿入）・Missile Defense
# （CoreS3 LIGHT_MISSILE=13。8-Bit Runnerの直後・Psychedelic/Tranceの直前へ挿入）・
# BASEBALL Arcade（CoreS3 LIGHT_BASEBALL=22。Desktop未実装のPINBALL Arcade(21)を
# 詰めて省略し、実装済みの中では最後尾のFlower Clockの直後へ挿入）を追加。残りの
# Phase 5対象（PINBALL Arcade / SKY BURNER）はPhase 5Cで追加予定。
LIGHT_MODES = [
    "None",
    "Disco Floor",
    "Laser Show",
    "Aurora",
    "Matrix",
    "Retro Race",
    "Sky Raid",
    "Eye Slot",
    "Classic Race",
    "Asteroid Field",
    "Tempest Tunnel",
    "PAC-MAN Arcade",
    "Fighter Duel",
    "8-Bit Runner",
    "Missile Defense",
    "Psychedelic / Trance",
    "Hypnotic Vortex",
    "Aquarium",
    "Flying Pompadour",
    "Rainbow Washing Machine",
    "Pixel Invasion",
    "Flower Clock",
    "PINBALL Arcade",
    "BASEBALL Arcade",
    "SKY BURNER",
]
(
    LIGHT_NONE, LIGHT_DISCO, LIGHT_LASER, LIGHT_AURORA, LIGHT_MATRIX,
    LIGHT_RACE, LIGHT_SKYRAID, LIGHT_EYESLOT, LIGHT_CLASSICRACE, LIGHT_ASTEROID,
    LIGHT_TUNNEL, LIGHT_PACMAN, LIGHT_STREETFIGHTER, LIGHT_MARIO, LIGHT_MISSILE, LIGHT_PSYCHE, LIGHT_VORTEX, LIGHT_AQUARIUM,
    LIGHT_FLYINGPOMPADOUR, LIGHT_RAINBOWWASHER, LIGHT_PIXELINVASION, LIGHT_FLOWERCLOCK, LIGHT_PINBALL, LIGHT_BASEBALL, LIGHT_SKYBURNER,
) = range(25)

# ===== Phase 5C: PINBALL Arcade（CoreS3 v5.6準拠）=====
PIN_TOP, PIN_BOTTOM, PIN_LEFT, PIN_RIGHT = SCENE_TOP, 240, 16, 304
PIN_BALL_R = 6.0
PIN_GRAVITY = 260.0
PIN_REST = 0.82
PIN_MAX_SPEED = 260.0
PIN_MIN_SPEED_STUCK = 12.0
PIN_STUCK_MS = 2200
PIN_STUCK_POS_R = 62.0
PIN_STUCK_POS_MS = 4000
PIN_STUCK_NUDGE_VX = 38.0
PIN_LAUNCH_X_MARGIN = 24.0
PIN_LAUNCH_X_MIN = PIN_LEFT + PIN_BALL_R + PIN_LAUNCH_X_MARGIN
PIN_LAUNCH_X_MAX = PIN_RIGHT - PIN_BALL_R - PIN_LAUNCH_X_MARGIN
PIN_LAUNCH_Y = PIN_TOP + 14.0
PIN_LAUNCH_VX_RANGE = 35.0
PIN_LAUNCH_VY_MIN, PIN_LAUNCH_VY_MAX = 60.0, 90.0
PIN_FLASH_MS = 220
PIN_EYE_Y = 90.0
PIN_EYE_X = (90.0, 230.0)
PIN_EYE_INNER_R, PIN_EYE_RING_R, PIN_EYE_OUTLINE_R = 21.0, 26.0, 29.0
PIN_EYE_COLLIDE_R, PIN_EYE_FLASH_R, PIN_EYE_KICK, PIN_EYE_SCORE = 29.0, 32.0, 200.0, 25
PIN_NOSE_COLLIDE_R, PIN_NOSE_FLASH_R, PIN_NOSE_KICK, PIN_NOSE_SCORE = 17.0, 26.0, 170.0, 15
PIN_MOUTH_PASS_FLASH_MS, PIN_MOUTH_PASS_TOUCH_R, PIN_MOUTH_PASS_FLASH_THICK = 100, PIN_BALL_R, 11
PIN_FLIPPER_LEN, PIN_FLIPPER_HW, PIN_FLIPPER_ZONE_R = 48.0, 5.0, 30.0
PIN_FLIP_UP_MS, PIN_FLIP_HOLD_MS, PIN_FLIP_RETURN_MS = 90, 130, 220
PIN_FLIP_LAUNCH_SPEED, PIN_FLIP_COOLDOWN_MS = 210.0, 160
PIN_FLIP_L_PX, PIN_FLIP_L_PY, PIN_FLIP_R_PX, PIN_FLIP_R_PY = 82.0, 224.0, 238.0, 224.0
PIN_FLIP_L_REST_DX, PIN_FLIP_L_REST_DY = 0.978, 0.208
PIN_FLIP_L_FIRE_DX, PIN_FLIP_L_FIRE_DY = 0.574, -0.819
PIN_FLIP_R_REST_DX, PIN_FLIP_R_REST_DY = -0.978, 0.208
PIN_FLIP_R_FIRE_DX, PIN_FLIP_R_FIRE_DY = -0.574, -0.819
PIN_BG_RGB=(0,0,0); PIN_FIELD_RGB=(18,30,70); PIN_WALL_RGB=(70,215,255); PIN_BALL_RGB=(235,235,240)
PIN_FLIPPER_RGB=(255,220,40); PIN_FLIPPER_HUB_RGB=(255,255,255); PIN_EYE_PINK_RGB=(255,140,190)
PIN_FLASH_RGB=(255,255,255); PIN_EYE_SCORE_RGB=(0,0,0); PIN_NOSE_RGB=(255,140,20)
PIN_GUIDE_TOP_Y=150.0; PIN_BOUND_DRAIN_EDGE=3
PIN_BOUND_X=(PIN_LEFT,PIN_RIGHT,PIN_RIGHT,PIN_FLIP_R_PX,PIN_FLIP_L_PX,PIN_LEFT)
PIN_BOUND_Y=(PIN_TOP,PIN_TOP,PIN_GUIDE_TOP_Y,PIN_FLIP_R_PY,PIN_FLIP_L_PY,PIN_GUIDE_TOP_Y)
PIN_FLIP_IDLE, PIN_FLIP_UP, PIN_FLIP_HOLD, PIN_FLIP_RETURN = range(4)

# ===== Phase 5C: SKY BURNER（CoreS3 v1.0準拠）=====
SKB_TOP=48; SKB_HORIZON_Y0=132
SKB_MAX_BANK_DEG=18.0; SKB_BANK_SMOOTH=0.07; SKB_ENEMY_MAX_BANK_DEG=26.0; SKB_ENEMY_BANK_SMOOTH=0.10
SKB_SEARCH_MIN_MS=500; SKB_SEARCH_MAX_MS=900; SKB_APPROACH_MS=2600
SKB_ENEMY_FOLLOW=0.10; SKB_AIM_FOLLOW=0.14; SKB_COOLDOWN_AIM_FOLLOW=0.05
SKB_LOCK_CAPTURE_PX=28.0; SKB_LOCK_REQUIRE_MS=850; SKB_LOCK_DECAY=0.6; SKB_LOCK_HOLD_MS=450
SKB_MISSILE_SPEED_PPS=340.0; SKB_MISSILE_TURN=0.14; SKB_MISSILE_MAX_MS=1700; SKB_MISSILE_HIT_PX=12.0
SKB_MSL_LAUNCH_Y=236.0; SKB_MSL_LANE_OFFSET=78.0
SKB_SMOKE_MAX=64; SKB_SMOKE_LIFE_MS=1300; SKB_SMOKE_SPAWN_PER_FRAME=3; SKB_SMOKE_R0=6.5; SKB_SMOKE_R_MIN=2.0; SKB_SMOKE_R_FAR=1.0
SKB_HIT_FLASH_MS=110; SKB_HIT_MID_MS=260; SKB_HIT_LIFE_MS=560; SKB_COOLDOWN_MIN_MS=450; SKB_COOLDOWN_MAX_MS=850
SKB_STREAK_COUNT=14; SKB_STREAK_SPEED_PPS=240.0; SKB_STREAK_MAX_R=190.0; SKB_STREAK_SEG_LEN=22.0
SKB_EMSL_MIN_INTERVAL_MS=2600; SKB_EMSL_MAX_INTERVAL_MS=4600; SKB_EMSL_APPROACH_MS=1000; SKB_EMSL_DODGE_MS=350
SKB_EMSL_SCALE_MIN=0.15; SKB_EMSL_SCALE_PEAK=1.6; SKB_EMSL_SCALE_END=0.55
SKB_ESMOKE_MAX=20; SKB_ESMOKE_LIFE_MS=550; SKB_ESMOKE_R_FAR=1.0; SKB_ESMOKE_R_NEAR=7.5
SKB_ST_SEARCH, SKB_ST_TRACK, SKB_ST_LOCKED, SKB_ST_MISSILE, SKB_ST_HIT, SKB_ST_COOLDOWN = range(6)
SKB_EMSL_APPROACH, SKB_EMSL_DODGE = range(2)
SKB_SKY_RGB=(70,130,215); SKB_SKY_HZ_RGB=(190,220,235); SKB_SEA_RGB=(20,85,125); SKB_SEA_HZ_RGB=(55,135,160)
SKB_STREAK_RGB=(255,255,255); SKB_ENEMY_RGB=(230,70,60); SKB_ENEMY_ENGINE_RGB=(255,150,60)
SKB_RETICLE_RGB=(80,255,140); SKB_LOCK_RGB=(255,60,60); SKB_MISSILE_RGB=(255,255,255); SKB_MISSILE_FLAME_RGB=(255,160,40)
SKB_SMOKE_RGB=(235,235,235); SKB_HIT_A_RGB=(255,255,0); SKB_HIT_B_RGB=(255,90,20); SKB_HIT_C_RGB=(255,255,255)

# ===== Lighting #1: Disco Floor（CoreS3 lightRenderDisco()準拠）=====
DISCO_COLS = 8
DISCO_ROWS = 6
DISCO_TW = 40
DISCO_TH = 40
DISCO_TILES = DISCO_COLS * DISCO_ROWS  # 48
DISCO_NPAL = 4
# CoreS3 DISCO_PAL_RGB[]と同じ8bit RGB値（565量子化前の値をそのまま使う。実機より色再現が上）。
DISCO_PAL_RGB = [
    [(255, 0, 0), (255, 120, 0), (255, 240, 0), (0, 220, 0), (0, 230, 255), (0, 90, 255), (255, 0, 230), (255, 255, 255)],
    [(255, 0, 0), (255, 240, 0), (0, 90, 255), (0, 220, 0), (255, 0, 0), (255, 240, 0), (0, 90, 255), (0, 220, 0)],
    [(255, 0, 230), (0, 230, 255), (255, 240, 0), (0, 220, 0), (0, 90, 255), (255, 0, 230), (0, 230, 255), (255, 255, 255)],
    [(255, 0, 0), (255, 255, 255), (0, 90, 255), (255, 240, 0), (0, 220, 0), (255, 0, 230), (0, 230, 255), (255, 120, 0)],
]
DISCO_NPAT = 11
(DISCO_PAT_TLBR, DISCO_PAT_TRBL, DISCO_PAT_LR, DISCO_PAT_RL, DISCO_PAT_BT, DISCO_PAT_TB,
 DISCO_PAT_OUT, DISCO_PAT_IN, DISCO_PAT_CHECKER, DISCO_PAT_PULSE, DISCO_PAT_SPARK) = range(DISCO_NPAT)
DISCO_TOP = SCENE_TOP
DISCO_SWITCH_MIN = 20000
DISCO_SWITCH_MAX = 30000
DISCO_TRAVEL_BASE = 0.05
DISCO_TRAVEL_K = 0.42
DISCO_HUE_BASE = 0.015
DISCO_HUE_K = 0.10
# 顔パーツ（目・鼻・口）の外接矩形。CoreS3 buildDiscoTable()と同じ座標。
DISCO_FACE_RECTS = [(66, 66, 48, 48), (206, 66, 48, 48), (138, 131, 44, 28), (132, 150, 56, 40)]

# ===== Lighting #2: Laser Show（CoreS3 lightRenderLaser()準拠。オーバーレイ種別だが
# Desktopは単一選択のため、選択時は自前で暗い会場を敷いてからビームを重ねる）=====
LASER_CROSS, LASER_FAN, LASER_DUAL, LASER_XBURST, LASER_RANDOM = range(5)
LASER_NPAT = 5
LASER_MAX = 14
LASER_YMIN = SCENE_TOP + 3
LASER_YMAX = 236
LASER_XMIN = 3
LASER_XMAX = 316
LASER_SWITCH_MIN = 18000
LASER_SWITCH_MAX = 27000
LASER_GREEN_RGB = (0, 255, 0)  # 0x07E0

# ===== Lighting #3: Aurora（CoreS3 lightRenderAurora()準拠）=====
AURORA_NCOL = 32
AURORA_CW = CANVAS_W // AURORA_NCOL
AURORA_TOP = SCENE_TOP
AURORA_NPAL = 6
AURORA_PAL_RGB = [(0, 255, 120), (80, 255, 170), (0, 230, 255), (0, 200, 210), (150, 90, 255), (255, 90, 210)]
AURORA_SKY_RGB = (0, 12, 41)  # 0x000E2Cに近い565値をRGB888展開した深い夜空色

# ===== Lighting #4: Matrix（CoreS3 lightRenderMatrix()準拠）=====
MATRIX_TOP = SCENE_TOP
MATRIX_COLW = 20
MATRIX_NCOL = CANVAS_W // MATRIX_COLW  # 16
MATRIX_CELLH = 22
MATRIX_GREEN_RGB = (0, 255, 0)   # 0x07E0
MATRIX_CYAN_RGB = (0, 255, 255)  # 0x07FF

# ===== Lighting #5: Retro Race（v0.5 Phase 4b。CoreS3 lightRenderRace()準拠。
# Pole Position風の疑似3Dレーススクリーンセーバー）=====
RACE_TOP = SCENE_TOP
RACE_HORIZON = 104
RACE_ROW_H = 4
RACE_MAX_CARS = 3
RACE_CAR_RGB = [(230, 60, 60), (70, 110, 230), (235, 200, 40), (235, 235, 235)]
RACE_MAX_SIGNS = 1
RACE_SIGN_TEXT = ["KARI", "POM", "GO!"]
RACE_SIGN_RGB = [(230, 140, 40), (40, 180, 170), (150, 90, 220), (250, 250, 235)]
RACE_SIGN_DARKTEXT = [True, True, False, True]
RACE_ROAD_MIN_HW = 8.0
RACE_ROAD_MAX_HW = 152.0
RACE_CURVE_MAX_PX = 132.0
RACE_STEER_MAX_PX = 46.0
RACE_SKY_TOP_RGB = (120, 190, 235)
RACE_SKY_HORIZON_RGB = (235, 235, 215)
RACE_SUN_RGB = (255, 235, 120)
RACE_GRASS_A_RGB = (40, 150, 50)
RACE_GRASS_B_RGB = (30, 125, 40)
RACE_ROAD_A_RGB = (90, 90, 95)
RACE_ROAD_B_RGB = (78, 78, 83)
RACE_CURB_A_RGB = (235, 60, 55)
RACE_CURB_B_RGB = (245, 245, 245)
RACE_LINE_RGB = (250, 235, 110)

# ===== Lighting #6: Sky Raid（v0.5 Phase 4b。CoreS3 lightRenderSkyRaid()準拠。
# Xevious風の縦スクロールスクリーンセーバー）=====
XEV_TOP = SCENE_TOP
XEV_ROW_H = 6
XEV_SHIP_Y = 206
XEV_MAX_ENEMIES = 4
XEV_MAX_SHOTS = 3
XEV_MAX_EXPL = 3
XEV_ENEMY_RGB = [(230, 70, 70), (230, 150, 60), (200, 90, 220)]
XEV_RUN_PX = 260.0
XEV_SHIP_RGB = (0, 255, 255)      # 0x07FF
XEV_SHOT_RGB = (255, 255, 0)      # 0xFFE0
XEV_EXPL_HOT_RGB = (255, 255, 255)
XEV_EXPL_COOL_RGB = (255, 150, 40)
XEV_GRASS_A_RGB = (60, 150, 70)
XEV_GRASS_B_RGB = (45, 130, 58)
XEV_FOREST_RGB = (25, 90, 40)
XEV_FOREST_D_RGB = (15, 70, 30)
XEV_ROAD_RGB = (95, 95, 100)
XEV_ROAD_LN_RGB = (230, 220, 120)
XEV_RIVER_RGB = (50, 110, 210)
XEV_RIVER_HI_RGB = (120, 180, 240)
XEV_BASE_RGB = (90, 90, 100)
XEV_BASE_AC_RGB = (200, 60, 60)
XEV_RUNWAY_RGB = (150, 150, 155)
XEV_RUNWAY_LN_RGB = (240, 240, 240)

# ===== Lighting #7(相対順): Eye Slot（CoreS3 lightRenderEyeSlot()準拠）=====
# 黒目2つがスロットリールに変わる演出。背景を塗る他のLightingと異なり、
# 「黒目の位置に絵柄を描く」顔レイヤー特殊フック（gEyeSlotActive）を伴う。
ESLOT_ROW_H = 40           # リール1コマぶんの高さ(px)。黒目(直径約40px)と同等
ESLOT_WIN_HALF_W = 20      # 表示窓の半幅(px)
ESLOT_SYM_R = 17           # 各絵柄の基準半径/半サイズ(px)
ESLOT_SYMBOL_COUNT = 8     # 7/BAR/Cherry/●/¥/$/❤/🥝
ESLOT_MAX_SPEED = 0.55     # 全開回転時のスクロール速度（絵柄/フレーム）
ESLOT_STRIP_L = (0, 1, 2, 3, 4, 5, 6, 7)
ESLOT_STRIP_R = (2, 5, 7, 1, 4, 0, 6, 3)
ESLOT_ST_ACCEL, ESLOT_ST_SPIN, ESLOT_ST_DECEL_L, ESLOT_ST_GAP, ESLOT_ST_DECEL_R, ESLOT_ST_RESULT = range(6)
ESLOT_EYE_L = (90, 90)     # 黒目のあった位置（左）。DesktopにeyeOffsetX/Y相当は無いため固定。
ESLOT_EYE_R = (230, 90)    # 同（右）

# ===== Lighting #8(相対順): Classic Race（v0.5 Phase 4b。CoreS3
# lightRenderClassicRace()準拠。1970年代後半風トップビューレースデモ）=====
CRACE_TOP = SCENE_TOP
CRACE_ROW_H = 4
CRACE_ROAD_HW = 75
CRACE_PLAYER_Y = 206
CRACE_MAX_ENEMIES = 3
CRACE_ENEMY_RGB = [(60, 110, 230), (235, 200, 40), (240, 240, 240)]
CRACE_GRASS_A_RGB = (55, 150, 65)
CRACE_GRASS_B_RGB = (45, 135, 55)
CRACE_ROAD_A_RGB = (25, 25, 28)
CRACE_ROAD_B_RGB = (20, 20, 22)
CRACE_EDGE_W_RGB = (245, 245, 240)
CRACE_OIL_RGB = (15, 15, 18)
CRACE_PLAYER_RGB = (230, 30, 30)

# ===== Lighting #9(相対順): Asteroid Field（v0.5 Phase 4b。CoreS3
# lightRenderAsteroid()準拠。ワイヤーフレーム隕石が漂う宇宙空間演出）=====
ASTR_TOP = SCENE_TOP
ASTR_COUNT = 13
ASTR_MAX_VERTS = 9
ASTR_STAR_COUNT = 16
ASTR_CSTAR_COUNT = 20
ASTR_CRAFT_MARGIN = 16.0
ASTR_CRAFT_SPEED = 0.30
ASTR_BULLET_COUNT = 2
ASTR_BULLET_SPEED = 4.0
ASTR_BULLET_RANGE = 55.0
ASTR_BULLET_DETECT_R = 60.0
ASTR_BULLET_MIN_INTERVAL_MS = 900
ASTR_NEON_RGB = [(255, 20, 147), (57, 255, 20), (255, 245, 10)]
ASTR_STAR_DIM_RGB = (90, 90, 95)
ASTR_STAR_FAINT_RGB = (55, 55, 60)
# GFXライブラリのRED/WHITE/YELLOW/GREEN定数のRGB888等価値。
ASTR_CSTAR_PALETTE_RGB = [(255, 0, 0), (255, 255, 255), (255, 255, 0), (0, 255, 0)]
ASTR_CRAFT_RGB = (210, 240, 255)

# ===== Lighting #10(相対順): Tempest Tunnel（v0.5 Phase 4b。CoreS3
# lightRenderTunnel()準拠。中央へ吸い込まれるワイヤーフレームトンネル演出）=====
TUN_TOP = SCENE_TOP
TUN_RINGS = 6
TUN_MAX_VERTS = 16
TUN_MIN_VERTS = 8
TUN_MIN_R = 6.0
TUN_MAX_R = 92.0
TUN_CENTER_X = 160
TUN_CENTER_Y = 144
TUN_RING_TWIST = 0.12
TUN_PULSE_AMPL = 0.06
TUN_HUE_STEP = 24.0
TUN_CRAFT_RADIUS = TUN_MAX_R - 6.0
TUN_CRAFT_ARC_CENTER = 1.5707963
TUN_CRAFT_ARC_HALF = 1.0471976
TUN_ENEMY_COUNT = 3
TUN_ENEMY_NEAR_T = 0.85
TUN_CSTAR_COUNT = 20
TUN_ROT_SPEED = 6.2831853 / 15000.0
TUN_FLOW_SPEED = TUN_RINGS / 4500.0
TUN_PULSE_SPEED = 6.2831853 / 3200.0
TUN_HUE_SPEED = 360.0 / 20000.0
TUN_CRAFT_STEER_RATE = 0.00035
TUN_ENEMY_RGB = (255, 70, 40)
TUN_CRAFT_RGB = (220, 245, 255)
TUN_CSTAR_PALETTE_RGB = [(255, 0, 0), (255, 255, 255), (255, 255, 0), (0, 255, 0)]
TUN_BULLET_COUNT = 2
TUN_BULLET_SPEED_PX_MS = 0.13
TUN_BULLET_DETECT_T = 0.55
TUN_BULLET_DETECT_ANGLE = 0.6
TUN_BULLET_MIN_INTERVAL_MS = 900

# ===== Lighting: PAC-MAN Arcade（v0.5 Phase 5A。CoreS3 lightRenderPacman()準拠。
# CoreS3 LIGHT_PACMAN=10相当。プレイヤー操作・当たり判定・スコア・ゲームオーバーは
# 無く、「勝手にパックマンが迷路を巡回し続けている」だけの自動デモ。ユーザー設計の
# 12行×20列迷路PAC_MAZEを正本とし、自機・オバケ3体とも隣接セル判定に基づく
# グリッド移動（直進優先→分岐はランダム→行き止まりのみ後戻り）で動く）=====
PAC_TOP = SCENE_TOP
PAC_CELL = 16
PAC_COLS = 20
PAC_ROWS = 12
PAC_TUNNEL_ROW = 5  # この行だけ左右端が通路＝左右トンネルとして接続する
PAC_GHOSTS = 3
# 迷路データ（正本）。1=壁 / 0=通路。CoreS3のPAC_MAZEと値・配置は一切変更していない。
PAC_MAZE = [
    [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1],
    [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1],
    [1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1],
    [1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1],
    [1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1],
    [0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 0, 0],
    [1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1],
    [1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1],
    [1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1],
    [1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1],
    [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1],
    [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1],
]
# パワーエサ：迷路四隅の外周通路4か所（PAC_MAZE上でいずれも通路であることを確認済み）。
PAC_POWER_COUNT = 4
PAC_POWER_COL = (1, 18, 1, 18)
PAC_POWER_ROW = (1, 1, 10, 10)
PAC_SPEED = 0.10  # 自機：1回の描画あたりの進行セル数（CoreS3から値は変更なし）
PAC_GHOST_SPEED = (0.085, 0.085, 0.075)  # CoreS3の速度感をそのまま踏襲
PAC_GHOST_SPAWN_COL = (9, 10, 11)
PAC_GHOST_SPAWN_ROW = (6, 6, 6)
PAC_GHOST_COL_RGB = [(230, 40, 40), (240, 130, 210), (70, 220, 230)]  # 赤・ピンク・シアン
PAC_DIRS = ((1, 0), (-1, 0), (0, 1), (0, -1))
PAC_WALL_COL_RGB = (33, 150, 243)
PAC_WALL_EDGE_RGB = (15, 90, 200)
PAC_DOT_COL_RGB = (255, 255, 190)
PAC_PACMAN_COL_RGB = (255, 225, 0)
PAC_GHOST_PUPIL_RGB = (20, 30, 140)
PAC_RESET_INTERVAL_MS = 45000  # ドット全消化に至らなくても45秒ごとに強制リセット

# Phase 5B共通定数：CoreS3のLighting合成周期(LIGHT_COMPOSITE_MS=90ms。約11fps)相当。
# CoreS3はupdateScreenEffects()内の「描画周期制限」により、Lighting描画関数（このFighter Duel/
# Missile Defense等を含む）を実機で約90msに1回しか呼ばない。そのため「1回の呼び出しあたり」で
# 加算する位置・イージング係数（例：sfF[i].x += facing*0.7*audioBoost、mslAimX += (target-aim)*0.12）は
# 実質「90msに1回だけ進む」値になっている。DesktopのLighting再描画は20ms周期（約50fps）のため、
# 該当ロジックを毎ティックそのまま加算すると実機の約4.5倍の速さで進んでしまう。これを避けるため、
# 「フレーム依存（更新回数依存）」のロジック更新だけをこの周期でゲートし、描画自体は毎ティック
# 行う（同じ状態を再描画するだけなので見た目には影響しない）。BASEBALL Arcadeは実時間dt方式の
# ため対象外（dtを毎ティック正しく計算すればそのままCoreS3と同じ速度になる）。PAC-MAN Arcade/
# Pixel Invasion（Phase 5A・実機確認済み・変更禁止）はこの定数を使わない。
LIGHT_FRAME_UPDATE_MS = 90

# ===== Lighting #11(相対順): Fighter Duel（Phase 5B。CoreS3 lightRenderStreetFighter()準拠）=====
SF_TOP = SCENE_TOP
SF_GROUND_Y = CANVAS_H - 40
SF_STAGE_L = 30.0
SF_STAGE_R = 290.0
(SF_IDLE, SF_WALK_F, SF_WALK_B, SF_CROUCH, SF_PUNCH, SF_KICK, SF_JUMP, SF_SPECIAL, SF_HIT) = range(9)
SF_SKY_A_RGB = (120, 190, 230)
SF_SKY_B_RGB = (170, 215, 235)
SF_HILL_RGB = (90, 150, 110)
SF_GROUND_RGB = (200, 160, 100)
SF_GROUND_LINE_RGB = (150, 110, 60)
SF_BELT_RGB = (25, 22, 20)
SF_HEADBAND_RGB = (225, 35, 35)
SF_HAIR_RGB = (35, 28, 24)
SF_P1_RGB = (235, 228, 205)   # Fighter A：白系の道着
SF_P2_RGB = (150, 25, 25)     # Fighter B：濃い赤の道着
SF_SKIN_RGB = (235, 190, 150)
SF_HP_GREEN_RGB = (0, 255, 0)
SF_HP_ORANGE_RGB = (255, 165, 0)
SF_HP_RED_RGB = (255, 0, 0)

# ===== Lighting #11(相対順): 8-Bit Runner（v0.5 Phase 4残り。CoreS3
# lightRenderMario()準拠。横スクロールアクションのオマージュ演出）=====
MAR_TOP = SCENE_TOP
MAR_GROUND_Y = CANVAS_H - 34
MAR_BLOCK_Y = MAR_GROUND_Y - 70
MAR_PLAYER_X = 70
MAR_OBST_MAX = 8
MAR_LOOP_DIST = 3000.0
MAR_CLOUD_COUNT = 5
MAR_JUMP_MS = 480.0
MAR_JUMP_H = 30.0
MAR_T_PIPE, MAR_T_ENEMY, MAR_T_BLOCK, MAR_T_ITEM, MAR_T_COIN = range(5)
MAR_SKY_RGB = (110, 190, 250)
MAR_CLOUD_RGB = (255, 255, 255)
MAR_GROUND_TOP_RGB = (70, 200, 70)
MAR_GROUND_BODY_RGB = (170, 110, 60)
MAR_GROUND_LINE_RGB = (120, 75, 35)
MAR_PIPE_RGB = (50, 180, 70)
MAR_PIPE_EDGE_RGB = (25, 120, 45)
MAR_BLOCK_RGB = (180, 110, 60)
MAR_ITEM_RGB = (250, 190, 30)
MAR_COIN_RGB = (255, 215, 0)
MAR_ENEMY_RGB = (150, 90, 40)
MAR_PLAYER_RED_RGB = (216, 30, 30)
MAR_PLAYER_OVER_RGB = (40, 80, 190)
MAR_PLAYER_SKIN_RGB = (250, 205, 160)
MAR_PLAYER_SHOE_RGB = (70, 45, 25)

# ===== Lighting #12(相対順): Missile Defense（Phase 5B。CoreS3 lightRenderMissile()準拠）=====
MSL_TOP = SCENE_TOP
MSL_GROUND_Y = 206
MSL_MAX_ENEMY = 3
MSL_BURST_COUNT = 4
MSL_BURST_FLASH_MS = 260
MSL_BURST_LIFE_MS = 900
MSL_BURST_MAX_R = 34
MSL_LOCK_DIST = 9.0
MSL_SHOT_DURATION_MS = 130
MSL_REACQUIRE_MIN_MS = 350
MSL_REACQUIRE_MAX_MS = 900
MSL_CSTAR_COUNT = 16
MSL_BASE_X = (54, 160, 266)
MSL_AIM_SEARCH, MSL_AIM_TRACK, MSL_AIM_COOLDOWN = range(3)
MSL_SKY_RGB = (0, 0, 0)
MSL_GROUND_RGB = (255, 210, 0)
MSL_MISSILE_RGB = (60, 160, 255)
MSL_BASE_RGB = (40, 220, 90)
MSL_AIM_RGB = (255, 255, 255)
MSL_SHOT_RGB = (200, 255, 255)
MSL_FLASH_RGB = (255, 255, 255)
MSL_FLASH2_RGB = (255, 160, 0)
MSL_SMOKE_RGB = (190, 190, 195)
MSL_CSTAR_PALETTE_RGB = [(255, 0, 0), (255, 255, 255), (255, 255, 0), (0, 255, 0)]  # RED/WHITE/YELLOW/GREEN

# ===== Lighting #13(相対順): Psychedelic / Trance（v0.5 Phase 4残り。CoreS3
# lightRenderPsychedelic()準拠。FLASH/MOTION/ACCENTの三層モンタージュ。音声非依存）=====
PSY_TOP = SCENE_TOP
PSY_LAYER_FLASH, PSY_LAYER_MOTION = 0, 1
PSY_M_RINGS, PSY_M_SPIRAL, PSY_M_MOIRE, PSY_M_WEDGE, PSY_M_SHARD, PSY_M_COUNT = range(6)
PSY_F_OPGRID, PSY_F_DOTS, PSY_F_HATCH, PSY_F_WOBBLE, PSY_F_COUNT = range(5)
PSY_A_FACE, PSY_A_INVERT, PSY_A_STAB, PSY_A_GIANT, PSY_A_SCAN, PSY_A_COUNT = range(6)
PSY_FLASH_MIN_F, PSY_FLASH_MAX_F = 1, 3
PSY_MOTION_MIN_F, PSY_MOTION_MAX_F = 8, 16
PSY_SHARD_MAX_F = 11
PSY_ACCENT_MIN_F, PSY_ACCENT_MAX_F = 1, 2
PSY_BURST_MIN, PSY_BURST_MAX = 2, 5
PSY_INTR_PCT = 11
PSY_INVERT_PCT = 7
PSY_RAMP = 1.25
PSY_FACE_MIN_GAP = 3
PSY_FACE_FORCE_GAP = 8
PSY_FACE_ZOOM = (1.7, 2.0, 2.3, 2.6)
PSY_FACE_ZOOM_STEP = 1.15
# CoreS3 PSY_PAL[12]（RGB565パック値）をRGB888へ正確に展開した12パレット
# （bg=背景／mn=主色／ac=アクセント／dark=暗いパレットか）。
PSY_PAL_RGB = [
    {"bg": (0, 0, 0), "mn": (255, 0, 0), "ac": (255, 255, 0), "dark": False},
    {"bg": (0, 0, 255), "mn": (255, 166, 0), "ac": (255, 255, 255), "dark": False},
    {"bg": (41, 0, 49), "mn": (255, 0, 255), "ac": (0, 255, 255), "dark": False},
    {"bg": (0, 0, 0), "mn": (0, 255, 0), "ac": (123, 0, 123), "dark": False},
    {"bg": (255, 0, 0), "mn": (0, 0, 0), "ac": (255, 255, 255), "dark": True},
    {"bg": (255, 255, 0), "mn": (0, 0, 255), "ac": (255, 0, 0), "dark": False},
    {"bg": (255, 255, 255), "mn": (0, 0, 0), "ac": (255, 0, 0), "dark": False},
    {"bg": (255, 130, 0), "mn": (49, 0, 82), "ac": (0, 255, 255), "dark": True},
    {"bg": (173, 255, 41), "mn": (255, 0, 181), "ac": (0, 0, 0), "dark": False},
    {"bg": (0, 255, 255), "mn": (0, 0, 99), "ac": (255, 255, 0), "dark": True},
    {"bg": (255, 48, 148), "mn": (0, 56, 24), "ac": (255, 255, 0), "dark": False},
    {"bg": (0, 0, 57), "mn": (255, 255, 255), "ac": (255, 0, 0), "dark": True},
]
# CoreS3 PSY_SPECTRUM[12]（RGB565）をRGB888へ正確に展開。
PSY_SPECTRUM_RGB = [
    (255, 0, 0), (255, 97, 0), (255, 166, 0), (255, 255, 0), (173, 255, 41), (0, 255, 0),
    (0, 255, 173), (0, 255, 255), (0, 130, 255), (49, 0, 123), (173, 0, 255), (255, 0, 255),
]
PSY_RING_MAX = 12
PSY_RING_PRELOAD = 10
PSY_RING_SPAWN_LOW = 3
PSY_RING_SPAWN_HI = 2
PSY_RING_LOW_COUNT = 7
PSY_RING_KILL_R = 215.0
PSY_RING_R0 = 3.5
PSY_RING_V0 = 5.5
PSY_RING_GRO_MIN = 1.030
PSY_RING_GRO_MAX = 1.075
PSY_RING_V_CLAMP = 34.0
PSY_RING_JITTER = 30.0
PSY_RING_DRIFT_X = 1.8
PSY_RING_DRIFT_Y = 1.3
PSY_RING_ELL_MIN = 0.62
PSY_RING_ELL_MAX = 1.48
PSY_RING_BIRTH_VX = 2.6
PSY_RING_BIRTH_VY = 1.8
PSY_RING_W_THIN_MIN, PSY_RING_W_THIN_MAX = 1, 2
PSY_RING_W_MID_MIN, PSY_RING_W_MID_MAX = 3, 6
PSY_RING_W_FAT_MIN, PSY_RING_W_FAT_MAX = 9, 15
PSY_RING_W_THIN_PCT = 30
PSY_RING_W_MID_PCT = 45
PSY_RING_POOL_MAX = 9
PSY_RING_RUN_PCT = 35
PSY_WD_MAX = 20
PSY_TRI_N = 15

# ===== Lighting #6(相対順): Hypnotic Vortex（CoreS3 lightRenderVortex()準拠）=====
VTX_TOP = SCENE_TOP
VTX_CX = 160
VTX_CY = (VTX_TOP + CANVAS_H) // 2
VTX_ARMS = 6
VTX_STEPS = 24
VTX_MAXR = 196.0
VTX_TWIST = 2.6
VTX_ROT_SPEED = 0.15  # rad/秒

# ===== Lighting #17(相対順): Aquarium（v0.5 Phase 4残り。CoreS3
# lightRenderAquarium()準拠。KariPom独自表現の水槽風スクリーンセーバー）=====
AQU_TOP = SCENE_TOP
AQU_FISH_COUNT = 3
AQU_SPECIES_COUNT = 4
AQU_BUBBLE_COUNT = 10
AQU_WEED_COUNT = 3
AQU_WEED_BLADES = 3
AQU_WEED_SEGS = 5
AQU_ROCK_COUNT = 2
AQU_SAND_DOT_COUNT = 14
# AquSpecies: (bodyLen, bodyHi, tailSize, pattern, longFins, tailStyle, speedMul)
AQU_SPECIES = [
    (26.0, 32.0, 16.0, 1, True, 0, 0.85),
    (36.0, 15.0, 15.0, 4, False, 0, 1.15),
    (20.0, 14.0, 28.0, 0, False, 1, 0.75),
    (21.0, 21.0, 11.0, 3, False, 0, 0.95),
]

# ===== Lighting #18(相対順): Flying Pompadour（v0.5 Phase 4残り。CoreS3
# lightRenderFlyingPompadour()準拠。「Flying Toasters」へのオマージュ。
# トースターの代わりにCoreS3風の小型直方体デバイス＋羽が右上→左下へ飛ぶ）=====
FLYP_TOP = SCENE_TOP
FLYP_SLOT_COUNT = 6
FLYP_LAYER_COUNT = 3
FLYP_PALETTE_COUNT = 4
FLYP_STAR_COUNT = 10
FLYP_CLOUD_COUNT = 3
FLYP_WING_FRAMES = (-1.0, -0.25, 0.5, 1.0)
# FlypPalette: (body, bodyShade, bezel, wing, wingShade)
FLYP_PALETTE_RGB = [
    {"body": (238, 232, 222), "bodyShade": (180, 172, 160), "bezel": (120, 108, 118), "wing": (255, 196, 206), "wingShade": (206, 150, 160)},
    {"body": (214, 232, 248), "bodyShade": (158, 182, 206), "bezel": (96, 108, 130), "wing": (190, 222, 240), "wingShade": (140, 176, 198)},
    {"body": (224, 246, 224), "bodyShade": (166, 202, 172), "bezel": (100, 128, 110), "wing": (210, 236, 190), "wingShade": (158, 196, 140)},
    {"body": (236, 224, 246), "bodyShade": (180, 164, 206), "bezel": (122, 108, 138), "wing": (224, 200, 240), "wingShade": (176, 152, 202)},
]
FLYP_LAYER_SCALE_LO = (0.55, 0.95, 1.45)
FLYP_LAYER_SCALE_HI = (0.80, 1.30, 1.95)
FLYP_LAYER_SPEED_LO = (0.37, 0.57, 0.77)
FLYP_LAYER_SPEED_HI = (0.53, 0.77, 1.03)

# ===== Lighting: Rainbow Washing Machine（CoreS3 lightRenderRainbowWashingMachine()準拠）=====
RWM_TOP = SCENE_TOP
RWM_CX = 160
RWM_CY = (RWM_TOP + CANVAS_H) // 2
RWM_MAX_R = 200.0
RWM_COUNT = 510
# CoreS3 RWM_PALETTE[9]（RGB565）をRGB888へ正確に展開した値。
RWM_PALETTE_RGB = [
    (255, 0, 0), (255, 166, 0), (255, 255, 0), (173, 255, 41), (0, 255, 0),
    (0, 255, 255), (0, 0, 255), (123, 0, 123), (255, 0, 255),
]
RWM_BAND_COUNT = 5
RWM_BAND_ANGVEL = [1.05, -1.55, 0.80, -1.35, 1.15]
RWM_BAND_WOBBLE_FREQ = [0.35, 0.50, 0.28, 0.60, 0.40]
RWM_BAND_WOBBLE_AMP = 0.15
RWM_GROWTH_RATE = 0.50
RWM_GROWTH_MIN = 5.0
RWM_RESPAWN_R = 2.0
RWM_RESPAWN_JITTER = 6.0

# ===== Lighting: Pixel Invasion（v0.5 Phase 5A。CoreS3 lightRenderPixelInvasion()準拠。
# CoreS3 LIGHT_PIXELINVASION=19相当。1970年代末〜80年代初頭の固定画面シューティングへの
# オマージュ。敵編隊8列×5段・自機・シールド4個・弾・UFOをすべて本ブロック内の
# オリジナルドット絵で表現。スコア・残機・GAME OVER・ユーザー操作は無く、生存数が
# 少ない／編隊が下降しすぎた／一定時間経過のいずれかで新しい編隊へ自然に切り替わる
# 永久ループのアニメーションLighting）=====
PIX_TOP = SCENE_TOP
PIX_COLS = 8
PIX_ROWS = 5
PIX_INV_CELL = 2       # 敵1体＝8x8ドット絵 × 2px = 16x16画面px
PIX_INV_HALF = 8
PIX_FORM_LEFT_X0 = 32
PIX_FORM_COL_DX = 34
PIX_FORM_TOP_Y = PIX_TOP + 4
PIX_FORM_ROW_DY = 20
PIX_STEP_PX = 6         # 1ステップあたりの水平移動量（「カッ、カッ、カッ」の刻み）
PIX_DROP_PX = 4         # 反転時に下降する量
PIX_FORM_MARGIN = 16    # 編隊が反転する画面端からの余白
PIX_RESET_ALIVE_MIN = 3     # 生存数がこれ以下になったら新しい編隊へ
PIX_ROUND_MAX_MS = 70000    # 安全策：この時間が経過したら強制的に新しい編隊へ

PIX_SHIELD_COUNT = 4
PIX_SHIELD_ROWS = 6
PIX_SHIELD_COLS = 10
PIX_SHIELD_CELL = 3
PIX_SHIELD_Y = 178

PIX_PLAYER_Y = 224
PIX_PLAYER_CELL = 2
PIX_PLAYER_MARGIN = 16
PIX_PLAYER_STEP_MS = 40
PIX_PLAYER_STEP_PX = 2.0

PIX_UFO_CELL = 2
PIX_UFO_Y = PIX_TOP + 12
PIX_UFO_SPEED = 55.0
PIX_UFO_MIN_GAP_MS = 9000
PIX_UFO_GAP_JITTER_MS = 12000

PIX_PBULLET_MAX = 1
PIX_EBULLET_MAX = 3
PIX_PBULLET_SPEED = 170.0
PIX_EBULLET_SPEED = 115.0
PIX_PLAYER_FIRE_MIN_MS = 1200
PIX_PLAYER_FIRE_JITTER_MS = 1300
PIX_ENEMY_FIRE_MIN_MS = 500
PIX_ENEMY_FIRE_JITTER_MS = 900
PIX_KILL_FLASH_MS = 150

# 黒背景・マゼンタ／紫・ターコイズ・グリーン・自機水色・敵弾赤・シールド赤・UFO橙黄
PIX_COL_MAGENTA = (230, 40, 220)
PIX_COL_TURQUOISE = (40, 220, 210)
PIX_COL_GREEN = (60, 220, 90)
PIX_COL_PLAYER = (80, 200, 255)
PIX_COL_PBULLET = (160, 235, 255)
PIX_COL_EBULLET = (255, 70, 70)
PIX_COL_SHIELD = (220, 30, 30)
PIX_COL_UFO = (255, 195, 40)

# 敵編隊のドット絵（8x8、1バイト=1行。bit7が左端、bit0が右端）。実在ゲームのスプライトは
# 参照せず、CoreS3側で新規デザインされた抽象的な宇宙生物のシルエット。段グループごとに
# 2フレーム（脚／触手のポーズ違い）。
PIX_SPR_TOP = [
    [0x3C, 0x7E, 0xFF, 0xDB, 0xFF, 0x24, 0x42, 0x81],
    [0x3C, 0x7E, 0xFF, 0xDB, 0xFF, 0x5A, 0x24, 0x5A],
]
PIX_SPR_MID = [
    [0x18, 0x3C, 0x7E, 0xFF, 0x66, 0xDB, 0x5A, 0xA5],
    [0x18, 0x3C, 0x7E, 0xFF, 0x66, 0xDB, 0xA5, 0x5A],
]
PIX_SPR_BOT = [
    [0x5A, 0x3C, 0x7E, 0xDB, 0xFF, 0x42, 0xA5, 0x42],
    [0x5A, 0x3C, 0x7E, 0xDB, 0xFF, 0xA5, 0x42, 0xA5],
]
PIX_SPR_PLAYER = [0x18, 0x3C, 0x7E, 0xFF, 0xDB, 0xA5]  # 自機（8x6、水色系の小さな砲台シルエット）
PIX_SPR_UFO = [0x3C, 0x7E, 0xFF, 0x5A, 0x24]           # UFO（8x5、オリジナルの円盤シルエット）
# シールドの初期形状（6行×10列、ドーム状＋下部にアーチの切り欠き）。ビット表現はCoreS3と同一。
PIX_SHIELD_BASE = [0x0FC, 0x1FE, 0x3FF, 0x3FF, 0x387, 0x303]

# ===== Lighting #9(相対順): Flower Clock（CoreS3 lightRenderFlowerClock()準拠）=====
# 12色の花びら状文字盤＋中央白窓に12/3/6/9の数字を描く背景と、顔レイヤーより
# 最前面へ時計の針を上書きするForeground Hook（CoreS3 fcDrawHandsForeground()）の
# 組み合わせ。針を背景側で描くと顔の鼻・口に隠れてしまうため、CoreS3と同じく
# 針だけを別ステップに分離している（_draw_lighting_foreground()から呼ぶ）。
FC_TOP = SCENE_TOP
FC_CX = 160
FC_CY = (FC_TOP + 240) // 2
FC_PETALS = 12
FC_RECT_HW = 90.0
FC_RECT_HH = 54.0
FC_RECT_R = 16.0
FC_R_OUTER = 220.0  # 画面四隅までの距離より大きく取り、clipで実際には画面いっぱいまで塗る
FC_NUM_PT = 14       # CoreS3 FC_NUM_TEXTSIZE=2（既定フォント12x16px/文字）に相当するQFontポイント数
FC_NUM_DY = 43.0
FC_NUM_DX = 80.0
FC_HAND_MIN_LEN = 50.0
FC_HAND_HOUR_LEN = 40.0
FC_HAND_MIN_HW = 7.0
FC_HAND_HOUR_HW = 6.0
FC_COL_PINK_RGB = (255, 105, 181)     # CoreS3 FC_COL_PINK(0xFB56)をRGB888へ正確に展開
FC_COL_PURPLE_RGB = (140, 40, 231)    # CoreS3 FC_COL_PURPLE(0x895C)をRGB888へ正確に展開
# CoreS3 FC_COLORS[12]（RGB565）をRGB888へ正確に展開。12時方向から時計回りに30°刻み。
FC_COLORS_RGB = [
    (255, 0, 0), (255, 130, 0), (255, 255, 0), (132, 255, 0), (0, 255, 0), (0, 255, 132),
    (0, 255, 255), (0, 130, 255), (0, 0, 255), (132, 0, 255), (255, 0, 255), (255, 0, 132),
]

# ===== Lighting #22(相対順): BASEBALL Arcade（Phase 5B。CoreS3 lightRenderBaseball()準拠。
# 実時間dt方式のため、Fighter Duel/Missile DefenseのLIGHT_FRAME_UPDATE_MSゲートは使わない）=====
BB_TOP = SCENE_TOP
BB_BOTTOM = 240
BB_HOME_X, BB_HOME_Y = 160.0, 222.0
BB_FIRST_X, BB_FIRST_Y = 240.0, 178.0
BB_SECOND_X, BB_SECOND_Y = 160.0, 127.0
BB_THIRD_X, BB_THIRD_Y = 80.0, 178.0
BB_PITCHER_X, BB_PITCHER_Y = 160.0, 187.0
# 守備選手9人の定位置（0-5内野：投手・捕手・一塁・二塁・三塁・遊撃／6-8外野：左翼・中堅・右翼）
BB_FLD_HOME_X = (160.0, 160.0, 260.0, 195.0, 60.0, 125.0, 80.0, 160.0, 240.0)
BB_FLD_HOME_Y = (187.0, 212.0, 167.0, 147.0, 167.0, 147.0, 97.0, 72.0, 97.0)
BB_INFIELD_LO, BB_INFIELD_HI = 0, 5
BB_OUTFIELD_LO, BB_OUTFIELD_HI = 6, 8
BB_FIELD_RGB = (40, 150, 55)
BB_DIRT_RGB = (150, 100, 55)
BB_LINE_RGB = (255, 255, 255)
BB_PLAYER_RGB = (255, 255, 255)
BB_BALL_RGB = (255, 255, 255)
BB_CAP_RGB = (25, 40, 95)
BB_PITCH_MS = 500.0
BB_SWING_PAUSE_MS = 150.0
BB_GROUND_SPEED = 220.0
BB_FLY_SPEED = 170.0
BB_FIELDER_SPEED = 90.0
BB_MIN_FLIGHT_MS = 260.0
BB_OUT_JITTER = 12
BB_HIT_GAP_TRIES = 140
BB_HIT_CHASE_FACTOR = 0.5
# CoreS3実コード（v2.6）の現在値をそのまま使用。ファイル冒頭のv2.4設計コメント（OUT22%等）は
# 後のv2.6で変更され、実際の#defineはOUT0%/DOUBLE20%/HR5%（残り75%がSINGLE）になっている。
BB_OUT_PCT = 0
BB_DOUBLE_PCT = 20
BB_HR_PCT = 5
BB_DIRT_HOME_R = 24.0
BB_MOUND_R = 14.0
BB_DIRT_BAND_HALFW = 14.0
BB_DIRT_CURVE_SEGS = 12
BB_HOME_SCR_Y = 226.0
BB_HORIZON_SCR_Y = 58.0
BB_MAX_DEPTH = 174.0
BB_MIN_SCALE = 0.34
BB_PERSPECTIVE_POW = 1.6
BB_HOME_SCR_X = 160.0
BB_FIELD_HALF_ANGLE_DEG = 60.0
BB_ST_PITCH, BB_ST_SWING_PAUSE, BB_ST_BALL_FLIGHT = range(3)

# Visualizer Random / Lighting Random 共通の切替間隔（3分）。
# CoreS3のrandomIntervalMs()相当（ただしDesktopでは5/10/15分の選択式にはせず固定）。
RANDOM_INTERVAL_MS = 180_000


def pick_random_avoiding(count: int, last_pick: int) -> int:
    """count個の候補からlast_pickと異なる値を1つ選ぶ（CoreS3のdo-while抽選と同じ考え方）。"""
    if count <= 1:
        return 0
    pick = last_pick
    while pick == last_pick:
        pick = random.randrange(count)
    return pick



# =============================================================================
# KariPom BBX Desktop-parity renderer (2026-08-17)
# The drawing/animation implementation is intentionally kept in sync with the
# supplied KariPom Desktop; Companion uses its own audio engine/state.
# =============================================================================
class KariPomDesktopCanvas(QWidget):
    """320x240の実機座標系をそのまま描くPC版かりポムの描画キャンバス（v0.4まではウィンドウ
    そのものだったが、v0.5でボタン行を追加するため子QWidgetへ分離した：Phase 1）。"""

    def __init__(self, audio_state, scale: int) -> None:
        super().__init__()
        self.audio_state = audio_state
        self.scale_factor = scale

        self.eye_mode = "open"  # open / blink
        self.nose_offset = 0
        self.mouth_open = False
        self.mouth_mx = NOSE_X
        self.mouth_my = NOSE_Y + 26
        self.mouth_mw = 18
        self.mouth_mh = 12
        self.was_speaking = False

        # ----- Character（v0.5 Phase 2で追加）-----
        # KariPom / Ms. KariPom / None の3状態。CharacterにRandomは無い。
        self.character_mode = CHAR_KARIPOM

        # ----- Visualizer選択状態（v0.5 Phase 2でボタン方式へ変更）-----
        # viz_manual_index: 0..len(VIS_MODES)-1 は VIS_MODES の直接選択、
        #   len(VIS_MODES) は「Random」（UI専用の疑似状態）を意味する。
        # viz_display_mode: 実際に描画対象となっているVIS_MODESのインデックス
        #   （Random中は _update_visualizer_random() が3分ごとに書き換える）。
        self.viz_manual_index = 0  # Face
        self.viz_display_mode = 0
        self.viz_random_last_switch_ms = 0
        self.viz_random_last_pick = -1

        # ----- Lighting選択状態（v0.5 Phase 2で追加。Phase 3以降で実描画を拡充）-----
        # light_manual_index: 0..len(LIGHT_MODES)-1 は LIGHT_MODES の直接選択
        #   （index 0 = "None"）、len(LIGHT_MODES) は「Random」を意味する。
        self.light_manual_index = 0  # None
        self.light_display_mode = 0
        self.light_random_last_switch_ms = 0
        self.light_random_last_pick = -1
        self.light_prev_display_mode = -1  # 直前フレームのLighting（切替検出＝CoreS3のneedsInit相当）

        # ----- Lighting #1: Disco Floor（v0.5 Phase 3）-----
        self.disco_ready = False
        self.disco_pal = DISCO_PAL_RGB
        self.disco_face_tile = [False] * DISCO_TILES
        self.disco_arg = [[0.0] * DISCO_TILES for _ in range(DISCO_NPAT)]
        self.disco_travel = 0.0
        self.disco_hue = 0.0
        self.disco_pat = 0
        self.disco_pat_ms = 0
        self.disco_pat_rng = 0x9E3779B9
        self.disco_pal_set = 0
        self.disco_pal_off = 0
        self.disco_energy = 0.0
        self.disco_agc_peak = 0.0
        self.disco_bass_avg = 0.0
        self.disco_flash = 0.0
        self.disco_beat_cd = 0
        self.disco_rng = 0x1234ABCD

        # ----- Lighting #2: Laser Show（v0.5 Phase 3）-----
        self.laser_beam = []       # 今フレームのビーム [{x0,y0,x1,y1,hue,bright}, ...]
        self.laser_prev = []       # 直前フレームのビーム（淡い残像用）
        self.laser_pat = 0
        self.laser_pat_ms = 0
        self.laser_phase = 0.0
        self.laser_energy = 0.0
        self.laser_agc_peak = 0.0
        self.laser_bass_avg = 0.0
        self.laser_flash = 0.0
        self.laser_beat_cd = 0
        self.laser_rng = 0xB5297A4D
        self.laser_rand_ms = 0

        # ----- Lighting #3: Aurora（v0.5 Phase 3）-----
        self.aurora_ready = False
        self.aurora_pal = AURORA_PAL_RGB
        self.aurora_col_base = [c * 0.55 for c in range(AURORA_NCOL)]
        self.aurora_t = 0.0
        self.aurora_energy = 0.0
        self.aurora_agc_peak = 0.0
        self.aurora_bass_avg = 0.0
        self.aurora_flash = 0.0
        self.aurora_beat_cd = 0

        # ----- Lighting #4: Matrix（v0.5 Phase 3）-----
        self.matrix_ready = False
        self.matrix_head_y = [0.0] * MATRIX_NCOL
        self.matrix_len = [4] * MATRIX_NCOL
        self.matrix_speed_var = [1.0] * MATRIX_NCOL
        self.matrix_energy = 0.0
        self.matrix_agc_peak = 0.0
        self.matrix_bass_avg = 0.0
        self.matrix_burst = 0.0
        self.matrix_beat_cd = 0
        self.matrix_rng = 0x51ED2A3B
        self.matrix_scan_y = -1

        # ----- Lighting: Retro Race（v0.5 Phase 4b。CoreS3 lightRenderRace()準拠）-----
        self.race_ready = False
        self.race_rng = 0x1234ABCD
        self.race_curve_smooth = 0.0
        self.race_curve_target = 0.0
        self.race_curve_change_at = 0
        self.race_player_x = 0.0
        self.race_player_x_target = 0.0
        self.race_speed = 1.0
        self.race_speed_target = 1.0
        self.race_speed_change_at = 0
        self.race_scroll_z = 0.0
        self.race_car_z = [0.0] * RACE_MAX_CARS
        self.race_car_lane = [0.0] * RACE_MAX_CARS
        self.race_car_speed_mul = [1.0] * RACE_MAX_CARS
        self.race_car_avoid_side = [1] * RACE_MAX_CARS
        self.race_car_color_idx = [0] * RACE_MAX_CARS
        self.race_sign_z = [0.0] * RACE_MAX_SIGNS
        self.race_sign_side = [1] * RACE_MAX_SIGNS
        self.race_sign_type = [0] * RACE_MAX_SIGNS
        self.race_sign_color_idx = [0] * RACE_MAX_SIGNS
        self.race_sign_text_idx = [0] * RACE_MAX_SIGNS
        self.race_sign_scale = [1.0] * RACE_MAX_SIGNS
        self.race_sign_speed_mul = [1.0] * RACE_MAX_SIGNS

        # ----- Lighting: Sky Raid（v0.5 Phase 4b。CoreS3 lightRenderSkyRaid()準拠）-----
        self.xev_ready = False
        self.xev_rng = 0x9E3779B9
        self.xev_seed = 0
        self.xev_scroll_y = 0.0
        self.xev_speed = 1.0
        self.xev_speed_target = 1.0
        self.xev_speed_change_at = 0
        self.xev_ship_x = 160.0
        self.xev_ship_x_target = 160.0
        self.xev_next_shot_at = 0
        self.xev_enemy_y = [0.0] * XEV_MAX_ENEMIES
        self.xev_enemy_x = [0.0] * XEV_MAX_ENEMIES
        self.xev_enemy_speed = [0.0] * XEV_MAX_ENEMIES
        self.xev_enemy_wob_phase = [0.0] * XEV_MAX_ENEMIES
        self.xev_enemy_color_idx = [0] * XEV_MAX_ENEMIES
        self.xev_shot_x = [0.0] * XEV_MAX_SHOTS
        self.xev_shot_y = [0.0] * XEV_MAX_SHOTS
        self.xev_shot_active = [False] * XEV_MAX_SHOTS
        self.xev_expl_x = [0.0] * XEV_MAX_EXPL
        self.xev_expl_y = [0.0] * XEV_MAX_EXPL
        self.xev_expl_age = [0] * XEV_MAX_EXPL
        self.xev_expl_seed = [0] * XEV_MAX_EXPL

        # ----- Lighting: Hypnotic Vortex（v0.5 Phase 3）-----
        self.vtx_ready = False
        self.vtx_radius_tab = [0.0] * VTX_STEPS
        self.vtx_twist_tab = [0.0] * VTX_STEPS
        self.vtx_rot = 0.0
        self.vtx_prev_ms = 0

        # ----- Lighting: Rainbow Washing Machine（v0.5 Phase 3）-----
        self.rwm_ready = False
        self.rwm_shard = []
        self.rwm_lcg = 20260810
        self.rwm_prev_ms = 0
        self.rwm_time_sec = 0.0

        # ----- Lighting: Eye Slot（v0.5 Phase 4a）-----
        # CoreS3 gEyeSlotActive相当。このフレームでEye Slotが目として採用されているか
        # （顔描画側で黒目描画をスキップする判定に使う。毎フレーム描画開始時にリセット）。
        self.eslot_active = False
        self.eslot_state = ESLOT_ST_ACCEL
        self.eslot_state_at = 0
        self.eslot_state_dur = 0
        self.eslot_reel_pos = [0.0, 0.0]
        self.eslot_decel_from = [0.0, 0.0]
        self.eslot_decel_to = [0.0, 0.0]
        self.eslot_decel_at = [0, 0]
        self.eslot_decel_dur = [0, 0]
        self.eslot_match = False
        self.eslot_result_at = 0
        self.eslot_rng = 0x51A5E1D1

        # ----- Lighting: Classic Race（v0.5 Phase 4b。CoreS3
        # lightRenderClassicRace()準拠）-----
        self.crace_ready = False
        self.crace_rng = 0x8B6A5CDE
        self.crace_curve_smooth = 0.0
        self.crace_curve_target = 0.0
        self.crace_curve_change_at = 0
        self.crace_player_x = 0.0
        self.crace_player_x_target = 0.0
        self.crace_speed = 1.0
        self.crace_speed_target = 1.0
        self.crace_speed_change_at = 0
        self.crace_scroll_z = 0.0
        self.crace_enemy_y = [0.0] * CRACE_MAX_ENEMIES
        self.crace_enemy_lane = [0.0] * CRACE_MAX_ENEMIES
        self.crace_enemy_speed_mul = [1.0] * CRACE_MAX_ENEMIES
        self.crace_enemy_avoid_side = [1] * CRACE_MAX_ENEMIES
        self.crace_enemy_color_idx = [0] * CRACE_MAX_ENEMIES
        self.crace_oil_y = -9999.0
        self.crace_oil_lane = 0.0
        self.crace_checker_y = -9999.0

        # ----- Lighting: Asteroid Field（v0.5 Phase 4b。CoreS3
        # lightRenderAsteroid()準拠。かりポムの黒目はDesktopにeyeOffsetX/Y相当の機構が
        # 無いため自機追従の目線演出は実装していない（構造的な仕様差。隕石／自機／弾の
        # 生成・移動・回転・ラップアラウンド・発射ロジックは変更していない）-----
        self.astr_ready = False
        self.astr_rng = 0x2F19E7A3
        self.astr_x = [0.0] * ASTR_COUNT
        self.astr_y = [0.0] * ASTR_COUNT
        self.astr_vx = [0.0] * ASTR_COUNT
        self.astr_vy = [0.0] * ASTR_COUNT
        self.astr_size = [10.0] * ASTR_COUNT
        self.astr_angle = [0.0] * ASTR_COUNT
        self.astr_ang_vel = [0.0] * ASTR_COUNT
        self.astr_vert_count = [5] * ASTR_COUNT
        self.astr_vert_r = [[1.0] * ASTR_MAX_VERTS for _ in range(ASTR_COUNT)]
        self.astr_color = [ASTR_NEON_RGB[0]] * ASTR_COUNT
        self.astr_star_x = [0] * ASTR_STAR_COUNT
        self.astr_star_y = [0] * ASTR_STAR_COUNT
        self.astr_star_col = [ASTR_STAR_DIM_RGB] * ASTR_STAR_COUNT
        self.astr_cstar_x = [0] * ASTR_CSTAR_COUNT
        self.astr_cstar_y = [0] * ASTR_CSTAR_COUNT
        self.astr_cstar_col = [ASTR_CSTAR_PALETTE_RGB[0]] * ASTR_CSTAR_COUNT
        self.astr_craft_x = 160.0
        self.astr_craft_y = 144.0
        self.astr_craft_vx = 0.0
        self.astr_craft_vy = 0.0
        self.astr_craft_target_x = 160.0
        self.astr_craft_target_y = 144.0
        self.astr_craft_angle = 0.0
        self.astr_craft_retarget_at = 0
        self.astr_bullet_active = [False] * ASTR_BULLET_COUNT
        self.astr_bullet_x = [0.0] * ASTR_BULLET_COUNT
        self.astr_bullet_y = [0.0] * ASTR_BULLET_COUNT
        self.astr_bullet_dx = [0.0] * ASTR_BULLET_COUNT
        self.astr_bullet_dy = [0.0] * ASTR_BULLET_COUNT
        self.astr_bullet_dist = [0.0] * ASTR_BULLET_COUNT
        self.astr_next_fire_at = 0
        self.astr_last_fire_at = 0

        # ----- Lighting: Tempest Tunnel（v0.5 Phase 4b。CoreS3
        # lightRenderTunnel()準拠。黒目はAsteroid Field同様の理由で実装していない
        # （トンネル本体・自機・発光体・弾のロジックは変更していない）-----
        self.tun_ready = False
        self.tun_rng = 0x51A7C33D
        self.tun_verts = 12
        self.tun_rot_dir = 1
        self.tun_rot_angle = 0.0
        self.tun_flow_phase = 0.0
        self.tun_pulse_phase = 0.0
        self.tun_hue_deg = 0.0
        self.tun_last_ms = 0
        self.tun_cstar_x = [0] * TUN_CSTAR_COUNT
        self.tun_cstar_y = [0] * TUN_CSTAR_COUNT
        self.tun_cstar_col = [TUN_CSTAR_PALETTE_RGB[0]] * TUN_CSTAR_COUNT
        self.tun_craft_angle = TUN_CRAFT_ARC_CENTER
        self.tun_craft_target_angle = TUN_CRAFT_ARC_CENTER
        self.tun_craft_avoid_bias = 0.0
        self.tun_craft_retarget_at = 0
        self.tun_enemy_depth = [0.0] * TUN_ENEMY_COUNT
        self.tun_enemy_angle = [0.0] * TUN_ENEMY_COUNT
        self.tun_enemy_speed = [0.0] * TUN_ENEMY_COUNT
        self.tun_enemy_type = [0] * TUN_ENEMY_COUNT
        self.tun_bullet_active = [False] * TUN_BULLET_COUNT
        self.tun_bullet_angle = [0.0] * TUN_BULLET_COUNT
        self.tun_bullet_radius = [0.0] * TUN_BULLET_COUNT
        self.tun_next_fire_at = 0
        self.tun_last_fire_at = 0

        # ----- Lighting: PINBALL Arcade（Phase 5C。CoreS3 v5.6準拠）-----
        self.pin_ball_x = 160.0; self.pin_ball_y = PIN_LAUNCH_Y; self.pin_ball_vx = 0.0; self.pin_ball_vy = 70.0
        self.pin_prev_ms = 0; self.pin_last_fast_ms = 0
        self.pin_stuck_ref_x = 160.0; self.pin_stuck_ref_y = PIN_LAUNCH_Y; self.pin_stuck_ref_ms = 0
        self.pin_score = 0
        self.pin_eye_flashing = [False, False]; self.pin_eye_flash_ms = [0, 0]
        self.pin_nose_flashing = False; self.pin_nose_flash_ms = 0
        self.pin_mouth_pass_flashing = False; self.pin_mouth_pass_flash_ms = 0
        self.pin_flip_state = [PIN_FLIP_IDLE, PIN_FLIP_IDLE]
        self.pin_flip_state_start = [0, 0]; self.pin_flip_last_fire = [0, 0]

        # ----- Lighting: SKY BURNER（Phase 5C。CoreS3 v1.0準拠）-----
        self.skb_state = SKB_ST_SEARCH; self.skb_state_start_ms = 0; self.skb_next_at = 0; self.skb_prev_ms = 0
        self.skb_rng = 0x5A17B00B; self.skb_bank_deg = 0.0
        self.skb_enemy_alive = False; self.skb_enemy_x = 160.0; self.skb_enemy_y = 90.0
        self.skb_enemy_target_x = 160.0; self.skb_enemy_target_y = 90.0; self.skb_enemy_wander_at = 0
        self.skb_enemy_scale = 0.35; self.skb_enemy_bank_deg = 0.0; self.skb_engage_start_ms = 0
        self.skb_aim_x = 160.0; self.skb_aim_y = SKB_HORIZON_Y0 - 26.0; self.skb_lock_timer_ms = 0.0
        self.skb_msl_active = False; self.skb_msl_x = 0.0; self.skb_msl_y = 0.0; self.skb_msl_vx = 0.0; self.skb_msl_vy = 0.0; self.skb_msl_start_ms = 0
        self.skb_smoke = [None] * SKB_SMOKE_MAX; self.skb_esmoke = [None] * SKB_ESMOKE_MAX
        self.skb_emsl_active = False; self.skb_emsl_phase = SKB_EMSL_APPROACH; self.skb_emsl_x = 0.0; self.skb_emsl_y = 0.0; self.skb_emsl_scale = 0.0
        self.skb_emsl_spawn_x = 0.0; self.skb_emsl_spawn_y = 0.0; self.skb_emsl_dodge_vx = 0.0; self.skb_emsl_dodge_vy = 0.0
        self.skb_emsl_phase_start_ms = 0; self.skb_emsl_next_at = 0
        self.skb_expl_active = False; self.skb_expl_x = 0.0; self.skb_expl_y = 0.0; self.skb_expl_start_ms = 0; self.skb_expl_seed = 0
        self.skb_streak_r = [0.0] * SKB_STREAK_COUNT; self.skb_streak_ang = [0.0] * SKB_STREAK_COUNT; self.skb_streak_ready = False
        self.skb_score = 0; self.skb_hit_count = 0; self.skb_enemy_wave_left = 5; self.skb_msl_ammo = 4; self.skb_msl_regen_at = 0
        self.skb_saam_ammo = 10; self.skb_saam_regen_at = 0; self.skb_throttle = 80.0

        # ----- Lighting: PAC-MAN Arcade（v0.5 Phase 5A。CoreS3 lightRenderPacman()準拠）-----
        self.pac_ready = False
        self.pac_rng = 20260811
        self.pac_dot_eaten = [[False] * PAC_COLS for _ in range(PAC_ROWS)]
        self.pac_col = 1
        self.pac_row = 1
        self.pac_progress = 0.0
        self.pac_dir_x = 0
        self.pac_dir_y = 0
        self.pac_ghost_col = [9, 10, 11]
        self.pac_ghost_row = [6, 6, 6]
        self.pac_ghost_progress = [0.0] * PAC_GHOSTS
        self.pac_ghost_dir_x = [0] * PAC_GHOSTS
        self.pac_ghost_dir_y = [0] * PAC_GHOSTS
        self.pac_reset_at = 0

        # ----- Lighting: Fighter Duel（Phase 5B。CoreS3 lightRenderStreetFighter()準拠）-----
        self.sf_ready = False
        self.sf_x = [SF_STAGE_L + 40.0, SF_STAGE_R - 40.0]
        self.sf_facing = [1, -1]
        self.sf_state = [SF_IDLE, SF_IDLE]
        self.sf_state_until = [0, 0]
        self.sf_impact_done = [False, False]
        self.sf_health = [100.0, 100.0]
        self.sf_hit_flash = [0, 0]
        self.sf_jump_phase = [0.0, 0.0]
        self.sf_proj_active = [False, False]
        self.sf_proj_x = [0.0, 0.0]
        self.sf_proj_dir = [0, 0]
        self.sf_round = 1
        self.sf_timer = 60
        self.sf_timer_tick_at = 0
        self.sf_ko_active = False
        self.sf_ko_until = 0
        self.sf_rng = 0x2545F491
        self.sf_last_update_ms = 0  # LIGHT_FRAME_UPDATE_MSゲート用（実機の呼び出し周期を再現）

        # ----- Lighting: 8-Bit Runner（v0.5 Phase 4残り。CoreS3 lightRenderMario()準拠）-----
        self.mar_ready = False
        self.mar_rng = 0x87654321
        self.mar_scroll_x = 0.0
        self.mar_obst: list = []
        self.mar_next_spawn_world_x = 0.0
        self.mar_spawn_idx = 0
        self.mar_coins = 0
        self.mar_player_jumping = False
        self.mar_jump_start_at = 0
        self.mar_run_phase = 0.0

        # ----- Lighting: Missile Defense（Phase 5B。CoreS3 lightRenderMissile()準拠）-----
        self.msl_star_ready = False
        self.msl_cstar_x = [0] * MSL_CSTAR_COUNT
        self.msl_cstar_y = [0] * MSL_CSTAR_COUNT
        self.msl_cstar_col = [MSL_CSTAR_PALETTE_RGB[0]] * MSL_CSTAR_COUNT
        self.msl_rng = 0xC001D00D
        self.msl_enemy_active = [False] * MSL_MAX_ENEMY
        self.msl_enemy_x0 = [0.0] * MSL_MAX_ENEMY
        self.msl_enemy_y0 = [0.0] * MSL_MAX_ENEMY
        self.msl_enemy_tx = [0.0] * MSL_MAX_ENEMY
        self.msl_enemy_ty = [0.0] * MSL_MAX_ENEMY
        self.msl_enemy_x = [0.0] * MSL_MAX_ENEMY
        self.msl_enemy_y = [0.0] * MSL_MAX_ENEMY
        self.msl_enemy_progress = [0.0] * MSL_MAX_ENEMY
        self.msl_enemy_speed = [0.0] * MSL_MAX_ENEMY
        self.msl_burst_active = [False] * MSL_BURST_COUNT
        self.msl_burst_x = [0.0] * MSL_BURST_COUNT
        self.msl_burst_y = [0.0] * MSL_BURST_COUNT
        self.msl_burst_start_ms = [0] * MSL_BURST_COUNT
        self.msl_aim_state = MSL_AIM_SEARCH
        self.msl_aim_x = 160.0
        self.msl_aim_y = 90.0
        self.msl_aim_target = -1
        self.msl_reacquire_at = 0
        self.msl_shot_active = False
        self.msl_shot_x0 = 0.0
        self.msl_shot_y0 = 0.0
        self.msl_shot_x1 = 0.0
        self.msl_shot_y1 = 0.0
        self.msl_shot_start_ms = 0
        self.msl_last_update_ms = 0  # LIGHT_FRAME_UPDATE_MSゲート用（実機の呼び出し周期を再現）

        # ----- Lighting: Psychedelic / Trance（v0.5 Phase 4残り。CoreS3
        # lightRenderPsychedelic()準拠。gViz（音）は一切参照しない設計要件を維持）-----
        self.psy_pal = 0
        self.psy_layer = PSY_LAYER_MOTION
        self.psy_kind = PSY_M_RINGS
        self.psy_frames = PSY_MOTION_MIN_F
        self.psy_idx = 0
        self.psy_shot_seed = 1
        self.psy_burst_left = 0
        self.psy_last_flash = 255
        self.psy_intr_left = 0
        self.psy_intr_kind = PSY_A_STAB
        self.psy_bag: list = []
        self.psy_shots_since_face = 99
        self.psy_last_face_idx = -1
        self.psy_lcg = 1
        # Face ACCENT: Desktopには CoreS3のSD /faces PNGキャッシュ相当の機構が無いため、
        # CoreS3自身が「キャッシュ未使用時」に備えて持つベクター顔フォールバック
        # （psyAccentFaceVector）のみを常時使用する（psyFaceShotIdxは常に-1のまま）。
        # ACCENTの抽選間隔・強制発火・連続禁止・2コマ目zoomなどのロジックは変更していない。
        self.psy_face_shot_idx = -1
        self.psy_face_shot_zoom = 2.0
        self.psy_face_shot_flip = False
        self.psy_face_shot_ang = 0.0
        self.psy_face_shot_dx = 160.0
        self.psy_face_shot_dy = 144.0
        self.psy_face_shot_ov = 3
        self.psy_face_shot_step = 0
        # MOTION-A: Expanding Rings
        self.psy_ring: list = []
        self.psy_ring_bx = 0.0
        self.psy_ring_by = 0.0
        self.psy_ring_bvx = 0.0
        self.psy_ring_bvy = 0.0
        self.psy_ring_pool: list = []
        self.psy_ring_run = 0
        self.psy_ring_run_col = (255, 255, 255)
        # MOTION-B: 偏心スパイラル
        self.psy_sp_ph = 0.0
        self.psy_sp_spin = 0.0
        self.psy_sp_twist = 0.0
        self.psy_sp_reach = 180.0
        self.psy_sp_cx = 160.0
        self.psy_sp_cy = 144.0
        self.psy_sp_ell = 1.0
        self.psy_sp_arms = 6
        self.psy_sp_skip = 0
        self.psy_sp_flip_at = 8
        self.psy_sp_f = 0
        # MOTION-C: モアレ干渉
        self.psy_mo_ax = 160.0
        self.psy_mo_ay = 144.0
        self.psy_mo_bx = 0.0
        self.psy_mo_by = 144.0
        self.psy_mo_bvx = 0.0
        self.psy_mo_bvy = 0.0
        self.psy_mo_skew = 0.0
        self.psy_mo_dsk = 0.03
        self.psy_mo_na = 44.0
        self.psy_mo_nb = 40.0
        # MOTION-D: 不規則放射ウェッジ
        self.psy_wd_ph = 0.0
        self.psy_wd_spin = 0.2
        self.psy_wd_x = 160.0
        self.psy_wd_y = 144.0
        self.psy_wd_vx = 0.0
        self.psy_wd_vy = 0.0
        self.psy_wd_breathe = 0.0
        self.psy_wd_raw = [1.0] * PSY_WD_MAX
        self.psy_wd_n = 13
        # MOTION-E: 破片三角
        self.psy_tri: list = []

        # ----- Lighting: Aquarium（v0.5 Phase 4残り。CoreS3 lightRenderAquarium()準拠）-----
        self.aqu_ready = False
        self.aqu_rng = 0x9E3779B9
        self.aqu_fish_active = [False] * AQU_FISH_COUNT
        self.aqu_fish_species = [0] * AQU_FISH_COUNT
        self.aqu_fish_x = [0.0] * AQU_FISH_COUNT
        self.aqu_fish_base_y = [0.0] * AQU_FISH_COUNT
        self.aqu_fish_dir = [1] * AQU_FISH_COUNT
        self.aqu_fish_speed = [0.2] * AQU_FISH_COUNT
        self.aqu_fish_scale = [1.0] * AQU_FISH_COUNT
        self.aqu_fish_bob_phase = [0.0] * AQU_FISH_COUNT
        self.aqu_fish_bob_freq = [1.0] * AQU_FISH_COUNT
        self.aqu_fish_bob_amp = [3.0] * AQU_FISH_COUNT
        self.aqu_fish_tail_phase = [0.0] * AQU_FISH_COUNT
        self.aqu_fish_hue = [0] * AQU_FISH_COUNT
        self.aqu_fish_respawn_at = [0] * AQU_FISH_COUNT
        self.aqu_bubble_x = [0.0] * AQU_BUBBLE_COUNT
        self.aqu_bubble_y = [0.0] * AQU_BUBBLE_COUNT
        self.aqu_bubble_r = [1.0] * AQU_BUBBLE_COUNT
        self.aqu_bubble_speed = [0.3] * AQU_BUBBLE_COUNT
        self.aqu_bubble_sway = [0.0] * AQU_BUBBLE_COUNT
        self.aqu_weed_base_x = [0.0] * AQU_WEED_COUNT
        self.aqu_weed_height = [[26.0] * AQU_WEED_BLADES for _ in range(AQU_WEED_COUNT)]
        self.aqu_weed_phase = [[0.0] * AQU_WEED_BLADES for _ in range(AQU_WEED_COUNT)]
        self.aqu_weed_color = [[(24, 84, 54)] * AQU_WEED_BLADES for _ in range(AQU_WEED_COUNT)]
        self.aqu_rock_x = [0.0] * AQU_ROCK_COUNT
        self.aqu_rock_y = [236.0] * AQU_ROCK_COUNT
        self.aqu_rock_r = [14.0] * AQU_ROCK_COUNT
        self.aqu_sand_x = [0] * AQU_SAND_DOT_COUNT
        self.aqu_sand_y = [234] * AQU_SAND_DOT_COUNT
        self.aqu_prev_ms = 0

        # ----- Lighting: Flying Pompadour（v0.5 Phase 4残り。CoreS3
        # lightRenderFlyingPompadour()準拠。「Flying Toasters」へのオマージュ）-----
        self.flyp_ready = False
        self.flyp_rng = 0x51ED270B
        self.flyp_palette_rt: list = []
        self.flyp_active = [False] * FLYP_SLOT_COUNT
        self.flyp_layer = [0] * FLYP_SLOT_COUNT
        self.flyp_palette_idx = [0] * FLYP_SLOT_COUNT
        self.flyp_x = [0.0] * FLYP_SLOT_COUNT
        self.flyp_y = [0.0] * FLYP_SLOT_COUNT
        self.flyp_vx = [0.0] * FLYP_SLOT_COUNT
        self.flyp_vy = [0.0] * FLYP_SLOT_COUNT
        self.flyp_scale = [1.0] * FLYP_SLOT_COUNT
        self.flyp_wing_phase = [0.0] * FLYP_SLOT_COUNT
        self.flyp_next_spawn_at = [0] * FLYP_SLOT_COUNT
        self.flyp_star_x = [0] * FLYP_STAR_COUNT
        self.flyp_star_y = [0] * FLYP_STAR_COUNT
        self.flyp_star_next_blink_at = [0] * FLYP_STAR_COUNT
        self.flyp_star_bright = [False] * FLYP_STAR_COUNT
        self.flyp_cloud_x = [0.0] * FLYP_CLOUD_COUNT
        self.flyp_cloud_y = [0.0] * FLYP_CLOUD_COUNT
        self.flyp_cloud_r = [20.0] * FLYP_CLOUD_COUNT
        self.flyp_cloud_speed = [0.1] * FLYP_CLOUD_COUNT
        self.flyp_prev_ms = 0

        # ----- Lighting: Pixel Invasion（v0.5 Phase 5A。CoreS3 lightRenderPixelInvasion()準拠）-----
        self.pix_ready = False
        self.pix_rng = 20260810
        self.pix_alive = [[False] * PIX_COLS for _ in range(PIX_ROWS)]
        self.pix_kill_flash_until = [[0] * PIX_COLS for _ in range(PIX_ROWS)]
        self.pix_form_offset_x = 0
        self.pix_form_offset_y = 0
        self.pix_form_dir = 1
        self.pix_anim_frame = 0
        self.pix_next_step_at = 0
        self.pix_round_start_ms = 0
        self.pix_shield_row_mask = [[0] * PIX_SHIELD_ROWS for _ in range(PIX_SHIELD_COUNT)]
        self.pix_player_x = 160.0
        self.pix_player_dir = 1
        self.pix_player_next_step_at = 0
        self.pix_player_next_fire_at = 0
        self.pix_ufo_active = False
        self.pix_ufo_x = 0.0
        self.pix_ufo_dir = 1
        self.pix_ufo_next_at = 0
        self.pix_pbullet_active = [False] * PIX_PBULLET_MAX
        self.pix_pbullet_x = [0.0] * PIX_PBULLET_MAX
        self.pix_pbullet_y = [0.0] * PIX_PBULLET_MAX
        self.pix_ebullet_active = [False] * PIX_EBULLET_MAX
        self.pix_ebullet_x = [0.0] * PIX_EBULLET_MAX
        self.pix_ebullet_y = [0.0] * PIX_EBULLET_MAX
        self.pix_next_enemy_fire_at = 0
        self.pix_prev_ms = 0

        # ----- Lighting: Flower Clock（v0.5 Phase 4a）-----
        self.fc_ready = False
        self.fc_inner: list[tuple[float, float]] = []
        self.fc_outer: list[tuple[float, float]] = []

        # ----- Lighting: BASEBALL Arcade（Phase 5B。CoreS3 lightRenderBaseball()準拠）-----
        self.bb_state = BB_ST_PITCH
        self.bb_state_start_ms = 0
        self.bb_prev_ms = 0
        self.bb_ball_x = BB_PITCHER_X
        self.bb_ball_y = BB_PITCHER_Y
        self.bb_is_fly = False
        self.bb_fly_is_hr = False
        self.bb_hit_target_x = BB_HOME_X
        self.bb_hit_target_y = BB_HOME_Y
        self.bb_hit_dist = 0.0
        self.bb_chaser_idx = -1
        self.bb_chase_speed = BB_FIELDER_SPEED
        self.bb_play_is_out = False
        self.bb_hit_bases = 1
        self.bb_fielder_x = list(BB_FLD_HOME_X)
        self.bb_fielder_y = list(BB_FLD_HOME_Y)
        self.bb_on_first = False
        self.bb_on_second = False
        self.bb_on_third = False

        # Visualizer専用状態
        self.halo_env = np.zeros(48, dtype=float)
        self.halo_agc_peak = 0.0
        self.mw_agc_peak = 0.0
        self.mw_phase = 0.0
        self.rhythm_hist = np.zeros((RHY_HIST_ROWS, 8), dtype=np.float32)
        self.rhythm_prev = np.zeros(8, dtype=float)
        self.rhythm_last_step = 0
        self.kal_rot = 0.0
        self.kal_pulse = 0.0
        self.kal_bass_avg = 0.0
        self.kal_level_fast = 0.0
        self.kal_phase = [0.0, 1.3, 2.7, 4.1]
        self.kal_period = [15.0, 18.0, 21.0, 13.0]
        self.avu_needle = np.zeros(8, dtype=float)
        self.avu_level_env = 0.0
        self.tetro_pieces = []
        self.tetro_last_spawn = 0
        self.tetro_bass_avg = 0.0

        # Flash Spotlight専用状態（v0.5 Phase 2で追加）
        self.spot_x = [0] * 8
        self.spot_y = [0] * 8
        self.spot_color = [QColor(0, 0, 0) for _ in range(8)]
        self.spot_last_switch_ms = 0
        self.spot_ready = False

        now = self._now_ms()
        self.next_nose_at = now + random.randrange(*NOSE_INTERVAL_MS)
        self.next_blink_check_at = now + random.randrange(*BLINK_INTERVAL_MS)
        self.next_mouth_at = now + MOUTH_PAKU_MS
        self.blink_events: list[tuple[int, str]] = []

        self.setFixedSize(CANVAS_W * scale, CANVAS_H * scale)
        self.setAttribute(Qt.WA_OpaquePaintEvent, True)

        self.timer = QTimer(self)
        self.timer.setInterval(20)
        self.timer.timeout.connect(self._tick)
        self.timer.start()

    @staticmethod
    def _now_ms() -> int:
        return int(time.monotonic() * 1000)

    # ----- Character（v0.5 Phase 2）-----
    def cycle_character(self, direction: int = 1) -> None:
        """Characterを切替。direction=+1で次、-1で前。"""
        step = 1 if direction >= 0 else -1
        self.character_mode = (self.character_mode + step) % len(CHARACTER_MODES)
        self.update()

    # ----- Visualizer選択・Random（v0.5 Phase 2）-----
    def visualizer_random_on(self) -> bool:
        return self.viz_manual_index == len(VIS_MODES)

    def cycle_visualizer(self, direction: int = 1) -> None:
        """Visualizerを切替。direction=+1で次、-1で前。Randomも循環に含む。"""
        step = 1 if direction >= 0 else -1
        self.viz_manual_index = (self.viz_manual_index + step) % (len(VIS_MODES) + 1)
        if self.visualizer_random_on():
            # Randomへ入った直後は次のtickで即座に1回選ぶ（CoreS3のRandom ON直後と同じ挙動）。
            self.viz_random_last_switch_ms = 0
        else:
            self.viz_display_mode = self.viz_manual_index
        self.update()

    def _update_visualizer_random(self, now: int) -> None:
        if not self.visualizer_random_on():
            return
        if self.viz_random_last_switch_ms != 0 and (now - self.viz_random_last_switch_ms) < RANDOM_INTERVAL_MS:
            return
        # Faceを候補に含め、Random自身は候補に含まれない（VIS_MODESに"Random"は無いため）。
        pick = pick_random_avoiding(len(VIS_MODES), self.viz_random_last_pick)
        self.viz_display_mode = pick
        self.viz_random_last_pick = pick
        self.viz_random_last_switch_ms = now

    # ----- Lighting選択・Random（v0.5 Phase 2。実描画はPhase 3以降）-----
    def lighting_random_on(self) -> bool:
        return self.light_manual_index == len(LIGHT_MODES)

    def cycle_lighting(self, direction: int = 1) -> None:
        """Lightingを切替。direction=+1で次、-1で前。Randomも循環に含む。"""
        step = 1 if direction >= 0 else -1
        self.light_manual_index = (self.light_manual_index + step) % (len(LIGHT_MODES) + 1)
        if self.lighting_random_on():
            self.light_random_last_switch_ms = 0
        else:
            self.light_display_mode = self.light_manual_index
        self.update()

    def _update_lighting_random(self, now: int) -> None:
        if not self.lighting_random_on():
            return
        if self.light_random_last_switch_ms != 0 and (now - self.light_random_last_switch_ms) < RANDOM_INTERVAL_MS:
            return
        # Noneを候補に含め（LIGHT_MODES[0]）、Random自身は候補に含まれない。
        pick = pick_random_avoiding(len(LIGHT_MODES), self.light_random_last_pick)
        self.light_display_mode = pick
        self.light_random_last_pick = pick
        self.light_random_last_switch_ms = now

    def _tick(self) -> None:
        now = self._now_ms()
        speaking, _rms, error, _fft = self.audio_state.snapshot()

        # 音声状態に合わせて実機同様、発話中だけ140ms周期で口を開閉する。
        if speaking != self.was_speaking:
            self.was_speaking = speaking
            if speaking:
                self.mouth_open = False
                self.next_mouth_at = now
            else:
                self.mouth_open = False

        if speaking and now >= self.next_mouth_at:
            self.next_mouth_at = now + MOUTH_PAKU_MS
            self.mouth_open = not self.mouth_open
            if self.mouth_open:
                self._randomize_talk_mouth()

        # 鼻ヒクヒク: 実機同様 ±1px を120〜280msで交互に動かす。
        if now >= self.next_nose_at:
            self.nose_offset = -1 if self.nose_offset >= 0 else 1
            self.next_nose_at = now + random.randrange(*NOSE_INTERVAL_MS)

        # 瞬き: 3〜10秒ごとの判定で45%。8%で二重瞬き。
        if now >= self.next_blink_check_at:
            self.next_blink_check_at = now + random.randrange(*BLINK_INTERVAL_MS)
            if random.random() < BLINK_PROBABILITY:
                self._schedule_blink(now)

        while self.blink_events and now >= self.blink_events[0][0]:
            _, mode = self.blink_events.pop(0)
            self.eye_mode = mode

        # Visualizer animation state
        fft01 = np.clip(_fft / 100.0, 0.0, 1.0)
        level = float(np.mean(fft01))
        bass = float(np.mean(fft01[:2]))

        # Mirror color phase
        self.mw_phase = (self.mw_phase + 0.02 + level * 0.30) % 6.0

        # 2026-08-10 latest Kaleidoscope: bass pulse + very slow rotation.
        self.kal_bass_avg += (bass - self.kal_bass_avg) * 0.15
        if bass > self.kal_bass_avg * 1.3 + 0.04:
            self.kal_pulse = 1.0
        self.kal_pulse *= 0.65
        self.kal_level_fast += (level - self.kal_level_fast) * (0.50 if level > self.kal_level_fast else 0.18)
        self.kal_rot = (self.kal_rot + 0.004 * (20.0 / 70.0)) % (2.0 * math.pi)

        # 2026-08-10 latest Analog VU: absolute + relative spectrum contribution.
        gained = np.clip(fft01 * np.asarray(EQ_GAIN8), 0.0, 1.5)
        mean = float(np.mean(gained))
        self.avu_level_env += (level - self.avu_level_env) * (0.50 if level > self.avu_level_env else 0.18)
        gate = float(np.clip((self.avu_level_env - 0.012) / 0.040, 0.0, 1.0))
        rel_den = max(mean, 0.060)
        for i in range(8):
            target = 0.0
            if gained[i] >= 0.020:
                a_abs = min(1.0, float(gained[i]) / 1.30)
                ratio = float(gained[i]) / rel_den
                a_rel = float(np.clip((ratio - 0.35) / (1.90 - 0.35), 0.0, 1.0))
                target = min(1.0, gate * (0.55 * a_abs + 0.60 * a_rel))
            coef = 0.60 if target > self.avu_needle[i] else 0.14
            self.avu_needle[i] += (target - self.avu_needle[i]) * coef

        self._update_rhythm(now, _fft)
        self._update_tetromino(now, fft01, level, bass)

        # v0.5 Phase 2: Visualizer Random / Lighting Random（各3分・完全に独立）。
        self._update_visualizer_random(now)
        self._update_lighting_random(now)

        self.update()

    @staticmethod
    def _interp_band(fft_levels, p: float) -> float:
        """8バンドを0..1位置へ線形補間し、0..1で返す。"""
        a = np.clip(np.asarray(fft_levels, dtype=float) / 100.0, 0.0, 1.0)
        x = np.clip(p, 0.0, 1.0) * 7.0
        i = int(math.floor(x))
        j = min(7, i + 1)
        t = x - i
        return float(a[i] * (1.0 - t) + a[j] * t)

    @staticmethod
    def _tint(color: QColor, white_pct: float) -> QColor:
        t = max(0.0, min(1.0, white_pct / 100.0))
        return QColor(
            int(color.red()   * (1-t) + 255*t),
            int(color.green() * (1-t) + 255*t),
            int(color.blue()  * (1-t) + 255*t),
        )

    def _update_rhythm(self, now: int, fft_levels) -> None:
        # CoreS3は40ms。GUIタイマー20msなので40msごとに1行進める。
        if now - self.rhythm_last_step < 40:
            return
        self.rhythm_last_step = now
        self.rhythm_hist[1:] = self.rhythm_hist[:-1]
        self.rhythm_hist[0] = 0
        vals = np.clip(np.asarray(fft_levels, dtype=float) * np.asarray(EQ_GAIN8), 0, 100)
        rise = vals - self.rhythm_prev
        for i in range(8):
            if vals[i] >= 12 and rise[i] >= 10:
                self.rhythm_hist[0, i] = vals[i]
        self.rhythm_prev = vals

    def _spawn_tetromino(self, color: QColor, now: int) -> None:
        if len(self.tetro_pieces) >= 4:
            return
        shape = random.choice(("O", "I", "L"))
        self.tetro_pieces.append({
            "shape": shape,
            "cx": float(random.randint(64, 256)),
            "cy": float(SCENE_TOP - 64),
            "vy": 0.6,
            "angle": 0.0,
            "target": 0.0,
            "color": color,
            "next_rot": now + random.randint(900, 1700),
            "slide_to": None,
            "slide_start": 0,
            "slide_dur": 0,
            "next_slide": now + random.randint(1200, 2200),
        })

    def _update_tetromino(self, now: int, fft01, level: float, bass: float) -> None:
        # band attackで出現。無音でも2.5秒に1個を保証。
        vals = np.clip(fft01 * np.asarray(EQ_GAIN8) * 100.0, 0, 100)
        if not hasattr(self, "_tetro_prev"):
            self._tetro_prev = np.zeros(8)
        for i in range(8):
            if vals[i] >= 15 and vals[i] - self._tetro_prev[i] >= 12 and now - self.tetro_last_spawn >= 220:
                self._spawn_tetromino(SPECTRUM_COLORS[i], now)
                self.tetro_last_spawn = now
                break
        self._tetro_prev = vals
        if not self.tetro_pieces and now - self.tetro_last_spawn >= 2500:
            self._spawn_tetromino(SPECTRUM_COLORS[4], now)
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
                p["target"] = p["angle"] + math.pi / 2
                p["next_rot"] = now + random.randint(900, 1700)

            if p["slide_to"] is not None:
                t = min(1.0, (now - p["slide_start"]) / max(1, p["slide_dur"]))
                e = t*t*(3-2*t)
                p["cx"] = p["slide_from"] + (p["slide_to"] - p["slide_from"]) * e
                if t >= 1.0:
                    p["slide_to"] = None
            elif now >= p["next_slide"]:
                if random.random() < 0.5:
                    target = p["cx"] + random.choice((-1,1)) * random.choice((40,80))
                    target = max(64.0, min(256.0, target))
                    if abs(target - p["cx"]) > 4:
                        p["slide_from"] = p["cx"]
                        p["slide_to"] = target
                        p["slide_start"] = now
                        p["slide_dur"] = random.randint(300,500)
                p["next_slide"] = now + random.randint(1200,2200)

            if p["cy"] - 64 > CANVAS_H:
                self.tetro_pieces.remove(p)

    def _schedule_blink(self, now: int) -> None:
        self.blink_events.append((now, "blink"))
        self.blink_events.append((now + BLINK_CLOSED_MS, "open"))

        if random.random() < DOUBLE_BLINK_PROBABILITY:
            second_close = now + BLINK_CLOSED_MS + DOUBLE_BLINK_GAP_MS
            self.blink_events.append((second_close, "blink"))
            self.blink_events.append((second_close + BLINK_CLOSED_MS, "open"))

        self.blink_events.sort(key=lambda item: item[0])

    def _randomize_talk_mouth(self) -> None:
        # ino: random(-1, 2) * 2 -> -2 / 0 / +2
        self.mouth_mx = NOSE_X + random.choice((-2, 0, 2))
        self.mouth_my = NOSE_Y + 26 + random.choice((-2, 0, 2))
        self.mouth_mw = 18 + random.choice((-2, 0, 2))
        self.mouth_mh = 12 + random.choice((-2, 0, 2))

    @staticmethod
    def _draw_thick_line(
        painter: QPainter,
        x0: int,
        y0: int,
        x1: int,
        y1: int,
        thickness: int,
        color: QColor,
    ) -> None:
        pen = QPen(color)
        pen.setWidth(thickness)
        pen.setCapStyle(Qt.RoundCap)
        painter.setPen(pen)
        painter.drawLine(x0, y0, x1, y1)

    @staticmethod
    def _fill_ellipse(painter: QPainter, cx: int, cy: int, rx: int, ry: int, color: QColor) -> None:
        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(color))
        painter.drawEllipse(cx - rx, cy - ry, rx * 2, ry * 2)

    @staticmethod
    def _fill_circle(painter: QPainter, cx: int, cy: int, radius: int, color: QColor) -> None:
        KariPomDesktopCanvas._fill_ellipse(painter, cx, cy, radius, radius, color)

    @staticmethod
    def _rgb_scale(rgb, b: int):
        """CoreS3 light565Scale()のRGB888版。bは明るさ0..255（そのまま各chへ同率適用）。"""
        if b >= 255:
            return rgb
        if b <= 0:
            return (0, 0, 0)
        return (rgb[0] * b // 255, rgb[1] * b // 255, rgb[2] * b // 255)

    @staticmethod
    def _rgb_lerp(a, c, t: int):
        """CoreS3 light565Lerp()のRGB888版。t:0..255でa→cへ線形補間。"""
        if t <= 0:
            return a
        if t >= 255:
            return c
        return (
            a[0] + (c[0] - a[0]) * t // 255,
            a[1] + (c[1] - a[1]) * t // 255,
            a[2] + (c[2] - a[2]) * t // 255,
        )

    @staticmethod
    def _fill_circle_alpha(painter: QPainter, cx: float, cy: float, radius: float, color: QColor, alpha: int) -> None:
        """半透明円を塗る。QPainterは標準でアルファ合成付きdrawEllipse()を持つため、
        CoreS3側のような走査線ごとの手動合成（fillRectAlpha連打）は不要。"""
        if radius <= 0:
            return
        c = QColor(color)
        c.setAlpha(alpha)
        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(c))
        painter.drawEllipse(QPointF(cx, cy), radius, radius)

    @staticmethod
    def _draw_text_centered(painter: QPainter, text: str, cx: int, cy: int, pt: int, color: QColor) -> None:
        """文字列を(cx,cy)中心・太字で描く（CoreS3のMC_DATUM＋1px3回重ね描き太字化に相当。
        QPainterはQFont.setBold(True)で標準的に太字化できるため、3回重ね描きの代わりに
        これを使う＝見た目の意図（太字）は同じまま実装をQt標準機能に置き換えている）。"""
        font = QFont()
        font.setBold(True)
        font.setPointSize(pt)
        painter.setFont(font)
        painter.setPen(QPen(color))
        fm = QFontMetrics(font)
        rect = fm.boundingRect(text)
        x = cx - rect.width() // 2 - rect.left()
        y = cy - rect.height() // 2 - rect.top()
        painter.drawText(x, y, text)

    def _draw_eyelashes(self, painter: QPainter) -> None:
        """Ms. KariPom専用のまつ毛。CoreS3 drawEyelashes()準拠：開眼時のみ、左右の目
        それぞれに角度25°/45°/65°・太さ4px・長さ9pxの直線を目の外周から放射状に描く。"""
        black = QColor("black")
        lash_len = 9
        lash_thick = 4
        eye_radius = 20
        left_cx, right_cx, cy = 90, 230, 90
        for deg in (25.0, 45.0, 65.0):
            rad = math.radians(deg)
            dx, dy = math.cos(rad), math.sin(rad)
            l_root = (left_cx - eye_radius * dx, cy - eye_radius * dy)
            l_tip = (left_cx - (eye_radius + lash_len) * dx, cy - (eye_radius + lash_len) * dy)
            self._draw_thick_line(painter, int(l_root[0]), int(l_root[1]), int(l_tip[0]), int(l_tip[1]), lash_thick, black)
            r_root = (right_cx + eye_radius * dx, cy - eye_radius * dy)
            r_tip = (right_cx + (eye_radius + lash_len) * dx, cy - (eye_radius + lash_len) * dy)
            self._draw_thick_line(painter, int(r_root[0]), int(r_root[1]), int(r_tip[0]), int(r_tip[1]), lash_thick, black)

    def _draw_face(self, painter: QPainter) -> None:
        """顔レイヤーの入口。CoreS3 sceneComposeAndPush()の「③.5 Eye Slotフック→④顔レイヤー」
        の2ステップをこの1メソッドにまとめている。Eye Slotが選択中は、Visualizer本体の
        描画で消えた可能性のあるリール窓を（Character=Noneでも）ここで必ず描き直す
        （CoreS3の「リールは顔レイヤーではなくLighting Layer0の描画物」という位置付けを
        踏襲：目なしはCharacter=Noneの対象だが、リールはその対象外にする）。同一フレーム内で
        Lighting背景層が既にリールを描いていた場合も、状態を進めない読み出し専用の再描画
        なので二重描画による見た目の変化は無い。"""
        if self.eslot_active:
            self._eslot_fill_windows_white(painter)
            self._eslot_draw_reels_frame(painter, self._now_ms())
        self._draw_face_parts(painter)

    def _draw_face_parts(self, painter: QPainter) -> None:
        # v0.5 Phase 2: Character=None（CoreS3のfaceDrawingEnabled()相当）の場合は
        # 共通の顔レイヤー（目・鼻・口）を一切描かない。Lighting/Visualizer側の
        # 固有オブジェクト（Eye Slotのリール等）はこのガードの対象外とし、CoreS3と
        # 同じく_draw_face()側で別途描画する。
        if self.character_mode == CHAR_NONE:
            return

        black = QColor("black")
        red = QColor("red")
        white = QColor("white")
        # v0.5 Phase 3: CoreS3 drawVisualizerFaceParts()のgLightingActive分岐に準拠。
        # Lighting選択中（色付き/暗色の背景になる）は、黒/赤の本体を描く前に白い縁取り
        # （バッキング）を敷いて色付き床の上でも顔が読めるようにする。Lighting未選択
        # （白背景のVisualizer/Face）のときは従来どおり縁取りなし（見た目は1バイトも
        # 変えない）。口の楕円だけは実機同様バッキングを+1に縮小（他は+2）。バッキングの
        # 中心はジッター中の口位置(self.mouth_mx/my)へ本体と同じ値を使い、ズレを防ぐ。
        lighting_active = self.light_display_mode != LIGHT_NONE
        # v0.5 Phase 4a: CoreS3 drawVisualizerFaceParts()のgEyeSlotActiveガードに準拠。
        # Eye Slot選択中は黒目の位置に既にリール（スロット絵柄）を描いているため、
        # ここでの黒目描画（開眼・まばたきどちらも）だけをスキップする。まつ毛・鼻・口は
        # 通常どおり描く（CoreS3と同じくリール以外の顔パーツは変えない）。
        if self.eslot_active:
            if self.character_mode == CHAR_MISS_KARIPOM and self.eye_mode != "blink":
                self._draw_eyelashes(painter)
        elif self.eye_mode == "blink":
            if lighting_active:
                self._draw_thick_line(painter, 72, 90, 108, 90, 6 + SLEEP_OUTLINE_PX, white)
                self._draw_thick_line(painter, 212, 90, 248, 90, 6 + SLEEP_OUTLINE_PX, white)
            self._draw_thick_line(painter, 72, 90, 108, 90, 6, black)
            self._draw_thick_line(painter, 212, 90, 248, 90, 6, black)
        else:
            if lighting_active:
                self._fill_circle(painter, 90, 90, 22, white)
                self._fill_circle(painter, 230, 90, 22, white)
            self._fill_circle(painter, 90, 90, 20, black)
            self._fill_circle(painter, 230, 90, 20, black)
            if self.character_mode == CHAR_MISS_KARIPOM:
                self._draw_eyelashes(painter)

        if lighting_active:
            self._fill_ellipse(painter, NOSE_X, NOSE_Y + self.nose_offset, 20, 14, white)
            if self.mouth_open and self.was_speaking:
                self._draw_thick_line(painter, NOSE_X, NOSE_Y + self.nose_offset + 8, NOSE_X, NOSE_Y + 25, 6 + SLEEP_OUTLINE_PX, white)
                self._fill_ellipse(painter, self.mouth_mx, self.mouth_my, self.mouth_mw + 1, self.mouth_mh + 1, white)
            else:
                self._draw_thick_line(painter, NOSE_X, NOSE_Y + self.nose_offset + 8, NOSE_X, NOSE_Y + 22, 6 + SLEEP_OUTLINE_PX, white)
                self._draw_thick_line(painter, NOSE_X, NOSE_Y + 22, NOSE_X - 20, NOSE_Y + 32, 6 + SLEEP_OUTLINE_PX, white)
                self._draw_thick_line(painter, NOSE_X, NOSE_Y + 22, NOSE_X + 20, NOSE_Y + 32, 6 + SLEEP_OUTLINE_PX, white)

        self._fill_ellipse(painter, NOSE_X, NOSE_Y + self.nose_offset, 18, 12, black)
        if self.mouth_open and self.was_speaking:
            self._draw_thick_line(painter, NOSE_X, NOSE_Y + self.nose_offset + 8, NOSE_X, NOSE_Y + 25, 6, black)
            self._fill_ellipse(painter, self.mouth_mx, self.mouth_my, self.mouth_mw, self.mouth_mh, red)
        else:
            self._draw_thick_line(painter, NOSE_X, NOSE_Y + self.nose_offset + 8, NOSE_X, NOSE_Y + 22, 6, black)
            self._draw_thick_line(painter, NOSE_X, NOSE_Y + 22, NOSE_X - 20, NOSE_Y + 32, 6, black)
            self._draw_thick_line(painter, NOSE_X, NOSE_Y + 22, NOSE_X + 20, NOSE_Y + 32, 6, black)

    def _draw_eq(self, painter: QPainter, fft_levels) -> None:
        painter.setPen(Qt.NoPen)
        for i in range(8):
            disp = max(0, min(100, int(round(float(fft_levels[i]) * EQ_GAIN8[i]))))
            h = disp * EQ_BAR_MAX_H // 100
            if h > 0:
                painter.setBrush(QBrush(SPECTRUM_COLORS[i]))
                painter.drawRect(i * EQ_COL_W + EQ_BAR_MX, EQ_BAR_BOTTOM_Y - h, EQ_BAR_W, h)
        # 顔を最後に描いて最前面へ。
        self._draw_face(painter)

    def _draw_halo(self, painter: QPainter, fft_levels) -> None:
        # 2026-08-10 latest CoreS3/Companion look: true circle, not flattened ellipse.
        vals = np.clip(np.asarray(fft_levels, dtype=float) / 100.0, 0.0, 1.0)
        peak = float(vals.max())
        self.halo_agc_peak += (peak - self.halo_agc_peak) * (1.0 if peak > self.halo_agc_peak else 0.03)
        ref = max(0.21, self.halo_agc_peak)
        agc = 0.64 / ref
        cx, cy = 160.0, 143.0
        rin, rout = 62.0, 93.0
        for j in range(48):
            rel = j / 47.0
            fold = min(rel * 2.0, 2.0 - rel * 2.0)
            v = min(1.0, self._interp_band(fft_levels, fold) * agc)
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
                painter.drawLine(int(cx + dx * rin), int(cy + dy * rin), int(cx + dx * rr), int(cy + dy * rr))
        self._draw_face(painter)

    @staticmethod
    def _spectrum_color(p: float) -> QColor:
        p = max(0.0, min(1.0, p)) * 7.0
        i = int(math.floor(p))
        j = min(7, i+1)
        t = p - i
        a, b = SPECTRUM_COLORS[i], SPECTRUM_COLORS[j]
        return QColor(
            int(a.red()*(1-t)+b.red()*t),
            int(a.green()*(1-t)+b.green()*t),
            int(a.blue()*(1-t)+b.blue()*t),
        )

    def _draw_mirror(self, painter: QPainter, fft_levels) -> None:
        vals = np.asarray(fft_levels, dtype=float)/100.0
        peak = float(vals.max())
        self.mw_agc_peak += (peak-self.mw_agc_peak)*(1.0 if peak>self.mw_agc_peak else 0.03)
        agc = 0.80 / max(0.20, self.mw_agc_peak)
        max_half = (MW_YMAX-MW_YMIN)//2
        for c in range(81):
            x = min(319, c*MW_SX)
            p = x/319.0
            v = min(1.0, self._interp_band(fft_levels,p)*agc)
            h = max(2, int((v**0.45)*max_half))
            top, bot = max(MW_YMIN,MW_CY-h), min(MW_YMAX,MW_CY+h)
            hue = MW_PAL[int((c*0.10+self.mw_phase)) % len(MW_PAL)]
            body = self._tint(hue,18)
            edge = self._tint(hue,62)
            painter.setPen(Qt.NoPen)
            if bot-top > 2*MW_EDGE:
                painter.setBrush(QBrush(body)); painter.drawRect(x,top+MW_EDGE,MW_SX,bot-top-2*MW_EDGE)
            painter.setBrush(QBrush(edge)); painter.drawRect(x,top,MW_SX,MW_EDGE); painter.drawRect(x,bot-MW_EDGE,MW_SX,MW_EDGE)
        self._draw_face(painter)

    def _draw_rhythm(self, painter: QPainter) -> None:
        painter.setPen(Qt.NoPen)
        for r in range(RHY_HIST_ROWS):
            if (r % 4) >= 3:
                continue
            y = SCENE_TOP + r*RHY_ROW_H
            for i in range(8):
                disp = float(self.rhythm_hist[r,i])
                if disp <= 0:
                    continue
                tint = max(0,min(88,100-disp))
                col = self._tint(SPECTRUM_COLORS[i],tint)
                painter.setBrush(QBrush(col))
                painter.drawRect(i*40+3,y,34,16)
        # 顔を最後に描いて最前面へ。
        self._draw_face(painter)

    def _draw_kaleido(self, painter: QPainter, fft_levels, now_sec: float) -> None:
        # 2026-08-10 latest compact Kaleidoscope: small, white, slow, audio-led.
        # v0.5 Phase 3.1: CoreS3 vizRenderKaleido()の`if (!gLightingActive) fillScreen(WHITE)`
        # と同じく、Lighting選択中はこの白背景で消さない（Lightingが透けて見えるようにする）。
        if self.light_display_mode == LIGHT_NONE:
            painter.fillRect(0, SCENE_TOP, CANVAS_W, CANVAS_H - SCENE_TOP, QColor("white"))
        painter.save()
        painter.setClipRect(0, SCENE_TOP, CANVAS_W, CANVAS_H - SCENE_TOP)
        vals = np.clip(np.asarray(fft_levels, dtype=float) / 100.0, 0.0, 1.0)
        gained = vals * np.asarray(EQ_GAIN8)
        mean = float(np.mean(gained))
        gate = float(np.clip((self.kal_level_fast - 0.015) / 0.05, 0.0, 1.0))
        loud_trim = 0.55 + 0.45 * min(1.0, self.kal_level_fast / 0.35)
        amp = gate * loud_trim
        pulse = 1.0 + 0.10 * self.kal_pulse
        gaps_min = (20.0, 24.0, 28.0, 34.0)
        gaps_max = (26.0, 30.0, 36.0, 44.0)
        cx, cy = 160.0, 144.0
        r_prev = 0.0

        def rel_shape(v: float) -> float:
            denom = max(mean, 0.05)
            ratio = max(-2.5, min(2.5, (float(v) - mean) / denom))
            mag = math.sqrt(abs(ratio) * 1.3)
            return max(-1.3, min(1.3, mag if ratio >= 0 else -mag))

        for ring in range(4):
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
            in0 = (r_in, 0.0)
            in1 = (r_in * math.cos(math.pi / 6), r_in * math.sin(math.pi / 6))
            out0 = (r_out, 0.0)
            out1 = (r_out * math.cos(math.pi / 6), r_out * math.sin(math.pi / 6))
            fp = (r_f * math.cos(ang), r_f * math.sin(ang))
            audio_avg = (abs(shape_r) + abs(shape_a)) * 0.5 * amp
            hue_spread = 1.0 + audio_avg * 0.9
            white_boost = min(92.0, 8.0 + audio_avg * 42.0 + self.kal_pulse * 55.0)
            colors = [self._tint(self._spectrum_color((ring * 0.22 + ofs * hue_spread) % 1.0), white_boost) for ofs in (0.00, 0.05, 0.10, 0.15)]
            tris = ((in0, in1, fp), (in1, out1, fp), (out1, out0, fp), (out0, in0, fp))
            for sec in range(6):
                sec_a = sec * math.pi / 3.0
                cs, ss = math.cos(sec_a), math.sin(sec_a)
                cr, sr = math.cos(self.kal_rot), math.sin(self.kal_rot)
                for mirrored in (False, True):
                    for tri, col in zip(tris, colors):
                        pts = []
                        for x, y in tri:
                            if mirrored:
                                y = -y
                            tx, ty = x * cs - y * ss, x * ss + y * cs
                            fx, fy = tx * cr - ty * sr, tx * sr + ty * cr
                            pts.append((cx + fx, cy + fy))
                        self._poly(painter, pts, col)
        painter.restore()
        self._draw_face(painter)

    @staticmethod
    def _qp(x, y):
        from PyQt5.QtCore import QPoint
        return QPoint(int(round(x)), int(round(y)))

    @classmethod
    def _poly(cls, painter: QPainter, points, color: QColor) -> None:
        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(color))
        painter.drawPolygon(*[cls._qp(x, y) for x, y in points])

    def _draw_face_with_rim(self,painter):
        # 白で一回太め→通常黒/赤を重ねる簡易リム
        white=QColor("white")
        if self.eye_mode=="blink":
            self._draw_thick_line(painter,72,90,108,90,10,white); self._draw_thick_line(painter,212,90,248,90,10,white)
        else:
            self._fill_circle(painter,90,90,24,white); self._fill_circle(painter,230,90,24,white)
        self._fill_ellipse(painter,NOSE_X,NOSE_Y+self.nose_offset,22,16,white)
        self._draw_face(painter)

    def _draw_analog_vu(self, painter: QPainter) -> None:
        # 2026-08-10 latest arch-top meter faces.
        # v0.5 Phase 3.1: CoreS3 vizRenderAnalogVu()の`if (!gLightingActive) fillScreen(WHITE)`
        # と同じく、Lighting選択中はこの白背景で消さない（Lightingが透けて見えるようにする）。
        if self.light_display_mode == LIGHT_NONE:
            painter.fillRect(0, SCENE_TOP, CANVAS_W, CANVAS_H - SCENE_TOP, QColor("white"))
        cream = QColor(245, 230, 200)
        frame = QColor(33, 36, 36)
        bezel = QColor(82, 82, 82)
        red_zone = QColor(192, 0, 0)
        needle = QColor(248, 0, 0)
        labels = ("L1", "L2", "M1", "M2", "M3", "M4", "H1", "H2")
        for b in range(8):
            col, row = b % 4, b // 4
            cx0, cy0 = col * 80, SCENE_TOP + row * 96
            px, py = cx0 + 3, cy0 + 8
            vx, vy = cx0 + 40, cy0 + 74
            panel_w, panel_h = 74, 76
            face_r = 44
            panel_bottom = py + panel_h - 1
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
            def arch_y(sx: int, radius: int) -> int:
                rr = max(0, radius * radius - (sx - vx) ** 2)
                return vy - int(round(math.sqrt(rr)))
            right_x = px + panel_w - 1
            painter.setPen(QPen(frame, 1))
            painter.drawLine(px, arch_y(px, face_r), px, panel_bottom)
            painter.drawLine(right_x, arch_y(right_x, face_r), right_x, panel_bottom)
            painter.drawLine(px, panel_bottom, right_x, panel_bottom)
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
            arc_pts = []
            for k in range(13):
                deg = -55.0 + 110.0 * k / 12.0
                a = math.radians(deg)
                arc_pts.append((vx + math.sin(a) * 42, vy - math.cos(a) * 42))
            painter.setPen(QPen(frame, 1))
            for p0, p1 in zip(arc_pts[:-1], arc_pts[1:]):
                painter.drawLine(int(p0[0]), int(p0[1]), int(p1[0]), int(p1[1]))
            painter.setPen(QPen(red_zone, 2))
            for k in range(16):
                t = 0.75 + 0.25 * k / 15.0
                a = math.radians(-55.0 + 110.0 * t)
                painter.drawLine(int(vx + math.sin(a) * 38), int(vy - math.cos(a) * 38), int(vx + math.sin(a) * 42), int(vy - math.cos(a) * 42))
            for k in range(7):
                t = k / 6.0
                a = math.radians(-55.0 + 110.0 * t)
                painter.setPen(QPen(red_zone if t >= 0.75 else frame, 1))
                painter.drawLine(int(vx + math.sin(a) * 34), int(vy - math.cos(a) * 34), int(vx + math.sin(a) * 42), int(vy - math.cos(a) * 42))
            painter.setPen(QPen(frame, 1))
            painter.drawText(cx0 + 17, cy0 + 80, labels[b])
            painter.drawText(cx0 + 53, cy0 + 80, "VU")
            a = math.radians(-55.0 + 110.0 * float(self.avu_needle[b]))
            sx, sy = math.sin(a), -math.cos(a)
            pxp, pyp = math.cos(a), math.sin(a)
            tip = (vx + sx * 38.0, vy + sy * 38.0)
            b1 = (vx + pxp, vy + pyp)
            b2 = (vx - pxp, vy - pyp)
            self._poly(painter, (b1, b2, tip), needle)
            self._fill_circle(painter, vx, vy, 3, frame)
            self._fill_circle(painter, vx, vy, 1, bezel)
        self._draw_face(painter)

    def _draw_tetromino(self, painter: QPainter) -> None:
        painter.save(); painter.setClipRect(0,SCENE_TOP,CANVAS_W,CANVAS_H-SCENE_TOP)
        shapes={"O":[(0,0),(1,0),(0,1),(1,1)],"I":[(0,0),(1,0),(2,0)],"L":[(0,0),(1,0),(0,1)]}
        for p in self.tetro_pieces:
            cells=shapes[p["shape"]]
            w=3 if p["shape"]=="I" else 2; h=1 if p["shape"]=="I" else 2
            ca,sa=math.cos(p["angle"]),math.sin(p["angle"])
            for col,row in cells:
                lcx=(col-(w-1)/2)*40; lcy=(row-(h-1)/2)*40
                half=19
                pts=[]
                for lx,ly in ((lcx-half,lcy-half),(lcx+half,lcy-half),(lcx+half,lcy+half),(lcx-half,lcy+half)):
                    rx=lx*ca-ly*sa; ry=lx*sa+ly*ca
                    pts.append(self._qp(p["cx"]+rx,p["cy"]+ry))
                painter.setPen(Qt.NoPen); painter.setBrush(QBrush(p["color"])); painter.drawPolygon(*pts)
        painter.restore()
        self._draw_face(painter)

    # ----- Flash Spotlight（v0.5 Phase 2で追加。CoreS3 vizRenderFlashSpotlight()準拠）-----
    def _spot_reshuffle(self) -> None:
        for i in range(8):
            self.spot_x[i] = random.randint(0, CANVAS_W - 1)
            self.spot_y[i] = random.randint(SCENE_TOP, CANVAS_H - 1)
            self.spot_color[i] = QColor(*SPOT_PALETTE_RGB[random.randrange(len(SPOT_PALETTE_RGB))])

    def _update_flash_spotlight(self, now: int) -> None:
        # 位置・色はSPOT_INTERVAL_MS(約120ms)ごとに一括再抽選（円自体は連続移動しない）。
        # CoreS3同様、この状態更新はFlash Spotlight選択中のみ行う。
        if not self.spot_ready or (now - self.spot_last_switch_ms) >= SPOT_INTERVAL_MS:
            self._spot_reshuffle()
            self.spot_last_switch_ms = now
            self.spot_ready = True

    def _draw_flash_spotlight(self, painter: QPainter, fft01) -> None:
        # 背景は一切塗らない（Lighting併用時はLightingが、無ければ白背景がそのまま透ける）。
        gained = np.clip(fft01 * np.asarray(EQ_GAIN8), 0.0, 1.5)
        mean = float(np.mean(gained))
        rel_den = max(mean, 0.060)
        circles = []
        for i in range(8):
            if gained[i] < SPOT_BAND_FLOOR:
                continue  # 無音バンドはそのフレーム非表示（0〜8個で変動）
            a_abs = min(1.0, float(gained[i]) / SPOT_ABS_FULL)
            ratio = float(gained[i]) / rel_den
            a_rel = float(np.clip((ratio - SPOT_REL_LO) / (SPOT_REL_HI - SPOT_REL_LO), 0.0, 1.0))
            amount = min(1.0, SPOT_W_ABS * a_abs + SPOT_W_REL * a_rel)
            radius = SPOT_R_MIN + (SPOT_R_MAX - SPOT_R_MIN) * amount
            circles.append((radius, self.spot_x[i], self.spot_y[i], self.spot_color[i]))
        # 大きい円を先に描き、後から描かれる半透明円が既に合成済みの結果へブレンドされる
        # 通常の重なり方にする（CoreS3と同じ重なり対策）。
        circles.sort(key=lambda c: c[0], reverse=True)
        for radius, cx, cy, color in circles:
            self._fill_circle_alpha(painter, cx, cy, radius, color, SPOT_ALPHA)
        self._draw_face(painter)

    # ----- Lighting #1: Disco Floor（v0.5 Phase 3。CoreS3 lightRenderDisco()準拠）-----
    def _disco_rand(self) -> int:
        self.disco_rng = (self.disco_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.disco_rng

    def _build_disco_table(self) -> None:
        def hits(rx, ry, rw, rh, tx, ty):
            return not (rx >= tx + DISCO_TW or rx + rw <= tx or ry >= ty + DISCO_TH or ry + rh <= ty)
        face_tile = [False] * DISCO_TILES
        for r in range(DISCO_ROWS):
            for c in range(DISCO_COLS):
                tx, ty = c * DISCO_TW, r * DISCO_TH
                hit = any(hits(rx, ry, rw, rh, tx, ty) for rx, ry, rw, rh in DISCO_FACE_RECTS)
                face_tile[r * DISCO_COLS + c] = hit
        self.disco_face_tile = face_tile

        arg = [[0.0] * DISCO_TILES for _ in range(DISCO_NPAT)]
        cx = (DISCO_COLS - 1) * 0.5
        cy = (DISCO_ROWS - 1) * 0.5
        maxd = math.sqrt(cx * cx + cy * cy)
        two_pi, cyc = 6.2831853, 1.5
        for r in range(DISCO_ROWS):
            for c in range(DISCO_COLS):
                idx = r * DISCO_COLS + c
                dist = math.sqrt((c - cx) ** 2 + (r - cy) ** 2)
                q = [0.0] * 8
                q[DISCO_PAT_TLBR] = (c + r) / ((DISCO_COLS - 1) + (DISCO_ROWS - 1))
                q[DISCO_PAT_TRBL] = ((DISCO_COLS - 1 - c) + r) / ((DISCO_COLS - 1) + (DISCO_ROWS - 1))
                q[DISCO_PAT_LR] = c / (DISCO_COLS - 1)
                q[DISCO_PAT_RL] = (DISCO_COLS - 1 - c) / (DISCO_COLS - 1)
                q[DISCO_PAT_BT] = (DISCO_ROWS - 1 - r) / (DISCO_ROWS - 1)
                q[DISCO_PAT_TB] = r / (DISCO_ROWS - 1)
                q[DISCO_PAT_OUT] = dist / maxd
                q[DISCO_PAT_IN] = 1.0 - dist / maxd
                for p in range(8):
                    arg[p][idx] = q[p] * two_pi * cyc
                arg[DISCO_PAT_CHECKER][idx] = ((c + r) & 1) * 3.14159265
                arg[DISCO_PAT_PULSE][idx] = 0.0
                arg[DISCO_PAT_SPARK][idx] = 0.0
        self.disco_arg = arg
        self.disco_ready = True

    def _light_render_disco(self, painter: QPainter, level: float, bass: float, treble: float, now_ms: int, needs_init: bool) -> None:
        if not self.disco_ready:
            self._build_disco_table()
        if needs_init:
            self.disco_travel = 0.0
            self.disco_hue = 0.0
            self.disco_pal_off = 0
            self.disco_flash = 0.0
            self.disco_energy = 0.0
            self.disco_agc_peak = 0.0
            self.disco_bass_avg = 0.0
            if self.disco_pat_ms == 0:
                self.disco_pat = DISCO_PAT_TLBR
                self.disco_pat_ms = now_ms + DISCO_SWITCH_MIN

        if level > self.disco_agc_peak:
            self.disco_agc_peak = level
        else:
            self.disco_agc_peak += (level - self.disco_agc_peak) * 0.03
        ref = max(self.disco_agc_peak, 0.14)
        e = min(1.0, max(0.0, level / ref))
        self.disco_energy += (e - self.disco_energy) * 0.50

        self.disco_bass_avg += (bass - self.disco_bass_avg) * 0.15
        if bass > self.disco_bass_avg * 1.35 + 0.05 and now_ms >= self.disco_beat_cd:
            self.disco_flash = 1.0
            self.disco_pal_off += 1
            if (self.disco_pal_off & 7) == 0:
                self.disco_pal_set = (self.disco_pal_set + 1) % DISCO_NPAL
            self.disco_beat_cd = now_ms + 170
        self.disco_flash *= 0.70

        if now_ms >= self.disco_pat_ms:
            nx = self.disco_pat
            while True:
                self.disco_pat_rng = (self.disco_pat_rng * 1664525 + 1013904223) & 0xFFFFFFFF
                nx = (self.disco_pat_rng >> 24) % DISCO_NPAT
                if nx != self.disco_pat:
                    break
            self.disco_pat = nx
            self.disco_pat_rng = (self.disco_pat_rng * 1664525 + 1013904223) & 0xFFFFFFFF
            self.disco_pat_ms = now_ms + DISCO_SWITCH_MIN + (self.disco_pat_rng >> 18) % (DISCO_SWITCH_MAX - DISCO_SWITCH_MIN)
            if self.disco_flash < 0.85:
                self.disco_flash = 0.85
            self.disco_pal_set = (self.disco_pal_set + 1) % DISCO_NPAL

        self.disco_travel += DISCO_TRAVEL_BASE + self.disco_energy * DISCO_TRAVEL_K
        self.disco_hue += DISCO_HUE_BASE + self.disco_energy * DISCO_HUE_K
        hue_i = int(self.disco_hue)
        pal = self.disco_pal[self.disco_pal_set]
        flash_t = int(self.disco_flash * 150.0)
        spark_th = 3 + int(treble * 45.0)
        spark = (self.disco_pat == DISCO_PAT_SPARK)
        arg = self.disco_arg[self.disco_pat]

        for r in range(DISCO_ROWS):
            ty = r * DISCO_TH
            dy0 = max(ty, DISCO_TOP)
            dh = ty + DISCO_TH - dy0
            if dh <= 0:
                continue
            for c in range(DISCO_COLS):
                idx = r * DISCO_COLS + c
                hue = (c + r + hue_i + self.disco_pal_off) & 7
                base = pal[hue]
                if spark:
                    b = int(190.0 + self.disco_energy * 45.0)
                    shine = 0
                    if (self._disco_rand() & 0xFF) < (40 + int(treble * 80.0)):
                        b, shine = 255, 120
                else:
                    m = 0.5 + 0.5 * math.cos(arg[idx] - self.disco_travel)
                    b = int(220.0 + self.disco_energy * 35.0)
                    shine = int(m * m * (60.0 + self.disco_energy * 70.0))
                    if (self._disco_rand() & 0xFF) < spark_th:
                        b, shine = 255, 130
                face_tile = self.disco_face_tile[idx]
                if face_tile and b < 235:
                    b = 235
                b = min(255, b)
                col = self._rgb_scale(base, b)
                if shine > 0:
                    col = self._rgb_lerp(col, (255, 255, 255), shine)
                if flash_t > 0:
                    col = self._rgb_lerp(col, (255, 255, 255), flash_t)
                if face_tile:
                    col = self._rgb_lerp(col, (255, 255, 255), 55)
                painter.fillRect(c * DISCO_TW, dy0, DISCO_TW, dh, QColor(*col))

    # ----- Lighting #2: Laser Show（v0.5 Phase 3。CoreS3 lightRenderLaser()準拠）-----
    def _laser_rand(self) -> int:
        self.laser_rng = (self.laser_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.laser_rng

    def _laser_frand(self) -> float:
        return float((self._laser_rand() >> 8) & 0xFFFFFF) / 16777216.0

    @staticmethod
    def _laser_clip(x0: float, y0: float, x1: float, y1: float):
        dx, dy = x1 - x0, y1 - y0
        t0, t1 = 0.0, 1.0
        p = (-dx, dx, -dy, dy)
        q = (x0 - LASER_XMIN, LASER_XMAX - x0, y0 - LASER_YMIN, LASER_YMAX - y0)
        for i in range(4):
            if abs(p[i]) < 1e-6:
                if q[i] < 0:
                    return None
            else:
                r = q[i] / p[i]
                if p[i] < 0:
                    if r > t1:
                        return None
                    if r > t0:
                        t0 = r
                else:
                    if r < t0:
                        return None
                    if r < t1:
                        t1 = r
        return (x0 + t0 * dx, y0 + t0 * dy, x0 + t1 * dx, y0 + t1 * dy)

    def _laser_draw_seg(self, painter: QPainter, x0: float, y0: float, x1: float, y1: float, color) -> None:
        x0 = min(max(x0, 0), CANVAS_W - 1); x1 = min(max(x1, 0), CANVAS_W - 1)
        y0 = min(max(y0, SCENE_TOP), CANVAS_H - 1); y1 = min(max(y1, SCENE_TOP), CANVAS_H - 1)
        painter.setPen(QPen(QColor(*color), 1))
        painter.drawLine(int(x0), int(y0), int(x1), int(y1))

    def _laser_draw_beam(self, painter: QPainter, fx0: float, fy0: float, fx1: float, fy1: float, hue_rgb, bright: int, with_core: bool) -> None:
        clipped = self._laser_clip(fx0, fy0, fx1, fy1)
        if clipped is None:
            return
        x0, y0, x1, y1 = clipped
        length = math.hypot(x1 - x0, y1 - y0)
        if length < 1.0:
            return
        nx, ny = -(y1 - y0) / length, (x1 - x0) / length
        c_out = self._rgb_scale(hue_rgb, bright * 28 // 100)
        c_mid = self._rgb_scale(hue_rgb, bright * 70 // 100)
        c_core = (255, 255, 255) if bright > 200 else self._rgb_scale(hue_rgb, bright)
        for s in (-2, 2):
            ox, oy = nx * s, ny * s
            self._laser_draw_seg(painter, x0 + ox, y0 + oy, x1 + ox, y1 + oy, c_out)
        for s in (-1, 1):
            ox, oy = nx * s, ny * s
            self._laser_draw_seg(painter, x0 + ox, y0 + oy, x1 + ox, y1 + oy, c_mid)
        if with_core:
            self._laser_draw_seg(painter, x0, y0, x1, y1, c_core)

    def _laser_push(self, ox: float, oy: float, a: float, hue_rgb, bright: float) -> None:
        if len(self.laser_beam) >= LASER_MAX:
            return
        ex, ey = ox + math.cos(a) * 520.0, oy + math.sin(a) * 520.0
        self.laser_beam.append({"x0": ox, "y0": oy, "x1": ex, "y1": ey, "hue": hue_rgb, "bright": int(bright)})

    def _light_render_laser(self, painter: QPainter, level: float, bass: float, treble: float, now_ms: int, needs_init: bool) -> None:
        if needs_init:
            self.laser_phase = 0.0
            self.laser_flash = 0.0
            self.laser_energy = 0.0
            self.laser_agc_peak = 0.0
            self.laser_bass_avg = 0.0
            self.laser_prev = []
            self.laser_beam = []
            if self.laser_pat_ms == 0:
                self.laser_pat = LASER_CROSS
                self.laser_pat_ms = now_ms + LASER_SWITCH_MIN

        if level > self.laser_agc_peak:
            self.laser_agc_peak = level
        else:
            self.laser_agc_peak += (level - self.laser_agc_peak) * 0.03
        ref = max(self.laser_agc_peak, 0.14)
        e = min(1.0, max(0.0, level / ref))
        self.laser_energy += (e - self.laser_energy) * 0.5

        self.laser_bass_avg += (bass - self.laser_bass_avg) * 0.15
        if bass > self.laser_bass_avg * 1.35 + 0.05 and now_ms >= self.laser_beat_cd:
            self.laser_flash = 1.0
            self.laser_beat_cd = now_ms + 160
        self.laser_flash *= 0.78

        if now_ms >= self.laser_pat_ms:
            nx = self.laser_pat
            while True:
                self.laser_rng = (self.laser_rng * 1664525 + 1013904223) & 0xFFFFFFFF
                nx = (self.laser_rng >> 25) % LASER_NPAT
                if nx != self.laser_pat:
                    break
            self.laser_pat = nx
            self.laser_rng = (self.laser_rng * 1664525 + 1013904223) & 0xFFFFFFFF
            self.laser_pat_ms = now_ms + LASER_SWITCH_MIN + (self.laser_rng >> 18) % (LASER_SWITCH_MAX - LASER_SWITCH_MIN)
            self.laser_rand_ms = 0

        self.laser_phase += 0.03 + self.laser_energy * 0.11

        # Laserはオーバーレイ種別だがDesktopは単一選択のため、選択時は必ず自前で
        # 暗い会場をy>=SCENE_TOPへ敷く（CoreS3のDisco非併用時と同じ経路）。
        painter.fillRect(0, SCENE_TOP, CANVAS_W, CANVAS_H - SCENE_TOP, QColor(0, 0, 0))

        for b in self.laser_prev:
            self._laser_draw_beam(painter, b["x0"], b["y0"], b["x1"], b["y1"], b["hue"], b["bright"] * 30 // 100, False)

        self.laser_beam = []
        base_bright = min(245, 150 + int(self.laser_energy * 90.0))
        col = LASER_GREEN_RGB
        cx, cy_c = 160.0, 144.0

        if self.laser_pat == LASER_CROSS:
            ox = [LASER_XMIN, LASER_XMAX, LASER_XMIN, LASER_XMAX]
            oy = [LASER_YMIN, LASER_YMIN, LASER_YMAX, LASER_YMAX]
            wob = math.sin(self.laser_phase) * 0.10
            for k in range(4):
                a = math.atan2(cy_c - oy[k], cx - ox[k]) + wob * (1 if (k & 1) else -1)
                self._laser_push(ox[k], oy[k], a, col, base_bright)
        elif self.laser_pat == LASER_FAN:
            base = -1.5708 + math.sin(self.laser_phase * 0.8) * 0.55
            n = 5 + int(self.laser_energy * 3.0)
            for k in range(n):
                a = base + (k - (n - 1) * 0.5) * 0.22
                self._laser_push(cx, LASER_YMAX, a, col, base_bright)
        elif self.laser_pat == LASER_DUAL:
            for k in range(3):
                ph = self.laser_phase + k * 2.1
                l_y = LASER_YMIN + (0.5 + 0.5 * math.sin(ph)) * (LASER_YMAX - LASER_YMIN)
                r_y = LASER_YMIN + (0.5 + 0.5 * math.sin(ph + 3.14159)) * (LASER_YMAX - LASER_YMIN)
                self._laser_push(LASER_XMIN, l_y, math.atan2(cy_c - l_y, cx - LASER_XMIN) * 0.6, col, base_bright)
                self._laser_push(LASER_XMAX, r_y, 3.14159 - math.atan2(cy_c - r_y, cx - LASER_XMIN) * 0.6, LASER_GREEN_RGB, base_bright)
        elif self.laser_pat == LASER_XBURST:
            br = 70 + int(self.laser_flash * 185.0)
            self._laser_push(LASER_XMIN, LASER_YMIN, math.atan2(LASER_YMAX - LASER_YMIN, LASER_XMAX - LASER_XMIN), col, br)
            self._laser_push(LASER_XMAX, LASER_YMIN, math.atan2(LASER_YMAX - LASER_YMIN, LASER_XMIN - LASER_XMAX), col, br)
            if self.laser_flash > 0.5:
                self._laser_push(LASER_XMIN, cy_c, 0.0, LASER_GREEN_RGB, br)
                self._laser_push(cx, LASER_YMIN, 1.5708, LASER_GREEN_RGB, br)
        else:  # LASER_RANDOM
            if now_ms >= self.laser_rand_ms:
                self.laser_rand_ms = now_ms + 1600 + (self._laser_rand() % 1400)
            n = 4 + int(self.laser_energy * 4.0)
            for k in range(n):
                seed = float((self.laser_rand_ms >> 5) + k * 97)
                ox2 = LASER_XMIN + math.fmod(seed * 0.61803, 1.0) * (LASER_XMAX - LASER_XMIN)
                oy2 = LASER_YMIN + math.fmod(seed * 0.31831, 1.0) * (LASER_YMAX - LASER_YMIN)
                a = seed + self.laser_phase * 0.5
                self._laser_push(ox2, oy2, a, LASER_GREEN_RGB, base_bright)

        if treble > 0.35 and len(self.laser_beam) < LASER_MAX:
            a = self._laser_frand() * 6.2831853
            self._laser_push(cx, cy_c, a, LASER_GREEN_RGB, 120 + int(treble * 120.0))

        flash_add = int(self.laser_flash * 90.0)
        for b in self.laser_beam:
            br = min(255, b["bright"] + flash_add)
            self._laser_draw_beam(painter, b["x0"], b["y0"], b["x1"], b["y1"], b["hue"], br, True)

        self.laser_prev = list(self.laser_beam)

    # ----- Lighting #3: Aurora（v0.5 Phase 3。CoreS3 lightRenderAurora()準拠）-----
    def _aurora_column(self, painter: QPainter, cx: int, yc: float, h: float, col, inten: int) -> None:
        nseg = 5
        seg_h = max(1.0, (2.0 * h) / nseg)
        for s in range(nseg):
            y0f = yc - h + seg_h * s
            y0 = int(y0f)
            ht = int(seg_h) + 1
            if y0 < AURORA_TOP:
                ht -= (AURORA_TOP - y0)
                y0 = AURORA_TOP
            if y0 > CANVAS_H - 1 or ht <= 0:
                continue
            if y0 + ht > CANVAS_H:
                ht = CANVAS_H - y0
            prof = 1.0 - abs(s - (nseg - 1) * 0.5) / ((nseg - 1) * 0.5)
            bb = min(255, int(inten * (0.22 + 0.78 * prof)))
            painter.fillRect(cx, y0, AURORA_CW, ht, QColor(*self._rgb_scale(col, bb)))

    def _light_render_aurora(self, painter: QPainter, level: float, bass: float, treble: float, now_ms: int, needs_init: bool) -> None:
        if needs_init:
            self.aurora_t = 0.0
            self.aurora_energy = 0.0
            self.aurora_agc_peak = 0.0
            self.aurora_bass_avg = 0.0
            self.aurora_flash = 0.0

        if level > self.aurora_agc_peak:
            self.aurora_agc_peak = level
        else:
            self.aurora_agc_peak += (level - self.aurora_agc_peak) * 0.03
        ref = max(self.aurora_agc_peak, 0.14)
        e = min(1.0, max(0.0, level / ref))
        self.aurora_energy += (e - self.aurora_energy) * 0.35

        self.aurora_bass_avg += (bass - self.aurora_bass_avg) * 0.15
        if bass > self.aurora_bass_avg * 1.4 + 0.05 and now_ms >= self.aurora_beat_cd:
            self.aurora_flash = 1.0
            self.aurora_beat_cd = now_ms + 220
        self.aurora_flash *= 0.86

        self.aurora_t += 0.020 + self.aurora_energy * 0.060

        painter.fillRect(0, AURORA_TOP, CANVAS_W, CANVAS_H - AURORA_TOP, QColor(*AURORA_SKY_RGB))

        base_inten = min(255, 150 + int(self.aurora_energy * 80.0) + int(self.aurora_flash * 60.0))
        hue_shift = int(self.aurora_t * 0.7)

        for layer in range(2):
            freq = 0.9 if layer == 0 else 1.6
            spd = 1.0 if layer == 0 else 1.7
            amp = 34.0 if layer == 0 else 22.0
            cen = 130.0 if layer == 0 else 108.0
            hbase = 30.0 if layer == 0 else 22.0
            dim = 0 if layer == 0 else 30
            for c in range(AURORA_NCOL):
                ph = self.aurora_col_base[c] * freq + self.aurora_t * spd + layer * 2.3
                yc = cen + amp * math.sin(ph)
                h = hbase * (0.6 + 0.4 * math.sin(ph * 1.7 + c))
                col = self.aurora_pal[(c + hue_shift + layer * 2) % AURORA_NPAL]
                inten = max(40, base_inten - dim)
                self._aurora_column(painter, c * AURORA_CW, yc, h, col, inten)

    # ----- Lighting #4: Matrix（v0.5 Phase 3。CoreS3 lightRenderMatrix()準拠）-----
    def _matrix_rand(self) -> int:
        self.matrix_rng = (self.matrix_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.matrix_rng

    def _build_matrix_table(self) -> None:
        head, length, spd = [], [], []
        for _c in range(MATRIX_NCOL):
            head.append(float(MATRIX_TOP) + float(int(self._matrix_rand() % 400) - 200))
            length.append(4 + (self._matrix_rand() % 6))
            spd.append(0.7 + (self._matrix_rand() % 60) / 100.0)
        self.matrix_head_y, self.matrix_len, self.matrix_speed_var = head, length, spd
        self.matrix_ready = True

    def _light_render_matrix(self, painter: QPainter, level: float, bass: float, treble: float, now_ms: int, needs_init: bool) -> None:
        if not self.matrix_ready:
            self._build_matrix_table()
        if needs_init:
            self.matrix_energy = 0.0
            self.matrix_agc_peak = 0.0
            self.matrix_bass_avg = 0.0
            self.matrix_burst = 0.0
            self.matrix_scan_y = -1

        if level > self.matrix_agc_peak:
            self.matrix_agc_peak = level
        else:
            self.matrix_agc_peak += (level - self.matrix_agc_peak) * 0.03
        ref = max(self.matrix_agc_peak, 0.14)
        e = min(1.0, max(0.0, level / ref))
        self.matrix_energy += (e - self.matrix_energy) * 0.40

        self.matrix_bass_avg += (bass - self.matrix_bass_avg) * 0.15
        if bass > self.matrix_bass_avg * 1.35 + 0.05 and now_ms >= self.matrix_beat_cd:
            self.matrix_burst = 1.0
            self.matrix_beat_cd = now_ms + 170
            self.matrix_scan_y = MATRIX_TOP + 4
        self.matrix_burst *= 0.80

        painter.fillRect(0, MATRIX_TOP, CANVAS_W, CANVAS_H - MATRIX_TOP, QColor(0, 0, 0))

        fall = 1.2 + self.matrix_energy * 4.5 + self.matrix_burst * 7.0
        spark_th = 2 + int(treble * 40.0)

        for c in range(MATRIX_NCOL):
            self.matrix_head_y[c] += fall * self.matrix_speed_var[c]
            if self.matrix_head_y[c] - self.matrix_len[c] * MATRIX_CELLH > CANVAS_H:
                self.matrix_head_y[c] = float(MATRIX_TOP) - float(self._matrix_rand() % 120)
                self.matrix_len[c] = 4 + (self._matrix_rand() % 6)
                self.matrix_speed_var[c] = 0.7 + (self._matrix_rand() % 60) / 100.0
            cx = c * MATRIX_COLW
            for k in range(self.matrix_len[c]):
                y = self.matrix_head_y[c] - k * MATRIX_CELLH
                y0 = int(y)
                y1 = y0 + MATRIX_CELLH - 3
                if y1 < MATRIX_TOP or y0 > CANVAS_H - 1:
                    continue
                if y0 < MATRIX_TOP:
                    y0 = MATRIX_TOP
                if y1 > CANVAS_H - 1:
                    y1 = CANVAS_H - 1
                if k == 0:
                    col = self._rgb_lerp(MATRIX_GREEN_RGB, (255, 255, 255), 150)
                else:
                    b = max(0.12, 1.0 - k * 0.15)
                    col = self._rgb_scale(MATRIX_GREEN_RGB, int(b * 255.0))
                if (self._matrix_rand() & 0xFF) < spark_th:
                    col = self._rgb_lerp(MATRIX_GREEN_RGB, (255, 255, 255), 200)
                painter.fillRect(cx + 2, y0, MATRIX_COLW - 4, y1 - y0 + 1, QColor(*col))

        if self.matrix_scan_y >= MATRIX_TOP:
            painter.fillRect(0, self.matrix_scan_y, CANVAS_W, 3, QColor(*MATRIX_CYAN_RGB))
            self.matrix_scan_y += 28
            if self.matrix_scan_y > 236:
                self.matrix_scan_y = -1

    # ----- Lighting: Hypnotic Vortex（v0.5 Phase 3。CoreS3 lightRenderVortex()準拠。
    # 音声非依存・実dt回転＝ユーザー指定のdtベース物理方針に最初から合致する効果）-----
    def _build_vortex_table(self) -> None:
        radius_tab, twist_tab = [], []
        for s in range(VTX_STEPS):
            t = s / (VTX_STEPS - 1)
            radius_tab.append(VTX_MAXR * (t ** 0.88))
            twist_tab.append(t * VTX_TWIST)
        self.vtx_radius_tab, self.vtx_twist_tab = radius_tab, twist_tab
        self.vtx_ready = True

    def _light_render_vortex(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if not self.vtx_ready:
            self._build_vortex_table()
        if needs_init or self.vtx_prev_ms == 0:
            self.vtx_rot = 0.0
            self.vtx_prev_ms = now_ms
        dt = (now_ms - self.vtx_prev_ms) / 1000.0
        if dt > 0.5:
            dt = 0.5
        self.vtx_prev_ms = now_ms
        self.vtx_rot += dt * VTX_ROT_SPEED

        painter.save()
        painter.setClipRect(0, VTX_TOP, CANVAS_W, CANVAS_H - VTX_TOP)
        painter.fillRect(0, VTX_TOP, CANVAS_W, CANVAS_H - VTX_TOP, QColor(255, 255, 255))

        two_pi_over_arms = 6.2831853 / VTX_ARMS
        black, white = QColor(0, 0, 0), QColor(255, 255, 255)

        bx = [[0.0] * VTX_STEPS for _ in range(VTX_ARMS + 1)]
        by = [[0.0] * VTX_STEPS for _ in range(VTX_ARMS + 1)]
        for k in range(VTX_ARMS + 1):
            base_ang = (k % VTX_ARMS) * two_pi_over_arms + self.vtx_rot
            for s in range(VTX_STEPS):
                ang = base_ang + self.vtx_twist_tab[s]
                r = self.vtx_radius_tab[s]
                bx[k][s] = VTX_CX + math.cos(ang) * r
                by[k][s] = VTX_CY + math.sin(ang) * r

        for k in range(VTX_ARMS):
            col = white if (k & 1) else black
            for s in range(VTX_STEPS - 1):
                self._poly(painter, [(bx[k][s], by[k][s]), (bx[k + 1][s], by[k + 1][s]), (bx[k][s + 1], by[k][s + 1])], col)
                self._poly(painter, [(bx[k + 1][s], by[k + 1][s]), (bx[k + 1][s + 1], by[k + 1][s + 1]), (bx[k][s + 1], by[k][s + 1])], col)

        painter.restore()

    # ----- Lighting: Rainbow Washing Machine（v0.5 Phase 3。CoreS3
    # lightRenderRainbowWashingMachine()準拠。実dt駆動＝ユーザー指定のdtベース物理方針）-----
    def _rwm_rand(self) -> int:
        x = self.rwm_lcg & 0xFFFFFFFF
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        self.rwm_lcg = x
        return x

    def _rwm_rand01(self) -> float:
        return (self._rwm_rand() & 0xFFFF) / 65535.0

    def _rwm_spawn_shard(self, r_start: float) -> dict:
        ang = self._rwm_rand01() * 6.2831853
        half_w = 0.045 + self._rwm_rand01() * 0.10
        return {
            "r": r_start,
            "ang": ang,
            "awl": half_w * (0.7 + self._rwm_rand01() * 0.6),
            "awr": half_w * (0.7 + self._rwm_rand01() * 0.6),
            "apex": (self._rwm_rand01() - 0.5) * half_w * 0.6,
            "gm": 0.82 + self._rwm_rand01() * 0.36,
            "ci": self._rwm_rand() % len(RWM_PALETTE_RGB),
        }

    def _build_rwm_table(self) -> None:
        self.rwm_lcg = 20260810
        self.rwm_shard = [self._rwm_spawn_shard((i / RWM_COUNT) * RWM_MAX_R) for i in range(RWM_COUNT)]
        self.rwm_ready = True

    def _light_render_rwm(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if not self.rwm_ready:
            self._build_rwm_table()
        if needs_init or self.rwm_prev_ms == 0:
            self.rwm_prev_ms = now_ms
            self.rwm_time_sec = 0.0
        dt = (now_ms - self.rwm_prev_ms) / 1000.0
        if dt > 0.5:
            dt = 0.5
        self.rwm_prev_ms = now_ms
        self.rwm_time_sec += dt

        painter.save()
        painter.setClipRect(0, RWM_TOP, CANVAS_W, CANVAS_H - RWM_TOP)
        painter.fillRect(0, RWM_TOP, CANVAS_W, CANVAS_H - RWM_TOP, QColor(0, 0, 0))

        for i, s in enumerate(self.rwm_shard):
            band = int(s["r"] / RWM_MAX_R * RWM_BAND_COUNT)
            band = max(0, min(RWM_BAND_COUNT - 1, band))
            wobble = 1.0 + RWM_BAND_WOBBLE_AMP * math.sin(self.rwm_time_sec * RWM_BAND_WOBBLE_FREQ[band] + band * 1.7)
            s["ang"] += RWM_BAND_ANGVEL[band] * wobble * dt
            s["r"] += (RWM_GROWTH_RATE * s["r"] + RWM_GROWTH_MIN) * s["gm"] * dt

            if s["r"] > RWM_MAX_R + 24.0:
                self.rwm_shard[i] = self._rwm_spawn_shard(RWM_RESPAWN_R + self._rwm_rand01() * RWM_RESPAWN_JITTER)
                continue

            depth_half = 3.0 + s["r"] * 0.09
            apex_r = max(0.0, s["r"] - depth_half)
            base_r = s["r"] + depth_half
            apex_ang = s["ang"] + s["apex"]
            ax = RWM_CX + math.cos(apex_ang) * apex_r
            ay = RWM_CY + math.sin(apex_ang) * apex_r
            lx = RWM_CX + math.cos(s["ang"] - s["awl"]) * base_r
            ly = RWM_CY + math.sin(s["ang"] - s["awl"]) * base_r
            rx = RWM_CX + math.cos(s["ang"] + s["awr"]) * base_r
            ry = RWM_CY + math.sin(s["ang"] + s["awr"]) * base_r
            col = QColor(*RWM_PALETTE_RGB[s["ci"]])
            self._poly(painter, [(ax, ay), (lx, ly), (rx, ry)], col)

        self._fill_circle(painter, RWM_CX, RWM_CY, 2, QColor(255, 255, 255))
        painter.restore()

    # ----- Lighting: Eye Slot（v0.5 Phase 4a。CoreS3 lightRenderEyeSlot()準拠）-----
    def _eslot_rand(self) -> int:
        self.eslot_rng = (self.eslot_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.eslot_rng

    def _eslot_rand01(self) -> float:
        return (self._eslot_rand() & 0xFFFF) / 65535.0

    def _eslot_draw_symbol(self, painter: QPainter, sym: int, cx: int, cy: int) -> None:
        r = ESLOT_SYM_R
        painter.setPen(Qt.NoPen)
        if sym == 0:  # 7（赤・太字）
            self._draw_text_centered(painter, "7", cx, cy, 22, QColor(255, 0, 0))
        elif sym == 1:  # BAR（黒いプレート＋白い太字BAR）
            painter.setBrush(QBrush(QColor(0, 0, 0)))
            painter.drawRect(cx - r, cy - r + 4, r * 2, r * 2 - 8)
            self._draw_text_centered(painter, "BAR", cx, cy, 11, QColor(255, 255, 255))
        elif sym == 2:  # Cherry（赤い実2つ＋緑の軸）
            painter.setBrush(QBrush(QColor(40, 150, 60)))
            painter.drawRect(cx - 2, cy - r, 4, r)
            self._fill_circle(painter, cx - r // 2, cy + r // 3, r // 2, QColor(255, 0, 0))
            self._fill_circle(painter, cx + r // 2, cy + r // 3, r // 2, QColor(255, 0, 0))
        elif sym == 3:  # ●（紺）
            self._fill_circle(painter, cx, cy, r - 1, QColor(30, 60, 150))
        elif sym == 4:  # ¥（太めのY型＋横線2本・黄色系）
            yen = QColor(255, 210, 40)
            th = 4
            leg_x = (r * 55) // 100
            self._draw_thick_line(painter, cx - leg_x, cy - r, cx, cy, th, yen)
            self._draw_thick_line(painter, cx + leg_x, cy - r, cx, cy, th, yen)
            self._draw_thick_line(painter, cx, cy, cx, cy + r, th, yen)
            painter.setPen(Qt.NoPen)
            painter.setBrush(QBrush(yen))
            painter.drawRect(cx - r // 2, cy + 2, r, th)
            painter.drawRect(cx - r // 2, cy + 2 + th * 2, r, th)
        elif sym == 5:  # $（太めのS＋貫通する縦線・緑系）
            dollar = QColor(60, 190, 80)
            th = 4
            hw = (r * 60) // 100
            vh = (r * 60) // 100
            painter.setBrush(QBrush(dollar))
            painter.drawRect(cx - hw, cy - r, hw * 2, th)
            painter.drawRect(cx - hw, cy - r, th, vh)
            painter.drawRect(cx - hw, cy - th // 2, hw * 2, th)
            painter.drawRect(cx + hw - th, cy, th, vh)
            painter.drawRect(cx - hw, cy + r - th, hw * 2, th)
            painter.drawRect(cx - th // 2, cy - r - 2, th, r * 2 + 4)
        elif sym == 6:  # ❤（赤い塗りつぶしハート・左右対称）
            heart = QColor(255, 0, 0)
            self._fill_circle(painter, cx - r // 2, cy - r // 4, r // 2, heart)
            self._fill_circle(painter, cx + r // 2, cy - r // 4, r // 2, heart)
            painter.setBrush(QBrush(heart))
            for i in range(r):
                row_w = r * 2 - i * 2
                if row_w < 2:
                    break
                painter.drawRect(cx - row_w // 2, cy - r // 4 + i, row_w, 2)
        else:  # 🥝（緑の断面：外周やや濃く・中心明るく・種を放射状に）
            self._fill_circle(painter, cx, cy, r - 1, QColor(70, 150, 50))
            self._fill_circle(painter, cx, cy, r - 5, QColor(190, 225, 140))
            painter.setBrush(QBrush(QColor(0, 0, 0)))
            for i in range(8):
                a = i * 0.7854
                sx = cx + int(math.cos(a) * (r - 8))
                sy = cy + int(math.sin(a) * (r - 8))
                painter.drawRect(sx - 1, sy - 1, 2, 2)

    def _eslot_draw_reel(self, painter: QPainter, reel_pos: float, center_x: int, center_y: int, strip) -> None:
        win_x = center_x - ESLOT_WIN_HALF_W
        win_y = center_y - ESLOT_ROW_H
        win_w = ESLOT_WIN_HALF_W * 2
        win_h = ESLOT_ROW_H * 2
        painter.save()
        painter.setClipRect(win_x, win_y, win_w, win_h)
        k0 = math.floor(reel_pos)
        for k in range(k0 - 1, k0 + 2):
            pos = reel_pos - k
            sym = strip[((k % ESLOT_SYMBOL_COUNT) + ESLOT_SYMBOL_COUNT) % ESLOT_SYMBOL_COUNT]
            row_y = center_y + round(pos * ESLOT_ROW_H)
            self._eslot_draw_symbol(painter, sym, center_x, row_y)
        painter.restore()
        # ペイライン（中央ラインの目印）。派手にしすぎず細い2本線に留める。
        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(QColor(180, 180, 180)))
        painter.drawRect(win_x, center_y - ESLOT_ROW_H // 2 - 1, win_w, 1)
        painter.drawRect(win_x, center_y + ESLOT_ROW_H // 2, win_w, 1)

    def _eslot_fill_windows_white(self, painter: QPainter) -> None:
        """リール窓（左右2枚）の背後だけを不透明な白で塗る。Visualizerが全画面描画した
        直後に絵柄を描き直す際、絵柄の隙間からVisualizerの色が透けないようにする
        （CoreS3 eslotFillWindowsWhite()準拠）。"""
        lx, cy = ESLOT_EYE_L
        rx, _ = ESLOT_EYE_R
        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(QColor(255, 255, 255)))
        painter.drawRect(lx - ESLOT_WIN_HALF_W, cy - ESLOT_ROW_H, ESLOT_WIN_HALF_W * 2, ESLOT_ROW_H * 2)
        painter.drawRect(rx - ESLOT_WIN_HALF_W, cy - ESLOT_ROW_H, ESLOT_WIN_HALF_W * 2, ESLOT_ROW_H * 2)

    def _eslot_draw_reels_frame(self, painter: QPainter, now_ms: int) -> None:
        """リール本体（＋揃った時の結果フラッシュ）を「現在の状態のまま」描くだけ
        （状態は一切進めない。CoreS3 eslotDrawReelsFrame()準拠）。"""
        if self.eslot_state == ESLOT_ST_RESULT and self.eslot_match:
            since = now_ms - self.eslot_result_at
            if since < 700:
                t = since / 700.0
                pulse = math.sin(t * math.pi)
                if pulse > 0.05:
                    hue = t * 6.0
                    seg = int(hue) % 6
                    f = hue - math.floor(hue)
                    if seg == 0:
                        rgb = (255, int(255 * f), 0)
                    elif seg == 1:
                        rgb = (int(255 * (1 - f)), 255, 0)
                    elif seg == 2:
                        rgb = (0, 255, int(255 * f))
                    elif seg == 3:
                        rgb = (0, int(255 * (1 - f)), 255)
                    elif seg == 4:
                        rgb = (int(255 * f), 0, 255)
                    else:
                        rgb = (255, 0, int(255 * (1 - f)))
                    lx, ly = ESLOT_EYE_L
                    rx, ry = ESLOT_EYE_R
                    ring_r = 26
                    painter.setPen(Qt.NoPen)
                    painter.setBrush(QBrush(QColor(*rgb)))
                    painter.drawRect(lx - ring_r, ly - 1, ring_r * 2, 2)
                    painter.drawRect(rx - ring_r, ry - 1, ring_r * 2, 2)

        lx, ly = ESLOT_EYE_L
        rx, ry = ESLOT_EYE_R
        self._eslot_draw_reel(painter, self.eslot_reel_pos[0], lx, ly, ESLOT_STRIP_L)
        self._eslot_draw_reel(painter, self.eslot_reel_pos[1], rx, ry, ESLOT_STRIP_R)

    def _eslot_update_and_draw_reels(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        """Eye Slotの状態更新＋リール本体の描画（CoreS3 eslotUpdateAndDrawReels()準拠）。"""
        self.eslot_active = True
        now = now_ms
        if needs_init:
            self.eslot_state = ESLOT_ST_ACCEL
            self.eslot_state_at = now
            self.eslot_state_dur = 500 + int(self._eslot_rand01() * 300.0)

        st = self.eslot_state
        if st == ESLOT_ST_ACCEL:
            dur = self.eslot_state_dur or 1
            p = min(1.0, (now - self.eslot_state_at) / float(dur))
            sp = ESLOT_MAX_SPEED * p
            self.eslot_reel_pos[0] += sp
            self.eslot_reel_pos[1] += sp
            if p >= 1.0:
                self.eslot_state = ESLOT_ST_SPIN
                self.eslot_state_at = now
                self.eslot_state_dur = 900 + int(self._eslot_rand01() * 900.0)
        elif st == ESLOT_ST_SPIN:
            self.eslot_reel_pos[0] += ESLOT_MAX_SPEED
            self.eslot_reel_pos[1] += ESLOT_MAX_SPEED
            if now - self.eslot_state_at >= self.eslot_state_dur:
                self.eslot_decel_from[0] = self.eslot_reel_pos[0]
                self.eslot_decel_at[0] = now
                self.eslot_decel_dur[0] = 650 + int(self._eslot_rand01() * 250.0)
                self.eslot_decel_to[0] = math.ceil(self.eslot_reel_pos[0]) + float(2 + (self._eslot_rand() % 3))
                self.eslot_state = ESLOT_ST_DECEL_L
                self.eslot_state_at = now
        elif st == ESLOT_ST_DECEL_L:
            self.eslot_reel_pos[1] += ESLOT_MAX_SPEED
            dur = self.eslot_decel_dur[0] or 1
            p = min(1.0, (now - self.eslot_decel_at[0]) / float(dur))
            e = 1.0 - (1.0 - p) ** 3
            self.eslot_reel_pos[0] = self.eslot_decel_from[0] + (self.eslot_decel_to[0] - self.eslot_decel_from[0]) * e
            if p >= 1.0:
                self.eslot_reel_pos[0] = self.eslot_decel_to[0]
                self.eslot_state = ESLOT_ST_GAP
                self.eslot_state_at = now
                self.eslot_state_dur = 350 + int(self._eslot_rand01() * 250.0)
        elif st == ESLOT_ST_GAP:
            self.eslot_reel_pos[1] += ESLOT_MAX_SPEED
            if now - self.eslot_state_at >= self.eslot_state_dur:
                self.eslot_decel_from[1] = self.eslot_reel_pos[1]
                self.eslot_decel_at[1] = now
                self.eslot_decel_dur[1] = 650 + int(self._eslot_rand01() * 250.0)
                self.eslot_decel_to[1] = math.ceil(self.eslot_reel_pos[1]) + float(2 + (self._eslot_rand() % 3))
                self.eslot_state = ESLOT_ST_DECEL_R
                self.eslot_state_at = now
        elif st == ESLOT_ST_DECEL_R:
            dur = self.eslot_decel_dur[1] or 1
            p = min(1.0, (now - self.eslot_decel_at[1]) / float(dur))
            e = 1.0 - (1.0 - p) ** 3
            self.eslot_reel_pos[1] = self.eslot_decel_from[1] + (self.eslot_decel_to[1] - self.eslot_decel_from[1]) * e
            if p >= 1.0:
                self.eslot_reel_pos[1] = self.eslot_decel_to[1]
                k_l = round(self.eslot_reel_pos[0])
                k_r = round(self.eslot_reel_pos[1])
                sym_l = ESLOT_STRIP_L[((k_l % ESLOT_SYMBOL_COUNT) + ESLOT_SYMBOL_COUNT) % ESLOT_SYMBOL_COUNT]
                sym_r = ESLOT_STRIP_R[((k_r % ESLOT_SYMBOL_COUNT) + ESLOT_SYMBOL_COUNT) % ESLOT_SYMBOL_COUNT]
                self.eslot_match = (sym_l == sym_r)
                self.eslot_result_at = now
                self.eslot_state = ESLOT_ST_RESULT
                self.eslot_state_at = now
                self.eslot_state_dur = 1600 + int(self._eslot_rand01() * 900.0)
        elif st == ESLOT_ST_RESULT:
            if now - self.eslot_state_at >= self.eslot_state_dur:
                self.eslot_state = ESLOT_ST_ACCEL
                self.eslot_state_at = now
                self.eslot_state_dur = 500 + int(self._eslot_rand01() * 300.0)

        self._eslot_draw_reels_frame(painter, now_ms)

    def _light_render_eyeslot(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        painter.fillRect(0, SCENE_TOP, CANVAS_W, CANVAS_H - SCENE_TOP, QColor("white"))
        self._eslot_update_and_draw_reels(painter, now_ms, needs_init)

    # ----- Lighting: Flower Clock（v0.5 Phase 4a。CoreS3 lightRenderFlowerClock()＋
    # fcDrawHandsForeground()準拠）-----
    @staticmethod
    def _fc_rounded_rect_dist(dx: float, dy: float, hw: float, hh: float, r: float) -> float:
        tx = (hw / abs(dx)) if abs(dx) > 1e-6 else 1e9
        ty = (hh / abs(dy)) if abs(dy) > 1e-6 else 1e9
        t = min(tx, ty)
        px, py = dx * t, dy * t
        corner_zone = (abs(py) > (hh - r)) if t == tx else (abs(px) > (hw - r))
        if not corner_zone:
            return t
        ccx = (hw - r) if dx >= 0.0 else -(hw - r)
        ccy = (hh - r) if dy >= 0.0 else -(hh - r)
        b = dx * ccx + dy * ccy
        c = ccx * ccx + ccy * ccy - r * r
        disc = max(0.0, b * b - c)
        sq = math.sqrt(disc)
        t_near = b - sq
        return t_near if t_near > 0.0 else (b + sq)

    def _build_flowerclock_table(self) -> None:
        step = 2.0 * math.pi / FC_PETALS
        inner, outer = [], []
        for k in range(FC_PETALS + 1):
            ang = k * step - step * 0.5
            s, c = math.sin(ang), math.cos(ang)
            r_inner = self._fc_rounded_rect_dist(s, -c, FC_RECT_HW, FC_RECT_HH, FC_RECT_R)
            inner.append((FC_CX + r_inner * s, FC_CY - r_inner * c))
            outer.append((FC_CX + FC_R_OUTER * s, FC_CY - FC_R_OUTER * c))
        self.fc_inner = inner
        self.fc_outer = outer
        self.fc_ready = True

    def _fc_draw_hand(self, painter: QPainter, ang_rad: float, length: float, half_width: float, rgb) -> None:
        sa, ca = math.sin(ang_rad), math.cos(ang_rad)
        tip = (FC_CX + length * sa, FC_CY - length * ca)
        b1 = (FC_CX + half_width * ca, FC_CY + half_width * sa)
        b2 = (FC_CX - half_width * ca, FC_CY - half_width * sa)
        self._poly(painter, [b1, b2, tip], QColor(*rgb))

    def _light_render_flowerclock(self, painter: QPainter) -> None:
        """文字盤（12色区画＋中央白窓＋12/3/6/9の数字）のみを描く。針はここでは描かない
        （顔の鼻・口に隠れてしまうため、_draw_lighting_foreground()の専用フックへ分離）。"""
        if not self.fc_ready:
            self._build_flowerclock_table()
        painter.save()
        painter.setClipRect(0, FC_TOP, CANVAS_W, CANVAS_H - FC_TOP)
        for k in range(FC_PETALS):
            col = QColor(*FC_COLORS_RGB[k])
            i0, i1 = self.fc_inner[k], self.fc_inner[k + 1]
            o0, o1 = self.fc_outer[k], self.fc_outer[k + 1]
            self._poly(painter, [i0, i1, o1], col)
            self._poly(painter, [i0, o1, o0], col)
        pen = QPen(QColor("black"))
        pen.setWidth(1)
        painter.setPen(pen)
        for k in range(FC_PETALS):
            ix, iy = self.fc_inner[k]
            ox, oy = self.fc_outer[k]
            painter.drawLine(self._qp(ix, iy), self._qp(ox, oy))
        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(QColor("white")))
        painter.drawRoundedRect(
            int(FC_CX - FC_RECT_HW), int(FC_CY - FC_RECT_HH),
            int(FC_RECT_HW * 2.0), int(FC_RECT_HH * 2.0), FC_RECT_R, FC_RECT_R,
        )
        purple = QColor(*FC_COL_PURPLE_RGB)
        self._draw_text_centered(painter, "12", FC_CX, int(FC_CY - FC_NUM_DY), FC_NUM_PT, purple)
        self._draw_text_centered(painter, "3", int(FC_CX + FC_NUM_DX), FC_CY, FC_NUM_PT, purple)
        self._draw_text_centered(painter, "6", FC_CX, int(FC_CY + FC_NUM_DY), FC_NUM_PT, purple)
        self._draw_text_centered(painter, "9", int(FC_CX - FC_NUM_DX), FC_CY, FC_NUM_PT, purple)
        painter.restore()

    def _fc_draw_hands_foreground(self, painter: QPainter) -> None:
        """CoreS3 fcDrawHandsForeground()準拠。顔レイヤーより最前面（Layer3）で針を描く
        ことで、鼻・口に隠れないようにする。秒針は無し。PC実時刻（ローカルタイムゾーン）を使用。"""
        painter.save()
        painter.setClipRect(0, FC_TOP, CANVAS_W, CANVAS_H - FC_TOP)
        now = datetime.now()
        minute_frac = now.minute / 60.0
        hour_frac = ((now.hour % 12) + minute_frac) / 12.0
        hour_ang = hour_frac * 2.0 * math.pi
        min_ang = minute_frac * 2.0 * math.pi
        self._fc_draw_hand(painter, hour_ang, FC_HAND_HOUR_LEN, FC_HAND_HOUR_HW, FC_COL_PURPLE_RGB)
        self._fc_draw_hand(painter, min_ang, FC_HAND_MIN_LEN, FC_HAND_MIN_HW, FC_COL_PINK_RGB)
        painter.restore()

    # ----- Lighting: BASEBALL Arcade（Phase 5B。CoreS3 lightRenderBaseball()準拠）-----
    @staticmethod
    def _bb_random(lo: int, hi_exclusive: int) -> int:
        """CoreS3のrandom(min,max)（min以上max未満の半開区間）と同じ意味で乱数を返す。
        BASEBALLはCoreS3側も専用LCGを持たずArduinoの共有random()をそのまま使うため、
        Desktopでも他モードのような専用_xx_rand()は追加せずPython標準randomを使う。"""
        return random.randint(lo, hi_exclusive - 1)

    @staticmethod
    def _bb_project(fx: float, fy: float):
        """CoreS3 bbProject()準拠：フィールド座標→画面座標＋奥行きスケール。
        ゲームロジック（距離判定・追跡・捕球等）はフィールド座標のまま行い、
        描画直前にのみこれを通す（CoreS3のコメント方針をそのまま踏襲）。"""
        depth = BB_HOME_Y - fy
        t = depth / BB_MAX_DEPTH
        if t < -0.15:
            t = -0.15
        if t > 1.05:
            t = 1.05
        tp = abs(t) ** BB_PERSPECTIVE_POW
        if t < 0.0:
            tp = -tp
        scale = 1.0 - tp * (1.0 - BB_MIN_SCALE)
        if scale < BB_MIN_SCALE:
            scale = BB_MIN_SCALE
        out_x = 160.0 + (fx - 160.0) * scale
        out_y = BB_HOME_SCR_Y - t * (BB_HOME_SCR_Y - BB_HORIZON_SCR_Y)
        return out_x, out_y, scale

    @staticmethod
    def _bb_boundary_ray_endpoint(angle_deg: float):
        """CoreS3 bbBoundaryRayEndpoint()準拠：ファウルラインはbbProject()を経由せず、
        画面座標上でホームベースを頂点とする一定角度の直線として直接引く。"""
        rad = math.radians(angle_deg)
        dx = math.sin(rad)
        dy = -math.cos(rad)
        len_x = 1e9
        len_y = 1e9
        if dx > 0.0001:
            len_x = (320.0 - BB_HOME_SCR_X) / dx
        elif dx < -0.0001:
            len_x = (0.0 - BB_HOME_SCR_X) / dx
        if dy < -0.0001:
            len_y = (float(BB_TOP) - BB_HOME_SCR_Y) / dy
        length = len_x if len_x < len_y else len_y
        return BB_HOME_SCR_X + dx * length, BB_HOME_SCR_Y + dy * length

    def _bb_draw_player(self, painter: QPainter, fx: float, fy: float, with_bat: bool, with_glove: bool, extra_scale: float) -> None:
        sx, sy, sc = self._bb_project(fx, fy)
        sc *= extra_scale
        x, y = int(round(sx)), int(round(sy))
        body_r = max(2, int(round(4.2 * sc)))
        head_r = max(2, int(round(2.6 * sc)))
        uni = QColor(*BB_PLAYER_RGB)
        cap = QColor(*BB_CAP_RGB)
        head_y = y - body_r - head_r + 1
        self._fill_circle(painter, x, y, body_r, uni)
        self._fill_circle(painter, x, head_y, head_r, uni)
        self._fill_circle(painter, x, head_y - (head_r // 2 + 1), head_r, cap)
        if with_glove:
            g_r = max(1, int(round(1.8 * sc)))
            self._fill_circle(painter, x - body_r - g_r + 1, y + 1, g_r, uni)
        if with_bat:
            blen = max(3, int(round(9.0 * sc)))
            painter.setPen(QPen(QColor(*BB_PLAYER_RGB), 1))
            painter.drawLine(x + body_r, y - 2, x + body_r + blen, y - 2 - blen)

    def _bb_draw_infield_dirt(self, painter: QPainter) -> None:
        """1塁・2塁・3塁を通る2次ベジエ曲線に幅を持たせた帯として内野の土を描く
        （CoreS3 v2.6 bbDrawInfieldDirt()準拠。円のblobを重ねる旧方式は使わない）。"""
        dirt = QColor(*BB_DIRT_RGB)
        hx, hy, hs = self._bb_project(BB_HOME_X, BB_HOME_Y)
        p1x, p1y, _p1s = self._bb_project(BB_FIRST_X, BB_FIRST_Y)
        p2x, p2y, _p2s = self._bb_project(BB_SECOND_X, BB_SECOND_Y)
        p3x, p3y, _p3s = self._bb_project(BB_THIRD_X, BB_THIRD_Y)

        ccx = 2.0 * p2x - 0.5 * (p1x + p3x)
        ccy = 2.0 * p2y - 0.5 * (p1y + p3y)

        m = BB_DIRT_CURVE_SEGS
        left = []
        right = []
        for k in range(m + 1):
            t = k / m
            omt = 1.0 - t
            bx = omt * omt * p1x + 2.0 * t * omt * ccx + t * t * p3x
            by = omt * omt * p1y + 2.0 * t * omt * ccy + t * t * p3y
            dx = 2.0 * omt * (ccx - p1x) + 2.0 * t * (p3x - ccx)
            dy = 2.0 * omt * (ccy - p1y) + 2.0 * t * (p3y - ccy)
            dl = math.sqrt(dx * dx + dy * dy)
            nx, ny = 0.0, 1.0
            if dl > 0.001:
                nx = -dy / dl
                ny = dx / dl
            left.append((bx + nx * BB_DIRT_BAND_HALFW, by + ny * BB_DIRT_BAND_HALFW))
            right.append((bx - nx * BB_DIRT_BAND_HALFW, by - ny * BB_DIRT_BAND_HALFW))

        for k in range(m):
            self._poly(painter, [left[k], right[k], left[k + 1]], dirt)
            self._poly(painter, [left[k + 1], right[k], right[k + 1]], dirt)

        home_r = max(4, int(round(BB_DIRT_HOME_R * hs)))
        self._fill_circle(painter, int(round(hx)), int(round(hy)), home_r, dirt)

        mx, my, msc = self._bb_project(BB_PITCHER_X, BB_PITCHER_Y)
        mr = max(3, int(round(BB_MOUND_R * msc)))
        self._fill_circle(painter, int(round(mx)), int(round(my)), mr, dirt)

    def _bb_advance_runners(self, bases: int) -> None:
        was_first = self.bb_on_first
        was_second = self.bb_on_second
        if bases >= 4:
            self.bb_on_first = self.bb_on_second = self.bb_on_third = False
        elif bases == 2:
            self.bb_on_third = was_first
            self.bb_on_second = True
            self.bb_on_first = False
        else:
            self.bb_on_third = was_second
            self.bb_on_second = was_first
            self.bb_on_first = True

    def _bb_reset_fielders_and_ball(self) -> None:
        for i in range(9):
            self.bb_fielder_x[i] = BB_FLD_HOME_X[i]
            self.bb_fielder_y[i] = BB_FLD_HOME_Y[i]
        self.bb_ball_x = BB_PITCHER_X
        self.bb_ball_y = BB_PITCHER_Y

    def _bb_reset_game(self, now_ms: int) -> None:
        self.bb_on_first = self.bb_on_second = self.bb_on_third = False
        self._bb_reset_fielders_and_ball()
        self.bb_state = BB_ST_PITCH
        self.bb_state_start_ms = now_ms

    @staticmethod
    def _bb_dist_sq_to_fielder(i: int, px: float, py: float) -> float:
        ddx = BB_FLD_HOME_X[i] - px
        ddy = BB_FLD_HOME_Y[i] - py
        return ddx * ddx + ddy * ddy

    # 投球が本塁へ到達した瞬間に呼ばれる。結果（OUT/SINGLE/DOUBLE/HOME RUN）を先に抽選し、
    # 見た目（狙い所・追跡担当・追跡速度）を後から生成する（CoreS3 v2.1〜v2.4の設計変更後の
    # 最終ロジックをそのまま踏襲。結果は以後一切変えない＝守備距離での再判定は行わない）。
    def _bb_start_swing(self, now_ms: int) -> None:
        r = self._bb_random(0, 100)
        if r < BB_OUT_PCT:
            bases = 0
        elif r < BB_OUT_PCT + BB_DOUBLE_PCT:
            bases = 2
        elif r < BB_OUT_PCT + BB_DOUBLE_PCT + BB_HR_PCT:
            bases = 4
        else:
            bases = 1

        self.bb_play_is_out = (bases == 0)
        self.bb_hit_bases = bases
        self.bb_fly_is_hr = (bases == 4)

        if bases == 0:
            self.bb_is_fly = self._bb_random(0, 100) < 45
        elif bases == 1:
            self.bb_is_fly = self._bb_random(0, 100) < 25
        else:
            self.bb_is_fly = True

        lo = BB_OUTFIELD_LO if self.bb_is_fly else BB_INFIELD_LO
        hi = BB_OUTFIELD_HI if self.bb_is_fly else BB_INFIELD_HI

        if self.bb_play_is_out:
            idx = lo + self._bb_random(0, hi - lo + 1)
            tx = BB_FLD_HOME_X[idx] + float(self._bb_random(-BB_OUT_JITTER, BB_OUT_JITTER + 1))
            ty = BB_FLD_HOME_Y[idx] + float(self._bb_random(-BB_OUT_JITTER, BB_OUT_JITTER + 1))
            self.bb_chaser_idx = idx
            self.bb_chase_speed = BB_FIELDER_SPEED
        else:
            best_tx, best_ty = BB_HOME_X, float(BB_TOP) + 8.0
            best_gap_sq = -1.0
            best_chaser = lo
            for _attempt in range(BB_HIT_GAP_TRIES):
                angle_deg = -50.0 + float(self._bb_random(0, 1001)) / 10.0
                rad = math.radians(angle_deg)
                dir_x = math.sin(rad)
                dir_y = -math.cos(rad)
                if bases == 1 and not self.bb_is_fly:
                    dist = 60.0 + float(self._bb_random(0, 111))
                elif bases == 1:
                    dist = 70.0 + float(self._bb_random(0, 41))
                elif bases == 2:
                    dist = 120.0 + float(self._bb_random(0, 71))
                else:
                    dist = 195.0 + float(self._bb_random(0, 66))
                cx = BB_HOME_X + dir_x * dist
                cy = BB_HOME_Y + dir_y * dist
                if cx < 12.0:
                    cx = 12.0
                elif cx > 308.0:
                    cx = 308.0
                if cy < float(BB_TOP) + 8.0:
                    cy = float(BB_TOP) + 8.0
                if cy > BB_HOME_Y - 10.0:
                    cy = BB_HOME_Y - 10.0

                nb = lo
                nbd_sq = 1e18
                for i in range(lo, hi + 1):
                    dd_sq = self._bb_dist_sq_to_fielder(i, cx, cy)
                    if dd_sq < nbd_sq:
                        nbd_sq = dd_sq
                        nb = i
                if nbd_sq > best_gap_sq:
                    best_gap_sq = nbd_sq
                    best_tx, best_ty = cx, cy
                    best_chaser = nb
            tx, ty = best_tx, best_ty
            self.bb_chaser_idx = best_chaser
            self.bb_chase_speed = BB_FIELDER_SPEED * BB_HIT_CHASE_FACTOR

        self.bb_hit_target_x = tx
        self.bb_hit_target_y = ty
        self.bb_hit_dist = math.sqrt((tx - BB_HOME_X) ** 2 + (ty - BB_HOME_Y) ** 2)

        self.bb_state = BB_ST_SWING_PAUSE
        self.bb_state_start_ms = now_ms

    # OUT/HITいずれも結果確定後は即座に次の投球へ連続して進む（結果表示のための
    # 待機状態は挟まない。CoreS3のコメント方針どおり「試合が止まって見える」ことを避ける）。
    def _bb_resolve_out(self, now_ms: int) -> None:
        self._bb_start_next_pitch(now_ms)

    def _bb_resolve_hit(self, now_ms: int) -> None:
        self._bb_advance_runners(self.bb_hit_bases)
        self._bb_start_next_pitch(now_ms)

    def _bb_start_next_pitch(self, now_ms: int) -> None:
        self._bb_reset_fielders_and_ball()
        self.bb_state = BB_ST_PITCH
        self.bb_state_start_ms = now_ms

    def _light_render_baseball(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        # ── 実時間dt方式（Fighter Duel/Missile Defenseとは異なりLIGHT_FRAME_UPDATE_MSで
        # ゲートしない。CoreS3自体がdtで進むため、Desktopの描画周期のままで正しい速度になる）──
        if needs_init or self.bb_prev_ms == 0:
            self._bb_reset_game(now_ms)
            self.bb_prev_ms = now_ms
        dt = (now_ms - self.bb_prev_ms) / 1000.0
        if dt > 0.12:
            dt = 0.12
        self.bb_prev_ms = now_ms
        now = now_ms

        if self.bb_state == BB_ST_PITCH:
            t = (now - self.bb_state_start_ms) / BB_PITCH_MS
            if t > 1.0:
                t = 1.0
            self.bb_ball_x = BB_PITCHER_X + (BB_HOME_X - BB_PITCHER_X) * t
            self.bb_ball_y = BB_PITCHER_Y + (BB_HOME_Y - BB_PITCHER_Y) * t
            if t >= 1.0:
                self._bb_start_swing(now)
        elif self.bb_state == BB_ST_SWING_PAUSE:
            if now - self.bb_state_start_ms >= BB_SWING_PAUSE_MS:
                self.bb_state = BB_ST_BALL_FLIGHT
                self.bb_state_start_ms = now
        elif self.bb_state == BB_ST_BALL_FLIGHT:
            speed = BB_FLY_SPEED if self.bb_is_fly else BB_GROUND_SPEED
            travel_ms = (self.bb_hit_dist / speed) * 1000.0
            if travel_ms < BB_MIN_FLIGHT_MS:
                travel_ms = BB_MIN_FLIGHT_MS
            elapsed = now - self.bb_state_start_ms
            still_flying = elapsed < travel_ms
            t = elapsed / travel_ms
            if t > 1.0:
                t = 1.0
            self.bb_ball_x = BB_HOME_X + (self.bb_hit_target_x - BB_HOME_X) * t
            self.bb_ball_y = BB_HOME_Y + (self.bb_hit_target_y - BB_HOME_Y) * t

            if self.bb_chaser_idx >= 0:
                fx = self.bb_fielder_x[self.bb_chaser_idx]
                fy = self.bb_fielder_y[self.bb_chaser_idx]
                ddx = self.bb_ball_x - fx
                ddy = self.bb_ball_y - fy
                dd = math.sqrt(ddx * ddx + ddy * ddy)
                if dd > 0.5:
                    step = self.bb_chase_speed * dt
                    if step > dd:
                        step = dd
                    self.bb_fielder_x[self.bb_chaser_idx] = fx + ddx / dd * step
                    self.bb_fielder_y[self.bb_chaser_idx] = fy + ddy / dd * step

            if not still_flying:
                if self.bb_play_is_out:
                    self._bb_resolve_out(now)
                else:
                    self._bb_resolve_hit(now)

        # ── 描画（ここから先だけbbProject()を通して斜め俯瞰の画面座標へ変換する） ──
        painter.fillRect(0, BB_TOP, CANVAS_W, 240 - BB_TOP, QColor(*BB_FIELD_RGB))

        hx, hy, _hs = self._bb_project(BB_HOME_X, BB_HOME_Y)
        fx1, fy1, fs1 = self._bb_project(BB_FIRST_X, BB_FIRST_Y)
        fx3, fy3, fs3 = self._bb_project(BB_THIRD_X, BB_THIRD_Y)
        sx2, sy2, ss2 = self._bb_project(BB_SECOND_X, BB_SECOND_Y)

        self._bb_draw_infield_dirt(painter)

        line_col = QColor(*BB_LINE_RGB)
        painter.setPen(QPen(line_col, 1))
        brx, bry = self._bb_boundary_ray_endpoint(BB_FIELD_HALF_ANGLE_DEG)
        blx, bly = self._bb_boundary_ray_endpoint(-BB_FIELD_HALF_ANGLE_DEG)
        painter.drawLine(int(BB_HOME_SCR_X), int(BB_HOME_SCR_Y), int(brx), int(bry))
        painter.drawLine(int(BB_HOME_SCR_X), int(BB_HOME_SCR_Y), int(blx), int(bly))

        painter.drawLine(int(hx), int(hy), int(fx1), int(fy1))
        painter.drawLine(int(fx1), int(fy1), int(sx2), int(sy2))
        painter.drawLine(int(sx2), int(sy2), int(fx3), int(fy3))
        painter.drawLine(int(fx3), int(fy3), int(hx), int(hy))

        s1 = max(1, int(round(3.0 * fs1)))
        s2 = max(1, int(round(3.0 * ss2)))
        s3 = max(1, int(round(3.0 * fs3)))
        painter.fillRect(int(fx1) - s1, int(fy1) - s1, s1 * 2, s1 * 2, line_col)
        painter.fillRect(int(sx2) - s2, int(sy2) - s2, s2 * 2, s2 * 2, line_col)
        painter.fillRect(int(fx3) - s3, int(fy3) - s3, s3 * 2, s3 * 2, line_col)
        painter.fillRect(int(hx) - 4, int(hy) - 4, 8, 8, line_col)
        painter.setPen(QPen(line_col, 1))
        painter.setBrush(Qt.NoBrush)
        painter.drawRect(int(hx) - 20, int(hy) - 8, 10, 18)
        painter.drawRect(int(hx) + 10, int(hy) - 8, 10, 18)

        if self.bb_on_first:
            self._bb_draw_player(painter, BB_FIRST_X - 14.0, BB_FIRST_Y + 8.0, False, False, 0.9)
        if self.bb_on_second:
            self._bb_draw_player(painter, BB_SECOND_X, BB_SECOND_Y + 14.0, False, False, 0.9)
        if self.bb_on_third:
            self._bb_draw_player(painter, BB_THIRD_X + 14.0, BB_THIRD_Y + 8.0, False, False, 0.9)

        for i in range(9):
            self._bb_draw_player(painter, self.bb_fielder_x[i], self.bb_fielder_y[i], False, True, 1.0)
        self._bb_draw_player(painter, BB_HOME_X, BB_HOME_Y, True, False, 1.0)

        bx, by, bscale = self._bb_project(self.bb_ball_x, self.bb_ball_y)
        ball_r = 2.2 * bscale
        if self.bb_state == BB_ST_BALL_FLIGHT and self.bb_is_fly:
            speed = BB_FLY_SPEED
            travel_ms = (self.bb_hit_dist / speed) * 1000.0
            if travel_ms < 1.0:
                travel_ms = 1.0
            t = (now_ms - self.bb_state_start_ms) / travel_ms
            if t > 1.0:
                t = 1.0
            air_phase = math.sin(t * math.pi)
            ball_r += air_phase * 2.5 * bscale
        if ball_r < 1.0:
            ball_r = 1.0
        self._fill_circle(painter, int(round(bx)), int(round(by)), int(round(ball_r)), QColor(*BB_BALL_RGB))

    # ----- Lighting: Retro Race（v0.5 Phase 4b。CoreS3 lightRenderRace()準拠。
    # Pole Position風の疑似3Dレーススクリーンセーバー。自動運転ロジック（カーブ・速度の
    # ゆるやかな遷移、前方車両を見た回避操舵、対向車・看板の再抽選と再出現）は
    # CoreS3のlightRenderRace()を忠実に移植している。かりポムの黒目はDesktopに
    # eyeOffsetX/Y相当の機構が無いため追従させない（構造的な仕様差であり、
    # 自動運転ロジック自体の簡略化ではない）-----
    def _race_rand(self) -> int:
        self.race_rng = (self.race_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.race_rng

    def _race_rand01(self) -> float:
        return (self._race_rand() & 0xFFFF) / 65535.0

    def _race_spawn_car(self, i: int, initial_spread: bool) -> None:
        if initial_spread:
            self.race_car_z[i] = -self._race_rand01() * 1.4
        else:
            self.race_car_z[i] = -(0.25 + self._race_rand01() * 0.9)
        self.race_car_lane[i] = (self._race_rand01() * 2.0 - 1.0) * 0.55
        self.race_car_speed_mul[i] = 0.85 + self._race_rand01() * 0.30
        self.race_car_avoid_side[i] = 1 if (self._race_rand() & 1) else -1
        self.race_car_color_idx[i] = self._race_rand() % 4

    def _race_spawn_sign(self, i: int, initial_spread: bool) -> None:
        if initial_spread:
            self.race_sign_z[i] = -self._race_rand01() * 0.30
        else:
            self.race_sign_z[i] = -(0.05 + self._race_rand01() * 0.25)
        self.race_sign_side[i] = 1 if (self._race_rand() & 1) else -1
        self.race_sign_type[i] = self._race_rand() % 3
        self.race_sign_color_idx[i] = self._race_rand() % 4
        self.race_sign_text_idx[i] = self._race_rand() % 3
        self.race_sign_scale[i] = 0.85 + self._race_rand01() * 0.5
        self.race_sign_speed_mul[i] = 0.85 + self._race_rand01() * 0.4

    @staticmethod
    def _race_draw_arrow_glyph(painter: QPainter, left_x: int, cy: int, w: int, h: int, direction: int, color: QColor) -> None:
        steps = 5
        col_w = max(1, w // steps)
        for k in range(steps):
            frac = k if direction > 0 else (steps - 1 - k)
            row_h = h - (frac * h) // steps
            if row_h < 1:
                row_h = 1
            x = left_x + k * col_w
            painter.fillRect(x, cy - row_h // 2, col_w + 1, row_h, color)

    @staticmethod
    def _race_draw_logo_glyph(painter: QPainter, x0: int, y0: int, w: int, h: int, col_a: QColor, col_b: QColor) -> None:
        cw = max(1, w // 2)
        ch = max(1, h // 2)
        painter.fillRect(x0, y0, cw, ch, col_a)
        painter.fillRect(x0 + cw, y0, w - cw, ch, col_b)
        painter.fillRect(x0, y0 + ch, cw, h - ch, col_b)
        painter.fillRect(x0 + cw, y0 + ch, w - cw, h - ch, col_a)

    def _build_race_table(self) -> None:
        self.race_curve_target = self.race_curve_smooth = 0.0
        self.race_player_x = self.race_player_x_target = 0.0
        self.race_speed = self.race_speed_target = 1.0
        self.race_scroll_z = 0.0
        for i in range(RACE_MAX_CARS):
            self._race_spawn_car(i, True)
        for i in range(RACE_MAX_SIGNS):
            self._race_spawn_sign(i, True)
        self.race_ready = True

    def _light_render_race(self, painter: QPainter, level: float, now_ms: int, needs_init: bool) -> None:
        if not self.race_ready:
            self._build_race_table()
        if needs_init:
            self.race_curve_change_at = 0
            self.race_speed_change_at = 0

        if now_ms >= self.race_curve_change_at:
            r = self._race_rand01()
            if r < 0.30:
                self.race_curve_target = 0.0
            elif r < 0.65:
                self.race_curve_target = 0.35 + self._race_rand01() * 0.65
            else:
                self.race_curve_target = -(0.35 + self._race_rand01() * 0.65)
            self.race_curve_change_at = now_ms + 3500 + int(self._race_rand01() * 5000.0)
        self.race_curve_smooth += (self.race_curve_target - self.race_curve_smooth) * 0.02

        if now_ms >= self.race_speed_change_at:
            self.race_speed_target = 0.80 + self._race_rand01() * 0.55
            self.race_speed_change_at = now_ms + 2500 + int(self._race_rand01() * 3500.0)
        curve_drag = 1.0 - abs(self.race_curve_smooth) * 0.25
        audio_boost = 1.0 + level * 0.35
        self.race_speed += ((self.race_speed_target * curve_drag * audio_boost) - self.race_speed) * 0.03
        if self.race_speed < 0.35:
            self.race_speed = 0.35
        self.race_scroll_z += self.race_speed * 3.2

        danger_idx = -1
        danger_t = -1.0
        for i in range(RACE_MAX_CARS):
            if self.race_car_z[i] < 0.45 or self.race_car_z[i] > 0.97:
                continue
            if self.race_car_z[i] > danger_t:
                danger_t = self.race_car_z[i]
                danger_idx = i
        if danger_idx >= 0:
            avoid = self.race_car_lane[danger_idx] + self.race_car_avoid_side[danger_idx] * 0.55
            self.race_player_x_target = max(-0.85, min(0.85, avoid))
        else:
            wander = math.sin(now_ms * 0.0007) * 0.12
            self.race_player_x_target = max(-0.6, min(0.6, -self.race_curve_smooth * 0.30 + wander))
        self.race_player_x += (self.race_player_x_target - self.race_player_x) * 0.05

        painter.fillRect(0, RACE_TOP, CANVAS_W, (RACE_HORIZON - 18) - RACE_TOP, QColor(*RACE_SKY_TOP_RGB))
        painter.fillRect(0, RACE_HORIZON - 18, CANVAS_W, 18, QColor(*RACE_SKY_HORIZON_RGB))
        self._fill_circle(painter, 258, RACE_TOP + 20, 12, QColor(*RACE_SUN_RGB))

        steer_offset = -self.race_player_x * RACE_STEER_MAX_PX

        for y in range(RACE_HORIZON, 240, RACE_ROW_H):
            t = (y - RACE_HORIZON) / float(239 - RACE_HORIZON)
            row_offset = self.race_curve_smooth * RACE_CURVE_MAX_PX * (1.0 - t) * (1.0 - t)
            center_x = 160.0 + row_offset + steer_offset
            half_w = RACE_ROAD_MIN_HW + (RACE_ROAD_MAX_HW - RACE_ROAD_MIN_HW) * t * t

            row_h = min(RACE_ROW_H, 240 - y)
            cx_l = int(center_x - half_w)
            cx_r = int(center_x + half_w)

            stripe_idx = int((self.race_scroll_z * (0.4 + t * 2.2)) / 10.0) + (y // RACE_ROW_H)

            grass = RACE_GRASS_A_RGB if (stripe_idx & 1) else RACE_GRASS_B_RGB
            if cx_l > 0:
                painter.fillRect(0, y, cx_l, row_h, QColor(*grass))
            if cx_r < CANVAS_W:
                painter.fillRect(cx_r, y, CANVAS_W - cx_r, row_h, QColor(*grass))

            curb_w = 3 + int(abs(self.race_curve_smooth) * 5.0)
            if curb_w > int(half_w):
                curb_w = int(half_w)
            if curb_w < 1:
                curb_w = 1
            curb = RACE_CURB_A_RGB if (stripe_idx & 1) else RACE_CURB_B_RGB
            painter.fillRect(cx_l, y, curb_w, row_h, QColor(*curb))
            painter.fillRect(cx_r - curb_w, y, curb_w, row_h, QColor(*curb))

            road = RACE_ROAD_A_RGB if (stripe_idx & 1) else RACE_ROAD_B_RGB
            road_w = (cx_r - curb_w) - (cx_l + curb_w)
            if road_w > 0:
                painter.fillRect(cx_l + curb_w, y, road_w, row_h, QColor(*road))
            if ((stripe_idx // 2) & 1) == 0:
                line_w = 2 + int(t * 4.0)
                painter.fillRect(int(center_x - line_w / 2), y, line_w, row_h, QColor(*RACE_LINE_RGB))

        for i in range(RACE_MAX_SIGNS):
            self.race_sign_z[i] += 0.0017 * self.race_speed * self.race_sign_speed_mul[i]
            if self.race_sign_z[i] > 1.05:
                self._race_spawn_sign(i, False)
                continue
            if self.race_sign_z[i] < 0.0 or self.race_sign_z[i] > 1.0:
                continue

            t = self.race_sign_z[i]
            y = RACE_HORIZON + int(t * (239 - RACE_HORIZON))
            row_offset = self.race_curve_smooth * RACE_CURVE_MAX_PX * (1.0 - t) * (1.0 - t)
            center_x = 160.0 + row_offset + steer_offset
            half_w = RACE_ROAD_MIN_HW + (RACE_ROAD_MAX_HW - RACE_ROAD_MIN_HW) * t * t

            scale = (0.45 + t * t * 2.1) * self.race_sign_scale[i]
            plate_w = max(6, int(16.0 * scale))
            plate_h = max(5, int(11.0 * scale))
            post_h = int(6.0 * scale) + 2

            side = self.race_sign_side[i]
            edge_x = int(center_x + side * (half_w + 8.0))
            post_x = edge_x + side * (plate_w // 2 + 3)
            post_y1 = y
            post_y0 = post_y1 - post_h
            plate_x0 = post_x - plate_w // 2
            plate_y0 = post_y0 - plate_h

            plate_col = RACE_SIGN_RGB[self.race_sign_color_idx[i]]
            painter.fillRect(post_x - 1, post_y0, 2, post_h, QColor(0, 0, 0))
            painter.fillRect(plate_x0, plate_y0, plate_w, plate_h, QColor(*plate_col))

            if t > 0.30:
                dark_text = RACE_SIGN_DARKTEXT[self.race_sign_color_idx[i]]
                glyph_col = QColor(0, 0, 0) if dark_text else QColor(255, 255, 255)
                sign_type = self.race_sign_type[i]
                if sign_type == 0:
                    direction = 1 if self.race_curve_smooth >= 0.0 else -1
                    self._race_draw_arrow_glyph(painter, plate_x0 + 2, plate_y0 + plate_h // 2,
                                                 plate_w - 4, plate_h - 4, direction, glyph_col)
                elif sign_type == 1:
                    col_b = QColor(255, 255, 255) if dark_text else QColor(0, 0, 0)
                    self._race_draw_logo_glyph(painter, plate_x0 + 2, plate_y0 + 2, plate_w - 4, plate_h - 4, glyph_col, col_b)
                else:
                    self._draw_text_centered(painter, RACE_SIGN_TEXT[self.race_sign_text_idx[i]],
                                              plate_x0 + plate_w // 2, plate_y0 + plate_h // 2, 7, glyph_col)

        order = list(range(RACE_MAX_CARS))
        for a in range(RACE_MAX_CARS - 1):
            for b in range(a + 1, RACE_MAX_CARS):
                if self.race_car_z[order[b]] < self.race_car_z[order[a]]:
                    order[a], order[b] = order[b], order[a]

        for i in order:
            self.race_car_z[i] += 0.0026 * self.race_speed * self.race_car_speed_mul[i]
            if self.race_car_z[i] > 1.05:
                self._race_spawn_car(i, False)
                continue
            if self.race_car_z[i] < 0.0 or self.race_car_z[i] > 1.0:
                continue

            t = self.race_car_z[i]
            y = RACE_HORIZON + int(t * (239 - RACE_HORIZON))
            row_offset = self.race_curve_smooth * RACE_CURVE_MAX_PX * (1.0 - t) * (1.0 - t)
            center_x = 160.0 + row_offset + steer_offset
            half_w = RACE_ROAD_MIN_HW + (RACE_ROAD_MAX_HW - RACE_ROAD_MIN_HW) * t * t
            car_x = center_x + self.race_car_lane[i] * (half_w * 0.75)

            car_w = int(10.0 + t * t * 34.0)
            car_h = int(7.0 + t * t * 22.0)
            cx0 = int(car_x) - car_w // 2
            cy0 = y - car_h

            body = RACE_CAR_RGB[self.race_car_color_idx[i]]
            painter.fillRect(cx0, cy0, car_w, car_h, QColor(*body))
            painter.fillRect(cx0 + car_w // 5, cy0 - car_h // 3, car_w - car_w * 2 // 5, car_h // 3 + 1, QColor(0, 0, 0))
            wheel_h = (car_h // 4) if car_h > 6 else 1
            painter.fillRect(cx0, cy0 + car_h - wheel_h, car_w, wheel_h, QColor(0, 0, 0))

    # ----- Lighting: Sky Raid（v0.5 Phase 4b。CoreS3 lightRenderSkyRaid()準拠。
    # Xevious風の縦スクロールスクリーンセーバー。自機の自動操舵・敵の下降/ホーミング/揺れ・
    # 自動ショットと簡易命中判定・爆発演出・ハッシュ地形生成は、CoreS3の
    # lightRenderSkyRaid()をそのまま移植している。黒目はRetro Raceと同じ理由で
    # 追従させない（構造的な仕様差）-----
    def _xev_rand(self) -> int:
        self.xev_rng = (self.xev_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.xev_rng

    def _xev_rand01(self) -> float:
        return (self._xev_rand() & 0xFFFF) / 65535.0

    def _xev_hash(self, seg: int) -> int:
        h = (seg * 2654435761 + self.xev_seed) & 0xFFFFFFFF
        h ^= (h >> 13)
        h = (h * 0x85EBCA6B) & 0xFFFFFFFF
        h ^= (h >> 16)
        return h & 0xFFFFFFFF

    def _xev_spawn_enemy(self, i: int, initial_spread: bool) -> None:
        if initial_spread:
            self.xev_enemy_y[i] = float(XEV_TOP) - self._xev_rand01() * 260.0
        else:
            self.xev_enemy_y[i] = float(XEV_TOP) - (20.0 + self._xev_rand01() * 170.0)
        self.xev_enemy_x[i] = 50.0 + self._xev_rand01() * 220.0
        self.xev_enemy_speed[i] = 0.55 + self._xev_rand01() * 0.55
        self.xev_enemy_wob_phase[i] = self._xev_rand01() * 6.2832
        self.xev_enemy_color_idx[i] = self._xev_rand() % 3

    def _xev_spawn_explosion(self, x: float, y: float) -> None:
        for i in range(XEV_MAX_EXPL):
            if self.xev_expl_age[i] != 0:
                continue
            self.xev_expl_x[i] = x
            self.xev_expl_y[i] = y
            self.xev_expl_age[i] = 1
            self.xev_expl_seed[i] = self._xev_rand()
            return

    def _build_skyraid_table(self) -> None:
        self.xev_seed = self._xev_rand()
        self.xev_scroll_y = 0.0
        self.xev_speed = self.xev_speed_target = 1.0
        self.xev_ship_x = self.xev_ship_x_target = 160.0
        self.xev_next_shot_at = 0
        for i in range(XEV_MAX_ENEMIES):
            self._xev_spawn_enemy(i, True)
        for i in range(XEV_MAX_SHOTS):
            self.xev_shot_active[i] = False
        for i in range(XEV_MAX_EXPL):
            self.xev_expl_age[i] = 0
        self.xev_ready = True

    @staticmethod
    def _xev_draw_ship(painter: QPainter, cx: int, cy: int, col: QColor) -> None:
        painter.fillRect(cx - 2, cy - 10, 4, 10, col)
        painter.fillRect(cx - 8, cy - 2, 16, 4, col)
        painter.fillRect(cx - 2, cy - 14, 4, 4, QColor(255, 255, 255))

    @staticmethod
    def _xev_draw_enemy(painter: QPainter, cx: int, cy: int, size: int, col: QColor) -> None:
        if size < 3:
            size = 3
        painter.fillRect(cx - size // 2, cy - size // 2, size, size, col)
        painter.fillRect(cx - size // 4, cy - size // 4, size // 2 + 1, size // 2 + 1, QColor(0, 0, 0))

    def _xev_draw_explosion(self, painter: QPainter, cx: int, cy: int, age: int, seed: int) -> None:
        col = QColor(*XEV_EXPL_HOT_RGB) if age <= 2 else QColor(*XEV_EXPL_COOL_RGB)
        for k in range(6):
            h = self._xev_hash((seed + k * 97 + age * 131) & 0xFFFFFFFF)
            ang = (h % 360) * 3.14159 / 180.0
            dist = 2.0 + age * 2.1 + ((h >> 8) % 5)
            dx = int(math.cos(ang) * dist)
            dy = int(math.sin(ang) * dist)
            sz = 2 if age <= 3 else 1
            painter.fillRect(cx + dx, cy + dy, sz, sz, col)

    def _light_render_skyraid(self, painter: QPainter, level: float, now_ms: int, needs_init: bool) -> None:
        if not self.xev_ready:
            self._build_skyraid_table()
        if needs_init:
            self.xev_speed_change_at = 0
            self.xev_next_shot_at = 0

        if now_ms >= self.xev_speed_change_at:
            self.xev_speed_target = 0.80 + self._xev_rand01() * 0.55
            self.xev_speed_change_at = now_ms + 2500 + int(self._xev_rand01() * 3500.0)
        audio_boost = 1.0 + level * 0.35
        self.xev_speed += ((self.xev_speed_target * audio_boost) - self.xev_speed) * 0.03
        if self.xev_speed < 0.35:
            self.xev_speed = 0.35
        self.xev_scroll_y += self.xev_speed * 2.6

        danger_idx = -1
        danger_y = -1.0e9
        for i in range(XEV_MAX_ENEMIES):
            if self.xev_enemy_y[i] < XEV_SHIP_Y - 100.0 or self.xev_enemy_y[i] > XEV_SHIP_Y - 5.0:
                continue
            if abs(self.xev_enemy_x[i] - self.xev_ship_x) > 70.0:
                continue
            if self.xev_enemy_y[i] > danger_y:
                danger_y = self.xev_enemy_y[i]
                danger_idx = i
        if danger_idx >= 0:
            direction = -1.0 if self.xev_enemy_x[danger_idx] >= self.xev_ship_x else 1.0
            self.xev_ship_x_target = max(50.0, min(270.0, self.xev_ship_x + direction * 70.0))
        else:
            wander = math.sin(now_ms * 0.0009) * 70.0
            self.xev_ship_x_target = max(60.0, min(260.0, 160.0 + wander))
        self.xev_ship_x += (self.xev_ship_x_target - self.xev_ship_x) * 0.03

        for y in range(XEV_TOP, 240, XEV_ROW_H):
            row_h = min(XEV_ROW_H, 240 - y)
            wy = self.xev_scroll_y + (y - XEV_TOP)
            run = int(math.floor(wy / XEV_RUN_PX))
            h_r = self._xev_hash(run)

            grass = XEV_GRASS_A_RGB if ((run + int(wy / 6.0)) & 1) else XEV_GRASS_B_RGB
            painter.fillRect(0, y, CANVAS_W, row_h, QColor(*grass))

            if (h_r % 20) < 9:
                ftype = (h_r >> 5) % 5
                center_base = 60.0 + ((h_r >> 10) % 190)
                width_base = 40.0 + ((h_r >> 18) % 80)
                wiggle_amp = 8.0 + ((h_r >> 24) % 20)
                wiggle_phase = (h_r % 1000) * 0.00628

                if ftype == 0:
                    wiggle_scale = 1.00
                elif ftype == 1:
                    wiggle_scale = 0.15
                elif ftype == 2:
                    wiggle_scale = 0.90
                elif ftype == 3:
                    wiggle_scale = 0.05
                else:
                    wiggle_scale = 0.12

                wiggle = math.sin(wy * 0.02 + wiggle_phase) * wiggle_amp * wiggle_scale
                f_center = center_base + wiggle
                f_width = width_base + math.sin(wy * 0.013 + wiggle_phase * 1.7) * (width_base * 0.15)
                fx = int(f_center - f_width * 0.5)
                fw = int(f_width)
                if fx < 0:
                    fw += fx
                    fx = 0
                if fx + fw > CANVAS_W:
                    fw = CANVAS_W - fx

                if fw > 0:
                    row_idx = y // XEV_ROW_H
                    if ftype == 0:
                        painter.fillRect(fx, y, fw, row_h, QColor(*XEV_FOREST_RGB))
                        if ((row_idx ^ run) & 3) == 0:
                            painter.fillRect(fx + fw // 3, y, fw // 4 + 2, row_h, QColor(*XEV_FOREST_D_RGB))
                    elif ftype == 1:
                        painter.fillRect(fx, y, fw, row_h, QColor(*XEV_ROAD_RGB))
                        if (row_idx & 1) == 0:
                            painter.fillRect(fx + fw // 2 - 1, y, 2, row_h, QColor(*XEV_ROAD_LN_RGB))
                    elif ftype == 2:
                        painter.fillRect(fx, y, fw, row_h, QColor(*XEV_RIVER_RGB))
                        hi_x = int(f_center + math.sin(wy * 0.05) * (f_width * 0.15)) - 2
                        painter.fillRect(hi_x, y, 4, row_h, QColor(*XEV_RIVER_HI_RGB))
                    elif ftype == 3:
                        painter.fillRect(fx, y, fw, row_h, QColor(*XEV_BASE_RGB))
                        if (row_idx % 5) == 0:
                            painter.fillRect(fx + 4, y, (fw - 8 if fw > 8 else fw), row_h, QColor(*XEV_BASE_AC_RGB))
                    else:
                        painter.fillRect(fx, y, fw, row_h, QColor(*XEV_RUNWAY_RGB))
                        if (row_idx & 3) == 0:
                            painter.fillRect(fx + fw // 2 - 1, y, 2, row_h, QColor(*XEV_RUNWAY_LN_RGB))

        for i in range(XEV_MAX_ENEMIES):
            self.xev_enemy_y[i] += (1.4 + self.xev_enemy_speed[i] * 1.6) * self.xev_speed
            self.xev_enemy_x[i] += (self.xev_ship_x - self.xev_enemy_x[i]) * 0.0025
            self.xev_enemy_x[i] += math.sin(now_ms * 0.005 + self.xev_enemy_wob_phase[i]) * 0.6
            self.xev_enemy_x[i] = max(20.0, min(300.0, self.xev_enemy_x[i]))
            if self.xev_enemy_y[i] > 255.0:
                self._xev_spawn_enemy(i, False)
                continue
            if self.xev_enemy_y[i] < float(XEV_TOP):
                continue
            size = int(6.0 + (self.xev_enemy_y[i] - XEV_TOP) * 0.03)
            self._xev_draw_enemy(painter, int(self.xev_enemy_x[i]), int(self.xev_enemy_y[i]), size,
                                  QColor(*XEV_ENEMY_RGB[self.xev_enemy_color_idx[i]]))

        if now_ms >= self.xev_next_shot_at:
            for i in range(XEV_MAX_SHOTS):
                if self.xev_shot_active[i]:
                    continue
                self.xev_shot_x[i] = self.xev_ship_x
                self.xev_shot_y[i] = float(XEV_SHIP_Y) - 14.0
                self.xev_shot_active[i] = True
                break
            self.xev_next_shot_at = now_ms + 260 + int(self._xev_rand01() * 260.0)

        for i in range(XEV_MAX_SHOTS):
            if not self.xev_shot_active[i]:
                continue
            self.xev_shot_y[i] -= (5.0 + self.xev_speed * 2.0)
            if self.xev_shot_y[i] < float(XEV_TOP) - 6.0:
                self.xev_shot_active[i] = False
                continue
            hit = False
            for e in range(XEV_MAX_ENEMIES):
                if self.xev_enemy_y[e] < float(XEV_TOP):
                    continue
                if abs(self.xev_shot_x[i] - self.xev_enemy_x[e]) < 10.0 and abs(self.xev_shot_y[i] - self.xev_enemy_y[e]) < 9.0:
                    self._xev_spawn_explosion(self.xev_enemy_x[e], self.xev_enemy_y[e])
                    self._xev_spawn_enemy(e, False)
                    self.xev_shot_active[i] = False
                    hit = True
                    break
            if not hit and self.xev_shot_active[i]:
                painter.fillRect(int(self.xev_shot_x[i]) - 1, int(self.xev_shot_y[i]) - 3, 2, 5, QColor(*XEV_SHOT_RGB))

        self._xev_draw_ship(painter, int(self.xev_ship_x), XEV_SHIP_Y, QColor(*XEV_SHIP_RGB))

        for i in range(XEV_MAX_EXPL):
            if self.xev_expl_age[i] == 0:
                continue
            self._xev_draw_explosion(painter, int(self.xev_expl_x[i]), int(self.xev_expl_y[i]), self.xev_expl_age[i], self.xev_expl_seed[i])
            self.xev_expl_age[i] += 1
            if self.xev_expl_age[i] > 6:
                self.xev_expl_age[i] = 0

    # ----- Lighting: Classic Race（v0.5 Phase 4b。CoreS3 lightRenderClassicRace()準拠。
    # 1970年代後半風トップビューレースデモ。連続sin波の一本道・自動操舵・敵車の下降と
    # 再抽選・オイル染み/チェッカーラインの周期出現は、CoreS3のlightRenderClassicRace()を
    # そのまま移植している。黒目はRetro Raceと同じ理由で追従させない（構造的な仕様差）-----
    def _crace_rand(self) -> int:
        self.crace_rng = (self.crace_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.crace_rng

    def _crace_rand01(self) -> float:
        return (self._crace_rand() & 0xFFFF) / 65535.0

    @staticmethod
    def _crace_road_center_x(curve_smooth: float, wy: float) -> float:
        return 160.0 + math.sin(wy * 0.006) * (curve_smooth * 34.0)

    def _crace_spawn_enemy(self, i: int, initial_spread: bool) -> None:
        if initial_spread:
            self.crace_enemy_y[i] = float(CRACE_TOP) - self._crace_rand01() * 260.0
        else:
            self.crace_enemy_y[i] = float(CRACE_TOP) - (20.0 + self._crace_rand01() * 170.0)
        self.crace_enemy_lane[i] = (self._crace_rand01() * 2.0 - 1.0) * 0.6
        self.crace_enemy_speed_mul[i] = 0.85 + self._crace_rand01() * 0.35
        self.crace_enemy_avoid_side[i] = 1 if (self._crace_rand() & 1) else -1
        self.crace_enemy_color_idx[i] = self._crace_rand() % 3

    def _build_crace_table(self) -> None:
        self.crace_curve_target = self.crace_curve_smooth = 0.0
        self.crace_player_x = self.crace_player_x_target = 0.0
        self.crace_speed = self.crace_speed_target = 1.0
        self.crace_scroll_z = 0.0
        self.crace_oil_y = -(300.0 + self._crace_rand01() * 400.0)
        self.crace_oil_lane = (self._crace_rand01() * 2.0 - 1.0) * 0.6
        self.crace_checker_y = -(900.0 + self._crace_rand01() * 1100.0)
        for i in range(CRACE_MAX_ENEMIES):
            self._crace_spawn_enemy(i, True)
        self.crace_ready = True

    @staticmethod
    def _crace_draw_player_car(painter: QPainter, cx: int, cy: int, col: QColor) -> None:
        painter.fillRect(cx - 6, cy - 14, 12, 22, col)
        painter.fillRect(cx - 9, cy - 3, 18, 5, col)
        black = QColor(0, 0, 0)
        painter.fillRect(cx - 3, cy - 16, 6, 5, black)
        painter.fillRect(cx - 7, cy - 16, 3, 3, black)
        painter.fillRect(cx + 4, cy - 16, 3, 3, black)
        painter.fillRect(cx - 7, cy + 6, 3, 3, black)
        painter.fillRect(cx + 4, cy + 6, 3, 3, black)

    @staticmethod
    def _crace_draw_enemy_car(painter: QPainter, cx: int, cy: int, col: QColor) -> None:
        painter.fillRect(cx - 6, cy - 10, 12, 18, col)
        painter.fillRect(cx - 3, cy - 11, 6, 4, QColor(0, 0, 0))

    def _light_render_classicrace(self, painter: QPainter, level: float, now_ms: int, needs_init: bool) -> None:
        if not self.crace_ready:
            self._build_crace_table()
        if needs_init:
            self.crace_curve_change_at = 0
            self.crace_speed_change_at = 0

        if now_ms >= self.crace_curve_change_at:
            self.crace_curve_target = 0.25 + self._crace_rand01() * 0.75
            self.crace_curve_change_at = now_ms + 4000 + int(self._crace_rand01() * 5000.0)
        self.crace_curve_smooth += (self.crace_curve_target - self.crace_curve_smooth) * 0.015

        if now_ms >= self.crace_speed_change_at:
            self.crace_speed_target = 0.80 + self._crace_rand01() * 0.5
            self.crace_speed_change_at = now_ms + 2500 + int(self._crace_rand01() * 3500.0)
        audio_boost = 1.0 + level * 0.30
        self.crace_speed += ((self.crace_speed_target * audio_boost) - self.crace_speed) * 0.03
        if self.crace_speed < 0.35:
            self.crace_speed = 0.35
        self.crace_scroll_z += self.crace_speed * 3.0

        danger_idx = -1
        danger_y = -1.0e9
        for i in range(CRACE_MAX_ENEMIES):
            if self.crace_enemy_y[i] < CRACE_PLAYER_Y - 90.0 or self.crace_enemy_y[i] > CRACE_PLAYER_Y - 8.0:
                continue
            if abs(self.crace_enemy_lane[i] - self.crace_player_x) > 0.55:
                continue
            if self.crace_enemy_y[i] > danger_y:
                danger_y = self.crace_enemy_y[i]
                danger_idx = i
        if danger_idx >= 0:
            self.crace_player_x_target = max(-0.85, min(0.85, self.crace_player_x + self.crace_enemy_avoid_side[danger_idx] * 0.55))
        else:
            wander = math.sin(now_ms * 0.0006) * 0.20
            self.crace_player_x_target = max(-0.5, min(0.5, wander))
        self.crace_player_x += (self.crace_player_x_target - self.crace_player_x) * 0.03

        for y in range(CRACE_TOP, 240, CRACE_ROW_H):
            row_h = min(CRACE_ROW_H, 240 - y)
            wy = self.crace_scroll_z + (y - CRACE_TOP)
            center_x = self._crace_road_center_x(self.crace_curve_smooth, wy)
            cx_l = int(center_x - CRACE_ROAD_HW)
            cx_r = int(center_x + CRACE_ROAD_HW)

            stripe_idx = int(wy / 10.0)

            grass = CRACE_GRASS_A_RGB if (stripe_idx & 1) else CRACE_GRASS_B_RGB
            if cx_l > 0:
                painter.fillRect(0, y, cx_l, row_h, QColor(*grass))
            if cx_r < CANVAS_W:
                painter.fillRect(cx_r, y, CANVAS_W - cx_r, row_h, QColor(*grass))

            edge_col = QColor(*CRACE_EDGE_W_RGB)
            painter.fillRect(cx_l, y, 3, row_h, edge_col)
            painter.fillRect(cx_r - 3, y, 3, row_h, edge_col)

            road = CRACE_ROAD_A_RGB if (stripe_idx & 1) else CRACE_ROAD_B_RGB
            road_w = (cx_r - 3) - (cx_l + 3)
            if road_w > 0:
                painter.fillRect(cx_l + 3, y, road_w, row_h, QColor(*road))

            if ((stripe_idx // 2) & 1) == 0:
                painter.fillRect(int(center_x) - 2, y, 4, row_h, edge_col)

            if self.crace_checker_y <= wy < self.crace_checker_y + 10.0:
                cxx = cx_l + 3
                cw = 8
                idx2 = ((cx_l // cw) + (y // CRACE_ROW_H)) & 1
                while cxx < cx_r - 3:
                    w2 = (cx_r - 3 - cxx) if (cxx + cw > cx_r - 3) else cw
                    painter.fillRect(cxx, y, w2, row_h, QColor(0, 0, 0) if (idx2 & 1) else edge_col)
                    idx2 += 1
                    cxx += cw

            if self.crace_oil_y <= wy < self.crace_oil_y + 14.0:
                oil_cx = center_x + self.crace_oil_lane * (CRACE_ROAD_HW - 14.0)
                d = wy - self.crace_oil_y - 7.0
                half_w = int(7.0 - abs(d) * 0.7)
                if half_w > 0:
                    painter.fillRect(int(oil_cx) - half_w, y, half_w * 2, row_h, QColor(*CRACE_OIL_RGB))

        if self.crace_checker_y > 260.0:
            self.crace_checker_y = self.crace_scroll_z + 900.0 + self._crace_rand01() * 1400.0
        if self.crace_oil_y > 260.0:
            self.crace_oil_y = self.crace_scroll_z + 300.0 + self._crace_rand01() * 500.0
            self.crace_oil_lane = (self._crace_rand01() * 2.0 - 1.0) * 0.6

        for i in range(CRACE_MAX_ENEMIES):
            self.crace_enemy_y[i] += (1.2 + self.crace_enemy_speed_mul[i] * 1.4) * self.crace_speed
            if self.crace_enemy_y[i] > 255.0:
                self._crace_spawn_enemy(i, False)
                continue
            if self.crace_enemy_y[i] < float(CRACE_TOP):
                continue
            wy_e = self.crace_scroll_z + (self.crace_enemy_y[i] - CRACE_TOP)
            center_at_e = self._crace_road_center_x(self.crace_curve_smooth, wy_e)
            ex = int(center_at_e + self.crace_enemy_lane[i] * (CRACE_ROAD_HW - 14.0))
            self._crace_draw_enemy_car(painter, ex, int(self.crace_enemy_y[i]),
                                        QColor(*CRACE_ENEMY_RGB[self.crace_enemy_color_idx[i]]))

        wy_p = self.crace_scroll_z + (CRACE_PLAYER_Y - CRACE_TOP)
        center_at_p = self._crace_road_center_x(self.crace_curve_smooth, wy_p)
        px = int(center_at_p + self.crace_player_x * (CRACE_ROAD_HW - 14.0))
        self._crace_draw_player_car(painter, px, CRACE_PLAYER_Y, QColor(*CRACE_PLAYER_RGB))

    # ----- Lighting: Asteroid Field（v0.5 Phase 4b。CoreS3 lightRenderAsteroid()準拠。
    # 隕石の生成（頂点数5〜9・半径比率0.6〜1.0を出現時に1回だけ抽選し以後は等速直線運動＋
    # 等速回転）・ラップアラウンド・自機の緩慢な目標追従操舵・弾の自動/優先発射は、
    # CoreS3のlightRenderAsteroid()をそのまま移植している。かりポムの黒目は
    # DesktopにeyeOffsetX/Y相当の機構が無いため自機追従の目線演出は実装していない
    # （構造的な仕様差。隕石／自機／弾のロジックは変更していない）-----
    def _astr_rand(self) -> int:
        self.astr_rng = (self.astr_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.astr_rng

    def _astr_rand01(self) -> float:
        return (self._astr_rand() & 0xFFFF) / 65535.0

    def _astr_spawn(self, i: int) -> None:
        self.astr_x[i] = self._astr_rand01() * 320.0
        self.astr_y[i] = float(ASTR_TOP) + self._astr_rand01() * (240.0 - ASTR_TOP)
        ang = self._astr_rand01() * 6.2831853
        spd = 0.25 + self._astr_rand01() * 0.55
        self.astr_vx[i] = math.cos(ang) * spd
        self.astr_vy[i] = math.sin(ang) * spd
        self.astr_size[i] = 10.0 + self._astr_rand01() * 16.0
        self.astr_angle[i] = self._astr_rand01() * 6.2831853
        av = (self._astr_rand01() * 2.0 - 1.0) * 0.030
        if abs(av) < 0.008:
            av = -0.008 if av < 0 else 0.008
        self.astr_ang_vel[i] = av

        vc = 5 + (self._astr_rand() % 5)
        self.astr_vert_count[i] = vc
        for v in range(vc):
            self.astr_vert_r[i][v] = 0.6 + self._astr_rand01() * 0.4

        ci = self._astr_rand() % 3
        self.astr_color[i] = ASTR_NEON_RGB[ci]

    def _build_asteroid_table(self) -> None:
        for i in range(ASTR_COUNT):
            self._astr_spawn(i)

        for i in range(ASTR_STAR_COUNT):
            self.astr_star_x[i] = int(self._astr_rand01() * 320.0)
            self.astr_star_y[i] = int(ASTR_TOP + self._astr_rand01() * (240.0 - ASTR_TOP))
            self.astr_star_col[i] = ASTR_STAR_DIM_RGB if (self._astr_rand() & 1) else ASTR_STAR_FAINT_RGB

        for i in range(ASTR_CSTAR_COUNT):
            self.astr_cstar_x[i] = int(self._astr_rand01() * 320.0)
            self.astr_cstar_y[i] = int(ASTR_TOP + self._astr_rand01() * (240.0 - ASTR_TOP))
            self.astr_cstar_col[i] = ASTR_CSTAR_PALETTE_RGB[self._astr_rand() % 4]

        self.astr_craft_x = 160.0
        self.astr_craft_y = 144.0
        self.astr_craft_vx = 0.0
        self.astr_craft_vy = 0.0
        self.astr_craft_target_x = self.astr_craft_x
        self.astr_craft_target_y = self.astr_craft_y
        self.astr_craft_angle = 0.0
        self.astr_craft_retarget_at = 0

        for b in range(ASTR_BULLET_COUNT):
            self.astr_bullet_active[b] = False
        self.astr_next_fire_at = 0
        self.astr_last_fire_at = 0

        self.astr_ready = True

    @staticmethod
    def _astr_draw_craft(painter: QPainter, cx: int, cy: int, angle: float) -> None:
        tip_r, back_r, spread = 9.0, 6.0, 2.4
        col = QColor(*ASTR_CRAFT_RGB)
        tip_x = cx + int(round(math.cos(angle) * tip_r))
        tip_y = cy + int(round(math.sin(angle) * tip_r))
        bl_x = cx + int(round(math.cos(angle + spread) * back_r))
        bl_y = cy + int(round(math.sin(angle + spread) * back_r))
        br_x = cx + int(round(math.cos(angle - spread) * back_r))
        br_y = cy + int(round(math.sin(angle - spread) * back_r))
        painter.setPen(QPen(col, 1))
        painter.drawLine(tip_x, tip_y, bl_x, bl_y)
        painter.drawLine(tip_x, tip_y, br_x, br_y)
        painter.drawLine(bl_x, bl_y, br_x, br_y)

    def _astr_draw_wireframe(self, painter: QPainter, i: int) -> None:
        vc = self.astr_vert_count[i]
        cx, cy, size, ang = self.astr_x[i], self.astr_y[i], self.astr_size[i], self.astr_angle[i]
        col = QColor(*self.astr_color[i])
        painter.setPen(QPen(col, 1))
        px0 = py0 = px_first = py_first = 0
        for v in range(vc):
            a = ang + (6.2831853 * v / vc)
            r = size * self.astr_vert_r[i][v]
            px = int(round(cx + math.cos(a) * r))
            py = int(round(cy + math.sin(a) * r))
            if v == 0:
                px_first, py_first = px, py
            else:
                painter.drawLine(px0, py0, px, py)
            px0, py0 = px, py
        painter.drawLine(px0, py0, px_first, py_first)

    def _astr_fire_bullet(self) -> None:
        for b in range(ASTR_BULLET_COUNT):
            if self.astr_bullet_active[b]:
                continue
            self.astr_bullet_active[b] = True
            self.astr_bullet_x[b] = self.astr_craft_x
            self.astr_bullet_y[b] = self.astr_craft_y
            self.astr_bullet_dx[b] = math.cos(self.astr_craft_angle)
            self.astr_bullet_dy[b] = math.sin(self.astr_craft_angle)
            self.astr_bullet_dist[b] = 0.0
            return

    def _light_render_asteroid(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if not self.astr_ready:
            self._build_asteroid_table()
        # needsInit時も隕石はモード切替をまたいで漂い続けてよい（CoreS3コメント準拠。
        # 再初期化は不要）。

        painter.fillRect(0, ASTR_TOP, CANVAS_W, CANVAS_H - ASTR_TOP, QColor(0, 0, 0))

        for i in range(ASTR_CSTAR_COUNT):
            painter.fillRect(self.astr_cstar_x[i], self.astr_cstar_y[i], 1, 1, QColor(*self.astr_cstar_col[i]))

        for i in range(ASTR_STAR_COUNT):
            painter.fillRect(self.astr_star_x[i], self.astr_star_y[i], 1, 1, QColor(*self.astr_star_col[i]))

        for i in range(ASTR_COUNT):
            self.astr_x[i] += self.astr_vx[i]
            self.astr_y[i] += self.astr_vy[i]
            self.astr_angle[i] += self.astr_ang_vel[i]
            if self.astr_angle[i] > 6.2831853:
                self.astr_angle[i] -= 6.2831853
            if self.astr_angle[i] < 0.0:
                self.astr_angle[i] += 6.2831853

            m = self.astr_size[i] + 4.0
            if self.astr_x[i] < -m:
                self.astr_x[i] = 320.0 + m
            elif self.astr_x[i] > 320.0 + m:
                self.astr_x[i] = -m
            if self.astr_y[i] < ASTR_TOP - m:
                self.astr_y[i] = 240.0 + m
            elif self.astr_y[i] > 240.0 + m:
                self.astr_y[i] = ASTR_TOP - m

            self._astr_draw_wireframe(painter, i)

        if now_ms >= self.astr_craft_retarget_at:
            self.astr_craft_target_x = ASTR_CRAFT_MARGIN + self._astr_rand01() * (320.0 - ASTR_CRAFT_MARGIN * 2.0)
            self.astr_craft_target_y = float(ASTR_TOP) + ASTR_CRAFT_MARGIN + self._astr_rand01() * (240.0 - ASTR_TOP - ASTR_CRAFT_MARGIN * 2.0)
            self.astr_craft_retarget_at = now_ms + 3000 + int(self._astr_rand01() * 3000.0)

        dx = self.astr_craft_target_x - self.astr_craft_x
        dy = self.astr_craft_target_y - self.astr_craft_y
        dist = math.sqrt(dx * dx + dy * dy)
        if dist > 1.0:
            desired_vx = (dx / dist) * ASTR_CRAFT_SPEED
            desired_vy = (dy / dist) * ASTR_CRAFT_SPEED
            self.astr_craft_vx += (desired_vx - self.astr_craft_vx) * 0.03
            self.astr_craft_vy += (desired_vy - self.astr_craft_vy) * 0.03
        else:
            self.astr_craft_vx *= 0.95
            self.astr_craft_vy *= 0.95
        self.astr_craft_x += self.astr_craft_vx
        self.astr_craft_y += self.astr_craft_vy
        if abs(self.astr_craft_vx) > 0.02 or abs(self.astr_craft_vy) > 0.02:
            self.astr_craft_angle = math.atan2(self.astr_craft_vy, self.astr_craft_vx)

        self._astr_draw_craft(painter, int(round(self.astr_craft_x)), int(round(self.astr_craft_y)), self.astr_craft_angle)

        priority_fired = False
        if now_ms - self.astr_last_fire_at >= ASTR_BULLET_MIN_INTERVAL_MS:
            for i in range(ASTR_COUNT):
                dxa = self.astr_x[i] - self.astr_craft_x
                dya = self.astr_y[i] - self.astr_craft_y
                if dxa * dxa + dya * dya <= ASTR_BULLET_DETECT_R * ASTR_BULLET_DETECT_R:
                    self._astr_fire_bullet()
                    self.astr_last_fire_at = now_ms
                    self.astr_next_fire_at = now_ms + 2500 + int(self._astr_rand01() * 2500.0)
                    priority_fired = True
                    break
        if not priority_fired and now_ms >= self.astr_next_fire_at:
            self._astr_fire_bullet()
            self.astr_last_fire_at = now_ms
            self.astr_next_fire_at = now_ms + 2500 + int(self._astr_rand01() * 2500.0)

        craft_col = QColor(*ASTR_CRAFT_RGB)
        for b in range(ASTR_BULLET_COUNT):
            if not self.astr_bullet_active[b]:
                continue
            self.astr_bullet_x[b] += self.astr_bullet_dx[b] * ASTR_BULLET_SPEED
            self.astr_bullet_y[b] += self.astr_bullet_dy[b] * ASTR_BULLET_SPEED
            self.astr_bullet_dist[b] += ASTR_BULLET_SPEED
            if self.astr_bullet_dist[b] >= ASTR_BULLET_RANGE:
                self.astr_bullet_active[b] = False
                continue
            tail_x = int(round(self.astr_bullet_x[b] - self.astr_bullet_dx[b] * 6.0))
            tail_y = int(round(self.astr_bullet_y[b] - self.astr_bullet_dy[b] * 6.0))
            painter.setPen(QPen(craft_col, 1))
            painter.drawLine(tail_x, tail_y, int(round(self.astr_bullet_x[b])), int(round(self.astr_bullet_y[b])))

    # ----- Lighting: Tempest Tunnel（v0.5 Phase 4b。CoreS3 lightRenderTunnel()準拠。
    # 実dt駆動のリング深度・回転・脈動・色相変化、発光体の奥→手前フロー、自機の弧内操舵と
    # 回避バイアス、弾の自動/優先発射は、CoreS3のlightRenderTunnel()をそのまま移植している。
    # 黒目はAsteroid Fieldと同じ理由で実装していない（構造的な仕様差）-----
    def _tun_rand(self) -> int:
        self.tun_rng = (self.tun_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.tun_rng

    def _tun_rand01(self) -> float:
        return (self._tun_rand() & 0xFFFF) / 65535.0

    @staticmethod
    def _tun_hue_to_rgb(hue_deg: float):
        hue_deg = math.fmod(hue_deg, 360.0)
        if hue_deg < 0.0:
            hue_deg += 360.0
        h = hue_deg / 60.0
        hi = int(h) % 6
        f = h - int(h)
        q = 255.0 * (1.0 - f)
        t = 255.0 * f
        if hi == 0:
            r, g, b = 255.0, t, 0.0
        elif hi == 1:
            r, g, b = q, 255.0, 0.0
        elif hi == 2:
            r, g, b = 0.0, 255.0, t
        elif hi == 3:
            r, g, b = 0.0, q, 255.0
        elif hi == 4:
            r, g, b = t, 0.0, 255.0
        else:
            r, g, b = 255.0, 0.0, q
        return (int(r), int(g), int(b))

    def _build_tunnel_table(self) -> None:
        self.tun_verts = TUN_MIN_VERTS + (self._tun_rand() % (TUN_MAX_VERTS - TUN_MIN_VERTS + 1))
        self.tun_rot_dir = 1 if (self._tun_rand() & 1) else -1
        self.tun_rot_angle = self._tun_rand01() * 6.2831853
        self.tun_flow_phase = self._tun_rand01() * TUN_RINGS
        self.tun_pulse_phase = self._tun_rand01() * 6.2831853
        self.tun_hue_deg = self._tun_rand01() * 360.0
        self.tun_last_ms = 0

        for i in range(TUN_CSTAR_COUNT):
            self.tun_cstar_x[i] = int(self._tun_rand01() * 320.0)
            self.tun_cstar_y[i] = int(TUN_TOP + self._tun_rand01() * (240.0 - TUN_TOP))
            self.tun_cstar_col[i] = TUN_CSTAR_PALETTE_RGB[self._tun_rand() % 4]

        self.tun_craft_angle = TUN_CRAFT_ARC_CENTER
        self.tun_craft_target_angle = TUN_CRAFT_ARC_CENTER
        self.tun_craft_avoid_bias = 0.0
        self.tun_craft_retarget_at = 0

        for e in range(TUN_ENEMY_COUNT):
            self.tun_enemy_depth[e] = self._tun_rand01() * TUN_RINGS
            self.tun_enemy_angle[e] = self._tun_rand01() * 6.2831853
            self.tun_enemy_speed[e] = TUN_RINGS / (2500.0 + self._tun_rand01() * 2000.0)
            self.tun_enemy_type[e] = self._tun_rand() % 2

        for b in range(TUN_BULLET_COUNT):
            self.tun_bullet_active[b] = False
        self.tun_next_fire_at = 0
        self.tun_last_fire_at = 0

        self.tun_ready = True

    @staticmethod
    def _tun_draw_craft(painter: QPainter, cx: int, cy: int, angle_to_center: float, col: QColor) -> None:
        tip_r, back_r, spread = 9.0, 6.0, 2.3
        tip_x = cx + int(round(math.cos(angle_to_center) * tip_r))
        tip_y = cy + int(round(math.sin(angle_to_center) * tip_r))
        bl_x = cx + int(round(math.cos(angle_to_center + spread) * back_r))
        bl_y = cy + int(round(math.sin(angle_to_center + spread) * back_r))
        br_x = cx + int(round(math.cos(angle_to_center - spread) * back_r))
        br_y = cy + int(round(math.sin(angle_to_center - spread) * back_r))
        painter.setPen(QPen(col, 1))
        painter.drawLine(tip_x, tip_y, bl_x, bl_y)
        painter.drawLine(tip_x, tip_y, br_x, br_y)
        painter.drawLine(bl_x, bl_y, br_x, br_y)

    @staticmethod
    def _tun_draw_enemy(painter: QPainter, cx: int, cy: int, kind: int, col: QColor) -> None:
        painter.setPen(QPen(col, 1))
        if kind == 0:
            painter.drawLine(cx, cy - 5, cx + 5, cy)
            painter.drawLine(cx + 5, cy, cx, cy + 5)
            painter.drawLine(cx, cy + 5, cx - 5, cy)
            painter.drawLine(cx - 5, cy, cx, cy - 5)
        else:
            painter.drawLine(cx, cy - 6, cx + 5, cy + 4)
            painter.drawLine(cx + 5, cy + 4, cx - 5, cy + 4)
            painter.drawLine(cx - 5, cy + 4, cx, cy - 6)

    def _tun_fire_bullet(self, angle: float) -> None:
        for b in range(TUN_BULLET_COUNT):
            if self.tun_bullet_active[b]:
                continue
            self.tun_bullet_active[b] = True
            self.tun_bullet_angle[b] = angle
            self.tun_bullet_radius[b] = TUN_CRAFT_RADIUS
            return

    def _light_render_tunnel(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if not self.tun_ready:
            self._build_tunnel_table()
        dt = 0.0 if (self.tun_last_ms == 0 or needs_init) else float(now_ms - self.tun_last_ms)
        if dt > 200.0:
            dt = 200.0
        self.tun_last_ms = now_ms

        self.tun_rot_angle += TUN_ROT_SPEED * self.tun_rot_dir * dt
        self.tun_flow_phase += TUN_FLOW_SPEED * dt
        if self.tun_flow_phase >= TUN_RINGS:
            self.tun_flow_phase = math.fmod(self.tun_flow_phase, float(TUN_RINGS))
        self.tun_pulse_phase += TUN_PULSE_SPEED * dt
        if self.tun_pulse_phase > 6.2831853:
            self.tun_pulse_phase -= 6.2831853
        self.tun_hue_deg += TUN_HUE_SPEED * dt
        if self.tun_hue_deg >= 360.0:
            self.tun_hue_deg = math.fmod(self.tun_hue_deg, 360.0)

        painter.fillRect(0, TUN_TOP, CANVAS_W, CANVAS_H - TUN_TOP, QColor(0, 0, 0))

        for i in range(TUN_CSTAR_COUNT):
            painter.fillRect(self.tun_cstar_x[i], self.tun_cstar_y[i], 1, 1, QColor(*self.tun_cstar_col[i]))

        pulse = 1.0 + math.sin(self.tun_pulse_phase) * TUN_PULSE_AMPL

        d = [0.0] * TUN_RINGS
        radius = [0.0] * TUN_RINGS
        col = [(0, 0, 0)] * TUN_RINGS
        vx = [[0] * TUN_MAX_VERTS for _ in range(TUN_RINGS)]
        vy = [[0] * TUN_MAX_VERTS for _ in range(TUN_RINGS)]

        for i in range(TUN_RINGS):
            depth = math.fmod(self.tun_flow_phase + i, float(TUN_RINGS))
            t = depth / float(TUN_RINGS)
            far3 = (1.0 - t) ** 3
            d[i] = depth
            radius[i] = (TUN_MIN_R + (TUN_MAX_R - TUN_MIN_R) * far3) * pulse
            col[i] = self._tun_hue_to_rgb(self.tun_hue_deg + i * TUN_HUE_STEP)

            ring_angle = self.tun_rot_angle + i * TUN_RING_TWIST
            for v in range(self.tun_verts):
                a = ring_angle + 6.2831853 * v / self.tun_verts
                vx[i][v] = int(round(TUN_CENTER_X + math.cos(a) * radius[i]))
                vy[i][v] = int(round(TUN_CENTER_Y + math.sin(a) * radius[i]))

        for i in range(TUN_RINGS):
            ring_col = QColor(*col[i])
            painter.setPen(QPen(ring_col, 1))
            for v in range(self.tun_verts):
                vn = 0 if (v + 1 == self.tun_verts) else (v + 1)
                painter.drawLine(vx[i][v], vy[i][v], vx[i][vn], vy[i][vn])

        for i in range(TUN_RINGS):
            j = 0 if (i + 1 == TUN_RINGS) else (i + 1)
            if d[j] < d[i]:
                continue
            ring_col = QColor(*col[i])
            painter.setPen(QPen(ring_col, 1))
            for v in range(self.tun_verts):
                painter.drawLine(vx[i][v], vy[i][v], vx[j][v], vy[j][v])

        enemy_t = [0.0] * TUN_ENEMY_COUNT
        for e in range(TUN_ENEMY_COUNT):
            self.tun_enemy_depth[e] -= self.tun_enemy_speed[e] * dt
            if self.tun_enemy_depth[e] <= 0.0:
                self.tun_enemy_depth[e] = float(TUN_RINGS)
                self.tun_enemy_angle[e] = self._tun_rand01() * 6.2831853
                self.tun_enemy_speed[e] = TUN_RINGS / (2500.0 + self._tun_rand01() * 2000.0)
                self.tun_enemy_type[e] = self._tun_rand() % 2
            t = 1.0 - (self.tun_enemy_depth[e] / float(TUN_RINGS))
            far3 = (1.0 - t) ** 3
            er = TUN_MIN_R + (TUN_MAX_R - TUN_MIN_R) * far3
            enemy_t[e] = t
            ex = TUN_CENTER_X + math.cos(self.tun_enemy_angle[e]) * er
            ey = TUN_CENTER_Y + math.sin(self.tun_enemy_angle[e]) * er
            self._tun_draw_enemy(painter, int(round(ex)), int(round(ey)), self.tun_enemy_type[e], QColor(*TUN_ENEMY_RGB))

        if now_ms >= self.tun_craft_retarget_at:
            self.tun_craft_target_angle = TUN_CRAFT_ARC_CENTER + (self._tun_rand01() * 2.0 - 1.0) * TUN_CRAFT_ARC_HALF
            self.tun_craft_retarget_at = now_ms + 3000 + int(self._tun_rand01() * 3000.0)
        steer = max(0.0, min(1.0, dt * TUN_CRAFT_STEER_RATE))
        self.tun_craft_angle += (self.tun_craft_target_angle - self.tun_craft_angle) * steer

        avoid_target = 0.0
        for e in range(TUN_ENEMY_COUNT):
            if enemy_t[e] < TUN_ENEMY_NEAR_T:
                continue
            diff = self.tun_craft_angle - self.tun_enemy_angle[e]
            while diff > 3.1415927:
                diff -= 6.2831853
            while diff < -3.1415927:
                diff += 6.2831853
            if abs(diff) < 0.5:
                avoid_target += (1.0 if diff >= 0.0 else -1.0) * 0.18
        avoid_target = max(-0.25, min(0.25, avoid_target))
        bias_steer = max(0.0, min(1.0, dt * 0.0015))
        self.tun_craft_avoid_bias += (avoid_target - self.tun_craft_avoid_bias) * bias_steer

        craft_eff_angle = self.tun_craft_angle + self.tun_craft_avoid_bias
        craft_x = TUN_CENTER_X + math.cos(craft_eff_angle) * TUN_CRAFT_RADIUS
        craft_y = TUN_CENTER_Y + math.sin(craft_eff_angle) * TUN_CRAFT_RADIUS
        self._tun_draw_craft(painter, int(round(craft_x)), int(round(craft_y)), craft_eff_angle + 3.1415927, QColor(*TUN_CRAFT_RGB))

        priority_fired = False
        if now_ms - self.tun_last_fire_at >= TUN_BULLET_MIN_INTERVAL_MS:
            for e in range(TUN_ENEMY_COUNT):
                if enemy_t[e] < TUN_BULLET_DETECT_T:
                    continue
                diff = craft_eff_angle - self.tun_enemy_angle[e]
                while diff > 3.1415927:
                    diff -= 6.2831853
                while diff < -3.1415927:
                    diff += 6.2831853
                if abs(diff) < TUN_BULLET_DETECT_ANGLE:
                    self._tun_fire_bullet(craft_eff_angle)
                    self.tun_last_fire_at = now_ms
                    self.tun_next_fire_at = now_ms + 2500 + int(self._tun_rand01() * 2500.0)
                    priority_fired = True
                    break
        if not priority_fired and now_ms >= self.tun_next_fire_at:
            self._tun_fire_bullet(craft_eff_angle)
            self.tun_last_fire_at = now_ms
            self.tun_next_fire_at = now_ms + 2500 + int(self._tun_rand01() * 2500.0)

        craft_col = QColor(*TUN_CRAFT_RGB)
        for b in range(TUN_BULLET_COUNT):
            if not self.tun_bullet_active[b]:
                continue
            self.tun_bullet_radius[b] -= TUN_BULLET_SPEED_PX_MS * dt
            if self.tun_bullet_radius[b] <= TUN_MIN_R:
                self.tun_bullet_active[b] = False
                continue
            bx = TUN_CENTER_X + math.cos(self.tun_bullet_angle[b]) * self.tun_bullet_radius[b]
            by = TUN_CENTER_Y + math.sin(self.tun_bullet_angle[b]) * self.tun_bullet_radius[b]
            tail_r = min(self.tun_bullet_radius[b] + 8.0, TUN_CRAFT_RADIUS)
            tx = TUN_CENTER_X + math.cos(self.tun_bullet_angle[b]) * tail_r
            ty = TUN_CENTER_Y + math.sin(self.tun_bullet_angle[b]) * tail_r
            painter.setPen(QPen(craft_col, 1))
            painter.drawLine(int(round(tx)), int(round(ty)), int(round(bx)), int(round(by)))

    # ----- Lighting: PAC-MAN Arcade（v0.5 Phase 5A。CoreS3 lightRenderPacman()準拠）-----
    def _pac_rand(self) -> int:
        """CoreS3 pacRandNext()と同じxorshift32（このLighting専用。共通化はしない）。"""
        x = self.pac_rng & 0xFFFFFFFF
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        self.pac_rng = x
        return x

    @staticmethod
    def _pac_cell_xy(col: int, row: int) -> tuple[int, int]:
        return col * PAC_CELL + PAC_CELL // 2, PAC_TOP + row * PAC_CELL + PAC_CELL // 2

    @staticmethod
    def _pac_wrap_col(col: int) -> int:
        return (col % PAC_COLS + PAC_COLS) % PAC_COLS

    def _pac_cell_open(self, row: int, col: int) -> bool:
        """指定セルが通路として移動可能か（PAC_MAZE正本を参照）。PAC_TUNNEL_ROWだけは
        左右端の外側も折り返して通路とみなす。他の行は画面端の外側を通路とみなさない。"""
        if row < 0 or row >= PAC_ROWS:
            return False
        if col < 0 or col >= PAC_COLS:
            if row != PAC_TUNNEL_ROW:
                return False
            col = self._pac_wrap_col(col)
        return PAC_MAZE[row][col] == 0

    def _pac_choose_dir(self, row: int, col: int, cur_dx: int, cur_dy: int) -> tuple[int, int]:
        """現在セル・現在方向から次の移動方向を選ぶ（経路探索なしの最小限ロジック）。
        (a) 直進が可能ならそのまま直進 (b) 不可能なら後戻り以外の通路からランダムに1つ選ぶ
        (c) 行き止まりなら後戻り。CoreS3 pacChooseDir()と同一ロジック。"""
        other: list[tuple[int, int]] = []
        straight_ok = False
        for dx, dy in PAC_DIRS:
            if not self._pac_cell_open(row + dy, col + dx):
                continue
            if dx == cur_dx and dy == cur_dy:
                straight_ok = True
            if not (dx == -cur_dx and dy == -cur_dy):
                other.append((dx, dy))
        if straight_ok:
            return cur_dx, cur_dy
        if other:
            pick = self._pac_rand() % len(other)
            return other[pick]
        return -cur_dx, -cur_dy

    def _pac_step_actor(
        self, col: int, row: int, progress: float, dir_x: int, dir_y: int, speed: float
    ) -> tuple[int, int, float, int, int]:
        """アクター（自機／オバケ）を1体ぶん1描画あたり進める。CoreS3 pacStepActor()と同じく、
        セル中心から隣接セル中心へprogress(0..1)で滑走し、遷移完了の瞬間だけ方向を選び直す。
        CoreS3はポインタ引数で書き換えるが、Desktopでは新しい状態をタプルで返し呼び出し側が
        代入する（ロジックは同一、言語の書き方の違いのみ）。"""
        progress += speed
        while progress >= 1.0:
            progress -= 1.0
            nc, nr = col + dir_x, row + dir_y
            if nr == PAC_TUNNEL_ROW:
                nc = self._pac_wrap_col(nc)
            col, row = nc, nr
            dir_x, dir_y = self._pac_choose_dir(row, col, dir_x, dir_y)
        return col, row, progress, dir_x, dir_y

    def _pac_reset_dots(self) -> None:
        for r in range(PAC_ROWS):
            for c in range(PAC_COLS):
                self.pac_dot_eaten[r][c] = False

    def _pac_reset_actors(self) -> None:
        self.pac_col, self.pac_row, self.pac_progress = 1, 1, 0.0
        self.pac_dir_x, self.pac_dir_y = 0, 0
        self.pac_dir_x, self.pac_dir_y = self._pac_choose_dir(self.pac_row, self.pac_col, self.pac_dir_x, self.pac_dir_y)
        for g in range(PAC_GHOSTS):
            self.pac_ghost_col[g] = PAC_GHOST_SPAWN_COL[g]
            self.pac_ghost_row[g] = PAC_GHOST_SPAWN_ROW[g]
            self.pac_ghost_progress[g] = 0.0
            self.pac_ghost_dir_x[g], self.pac_ghost_dir_y[g] = 0, 0
            self.pac_ghost_dir_x[g], self.pac_ghost_dir_y[g] = self._pac_choose_dir(
                self.pac_ghost_row[g], self.pac_ghost_col[g], self.pac_ghost_dir_x[g], self.pac_ghost_dir_y[g]
            )

    def _pac_init_all(self) -> None:
        self._pac_reset_dots()
        self._pac_reset_actors()
        self.pac_reset_at = 0
        self.pac_ready = True

    def _pac_draw_ghost(self, painter: QPainter, cx: int, cy: int, color_rgb, dir_x: int) -> None:
        r = 7
        body_col = QColor(*color_rgb)
        painter.fillRect(cx - r, cy - 2, r * 2, r + 4, body_col)  # 胴体（角ばった下半分）
        self._fill_circle(painter, cx, cy - 1, r, body_col)  # 丸い頭
        black = QColor(0, 0, 0)
        for k in range(-2, 3):  # すそのギザギザ（背景の黒で三角に切り欠く）
            nx = cx + k * (r // 2)
            self._poly(painter, [(nx - r // 4, cy + r + 2), (nx + r // 4, cy + r + 2), (nx, cy + r - 2)], black)
        ex = dir_x * 2  # 目（進行方向へ寄せる）
        white = QColor(255, 255, 255)
        pupil = QColor(*PAC_GHOST_PUPIL_RGB)
        self._fill_circle(painter, cx - 3 + ex, cy - 2, 3, white)
        self._fill_circle(painter, cx + 3 + ex, cy - 2, 3, white)
        self._fill_circle(painter, cx - 3 + ex * 2, cy - 2, 1, pupil)
        self._fill_circle(painter, cx + 3 + ex * 2, cy - 2, 1, pupil)

    def _light_render_pacman(self, painter: QPainter, level: float, now_ms: int, needs_init: bool) -> None:
        if not self.pac_ready:
            self._pac_init_all()
        if needs_init:
            self._pac_reset_actors()  # 短時間の再有効化では迷路・ドット進捗は消さず、動きだけ揃え直す
        audio_boost = 1.0 + level * 0.30

        wall_col = QColor(*PAC_WALL_COL_RGB)
        wall_edge = QColor(*PAC_WALL_EDGE_RGB)
        dot_col = QColor(*PAC_DOT_COL_RGB)
        pacman_col = QColor(*PAC_PACMAN_COL_RGB)
        black = QColor(0, 0, 0)

        painter.fillRect(0, PAC_TOP, CANVAS_W, CANVAS_H - PAC_TOP, black)

        # ── 壁タイル（PAC_MAZEを正本として、CoreS3と同じ二重矩形の描き方をそのまま使う）──
        for row in range(PAC_ROWS):
            for col in range(PAC_COLS):
                if PAC_MAZE[row][col] == 0:
                    continue
                x, y = col * PAC_CELL, PAC_TOP + row * PAC_CELL
                painter.fillRect(x + 1, y + 1, PAC_CELL - 2, PAC_CELL - 2, wall_edge)
                painter.fillRect(x + 2, y + 2, PAC_CELL - 6, PAC_CELL - 6, wall_col)

        # ── ドット／パワーエサ（通路セルすべてに配置。壁セルには描かない）──
        blink = ((now_ms // 260) % 2) == 0
        for row in range(PAC_ROWS):
            for col in range(PAC_COLS):
                if PAC_MAZE[row][col] != 0:
                    continue
                is_power = any(PAC_POWER_COL[k] == col and PAC_POWER_ROW[k] == row for k in range(PAC_POWER_COUNT))
                cx, cy = self._pac_cell_xy(col, row)
                if is_power:
                    if not blink or self.pac_dot_eaten[row][col]:
                        continue
                    self._fill_circle(painter, cx, cy, 4, dot_col)
                else:
                    if self.pac_dot_eaten[row][col]:
                        continue
                    self._fill_circle(painter, cx, cy, 2, dot_col)

        # ── 自機（PAC_MAZEの通路だけを、隣接セル判定でグリッド移動）──
        self.pac_col, self.pac_row, self.pac_progress, self.pac_dir_x, self.pac_dir_y = self._pac_step_actor(
            self.pac_col, self.pac_row, self.pac_progress, self.pac_dir_x, self.pac_dir_y, PAC_SPEED * audio_boost
        )
        self.pac_dot_eaten[self.pac_row][self.pac_col] = True  # 現在セルのドットを消費

        x0, y0 = self._pac_cell_xy(self.pac_col, self.pac_row)
        x1, y1 = self._pac_cell_xy(self.pac_col + self.pac_dir_x, self.pac_row + self.pac_dir_y)
        pac_x = x0 + int((x1 - x0) * self.pac_progress)
        pac_y = y0 + int((y1 - y0) * self.pac_progress)

        mouth_t = (math.sin(now_ms * 0.012) + 1.0) * 0.5  # 0..1
        mouth_deg = 8 + int(mouth_t * 30.0)  # 8〜38度

        self._fill_circle(painter, pac_x, pac_y, 7, pacman_col)
        if self.pac_dir_x != 0 or self.pac_dir_y != 0:
            base_ang = math.atan2(self.pac_dir_y, self.pac_dir_x)
            half = mouth_deg * math.pi / 180.0
            ax = pac_x + math.cos(base_ang + half) * 10.0
            ay = pac_y + math.sin(base_ang + half) * 10.0
            bx = pac_x + math.cos(base_ang - half) * 10.0
            by = pac_y + math.sin(base_ang - half) * 10.0
            self._poly(painter, [(pac_x, pac_y), (ax, ay), (bx, by)], black)

        # ── 敵（オバケ）3体：3体とも同じグリッド移動ロジックで独立に動く ──
        for g in range(PAC_GHOSTS):
            (
                self.pac_ghost_col[g],
                self.pac_ghost_row[g],
                self.pac_ghost_progress[g],
                self.pac_ghost_dir_x[g],
                self.pac_ghost_dir_y[g],
            ) = self._pac_step_actor(
                self.pac_ghost_col[g],
                self.pac_ghost_row[g],
                self.pac_ghost_progress[g],
                self.pac_ghost_dir_x[g],
                self.pac_ghost_dir_y[g],
                PAC_GHOST_SPEED[g] * audio_boost,
            )
            self.pac_dot_eaten[self.pac_ghost_row[g]][self.pac_ghost_col[g]] = True  # オバケも通過した通路のドットを消費

            gx0, gy0 = self._pac_cell_xy(self.pac_ghost_col[g], self.pac_ghost_row[g])
            gx1, gy1 = self._pac_cell_xy(
                self.pac_ghost_col[g] + self.pac_ghost_dir_x[g], self.pac_ghost_row[g] + self.pac_ghost_dir_y[g]
            )
            gx = gx0 + int((gx1 - gx0) * self.pac_ghost_progress[g])
            gy = gy0 + int((gy1 - gy0) * self.pac_ghost_progress[g])
            gdir_x = 1 if self.pac_ghost_dir_x[g] > 0 else (-1 if self.pac_ghost_dir_x[g] < 0 else 0)
            self._pac_draw_ghost(painter, gx, gy, PAC_GHOST_COL_RGB[g], gdir_x)

        # ── ドット全消化 or 一定時間経過で無限ループのため復活 ──
        all_eaten = True
        for row in range(PAC_ROWS):
            if not all_eaten:
                break
            for col in range(PAC_COLS):
                if PAC_MAZE[row][col] == 0 and not self.pac_dot_eaten[row][col]:
                    all_eaten = False
                    break
        if self.pac_reset_at == 0:
            self.pac_reset_at = now_ms + PAC_RESET_INTERVAL_MS
        if all_eaten or now_ms >= self.pac_reset_at:
            self._pac_reset_dots()
            self.pac_reset_at = now_ms + PAC_RESET_INTERVAL_MS

    # ----- Lighting: Fighter Duel（Phase 5B。CoreS3 lightRenderStreetFighter()準拠）-----
    def _sf_rand(self) -> int:
        self.sf_rng = (self.sf_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.sf_rng

    def _sf_rand_range(self, lo: int, hi: int) -> int:
        return lo + (self._sf_rand() % (hi - lo + 1))

    def _sf_reset_fighter(self, i: int, x: float, facing: int, now_ms: int) -> None:
        self.sf_x[i] = x
        self.sf_facing[i] = facing
        self.sf_state[i] = SF_IDLE
        self.sf_state_until[i] = now_ms + 400
        self.sf_impact_done[i] = False
        self.sf_health[i] = 100.0
        self.sf_hit_flash[i] = 0
        self.sf_jump_phase[i] = 0.0
        self.sf_proj_active[i] = False

    def _sf_init_all(self, now_ms: int) -> None:
        self._sf_reset_fighter(0, SF_STAGE_L + 40.0, 1, now_ms)
        self._sf_reset_fighter(1, SF_STAGE_R - 40.0, -1, now_ms)
        self.sf_round = 1
        self.sf_timer = 60
        self.sf_timer_tick_at = now_ms + 1000
        self.sf_ko_active = False
        self.sf_ready = True

    def _sf_new_round(self, now_ms: int) -> None:
        self._sf_reset_fighter(0, SF_STAGE_L + 40.0, 1, now_ms)
        self._sf_reset_fighter(1, SF_STAGE_R - 40.0, -1, now_ms)
        self.sf_round += 1
        self.sf_timer = 60
        self.sf_timer_tick_at = now_ms + 1000
        self.sf_ko_active = False

    def _sf_apply_hit(self, target: int, dmg: float, from_dir: int, now_ms: int) -> None:
        self.sf_health[target] -= dmg
        self.sf_hit_flash[target] = 6
        self.sf_state[target] = SF_HIT
        self.sf_state_until[target] = now_ms + 260
        self.sf_x[target] += float(from_dir) * 10.0
        if self.sf_x[target] < SF_STAGE_L:
            self.sf_x[target] = SF_STAGE_L
        if self.sf_x[target] > SF_STAGE_R:
            self.sf_x[target] = SF_STAGE_R
        if self.sf_health[target] <= 0.0:
            self.sf_health[target] = 0.0
            self.sf_ko_active = True
            self.sf_ko_until = now_ms + 2600

    # 次の行動を距離ベースの重み付け抽選で決める（CoreS3 sfDecideNext()の抽選テーブルそのまま）
    def _sf_decide_next(self, i: int, dist: float, now_ms: int) -> None:
        r = self._sf_rand_range(0, 99)
        if dist > 110:
            st = SF_WALK_F if r < 80 else (SF_SPECIAL if r < 92 else SF_JUMP)
        elif dist > 60:
            st = (SF_WALK_F if r < 45 else
                  SF_PUNCH if r < 65 else
                  SF_KICK if r < 82 else
                  SF_SPECIAL if r < 92 else SF_JUMP)
        else:
            st = (SF_PUNCH if r < 35 else
                  SF_KICK if r < 60 else
                  SF_WALK_B if r < 78 else
                  SF_CROUCH if r < 90 else SF_JUMP)
        dur = {
            SF_WALK_F: 260, SF_WALK_B: 240, SF_CROUCH: 380, SF_PUNCH: 260,
            SF_KICK: 320, SF_JUMP: 480, SF_SPECIAL: 300,
        }.get(st, 300)
        self.sf_state[i] = st
        self.sf_state_until[i] = now_ms + dur
        self.sf_impact_done[i] = False

    def _sf_update_projectile(self, i: int, now_ms: int) -> None:
        other = 1 - i
        if not self.sf_proj_active[i]:
            return
        self.sf_proj_x[i] += self.sf_proj_dir[i] * 6.5
        if abs(self.sf_proj_x[i] - self.sf_x[other]) < 14.0 and not self.sf_ko_active:
            self._sf_apply_hit(other, 9.0 + float(self._sf_rand_range(0, 4)), self.sf_proj_dir[i], now_ms)
            self.sf_proj_active[i] = False
            return
        if self.sf_proj_x[i] < SF_STAGE_L - 10 or self.sf_proj_x[i] > SF_STAGE_R + 10:
            self.sf_proj_active[i] = False

    def _sf_draw_bar(self, painter: QPainter, x: int, pct: float, right_align: bool) -> None:
        col = SF_HP_GREEN_RGB if pct > 50 else (SF_HP_ORANGE_RGB if pct > 20 else SF_HP_RED_RGB)
        w = 110
        painter.setPen(QColor(255, 255, 255))
        painter.setBrush(Qt.NoBrush)
        painter.drawRect(x, SF_TOP + 6, w, 12)
        fw = int(w * (pct / 100.0))
        if fw > 0:
            fx = (x + w - fw) if right_align else x
            painter.fillRect(fx + 1, SF_TOP + 7, fw - 2, 10, QColor(*col))

    # 2026/07/27改訂（CoreS3）のポーズ描画をそのまま踏襲：常時ファイティングスタンス、
    # Fighter A=白系道着+赤鉢巻、Fighter B=濃色道着+とげ状黒髪シルエットで区別する。
    def _sf_draw_fighter(self, painter: QPainter, i: int, gi_col_rgb, skin_col_rgb, is_fighter_a: bool) -> None:
        f_state = self.sf_state[i]
        f_facing = self.sf_facing[i]
        cx = int(round(self.sf_x[i]))
        base_y = SF_GROUND_Y
        crouch_extra = 8 if f_state == SF_CROUCH else 0
        jump_y = int(math.sin(self.sf_jump_phase[i] * 3.14159) * 34.0) if f_state == SF_JUMP else 0
        cy = base_y - jump_y
        gi_col = QColor(255, 255, 255) if self.sf_hit_flash[i] > 0 else QColor(*gi_col_rgb)
        skin = QColor(*skin_col_rgb)
        fwd = f_facing

        belt_col = QColor(*SF_BELT_RGB)
        headband_col = QColor(*SF_HEADBAND_RGB)
        hair_col = QColor(*SF_HAIR_RGB)

        leg_h = 16 - crouch_extra // 2
        torso_h = (22 if is_fighter_a else 24) - crouch_extra
        torso_w = 16 if is_fighter_a else 18
        head_r = 8

        front_leg_x = cx + fwd * 5
        back_leg_x = cx - fwd * 6
        foot_y = cy - 3

        if f_state == SF_KICK:
            reach = fwd * 26
            painter.fillRect(back_leg_x - 3, cy - leg_h, 7, leg_h, gi_col)
            painter.fillRect(back_leg_x - 4, foot_y, 8, 3, skin)
            painter.fillRect(cx + reach - 5, cy - leg_h - 6, 11, 6, skin)
        elif f_state == SF_JUMP:
            painter.fillRect(cx - 8, cy - leg_h + 5, 7, leg_h - 5, gi_col)
            painter.fillRect(cx + 1, cy - leg_h + 5, 7, leg_h - 5, gi_col)
            painter.fillRect(cx - 8, cy - 1, 7, 3, skin)
            painter.fillRect(cx + 1, cy - 1, 7, 3, skin)
        else:
            painter.fillRect(back_leg_x - 3, cy - leg_h, 7, leg_h, gi_col)
            painter.fillRect(front_leg_x - 3, cy - leg_h, 7, leg_h, gi_col)
            painter.fillRect(back_leg_x - 4, foot_y, 8, 3, skin)
            painter.fillRect(front_leg_x - 4, foot_y, 8, 3, skin)

        torso_y = cy - leg_h - torso_h
        torso_x = cx - torso_w // 2 + fwd * 2
        painter.fillRect(torso_x, torso_y, torso_w, torso_h, gi_col)
        painter.fillRect(torso_x - 1, torso_y + torso_h - 5, torso_w + 2, 5, belt_col)
        painter.fillRect(torso_x + torso_w // 2 - 3, torso_y, 6, 4, skin)

        torso_center_x = torso_x + torso_w // 2
        guard_y = torso_y - 2

        if f_state == SF_PUNCH:
            reach = fwd * 24
            painter.fillRect(torso_center_x + reach - 5, guard_y + 2, 10, 7, skin)
            painter.fillRect(torso_center_x - fwd * 8 - 3, guard_y + 8, 7, 8, gi_col)
        elif f_state == SF_SPECIAL:
            reach = fwd * 16
            painter.fillRect(torso_center_x + reach - 5, guard_y, 10, 6, skin)
            painter.fillRect(torso_center_x + reach - 5, guard_y + 8, 10, 6, skin)
        elif f_state == SF_HIT:
            painter.fillRect(torso_center_x - fwd * 14 - 4, guard_y + 4, 8, 8, skin)
            painter.fillRect(torso_center_x + fwd * 14 - 4, guard_y + 2, 8, 8, skin)
        else:
            painter.fillRect(torso_center_x + fwd * 7 - 3, guard_y, 7, 8, skin)
            painter.fillRect(torso_center_x - fwd * 7 - 3, guard_y + 5, 7, 8, skin)

        head_x = torso_center_x + (-fwd * 6 if f_state == SF_HIT else 0)
        head_y = torso_y - head_r + 1
        self._fill_circle(painter, head_x, head_y, head_r, skin)

        if is_fighter_a:
            painter.fillRect(head_x - head_r, head_y - head_r + 2, head_r * 2, 4, headband_col)
            painter.fillRect(head_x - fwd * (head_r + 5), head_y - head_r + 2, 6, 3, headband_col)
        else:
            self._poly(painter, [(head_x - 6, head_y - head_r + 3), (head_x, head_y - head_r - 6),
                                  (head_x + 2, head_y - head_r + 2)], hair_col)
            self._poly(painter, [(head_x - 1, head_y - head_r + 2), (head_x + 5, head_y - head_r - 6),
                                  (head_x + 7, head_y - head_r + 3)], hair_col)

    def _light_render_streetfighter(self, painter: QPainter, level: float, now_ms: int, needs_init: bool) -> None:
        if not self.sf_ready:
            self._sf_init_all(now_ms)
        # ── ロジック更新：CoreS3の実効呼び出し周期(LIGHT_FRAME_UPDATE_MS)に合わせてのみ進める ──
        if needs_init or (now_ms - self.sf_last_update_ms) >= LIGHT_FRAME_UPDATE_MS:
            self.sf_last_update_ms = now_ms
            audio_boost = 1.0 + level * 0.25
            now = now_ms

            if not self.sf_ko_active and now >= self.sf_timer_tick_at:
                self.sf_timer_tick_at = now + 1000
                if self.sf_timer > 0:
                    self.sf_timer -= 1
                if self.sf_timer <= 0:
                    self.sf_ko_active = True
                    self.sf_ko_until = now + 2200

            if self.sf_ko_active:
                if now >= self.sf_ko_until:
                    self._sf_new_round(now)
            else:
                dist = abs(self.sf_x[0] - self.sf_x[1])
                for i in (0, 1):
                    other = 1 - i
                    self.sf_facing[i] = 1 if self.sf_x[other] >= self.sf_x[i] else -1
                    if self.sf_hit_flash[i] > 0:
                        self.sf_hit_flash[i] -= 1

                    if now >= self.sf_state_until[i]:
                        if self.sf_state[i] == SF_SPECIAL and not self.sf_impact_done[i]:
                            self.sf_proj_active[i] = True
                            self.sf_proj_x[i] = self.sf_x[i] + self.sf_facing[i] * 16.0
                            self.sf_proj_dir[i] = self.sf_facing[i]
                        self._sf_decide_next(i, dist, now)

                    st = self.sf_state[i]
                    if st == SF_WALK_F:
                        self.sf_x[i] += self.sf_facing[i] * 0.7 * audio_boost
                    elif st == SF_WALK_B:
                        self.sf_x[i] -= self.sf_facing[i] * 0.6 * audio_boost
                    elif st == SF_JUMP:
                        self.sf_jump_phase[i] += 0.03
                        if self.sf_jump_phase[i] > 1.0:
                            self.sf_jump_phase[i] = 0.0
                    if self.sf_x[i] < SF_STAGE_L:
                        self.sf_x[i] = SF_STAGE_L
                    if self.sf_x[i] > SF_STAGE_R:
                        self.sf_x[i] = SF_STAGE_R

                    if not self.sf_impact_done[i] and st in (SF_PUNCH, SF_KICK):
                        total = 260 if st == SF_PUNCH else 320
                        if (self.sf_state_until[i] - now) <= (total // 2):
                            self.sf_impact_done[i] = True
                            reach = 46.0 if st == SF_PUNCH else 58.0
                            if abs(self.sf_x[i] - self.sf_x[other]) <= reach and self._sf_rand_range(0, 99) < 55:
                                dmg = float(self._sf_rand_range(4, 7)) if st == SF_PUNCH else float(self._sf_rand_range(6, 10))
                                self._sf_apply_hit(other, dmg, self.sf_facing[i], now)

                self.sf_health[0] -= 0.015
                self.sf_health[1] -= 0.015
                if self.sf_health[0] <= 0.0 or self.sf_health[1] <= 0.0:
                    self.sf_ko_active = True
                    self.sf_ko_until = now + 2600

                self._sf_update_projectile(0, now)
                self._sf_update_projectile(1, now)

        # ── 背景 ──
        painter.fillRect(0, SF_TOP, CANVAS_W, SF_GROUND_Y - SF_TOP, QColor(*SF_SKY_A_RGB))
        painter.fillRect(0, SF_TOP + 40, CANVAS_W, SF_GROUND_Y - SF_TOP - 40, QColor(*SF_SKY_B_RGB))
        for k in range(3):
            hx = 60 + k * 110
            painter.fillRect(hx - 40, SF_GROUND_Y - 26, 80, 26, QColor(*SF_HILL_RGB))
        painter.fillRect(0, SF_GROUND_Y, CANVAS_W, CANVAS_H - SF_GROUND_Y, QColor(*SF_GROUND_RGB))
        painter.fillRect(0, SF_GROUND_Y, CANVAS_W, 3, QColor(*SF_GROUND_LINE_RGB))

        # ── 体力ゲージ・ラウンド表示 ──
        self._sf_draw_bar(painter, 14, self.sf_health[0], False)
        self._sf_draw_bar(painter, CANVAS_W - 14 - 110, self.sf_health[1], True)
        font = QFont()
        font.setBold(True)
        font.setPointSize(9)
        painter.setFont(font)
        painter.setPen(QColor(255, 255, 255))
        painter.drawText(122, SF_TOP + 34, f"ROUND {self.sf_round}")
        painter.drawText(152, SF_TOP + 16, str(self.sf_timer))

        # ── 描画：CoreS3同様、K.O.中でもファイター・飛び道具は変わらず描画し続け、
        # K.O.文字はその上に重ねて表示するだけ（sfDrawFighter()はsfKoActiveで分岐しない）──
        self._sf_draw_fighter(painter, 0, SF_P1_RGB, SF_SKIN_RGB, True)
        self._sf_draw_fighter(painter, 1, SF_P2_RGB, SF_SKIN_RGB, False)
        for i in (0, 1):
            if not self.sf_proj_active[i]:
                continue
            pc = SF_P1_RGB if i == 0 else SF_P2_RGB
            pc_col = QColor(*pc)
            self._fill_circle(painter, int(round(self.sf_proj_x[i])), SF_GROUND_Y - 26, 6, pc_col)
            self._fill_circle(painter, int(round(self.sf_proj_x[i] - self.sf_proj_dir[i] * 8)), SF_GROUND_Y - 26, 3, pc_col)

        if self.sf_ko_active:
            ko_font = QFont()
            ko_font.setBold(True)
            ko_font.setPointSize(27)
            painter.setFont(ko_font)
            painter.setPen(QColor(255, 0, 0))
            painter.drawText(118, 156, "K.O.")

    # ----- Lighting: 8-Bit Runner（v0.5 Phase 4残り。CoreS3 lightRenderMario()準拠）-----
    def _mar_rand(self) -> int:
        self.mar_rng = (self.mar_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.mar_rng

    def _mar_rand01(self) -> float:
        return (self._mar_rand() & 0xFFFF) / 65535.0

    def _mar_spawn_next(self) -> None:
        gap = 90.0 + self._mar_rand01() * 110.0
        self.mar_next_spawn_world_x += gap
        slot = self.mar_spawn_idx % MAR_OBST_MAX
        self.mar_spawn_idx += 1
        r = self._mar_rand() % 100
        if r < 25:
            t = MAR_T_PIPE
        elif r < 50:
            t = MAR_T_ENEMY
        elif r < 65:
            t = MAR_T_ITEM
        elif r < 80:
            t = MAR_T_BLOCK
        else:
            t = MAR_T_COIN
        self.mar_obst[slot] = {"worldX": self.mar_next_spawn_world_x, "type": t, "triggered": False, "bumpTimer": 0}

    def _mar_init_all(self) -> None:
        self.mar_scroll_x = 0.0
        self.mar_coins = 0
        self.mar_player_jumping = False
        self.mar_run_phase = 0.0
        self.mar_next_spawn_world_x = 200.0
        self.mar_spawn_idx = 0
        self.mar_obst = [{"worldX": -9999.0, "type": MAR_T_COIN, "triggered": True, "bumpTimer": 0} for _ in range(MAR_OBST_MAX)]
        for _ in range(MAR_OBST_MAX):
            self._mar_spawn_next()
        self.mar_ready = True

    def _mar_start_jump(self, now_ms: int) -> None:
        self.mar_player_jumping = True
        self.mar_jump_start_at = now_ms

    @staticmethod
    def _mar_draw_player(painter: QPainter, px: int, py: int, leg_phase: float, jumping: bool) -> None:
        RED = QColor(*MAR_PLAYER_RED_RGB)
        OVER = QColor(*MAR_PLAYER_OVER_RGB)
        SKIN = QColor(*MAR_PLAYER_SKIN_RGB)
        SHOE = QColor(*MAR_PLAYER_SHOE_RGB)
        BLACK = QColor(0, 0, 0)
        frame_a = leg_phase >= 0.0
        torso_y = py - 30
        torso_h = 17
        if jumping:
            fThX, fThY, fShinX, fShinY, fShoeX, fShoeY = px, py - 19, px + 4, py - 13, px + 4, py - 9
            bThX, bThY, bShinX, bShinY, bShoeX, bShoeY = px - 6, py - 15, px - 9, py - 9, px - 10, py - 5
        elif frame_a:
            fThX, fThY, fShinX, fShinY, fShoeX, fShoeY = px - 1, py - 14, px + 2, py - 8, px + 2, py - 4
            bThX, bThY, bShinX, bShinY, bShoeX, bShoeY = px - 6, py - 14, px - 7, py - 8, px - 8, py - 5
        else:
            fThX, fThY, fShinX, fShinY, fShoeX, fShoeY = px, py - 18, px + 3, py - 12, px + 3, py - 8
            bThX, bThY, bShinX, bShinY, bShoeX, bShoeY = px - 5, py - 14, px - 5, py - 8, px - 6, py - 4

        painter.fillRect(bThX, bThY, 6, 6, OVER)
        painter.fillRect(bShinX, bShinY, 6, 4, OVER)
        painter.fillRect(bShoeX, bShoeY, 7, 4, SHOE)

        if not jumping and not frame_a:
            painter.fillRect(px - 10, torso_y + 9, 4, 5, RED)
            KariPomDesktopCanvas._fill_circle(painter, px - 9, torso_y + 15, 2, SKIN)

        painter.fillRect(px - 7, torso_y, 11, 4, RED)
        painter.fillRect(px - 7, torso_y + 4, 14, 5, RED)
        painter.fillRect(px - 7, torso_y + 9, 14, torso_h - 9, OVER)
        painter.fillRect(px + 1, torso_y + 4, 6, 5, OVER)
        painter.fillRect(px + 2, torso_y, 2, 4, OVER)

        painter.fillRect(fThX, fThY, 6, 6, OVER)
        painter.fillRect(fShinX, fShinY, 6, 4, OVER)
        painter.fillRect(fShoeX, fShoeY, 7, 4, SHOE)

        if jumping:
            painter.fillRect(px + 2, torso_y + 1, 5, 8, RED)
            KariPomDesktopCanvas._fill_circle(painter, px + 4, torso_y + 11, 3, SKIN)
        elif frame_a:
            painter.fillRect(px - 9, torso_y + 2, 5, 6, RED)
            painter.fillRect(px - 8, torso_y + 7, 6, 5, RED)
            KariPomDesktopCanvas._fill_circle(painter, px - 6, torso_y + 12, 3, SKIN)
        else:
            painter.fillRect(px + 1, torso_y + 3, 4, 5, RED)
            painter.fillRect(px + 4, torso_y + 7, 6, 4, RED)
            KariPomDesktopCanvas._fill_circle(painter, px + 8, torso_y + 12, 3, SKIN)

        KariPomDesktopCanvas._fill_circle(painter, px, py - 36, 9, SKIN)
        painter.fillRect(px - 10, py - 40, 5, 9, BLACK)
        painter.fillRect(px + 2, py - 38, 2, 3, BLACK)
        painter.fillRect(px + 7, py - 37, 6, 4, SKIN)
        painter.fillRect(px - 1, py - 33, 10, 3, BLACK)
        painter.fillRect(px - 10, py - 46, 19, 6, RED)
        painter.fillRect(px + 1, py - 42, 11, 4, RED)

    def _light_render_mario(self, painter: QPainter, level: float, now_ms: int, needs_init: bool) -> None:
        if not self.mar_ready:
            self._mar_init_all()
        if needs_init:
            self.mar_player_jumping = False
        audio_boost = 1.0 + level * 0.20
        speed = 1.4 * audio_boost

        painter.fillRect(0, MAR_TOP, CANVAS_W, MAR_GROUND_Y - MAR_TOP, QColor(*MAR_SKY_RGB))

        cloud_col = QColor(*MAR_CLOUD_RGB)
        for k in range(MAR_CLOUD_COUNT):
            sx = ((now_ms * 0.018 * (0.6 + 0.15 * k) + k * 150.0) % 420.0) - 100.0
            cy = MAR_TOP + 14 + (k % 3) * 16
            self._fill_circle(painter, int(sx), cy, 10, cloud_col)
            self._fill_circle(painter, int(sx) + 10, cy - 4, 8, cloud_col)
            self._fill_circle(painter, int(sx) - 10, cy - 3, 8, cloud_col)

        painter.fillRect(0, MAR_GROUND_Y, CANVAS_W, 6, QColor(*MAR_GROUND_TOP_RGB))
        painter.fillRect(0, MAR_GROUND_Y + 6, CANVAS_W, CANVAS_H - (MAR_GROUND_Y + 6), QColor(*MAR_GROUND_BODY_RGB))
        stripe_offset = int(self.mar_scroll_x) % 20
        x = -stripe_offset
        line_col = QColor(*MAR_GROUND_LINE_RGB)
        while x < CANVAS_W:
            painter.fillRect(int(x), MAR_GROUND_Y + 6, 2, CANVAS_H - (MAR_GROUND_Y + 6), line_col)
            x += 20

        self.mar_scroll_x += speed
        if self.mar_scroll_x >= MAR_LOOP_DIST:
            self._mar_init_all()
        while self.mar_next_spawn_world_x - self.mar_scroll_x < 400.0:
            self._mar_spawn_next()

        for o in self.mar_obst:
            screen_xf = o["worldX"] - self.mar_scroll_x
            if screen_xf < -60.0 or screen_xf > 380.0:
                continue
            sx = int(round(screen_xf))
            ground_obst = o["type"] in (MAR_T_PIPE, MAR_T_ENEMY)
            if ground_obst:
                if o["type"] == MAR_T_PIPE:
                    painter.fillRect(sx - 16, MAR_GROUND_Y - 34, 32, 34, QColor(*MAR_PIPE_RGB))
                    painter.fillRect(sx - 19, MAR_GROUND_Y - 40, 38, 8, QColor(*MAR_PIPE_EDGE_RGB))
                else:
                    enemy_col = QColor(*MAR_ENEMY_RGB)
                    self._fill_circle(painter, sx, MAR_GROUND_Y - 6, 8, enemy_col)
                    painter.fillRect(sx - 8, MAR_GROUND_Y - 2, 6, 4, enemy_col)
                    painter.fillRect(sx + 2, MAR_GROUND_Y - 2, 6, 4, enemy_col)
                    self._fill_circle(painter, sx - 3, MAR_GROUND_Y - 8, 2, QColor(255, 255, 255))
                    self._fill_circle(painter, sx + 3, MAR_GROUND_Y - 8, 2, QColor(255, 255, 255))
            elif o["type"] == MAR_T_BLOCK:
                painter.fillRect(sx - 12, MAR_BLOCK_Y - 12, 24, 24, QColor(*MAR_BLOCK_RGB))
            elif o["type"] == MAR_T_ITEM:
                by = MAR_BLOCK_Y - 12 - (min(o["bumpTimer"], 4) if o["bumpTimer"] > 0 else 0)
                if o["bumpTimer"] > 0:
                    o["bumpTimer"] -= 1
                painter.fillRect(sx - 12, by, 24, 24, QColor(*MAR_BLOCK_RGB) if o["triggered"] else QColor(*MAR_ITEM_RGB))
                if not o["triggered"]:
                    self._draw_text_centered(painter, "?", sx, by + 12, 13, QColor(0, 0, 0))
            elif o["type"] == MAR_T_COIN:
                if not o["triggered"]:
                    self._fill_circle(painter, sx, MAR_BLOCK_Y - 4, 6, QColor(*MAR_COIN_RGB))

            if not o["triggered"] and MAR_PLAYER_X + 26 < sx < MAR_PLAYER_X + 54 and not self.mar_player_jumping:
                o["triggered"] = True
                self._mar_start_jump(now_ms)
                if o["type"] == MAR_T_ITEM:
                    o["bumpTimer"] = 8
                    self.mar_coins += 1
                if o["type"] == MAR_T_COIN:
                    self.mar_coins += 1

        jump_offset = 0.0
        if self.mar_player_jumping:
            t = (now_ms - self.mar_jump_start_at) / MAR_JUMP_MS
            if t >= 1.0:
                self.mar_player_jumping = False
            else:
                jump_offset = math.sin(t * 3.14159) * MAR_JUMP_H
        self.mar_run_phase += 0.35 * audio_boost
        py = MAR_GROUND_Y - int(jump_offset)
        self._mar_draw_player(painter, MAR_PLAYER_X, py, math.sin(self.mar_run_phase), self.mar_player_jumping)

        font = QFont()
        font.setBold(True)
        font.setPointSize(9)
        painter.setFont(font)
        painter.setPen(QColor(*MAR_COIN_RGB))
        painter.drawText(8, MAR_TOP + 16, f"COIN x{self.mar_coins}")

    # ----- Lighting: Missile Defense（Phase 5B。CoreS3 lightRenderMissile()準拠）-----
    def _msl_rand(self) -> int:
        self.msl_rng = (self.msl_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.msl_rng

    def _msl_rand01(self) -> float:
        return (self._msl_rand() >> 8) / 16777216.0

    def _msl_spawn_enemy(self, i: int) -> None:
        self.msl_enemy_x0[i] = 20.0 + self._msl_rand01() * 280.0
        self.msl_enemy_y0[i] = float(MSL_TOP) + 4.0 + self._msl_rand01() * 18.0
        self.msl_enemy_tx[i] = 20.0 + self._msl_rand01() * 280.0
        self.msl_enemy_ty[i] = float(MSL_GROUND_Y)
        self.msl_enemy_x[i] = self.msl_enemy_x0[i]
        self.msl_enemy_y[i] = self.msl_enemy_y0[i]
        self.msl_enemy_progress[i] = 0.0
        self.msl_enemy_speed[i] = 0.0028 + self._msl_rand01() * 0.0022
        self.msl_enemy_active[i] = True

    def _msl_build_stars(self) -> None:
        for i in range(MSL_CSTAR_COUNT):
            self.msl_cstar_x[i] = int(self._msl_rand01() * 320.0)
            self.msl_cstar_y[i] = int(float(MSL_TOP) + self._msl_rand01() * float(MSL_GROUND_Y - MSL_TOP))
            self.msl_cstar_col[i] = MSL_CSTAR_PALETTE_RGB[self._msl_rand() % 4]
        self.msl_star_ready = True

    def _msl_reset_state(self) -> None:
        for i in range(MSL_MAX_ENEMY):
            self._msl_spawn_enemy(i)
            # 起動直後に全機が同時に発射点へ揃わないよう、飛来の進み具合を分散させる
            self.msl_enemy_progress[i] = self._msl_rand01() * 0.5
            self.msl_enemy_x[i] = self.msl_enemy_x0[i] + (self.msl_enemy_tx[i] - self.msl_enemy_x0[i]) * self.msl_enemy_progress[i]
            self.msl_enemy_y[i] = self.msl_enemy_y0[i] + (self.msl_enemy_ty[i] - self.msl_enemy_y0[i]) * self.msl_enemy_progress[i]
        for i in range(MSL_BURST_COUNT):
            self.msl_burst_active[i] = False
        self.msl_aim_state = MSL_AIM_SEARCH
        self.msl_aim_target = -1
        self.msl_aim_x = 160.0
        self.msl_aim_y = 90.0
        self.msl_reacquire_at = 0
        self.msl_shot_active = False

    def _msl_start_burst(self, x: float, y: float, now_ms: int) -> None:
        slot = -1
        oldest = 0xFFFFFFFF
        for i in range(MSL_BURST_COUNT):
            if not self.msl_burst_active[i]:
                slot = i
                break
            if self.msl_burst_start_ms[i] < oldest:
                oldest = self.msl_burst_start_ms[i]
                slot = i
        self.msl_burst_active[slot] = True
        self.msl_burst_x[slot] = x
        self.msl_burst_y[slot] = y
        self.msl_burst_start_ms[slot] = now_ms

    def _msl_pick_base(self, target_x: float) -> int:
        best = 0
        best_d = abs(target_x - MSL_BASE_X[0])
        for k in range(1, 3):
            d = abs(target_x - MSL_BASE_X[k])
            if d < best_d:
                best_d = d
                best = k
        return best

    def _msl_draw_base(self, painter: QPainter, x: int) -> None:
        painter.setPen(QPen(QColor(*MSL_BASE_RGB), 1))
        painter.drawLine(x - 6, MSL_GROUND_Y, x + 6, MSL_GROUND_Y)
        painter.drawLine(x - 6, MSL_GROUND_Y, x, MSL_GROUND_Y - 7)
        painter.drawLine(x + 6, MSL_GROUND_Y, x, MSL_GROUND_Y - 7)

    def _msl_draw_crosshair(self, painter: QPainter, x: int, y: int) -> None:
        painter.setPen(QPen(QColor(*MSL_AIM_RGB), 1))
        painter.drawLine(x - 7, y, x - 2, y)
        painter.drawLine(x + 2, y, x + 7, y)
        painter.drawLine(x, y - 7, x, y - 2)
        painter.drawLine(x, y + 2, x, y + 7)
        painter.setBrush(Qt.NoBrush)
        painter.drawRect(x - 3, y - 3, 6, 6)

    def _msl_draw_burst(self, painter: QPainter, i: int, now_ms: int) -> None:
        el = now_ms - self.msl_burst_start_ms[i]
        by = int(round(self.msl_burst_y[i]))
        if el < MSL_BURST_FLASH_MS:
            t = el / MSL_BURST_FLASH_MS
            r = 3 + int(t * (MSL_BURST_MAX_R - 3))
            if by - r < MSL_TOP:
                r = by - MSL_TOP
            if r < 1:
                return
            col_rgb = self._rgb_lerp(MSL_FLASH_RGB, MSL_FLASH2_RGB, int(t * 255.0))
            self._fill_circle(painter, int(round(self.msl_burst_x[i])), by, r, QColor(*col_rgb))
        elif el < MSL_BURST_LIFE_MS:
            t = (el - MSL_BURST_FLASH_MS) / (MSL_BURST_LIFE_MS - MSL_BURST_FLASH_MS)
            r = MSL_BURST_MAX_R - int(t * (MSL_BURST_MAX_R - 6))
            if r < 1:
                r = 1
            if by - r < MSL_TOP:
                r = by - MSL_TOP
            if r < 1:
                return
            col_rgb = self._rgb_lerp(MSL_FLASH2_RGB, MSL_SMOKE_RGB, int(t * 255.0))
            self._fill_circle(painter, int(round(self.msl_burst_x[i])), by, r, QColor(*col_rgb))

    def _light_render_missile(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if not self.msl_star_ready:
            self._msl_build_stars()
        if needs_init:
            self._msl_reset_state()

        # ── ロジック更新：CoreS3の実効呼び出し周期(LIGHT_FRAME_UPDATE_MS)に合わせてのみ進める。
        # TRACK中のイージング(aim += (target-aim)*0.12)もこの周期でのみ1回適用することで、
        # CoreS3実機と同じ追尾速度になる（Desktopの20ms毎にそのまま適用すると約4.5倍速になる）──
        if needs_init or (now_ms - self.msl_last_update_ms) >= LIGHT_FRAME_UPDATE_MS:
            self.msl_last_update_ms = now_ms
            now = now_ms

            for i in range(MSL_MAX_ENEMY):
                if not self.msl_enemy_active[i]:
                    continue
                self.msl_enemy_progress[i] += self.msl_enemy_speed[i]
                if self.msl_enemy_progress[i] >= 1.0:
                    self._msl_start_burst(self.msl_enemy_tx[i], self.msl_enemy_ty[i], now)
                    self.msl_enemy_active[i] = False
                    if self.msl_aim_target == i:
                        self.msl_aim_target = -1
                        self.msl_aim_state = MSL_AIM_SEARCH
                    continue
                self.msl_enemy_x[i] = self.msl_enemy_x0[i] + (self.msl_enemy_tx[i] - self.msl_enemy_x0[i]) * self.msl_enemy_progress[i]
                self.msl_enemy_y[i] = self.msl_enemy_y0[i] + (self.msl_enemy_ty[i] - self.msl_enemy_y0[i]) * self.msl_enemy_progress[i]

            for i in range(MSL_MAX_ENEMY):
                if not self.msl_enemy_active[i]:
                    self._msl_spawn_enemy(i)

            if self.msl_aim_state == MSL_AIM_COOLDOWN:
                if now >= self.msl_reacquire_at:
                    self.msl_aim_state = MSL_AIM_SEARCH
            if self.msl_aim_state == MSL_AIM_SEARCH:
                best = -1
                best_prog = -1.0
                for i in range(MSL_MAX_ENEMY):
                    if self.msl_enemy_active[i] and self.msl_enemy_progress[i] > best_prog:
                        best_prog = self.msl_enemy_progress[i]
                        best = i
                if best >= 0:
                    self.msl_aim_target = best
                    self.msl_aim_state = MSL_AIM_TRACK
            if self.msl_aim_state == MSL_AIM_TRACK:
                if self.msl_aim_target < 0 or not self.msl_enemy_active[self.msl_aim_target]:
                    self.msl_aim_target = -1
                    self.msl_aim_state = MSL_AIM_SEARCH
                else:
                    ti = self.msl_aim_target
                    self.msl_aim_x += (self.msl_enemy_x[ti] - self.msl_aim_x) * 0.12
                    self.msl_aim_y += (self.msl_enemy_y[ti] - self.msl_aim_y) * 0.12
                    dx = self.msl_enemy_x[ti] - self.msl_aim_x
                    dy = self.msl_enemy_y[ti] - self.msl_aim_y
                    if (dx * dx + dy * dy) <= (MSL_LOCK_DIST * MSL_LOCK_DIST):
                        base = self._msl_pick_base(self.msl_enemy_x[ti])
                        self.msl_shot_x0 = float(MSL_BASE_X[base])
                        self.msl_shot_y0 = float(MSL_GROUND_Y - 7)
                        self.msl_shot_x1 = self.msl_enemy_x[ti]
                        self.msl_shot_y1 = self.msl_enemy_y[ti]
                        self.msl_shot_active = True
                        self.msl_shot_start_ms = now
                        self._msl_start_burst(self.msl_enemy_x[ti], self.msl_enemy_y[ti], now)
                        self.msl_enemy_active[ti] = False
                        self.msl_aim_target = -1
                        self.msl_aim_state = MSL_AIM_COOLDOWN
                        self.msl_reacquire_at = now + MSL_REACQUIRE_MIN_MS + int(
                            self._msl_rand01() * (MSL_REACQUIRE_MAX_MS - MSL_REACQUIRE_MIN_MS)
                        )

        # ── 描画（爆発の残り時間・迎撃レーザーの表示可否は実時間millis()依存のため、
        # 更新ゲートの外＝毎ティック最新のnow_msで判定する。CoreS3のmslDrawBurst()等と同じ）──
        painter.fillRect(0, MSL_TOP, CANVAS_W, MSL_GROUND_Y - MSL_TOP, QColor(*MSL_SKY_RGB))
        painter.fillRect(0, MSL_GROUND_Y, CANVAS_W, CANVAS_H - MSL_GROUND_Y, QColor(*MSL_GROUND_RGB))

        for i in range(MSL_CSTAR_COUNT):
            painter.fillRect(self.msl_cstar_x[i], self.msl_cstar_y[i], 1, 1, QColor(*self.msl_cstar_col[i]))

        for k in range(3):
            self._msl_draw_base(painter, MSL_BASE_X[k])

        painter.setPen(QPen(QColor(*MSL_MISSILE_RGB), 1))
        for i in range(MSL_MAX_ENEMY):
            if not self.msl_enemy_active[i]:
                continue
            painter.drawLine(int(round(self.msl_enemy_x0[i])), int(round(self.msl_enemy_y0[i])),
                              int(round(self.msl_enemy_x[i])), int(round(self.msl_enemy_y[i])))

        self._msl_draw_crosshair(painter, int(round(self.msl_aim_x)), int(round(self.msl_aim_y)))

        if self.msl_shot_active:
            if now_ms - self.msl_shot_start_ms < MSL_SHOT_DURATION_MS:
                painter.setPen(QPen(QColor(*MSL_SHOT_RGB), 1))
                painter.drawLine(int(round(self.msl_shot_x0)), int(round(self.msl_shot_y0)),
                                  int(round(self.msl_shot_x1)), int(round(self.msl_shot_y1)))
            else:
                self.msl_shot_active = False

        for i in range(MSL_BURST_COUNT):
            if not self.msl_burst_active[i]:
                continue
            el = now_ms - self.msl_burst_start_ms[i]
            if el >= MSL_BURST_LIFE_MS:
                self.msl_burst_active[i] = False
                continue
            self._msl_draw_burst(painter, i, now_ms)

    # ----- Lighting: Aquarium（v0.5 Phase 4残り。CoreS3 lightRenderAquarium()準拠）-----
    def _aqu_rand(self) -> int:
        self.aqu_rng = (self.aqu_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.aqu_rng

    def _aqu_rand01(self) -> float:
        return (self._aqu_rand() & 0xFFFF) / 65535.0

    @staticmethod
    def _aqu_clamp255(v: float) -> int:
        iv = int(v)
        return 255 if iv > 255 else (0 if iv < 0 else iv)

    def _aqu_spawn_fish(self, i: int) -> None:
        self.aqu_fish_species[i] = self._aqu_rand() % AQU_SPECIES_COUNT
        body_len, body_hi, tail_size, pattern, long_fins, tail_style, speed_mul = AQU_SPECIES[self.aqu_fish_species[i]]
        self.aqu_fish_dir[i] = 1 if (self._aqu_rand() & 1) else -1
        self.aqu_fish_scale[i] = 1.05 + self._aqu_rand01() * 0.85
        self.aqu_fish_speed[i] = (0.20 + self._aqu_rand01() * 0.16) * speed_mul

        scale = self.aqu_fish_scale[i]
        fin_vert = body_hi * 0.9 if long_fins else (tail_size * 0.85 if tail_style == 1 else tail_size * 0.25)
        vert_half = (body_hi * 0.5 + fin_vert) * scale
        avail_y = (240.0 - float(AQU_TOP)) - 2.0 * vert_half - 12.0
        if avail_y < 0.0:
            avail_y = 0.0
        self.aqu_fish_base_y[i] = float(AQU_TOP) + vert_half + 6.0 + self._aqu_rand01() * avail_y

        self.aqu_fish_bob_phase[i] = self._aqu_rand01() * 6.2831853
        self.aqu_fish_bob_freq[i] = 0.7 + self._aqu_rand01() * 0.5
        self.aqu_fish_bob_amp[i] = 3.0 + self._aqu_rand01() * 4.0
        self.aqu_fish_tail_phase[i] = self._aqu_rand01() * 6.2831853
        self.aqu_fish_hue[i] = self._aqu_rand() % 256

        margin = (body_len * 0.5 + tail_size + 16.0) * scale
        self.aqu_fish_x[i] = -margin if self.aqu_fish_dir[i] > 0 else (320.0 + margin)
        self.aqu_fish_active[i] = True

    def _build_aquarium_table(self) -> None:
        for i in range(AQU_FISH_COUNT):
            self._aqu_spawn_fish(i)
            self.aqu_fish_x[i] = self._aqu_rand01() * 320.0
            self.aqu_fish_respawn_at[i] = 0
        for i in range(AQU_BUBBLE_COUNT):
            self.aqu_bubble_x[i] = self._aqu_rand01() * 320.0
            self.aqu_bubble_y[i] = float(AQU_TOP) + self._aqu_rand01() * (240.0 - float(AQU_TOP))
            self.aqu_bubble_r[i] = 1.0 + self._aqu_rand01() * 2.0
            self.aqu_bubble_speed[i] = 0.25 + self._aqu_rand01() * 0.35
            self.aqu_bubble_sway[i] = self._aqu_rand01() * 6.2831853
        for w in range(AQU_WEED_COUNT):
            self.aqu_weed_base_x[w] = 30.0 + w * (260.0 / AQU_WEED_COUNT) + self._aqu_rand01() * 20.0
            for b in range(AQU_WEED_BLADES):
                self.aqu_weed_height[w][b] = 26.0 + self._aqu_rand01() * 20.0
                self.aqu_weed_phase[w][b] = self._aqu_rand01() * 6.2831853
                self.aqu_weed_color[w][b] = (34, 110, 70) if (b & 1) else (24, 84, 54)
        for r in range(AQU_ROCK_COUNT):
            self.aqu_rock_x[r] = 60.0 + r * (200.0 / AQU_ROCK_COUNT) + self._aqu_rand01() * 30.0
            self.aqu_rock_y[r] = 236.0
            self.aqu_rock_r[r] = 14.0 + self._aqu_rand01() * 8.0
        for s in range(AQU_SAND_DOT_COUNT):
            self.aqu_sand_x[s] = int(self._aqu_rand01() * 320.0)
            self.aqu_sand_y[s] = int(234.0 + self._aqu_rand01() * 5.0)
        self.aqu_ready = True

    def _aqu_draw_background(self, painter: QPainter, now_ms: int) -> None:
        bands = 12
        total = 240 - AQU_TOP
        band_h = total // bands
        for b in range(bands):
            t = b / float(bands - 1)
            r = self._aqu_clamp255(10 + (1.0 - t) * 12.0)
            g = self._aqu_clamp255(70 - t * 40.0)
            bl = self._aqu_clamp255(95 - t * 45.0)
            y0 = AQU_TOP + b * band_h
            h = (240 - y0) if b == bands - 1 else band_h
            painter.fillRect(0, y0, CANVAS_W, h, QColor(r, g, bl))
        t1 = now_ms / 1000.0
        col = QColor(46, 108, 118)
        for k in range(2):
            sway = math.sin(t1 * 0.35 + k * 2.4) * 24.0
            base_x = 80.0 + k * 150.0 + sway
            self._poly(painter, [(base_x, AQU_TOP), (base_x + 20, AQU_TOP), (base_x - 34, 240)], col)

    def _aqu_draw_rocks_and_sand(self, painter: QPainter) -> None:
        base = QColor(58, 66, 78)
        hi = QColor(88, 98, 112)
        for r in range(AQU_ROCK_COUNT):
            cx, cy, rad = self.aqu_rock_x[r], self.aqu_rock_y[r], self.aqu_rock_r[r]
            self._fill_circle(painter, int(cx), int(cy), int(rad), base)
            self._fill_circle(painter, int(cx - rad * 0.6), int(cy - rad * 0.1), int(rad * 0.7), base)
            self._fill_circle(painter, int(cx + rad * 0.55), int(cy - rad * 0.05), int(rad * 0.65), base)
            self._fill_circle(painter, int(cx - rad * 0.2), int(cy - rad * 0.75), int(rad * 0.35), hi)
        painter.fillRect(0, 234, CANVAS_W, 240 - 234, QColor(70, 78, 60))
        sand_col = QColor(90, 96, 74)
        for s in range(AQU_SAND_DOT_COUNT):
            painter.fillRect(self.aqu_sand_x[s], self.aqu_sand_y[s], 1, 1, sand_col)

    def _aqu_draw_weeds(self, painter: QPainter, now_ms: int) -> None:
        t = now_ms / 1000.0
        for w in range(AQU_WEED_COUNT):
            for b in range(AQU_WEED_BLADES):
                base_x = self.aqu_weed_base_x[w] + (b - 1) * 6.0
                h = self.aqu_weed_height[w][b]
                phase = self.aqu_weed_phase[w][b]
                col = QColor(*self.aqu_weed_color[w][b])
                px, py = int(base_x), 238
                for s in range(1, AQU_WEED_SEGS + 1):
                    ft = s / float(AQU_WEED_SEGS)
                    sway = math.sin(t * 0.9 + phase + ft * 2.2) * (4.0 * ft)
                    nx = int(base_x + sway)
                    ny = int(238.0 - h * ft)
                    self._draw_thick_line(painter, px, py, nx, ny, 1, col)
                    px, py = nx, ny

    def _aqu_update_and_draw_bubbles(self, painter: QPainter, now_ms: int) -> None:
        t = now_ms / 1000.0
        bub_col = QColor(190, 222, 228)
        white = QColor(255, 255, 255)
        for i in range(AQU_BUBBLE_COUNT):
            self.aqu_bubble_y[i] -= self.aqu_bubble_speed[i]
            if self.aqu_bubble_y[i] < float(AQU_TOP) - 4.0:
                self.aqu_bubble_y[i] = 238.0
                self.aqu_bubble_x[i] = self._aqu_rand01() * 320.0
                self.aqu_bubble_r[i] = 1.0 + self._aqu_rand01() * 2.0
                self.aqu_bubble_speed[i] = 0.25 + self._aqu_rand01() * 0.35
            sway = math.sin(t * 1.4 + self.aqu_bubble_sway[i]) * 3.0
            bx = int(self.aqu_bubble_x[i] + sway)
            by = int(self.aqu_bubble_y[i])
            r = int(self.aqu_bubble_r[i])
            self._fill_circle(painter, bx, by, r, bub_col)
            self._fill_circle(painter, bx - (1 if r > 1 else 0), by - (1 if r > 1 else 0), 1, white)

    def _aqu_draw_fish(self, painter: QPainter, i: int) -> None:
        body_len0, body_hi0, tail_size0, pattern, long_fins, tail_style, _speed_mul = AQU_SPECIES[self.aqu_fish_species[i]]
        scale = self.aqu_fish_scale[i]
        d = self.aqu_fish_dir[i]
        x = self.aqu_fish_x[i]
        y = self.aqu_fish_base_y[i] + math.sin(self.aqu_fish_bob_phase[i]) * self.aqu_fish_bob_amp[i]

        body_len = body_len0 * scale
        body_hi = body_hi0 * scale
        tail_len = tail_size0 * scale

        seed = self.aqu_fish_hue[i]
        rr = self._aqu_clamp255(110 + (seed % 110))
        gg = self._aqu_clamp255(70 + ((seed >> 2) % 140))
        bb = self._aqu_clamp255(130 + ((seed >> 4) % 110))
        body_top = QColor(rr, gg, bb)
        body_belly = QColor(self._aqu_clamp255(rr + 70), self._aqu_clamp255(gg + 60), self._aqu_clamp255(bb + 50))
        body_shadow = QColor(self._aqu_clamp255(rr - 50), self._aqu_clamp255(gg - 40), self._aqu_clamp255(bb - 40))
        fin_col = QColor(self._aqu_clamp255(rr * 0.55), self._aqu_clamp255(gg * 0.55), self._aqu_clamp255(bb * 0.65))
        gill_col = QColor(self._aqu_clamp255(rr - 35), self._aqu_clamp255(gg - 25), self._aqu_clamp255(bb - 25))
        mark_col = QColor(25, 20, 20) if pattern == 3 else QColor(250, 248, 240)
        hilite_col = QColor(self._aqu_clamp255(rr + 110), self._aqu_clamp255(gg + 110), self._aqu_clamp255(bb + 100))

        tail_wag = math.sin(self.aqu_fish_tail_phase[i]) * 0.6
        fin_wag = math.sin(self.aqu_fish_tail_phase[i] * 0.75 + 1.0) * 0.2

        head_x = x + (body_len * 0.5) * d
        tail_base_x = x - (body_len * 0.5) * d

        if tail_style == 1:
            for lobe in (-1, 0, 1):
                lobe_ang = lobe * 0.42 + tail_wag * 0.5
                tx = tail_base_x - d * tail_len * (1.05 + abs(tail_wag) * 0.15)
                ty = y + math.sin(lobe_ang) * tail_len * 0.85
                self._poly(painter, [(tail_base_x, y - 3), (tail_base_x, y + 3), (tx, ty)],
                           fin_col if lobe == 0 else body_shadow)
        else:
            t_tip_x = tail_base_x - d * tail_len * (1.0 + abs(tail_wag) * 0.25)
            t_top_y = y - tail_len * 0.55 + tail_wag * tail_len * 0.55
            t_bot_y = y + tail_len * 0.55 + tail_wag * tail_len * 0.55
            self._poly(painter, [(tail_base_x, y), (t_tip_x, t_top_y), (tail_base_x, y - body_hi * 0.12)], fin_col)
            self._poly(painter, [(tail_base_x, y), (t_tip_x, t_bot_y), (tail_base_x, y + body_hi * 0.12)], fin_col)

        if long_fins:
            anal_x = x - d * body_len * 0.05
            anal_bot_y = y + body_hi * 0.5 + body_hi * 0.85 + fin_wag * 8.0
            self._poly(painter, [
                (anal_x - body_len * 0.16, y + body_hi * 0.42),
                (anal_x + body_len * 0.10, y + body_hi * 0.42),
                (anal_x, anal_bot_y),
            ], fin_col)

        # v0.5 Phase 4見た目修正（2026-08-16、CoreS3側と同時修正）：根本（幅の広い側）を
        # 胴体楕円の縁ちょうど（tail_base_x、楕円の高さが0になる位置）に置くと、楕円の縁が
        # 滑らかに0へすぼまるのに対し三角形はそこで急に0.76*body_hi幅の垂直な壁になって
        # しまい、「楕円＋三角」が別パーツに見える原因になっていた。CoreS3側
        # （aquDrawFish()）で先に確認・修正した内容をそのまま移植：根本の位置だけを胴体
        # 中心側（tail_wedge_base_x）へ寄せ、その位置での楕円の自然な高さ
        # （0.38*body_hi相当）と三角形の根本幅を一致させ、先端は従来どおりtail_base_x
        # （尾びれの付け根）へ向けてすぼめる。三角形の形状・色・尾びれ側の座標・魚の輪郭
        # サイズそのものは変更していない（根本位置と先端位置を入れ替えただけ）。
        tail_wedge_base_x = x - d * body_len * 0.32
        self._poly(painter, [
            (tail_wedge_base_x, y - body_hi * 0.38), (tail_wedge_base_x, y + body_hi * 0.38),
            (tail_base_x, y),
        ], body_top)

        self._fill_ellipse(painter, int(x), int(y), int(body_len * 0.5), int(body_hi * 0.5), body_top)
        self._fill_ellipse(painter, int(x), int(y + body_hi * 0.20), int(body_len * 0.44), int(body_hi * 0.30), body_belly)

        # v0.5 Phase 4見た目修正：尾の付け根と同じ理由で、根本（head_x）を胴体中心側
        # （nose_base_x）へ寄せ、その位置での楕円の自然な高さ（0.30*body_hi相当）と
        # 三角形の根本幅を一致させた。鼻先の位置・とがり方（head_x + body_len*0.16の
        # 先端）は変更していない。
        nose_base_x = x + d * body_len * 0.40
        self._poly(painter, [
            (nose_base_x, y - body_hi * 0.30), (nose_base_x, y + body_hi * 0.30),
            (head_x + d * body_len * 0.16, y),
        ], body_top)

        self._draw_thick_line(painter, int(x - body_len * 0.28), int(y - body_hi * 0.30),
                               int(x + body_len * 0.18), int(y - body_hi * 0.36), 1, hilite_col)
        self._draw_thick_line(painter, int(x - body_len * 0.24), int(y + body_hi * 0.40),
                               int(x + body_len * 0.20), int(y + body_hi * 0.42), 1, body_shadow)

        gill_x = int(head_x - d * body_len * 0.22)
        self._draw_thick_line(painter, gill_x, int(y - body_hi * 0.38),
                               int(gill_x - d * body_hi * 0.10), int(y + body_hi * 0.38), 1, gill_col)

        if pattern == 1:
            for s in (-1, 0, 1, 2):
                sx = int(x + (s - 0.5) * body_len * 0.20)
                self._draw_thick_line(painter, sx, int(y - body_hi * 0.46), sx, int(y + body_hi * 0.46), 1, mark_col)
        elif pattern == 2:
            for s in range(5):
                ang = s * 1.25 + float(seed)
                sx = int(x + math.cos(ang) * body_len * 0.24)
                sy = int(y + math.sin(ang) * body_hi * 0.24)
                self._fill_circle(painter, sx, sy, int(2.2 * scale) + 1, mark_col)
        elif pattern == 3:
            sx = int(x)
            painter.fillRect(sx - int(3 * scale) - 1, int(y - body_hi * 0.48), int(6 * scale) + 2, int(body_hi * 0.96), QColor(250, 248, 240))
            painter.fillRect(sx - int(3 * scale), int(y - body_hi * 0.46), int(6 * scale), int(body_hi * 0.92), mark_col)
        elif pattern == 4:
            self._draw_thick_line(painter, int(head_x), int(y), int(tail_base_x), int(y), 1, mark_col)

        dorsal_x = x - d * body_len * 0.08
        dorsal_h = body_hi * (1.35 if long_fins else 0.75)
        dorsal_top_y = y - body_hi * 0.5 - dorsal_h + fin_wag * 10.0
        self._poly(painter, [
            (dorsal_x - body_len * 0.16, y - body_hi * 0.40),
            (dorsal_x + body_len * 0.10, y - body_hi * 0.40),
            (dorsal_x, dorsal_top_y),
        ], fin_col)

        pec_x = head_x - d * body_len * 0.22
        pec_y = y + body_hi * 0.12
        self._poly(painter, [
            (pec_x, pec_y),
            (pec_x - d * body_len * 0.20, pec_y + body_hi * 0.55 + fin_wag * 8.0),
            (pec_x - d * body_len * 0.06, pec_y),
        ], fin_col)

        eye_x = int(head_x - d * body_len * 0.10)
        eye_y = int(y - body_hi * 0.10)
        eye_r_full = int(body_hi * 0.17) + 1
        eye_r = max(1, eye_r_full // 2)
        self._fill_circle(painter, eye_x, eye_y, eye_r, QColor(0, 0, 0))
        self._fill_circle(painter, eye_x + (1 if d > 0 else -1), eye_y - 1, 1, QColor(255, 255, 255))

    def _aqu_update_fish(self, i: int, now_ms: int, dt: float) -> None:
        if not self.aqu_fish_active[i]:
            if self.aqu_fish_respawn_at[i] != 0 and now_ms >= self.aqu_fish_respawn_at[i]:
                self._aqu_spawn_fish(i)
                self.aqu_fish_respawn_at[i] = 0
            return
        self.aqu_fish_x[i] += self.aqu_fish_speed[i] * self.aqu_fish_dir[i]
        self.aqu_fish_bob_phase[i] += dt * self.aqu_fish_bob_freq[i]
        self.aqu_fish_tail_phase[i] += dt * 3.0

        body_len0, _body_hi0, tail_size0, *_rest = AQU_SPECIES[self.aqu_fish_species[i]]
        margin = (body_len0 * 0.5 + tail_size0 + 16.0) * self.aqu_fish_scale[i]
        off_right = self.aqu_fish_dir[i] > 0 and self.aqu_fish_x[i] > 320.0 + margin
        off_left = self.aqu_fish_dir[i] < 0 and self.aqu_fish_x[i] < -margin
        if off_right or off_left:
            self.aqu_fish_active[i] = False
            self.aqu_fish_respawn_at[i] = now_ms + 1200 + int(self._aqu_rand01() * 3200.0)

    def _light_render_aquarium(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if not self.aqu_ready:
            self._build_aquarium_table()
        if needs_init or self.aqu_prev_ms == 0:
            self.aqu_prev_ms = now_ms
        dt = (now_ms - self.aqu_prev_ms) / 1000.0
        if dt > 0.5:
            dt = 0.5
        self.aqu_prev_ms = now_ms

        painter.save()
        painter.setClipRect(0, AQU_TOP, CANVAS_W, CANVAS_H - AQU_TOP)

        self._aqu_draw_background(painter, now_ms)
        self._aqu_draw_rocks_and_sand(painter)
        self._aqu_draw_weeds(painter, now_ms)
        self._aqu_update_and_draw_bubbles(painter, now_ms)

        for i in range(AQU_FISH_COUNT):
            self._aqu_update_fish(i, now_ms, dt)
            if self.aqu_fish_active[i]:
                self._aqu_draw_fish(painter, i)

        painter.restore()

    # ----- Lighting: Flying Pompadour（v0.5 Phase 4残り。CoreS3
    # lightRenderFlyingPompadour()準拠。「Flying Toasters」へのオマージュ）-----
    def _flyp_rand(self) -> int:
        self.flyp_rng = (self.flyp_rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.flyp_rng

    def _flyp_rand01(self) -> float:
        return (self._flyp_rand() & 0xFFFF) / 65535.0

    def _flyp_build_palette(self) -> None:
        self.flyp_palette_rt = [dict(p) for p in FLYP_PALETTE_RGB]

    def _flyp_spawn(self, i: int, now_ms: int) -> None:
        layer = self._flyp_rand() % FLYP_LAYER_COUNT
        self.flyp_layer[i] = layer
        self.flyp_palette_idx[i] = self._flyp_rand() % FLYP_PALETTE_COUNT

        scale_lo, scale_hi = FLYP_LAYER_SCALE_LO[layer], FLYP_LAYER_SCALE_HI[layer]
        speed_lo, speed_hi = FLYP_LAYER_SPEED_LO[layer], FLYP_LAYER_SPEED_HI[layer]
        self.flyp_scale[i] = scale_lo + self._flyp_rand01() * (scale_hi - scale_lo)
        base_speed = speed_lo + self._flyp_rand01() * (speed_hi - speed_lo)

        ang_deg = 25.0 + self._flyp_rand01() * 25.0
        ang = math.radians(ang_deg)
        self.flyp_vx[i] = -math.cos(ang) * base_speed
        self.flyp_vy[i] = math.sin(ang) * base_speed

        margin = 30.0 * self.flyp_scale[i]
        self.flyp_x[i] = 320.0 + margin + self._flyp_rand01() * 40.0
        self.flyp_y[i] = float(FLYP_TOP) + 6.0 + self._flyp_rand01() * 70.0
        self.flyp_wing_phase[i] = self._flyp_rand01() * 6.2831853
        self.flyp_active[i] = True

    def _build_flyingpompadour_table(self, now_ms: int) -> None:
        self._flyp_build_palette()
        for i in range(FLYP_SLOT_COUNT):
            self.flyp_active[i] = False
            self.flyp_next_spawn_at[i] = now_ms + int(self._flyp_rand01() * 3500.0)
        for s in range(FLYP_STAR_COUNT):
            self.flyp_star_x[s] = int(self._flyp_rand01() * 320.0)
            self.flyp_star_y[s] = int(float(FLYP_TOP) + self._flyp_rand01() * 90.0)
            self.flyp_star_bright[s] = (self._flyp_rand() & 1) != 0
            self.flyp_star_next_blink_at[s] = now_ms + 400 + int(self._flyp_rand01() * 2200.0)
        for c in range(FLYP_CLOUD_COUNT):
            self.flyp_cloud_x[c] = self._flyp_rand01() * 320.0
            self.flyp_cloud_y[c] = float(FLYP_TOP) + 70.0 + self._flyp_rand01() * 90.0
            self.flyp_cloud_r[c] = 20.0 + self._flyp_rand01() * 16.0
            self.flyp_cloud_speed[c] = 0.08 + self._flyp_rand01() * 0.10
        self.flyp_ready = True

    def _flyp_draw_background(self, painter: QPainter, now_ms: int) -> None:
        bands = 10
        total = 240 - FLYP_TOP
        band_h = total // bands
        for b in range(bands):
            t = b / float(bands - 1)
            r = int(60 + (1.0 - t) * 70.0)
            g = int(40 + (1.0 - t) * 35.0)
            bl = int(70 + (1.0 - t) * 40.0 + t * 20.0)
            y0 = FLYP_TOP + b * band_h
            h = (240 - y0) if b == bands - 1 else band_h
            painter.fillRect(0, y0, CANVAS_W, h, QColor(r, g, bl))

        bright_col = QColor(255, 250, 230)
        dim_col = QColor(140, 130, 150)
        for s in range(FLYP_STAR_COUNT):
            if now_ms >= self.flyp_star_next_blink_at[s]:
                self.flyp_star_bright[s] = not self.flyp_star_bright[s]
                self.flyp_star_next_blink_at[s] = now_ms + 400 + int(self._flyp_rand01() * 2200.0)
            col = bright_col if self.flyp_star_bright[s] else dim_col
            painter.fillRect(self.flyp_star_x[s], self.flyp_star_y[s], 1, 1, col)

        cloud_col = QColor(90, 70, 100)
        for c in range(FLYP_CLOUD_COUNT):
            self.flyp_cloud_x[c] -= self.flyp_cloud_speed[c]
            if self.flyp_cloud_x[c] < -self.flyp_cloud_r[c] * 2.2:
                self.flyp_cloud_x[c] = 320.0 + self.flyp_cloud_r[c]
            cx, cy, r = self.flyp_cloud_x[c], self.flyp_cloud_y[c], self.flyp_cloud_r[c]
            self._fill_circle(painter, int(cx), int(cy), int(r * 0.7), cloud_col)
            self._fill_circle(painter, int(cx - r * 0.8), int(cy + r * 0.15), int(r * 0.55), cloud_col)
            self._fill_circle(painter, int(cx + r * 0.85), int(cy + r * 0.1), int(r * 0.5), cloud_col)

    def _flyp_draw_flyer(self, painter: QPainter, i: int) -> None:
        pal = self.flyp_palette_rt[self.flyp_palette_idx[i]]
        pal_body = QColor(*pal["body"])
        pal_shade = QColor(*pal["bodyShade"])
        pal_bezel = QColor(*pal["bezel"])
        pal_wing = QColor(*pal["wing"])
        pal_wingshade = QColor(*pal["wingShade"])
        scale = self.flyp_scale[i]
        x, y = self.flyp_x[i], self.flyp_y[i]

        bw = int(20.0 * scale)
        bh = int(15.0 * scale)
        depth = int(6.0 * scale) + 1

        body_l = int(x - bw * 0.5)
        body_r = body_l + bw
        body_t = int(y - bh * 0.5)
        body_b = body_t + bh

        spd = math.hypot(self.flyp_vx[i], self.flyp_vy[i])
        dx = (self.flyp_vx[i] / spd) if spd > 0.0001 else 1.0
        dy = (self.flyp_vy[i] / spd) if spd > 0.0001 else -1.0
        trail_col = QColor(210, 200, 210)
        for k in (1, 2, 3):
            tx = x - dx * (bw * 0.9 + k * 6.0 * scale)
            ty = y - dy * (bw * 0.9 + k * 6.0 * scale)
            self._draw_thick_line(painter, int(tx), int(ty), int(tx - dx * 3.0), int(ty - dy * 3.0), 1, trail_col)

        frame = int((self.flyp_wing_phase[i] % 6.2831853) / 6.2831853 * 4.0) & 3
        wing_lift = FLYP_WING_FRAMES[frame] * 5.0 * scale

        # v0.5 Phase 4見た目修正：座標式・半径式・3枚重ね・配色パターン（root→tipで
        # wingShade/wing交互）はCoreS3のflypDrawFlyer()と完全一致させたまま変更していない
        # （検証済み）。CoreS3実機の小さい画面では3枚が密着した「翼らしい質感」に見えるが、
        # Desktopの大画面では同じ重なり量でも境界線がはっきり見え「丸3個」に分離して
        # 見えてしまっていたため、3枚の中心間隔（position係数）と羽ばたきによる縦方向の
        # 移動量（lift係数）だけを、根元・中間・先端の相対順序と半径式は変えずに
        # 約2/3へ圧縮し、重なりを増やして連続した翼のシルエットに近づけた
        # （羽の可動域・伸び方向・色配置・枚数はCoreS3のまま）。
        wing_span = bw * 0.72
        # 羽形状・位置・羽ばたきは元実装のまま。色だけ一色化する。
        # 奥羽は、白との差がわずかに分かる程度の薄いグレー。
        rear_wing_col = QColor(218, 218, 222)
        front_wing_col = QColor(248, 248, 250)
        ws = wing_span * 0.92
        lift0, lift1, lift2 = wing_lift * 0.15, wing_lift * 0.37, wing_lift * 0.57
        rx0, ry0 = body_l - int(ws * 0.20), int(y - bh * 0.05 - lift0)
        rx1, ry1 = body_l - int(ws * 0.41), int(y - bh * 0.18 - lift1)
        rx2, ry2 = body_l - int(ws * 0.63), int(y - bh * 0.28 - lift2)
        self._fill_ellipse(painter, rx0, ry0, int(bw * 0.26) + 1, int(bh * 0.32) + 1, rear_wing_col)
        self._fill_ellipse(painter, rx1, ry1, int(bw * 0.22) + 1, int(bh * 0.27) + 1, rear_wing_col)
        self._fill_ellipse(painter, rx2, ry2, int(bw * 0.17) + 1, int(bh * 0.21) + 1, rear_wing_col)

        ws = wing_span
        lift0, lift1, lift2 = wing_lift * 0.17, wing_lift * 0.43, wing_lift * 0.67
        rx0, ry0 = body_r + int(ws * 0.20), int(y - bh * 0.05 - lift0)
        rx1, ry1 = body_r + int(ws * 0.41), int(y - bh * 0.18 - lift1)
        rx2, ry2 = body_r + int(ws * 0.63), int(y - bh * 0.28 - lift2)

        cx_back = (body_l + body_r) // 2 + depth
        back_top_y = body_t - depth
        if self.flyp_layer[i] == 0:
            ear_w = int(bw * 0.12) + 1
            ear_h_full = int(bh * 0.55) + 1
            ear_h = max(1, int(ear_h_full * 0.667))
            ear_gap_x = int(bw * 0.18) + 1
            ear_base_y = back_top_y + 1
            painter.fillRect(cx_back - ear_gap_x - ear_w // 2, ear_base_y - ear_h, ear_w, ear_h, pal_shade)
            painter.fillRect(cx_back + ear_gap_x - ear_w // 2, ear_base_y - ear_h, ear_w, ear_h, pal_shade)
        else:
            ear_w = int(bw * 0.15) + 1
            ear_h_full = int(bh * 0.8) + 2
            ear_h = max(1, int(ear_h_full * 0.667))
            ear_gap_x = int(bw * 0.20) + 1
            ear_base_y = back_top_y + 1
            ear_tip_y = ear_base_y - ear_h
            shade_w = 1
            for s in (-1, 1):
                ex = cx_back + s * ear_gap_x
                painter.fillRect(ex - ear_w // 2, ear_tip_y, ear_w, ear_h, pal_shade)
                self._fill_circle(painter, ex, ear_tip_y, ear_w // 2, pal_shade)
                painter.fillRect(ex + s * (ear_w // 2 - shade_w), ear_tip_y, shade_w, ear_h, pal_bezel)
                self._fill_circle(painter, ex - s * (ear_w // 2 - shade_w), ear_tip_y, 1, pal_body)

        # v0.5 Phase 4見た目修正：CoreS3側コメントは「耳の根元は正面シェルの後方にあるため、
        # 正面シェルの描画で自然に隠れる」としているが、実際にはCoreS3側にも本体「上面」
        # （耳の付け根〜正面シェル上端の間、backTopYからbodyTまでの領域）を塗るポリゴンが
        # 無く、実機の小さい画面では目立たなかった背景の透過がDesktopの大画面では
        # はっきり見えてしまっていた。ear/wing/body側面・底面・正面いずれの座標・サイズ・
        # 描画順も変更せず、CoreS3には存在しない「上面」パネルをここに追加するだけの
        # 最小追加で、耳の付け根付近の透けを解消する（本体形状・耳の位置サイズは無改造）。
        self._poly(
            painter,
            [
                (body_l, body_t),
                (body_r, body_t),
                (body_r + depth, body_t - depth),
                (body_l + depth, body_t - depth),
            ],
            pal_bezel,
        )

        self._poly(painter, [(body_l, body_b), (body_r, body_b), (body_r + depth, body_b - depth)], pal_shade)
        self._poly(painter, [(body_l, body_b), (body_r + depth, body_b - depth), (body_l + depth, body_b - depth)], pal_shade)
        self._poly(painter, [(body_r, body_t), (body_r, body_b), (body_r + depth, body_b - depth)], pal_bezel)
        self._poly(painter, [(body_r, body_t), (body_r + depth, body_b - depth), (body_r + depth, body_t - depth)], pal_bezel)

        painter.fillRect(body_l, body_t, bw, bh, pal_body)
        white = QColor(255, 255, 255)
        self._draw_thick_line(painter, body_l, body_t, body_r, body_t, 1, white)
        self._draw_thick_line(painter, body_l, body_t, body_l, body_b, 1, white)

        # 画面右側の手前羽：筐体より前に描画する。
        # 羽の形・位置・羽ばたき・色は変更しない。
        self._fill_ellipse(painter, rx0, ry0, int(bw * 0.28) + 1, int(bh * 0.34) + 1, front_wing_col)
        self._fill_ellipse(painter, rx1, ry1, int(bw * 0.24) + 1, int(bh * 0.29) + 1, front_wing_col)
        self._fill_ellipse(painter, rx2, ry2, int(bw * 0.19) + 1, int(bh * 0.23) + 1, front_wing_col)

        disp_w = int(bw * 0.66)
        disp_h = int(bh * 0.66)
        disp_x = (body_l + body_r) // 2 - disp_w // 2
        disp_y = (body_t + body_b) // 2 - disp_h // 2
        painter.fillRect(disp_x - 1, disp_y - 1, disp_w + 2, disp_h + 2, pal_bezel)
        painter.fillRect(disp_x, disp_y, disp_w, disp_h, white)

        black = QColor(0, 0, 0)
        eye_cx = disp_x + disp_w // 2
        eye_cy = disp_y + int(disp_h * 0.32)
        eye_dx = int(disp_w * 0.30) + 1
        eye_r = int(disp_h * 0.075) + 1
        self._fill_circle(painter, eye_cx - eye_dx, eye_cy, eye_r, black)
        self._fill_circle(painter, eye_cx + eye_dx, eye_cy, eye_r, black)

        if self.flyp_layer[i] >= 1:
            nose_cy = eye_cy + int(disp_h * 0.24)
            nose_rw = int(disp_w * 0.055) + 1
            nose_rh = int(disp_h * 0.05) + 1
            self._fill_ellipse(painter, eye_cx, nose_cy, nose_rw, nose_rh, black)
            line_bot_y = nose_cy + nose_rh + int(disp_h * 0.16)
            self._draw_thick_line(painter, eye_cx, nose_cy + nose_rh, eye_cx, line_bot_y, 1, black)
            mouth_half_w = int(disp_w * 0.13) + 1
            mouth_drop_y = int(disp_h * 0.09) + 1
            self._draw_thick_line(painter, eye_cx, line_bot_y, eye_cx - mouth_half_w, line_bot_y + mouth_drop_y, 1, black)
            self._draw_thick_line(painter, eye_cx, line_bot_y, eye_cx + mouth_half_w, line_bot_y + mouth_drop_y, 1, black)

        self._fill_circle(painter, body_r - 3, body_t + 3, 1, QColor(230, 210, 140))

    def _flyp_update_slot(self, i: int, now_ms: int, dt: float) -> None:
        if not self.flyp_active[i]:
            if now_ms >= self.flyp_next_spawn_at[i]:
                self._flyp_spawn(i, now_ms)
            return
        self.flyp_x[i] += self.flyp_vx[i] * dt * 60.0
        self.flyp_y[i] += self.flyp_vy[i] * dt * 60.0
        self.flyp_wing_phase[i] += dt * (3.6 + self.flyp_layer[i] * 0.8)

        margin = 34.0 * self.flyp_scale[i]
        off_screen = (self.flyp_x[i] < -margin) or (self.flyp_y[i] > 240.0 + margin)
        if off_screen:
            self.flyp_active[i] = False
            self.flyp_next_spawn_at[i] = now_ms + 300 + int(self._flyp_rand01() * 2600.0)

    def _light_render_flyingpompadour(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if not self.flyp_ready:
            self._build_flyingpompadour_table(now_ms)
        if needs_init or self.flyp_prev_ms == 0:
            self.flyp_prev_ms = now_ms
        dt = (now_ms - self.flyp_prev_ms) / 1000.0
        if dt > 0.5:
            dt = 0.5
        self.flyp_prev_ms = now_ms

        painter.save()
        painter.setClipRect(0, FLYP_TOP, CANVAS_W, CANVAS_H - FLYP_TOP)

        self._flyp_draw_background(painter, now_ms)

        for i in range(FLYP_SLOT_COUNT):
            self._flyp_update_slot(i, now_ms, dt)
        for pass_ in range(FLYP_LAYER_COUNT):
            for i in range(FLYP_SLOT_COUNT):
                if self.flyp_active[i] and self.flyp_layer[i] == pass_:
                    self._flyp_draw_flyer(painter, i)

        painter.restore()

    # ----- Lighting: Pixel Invasion（v0.5 Phase 5A。CoreS3 lightRenderPixelInvasion()準拠）-----
    def _pix_rand(self) -> int:
        """CoreS3 pixRand()と同じxorshift32（このLighting専用。共通化はしない）。"""
        x = self.pix_rng & 0xFFFFFFFF
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        self.pix_rng = x
        return x

    def _pix_rand01(self) -> float:
        return (self._pix_rand() & 0xFFFF) / 65535.0

    def _pix_invader_x(self, c: int) -> int:
        return PIX_FORM_LEFT_X0 + c * PIX_FORM_COL_DX + self.pix_form_offset_x

    def _pix_invader_y(self, r: int) -> int:
        return PIX_FORM_TOP_Y + r * PIX_FORM_ROW_DY + self.pix_form_offset_y

    @staticmethod
    def _pix_row_color(r: int):
        if r == 0:
            return PIX_COL_MAGENTA
        if r <= 2:
            return PIX_COL_TURQUOISE
        return PIX_COL_GREEN

    @staticmethod
    def _pix_row_sprite(r: int, frame: int):
        if r == 0:
            return PIX_SPR_TOP[frame]
        if r <= 2:
            return PIX_SPR_MID[frame]
        return PIX_SPR_BOT[frame]

    @staticmethod
    def _pix_shield_x(i: int) -> int:
        return 40 + i * 70

    def _pix_alive_count(self) -> int:
        return sum(1 for r in range(PIX_ROWS) for c in range(PIX_COLS) if self.pix_alive[r][c])

    @staticmethod
    def _pix_draw_sprite(painter: QPainter, cx: int, cy: int, rows, row_count: int, color_rgb, px: int) -> None:
        """8列幅のドット絵を、1論理ピクセル=px画面pxのベタ塗り矩形として描く。CoreS3
        pixDrawSprite()と同一ロジック（bit7が左端、bit0が右端）。"""
        w, h = 8 * px, row_count * px
        x0, y0 = cx - w // 2, cy - h // 2
        col = QColor(*color_rgb)
        for r in range(row_count):
            bits = rows[r]
            for c in range(8):
                if bits & (0x80 >> c):
                    painter.fillRect(x0 + c * px, y0 + r * px, px, px, col)

    def _pix_reset_shields(self) -> None:
        for s in range(PIX_SHIELD_COUNT):
            for r in range(PIX_SHIELD_ROWS):
                self.pix_shield_row_mask[s][r] = PIX_SHIELD_BASE[r]

    def _pix_reset_formation(self) -> None:
        for r in range(PIX_ROWS):
            for c in range(PIX_COLS):
                self.pix_alive[r][c] = True
                self.pix_kill_flash_until[r][c] = 0
        self.pix_form_offset_x = 0
        self.pix_form_offset_y = 0
        self.pix_form_dir = 1
        self.pix_anim_frame = 0

    def _pix_reset_round(self, now_ms: int) -> None:
        self._pix_reset_formation()
        self._pix_reset_shields()
        for i in range(PIX_PBULLET_MAX):
            self.pix_pbullet_active[i] = False
        for i in range(PIX_EBULLET_MAX):
            self.pix_ebullet_active[i] = False
        self.pix_round_start_ms = now_ms
        self.pix_next_step_at = now_ms + 200
        self.pix_next_enemy_fire_at = now_ms + PIX_ENEMY_FIRE_MIN_MS + int(self._pix_rand01() * PIX_ENEMY_FIRE_JITTER_MS)

    def _pix_init_all(self, now_ms: int) -> None:
        self.pix_player_x = 160.0
        self.pix_player_dir = 1
        self.pix_player_next_step_at = now_ms
        self.pix_player_next_fire_at = now_ms + PIX_PLAYER_FIRE_MIN_MS
        self.pix_ufo_active = False
        self.pix_ufo_next_at = now_ms + PIX_UFO_MIN_GAP_MS + int(self._pix_rand01() * PIX_UFO_GAP_JITTER_MS)
        self._pix_reset_round(now_ms)
        self.pix_ready = True

    def _pix_step_formation(self, now_ms: int) -> None:
        """編隊のステップ移動（一定ピクセルずつ進む「カッ、カッ、カッ」という刻み）。端に達したら
        移動せず反転＋下降だけを行う（連続スクロールにしない＝CoreS3と同じ挙動）。"""
        if now_ms < self.pix_next_step_at:
            return
        alive_n = self._pix_alive_count()
        step_ms = 180 + alive_n * 10
        if step_ms > 560:
            step_ms = 560
        self.pix_next_step_at = now_ms + step_ms

        new_offset_x = self.pix_form_offset_x + self.pix_form_dir * PIX_STEP_PX
        left_edge = PIX_FORM_LEFT_X0 + 0 * PIX_FORM_COL_DX + new_offset_x - PIX_INV_HALF
        right_edge = PIX_FORM_LEFT_X0 + (PIX_COLS - 1) * PIX_FORM_COL_DX + new_offset_x + PIX_INV_HALF
        if left_edge < PIX_FORM_MARGIN or right_edge > CANVAS_W - PIX_FORM_MARGIN:
            self.pix_form_dir = -self.pix_form_dir
            self.pix_form_offset_y += PIX_DROP_PX
        else:
            self.pix_form_offset_x = new_offset_x
        self.pix_anim_frame ^= 1

    def _pix_check_round_reset(self, now_ms: int) -> None:
        """生存数が少ない／編隊が下降しすぎた／時間経過、のいずれかで新しい編隊へ。"""
        alive_n = self._pix_alive_count()
        too_low = alive_n <= PIX_RESET_ALIVE_MIN
        descended = self._pix_invader_y(PIX_ROWS - 1) >= (PIX_SHIELD_Y - 10)
        timed_out = (now_ms - self.pix_round_start_ms) >= PIX_ROUND_MAX_MS
        if too_low or descended or timed_out:
            self._pix_reset_round(now_ms)

    def _pix_update_player(self, now_ms: int) -> None:
        if now_ms < self.pix_player_next_step_at:
            return
        self.pix_player_next_step_at = now_ms + PIX_PLAYER_STEP_MS
        self.pix_player_x += self.pix_player_dir * PIX_PLAYER_STEP_PX
        if self.pix_player_x < PIX_PLAYER_MARGIN:
            self.pix_player_x = PIX_PLAYER_MARGIN
            self.pix_player_dir = 1
        if self.pix_player_x > CANVAS_W - PIX_PLAYER_MARGIN:
            self.pix_player_x = CANVAS_W - PIX_PLAYER_MARGIN
            self.pix_player_dir = -1

    def _pix_update_player_fire(self, now_ms: int) -> None:
        if now_ms < self.pix_player_next_fire_at:
            return
        self.pix_player_next_fire_at = now_ms + PIX_PLAYER_FIRE_MIN_MS + int(self._pix_rand01() * PIX_PLAYER_FIRE_JITTER_MS)
        if not self.pix_pbullet_active[0]:
            self.pix_pbullet_active[0] = True
            self.pix_pbullet_x[0] = self.pix_player_x
            self.pix_pbullet_y[0] = PIX_PLAYER_Y - 10.0

    def _pix_update_ufo(self, now_ms: int, dt: float) -> None:
        if not self.pix_ufo_active:
            if now_ms >= self.pix_ufo_next_at:
                self.pix_ufo_active = True
                self.pix_ufo_dir = 1 if (self._pix_rand() & 1) else -1
                self.pix_ufo_x = -20.0 if self.pix_ufo_dir > 0 else float(CANVAS_W) + 20.0
            return
        self.pix_ufo_x += self.pix_ufo_dir * PIX_UFO_SPEED * dt
        if (self.pix_ufo_dir > 0 and self.pix_ufo_x > float(CANVAS_W) + 20.0) or (
            self.pix_ufo_dir < 0 and self.pix_ufo_x < -20.0
        ):
            self.pix_ufo_active = False
            self.pix_ufo_next_at = now_ms + PIX_UFO_MIN_GAP_MS + int(self._pix_rand01() * PIX_UFO_GAP_JITTER_MS)

    def _pix_spawn_enemy_bullet(self, now_ms: int) -> None:
        """生存している列からランダムに1つ選び、その列でもっとも手前（下側）の敵から弾を発射する。"""
        if now_ms < self.pix_next_enemy_fire_at:
            return
        self.pix_next_enemy_fire_at = now_ms + PIX_ENEMY_FIRE_MIN_MS + int(self._pix_rand01() * PIX_ENEMY_FIRE_JITTER_MS)

        slot = -1
        for i in range(PIX_EBULLET_MAX):
            if not self.pix_ebullet_active[i]:
                slot = i
                break
        if slot < 0:
            return

        alive_cols = [c for c in range(PIX_COLS) if any(self.pix_alive[r][c] for r in range(PIX_ROWS))]
        if not alive_cols:
            return
        c = alive_cols[self._pix_rand() % len(alive_cols)]
        shooter_row = -1
        for r in range(PIX_ROWS - 1, -1, -1):
            if self.pix_alive[r][c]:
                shooter_row = r
                break
        if shooter_row < 0:
            return

        self.pix_ebullet_active[slot] = True
        self.pix_ebullet_x[slot] = float(self._pix_invader_x(c))
        self.pix_ebullet_y[slot] = float(self._pix_invader_y(shooter_row)) + float(PIX_INV_HALF)

    def _pix_shield_hit(self, s: int, wx: float, wy: float) -> bool:
        """シールドの1セルに弾が当たったかを判定し、当たっていればそのセルだけを消してTrueを
        返す（既に空のセルなら弾はそのまま素通りする＝False）。"""
        local_x = int(wx - self._pix_shield_x(s))
        local_y = int(wy - PIX_SHIELD_Y)
        if local_x < 0 or local_y < 0:
            return False
        c = local_x // PIX_SHIELD_CELL
        r = local_y // PIX_SHIELD_CELL
        if c < 0 or c >= PIX_SHIELD_COLS or r < 0 or r >= PIX_SHIELD_ROWS:
            return False
        bit = 1 << c
        if not (self.pix_shield_row_mask[s][r] & bit):
            return False
        self.pix_shield_row_mask[s][r] &= (~bit) & 0xFFFF
        return True

    def _pix_update_bullets(self, now_ms: int, dt: float) -> None:
        # ── 自機弾：上へ進み、シールド／敵編隊との当たりを判定 ──
        for i in range(PIX_PBULLET_MAX):
            if not self.pix_pbullet_active[i]:
                continue
            self.pix_pbullet_y[i] -= PIX_PBULLET_SPEED * dt
            if self.pix_pbullet_y[i] < PIX_TOP:
                self.pix_pbullet_active[i] = False
                continue

            absorbed = False
            for s in range(PIX_SHIELD_COUNT):
                if absorbed:
                    break
                if self._pix_shield_hit(s, self.pix_pbullet_x[i], self.pix_pbullet_y[i]):
                    absorbed = True
            if absorbed:
                self.pix_pbullet_active[i] = False
                continue

            for r in range(PIX_ROWS):
                if not self.pix_pbullet_active[i]:
                    break
                for c in range(PIX_COLS):
                    if not self.pix_pbullet_active[i]:
                        break
                    if not self.pix_alive[r][c]:
                        continue
                    ix, iy = self._pix_invader_x(c), self._pix_invader_y(r)
                    if (
                        ix - PIX_INV_HALF <= self.pix_pbullet_x[i] <= ix + PIX_INV_HALF
                        and iy - PIX_INV_HALF <= self.pix_pbullet_y[i] <= iy + PIX_INV_HALF
                    ):
                        self.pix_alive[r][c] = False
                        self.pix_kill_flash_until[r][c] = now_ms + PIX_KILL_FLASH_MS
                        self.pix_pbullet_active[i] = False

        # ── 敵弾：下へ進み、シールドとの当たりのみ判定（自機には当てない＝ゲームオーバー無し）──
        for i in range(PIX_EBULLET_MAX):
            if not self.pix_ebullet_active[i]:
                continue
            self.pix_ebullet_y[i] += PIX_EBULLET_SPEED * dt
            if self.pix_ebullet_y[i] > PIX_PLAYER_Y - 4.0:
                self.pix_ebullet_active[i] = False
                continue
            for s in range(PIX_SHIELD_COUNT):
                if self._pix_shield_hit(s, self.pix_ebullet_x[i], self.pix_ebullet_y[i]):
                    self.pix_ebullet_active[i] = False
                    break

    def _pix_draw_shield(self, painter: QPainter, s: int) -> None:
        base_x = self._pix_shield_x(s)
        col = QColor(*PIX_COL_SHIELD)
        for r in range(PIX_SHIELD_ROWS):
            mask = self.pix_shield_row_mask[s][r]
            for c in range(PIX_SHIELD_COLS):
                if mask & (1 << c):
                    painter.fillRect(
                        base_x + c * PIX_SHIELD_CELL, PIX_SHIELD_Y + r * PIX_SHIELD_CELL, PIX_SHIELD_CELL, PIX_SHIELD_CELL, col
                    )

    def _pix_draw_formation(self, painter: QPainter, now_ms: int) -> None:
        white = QColor(255, 255, 255)
        for r in range(PIX_ROWS):
            for c in range(PIX_COLS):
                cx, cy = self._pix_invader_x(c), self._pix_invader_y(r)
                if self.pix_alive[r][c]:
                    self._pix_draw_sprite(
                        painter, cx, cy, self._pix_row_sprite(r, self.pix_anim_frame), 8, self._pix_row_color(r), PIX_INV_CELL
                    )
                elif now_ms < self.pix_kill_flash_until[r][c]:
                    # ごく短い数フレームの撃墜フラッシュ（白いピクセルの点滅のみ。爆発演出は無し）
                    painter.fillRect(cx - PIX_INV_HALF, cy - PIX_INV_HALF, PIX_INV_HALF * 2, PIX_INV_HALF * 2, white)

    def _pix_draw_player(self, painter: QPainter) -> None:
        self._pix_draw_sprite(painter, int(self.pix_player_x), PIX_PLAYER_Y, PIX_SPR_PLAYER, 6, PIX_COL_PLAYER, PIX_PLAYER_CELL)

    def _pix_draw_ufo(self, painter: QPainter) -> None:
        if not self.pix_ufo_active:
            return
        self._pix_draw_sprite(painter, int(self.pix_ufo_x), PIX_UFO_Y, PIX_SPR_UFO, 5, PIX_COL_UFO, PIX_UFO_CELL)

    def _pix_draw_bullets(self, painter: QPainter) -> None:
        pcol = QColor(*PIX_COL_PBULLET)
        ecol = QColor(*PIX_COL_EBULLET)
        for i in range(PIX_PBULLET_MAX):
            if not self.pix_pbullet_active[i]:
                continue
            painter.fillRect(int(self.pix_pbullet_x[i]) - 1, int(self.pix_pbullet_y[i]) - 3, 2, 6, pcol)
        for i in range(PIX_EBULLET_MAX):
            if not self.pix_ebullet_active[i]:
                continue
            painter.fillRect(int(self.pix_ebullet_x[i]) - 1, int(self.pix_ebullet_y[i]) - 3, 2, 6, ecol)

    def _light_render_pixelinvasion(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if not self.pix_ready:
            self._pix_init_all(now_ms)

        if needs_init or self.pix_prev_ms == 0:
            self.pix_prev_ms = now_ms
        dt = (now_ms - self.pix_prev_ms) / 1000.0
        if dt > 0.5:
            dt = 0.5
        self.pix_prev_ms = now_ms

        self._pix_step_formation(now_ms)
        self._pix_update_player(now_ms)
        self._pix_update_player_fire(now_ms)
        self._pix_update_ufo(now_ms, dt)
        self._pix_spawn_enemy_bullet(now_ms)
        self._pix_update_bullets(now_ms, dt)
        self._pix_check_round_reset(now_ms)

        painter.save()
        painter.setClipRect(0, PIX_TOP, CANVAS_W, CANVAS_H - PIX_TOP)
        painter.fillRect(0, PIX_TOP, CANVAS_W, CANVAS_H - PIX_TOP, QColor(0, 0, 0))  # 完全な黒背景のみ

        for s in range(PIX_SHIELD_COUNT):
            self._pix_draw_shield(painter, s)
        self._pix_draw_formation(painter, now_ms)
        self._pix_draw_player(painter)
        self._pix_draw_ufo(painter)
        self._pix_draw_bullets(painter)

        painter.restore()

    # ----- Lighting: Psychedelic / Trance（v0.5 Phase 4残り。CoreS3
    # lightRenderPsychedelic()準拠。MOTION 5種・FLASH 4種・ACCENT 5種の三層モンタージュ）-----
    def _psy_rnd_f(self, lo: float, hi: float) -> float:
        return lo + (hi - lo) * random.random()

    def _psy_rnd_i(self, lo: int, hi: int) -> int:
        return random.randint(lo, hi)

    def _psy_chance(self, pct: int) -> bool:
        return random.randint(0, 99) < pct

    def _psy_lcg_seed(self, s: int) -> None:
        self.psy_lcg = s if s else 1

    def _psy_lcg_next(self) -> int:
        x = self.psy_lcg & 0xFFFFFFFF
        x = (x ^ (x << 13)) & 0xFFFFFFFF
        x = x ^ (x >> 17)
        x = (x ^ (x << 5)) & 0xFFFFFFFF
        self.psy_lcg = x
        return x

    def _psy_lcg_f(self, lo: float, hi: float) -> float:
        return lo + (hi - lo) * ((self._psy_lcg_next() & 0xFFFF) / 65535.0)

    def _psy_lcg_i(self, lo: int, hi: int) -> int:
        if hi <= lo:
            return lo
        return lo + (self._psy_lcg_next() % (hi - lo + 1))

    @staticmethod
    def _psy_inv(rgb) -> tuple:
        return (255 - rgb[0], 255 - rgb[1], 255 - rgb[2])

    # MOTION-A: Expanding Rings
    def _psy_ring_build_pool(self) -> None:
        P = PSY_PAL_RGB[self.psy_pal]
        start = self._psy_rnd_i(0, 11)
        span = self._psy_rnd_i(5, 7)
        pool = []
        for i in range(span):
            if len(pool) >= PSY_RING_POOL_MAX:
                break
            step = self._psy_rnd_i(1, 2)
            pool.append(PSY_SPECTRUM_RGB[(start + i * step) % 12])
        if len(pool) < PSY_RING_POOL_MAX:
            pool.append(P["mn"])
        if len(pool) < PSY_RING_POOL_MAX:
            pool.append(P["ac"])
        if len(pool) < PSY_RING_POOL_MAX:
            pool.append((255, 255, 255))
        self.psy_ring_pool = pool

    def _psy_ring_pick_color(self):
        if self.psy_ring_run > 0:
            self.psy_ring_run -= 1
            return self.psy_ring_run_col
        if not self.psy_ring_pool:
            return (255, 255, 255)
        n = len(self.psy_ring_pool)
        total = sum((n - i) * 3 + 2 for i in range(n))
        pick = random.randint(0, total - 1)
        acc = 0
        col = self.psy_ring_pool[0]
        for i in range(n):
            acc += (n - i) * 3 + 2
            if pick < acc:
                col = self.psy_ring_pool[i]
                break
        if self._psy_chance(PSY_RING_RUN_PCT):
            self.psy_ring_run = self._psy_rnd_i(1, 2)
            self.psy_ring_run_col = col
        return col

    def _psy_ring_pick_width(self) -> int:
        p = random.randint(0, 99)
        if p < PSY_RING_W_THIN_PCT:
            return self._psy_rnd_i(PSY_RING_W_THIN_MIN, PSY_RING_W_THIN_MAX)
        if p < PSY_RING_W_THIN_PCT + PSY_RING_W_MID_PCT:
            return self._psy_rnd_i(PSY_RING_W_MID_MIN, PSY_RING_W_MID_MAX)
        return self._psy_rnd_i(PSY_RING_W_FAT_MIN, PSY_RING_W_FAT_MAX)

    def _psy_ring_spawn(self, r0: float, v0: float) -> None:
        if len(self.psy_ring) >= PSY_RING_MAX:
            return
        self.psy_ring.append({
            "r": r0, "v": v0,
            "cx": self.psy_ring_bx + self._psy_rnd_f(-PSY_RING_JITTER, PSY_RING_JITTER),
            "cy": self.psy_ring_by + self._psy_rnd_f(-PSY_RING_JITTER, PSY_RING_JITTER),
            "dx": self._psy_rnd_f(-PSY_RING_DRIFT_X, PSY_RING_DRIFT_X),
            "dy": self._psy_rnd_f(-PSY_RING_DRIFT_Y, PSY_RING_DRIFT_Y),
            "ell": self._psy_rnd_f(PSY_RING_ELL_MIN, PSY_RING_ELL_MAX),
            "gro": self._psy_rnd_f(PSY_RING_GRO_MIN, PSY_RING_GRO_MAX),
            "col": self._psy_ring_pick_color(),
            "wid": self._psy_ring_pick_width(),
        })

    def _psy_rings_init(self) -> None:
        self.psy_ring_bx = self._psy_rnd_f(100.0, 220.0)
        self.psy_ring_by = self._psy_rnd_f(100.0, 190.0)
        self.psy_ring_bvx = self._psy_rnd_f(-PSY_RING_BIRTH_VX, PSY_RING_BIRTH_VX)
        self.psy_ring_bvy = self._psy_rnd_f(-PSY_RING_BIRTH_VY, PSY_RING_BIRTH_VY)
        self.psy_ring = []
        self.psy_ring_run = 0
        self._psy_ring_build_pool()
        r, v = 4.5, 4.5
        for _ in range(min(PSY_RING_PRELOAD, PSY_RING_MAX)):
            self._psy_ring_spawn(r, v)
            r *= 1.115 * 1.115
            v *= 1.05 * 1.05

    def _psy_rings_step(self, ramp: float) -> None:
        rs = math.sqrt(ramp)
        kept = []
        for ring in self.psy_ring:
            ring["v"] = ring["v"] * ring["gro"]
            if ring["v"] > PSY_RING_V_CLAMP:
                ring["v"] = PSY_RING_V_CLAMP
            ring["r"] += ring["v"] * rs
            ring["cx"] += ring["dx"] * ramp
            ring["cy"] += ring["dy"] * ramp
            if ring["r"] < PSY_RING_KILL_R:
                kept.append(ring)
        self.psy_ring = kept
        self.psy_ring_bx += self.psy_ring_bvx * ramp
        self.psy_ring_by += self.psy_ring_bvy * ramp
        if self.psy_ring_bx < 80.0 or self.psy_ring_bx > 240.0:
            self.psy_ring_bvx = -self.psy_ring_bvx
        if self.psy_ring_by < 95.0 or self.psy_ring_by > 195.0:
            self.psy_ring_bvy = -self.psy_ring_bvy
        spawn = PSY_RING_SPAWN_LOW if len(self.psy_ring) < PSY_RING_LOW_COUNT else PSY_RING_SPAWN_HI
        for _ in range(spawn):
            self._psy_ring_spawn(PSY_RING_R0, PSY_RING_V0)

    def _psy_rings_draw(self, painter: QPainter, inv: bool) -> None:
        P = PSY_PAL_RGB[self.psy_pal]
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["bg"]))
        for big_pass in (True, False):
            for ring in self.psy_ring:
                big = ring["r"] >= 90.0
                if big != big_pass:
                    continue
                rx = max(1, int(ring["r"]))
                ry = max(1, int(ring["r"] * ring["ell"]))
                th = max(1, int(ring["wid"] * (0.45 + ring["r"] / PSY_RING_KILL_R)))
                col = self._psy_inv(ring["col"]) if inv else ring["col"]
                pen = QPen(QColor(*col))
                pen.setWidth(1)
                painter.setPen(pen)
                painter.setBrush(Qt.NoBrush)
                cx, cy = int(ring["cx"]), int(ring["cy"])
                for k in range(th):
                    painter.drawEllipse(QPointF(cx, cy), rx + k, ry + k)

    # MOTION-B: 偏心スパイラル
    def _psy_spiral_init(self) -> None:
        self.psy_sp_cx = self._psy_rnd_f(70.0, 250.0)
        self.psy_sp_cy = self._psy_rnd_f(90.0, 200.0)
        self.psy_sp_arms = self._psy_rnd_i(6, 9)
        self.psy_sp_twist = self._psy_rnd_f(0.012, 0.018)
        self.psy_sp_ell = self._psy_rnd_f(0.70, 1.30)
        self.psy_sp_ph = self._psy_rnd_f(0.0, 6.2831853)
        self.psy_sp_spin = (1.0 if self._psy_chance(50) else -1.0) * 0.155
        self.psy_sp_reach = 180.0
        self.psy_sp_skip = self._psy_rnd_i(0, self.psy_sp_arms - 1)
        self.psy_sp_flip_at = self._psy_rnd_i(6, 12)
        self.psy_sp_f = 0

    def _psy_spiral_step(self, ramp: float) -> None:
        self.psy_sp_f += 1
        if self.psy_sp_f == self.psy_sp_flip_at:
            self.psy_sp_spin *= -1.35
        self.psy_sp_ph += self.psy_sp_spin * ramp
        self.psy_sp_cx += 1.7 * ramp
        self.psy_sp_cy += 0.85 * ramp
        self.psy_sp_twist += 0.00022 * ramp
        self.psy_sp_reach *= (1.0 + 0.017 * ramp)
        if self.psy_sp_reach > 300.0:
            self.psy_sp_reach = 300.0
        if self.psy_sp_ph > 62.83:
            self.psy_sp_ph -= 62.83
        if self.psy_sp_ph < -62.83:
            self.psy_sp_ph += 62.83

    def _psy_spiral_draw(self, painter: QPainter, inv: bool) -> None:
        P = PSY_PAL_RGB[self.psy_pal]
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["bg"]))
        c_a, c_b = (P["ac"], P["mn"]) if inv else (P["mn"], P["ac"])
        col_a, col_b = QColor(*c_a), QColor(*c_b)
        n = 18 + int(self.psy_sp_reach / 11.0)
        if n > 45:
            n = 45
        for a in range(self.psy_sp_arms):
            if a == self.psy_sp_skip:
                continue
            col = col_b if (a & 1) else col_a
            base = self.psy_sp_ph + a * 6.2831853 / self.psy_sp_arms
            for i in range(n):
                t = i / float(n - 1) if n > 1 else 0.0
                r = 6.0 + (t ** 0.85) * self.psy_sp_reach
                th = base + r * self.psy_sp_twist
                x = int(self.psy_sp_cx + r * math.cos(th))
                y = int(self.psy_sp_cy + r * math.sin(th) * self.psy_sp_ell)
                w = int(3.0 + 13.0 * t)
                self._fill_circle(painter, x, y, w, col)

    # MOTION-C: モアレ干渉
    def _psy_moire_init(self) -> None:
        self.psy_mo_ax = self._psy_rnd_f(120.0, 190.0)
        self.psy_mo_ay = self._psy_rnd_f(110.0, 170.0)
        from_left = self._psy_chance(50)
        self.psy_mo_bx = -30.0 if from_left else 350.0
        self.psy_mo_by = self._psy_rnd_f(95.0, 190.0)
        self.psy_mo_bvx = 40.0 if from_left else -40.0
        self.psy_mo_bvy = self._psy_rnd_f(-4.0, 4.0)
        self.psy_mo_skew = 0.0
        self.psy_mo_dsk = self._psy_rnd_f(0.030, 0.048)
        self.psy_mo_na = 44.0
        self.psy_mo_nb = 40.0

    def _psy_moire_step(self, ramp: float) -> None:
        self.psy_mo_bx += self.psy_mo_bvx * ramp
        self.psy_mo_by += self.psy_mo_bvy * ramp
        self.psy_mo_skew += self.psy_mo_dsk * ramp
        self.psy_mo_ax += 1.4 * ramp
        self.psy_mo_na -= 1.1 * ramp
        if self.psy_mo_na < 18.0:
            self.psy_mo_na = 18.0
        self.psy_mo_nb -= 0.9 * ramp
        if self.psy_mo_nb < 16.0:
            self.psy_mo_nb = 16.0

    def _psy_moire_draw(self, painter: QPainter, inv: bool) -> None:
        P = PSY_PAL_RGB[self.psy_pal]
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["bg"]))
        c_a = P["ac"] if inv else P["mn"]
        c_b = P["mn"] if inv else P["ac"]
        na, nb = int(self.psy_mo_na), int(self.psy_mo_nb)
        col_a = QColor(*c_a)
        for i in range(na):
            a = i * 3.14159265 / na
            c = math.cos(a) * 560.0
            s = math.sin(a) * 560.0
            self._draw_thick_line(painter, int(self.psy_mo_ax - c), int(self.psy_mo_ay - s),
                                   int(self.psy_mo_ax + c), int(self.psy_mo_ay + s), 1, col_a)
        col_b = QColor(*c_b)
        for i in range(nb):
            a = i * 3.14159265 / nb + self.psy_mo_skew
            c = math.cos(a) * 560.0
            s = math.sin(a) * 560.0
            self._draw_thick_line(painter, int(self.psy_mo_bx - c), int(self.psy_mo_by - s),
                                   int(self.psy_mo_bx + c), int(self.psy_mo_by + s), 1, col_b)

    # MOTION-D: 不規則放射ウェッジ
    def _psy_wedge_init(self) -> None:
        self.psy_wd_x = self._psy_rnd_f(30.0, 290.0)
        self.psy_wd_y = self._psy_rnd_f(70.0, 220.0)
        self.psy_wd_n = self._psy_rnd_i(13, 19)
        self.psy_wd_raw = [self._psy_rnd_f(0.5, 1.6) for _ in range(self.psy_wd_n)]
        self.psy_wd_ph = self._psy_rnd_f(0.0, 6.2831853)
        self.psy_wd_spin = (1.0 if self._psy_chance(50) else -1.0) * self._psy_rnd_f(0.17, 0.27)
        self.psy_wd_vx = self._psy_rnd_f(-9.0, 9.0)
        self.psy_wd_vy = self._psy_rnd_f(-6.0, 6.0)
        self.psy_wd_breathe = 0.0

    def _psy_wedge_step(self, ramp: float) -> None:
        self.psy_wd_ph += self.psy_wd_spin * ramp
        self.psy_wd_x += self.psy_wd_vx * ramp
        self.psy_wd_y += self.psy_wd_vy * ramp
        if self.psy_wd_x < 10.0 or self.psy_wd_x > 310.0:
            self.psy_wd_vx = -self.psy_wd_vx
        if self.psy_wd_y < 60.0 or self.psy_wd_y > 230.0:
            self.psy_wd_vy = -self.psy_wd_vy
        self.psy_wd_breathe += 0.55 * ramp
        k = int(self.psy_wd_breathe) % self.psy_wd_n
        self.psy_wd_raw[k] *= 1.16
        if self.psy_wd_raw[k] > 2.2:
            self.psy_wd_raw[k] = 2.2
        if self.psy_wd_ph > 62.83:
            self.psy_wd_ph -= 62.83
        if self.psy_wd_ph < -62.83:
            self.psy_wd_ph += 62.83

    def _psy_wedge_draw(self, painter: QPainter, inv: bool) -> None:
        P = PSY_PAL_RGB[self.psy_pal]
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["bg"]))
        c_a = P["ac"] if inv else P["mn"]
        c_b = P["mn"] if inv else P["ac"]
        bg_col = QColor(*P["bg"])
        col_a, col_b = QColor(*c_a), QColor(*c_b)
        total = sum(self.psy_wd_raw)
        if total <= 0.0:
            return
        a = self.psy_wd_ph
        ax, ay = self.psy_wd_x, self.psy_wd_y
        for i in range(self.psy_wd_n):
            a1 = a
            a += self.psy_wd_raw[i] / total * 6.2831853
            a2 = a
            if i % 9 == 4:
                col = bg_col
            else:
                col = col_b if (i & 1) else col_a
            for s in range(3):
                t0 = a1 + (a2 - a1) * s / 3.0
                t1 = a1 + (a2 - a1) * (s + 1) / 3.0
                self._poly(painter, [
                    (ax, ay),
                    (ax + math.cos(t0) * 500.0, ay + math.sin(t0) * 500.0),
                    (ax + math.cos(t1) * 500.0, ay + math.sin(t1) * 500.0),
                ], col)

    # MOTION-E: 破片三角
    def _psy_shard_init(self) -> None:
        tris = []
        for i in range(3):
            xs = [self._psy_rnd_f(-120, 0), self._psy_rnd_f(320, 440), self._psy_rnd_f(0, 320)]
            ys = [self._psy_rnd_f(-60, 300), self._psy_rnd_f(-60, 300), self._psy_rnd_f(-120, 420)]
            tris.append({"x": xs, "y": ys, "vx": self._psy_rnd_f(-7, 7), "vy": self._psy_rnd_f(-5, 5), "col": i % 3})
        for i in range(3, PSY_TRI_N):
            cx, cy, sz = self._psy_rnd_f(0, 320), self._psy_rnd_f(PSY_TOP, 240), self._psy_rnd_f(14, 52)
            xs = [cx + self._psy_rnd_f(-sz, sz) for _ in range(3)]
            ys = [cy + self._psy_rnd_f(-sz, sz) for _ in range(3)]
            tris.append({"x": xs, "y": ys, "vx": self._psy_rnd_f(-11, 11), "vy": self._psy_rnd_f(-8, 8), "col": (i + 1) % 3})
        self.psy_tri = tris

    def _psy_shard_step(self, ramp: float) -> None:
        th = 0.09
        cs = math.cos(th * ramp)
        sn = math.sin(th * ramp)
        for t in self.psy_tri:
            t["vx"] *= 1.06
            t["vy"] *= 1.06
            cx = sum(t["x"]) / 3.0
            cy = sum(t["y"]) / 3.0
            for k in range(3):
                dx = t["x"][k] - cx
                dy = t["y"][k] - cy
                t["x"][k] = cx + dx * cs - dy * sn + t["vx"] * ramp
                t["y"][k] = cy + dx * sn + dy * cs + t["vy"] * ramp

    def _psy_shard_draw(self, painter: QPainter, inv: bool) -> None:
        P = PSY_PAL_RGB[self.psy_pal]
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["bg"]))
        c0 = P["ac"] if inv else P["mn"]
        c1 = P["mn"] if inv else P["ac"]
        col0, col1, white = QColor(*c0), QColor(*c1), QColor(255, 255, 255)
        for t in self.psy_tri:
            col = col0 if t["col"] == 0 else (col1 if t["col"] == 1 else white)
            self._poly(painter, list(zip(t["x"], t["y"])), col)

    # MOTION共通ディスパッチ
    def _psy_motion_init(self, k: int) -> None:
        if k == PSY_M_RINGS:
            self._psy_rings_init()
        elif k == PSY_M_SPIRAL:
            self._psy_spiral_init()
        elif k == PSY_M_MOIRE:
            self._psy_moire_init()
        elif k == PSY_M_WEDGE:
            self._psy_wedge_init()
        else:
            self._psy_shard_init()

    def _psy_motion_step(self, k: int, ramp: float) -> None:
        if k == PSY_M_RINGS:
            self._psy_rings_step(ramp)
        elif k == PSY_M_SPIRAL:
            self._psy_spiral_step(ramp)
        elif k == PSY_M_MOIRE:
            self._psy_moire_step(ramp)
        elif k == PSY_M_WEDGE:
            self._psy_wedge_step(ramp)
        else:
            self._psy_shard_step(ramp)

    def _psy_motion_draw(self, painter: QPainter, k: int, inv: bool) -> None:
        if k == PSY_M_RINGS:
            self._psy_rings_draw(painter, inv)
        elif k == PSY_M_SPIRAL:
            self._psy_spiral_draw(painter, inv)
        elif k == PSY_M_MOIRE:
            self._psy_moire_draw(painter, inv)
        elif k == PSY_M_WEDGE:
            self._psy_wedge_draw(painter, inv)
        else:
            self._psy_shard_draw(painter, inv)

    # FLASH（静止の一撃。psyShotSeedから決定的に再生成し、1〜3フレームのあいだ同じ絵を出す）
    def _psy_flash_opgrid(self, painter: QPainter, P: dict) -> None:
        cols = self._psy_lcg_i(9, 14)
        rows = self._psy_lcg_i(6, 9)
        amp = float(self._psy_lcg_i(10, 26))
        ph = self._psy_lcg_f(0.0, 6.28)
        cw = []
        total = 0.0
        for i in range(cols):
            v = 6.0 + 34.0 * abs(math.sin(i * 0.9 + ph))
            cw.append(v)
            total += v
        y = float(PSY_TOP)
        mn_col, ac_col = QColor(*P["mn"]), QColor(*P["ac"])
        for j in range(rows):
            rh = (240 - PSY_TOP) / float(rows) * (0.6 + 0.8 * abs(math.sin(j * 1.1 + ph * 0.7)))
            x = -40.0 + amp * math.sin(j * 0.8 + ph)
            i = 0
            while x < 320.0 and i < 64:
                wd = cw[i % cols] * (320.0 / total * 1.15)
                painter.fillRect(int(x), int(y), int(wd) + 1, int(rh) + 1, ac_col if ((i + j) & 1) else mn_col)
                x += wd
                i += 1
            y += rh

    def _psy_flash_dots(self, painter: QPainter, P: dict) -> None:
        n_big = self._psy_lcg_i(1, 2)
        n_mid = self._psy_lcg_i(2, 3)
        n_sml = self._psy_lcg_i(3, 5)
        qx = self._psy_lcg_i(0, 1) * 160
        qy = PSY_TOP + self._psy_lcg_i(0, 1) * 96
        mn_col, ac_col, bg_col = QColor(*P["mn"]), QColor(*P["ac"]), QColor(*P["bg"])
        for i in range(n_big):
            x = self._psy_lcg_i(-60, 380)
            y = self._psy_lcg_i(20, 260)
            r = self._psy_lcg_i(85, 150)
            self._fill_circle(painter, x, y, r, ac_col if (i & 1) else mn_col)
            ir = r * 48 // 100
            self._fill_circle(painter, x + self._psy_lcg_i(-r // 3, r // 3), y + self._psy_lcg_i(-r // 3, r // 3),
                               ir, mn_col if (i & 1) else bg_col)
        for i in range(n_mid):
            x = self._psy_lcg_i(0, 320)
            y = self._psy_lcg_i(PSY_TOP, 240)
            r = self._psy_lcg_i(25, 50)
            self._fill_circle(painter, x, y, r, mn_col if (i & 1) else ac_col)
            self._fill_circle(painter, x + self._psy_lcg_i(-r // 3, r // 3), y + self._psy_lcg_i(-r // 3, r // 3),
                               r * 55 // 100, bg_col if (i & 1) else mn_col)
        for i in range(n_sml):
            self._fill_circle(painter, qx + self._psy_lcg_i(0, 160), qy + self._psy_lcg_i(0, 96),
                               self._psy_lcg_i(6, 14), ac_col if (i & 1) else mn_col)

    def _psy_flash_hatch(self, painter: QPainter, P: dict) -> None:
        ang = self._psy_lcg_i(0, 170) * 0.0174533
        dd = self._psy_lcg_i(3, 7) * 0.0174533
        mn_col, ac_col = QColor(*P["mn"]), QColor(*P["ac"])
        for f in range(2):
            a = ang if f == 0 else ang + dd
            sp = 7 if f == 0 else 9
            dx, dy = math.cos(a), math.sin(a)
            col = mn_col if f == 0 else ac_col
            for k in range(-60, 60):
                px = 160.0 - dy * (k * sp)
                py = 144.0 + dx * (k * sp)
                self._draw_thick_line(painter, int(px - dx * 500.0), int(py - dy * 500.0),
                                       int(px + dx * 500.0), int(py + dy * 500.0), 1, col)

    def _psy_flash_wobble(self, painter: QPainter, P: dict) -> None:
        cols = [QColor(*P["mn"]), QColor(*P["ac"]), QColor(*P["bg"])]
        bands = self._psy_lcg_i(4, 6)
        ph = self._psy_lcg_f(0.0, 6.28)
        x = 0
        while x < 320:
            for b in range(bands):
                k0 = b * 0.9
                k1 = (b + 1) * 0.9
                o0 = (26.0 * math.sin((x + ph * 40.0) / 47.0 * 6.2831853 + k0)
                      + 16.0 * math.sin(x / 61.0 * 6.2831853 + k0 * 1.7)
                      + 10.0 * math.sin(x / 93.0 * 6.2831853))
                o1 = (26.0 * math.sin((x + ph * 40.0) / 47.0 * 6.2831853 + k1)
                      + 16.0 * math.sin(x / 61.0 * 6.2831853 + k1 * 1.7)
                      + 10.0 * math.sin(x / 93.0 * 6.2831853))
                y0 = int(PSY_TOP + (240 - PSY_TOP) * b / float(bands) + o0)
                y1 = int(PSY_TOP + (240 - PSY_TOP) * (b + 1) / float(bands) + o1)
                if y1 < y0:
                    y0, y1 = y1, y0
                painter.fillRect(x, y0, 8, y1 - y0 + 1, cols[b % 3])
            x += 8

    def _psy_flash_draw(self, painter: QPainter, k: int) -> None:
        P = PSY_PAL_RGB[self.psy_pal]
        self._psy_lcg_seed(self.psy_shot_seed)
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["bg"]))
        if k == PSY_F_OPGRID:
            self._psy_flash_opgrid(painter, P)
        elif k == PSY_F_DOTS:
            self._psy_flash_dots(painter, P)
        elif k == PSY_F_HATCH:
            self._psy_flash_hatch(painter, P)
        else:
            self._psy_flash_wobble(painter, P)

    # ACCENT（割り込み）。Face ACCENTはDesktopにCoreS3のSD /facesキャッシュ相当が無いため、
    # CoreS3自身がキャッシュ未使用時に備えて持つベクター顔フォールバック
    # （psyAccentFaceVector）を常時使用する（抽選間隔・強制発火・連続禁止・2コマ目zoomの
    # ロジックは変更していない）。
    def _psy_accent_face_vector(self, painter: QPainter, P: dict, zoom: float) -> None:
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["bg"]))
        cx, cy = int(self.psy_face_shot_dx), int(self.psy_face_shot_dy)
        s = int(12.0 * zoom)
        mn_col, ac_col = QColor(*P["mn"]), QColor(*P["ac"])
        self._fill_circle(painter, cx - s * 2, cy - s, int(s * 1.3), mn_col)
        self._fill_circle(painter, cx + s * 2, cy - s, int(s * 1.3), mn_col)
        self._fill_circle(painter, cx, cy + s // 2, s, ac_col)
        painter.fillRect(cx - s * 2, cy + s * 2, s * 4, s // 2, ac_col)

    def _psy_face_shot_begin(self) -> None:
        self.psy_face_shot_idx = -1
        self.psy_face_shot_zoom = random.choice(PSY_FACE_ZOOM)
        self.psy_face_shot_flip = self._psy_chance(50)
        self.psy_face_shot_ang = float(self._psy_rnd_i(-22, 22))
        self.psy_face_shot_dx = 160.0 + self._psy_rnd_f(-45.0, 45.0)
        self.psy_face_shot_dy = 144.0 + self._psy_rnd_f(-30.0, 30.0)
        self.psy_face_shot_ov = self._psy_rnd_i(0, 3)
        self.psy_face_shot_step = 0
        self.psy_shots_since_face = 0

    def _psy_accent_face(self, painter: QPainter, P: dict) -> None:
        z = self.psy_face_shot_zoom * (1.0 if self.psy_face_shot_step == 0 else PSY_FACE_ZOOM_STEP)
        if self.psy_face_shot_step < 255:
            self.psy_face_shot_step += 1
        self._psy_accent_face_vector(painter, P, z)

    def _psy_accent_stab(self, painter: QPainter, P: dict) -> None:
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["mn"]))
        n = self._psy_rnd_i(2, 3)
        black = QColor(0, 0, 0)
        for _ in range(n):
            y = self._psy_rnd_i(PSY_TOP, 220)
            painter.fillRect(0, y, CANVAS_W, self._psy_rnd_i(14, 34), black)

    def _psy_accent_giant(self, painter: QPainter, P: dict) -> None:
        painter.fillRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP, QColor(*P["bg"]))
        ac_col = QColor(*P["ac"])
        if self._psy_chance(50):
            self._fill_circle(painter, self._psy_rnd_i(60, 260), self._psy_rnd_i(80, 200), self._psy_rnd_i(120, 190), ac_col)
        else:
            self._poly(painter, [
                (self._psy_rnd_i(-100, 80), self._psy_rnd_i(-40, 120)),
                (self._psy_rnd_i(240, 420), self._psy_rnd_i(60, 200)),
                (self._psy_rnd_i(40, 280), self._psy_rnd_i(200, 340)),
            ], ac_col)

    def _psy_accent_scan(self, painter: QPainter) -> None:
        black = QColor(0, 0, 0)
        y = PSY_TOP
        while y < 240:
            painter.fillRect(0, y, CANVAS_W, 4, black)
            y += 10

    def _psy_draw_accent(self, painter: QPainter, kind: int, P: dict) -> None:
        if kind == PSY_A_FACE:
            self._psy_accent_face(painter, P)
        elif kind == PSY_A_INVERT:
            self._psy_motion_draw(painter, self.psy_kind, True)
        elif kind == PSY_A_STAB:
            self._psy_accent_stab(painter, P)
        elif kind == PSY_A_GIANT:
            self._psy_accent_giant(painter, P)
        else:
            self._psy_accent_scan(painter)

    def _psy_pick_accent(self) -> int:
        face_ok = self.psy_shots_since_face >= PSY_FACE_MIN_GAP
        if face_ok and self.psy_shots_since_face >= PSY_FACE_FORCE_GAP:
            return PSY_A_FACE
        cand = []
        if face_ok:
            cand.append(PSY_A_FACE)
        cand.append(PSY_A_STAB)
        cand.append(PSY_A_GIANT)
        cand.append(PSY_A_SCAN)
        return random.choice(cand)

    # ショット遷移
    def _psy_pick_palette(self) -> None:
        prev = self.psy_pal
        need_bright = PSY_PAL_RGB[prev]["dark"]
        cand = [i for i in range(12) if i != prev and not (need_bright and PSY_PAL_RGB[i]["dark"])]
        self.psy_pal = random.choice(cand) if cand else (prev + 1) % 12

    def _psy_pick_motion(self) -> int:
        if not self.psy_bag:
            self.psy_bag = list(range(PSY_M_COUNT))
            random.shuffle(self.psy_bag)
        return self.psy_bag.pop()

    def _psy_start_motion_shot(self) -> None:
        self._psy_pick_palette()
        self.psy_layer = PSY_LAYER_MOTION
        self.psy_kind = self._psy_pick_motion()
        if self.psy_kind == PSY_M_SHARD:
            self.psy_frames = self._psy_rnd_i(PSY_MOTION_MIN_F, PSY_SHARD_MAX_F)
        else:
            self.psy_frames = self._psy_rnd_i(PSY_MOTION_MIN_F, PSY_MOTION_MAX_F)
        self.psy_idx = 0
        self._psy_motion_init(self.psy_kind)
        self.psy_shots_since_face += 1

    def _psy_start_flash_shot(self) -> None:
        self._psy_pick_palette()
        self.psy_layer = PSY_LAYER_FLASH
        k = random.randint(0, PSY_F_COUNT - 1)
        while PSY_F_COUNT > 1 and k == self.psy_last_flash:
            k = random.randint(0, PSY_F_COUNT - 1)
        self.psy_last_flash = k
        self.psy_kind = k
        self.psy_frames = self._psy_rnd_i(PSY_FLASH_MIN_F, PSY_FLASH_MAX_F)
        self.psy_idx = 0
        self.psy_shot_seed = random.randint(1, 0x7FFFFFFF)
        self.psy_shots_since_face += 1

    def _psy_next_shot(self) -> None:
        if self.psy_layer == PSY_LAYER_MOTION:
            self.psy_burst_left = self._psy_rnd_i(PSY_BURST_MIN, PSY_BURST_MAX)
            self._psy_start_flash_shot()
        else:
            if self.psy_burst_left > 0:
                self.psy_burst_left -= 1
            if self.psy_burst_left > 0:
                self._psy_start_flash_shot()
            else:
                self._psy_start_motion_shot()

    # 本体
    def _light_render_psyche(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if needs_init:
            self.psy_bag = []
            self.psy_burst_left = 0
            self.psy_intr_left = 0
            self.psy_last_flash = 255
            self.psy_shots_since_face = 99
            self.psy_last_face_idx = -1
            self.psy_pal = random.randint(0, 11)
            self._psy_start_motion_shot()

        painter.save()
        painter.setClipRect(0, PSY_TOP, CANVAS_W, CANVAS_H - PSY_TOP)

        if self.psy_intr_left > 0:
            self.psy_intr_left -= 1
            P = PSY_PAL_RGB[self.psy_pal]
            if self.psy_layer == PSY_LAYER_MOTION:
                t = (self.psy_idx + 1) / float(self.psy_frames)
                ramp = 1.0 + PSY_RAMP * (t ** 2.2)
                self._psy_motion_step(self.psy_kind, ramp)
                if self.psy_intr_kind == PSY_A_SCAN:
                    self._psy_motion_draw(painter, self.psy_kind, False)
            self._psy_draw_accent(painter, self.psy_intr_kind, P)
            painter.restore()
            return

        self.psy_idx += 1
        if self.psy_idx >= self.psy_frames:
            self._psy_next_shot()

        if self.psy_layer == PSY_LAYER_MOTION:
            t = self.psy_idx / float(self.psy_frames)
            ramp = 1.0 + PSY_RAMP * (t ** 2.2)
            self._psy_motion_step(self.psy_kind, ramp)

            if self.psy_idx > 2 and self.psy_idx + 1 < self.psy_frames:
                if self._psy_chance(PSY_INTR_PCT):
                    self.psy_intr_kind = self._psy_pick_accent()
                    self.psy_intr_left = self._psy_rnd_i(PSY_ACCENT_MIN_F, PSY_ACCENT_MAX_F) - 1
                    if self.psy_intr_kind == PSY_A_FACE:
                        self._psy_face_shot_begin()
                    P = PSY_PAL_RGB[self.psy_pal]
                    if self.psy_intr_kind == PSY_A_SCAN:
                        self._psy_motion_draw(painter, self.psy_kind, False)
                    self._psy_draw_accent(painter, self.psy_intr_kind, P)
                    painter.restore()
                    return
                if self._psy_chance(PSY_INVERT_PCT):
                    self._psy_motion_draw(painter, self.psy_kind, True)
                    painter.restore()
                    return
            self._psy_motion_draw(painter, self.psy_kind, False)
        else:
            self._psy_flash_draw(painter, self.psy_kind)

        painter.restore()

    # ===== Phase 5C: PINBALL Arcade =====
    def _pin_launch_ball(self, now_ms: int) -> None:
        self.pin_ball_x = random.uniform(PIN_LAUNCH_X_MIN, PIN_LAUNCH_X_MAX)
        self.pin_ball_y = PIN_LAUNCH_Y
        self.pin_ball_vx = random.uniform(-PIN_LAUNCH_VX_RANGE, PIN_LAUNCH_VX_RANGE)
        self.pin_ball_vy = random.uniform(PIN_LAUNCH_VY_MIN, PIN_LAUNCH_VY_MAX)
        self.pin_last_fast_ms = now_ms
        self.pin_stuck_ref_x = self.pin_ball_x
        self.pin_stuck_ref_y = self.pin_ball_y
        self.pin_stuck_ref_ms = now_ms

    def _pin_reset(self, now_ms: int) -> None:
        self.pin_score = 0
        self.pin_eye_flashing = [False, False]
        self.pin_nose_flashing = False
        self.pin_mouth_pass_flashing = False
        self.pin_flip_state = [PIN_FLIP_IDLE, PIN_FLIP_IDLE]
        self.pin_flip_state_start = [now_ms, now_ms]
        self.pin_flip_last_fire = [0, 0]
        self._pin_launch_ball(now_ms)

    def _pin_collide_segment(self, x0: float, y0: float, x1: float, y1: float) -> bool:
        dx, dy = x1 - x0, y1 - y0
        len2 = dx * dx + dy * dy
        t = 0.0 if len2 < 1e-6 else ((self.pin_ball_x - x0) * dx + (self.pin_ball_y - y0) * dy) / len2
        t = max(0.0, min(1.0, t))
        qx, qy = x0 + dx * t, y0 + dy * t
        rx, ry = self.pin_ball_x - qx, self.pin_ball_y - qy
        dist2 = rx * rx + ry * ry
        if dist2 >= PIN_BALL_R * PIN_BALL_R or dist2 < 1e-8:
            return False
        dist = math.sqrt(dist2)
        nx, ny = rx / dist, ry / dist
        self.pin_ball_x = qx + nx * PIN_BALL_R
        self.pin_ball_y = qy + ny * PIN_BALL_R
        vn = self.pin_ball_vx * nx + self.pin_ball_vy * ny
        if vn < 0.0:
            self.pin_ball_vx = (self.pin_ball_vx - 2.0 * vn * nx) * PIN_REST
            self.pin_ball_vy = (self.pin_ball_vy - 2.0 * vn * ny) * PIN_REST
        return True

    def _pin_collide_eyes(self, now_ms: int) -> None:
        for i, ex in enumerate(PIN_EYE_X):
            dx, dy = self.pin_ball_x - ex, self.pin_ball_y - PIN_EYE_Y
            min_d = PIN_BALL_R + PIN_EYE_COLLIDE_R
            d2 = dx * dx + dy * dy
            if d2 >= min_d * min_d or d2 < 1e-6:
                continue
            d = math.sqrt(d2); nx, ny = dx / d, dy / d
            self.pin_ball_x, self.pin_ball_y = ex + nx * min_d, PIN_EYE_Y + ny * min_d
            sp = max(PIN_EYE_KICK, math.hypot(self.pin_ball_vx, self.pin_ball_vy))
            self.pin_ball_vx, self.pin_ball_vy = nx * sp, ny * sp
            self.pin_eye_flashing[i] = True; self.pin_eye_flash_ms[i] = now_ms
            self.pin_score += PIN_EYE_SCORE; self.pin_last_fast_ms = now_ms

    def _pin_collide_nose(self, now_ms: int) -> None:
        dx, dy = self.pin_ball_x - NOSE_X, self.pin_ball_y - NOSE_Y
        min_d = PIN_BALL_R + PIN_NOSE_COLLIDE_R
        d2 = dx * dx + dy * dy
        if d2 >= min_d * min_d or d2 < 1e-6:
            return
        d = math.sqrt(d2); nx, ny = dx / d, dy / d
        self.pin_ball_x, self.pin_ball_y = NOSE_X + nx * min_d, NOSE_Y + ny * min_d
        sp = max(PIN_NOSE_KICK, math.hypot(self.pin_ball_vx, self.pin_ball_vy))
        self.pin_ball_vx, self.pin_ball_vy = nx * sp, ny * sp
        self.pin_nose_flashing = True; self.pin_nose_flash_ms = now_ms
        self.pin_score += PIN_NOSE_SCORE; self.pin_last_fast_ms = now_ms

    @staticmethod
    def _pin_segment_touch(px: float, py: float, r: float, x0: float, y0: float, x1: float, y1: float) -> bool:
        dx, dy = x1 - x0, y1 - y0; len2 = dx * dx + dy * dy
        t = 0.0 if len2 < 1e-6 else ((px - x0) * dx + (py - y0) * dy) / len2
        t = max(0.0, min(1.0, t)); cx, cy = x0 + dx * t, y0 + dy * t
        return (px - cx) ** 2 + (py - cy) ** 2 <= r * r

    def _pin_check_mouth_pass(self, now_ms: int) -> None:
        mx, my = float(NOSE_X), float(NOSE_Y)
        p = (self.pin_ball_x, self.pin_ball_y, PIN_MOUTH_PASS_TOUCH_R)
        touch = (self._pin_segment_touch(*p, mx, my + 8, mx, my + 22) or
                 self._pin_segment_touch(*p, mx, my + 22, mx - 20, my + 32) or
                 self._pin_segment_touch(*p, mx, my + 22, mx + 20, my + 32))
        if touch:
            self.pin_mouth_pass_flashing = True; self.pin_mouth_pass_flash_ms = now_ms

    def _pin_update_flipper(self, idx: int, now_ms: int, px: float, py: float, rdx: float, rdy: float, fdx: float, fdy: float) -> None:
        state = self.pin_flip_state[idx]
        if state == PIN_FLIP_IDLE and now_ms - self.pin_flip_last_fire[idx] >= PIN_FLIP_COOLDOWN_MS:
            zx, zy = px + rdx * PIN_FLIPPER_LEN * 0.55, py + rdy * PIN_FLIPPER_LEN * 0.55
            dx, dy = self.pin_ball_x - zx, self.pin_ball_y - zy
            if dx * dx + dy * dy <= PIN_FLIPPER_ZONE_R ** 2 and self.pin_ball_vy > 0:
                self.pin_flip_state[idx] = PIN_FLIP_UP; self.pin_flip_state_start[idx] = now_ms; self.pin_flip_last_fire[idx] = now_ms
                sp = PIN_FLIP_LAUNCH_SPEED + random.randrange(0, 40); jitter = random.randrange(-10, 11) * 0.01
                self.pin_ball_vx, self.pin_ball_vy = fdx * sp + jitter * sp, fdy * sp
                self.pin_last_fast_ms = now_ms
        el = now_ms - self.pin_flip_state_start[idx]; st = self.pin_flip_state[idx]
        if st == PIN_FLIP_UP and el >= PIN_FLIP_UP_MS:
            self.pin_flip_state[idx] = PIN_FLIP_HOLD; self.pin_flip_state_start[idx] = now_ms
        elif st == PIN_FLIP_HOLD and el >= PIN_FLIP_HOLD_MS:
            self.pin_flip_state[idx] = PIN_FLIP_RETURN; self.pin_flip_state_start[idx] = now_ms
        elif st == PIN_FLIP_RETURN and el >= PIN_FLIP_RETURN_MS:
            self.pin_flip_state[idx] = PIN_FLIP_IDLE; self.pin_flip_state_start[idx] = now_ms

    def _pin_flipper_phase(self, idx: int, now_ms: int) -> float:
        el = now_ms - self.pin_flip_state_start[idx]; st = self.pin_flip_state[idx]
        if st == PIN_FLIP_UP: return min(1.0, el / PIN_FLIP_UP_MS)
        if st == PIN_FLIP_HOLD: return 1.0
        if st == PIN_FLIP_RETURN: return max(0.0, 1.0 - el / PIN_FLIP_RETURN_MS)
        return 0.0

    def _pin_draw_flipper(self, painter: QPainter, px: float, py: float, dx: float, dy: float) -> None:
        tx, ty = px + dx * PIN_FLIPPER_LEN, py + dy * PIN_FLIPPER_LEN
        self._draw_thick_line(painter, round(px), round(py), round(tx), round(ty), round(PIN_FLIPPER_HW * 2), QColor(*PIN_FLIPPER_RGB))
        self._fill_circle(painter, round(px), round(py), round(PIN_FLIPPER_HW + 1), QColor(*PIN_FLIPPER_HUB_RGB))

    def _pin_draw_foreground(self, painter: QPainter, now_ms: int) -> None:
        # CoreS3 sceneComposeAndPush() 4.6と同じ順序：eyes → mouth flash → nose idle。
        # 鼻ヒットの大きな白フラッシュ自体はLighting層で描き、顔レイヤーを挟んだ後、
        # 最後に通常のオレンジ鼻を上書きする。Character=Noneでもゲーム役物として残す。
        for i, ex in enumerate(PIN_EYE_X):
            if self.pin_eye_flashing[i] and now_ms - self.pin_eye_flash_ms[i] < PIN_FLASH_MS:
                self._fill_circle(painter, round(ex), round(PIN_EYE_Y), round(PIN_EYE_FLASH_R), QColor(*PIN_FLASH_RGB))
            else:
                self.pin_eye_flashing[i] = False
                self._fill_circle(painter, round(ex), round(PIN_EYE_Y), round(PIN_EYE_OUTLINE_R), QColor(*PIN_FLASH_RGB))
                self._fill_circle(painter, round(ex), round(PIN_EYE_Y), round(PIN_EYE_RING_R), QColor(*PIN_EYE_PINK_RGB))
                self._fill_circle(painter, round(ex), round(PIN_EYE_Y), round(PIN_EYE_INNER_R), QColor(*PIN_FLASH_RGB))
                self._draw_text_centered(painter, "100", round(ex), round(PIN_EYE_Y), 9, QColor(*PIN_EYE_SCORE_RGB))
        if self.pin_mouth_pass_flashing:
            if now_ms - self.pin_mouth_pass_flash_ms < PIN_MOUTH_PASS_FLASH_MS:
                c = QColor(*PIN_FLASH_RGB); t = PIN_MOUTH_PASS_FLASH_THICK
                self._draw_thick_line(painter, NOSE_X, NOSE_Y + 8, NOSE_X, NOSE_Y + 22, t, c)
                self._draw_thick_line(painter, NOSE_X, NOSE_Y + 22, NOSE_X - 20, NOSE_Y + 32, t, c)
                self._draw_thick_line(painter, NOSE_X, NOSE_Y + 22, NOSE_X + 20, NOSE_Y + 32, t, c)
            else:
                self.pin_mouth_pass_flashing = False
        self._fill_ellipse(painter, NOSE_X, NOSE_Y, 18, 12, QColor(*PIN_NOSE_RGB))

    def _light_render_pinball(self, painter: QPainter, now_ms: int, needs_init: bool) -> None:
        if needs_init or self.pin_prev_ms == 0:
            self._pin_reset(now_ms); self.pin_prev_ms = now_ms
        dt = min(0.12, max(0.0, (now_ms - self.pin_prev_ms) / 1000.0)); self.pin_prev_ms = now_ms
        self.pin_ball_vy += PIN_GRAVITY * dt
        speed = math.hypot(self.pin_ball_vx, self.pin_ball_vy)
        if speed > PIN_MAX_SPEED:
            sc = PIN_MAX_SPEED / speed; self.pin_ball_vx *= sc; self.pin_ball_vy *= sc; speed = PIN_MAX_SPEED
        steps = max(1, min(10, math.ceil(speed * dt / 4.0))); subdt = dt / steps if steps else 0.0
        for _ in range(steps):
            self.pin_ball_x += self.pin_ball_vx * subdt; self.pin_ball_y += self.pin_ball_vy * subdt
            for i in range(len(PIN_BOUND_X)):
                if i == PIN_BOUND_DRAIN_EDGE: continue
                j = (i + 1) % len(PIN_BOUND_X)
                if self._pin_collide_segment(PIN_BOUND_X[i], PIN_BOUND_Y[i], PIN_BOUND_X[j], PIN_BOUND_Y[j]): self.pin_last_fast_ms = now_ms
            self._pin_collide_eyes(now_ms); self._pin_collide_nose(now_ms); self._pin_check_mouth_pass(now_ms)
        self._pin_update_flipper(0, now_ms, PIN_FLIP_L_PX, PIN_FLIP_L_PY, PIN_FLIP_L_REST_DX, PIN_FLIP_L_REST_DY, PIN_FLIP_L_FIRE_DX, PIN_FLIP_L_FIRE_DY)
        self._pin_update_flipper(1, now_ms, PIN_FLIP_R_PX, PIN_FLIP_R_PY, PIN_FLIP_R_REST_DX, PIN_FLIP_R_REST_DY, PIN_FLIP_R_FIRE_DX, PIN_FLIP_R_FIRE_DY)
        if self.pin_ball_y - PIN_BALL_R > PIN_BOTTOM: self._pin_launch_ball(now_ms)
        spd = math.hypot(self.pin_ball_vx, self.pin_ball_vy)
        if spd > PIN_MIN_SPEED_STUCK: self.pin_last_fast_ms = now_ms
        elif now_ms - self.pin_last_fast_ms > PIN_STUCK_MS: self._pin_launch_ball(now_ms)
        dsx, dsy = self.pin_ball_x - self.pin_stuck_ref_x, self.pin_ball_y - self.pin_stuck_ref_y
        if dsx * dsx + dsy * dsy > PIN_STUCK_POS_R ** 2:
            self.pin_stuck_ref_x, self.pin_stuck_ref_y, self.pin_stuck_ref_ms = self.pin_ball_x, self.pin_ball_y, now_ms
        elif now_ms - self.pin_stuck_ref_ms > PIN_STUCK_POS_MS:
            self.pin_ball_vx += PIN_STUCK_NUDGE_VX if random.randrange(2) == 0 else -PIN_STUCK_NUDGE_VX
            self.pin_stuck_ref_x, self.pin_stuck_ref_y, self.pin_stuck_ref_ms = self.pin_ball_x, self.pin_ball_y, now_ms
        if self.pin_ball_x < PIN_LEFT - 40 or self.pin_ball_x > PIN_RIGHT + 40 or self.pin_ball_y < PIN_TOP - 40 or self.pin_ball_y > PIN_BOTTOM + 80:
            self._pin_launch_ball(now_ms)
        painter.fillRect(0, PIN_TOP, 320, 240 - PIN_TOP, QColor(*PIN_BG_RGB))
        field = QColor(*PIN_FIELD_RGB)
        for k in range(1, len(PIN_BOUND_X)-1):
            self._poly(painter, [(PIN_BOUND_X[0],PIN_BOUND_Y[0]), (PIN_BOUND_X[k],PIN_BOUND_Y[k]), (PIN_BOUND_X[k+1],PIN_BOUND_Y[k+1])], field)
        wall = QColor(*PIN_WALL_RGB)
        for i in range(len(PIN_BOUND_X)):
            if i == PIN_BOUND_DRAIN_EDGE: continue
            j=(i+1)%len(PIN_BOUND_X); self._draw_thick_line(painter, round(PIN_BOUND_X[i]),round(PIN_BOUND_Y[i]),round(PIN_BOUND_X[j]),round(PIN_BOUND_Y[j]),1,wall)
        if self.pin_nose_flashing and now_ms - self.pin_nose_flash_ms < PIN_FLASH_MS:
            self._fill_circle(painter, NOSE_X, NOSE_Y, round(PIN_NOSE_FLASH_R), QColor(*PIN_FLASH_RGB))
        pl=self._pin_flipper_phase(0,now_ms); pr=self._pin_flipper_phase(1,now_ms)
        self._pin_draw_flipper(painter,PIN_FLIP_L_PX,PIN_FLIP_L_PY,PIN_FLIP_L_REST_DX+(PIN_FLIP_L_FIRE_DX-PIN_FLIP_L_REST_DX)*pl,PIN_FLIP_L_REST_DY+(PIN_FLIP_L_FIRE_DY-PIN_FLIP_L_REST_DY)*pl)
        self._pin_draw_flipper(painter,PIN_FLIP_R_PX,PIN_FLIP_R_PY,PIN_FLIP_R_REST_DX+(PIN_FLIP_R_FIRE_DX-PIN_FLIP_R_REST_DX)*pr,PIN_FLIP_R_REST_DY+(PIN_FLIP_R_FIRE_DY-PIN_FLIP_R_REST_DY)*pr)
        self._fill_circle(painter, round(self.pin_ball_x), round(self.pin_ball_y), round(PIN_BALL_R), QColor(*PIN_BALL_RGB))

    # ===== Phase 5C: SKY BURNER =====
    def _skb_rand(self) -> int:
        self.skb_rng = (self.skb_rng * 1664525 + 1013904223) & 0xffffffff
        return self.skb_rng

    def _skb_rand01(self) -> float:
        return (self._skb_rand() >> 8) / 16777216.0

    @staticmethod
    def _skb_clamp_y(y: int) -> int:
        return max(SKB_TOP, min(240, y))

    def _skb_reset(self, now: int) -> None:
        self.skb_state=SKB_ST_SEARCH; self.skb_state_start_ms=now; self.skb_next_at=now+SKB_SEARCH_MIN_MS+int(self._skb_rand01()*(SKB_SEARCH_MAX_MS-SKB_SEARCH_MIN_MS))
        self.skb_bank_deg=0.0; self.skb_enemy_alive=False; self.skb_enemy_bank_deg=0.0
        self.skb_aim_x=160.0; self.skb_aim_y=SKB_HORIZON_Y0-26.0; self.skb_lock_timer_ms=0.0; self.skb_msl_active=False
        self.skb_smoke=[None]*SKB_SMOKE_MAX; self.skb_esmoke=[None]*SKB_ESMOKE_MAX; self.skb_emsl_active=False
        self.skb_emsl_next_at=now+1800+int(self._skb_rand01()*1800); self.skb_expl_active=False
        self.skb_score=0; self.skb_hit_count=0; self.skb_enemy_wave_left=5+self._skb_rand()%5; self.skb_msl_ammo=4; self.skb_msl_regen_at=now+3400
        self.skb_saam_ammo=10; self.skb_saam_regen_at=now+2200; self.skb_throttle=80.0
        if not self.skb_streak_ready:
            for i in range(SKB_STREAK_COUNT):
                self.skb_streak_ang[i]=i/SKB_STREAK_COUNT*2*math.pi+self._skb_rand01()*0.2; self.skb_streak_r[i]=self._skb_rand01()*SKB_STREAK_MAX_R
            self.skb_streak_ready=True

    def _skb_spawn_enemy(self, now: int) -> None:
        self.skb_enemy_x=90+self._skb_rand01()*140; self.skb_enemy_y=SKB_TOP+26+self._skb_rand01()*40
        self.skb_enemy_target_x=self.skb_enemy_x; self.skb_enemy_target_y=self.skb_enemy_y; self.skb_enemy_scale=0.35; self.skb_enemy_alive=True
        self.skb_engage_start_ms=now; self.skb_enemy_wander_at=now; self.skb_lock_timer_ms=0.0

    def _skb_update_enemy(self, now: int) -> None:
        if now >= self.skb_enemy_wander_at:
            dx=(self._skb_rand01()*2-1)*70; dy=(self._skb_rand01()*2-1)*34
            self.skb_enemy_target_x=max(55,min(265,self.skb_enemy_x+dx)); self.skb_enemy_target_y=max(SKB_TOP+14,min(SKB_HORIZON_Y0-6,self.skb_enemy_y+dy))
            self.skb_enemy_wander_at=now+380+int(self._skb_rand01()*380)
        self.skb_enemy_x+=(self.skb_enemy_target_x-self.skb_enemy_x)*SKB_ENEMY_FOLLOW; self.skb_enemy_y+=(self.skb_enemy_target_y-self.skb_enemy_y)*SKB_ENEMY_FOLLOW
        bt=max(-SKB_ENEMY_MAX_BANK_DEG,min(SKB_ENEMY_MAX_BANK_DEG,(self.skb_enemy_target_x-self.skb_enemy_x)/70*SKB_ENEMY_MAX_BANK_DEG))
        self.skb_enemy_bank_deg+=(bt-self.skb_enemy_bank_deg)*SKB_ENEMY_BANK_SMOOTH
        t=min(1.0,(now-self.skb_engage_start_ms)/SKB_APPROACH_MS); self.skb_enemy_scale=0.35+t*0.75

    def _skb_update_aim_lock(self, dt_ms: float) -> None:
        self.skb_aim_x+=(self.skb_enemy_x-self.skb_aim_x)*SKB_AIM_FOLLOW; self.skb_aim_y+=(self.skb_enemy_y-self.skb_aim_y)*SKB_AIM_FOLLOW
        d=math.hypot(self.skb_enemy_x-self.skb_aim_x,self.skb_enemy_y-self.skb_aim_y)
        self.skb_lock_timer_ms = self.skb_lock_timer_ms+dt_ms if d<SKB_LOCK_CAPTURE_PX else max(0.0,self.skb_lock_timer_ms-dt_ms*SKB_LOCK_DECAY)

    def _skb_spawn_smoke(self, arr_name: str, x: float, y: float, r0: float, now: int) -> None:
        arr=getattr(self,arr_name); slot=next((i for i,v in enumerate(arr) if v is None),None)
        if slot is None: slot=min(range(len(arr)),key=lambda i: arr[i][3])
        arr[slot]=(x,y,r0,now)

    def _skb_depth_t(self,y:float)->float:
        return max(0.0,min(1.0,(y-SKB_TOP)/(SKB_MSL_LAUNCH_Y-SKB_TOP)))

    def _skb_spawn_trail(self,x0,y0,x1,y1,now):
        dx,dy=x1-x0,y1-y0; ln=math.hypot(dx,dy); px=(-dy/ln if ln>0.1 else 0); py=(dx/ln if ln>0.1 else 0)
        for k in range(SKB_SMOKE_SPAWN_PER_FRAME):
            t=(k+1)/(SKB_SMOKE_SPAWN_PER_FRAME+1); jitter=(self._skb_rand01()*2-1)*2.2; sx=x0+dx*t+px*jitter; sy=y0+dy*t+py*jitter
            dep=self._skb_depth_t(sy); rb=SKB_SMOKE_R_FAR+(SKB_SMOKE_R0-SKB_SMOKE_R_FAR)*dep*dep; self._skb_spawn_smoke('skb_smoke',sx,sy,rb*(0.85+self._skb_rand01()*0.3),now)

    @staticmethod
    def _mix_rgb(a,b,t): return tuple(round(a[i]+(b[i]-a[i])*t) for i in range(3))

    def _skb_draw_smoke_arr(self,painter,arr_name,now,life,enemy=False):
        arr=getattr(self,arr_name)
        for i,v in enumerate(arr):
            if v is None: continue
            x,y,r0,st=v; el=now-st
            if el>=life: arr[i]=None; continue
            t=el/life; rmin=1.0 if enemy else SKB_SMOKE_R_MIN*(r0/SKB_SMOKE_R0); r=max(1,r0-(r0-rmin)*t)
            fade_start=0.5 if enemy else 0.55; u=max(0,min(1,(t-fade_start)/(1-fade_start)))
            col=SKB_SMOKE_RGB if t<fade_start else self._mix_rgb(SKB_SMOKE_RGB,SKB_SKY_HZ_RGB,u*0.86)
            self._fill_circle(painter,round(x),round(y),max(1,round(r)),QColor(*col))

    def _skb_hash(self,seed,salt):
        h=(seed*2654435761+salt*0x85ebca6b)&0xffffffff; h^=h>>13; h=(h*0xC2B2AE35)&0xffffffff; h^=h>>16; return h&0xffffffff

    def _skb_start_explosion(self,x,y,now): self.skb_expl_active=True; self.skb_expl_x=x; self.skb_expl_y=y; self.skb_expl_start_ms=now; self.skb_expl_seed=self._skb_rand()

    def _skb_draw_explosion(self,painter,now):
        if not self.skb_expl_active: return
        el=now-self.skb_expl_start_ms
        if el>=SKB_HIT_LIFE_MS: self.skb_expl_active=False; return
        if el<SKB_HIT_FLASH_MS: t=el/SKB_HIT_FLASH_MS; r=3+round(t*22); col=SKB_HIT_A_RGB
        elif el<SKB_HIT_MID_MS: t=(el-SKB_HIT_FLASH_MS)/(SKB_HIT_MID_MS-SKB_HIT_FLASH_MS); r=22+round(t*10); col=self._mix_rgb(SKB_HIT_A_RGB,SKB_HIT_B_RGB,t)
        else: t=(el-SKB_HIT_MID_MS)/(SKB_HIT_LIFE_MS-SKB_HIT_MID_MS); r=max(1,32-round(t*30)); col=self._mix_rgb(SKB_HIT_B_RGB,SKB_HIT_C_RGB,t)
        self._fill_circle(painter,round(self.skb_expl_x),round(self.skb_expl_y),r,QColor(*col))
        if el>=SKB_HIT_FLASH_MS:
            t2=(el-SKB_HIT_FLASH_MS)/(SKB_HIT_LIFE_MS-SKB_HIT_FLASH_MS)
            for k in range(6):
                h=self._skb_hash(self.skb_expl_seed,k*97+11); a=(h%360)*math.pi/180; d=6+t2*26+((h>>8)%5)
                x1=self.skb_expl_x+math.cos(a)*d; y1=self.skb_expl_y+math.sin(a)*d; x0=self.skb_expl_x+math.cos(a)*(d-5); y0=self.skb_expl_y+math.sin(a)*(d-5)
                self._draw_thick_line(painter,round(x0),round(y0),round(x1),round(y1),1,QColor(*(SKB_HIT_B_RGB if t2<0.5 else SKB_HIT_C_RGB)))

    def _skb_fire_missile(self,now):
        lane=self._skb_rand()%3-1; self.skb_msl_x=160+lane*SKB_MSL_LANE_OFFSET+(self._skb_rand01()*16-8); self.skb_msl_y=SKB_MSL_LAUNCH_Y
        dx,dy=self.skb_enemy_x-self.skb_msl_x,self.skb_enemy_y-self.skb_msl_y; ln=max(1,math.hypot(dx,dy)); self.skb_msl_vx=dx/ln*SKB_MISSILE_SPEED_PPS; self.skb_msl_vy=dy/ln*SKB_MISSILE_SPEED_PPS
        self.skb_msl_active=True; self.skb_msl_start_ms=now; self.skb_smoke=[None]*SKB_SMOKE_MAX; self.skb_msl_ammo=max(0,self.skb_msl_ammo-1)

    def _skb_update_missile(self,now,dt):
        if not self.skb_msl_active:return
        dx,dy=self.skb_enemy_x-self.skb_msl_x,self.skb_enemy_y-self.skb_msl_y; ln=math.hypot(dx,dy)
        if ln>0.5:
            dvx,dvy=dx/ln*SKB_MISSILE_SPEED_PPS,dy/ln*SKB_MISSILE_SPEED_PPS; self.skb_msl_vx+=(dvx-self.skb_msl_vx)*SKB_MISSILE_TURN; self.skb_msl_vy+=(dvy-self.skb_msl_vy)*SKB_MISSILE_TURN
            vl=math.hypot(self.skb_msl_vx,self.skb_msl_vy)
            if vl>1: self.skb_msl_vx=self.skb_msl_vx/vl*SKB_MISSILE_SPEED_PPS; self.skb_msl_vy=self.skb_msl_vy/vl*SKB_MISSILE_SPEED_PPS
        ox,oy=self.skb_msl_x,self.skb_msl_y; self.skb_msl_x+=self.skb_msl_vx*dt; self.skb_msl_y+=self.skb_msl_vy*dt; self._skb_spawn_trail(ox,oy,self.skb_msl_x,self.skb_msl_y,now)
        if ln<=SKB_MISSILE_HIT_PX or now-self.skb_msl_start_ms>=SKB_MISSILE_MAX_MS:
            self._skb_start_explosion(self.skb_enemy_x,self.skb_enemy_y,now); self.skb_enemy_alive=False; self.skb_msl_active=False; self.skb_state=SKB_ST_HIT; self.skb_state_start_ms=now
            self.skb_hit_count+=1; self.skb_score+=150+self._skb_rand()%100; self.skb_enemy_wave_left-=1
            if self.skb_enemy_wave_left<=0:self.skb_enemy_wave_left=5+self._skb_rand()%5

    def _skb_rot(self,lx,ly,sin_a,cos_a,px,py): return (round(px+lx*cos_a-ly*sin_a),round(py+lx*sin_a+ly*cos_a))

    def _skb_draw_enemy(self,painter):
        if not self.skb_enemy_alive:return
        s=self.skb_enemy_scale; a=self.skb_enemy_bank_deg*math.pi/180; sn,cs=math.sin(a),math.cos(a); px,py=self.skb_enemy_x,self.skb_enemy_y; c=QColor(*SKB_ENEMY_RGB)
        def pt(x,y):return self._skb_rot(x*s,y*s,sn,cs,px,py)
        self._poly(painter,[pt(-3,-2),pt(0,-15),pt(3,-2)],c); self._poly(painter,[pt(-11,-3),pt(11,-3),pt(11,0),pt(-11,0)],c)
        self._poly(painter,[pt(-3.5,-4),pt(3.5,-4),pt(3.5,9),pt(-3.5,9)],c); self._poly(painter,[pt(-3,2),pt(-24,10),pt(-13,5)],c); self._poly(painter,[pt(3,2),pt(24,10),pt(13,5)],c)
        er=max(1,round(2.2*s)); e0=pt(-2.2,9); e1=pt(2.2,9); self._fill_circle(painter,*e0,er,QColor(*SKB_ENEMY_ENGINE_RGB)); self._fill_circle(painter,*e1,er,QColor(*SKB_ENEMY_ENGINE_RGB))

    def _skb_draw_background(self,painter,bank):
        sl=math.tan(bank); hy=lambda x:self._skb_clamp_y(round(SKB_HORIZON_Y0+(x-160)*sl)); hl,hr=hy(0),hy(320); hl2=self._skb_clamp_y(hl-16); hr2=self._skb_clamp_y(hr-16); gl2=self._skb_clamp_y(hl+14); gr2=self._skb_clamp_y(hr+14)
        self._poly(painter,[(0,SKB_TOP),(320,SKB_TOP),(320,hr),(0,hl)],QColor(*SKB_SKY_RGB)); self._poly(painter,[(0,hl2),(320,hr2),(320,hr),(0,hl)],QColor(*SKB_SKY_HZ_RGB))
        self._poly(painter,[(0,hl),(320,hr),(320,240),(0,240)],QColor(*SKB_SEA_RGB)); self._poly(painter,[(0,hl),(320,hr),(320,gr2),(0,gl2)],QColor(*SKB_SEA_HZ_RGB))

    def _skb_draw_streaks(self,painter,bank,dt):
        for i in range(SKB_STREAK_COUNT):
            a=self.skb_streak_ang[i]+bank; cx,cy=math.cos(a),math.sin(a)*0.62; r0=self.skb_streak_r[i]; r1=r0+SKB_STREAK_SEG_LEN
            x0,y0=round(160+cx*r0),self._skb_clamp_y(round(SKB_HORIZON_Y0+cy*r0)); x1,y1=round(160+cx*r1),self._skb_clamp_y(round(SKB_HORIZON_Y0+cy*r1)); bri=min(255,round(60+195*r0/SKB_STREAK_MAX_R))
            col=tuple(v*bri//255 for v in SKB_STREAK_RGB); self._draw_thick_line(painter,x0,y0,x1,y1,1,QColor(*col)); self.skb_streak_r[i]+=SKB_STREAK_SPEED_PPS*dt
            if self.skb_streak_r[i]>SKB_STREAK_MAX_R:self.skb_streak_r[i]=self._skb_rand01()*20

    def _skb_draw_reticle(self,painter,now):
        locked=self.skb_state in (SKB_ST_LOCKED,SKB_ST_MISSILE); col=QColor(*(SKB_LOCK_RGB if locked else SKB_RETICLE_RGB)); ax,ay=round(self.skb_aim_x),round(self.skb_aim_y); o=16 if locked else 13; inn=7 if locked else 6
        for x0,y0,x1,y1 in [(ax-o,ay,ax-inn,ay),(ax+inn,ay,ax+o,ay),(ax,ay-o,ax,ay-inn),(ax,ay+inn,ax,ay+o)]:self._draw_thick_line(painter,x0,y0,x1,y1,1,col)
        painter.setPen(QPen(col)); painter.setBrush(Qt.NoBrush); painter.drawRect(ax-4,ay-4,8,8)
        if locked:
            b=o+4
            for sx in (-1,1):
                for sy in (-1,1):
                    self._draw_thick_line(painter,ax+sx*b,ay+sy*b,ax+sx*(b-6),ay+sy*b,1,col); self._draw_thick_line(painter,ax+sx*b,ay+sy*b,ax+sx*b,ay+sy*(b-6),1,col)
        if self.skb_state==SKB_ST_LOCKED and (now//150)%2==0:self._draw_text_centered(painter,'LOCK',ax,ay-o-14,9,QColor(*SKB_LOCK_RGB))

    def _skb_update_enemy_missile(self,now,dt):
        if not self.skb_emsl_active and self.skb_enemy_alive and self.skb_state in (SKB_ST_TRACK,SKB_ST_LOCKED,SKB_ST_MISSILE) and now>=self.skb_emsl_next_at:
            self.skb_emsl_active=True; self.skb_emsl_phase=SKB_EMSL_APPROACH; self.skb_emsl_phase_start_ms=now; self.skb_emsl_spawn_x=self.skb_enemy_x; self.skb_emsl_spawn_y=self.skb_enemy_y; self.skb_emsl_x=self.skb_enemy_x; self.skb_emsl_y=self.skb_enemy_y; self.skb_emsl_scale=SKB_EMSL_SCALE_MIN; self.skb_esmoke=[None]*SKB_ESMOKE_MAX
            self.skb_emsl_next_at=now+SKB_EMSL_APPROACH_MS+SKB_EMSL_DODGE_MS+SKB_EMSL_MIN_INTERVAL_MS+int(self._skb_rand01()*(SKB_EMSL_MAX_INTERVAL_MS-SKB_EMSL_MIN_INTERVAL_MS))
        if not self.skb_emsl_active:return
        if self.skb_emsl_phase==SKB_EMSL_APPROACH:
            t=min(1,(now-self.skb_emsl_phase_start_ms)/SKB_EMSL_APPROACH_MS); te=t*t; ox,oy=self.skb_emsl_x,self.skb_emsl_y; self.skb_emsl_x=self.skb_emsl_spawn_x+(160-self.skb_emsl_spawn_x)*te; self.skb_emsl_y=self.skb_emsl_spawn_y+(250-self.skb_emsl_spawn_y)*te; self.skb_emsl_scale=SKB_EMSL_SCALE_MIN+(SKB_EMSL_SCALE_PEAK-SKB_EMSL_SCALE_MIN)*te
            c=max(0,min(1,(self.skb_emsl_scale-SKB_EMSL_SCALE_MIN)/(SKB_EMSL_SCALE_PEAK-SKB_EMSL_SCALE_MIN))); rr=SKB_ESMOKE_R_FAR+(SKB_ESMOKE_R_NEAR-SKB_ESMOKE_R_FAR)*c*c; self._skb_spawn_smoke('skb_esmoke',ox,oy,rr,now)
            if t>=1:self.skb_emsl_dodge_vx=(-1 if self._skb_rand01()<.5 else 1)*(260+self._skb_rand01()*120); self.skb_emsl_dodge_vy=-60+self._skb_rand01()*40; self.skb_emsl_phase=SKB_EMSL_DODGE; self.skb_emsl_phase_start_ms=now
        else:
            ox,oy=self.skb_emsl_x,self.skb_emsl_y; self.skb_emsl_x+=self.skb_emsl_dodge_vx*dt; self.skb_emsl_y+=self.skb_emsl_dodge_vy*dt; self.skb_emsl_scale+=(SKB_EMSL_SCALE_END-self.skb_emsl_scale)*.15
            c=max(0,min(1,(self.skb_emsl_scale-SKB_EMSL_SCALE_MIN)/(SKB_EMSL_SCALE_PEAK-SKB_EMSL_SCALE_MIN))); self._skb_spawn_smoke('skb_esmoke',ox,oy,SKB_ESMOKE_R_FAR+(SKB_ESMOKE_R_NEAR-SKB_ESMOKE_R_FAR)*c*c,now)
            if self.skb_emsl_x<-40 or self.skb_emsl_x>360 or self.skb_emsl_y>280 or self.skb_emsl_y<SKB_TOP-40 or now-self.skb_emsl_phase_start_ms>=SKB_EMSL_DODGE_MS*3:self.skb_emsl_active=False

    def _skb_draw_enemy_missile(self,painter):
        if not self.skb_emsl_active:return
        r=max(1,round(2+3*self.skb_emsl_scale)); self._fill_circle(painter,round(self.skb_emsl_x),round(self.skb_emsl_y),r,QColor(*SKB_LOCK_RGB)); self._fill_circle(painter,round(self.skb_emsl_x),round(self.skb_emsl_y),max(1,r-2),QColor(255,255,255))

    def _skb_draw_missile(self,painter):
        if not self.skb_msl_active:return
        vl=math.hypot(self.skb_msl_vx,self.skb_msl_vy); dx,dy=(self.skb_msl_vx/vl,self.skb_msl_vy/vl) if vl>.1 else (0,-1); hx,hy=round(self.skb_msl_x+dx*5),round(self.skb_msl_y+dy*5); tx,ty=round(self.skb_msl_x-dx*5),round(self.skb_msl_y-dy*5)
        self._draw_thick_line(painter,tx,ty,hx,hy,1,QColor(*SKB_MISSILE_RGB)); self._fill_circle(painter,hx,hy,2,QColor(*SKB_MISSILE_RGB)); self._fill_circle(painter,tx,ty,2,QColor(*SKB_MISSILE_FLAME_RGB))

    def _skb_update_hud(self,now):
        if self.skb_msl_ammo<4 and now>=self.skb_msl_regen_at:self.skb_msl_ammo+=1; self.skb_msl_regen_at=now+3400
        if now>=self.skb_saam_regen_at:self.skb_saam_ammo=self.skb_saam_ammo-1 if self.skb_saam_ammo>0 else 10; self.skb_saam_regen_at=now+2200
        target=100 if self.skb_state in (SKB_ST_LOCKED,SKB_ST_MISSILE) else 76+math.sin(now*.0006)*6; self.skb_throttle+=(target-self.skb_throttle)*.05

    def _skb_draw_hud(self,painter,now):
        c=QColor(*SKB_RETICLE_RGB); font=QFont(); font.setPointSize(5); painter.setFont(font); painter.setPen(QPen(c))
        painter.drawText(4,56,f'SCORE:{self.skb_score%1000000:06d}'); painter.drawText(4,64,f'ENEMY:{self.skb_enemy_wave_left:02d} HIT:{self.skb_hit_count%100:02d}')
        spd=700+round(math.sin(now*.0009)*90)+round(math.sin(now*.0037)*18); alt=5200+round(math.sin(now*.00042)*650)+round(math.cos(now*.0021)*80)
        painter.drawText(4,144,f'SPD:{spd:04d}'); painter.drawText(246,144,f'ALT:{alt:05d}')
        painter.drawRect(6,168,10,56); fh=round(self.skb_throttle/100*54); painter.fillRect(7,223-fh,8,fh,c); painter.drawText(4,234,f'THR{round(self.skb_throttle):3d}')
        painter.drawText(248,174,'GUN:INF'); painter.drawText(248,183,f'MSL:{self.skb_msl_ammo:02d}'); painter.drawText(248,192,f'SAAM:{self.skb_saam_ammo:02d}'); painter.drawText(248,201,'DMG:00%')
        painter.setBrush(Qt.NoBrush); painter.drawEllipse(280,50,32,32); self._fill_circle(painter,296,66,1,c)
        if self.skb_enemy_alive:self._fill_circle(painter,round(296+max(-1,min(1,(self.skb_enemy_x-160)/160))*13),round(66+max(-1,min(1,(self.skb_enemy_y-SKB_HORIZON_Y0)/90))*13),2,QColor(*SKB_ENEMY_RGB))
        if self.skb_emsl_active and (now//150)%2==0:self._fill_circle(painter,round(296+max(-1,min(1,(self.skb_emsl_x-160)/160))*13),round(66+max(-1,min(1,(self.skb_emsl_y-SKB_HORIZON_Y0)/140))*13),2,QColor(*SKB_LOCK_RGB))
        if self.skb_emsl_active and (now//200)%2==0:self._draw_text_centered(painter,'<MISSILE ALERT>',160,58,6,QColor(*SKB_LOCK_RGB))

    def _light_render_skyburner(self,painter:QPainter,now:int,needs_init:bool)->None:
        # CoreS3ではLighting合成が約90ms周期。SKY BURNERにはdt駆動部分に加え、
        # ENEMY_FOLLOW/AIM_FOLLOW/BANK_SMOOTH/MISSILE_TURN等の「1更新ごとの係数」が
        # 混在するため、Desktopの20ms paint tickで毎回ロジックを進めると約4.5倍速になる。
        # そこでFighter Duel/Missile Defenseと同じ実効更新ゲートを使い、描画だけ毎tick行う。
        if needs_init or self.skb_prev_ms == 0:
            self._skb_reset(now)
            self.skb_prev_ms = now - LIGHT_FRAME_UPDATE_MS
        do_update = (now - self.skb_prev_ms) >= LIGHT_FRAME_UPDATE_MS
        dt = 0.0
        if do_update:
            dt=min(.20,max(0,(now-self.skb_prev_ms)/1000)); dtms=dt*1000; self.skb_prev_ms=now
            if self.skb_state==SKB_ST_SEARCH:
                self.skb_aim_x=160+math.sin(now*.0032)*20; self.skb_aim_y=SKB_HORIZON_Y0-26+math.cos(now*.0021)*12
                if now>=self.skb_next_at:self._skb_spawn_enemy(now); self.skb_state=SKB_ST_TRACK; self.skb_state_start_ms=now
            elif self.skb_state==SKB_ST_TRACK:
                self._skb_update_enemy(now); self._skb_update_aim_lock(dtms)
                if self.skb_lock_timer_ms>=SKB_LOCK_REQUIRE_MS:self.skb_state=SKB_ST_LOCKED; self.skb_state_start_ms=now
            elif self.skb_state==SKB_ST_LOCKED:
                self._skb_update_enemy(now); self._skb_update_aim_lock(dtms)
                if now-self.skb_state_start_ms>=SKB_LOCK_HOLD_MS:self._skb_fire_missile(now); self.skb_state=SKB_ST_MISSILE; self.skb_state_start_ms=now
            elif self.skb_state==SKB_ST_MISSILE:
                self._skb_update_enemy(now); self.skb_aim_x+=(self.skb_enemy_x-self.skb_aim_x)*SKB_AIM_FOLLOW; self.skb_aim_y+=(self.skb_enemy_y-self.skb_aim_y)*SKB_AIM_FOLLOW; self._skb_update_missile(now,dt)
            elif self.skb_state==SKB_ST_HIT:
                if now-self.skb_state_start_ms>=SKB_HIT_LIFE_MS:self.skb_state=SKB_ST_COOLDOWN; self.skb_state_start_ms=now; self.skb_next_at=now+SKB_COOLDOWN_MIN_MS+int(self._skb_rand01()*(SKB_COOLDOWN_MAX_MS-SKB_COOLDOWN_MIN_MS))
            elif self.skb_state==SKB_ST_COOLDOWN:
                self.skb_aim_x+=(160-self.skb_aim_x)*SKB_COOLDOWN_AIM_FOLLOW; self.skb_aim_y+=(SKB_HORIZON_Y0-26-self.skb_aim_y)*SKB_COOLDOWN_AIM_FOLLOW
                if now>=self.skb_next_at:self.skb_state=SKB_ST_SEARCH; self.skb_state_start_ms=now; self.skb_next_at=now+SKB_SEARCH_MIN_MS+int(self._skb_rand01()*(SKB_SEARCH_MAX_MS-SKB_SEARCH_MIN_MS))
            self._skb_update_enemy_missile(now,dt); self._skb_update_hud(now)
            bt=max(-SKB_MAX_BANK_DEG,min(SKB_MAX_BANK_DEG,(self.skb_aim_x-160)/140*SKB_MAX_BANK_DEG)); self.skb_bank_deg+=(bt-self.skb_bank_deg)*SKB_BANK_SMOOTH
        bank=self.skb_bank_deg*math.pi/180
        self._skb_draw_background(painter,bank); self._skb_draw_streaks(painter,bank,dt); self._skb_draw_enemy(painter); self._skb_draw_smoke_arr(painter,'skb_smoke',now,SKB_SMOKE_LIFE_MS); self._skb_draw_missile(painter); self._skb_draw_smoke_arr(painter,'skb_esmoke',now,SKB_ESMOKE_LIFE_MS,True); self._skb_draw_enemy_missile(painter); self._skb_draw_explosion(painter,now); self._skb_draw_hud(painter,now); self._skb_draw_reticle(painter,now)

    # ----- Lighting 合成基盤（v0.5 Phase 3。CoreS3 sceneDrawLightingLayer()相当）-----
    def _draw_lighting_background(self, painter: QPainter, fft01, now_ms: int, now_sec: float) -> None:
        """CoreS3 sceneDrawLightingLayer()に相当するLayer0。index0="None"は何も描かず
        白背景のまま（painter.fillRect済み）。モード切替の1フレーム目はneeds_init=True
        （CoreS3のneedsInitと同じ役割）で各Lightingの内部状態を初期化させる。"""
        idx = self.light_display_mode
        needs_init = (idx != self.light_prev_display_mode)
        self.light_prev_display_mode = idx
        # v0.5 Phase 4a: CoreS3のgEyeSlotActiveと同じく、Eye Slot以外へ切り替わったら
        # 必ず一度falseへ戻す（このフレームで実際にEye Slotが描かれた時だけtrueに戻る）。
        self.eslot_active = False
        if idx <= LIGHT_NONE or idx >= len(LIGHT_MODES):
            return
        level = float(np.mean(fft01))
        bass = float(np.mean(fft01[:2]))
        n = len(fft01)
        s_idx = min(n - 1, (n * 3) // 4) if n > 0 else 0
        treble = float(np.mean(fft01[s_idx:])) if n > 0 else 0.0

        if idx == LIGHT_DISCO:
            self._light_render_disco(painter, level, bass, treble, now_ms, needs_init)
        elif idx == LIGHT_LASER:
            self._light_render_laser(painter, level, bass, treble, now_ms, needs_init)
        elif idx == LIGHT_AURORA:
            self._light_render_aurora(painter, level, bass, treble, now_ms, needs_init)
        elif idx == LIGHT_MATRIX:
            self._light_render_matrix(painter, level, bass, treble, now_ms, needs_init)
        elif idx == LIGHT_RACE:
            self._light_render_race(painter, level, now_ms, needs_init)
        elif idx == LIGHT_SKYRAID:
            self._light_render_skyraid(painter, level, now_ms, needs_init)
        elif idx == LIGHT_EYESLOT:
            self._light_render_eyeslot(painter, now_ms, needs_init)
        elif idx == LIGHT_CLASSICRACE:
            self._light_render_classicrace(painter, level, now_ms, needs_init)
        elif idx == LIGHT_ASTEROID:
            self._light_render_asteroid(painter, now_ms, needs_init)
        elif idx == LIGHT_TUNNEL:
            self._light_render_tunnel(painter, now_ms, needs_init)
        elif idx == LIGHT_PACMAN:
            self._light_render_pacman(painter, level, now_ms, needs_init)
        elif idx == LIGHT_STREETFIGHTER:
            self._light_render_streetfighter(painter, level, now_ms, needs_init)
        elif idx == LIGHT_MARIO:
            self._light_render_mario(painter, level, now_ms, needs_init)
        elif idx == LIGHT_MISSILE:
            self._light_render_missile(painter, now_ms, needs_init)
        elif idx == LIGHT_PSYCHE:
            self._light_render_psyche(painter, now_ms, needs_init)
        elif idx == LIGHT_VORTEX:
            self._light_render_vortex(painter, now_ms, needs_init)
        elif idx == LIGHT_AQUARIUM:
            self._light_render_aquarium(painter, now_ms, needs_init)
        elif idx == LIGHT_FLYINGPOMPADOUR:
            self._light_render_flyingpompadour(painter, now_ms, needs_init)
        elif idx == LIGHT_RAINBOWWASHER:
            self._light_render_rwm(painter, now_ms, needs_init)
        elif idx == LIGHT_PIXELINVASION:
            self._light_render_pixelinvasion(painter, now_ms, needs_init)
        elif idx == LIGHT_FLOWERCLOCK:
            self._light_render_flowerclock(painter)
        elif idx == LIGHT_PINBALL:
            self._light_render_pinball(painter, now_ms, needs_init)
        elif idx == LIGHT_BASEBALL:
            self._light_render_baseball(painter, now_ms, needs_init)
        elif idx == LIGHT_SKYBURNER:
            self._light_render_skyburner(painter, now_ms, needs_init)

    def _draw_lighting_foreground(self, painter: QPainter, now_sec: float) -> None:
        """CoreS3 sceneComposeAndPush()の4.5/4.6フックに相当。Flower Clockの針・
        PINBALLの目バンパー等、顔より最前面に描く必要があるLighting固有要素をここに置く
        （Phase 5で残りを実装。Character=Noneでもこれらは描き続ける設計にする）。"""
        if self.light_display_mode == LIGHT_FLOWERCLOCK:
            self._fc_draw_hands_foreground(painter)
        elif self.light_display_mode == LIGHT_PINBALL:
            self._pin_draw_foreground(painter, self._now_ms())

    def paintEvent(self, event) -> None:  # noqa: ARG002
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, False)
        painter.fillRect(self.rect(), QColor("white"))
        painter.scale(self.scale_factor, self.scale_factor)

        _speaking, _rms, _error, fft_levels = self.audio_state.snapshot()
        fft01 = np.clip(np.asarray(fft_levels, dtype=float) / 100.0, 0.0, 1.0)
        now_sec = time.monotonic()
        now_ms = self._now_ms()

        # Layer 0: Lighting（背景）
        self._draw_lighting_background(painter, fft01, now_ms, now_sec)

        # Layer 1 + Layer 2: Visualizer（各メソッドが最後にself._draw_face()を呼ぶ）
        mode = self.viz_display_mode
        if mode == 0:
            self._draw_face(painter)
        elif mode == 1:
            self._draw_eq(painter, fft_levels)
        elif mode == 2:
            self._draw_halo(painter, fft_levels)
        elif mode == 3:
            self._draw_mirror(painter, fft_levels)
        elif mode == 4:
            self._draw_rhythm(painter)
        elif mode == 5:
            self._draw_kaleido(painter, fft_levels, now_sec)
        elif mode == 6:
            self._draw_analog_vu(painter)
        elif mode == 7:
            self._draw_tetromino(painter)
        elif mode == 8:
            self._update_flash_spotlight(now_ms)
            self._draw_flash_spotlight(painter, fft01)

        # Layer 3: Lighting前景フック（Phase 4/5で実装）
        self._draw_lighting_foreground(painter, now_sec)

        painter.end()



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


# =============================================================================
# KariPom BBX panel — Desktop-style 3-button UI
# Left third = previous / center third = no-op / right third = next.
# =============================================================================

class SplitNavButton(QPushButton):
    """3分割ナビゲーションの左右操作領域を、文字列とは独立した細いシェブロンで示す。

    ラベル文字は従来どおり中央配置のまま維持し、左右端にだけ薄い ‹ › 相当の
    ベクター線を重ねることで、括弧に見えにくくしつつ前後操作を示す。
    クリック判定そのもの（左1/3=前、中央1/3=no-op、右1/3=次）は変更しない。
    """

    def __init__(self, hint_rgb, parent=None):
        super().__init__(parent)
        self._hint_rgb = hint_rgb

    def paintEvent(self, event):
        super().paintEvent(event)

        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)

        color = QColor(*self._hint_rgb)
        color.setAlpha(145)
        pen = QPen(color)
        pen.setWidthF(1.7)
        pen.setCapStyle(Qt.RoundCap)
        pen.setJoinStyle(Qt.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.NoBrush)

        # 文字から十分離れた左右端へ配置する。記号を文字列に含めないため、
        # "‹ Character ›"のような括弧表現には見えない。
        cy = self.height() * 0.5
        span = max(4.0, min(7.0, self.height() * 0.095))
        margin = max(9.0, min(15.0, self.width() * 0.065))
        lx = margin
        rx = self.width() - margin
        dx = span * 0.55

        painter.drawLine(QPointF(lx + dx, cy - span), QPointF(lx - dx, cy))
        painter.drawLine(QPointF(lx - dx, cy), QPointF(lx + dx, cy + span))
        painter.drawLine(QPointF(rx - dx, cy - span), QPointF(rx + dx, cy))
        painter.drawLine(QPointF(rx + dx, cy), QPointF(rx - dx, cy + span))
        painter.end()

class KariPomBBXPanel(QWidget):
    def __init__(self, audio_state, visualizer_ready_cb, scale=2, parent=None):
        super().__init__(parent)
        self.visualizer_ready_cb = visualizer_ready_cb
        self.canvas = KariPomDesktopCanvas(audio_state, scale)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        root.addWidget(self.canvas)

        canvas_width = CANVAS_W * scale
        btn_w = canvas_width // 3
        last_btn_w = canvas_width - btn_w * 2

        probe = QPushButton()
        self._btn_base_pt = probe.font().pointSize()
        if self._btn_base_pt <= 0:
            self._btn_base_pt = BTN_FONT_PT_FALLBACK
        self._btn_base_pt += BTN_FONT_PT_BOOST
        probe_font = QFont()
        probe_font.setBold(True)
        probe_font.setPointSize(self._btn_base_pt)
        line_h = QFontMetrics(probe_font).height()
        btn_h = max(BTN_HEIGHT_BASE + BTN_HEIGHT_PER_SCALE * scale, line_h * 3 + 14)

        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(0)
        self.btn_character = SplitNavButton(BTN_FG_ON_BLUE)
        self.btn_visualizer = SplitNavButton(BTN_FG_ON_WHITE)
        self.btn_lighting = SplitNavButton(BTN_FG_ON_RED)
        self.btn_character.setStyleSheet(self._button_style(BTN_BG_CHARACTER, BTN_FG_ON_BLUE))
        self.btn_visualizer.setStyleSheet(self._button_style(BTN_BG_VISUALIZER, BTN_FG_ON_WHITE))
        self.btn_lighting.setStyleSheet(self._button_style(BTN_BG_LIGHTING, BTN_FG_ON_RED))
        widths = (btn_w, btn_w, last_btn_w)
        self._btn_target_width = {}
        for btn, w in zip((self.btn_character, self.btn_visualizer, self.btn_lighting), widths):
            btn.setFixedWidth(w)
            btn.setFixedHeight(btn_h)
            self._btn_target_width[id(btn)] = w
            btn.installEventFilter(self)
            row.addWidget(btn)
        root.addLayout(row)
        self._refresh_buttons()

    def reset_visualizer(self):
        """PC音声条件を満たさない時は、見た目と内部状態をFaceへ完全に戻す。"""
        self.canvas.viz_manual_index = 0
        self.canvas.viz_display_mode = 0
        self.canvas.viz_random_last_switch_ms = 0
        self.canvas.viz_random_last_pick = -1
        self.canvas.update()
        self._refresh_buttons()

    def eventFilter(self, obj, event):
        if obj in (self.btn_character, self.btn_visualizer, self.btn_lighting):
            if event.type() == QEvent.MouseButtonRelease and event.button() == Qt.LeftButton:
                if not obj.rect().contains(event.pos()):
                    return True
                x = event.pos().x()
                third = obj.width() / 3.0
                if x < third:
                    direction = -1
                elif x >= third * 2:
                    direction = 1
                else:
                    return True

                if obj is self.btn_character:
                    self.canvas.cycle_character(direction)
                elif obj is self.btn_visualizer:
                    next_index = (self.canvas.viz_manual_index + (1 if direction >= 0 else -1)) % (len(VIS_MODES) + 1)
                    # Face(index 0)以外、Random(index len(VIS_MODES))を選ぶ時だけPC音声環境を確認。
                    if next_index != 0 and not self.visualizer_ready_cb():
                        self.reset_visualizer()
                        return True
                    self.canvas.cycle_visualizer(direction)
                else:
                    self.canvas.cycle_lighting(direction)
                self._refresh_buttons()
                return True
        return super().eventFilter(obj, event)

    @staticmethod
    def _button_style(bg_rgb, fg_rgb):
        bg = QColor(*bg_rgb)
        fg = QColor(*fg_rgb)
        border = bg.darker(130) if bg_rgb != BTN_BG_VISUALIZER else QColor(180, 180, 180)
        hover = bg.darker(112) if bg_rgb == BTN_BG_VISUALIZER else bg.lighter(115)
        pressed = bg.darker(122)
        return (
            f"QPushButton {{ background-color: {bg.name()}; color: {fg.name()}; border: 1px solid {border.name()}; }}"
            f"QPushButton:hover {{ background-color: {hover.name()}; }}"
            f"QPushButton:pressed {{ background-color: {pressed.name()}; }}"
        )

    @staticmethod
    def _wrap_value(value):
        spaces = [i for i, ch in enumerate(value) if ch == " "]
        if not spaces:
            return None
        mid = len(value) / 2.0
        split_at = min(spaces, key=lambda i: abs(i - mid))
        return [value[:split_at], value[split_at + 1:]]

    def _fit_button_text(self, btn, label, value):
        avail_w = max(10, self._btn_target_width[id(btn)] - 10)
        font = QFont()
        font.setBold(True)

        def widest(lines, pt):
            font.setPointSize(pt)
            return max(QFontMetrics(font).horizontalAdvance(line) for line in lines)

        base_pt = self._btn_base_pt
        lines = [label, value]
        if widest(lines, base_pt) <= avail_w:
            font.setPointSize(base_pt)
            btn.setFont(font)
            btn.setText("\n".join(lines))
            return
        wrapped = self._wrap_value(value)
        if wrapped is not None:
            lines = [label] + wrapped
            if widest(lines, base_pt) <= avail_w:
                font.setPointSize(base_pt)
                btn.setFont(font)
                btn.setText("\n".join(lines))
                return
        pt = base_pt
        while pt > BTN_FONT_PT_MIN and widest(lines, pt) > avail_w:
            pt -= 1
        font.setPointSize(pt)
        btn.setFont(font)
        btn.setText("\n".join(lines))

    def _refresh_buttons(self):
        c = self.canvas
        self._fit_button_text(self.btn_character, "Character:", CHARACTER_MODES[c.character_mode])
        viz_label = "Random" if c.visualizer_random_on() else VIS_MODES[c.viz_manual_index]
        self._fit_button_text(self.btn_visualizer, "Visualizer:", viz_label)
        light_label = "Random" if c.lighting_random_on() else LIGHT_MODES[c.light_manual_index]
        self._fit_button_text(self.btn_lighting, "Lighting:", light_label)


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
        # 2026-08-17: KariPom Desktopと同じ320x240描画エンジン＋下部3ボタンGUIへ統一。
        face_page = QWidget()
        face_lay = QVBoxLayout(face_page)
        face_lay.setContentsMargins(12, 12, 12, 12)
        face_lay.addStretch()
        self.bbx_panel = KariPomBBXPanel(self._audio_state, self._ensure_visualizer_ready, scale=2)
        self.face_widget = self.bbx_panel.canvas  # 既存コード/将来互換の参照名を維持
        face_row = QHBoxLayout()
        face_row.addStretch()
        face_row.addWidget(self.bbx_panel)
        face_row.addStretch()
        face_lay.addLayout(face_row)
        self.lbl_face_mode = QLabel("各ボタン：左1/3=前へ／中央=無操作／右1/3=次へ　（Visualizer・LightingはDesktopと同じ全モード＋Random）")
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

    # ─── Visualizer開始前チェック（2026-08-17）────────────────
    def _open_macos_screen_capture_settings(self):
        """macOSの『画面とシステムオーディオの録音』設定を開く。"""
        try:
            subprocess.Popen([
                "open",
                "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture",
            ])
        except Exception as exc:
            QMessageBox.warning(
                self,
                "システム設定を開けません",
                f"システム設定 → プライバシーとセキュリティ → "
                f"画面とシステムオーディオの録音 を開いてKariPomを許可してください。\n\n{exc}",
            )

    def _show_sck_permission_dialog(self):
        box = QMessageBox(self)
        box.setIcon(QMessageBox.Warning)
        box.setWindowTitle("macOSの音声取得を許可してください")
        box.setText("KariPomがMacの再生音を取得する権限が許可されていません。")
        box.setInformativeText(
            "システム設定の『プライバシーとセキュリティ』→"
            "『画面とシステムオーディオの録音』で、"
            "KariPomを実行しているアプリ（開発中はTerminal）を許可してください。\n\n"
            "許可後、Talkを開始してからもう一度Visualizerを選んでください。"
        )
        settings_btn = box.addButton("システム設定を開く", QMessageBox.ActionRole)
        box.addButton("閉じる", QMessageBox.RejectRole)
        box.exec_()
        if box.clickedButton() is settings_btn:
            self._open_macos_screen_capture_settings()

    def _ensure_visualizer_ready(self):
        """BBXでFace以外のVisualizer/Randomへ入る直前に呼ぶ。
        条件を満たさない場合はOS別ダイアログを出し、呼び出し側がVisualizerをFaceへ戻す。
        macOSはBlackHoleを使わずScreenCaptureKitを使用する。
        """
        error_code, error_detail = self._audio_state.error_info()
        if platform.system() == "Darwin" and error_code == "SCK_PERMISSION_DENIED":
            self._show_sck_permission_dialog()
            return False

        if not self._talk_is_running():
            box = QMessageBox(self)
            box.setIcon(QMessageBox.Warning)
            box.setWindowTitle("Visualizerを開始できません")
            if platform.system() == "Darwin" and error_code:
                box.setText("macOSのPC音声取得が停止しています。")
                box.setInformativeText(
                    f"{error_detail or error_code}\n\n"
                    "Talkを開始してから、もう一度Visualizerを選んでください。"
                    "\n\nVisualizerはFaceへ戻します。"
                )
            else:
                box.setText("CompanionのTalk（PC音声取得）が停止しています。")
                box.setInformativeText(
                    "VisualizerにはPC音声の取得が必要です。Talkを開始してから、"
                    "もう一度Visualizerを選んでください。\n\nVisualizerはFaceへ戻します。"
                )
            start_btn = box.addButton("Talkを開始", QMessageBox.AcceptRole)
            box.addButton("キャンセル", QMessageBox.RejectRole)
            box.exec_()
            if box.clickedButton() is start_btn:
                self._talk_start(silent=False)
                self._talk_update_ui()
            return False

        ok, code, detail = self._talk_engine.inspect_audio_environment()
        if ok:
            return True

        os_name = platform.system()
        box = QMessageBox(self)
        box.setIcon(QMessageBox.Warning)
        box.setWindowTitle("Visualizerの音声環境を確認してください")

        if code == "UNSUPPORTED_MACOS":
            box.setText("このmacOSではScreenCaptureKit音声取得を使用できません。")
            box.setInformativeText(
                "KariPom CompanionのBlackHole不要モードはmacOS 13 Ventura以降が必要です。"
                "\n\nVisualizerはFaceへ戻します。"
            )
        elif code == "MISSING_HELPER_BINARY":
            box.setText("ScreenCaptureKit helperが見つかりません。")
            box.setInformativeText(
                f"{detail}\n\n"
                "mac-companion/resources/ に同梱されているはずのhelperが見当たりません。"
                "配布物（リポジトリ）が壊れていないか確認し、KariPom Companionを再起動してください。"
                "\n\nVisualizerはFaceへ戻します。"
            )
        elif code == "MISSING_SOUNDCARD":
            box.setText("Windows/Linux用の音声ライブラリ soundcard が見つかりません。")
            extra = (
                "\nLinuxでは libpulse0（PipeWire環境では pipewire-pulse）も確認してください。"
                if os_name == "Linux" else ""
            )
            box.setInformativeText(
                "KariPom CompanionのVisualizerには soundcard が必要です。"
                "\n\npip install soundcard" + extra +
                "\n\n準備後、もう一度Visualizerを選んでください。VisualizerはFaceへ戻します。"
            )
        elif code == "MISSING_LOOPBACK":
            if os_name == "Windows":
                box.setText("WindowsのPC再生音ループバックが見つかりません。")
                box.setInformativeText(
                    "通常、追加の仮想オーディオソフトは不要です。Windowsの再生デバイスが"
                    "有効になっていることを確認してください。\n\n"
                    "確認後、もう一度Visualizerを選んでください。VisualizerはFaceへ戻します。"
                )
            else:
                box.setText("Linuxのmonitor sourceが見つかりません。")
                box.setInformativeText(
                    "PulseAudio / PipeWire のmonitor sourceを利用できる状態にしてください。"
                    "libpulse0、PipeWire環境ではpipewire-pulseも確認してください。\n\n"
                    "確認後、もう一度Visualizerを選んでください。VisualizerはFaceへ戻します。"
                )
        else:
            box.setText("PC音声環境を確認できませんでした。")
            box.setInformativeText(
                f"{detail}\n\n設定を確認後、もう一度Visualizerを選んでください。"
                "VisualizerはFaceへ戻します。"
            )

        box.addButton("閉じる", QMessageBox.RejectRole)
        box.exec_()
        return False


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
            if error:
                self.lbl_face_status.setText(f"Audio Error: {error}")
            else:
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
# numpy はBBX/Desktop互換描画でもGUI起動時から使用するため、上部でimport済みの
# グローバル np を維持する。soundcardのみTalk起動時（Windows/Linux）に遅延importする。
# macOSはBlackHole/sounddeviceを使わず、Apple純正ScreenCaptureKitを使用する。
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


# ===== デバイス検索（OS依存部）=====
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
        try:
            helper_path = locate_mac_sck_helper()
            print(f"macOS: ScreenCaptureKit helper = {helper_path}")
        except Exception as e:
            print(f"macOS: ScreenCaptureKit helperを確認できません ({e})")
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


_mac_sck_permission_denied = False
_mac_sck_last_detail = ""


# ===== Mac: ScreenCaptureKit helper（別プロセス）から取得（BlackHole不要）=====
def _run_mac_sck_capture(helper_path, generation):
    """ScreenCaptureKit helperをサブプロセスとして起動し、Float32 mono PCMを
    process_block()へ渡す取得スレッド本体（Windows/Linuxの_capture_loop()に相当）。
    権限拒否を検出した場合は_mac_sck_permission_deniedを立てて終了する
    （再試行しても解決しないため、呼び元run_talk_mode()はこれを見て再試行を止める）。"""
    global _mac_sck_permission_denied, _mac_sck_last_detail

    proc = None
    stderr_lines = []
    try:
        proc = subprocess.Popen(
            [str(helper_path)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )

        def stderr_reader():
            assert proc is not None and proc.stderr is not None
            for raw in iter(proc.stderr.readline, b""):
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    stderr_lines.append(line)
                    print(f"[ScreenCaptureKit] {line}", file=sys.stderr)

        threading.Thread(target=stderr_reader, name="KariPomTalkSCKStderr",
                          daemon=True).start()

        assert proc.stdout is not None
        pending = bytearray()
        bytes_per_block = BLOCKSIZE * 4  # Float32 mono
        while _capture_generation == generation:
            chunk = proc.stdout.read(max(4096, bytes_per_block - len(pending)))
            if not chunk:
                break
            pending.extend(chunk)
            while len(pending) >= bytes_per_block:
                raw_block = bytes(pending[:bytes_per_block])
                del pending[:bytes_per_block]
                mono = np.frombuffer(raw_block, dtype=np.float32).copy()
                if mono.size == BLOCKSIZE:
                    process_block(mono)
    except Exception as e:
        print(f"\n[watchdog] ScreenCaptureKit capture エラー: {type(e).__name__}: {e}",
              file=sys.stderr)
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                proc.kill()
        detail = stderr_lines[-1] if stderr_lines else ""
        if detail and EmbeddedTalkEngine._is_sck_permission_denied(detail):
            _mac_sck_permission_denied = True
            _mac_sck_last_detail = detail


def _run_stream_sck(helper_path):
    """取得スレッドを起動し、Mac版_run_stream()と同じ監視・FFTループを回す。
    停止検出時は return して呼び元に再起動を促す。権限拒否時は
    _mac_sck_permission_denied が立った状態で戻るため、呼び元（run_talk_mode）は
    再試行せず終了する。"""
    global _last_callback_time, FFT_ENABLED, _capture_generation

    _capture_generation += 1
    generation = _capture_generation
    _last_callback_time = time.time()   # 起動直後はタイムアウトさせない

    th = threading.Thread(target=_run_mac_sck_capture, args=(helper_path, generation),
                          daemon=True)
    th.start()

    fft_error_count = 0
    while True:
        # ---- ウォッチドッグ ----
        if not th.is_alive():
            print("\n[watchdog] ScreenCaptureKit取得スレッドが停止しました。再起動します...",
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
    global np, sc
    global THRESHOLD, FFT_ENABLED, FFT_SEND_ENABLED, INPUT_HINT
    global M5_IP, sock
    global _fft_window, _fft_freqs, _fft_band_bins, _fft_levels, _FFT_BAR_CHARS
    global _capture_generation
    global _last_ip_reload_check

    # ===== 音声ライブラリの読み込み（Talkモード起動時のみ実行）=====
    #   Mac          : Apple純正ScreenCaptureKit（同梱の事前ビルド済みhelperを起動。
    #                  BlackHole/sounddeviceは不要）
    #   Windows/Linux: soundcard（WASAPI Loopback / Pulseモニター対応）
    import numpy as _np
    np = _np

    if IS_MAC:
        pass
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
        print("=== Karipom Mac Audio Link (ScreenCaptureKit) ===")
        try:
            helper_path = locate_mac_sck_helper()
        except Exception as e:
            print(f"ScreenCaptureKit helperが見つかりません: {e}")
            sys.exit(1)
        print(f"Using ScreenCaptureKit helper: {helper_path}")
        device_id = None
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
            # ---- Mac: ScreenCaptureKit helper（別プロセス）を使用。BlackHole不要 ----
            while True:
                try:
                    # 取得スレッドを起動して監視ループを回す。停止検出で return してくる。
                    _run_stream_sck(helper_path)

                except Exception as e:
                    # helper起動失敗・一時的な取得エラーなど、あらゆる例外をここで
                    # 受け止め、プロセスを終了させない（権限拒否は下でsys.exit(1)する）。
                    print(f"\n[watchdog] ScreenCaptureKit エラー: {type(e).__name__}: {e}",
                          file=sys.stderr)
                    send_message(b"SPEAK_STOP")

                if _mac_sck_permission_denied:
                    print("\nmacOSの『画面とシステムオーディオの録音』権限が許可されていません。",
                          file=sys.stderr)
                    print(f"詳細: {_mac_sck_last_detail}", file=sys.stderr)
                    print("システム設定 → プライバシーとセキュリティ → 画面とシステムオーディオの録音 で"
                          "許可してから、もう一度起動してください。", file=sys.stderr)
                    sys.exit(1)

                # ---- 自己復旧：一時停止（スリープ復帰等）後に再オープン ----
                print(f"[watchdog] {STREAM_RESTART_WAIT}秒後に再起動します...",
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
