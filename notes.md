# Pintos Project 1 笔记

## 0. 实验之前

### 0.1 基线 baseline
- 日期：2026-10-20
- commit：046e4dc
- tag：baseline
- 复现：cd src/threads && make && make check 2>&1 | tee ~/baseline-threads.log
- 规模：全套 27 个测试 → 7 pass / 20 FAIL
- 稳定性：连跑 3 次，结果**逐行一致**，无 flaky

#### 强红线（pass→FAIL 立即停手回查）
alarm-single
alarm-multiple
alarm-simultaneous
alarm-zero
alarm-negative

#### 弱红线（判据宽松，RR 碰巧满足；稳定但不过度解读）
mlfqs-fair-2
mlfqs-fair-20

#### 20 个 FAIL = 路线图
alarm-priority                          → 阶段 A 收尾（需先做闹钟主体）
priority-change / -fifo / -preempt      → 阶段 A
priority-sema / -condvar                → 阶段 A（唤醒后抢占）
priority-donate-one/multiple/multiple2/nest/sema/lower/chain → 阶段 B
mlfqs-load-1/load-60/load-avg/recent-1/nice-2/nice-10/block → 阶段 C

### 0.2 回滚点验收（baseline tag）
- baseline = 046e4dc "Add build workflow"，与 proj1 起点同一个 commit（纯净基线）
- 验证方式：git worktree add /tmp/baseline-wt baseline && make → 退出码 0
- 工具链形态：gcc -m32（x86_64 + multilib，非交叉编译）
- ⚠️ 已知噪音 warnings（lib/ devices/ tests/ 内的，一律忽略）：
    init.c noreturn-return / debug.c frame-address / string.c nonnull-compare
    / alarm-wait.c format-truncation
- 待确认：`date` 输出
