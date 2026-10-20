# Pintos Project 1 笔记

## 基线 baseline
- 日期：2026-10-20
- commit：046e4dc
- tag：baseline
- 复现：cd src/threads && make && make check 2>&1 | tee ~/baseline-threads.log
- 规模：全套 27 个测试 → 7 pass / 20 FAIL
- 稳定性：连跑 3 次，结果**逐行一致**，无 flaky

### 强红线（pass→FAIL 立即停手回查）
alarm-single
alarm-multiple
alarm-simultaneous
alarm-zero
alarm-negative

### 弱红线（判据宽松，RR 碰巧满足；稳定但不过度解读）
mlfqs-fair-2
mlfqs-fair-20

### 20 个 FAIL = 路线图
alarm-priority                          → 阶段 A 收尾（需先做闹钟主体）
priority-change / -fifo / -preempt      → 阶段 A
priority-sema / -condvar                → 阶段 A（唤醒后抢占）
priority-donate-one/multiple/multiple2/nest/sema/lower/chain → 阶段 B
mlfqs-load-1/load-60/load-avg/recent-1/nice-2/nice-10/block → 阶段 C
