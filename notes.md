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

#### 基线文件位置
- 基线日志：/root/baseline-threads.log（若在 /home/student 则后者为准）
- 强/弱红线名单：/root/baseline-pass.txt（7 行，sort 后顺序）
- 每次回归：make check → /tmp/after-alarm.log → grep pass → diff 上面这个文件

### 0.2 回滚点验收（baseline tag）
- baseline = 046e4dc "Add build workflow"，与 proj1 起点同一个 commit（纯净基线）
- 验证方式：git worktree add /tmp/baseline-wt baseline && make → 退出码 0
- 工具链形态：gcc -m32（x86_64 + multilib，非交叉编译）
- ⚠️ 已知噪音 warnings（lib/ devices/ tests/ 内的，一律忽略）：
    init.c noreturn-return / debug.c frame-address / string.c nonnull-compare
    / alarm-wait.c format-truncation
- 待确认：`date` 输出

## 1. Task 1：闹钟时钟——用阻塞替代忙等待

### 1.0 一句话结论

把 `timer_sleep` 的「while 循环 + thread_yield 忙等」换成「入睡眠队列 + block，由时钟中断唤醒」。

**5/5 alarm 强红线仍 green，全套结果与基线零差异（7 pass / 20 FAIL）** —— 这不是"没做事"，而是忙等待在**功能上本来就正确**（时间照样走得准，只是 CPU 空转）。所以 Task 1 的交付物是"CPU 不再空转"，必须靠 1.6 的额外证据验收，**不能只信 make check**。

- 完成日期：2026-10-2x
- 起点 commit：046e4dc (baseline)
- 改动文件：`threads/thread.h` / `threads/thread.c` / `devices/timer.c`（3 个，无新增/删除文件）

---

### 1.1 可验收判据（三条，缺一不可）

| # | 判据 | 怎么查 |
|---|---|---|
| A | 5 条 alarm 强红线全 pass | `make check` 输出 |
| B | 编译零 error，且**无 `unused-function` / `implicit-declaration`** | 1.6 的过滤命令 |
| C | 忙等待代码彻底消失（不是新旧两条路径并存） | `grep -n "thread_yield\|timer_elapsed" devices/timer.c` 应为空 |

⚠️ **判据 C 最容易自欺**：旧循环留着没删，测试照样 pass（唤醒逻辑同时也在跑），
属于"看起来做完了"。提交前必须 grep 一遍。

---

### 1.2 设计决策（4 条语义约定，写代码前定死）

| 约定 | 内容 | 违反后的症状 |
|---|---|---|
| 相对进、绝对存 | `timer_sleep(ticks)` 入参是**相对时长**；`t->wakeup_ticks` 存**绝对时刻** `timer_ticks()+ticks` | 早睡的线程提前醒；`alarm-simultaneous` 乱序 |
| 睡眠队列有序 | `sleep_list` 按 `wakeup_ticks` **升序**，`list_insert_ordered` + `wakeup_cmp` | 队头 `break` 误伤后面本该醒的线程 |
| 入队原子 | 「读 `timer_ticks()` + 入队 + `thread_block()`」必须在 `intr_disable()` 内 | 偶发睡过头，**不可稳定复现** |
| 非正值直返 | `ticks <= 0` 直接 return，不入队、不 block | `alarm-zero` / `alarm-negative` 卡死 |

**关于"原子"**：`thread_block()` 内部有 `ASSERT (intr_get_level () == INTR_OFF)`，`thread_unblock()` 同理 ——
关中断不是风格选择，是这两个函数**强制的契约**。
而 `thread_wakeup_sleepers` 由 `timer_interrupt` 调用，中断上下文本来就是关的，**不要**再 `intr_disable()`。

**为什么单开 `sleep_elem` 而不复用 `thread->elem`**：`elem` 已被 `ready_list`（将来还有信号量 `waiters`）占用，
复用会让同一个 `elem` 同时挂在两个链表里 → 链表静默损坏、症状随机。
这个「**每个用途一个独立 `elem`**」的策略要一路延续到 Task 2 的 `donations`。

---

### 1.3 改动清单（文件 → 位置 → 改动）

| 文件 | 位置 | 改动 |
|---|---|---|
| `threads/thread.h` | `struct thread` | 新增 `int64_t wakeup_ticks;`、`struct list_elem sleep_elem;` |
| `threads/thread.h` | 声明区 | 新增 `void thread_sleep (int64_t ticks);`、`void thread_wakeup_sleepers (int64_t tick);` |
| `threads/thread.c` | 文件作用域 | 新增 `static struct list sleep_list;` |
| `threads/thread.c` | `thread_init()` | 新增 `list_init (&sleep_list);` ⚠️ 漏了能编过，一跑就 Page Fault |
| `threads/thread.c` | 新增 | `static bool wakeup_cmp (...)` 按 `wakeup_ticks` 升序 |
| `threads/thread.c` | 新增 | `void thread_sleep (int64_t ticks)` |
| `threads/thread.c` | 新增 | `void thread_wakeup_sleepers (int64_t tick)` |
| `threads/thread.c` | 顶部 | 新增 `#include "devices/timer.h"` |
| `devices/timer.c` | `timer_sleep()` | **整段重写**：删 `start` 变量与 `while (timer_elapsed (start) < ticks) thread_yield ();` |
| `devices/timer.c` | `timer_interrupt()` | `ticks++` 之后、`thread_tick()` 之前插入 `thread_wakeup_sleepers (ticks);` |
| `devices/timer.c` | 顶部 | 确认有 `#include "threads/thread.h"` |

`init.c` / `synch.c` / `palloc.c` / 所有测试文件：**零改动**。

---

### 1.4 关键代码（最终形态）

**`threads/thread.h`**

```c
struct thread
  {
    /* ... 原有成员不变 ... */

    /* ===== Task 1: 闹钟 ===== */
    int64_t wakeup_ticks;          /* 绝对唤醒时刻（tick），不是相对时长 */
    struct list_elem sleep_elem;   /* sleep_list 中的节点（独立 elem！） */
  };

void thread_sleep (int64_t ticks);
void thread_wakeup_sleepers (int64_t tick);
```

---

### 1.5 编译期踩坑记录（6 条报错 → 根因 → 修法）

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 1 | 'struct thread' has no member named 'sleep_elem'; did you mean 'sleepelem'? | 头文件成员名与 .c 里 list_entry 名字不一致 | 全项目统一 | sleep_elem |
| 2 | expected expression before 'struct' + 't' undeclared + break statement not within loop | C89 不许 while (...) 后直接跟声明（声明不是语句），没加大括号 | while (...) { struct thread *t = ...; ... } |
| 3 | implicit declaration of function 'thread_sleep' | thread.h 没原型 | 补 void thread_sleep (int64_t); |
| 4 | conflicting types for 'thread_wakeup_sleepers' + too many arguments to function | 三处签名不一致：头文件 void(void) / 定义 (int64_t) / 调用传参 | 三处一起改（头文件是唯一契约） |
| 5 | undefined reference to 'thread_sleep'（链接期） | 声明有、定义无：没写 / 写成 static / 嵌套进别的函数体（GNU 扩展不报警） | 用 1.8 的 nm 定位，补非 static 定义 |
| 6 | warning: 'wakeup_cmp' defined but not used | 唯一调用者 thread_sleep（中的 list_insert_ordered）不存在 | 补 thread_sleep 后自动消失 |
两条可复用的判据：

implicit-declaration = 缺声明（编译期）；undefined reference = 缺定义（链接期）。别混。
-Wall 下的 unused-function 几乎是"写了半截"的指纹：定义了却没人调用，说明调用路径没接上。

---

### 1.6 验收与证据
① 编译

```bash
cd src/threads/build
make 2>&1 | grep -E "warning|error" \
  | grep -vE "noreturn|frame-address|nonnull-compare|format-truncation"
```
应无输出。剩下的 init.c noreturn / debug.c frame-address / string.c nonnull-compare / alarm-wait.c format-truncation 全是基线已知噪音，忽略。

⚠️ 增量编译可能被旧 .o 蒙混，结项前必做一次干净重建：make clean && make

② 全套回归（与基线 diff）

```bash
make check 2>&1 | tee /tmp/after-alarm.log
grep -E "^(pass|FAIL)" /tmp/after-alarm.log | grep pass | sort > /tmp/after-pass
```
期望：diff 零差异（仍是那 7 行）。

⚠️ 这一点必须理解透：基线里 alarm 5 条本来就在 pass（忙等待功能正确）， 所以 Task 1 的正确性不能用"新增 pass"来证明。diff=0 的意义是 "没有引入任何回归"——必要条件，不是充分条件。 另外：强/弱红线名单无需更新，baseline-pass.txt 仍是 7 行，别误以为要往里加。

③ 忙等待是否真的消失（Task 1 的真正判据）

```bash
grep -n "thread_yield"  devices/timer.c    # 应无输出
grep -n "timer_elapsed" devices/timer.c    # 应无输出
```
两条都空 → 旧的「while + yield」路径彻底移除，不是两条并存。

④ 可选实验：用 idle 占比直观证明"不再空转"
忙等待时 idle_thread 几乎拿不到 tick；改造后它应吃掉大部分 tick。

做法（临时插桩，验完 revert，不要提交）：在 thread.c 的 thread_tick() 开头加

```c
/* 临时 */
static int64_t idle_ticks, busy_ticks;
if (t == idle_thread) idle_ticks++; else busy_ticks++;
```
每隔 100 tick 或测试结束 printf 一次比例，跑 pintos -- -q run alarm-multiple 对比。 预期：改造后 idle 占比大幅上升。

若框架已自带 idle/busy 统计就直接用，别重复插桩。

---

### 1.7 语义层坑位清单（能编过但会跑错）

| # | 坑 | 症状 | 正确做法 |
|---|---|---|---|
| 1 | 存了相对值：`wakeup_ticks = ticks` | 早睡的线程提前醒 | `timer_ticks () + ticks` |
| 2 | 取时间与入队之间没关中断 | 偶发睡过头，难复现 | `intr_disable()` 包住「取时间 + 入队 + block」 |
| 3 | `list_push_back` 入队 | `alarm-simultaneous` 有线程被饿住 | `list_insert_ordered` + `wakeup_cmp` |
| 4 | 没判 `ticks <= 0` | `alarm-zero` / `alarm-negative` 卡死 | 直接 return |
| 5 | `timer_interrupt` 里先唤醒后 `ticks++` | 唤醒永远慢一 tick，超时 | `ticks++` 在前 |
| 6 | `thread_init` 漏 `list_init (&sleep_list)` | 编译链接全过，一跑 Page Fault / 死循环 | 补上（**"能编过 ≠ 能跑"** 的典型） |
| 7 | `thread_wakeup_sleepers` 里调 `intr_disable()` | 中断上下文无必要，易与断言冲突 | 中断里已是关的，直接操作 |

**判据小结**：1–5 属"逻辑对但时刻错"，靠 `alarm-*` 全套能抓；6–7 属"结构错"，
表现为 PANIC / 断言失败，`make check` 立刻炸，不会藏。

---

### 1.8 调试工具箱（跨任务通用）

| 场景 | 命令 | 判读依据 |
|---|---|---|
| 声明有、定义无（`undefined reference`） | `nm threads/thread.o \| grep -i sleep` | 要见 `T thread_sleep`、`T thread_wakeup_sleepers`（**大写 T** = 全局定义）；**小写 t** = 被写成 `static`；**无输出** = 没定义 / 嵌套在别的函数里 / 名字拼错；`U timer_ticks` 是外部引用，正常 |
| 找残留忙等待 | `grep -rn "thread_yield" devices/` | 应为空（`timer.c` 改造后不再调用 `thread_yield`） |
| 找半截实现 | `make 2>&1 \| grep -E "unused-function\|implicit"` | 应无输出 |
| 过滤已知噪音后看 warning | `make 2>&1 \| grep -E "warning\|error" \| grep -vE "noreturn\|frame-address\|nonnull-compare\|format-truncation"` | 应无输出 |
| 看失败详情 | `cat tests/threads/<name>.result` | 完整输出含 PANIC / Page Fault 回溯 |
| 单跑并看内核输出 | `pintos -v -- -q run alarm-multiple` | `-v` 实时打印串口输出 |
| 排除旧 `.o` 干扰 | `make clean && make` | 干净重建 |

**`nm` 符号字母速查**

| 字母 | 含义 | 结论 |
|---|---|---|
| `T` | 全局（外部可见）定义 | ✅ 正确 |
| `t` | 局部（`static`）定义 | ❌ 去掉 `static` |
| `U` | 未定义引用（外部依赖） | 正常，说明只是调用方 |
| 无该行 | 无此符号 | ❌ 没写 / 嵌套 / 拼错 |

> **`undefined reference` 永远是「声明有、定义无」**——先用 `nm` 看符号表，
> 比反复读代码快得多。


### 1.9 复用教训（带进 Task 2）

1. **头文件是唯一契约**：同一函数在「头文件声明 / 自身定义 / 调用点」三处签名必须一致。
   不一致时编译器只会报 `conflicting types` 或 `too many arguments`，必须回看三处才能定位。
2. **Pintos 是 C89**：声明必须放在语句块开头；`while` / `if` 后直接跟声明一定报
   `expected expression before ...`，且会带出一串 `undeclared` / `break not within loop` 的假错误。
3. **`-Wall` 的 warning 是进度条，不是噪音**：`unused-function` / `implicit-declaration`
   出现一次 = 有一处接线没接完。存量噪音只有四项（1.6 的过滤清单），其余一律当场修。
4. **`undefined reference` ≠ 编译错误**：先用 `nm <obj> | grep <symbol>` 看符号表，
   分清"缺定义"“写成 static”“嵌套函数”三种情况，比读代码快。
5. **"能编过 ≠ 能跑"**：`list_init` 漏调、`elem` 复用、断言条件写错都属于这一类。
   每完成一个小步就 `make clean && make check`，别攒到最后。
6. **不确定就 grep 自己**：`grep -rn "ready_list" threads/` 之类的一行命令，
   比"我记得好像改过"可靠得多（Task 2 会用到同样手法找 `push_back (&ready_list`）。

---

### 1.10 移交 Task 2 的接口与遗留

**新增的对外符号（Task 2 不要动它）**

| 符号 | 类型 | 归属 | 说明 |
|---|---|---|---|
| `thread_sleep` | 全局函数 | `thread.c` | `timer.c` 调用；非 static |
| `thread_wakeup_sleepers` | 全局函数 | `thread.c` | `timer_interrupt` 调用；非 static |
| `sleep_list` | static 变量 | `thread.c` | 仅内部可见，**别放头文件** |
| `wakeup_cmp` | static 函数 | `thread.c` | 仅 `list_insert_ordered` 使用 |
| `wakeup_ticks` / `sleep_elem` | 成员 | `struct thread` | 与优先级字段互不干扰 |

**遗留（明确不属于 Task 1）**

- `alarm-priority` 仍 FAIL：考的是"同一 tick 唤醒的线程按优先级抢占"，
  即 `thread_unblock` 里的 `list_push_back (&ready_list, ...)` → 归**阶段 A（Task 2 第一步）**。
- `alarm-wait` 存在但**不在 `TESTS` 里**，不参与评分，别为它调试。
- `mlfqs-*` 仍 FAIL：归**阶段 C**，与闹钟无关。

**给 Task 2 的两条结构性提醒（写在这里，动手前看一遍）**

1. **抢占逻辑不能塞进 `thread_unblock`**。它的调用者包含中断上下文
   （`timer_interrupt` → `thread_wakeup_sleepers` → `thread_unblock`），中断里不许 `schedule()`。
   抢占只能放在 `thread_create` 这类确定处于线程上下文的调用点，或使用 `intr_yield_on_return()`。
2. **`elem` 复用策略要延续**。`sleep_elem` 单开是对的；Task 2 的 `donations` 账本
   也必须各自带独立 `elem`，绝不能挂在 `thread->elem` 上（它已被 `ready_list` / `waiters` 复用）。

---

### 1.11 回滚点与坐标

```bash
cd src/threads
git add -A
git commit -m "Task1: replace busy-wait timer_sleep with blocking sleep queue"
git tag -a task1-alarm -m "Task1 done: 5/5 alarm pass, busy-wait removed, no regression"

# 改动规模核对（应为 3 files changed）
git diff baseline..task1-alarm --stat

# 可选：独立 worktree 复验（不回滚当前工作区）
git worktree add /tmp/task1-wt task1-alarm
cd /tmp/task1-wt/src/threads && make && make check 2>&1 | tail -3

```
坐标：baseline (046e4dc) → task1-alarm (<新 commit>)
期望 --stat：threads/thread.h / threads/thread.c / devices/timer.c，3 个文件
期望 make check 尾行：20 of 27 tests failed.（与基线一致，不是错误）
