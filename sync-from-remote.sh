#!/bin/bash
# =============================================================================
# sync-from-remote.sh —— 把远端 dev-server 的项目文件拉到本地
# =============================================================================
# 背景：本地这份是远端的旧快照（本地 HEAD 7104ed1，远端 125cc4b + 未提交 WIP）。
#       CLion 配「远程工具链」后会把本地文件推到远端构建，本地不对齐就会
#       把远端的 reference collapsing 改动推平。所以先跑这个对齐。
#
# 用法：
#   ./sync-from-remote.sh            干跑，只打印将要变更的清单（默认）
#   ./sync-from-remote.sh --apply    真正写入本地
#
# 不要加 sudo：远端侧的用户名由 ~/.ssh/config 里的 dev-server（root）决定，
#             本地加 sudo 会把本地文件属主改成 root。
# =============================================================================

set -euo pipefail
cd "$(dirname "$0")"

# 注意：故意用字符串而非数组。macOS 自带 bash 3.2 在 set -u 下展开空数组
#（"${DRY[@]}"）会报 unbound variable，干跑能过、真写必炸。
DRY="-n"
if [ "${1:-}" = "--apply" ]; then
    DRY=""
    echo "  ⚠️   真写模式，将覆盖本地文件"
else
    echo "  🔍  干跑模式（只列出变更，不写盘）；确认后加 --apply"
fi

# ConnectTimeout 调大：远端负载高时 sshd 的 banner 要等 20~40s 才吐出来
rsync -az ${DRY} --itemize-changes \
    --exclude 'build/' \
    --exclude 'build-linux/' \
    --exclude 'cmake-build-debug/' \
    --exclude '.build/' \
    --exclude 'Testing/' \
    --exclude '.cache/' \
    --exclude '_deps/' \
    --exclude '.logbaseline/' \
    --exclude '.git/' \
    --exclude '*.o' \
    --exclude 'minicc' \
    --exclude '.idea/workspace.xml' \
    -e "ssh -o ConnectTimeout=40" \
    dev-server:/root/cppproject/mycompiler/ ./
