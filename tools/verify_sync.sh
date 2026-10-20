#!/usr/bin/env bash
# 音畫同步回歸測試。
#
# 判準有兩個，缺一不可：
#   1. 絕對值：|呈現差| <= 10ms。素材源（anim_window --sync）現在是樣本鎖定的
#      （見 src/probe/anim_window.cpp 的說明），殘差 ~2ms，所以這條門檻有意義。
#   2. 一致性：CLI 與 GUI 兩條路的答案必須相同（差 <= 10ms）。
#
# 為什麼第 2 條不能省：recorder.cpp 的那個 bug（音軌少了 gap 補償）在 CLI 上
# 剛好被素材源自己的延遲抵掉，量出來一直是「對齊」。只量一條路會漏掉它。
#
# 用法： bash verify_sync.sh            （SECS=6 預設）
#        SECS=10 bash verify_sync.sh
set -u
cd "$(dirname "$0")"

SECS=${SECS:-6}
FREQ=${FREQ:-3000}
PERIOD=${PERIOD:-400}
TOL=${TOL:-10}

# 診斷日誌（avsync.log）預設是關的，這裡打開才看得到 gap / firstPacketQpc
export CAPWGC_AVSYNC_LOG=1

kill_anim() {
  taskkill //F //IM anim_window.exe >/dev/null 2>&1
  sleep 0.5
}

start_anim() {
  kill_anim
  rm -f anim_sync.log
  ./anim_window.exe --sync --interval "$PERIOD" --borderless --seconds 30 >anim.log 2>&1 &
  sleep 2
}

# 從 mp4_check 的輸出裡抓「呈現時間差」的毫秒數（可能帶小數）
offset_of() {
  ./mp4_check.exe --sync "$1" --freq "$FREQ" --period "$PERIOD" 2>&1 |
    sed -n 's/.*呈現時間差 = \([-+0-9.]*\)ms.*/\1/p' | head -1
}

verdict() {  # $1=值 $2=標籤
  local v=$1 label=$2
  if [ -z "$v" ]; then echo "FAIL $label（量不到事件）"; return 1; fi
  local a=${v#-}
  if awk "BEGIN{exit !($a <= $TOL)}"; then echo "PASS $label ${v}ms"; return 0
  else echo "FAIL $label ${v}ms（超過 ±${TOL}ms）"; return 1; fi
}

echo "== CLI =="
start_anim
rm -f "$LOCALAPPDATA/CapWGC/avsync.log"
./CapCli.exe --out sync_cli_reg.mp4 --monitor --seconds "$SECS" --audio --progressive 2>&1 |
  grep -E '^sync|^offset'
CLI_V=$(offset_of sync_cli_reg.mp4)
verdict "$CLI_V" "CLI"

echo "== GUI (Recorder) =="
start_anim
rm -f "$LOCALAPPDATA/CapWGC/avsync.log"
./CapWGC.exe --selftest "$SECS" --audio
GUIFILE=$(ls -t "$LOCALAPPDATA/CapWGC/"*.mp4 | head -1)
echo "gui file: $GUIFILE"
grep -E 'offset|first AU|first write|skip' "$LOCALAPPDATA/CapWGC/avsync.log" 2>/dev/null | head -8
GUI_V=$(offset_of "$GUIFILE")
verdict "$GUI_V" "GUI"

kill_anim

# 一致性
if [ -n "$CLI_V" ] && [ -n "$GUI_V" ]; then
  # 注意要包括號：值是帶正負號的字串，`7.6--1.2` 會被 awk 當成語法錯誤
  D=$(awk "BEGIN{d=($CLI_V)-($GUI_V); if(d<0)d=-d; printf \"%.1f\", d}")
  if awk "BEGIN{exit !($D <= $TOL)}"; then
    echo "PASS 一致性（兩條路差 ${D}ms）"
  else
    echo "FAIL 一致性（兩條路差 ${D}ms）—— 管線本身不一致，不是素材源的問題"
  fi
fi
