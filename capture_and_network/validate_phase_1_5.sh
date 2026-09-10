#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPORT_FILE="$ROOT_DIR/validation_report.md"
LOG_FILE="$ROOT_DIR/validation_log.txt"

: > "$LOG_FILE"

{
  printf '%s\n' '# Phase 1-5 验证报告'
  printf '%s\n' ''
  printf '%s\n' '## 1. 执行环境'
  printf '%s\n' "- 时间: $(date '+%Y-%m-%d %H:%M:%S %Z')"
  printf '%s\n' "- 工作目录: $ROOT_DIR"
  printf '%s\n' "- OS: $(uname -a)"
  printf '%s\n' ''
  printf '%s\n' '## 2. 验证脚本'
  printf '%s\n' '```bash'
  printf '%s\n' 'cmake -S . -B build'
  printf '%s\n' 'cmake --build build -j2'
  printf '%s\n' './build/test_sender_spool_resume'
  printf '%s\n' './build/test_reliable_resume'
  printf '%s\n' './build/test_phase5_checkpoint'
  printf '%s\n' './build/test_processing_resume'
  printf '%s\n' '```'
  printf '%s\n' ''

  printf '%s\n' '## 3. 编译与构建'
  cd "$ROOT_DIR"
  printf '%s\n' '```bash'
  cmake -S . -B build
  printf '%s\n' '---'
  cmake --build build -j2
  printf '%s\n' '```'
  printf '%s\n' ''

  printf '%s\n' '## 4. 回归测试执行'
  TESTS=(
    './build/test_sender_spool_resume'
    './build/test_reliable_resume'
    './build/test_phase5_checkpoint'
    './build/test_processing_resume'
  )

  for test in "${TESTS[@]}"; do
    printf '%s\n' "### 运行: $test"
    printf '%s\n' '```bash'
    "$test"
    RC=$?
    printf '%s\n' '```'
    printf '%s\n' "- 退出码: $RC"
    printf '%s\n' ''
    if [ "$RC" -ne 0 ]; then
      printf '%s\n' '## 5. 结论'
      printf '%s\n' "- 失败：$test 返回退出码 $RC"
      exit "$RC"
    fi
  done

  printf '%s\n' '## 5. 结论'
  printf '%s\n' '- 结果：全部验证通过'
  printf '%s\n' '- 结论：Phase 2-5 关键恢复与 checkpoint 逻辑已通过回归验证'
  printf '%s\n' ''
} > "$REPORT_FILE" 2>&1

cat "$REPORT_FILE"
