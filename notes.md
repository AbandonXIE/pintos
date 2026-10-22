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

## 2. Task 2：优先级调度与优先级捐赠

### 2.0 一句话结论

把「FIFO 就绪队列 + 无主信号量」改造成「按有效优先级排序 + 锁持有者可被捐赠抬高优先级」。

- 完成日期：2026-10-21
- 起点 commit：task1-alarm
- 改动文件：`threads/thread.h` / `threads/thread.c` / `threads/synch.h` / `threads/synch.c`（4 个，无新增/删除文件）

**最终成绩：12/12 全绿**
- 捐赠 7 条：`donate-one` / `-sema` / `-nest` / `-multiple` / `-multiple2` / `-lower` / `-chain`
- 回归 5 条：`priority-fifo` / `-preempt` / `-change` / `-sema` / `-condvar`
- 附带转绿：`alarm-priority`（Task 1 的遗留，靠 2.2 的就绪队列有序 + 抢占）

与 Task 1 的根本差别：**Task 1 是「没有新增 pass 也算成功」，Task 2 是「必须新增 12 个 pass」**。
所以 Task 2 的 `make check` 是充分证据，不需要 1.6 那种额外旁证。
但同时它有个更阴的陷阱：**部分测试会假装通过**（见 2.6 坑 ③）。

---

### 2.1 可验收判据（四条，缺一不可）

| # | 判据 | 怎么查 |
|---|---|---|
| A | 7 条 donate 测试全 pass | `make check` 输出 / 单跑 `.result` |
| B | 5 条 priority 回归测试全 pass，且 `alarm-*` 5 条不退步 | 同上 |
| C | 编译零 error，无 `unused-function` / `implicit-declaration` / `no previous prototype` | 1.6 的过滤命令 |
| D | **互斥没被破坏**：`lock_release` 的 `ASSERT (lock->holder == thread_current ())` 从未触发 | 见 2.6 坑 ③，这是唯一能抓它的手段 |

⚠️ **判据 D 是最容易漏的一条**。`lock_acquire` 写错时，`donate-one` 等 6 个测试照样 PASS
（它们只 `msg` 打印优先级数值，从不检查同一把锁是否被两个线程同时持有），
只有 `donate-chain` 会走到 `lock_release` 断言并 PANIC。
**"6/7 绿"在这种情况下完全不代表你只差一点**，而是代表互斥已经坏了。

---

### 2.2 设计决策（6 条语义约定，写代码前定死）

| 约定 | 内容 | 违反后的症状 |
|---|---|---|
| 基准/有效分离 | `base_priority` 只由 `thread_set_priority` 写；`priority` 是**有效优先级**，对外由 `thread_get_priority` 返回 | `donate-lower` FAIL；`get_priority` 返回 base 时 donate 全红 |
| 捐赠取 max 不累加 | `effective = max(base, 各锁的 max_priority)` | 优先级虚高，`donate-multiple` 乱序 |
| 锁上记账 | 每把锁带 `max_priority`（该锁等待者中的最高优先级）+ `in_donations`（是否已挂进持有者的捐赠链表） | 见 2.6 坑 ②，链表被拆坏 |
| 无条件 `sema_down` | **锁空闲时也要消费信号量令牌** | 见 2.6 坑 ③，互斥彻底失效 |
| 释放顺序固定 | 摘捐赠 → 清 `max_priority` → **清 `holder`** → 重算优先级 → `sema_up` | `holder` 清晚了，新持有者看到旧 holder |
| 级联只向上升 | `thread_donate_priority` 只在 `holder->priority > old` 时继续上溯 | 深链上优先级来回横跳 → 无限递归 |

**关于"每个用途一个独立 elem"**：Task 1 已为 `sleep_elem` 立过规矩，这里继续执行——
`struct lock` 自带 `struct list_elem elem` 用于挂进 `holder->donations`，
**绝不能复用 `thread->elem`**（它已被 `ready_list` / `sema->waiters` 复用）。
`thread->elem` 的双重身份（ready / sema waiters）之所以安全，是因为"就绪"和"阻塞"互斥；
而 `donations` 与它们是**同时存在**的，必须独立。

---

### 2.3 改动清单（文件 → 位置 → 改动）

| 文件 | 位置 | 改动 |
|---|---|---|
| `threads/thread.h` | `struct thread` | `int priority`（语义改为**有效**优先级）、`int base_priority`、`struct lock *waiting_lock`、`struct list donations` |
| `threads/thread.h` | 声明区 | `priority_more`、`thread_update_priority`、`thread_donate_priority`（**只放原型，每行带分号**） |
| `threads/thread.h` | 顶部 | `struct lock;` 前向声明（原型里用到 `struct lock *`，避免 prototype-scope 警告） |
| `threads/thread.c` | `thread_init()` | 无（`donations` 在 `init_thread` 初始化，`sleep_list` 已在 Task 1 加好） |
| `threads/thread.c` | `init_thread()` | ⚠️ `base_priority = priority`、`waiting_lock = NULL`、**`list_init (&t->donations)`** |
| `threads/thread.c` | `thread_create()` | 新线程优先级高于当前线程时 `thread_yield ()`（唯一的合法抢占点，见 2.6 说明） |
| `threads/thread.c` | `thread_unblock()` | `list_push_back` → `list_insert_ordered (&ready_list, ..., priority_more, NULL)` |
| `threads/thread.c` | `thread_yield()` | 同上，按优先级归位 |
| `threads/thread.c` | `thread_set_priority()` | 只写 `base_priority`，随后 `thread_update_priority (cur)`，再按需 yield |
| `threads/thread.c` | `thread_get_priority()` | 返回 `priority`（有效），**不是** `base_priority` |
| `threads/thread.c` | 新增 | `priority_more`、`thread_resort_ready`(static)、`thread_update_priority`、`thread_donate_priority` |
| `threads/synch.h` | `struct lock` | 新增 `int max_priority;` `bool in_donations;` `struct list_elem elem;` |
| `threads/synch.c` | `sema_down()` | 入队改 `list_insert_ordered (&sema->waiters, ..., priority_more, NULL)` |
| `threads/synch.c` | `sema_up()` | 取 `list_pop_front` 的最高优先级者，`intr_context()` 时用 `intr_yield_on_return()`，否则 `thread_yield()` |
| `threads/synch.c` | `lock_init()` | ⚠️ 新增 `max_priority = 0; in_donations = false;` |
| `threads/synch.c` | `lock_acquire()` | 捐赠分支 + **无条件** `sema_down`（见 2.6 坑 ③） |
| `threads/synch.c` | `lock_release()` | 摘捐赠 → 清 max → 清 holder → 重算 → `sema_up` |
| `threads/synch.c` | `struct semaphore_elem` | 新增 `int priority;` + `sema_elem_more` 比较器（condvar 排序用） |

`devices/` / `lib/` / `tests/` / `init.c` / `palloc.c`：**零改动**。

> **不要**把抢占塞进 `thread_unblock()`：它的调用链包含中断上下文
> （`timer_interrupt → thread_wakeup_sleepers → thread_unblock`），中断里不许 `schedule()`。

---

### 2.4 关键代码（最终形态）

**`threads/thread.c`**

```c
/* 高优先级排前面 */
bool
priority_more (const struct list_elem *a, const struct list_elem *b,
               void *aux UNUSED)
{
  return list_entry (a, struct thread, elem)->priority
       > list_entry (b, struct thread, elem)->priority;
}

static void
thread_resort_ready (struct thread *t)
{
  if (t->status == THREAD_READY)
    {
      list_remove (&t->elem);
      list_insert_ordered (&ready_list, &t->elem, priority_more, NULL);
    }
}

/* 有效优先级 = max(基准, 各捐赠锁的 max_priority) */
void
thread_update_priority (struct thread *t)
{
  int max = t->base_priority;
  struct list_elem *e;

  for (e = list_begin (&t->donations); e != list_end (&t->donations);
       e = list_next (e))
    {
      struct lock *l = list_entry (e, struct lock, elem);
      if (l->max_priority > max)
        max = l->max_priority;
    }

  if (max == t->priority)
    return;

  t->priority = max;

  if (t->status == THREAD_READY)
    thread_resort_ready (t);
}

/* 捐赠 + 沿 waiting_lock 链向上级联（只向上升） */
void
thread_donate_priority (struct thread *holder, struct lock *lock)
{
  if (holder == NULL)
    return;

  if (!lock->in_donations)
    {
      list_push_back (&holder->donations, &lock->elem);
      lock->in_donations = true;
    }

  int old = holder->priority;
  thread_update_priority (holder);

  if (holder->priority > old && holder->waiting_lock != NULL)
    {
      struct lock *up = holder->waiting_lock;
      if (up->max_priority < holder->priority)
        up->max_priority = holder->priority;
      thread_donate_priority (up->holder, up);
    }
}
```
**`threads/synch.c`**

```c
Copy
void
lock_init (struct lock *lock)
{
  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);

  lock->max_priority = 0;        /* ⚠️ 必须 */
  lock->in_donations = false;    /* ⚠️ 必须 */
}

void
lock_acquire (struct lock *lock)
{
  struct thread *cur = thread_current ();

  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  if (lock->holder != NULL)          /* 只有被占着才谈捐赠 */
    {
      cur->waiting_lock = lock;
      if (lock->max_priority < cur->priority)
        lock->max_priority = cur->priority;
      thread_donate_priority (lock->holder, lock);
    }

  sema_down (&lock->semaphore);      /* ⚠️ 必须在 if 外面，见 2.6 坑 ③ */
  cur->waiting_lock = NULL;

  lock->holder = cur;
}

void
lock_release (struct lock *lock)
{
  ASSERT (lock->holder == thread_current ());

  struct thread *cur = thread_current ();

  if (lock->in_donations)
    {
      list_remove (&lock->elem);
      lock->in_donations = false;
    }
  lock->max_priority = 0;

  lock->holder = NULL;               /* ⚠️ 必须早于 sema_up */
  thread_update_priority (cur);

  sema_up (&lock->semaphore);
}
```
关于 thread_reinherit（不做，记录原因）：曾尝试在 lock_release 里加"优先级下降沿链重播"， 结果 donate-chain 死循环。原因是它会沿 waiting_lock 反复上溯，而链上存在 "已被 sema_up 唤醒（status=READY）但 waiting_lock 尚未清空"的窗口，状态假设不成立。 Pintos 这 12 个测试都不需要它——撤销时靠 thread_update_priority(cur) 重算 （取剩余 donations 的 max）已经足够。这也顺带说明：不要给 thread_update_priority 加 THREAD_BLOCKED 分支去重排 sema->waiters，那会对"不属于该链表的 elem"做 list_remove，是链表成环的直接来源。

### 2.5 编译/链接期踩坑记录（4 条）

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 1 | `thread.h:148: old-style parameter declarations in prototyped function definition`，随后 **300+ 条** `storage class specified for parameter 'va_list' / 'test_func' / 'intr_handler_func'` | 把**函数体**贴进了 `thread.h`，或原型**漏分号**（`void f (struct thread *t)` 无 `;` 被当成函数定义开头） | 头文件只留原型、每行带 `;`；`static` 内部函数（`thread_resort_ready`）**根本不该出现在头文件** |
| 2 | `'struct lock' declared inside parameter list will not be visible...` / 类型冲突 | `thread.h` 没包含 `synch.h`，原型里的 `struct lock *` 成了 prototype 作用域的新类型 | 顶部加 `struct lock;` 前向声明（**不要** `#include`，会循环包含） |
| 3 | `no previous prototype for 'thread_update_priority'` | 有定义、头文件漏声明 | 补声明；Pintos 开了 `-Wmissing-prototypes` |
| 4 | `implicit declaration of function 'thread_reinherit'` | 调用点有、声明无，或声明写到了 `#endif` 之后 | 声明放在 `thread_get_priority` 之后、`#endif` 之前 |

**判据**：`implicit declaration` = 缺声明（编译期）；`undefined reference` = 缺定义（链接期）；
`no previous prototype` = 声明缺失/位置错。**`thread.h` 一崩，所有 `.c` 连带报错，先修头文件。**

---

### 2.6 运行时三大杀手（本任务的核心教训）

#### 坑 ① `init_thread` 漏 `list_init (&t->donations)`

| 项 | 内容 |
|---|---|
| 真相 | `memset (t, 0, sizeof *t)` 后 `head.next = head.prev = NULL`，这**不是空链表而是坏链表**（空链表 head.next 应指回 `&head`） |
| 首爆点 | `list_push_back (&holder->donations, &lock->elem)` 内 `before->prev->next = elem` → 对 `NULL->next` 解引用 |
| 症状 | 极低地址 Page Fault，或一路乱走烧满 CPU（`TIMEOUT after 60 seconds of host CPU time`） |
| 关键 | 必须加在 **`init_thread`**，不是 `thread_create`——`initial_thread`（测试主线程）与 `idle_thread` 都只走 `init_thread` |

#### 坑 ② `lock_init` 漏初始化 `max_priority` / `in_donations`

| 项 | 内容 |
|---|---|
| 现象 | 所有 donate 测试 `TIMEOUT`，**一句测试输出都没有**，boot 消息完全正常 |
| 真相 | 测试里的锁是**栈上局部变量** `struct lock lock;` → 内容是上一个栈帧的垃圾 → `in_donations` 随机。若非 0：捐赠时跳过 `list_push_back`（锁没进 donations），释放时却对**从未接入链表的野 elem** 执行 `elem->prev->next = elem->next` → 随机地址被写坏 → 命中 `ready_list`/`all_list`/`sema->waiters` → 链表成环 → `list_next` 走不到 `list_end` |
| 为何 boot 不炸 | `tid_lock` 是**全局变量**，BSS 零初始化，`in_donations` 恰为 0；只有栈上的测试锁是垃圾 |
| 修法 | `lock_init` 补 `max_priority = 0; in_donations = false;` |

> **通用判据**：带 "host CPU time" 的 `TIMEOUT` 几乎总是"链表成环 / 无限递归"，
> 而不是"等不到唤醒"——纯阻塞会进 `idle` 的 `hlt`，不烧 CPU。

#### 坑 ③ `sema_down` 被放进 `if (lock->holder != NULL)` ← 最隐蔽

| 项 | 内容 |
|---|---|
| 错误写法 | `if (holder != NULL) { ...捐赠...; sema_down(); }` 然后 `lock->holder = cur;` |
| 后果 | `sema_down` 原本干两件事：**阻塞睡眠** + **把 value 从 1 减到 0（消费"锁可用"令牌）**。放进 `if` 后，**锁空闲时没人消费令牌**，`value` 恒为 1 → 后来者 `sema_down` 见 `value == 1` 直接返回 → **两个线程同时"持有"同一把锁** |
| 症状 | 前 6 个 donate 测试**全绿**（只 `msg` 打印优先级，不检查互斥）；`donate-chain` 炸在 `lock_release: assertion 'lock->holder == thread_current ()' failed` → 连环 `#PF` |
| 铁证 | `chain` 的 `.result` 中 `thread 1 got lock` 出现在 main 还持有 `locks[0]` 之时 |
| 修法 | `sema_down` **无条件调用**，放到 `if` 之外 |

> **"6/7 绿"不代表只差一点**。是否踩此坑只看一件事：
> `lock_release` 的 ASSERT 有没有触发过（见 2.1-D）。

---

### 2.7 验收与证据

#### ① 编译（本仓库不是 `make` 一步到位，按 CI 方式建 build）

```bash
cd /root/pintos/src/threads
mkdir -p build/devices build/lib/kernel build/lib/user build/tests/threads build/threads
cp ../Makefile.build build/Makefile
cd build && make 2>&1 | grep -E "warning|error" \
  | grep -vE "noreturn|frame-address|nonnull-compare|format-truncation"
ls -l kernel.bin
```

应无输出（4 项噪音同 1.6），kernel.bin 必须存在。 ⚠️ 改过 thread.h / synch.h 后做一次 rm -rf build 干净重建，防旧 .o 蒙混。

#### ② 12 条测试（用 .result 汇总，别用 make check 全跑）

```bash
export PATH=$PATH:/root/pintos/src/utils
cd /root/pintos/src/threads/build
for t in one sema nest multiple multiple2 lower chain; do
  printf "%-10s " "$t"; timeout 40 pintos --qemu -- -q run priority-donate-$t 2>&1 | tail -1
done
for t in fifo preempt change sema condvar; do
  printf "%-10s " "$t"; timeout 40 pintos --qemu -- -q run priority-$t 2>&1 | tail -1
done
```
期望 12 行全是 Powering off...（内核正常 shutdown_power_off）。

| 结果 | 含义 |
| --- | --- |
| Powering off... | PASS |
| Terminated / 无输出 | 挂死，看 .result 与 2.6 |
| PANIC / assertion | 互斥或链表被破坏，优先查 2.6 坑 ③ |
| Expected priority N, got M | get_priority 返回了 base，或捐赠没传到 |

#### ③ 失败时唯一可靠的取证手段：.result 文件

```bash
cd /root/pintos/src/threads/build
rm -f tests/threads/priority-donate-chain.result
make tests/threads/priority-donate-chain.result
head -40 tests/threads/priority-donate-chain.result
```
⚠️ 本容器里 pintos --qemu -- run xxx | tail 的普通模式输出不可靠： qemu 被 timeout 杀掉时串口缓冲会丢，常只剩 Terminated，连 boot 后的 msg 都看不到。 一律以 .result 为准。

#### ④ 与基线的差异（Task 2 的正向证据）
```bash
make check 2>&1 | tee /tmp/after-prio.log
grep -E "^(pass|FAIL)" /tmp/after-prio.log | grep pass | sort > /tmp/after-pass
diff /root/baseline-pass.txt /tmp/after-pass   # 应只剩 mlfqs-* 与 userprog 相关差异
```
Task 2 与 Task 1 不同：这里 diff 必须变，新增 12 条 pass（含 alarm-priority）。

### 2.8 语义层坑位清单（能编过但会跑错）

| # | 坑 | 症状 | 正确做法 |
| --- | --- | --- | --- |
| 1 | thread_get_priority 返回 base_priority | donate-* 全红，报 Expected priority 33, got 31 | 返回 priority |
| 2 | lock->max_priority 释放时不清零 | 这把锁下一轮带着旧优先级，donate-multiple 乱序 | lock_release 里 max_priority = 0 |
| 3 | lock->holder 在 sema_up 之后才清 | 新持有者看到旧 holder，ASSERT 或二次捐赠 | 先 holder = NULL，再 sema_up |
| 4 | thread_set_priority 直接写 priority | 覆盖捐赠值，donate-lower | FAIL	只写 base_priority，再 update_priority |
| 5 | 捐赠做加法 | 优先级虚高 | 取 max |
| 6 | 级联条件用 !=	深链上来回横跳，donate-chain 挂死	用 >（只向上升） |
| 7 | 在 thread_update_priority 里加 THREAD_BLOCKED 分支重排 sema->waiters | 对不属于该链表的 elem 做 list_remove → 链表成环 | 不做；12 条测试都不需要 |
| 8 | lock_acquire 里加 thread_yield() | 制造"waiting_lock | 已设但状态 READY"的畸形窗口	删除；sema_down 自会让出 |
| 9 | donations / sleep_elem 复用 thread->elem | 同一 elem 挂两个链表，静默损坏 | 每个用途独立 elem |
判据小结：1、4、5 属"数值错"，donate-* 的 msg 能直接抓； 2、3、6 属"时序错"，表现为乱序或挂死；7、8、9 属"结构性错"，表现为 PANIC / 死循环。

### 2.9 调试工具箱（在 1.8 基础上新增 4 条）

| 场景 | 命令 | 判读 |
| --- | --- | --- |
| 看真实内核输出（首选） | cat tests/threads/<name>.result | head -40	含全部 msg + PANIC 回溯；比管道可靠 |
| 定位"卡在第几条断言" | 同上，看最后一条 msg | chain 是 msg 顺序最规整的测试，一眼定阶段 |
| 二分链深（不改自己的代码） | sed -i 's/#define NESTING_DEPTH 8/#define NESTING_DEPTH 4/' tests/threads/priority-donate-chain.c | 2/3/4 绿而 8 红 → 深度相关；2 就红 → 双锁持有模式问题。测完改回 8 |
| 确认符号是否真的导出 | nm build/threads/thread.o grep -i donate | 大写 T = 全局定义（对）；小写 t = 被写成 static（错）；无输出 = 没定义/拼错 |
| 查残留的 FIFO 入队 | grep -rn "push_back (&ready_list" threads/ | 应为空（全部换成 list_insert_ordered） |
| 确认新字段初始化 | grep -n "list_init (&t->donations)" threads/thread.c、grep -n "in_donations = false" threads/synch.c | 两条都必须有输出（2.6 坑 ①②） |
信号速查：signal 2 = 你的 timeout 主动杀；signal 15 = timeout 到期 SIGTERM； Powering off... = 内核正常关机（成功）；#PF 连环刷 = 已 PANIC 但没关机，堆栈已被破坏。 |

### 2.10 复用教训（带进 Task 3 / 今后）

同步原语有隐含契约：sema_down 不只是"睡到有资源"，它同时消费资源。 改写时必须问："调用点被挪走/条件化之后，这段副作用还在不在？"（坑 ③ 的根源）
"初始化"必须覆盖到所有字段：新增成员后，去 init_thread / lock_init / sema_init 逐个 grep 一遍"谁给这个字段赋初值"。memset 不等于 list_init（坑 ①②）。
栈变量 vs 全局变量的差异会伪装成"偶发 bug"：BSS 零初始化掩盖了漏初始化的问题， 只有测试里的栈对象才暴露。遇到"开机正常、一跑测试就死"，先查这条。
通过数不是唯一判据：互斥破坏时 6/7 仍绿。凡是引入 ASSERT 的路径 （本例 lock_release），都要把"断言是否触发"当作独立判据。
头文件是唯一契约，且只放声明：一崩就是全项目 300+ 连带错误； static 函数不进头文件；跨头文件的 struct X * 一律前向声明。
测试是证据来源，不是调试仪式：.result 里测试自己的 msg 顺序 比任何 printf 插桩都权威（chain 的 thread 1 got lock 早于 main 释放，直接指向坑 ③）。
可疑的"加固"要先证伪再加：thread_reinherit 是逻辑上正确的加固， 但在本测试集里制造了死循环。先让 12 条测试全绿，再谈语义完备性。

### 2.11 移交 Task 3 的接口与遗留

| 符号 | 类型 | 归属 | Task 3 里怎么处理 |
|---|---|---|---|
| `priority_more` | 全局函数 | thread.c | 不动；`ready_list` 与 `sema->waiters` 共用 |
| `thread_update_priority` | 全局函数 | thread.c | 开头加 `if (thread_mlfqs) return;`，写者让给 4 tick 重算 |
| `thread_donate_priority` | 全局函数 | thread.c | 仅在 `!thread_mlfqs` 下可达，逻辑不动 |
| `base_priority` / `priority` | 成员 | `struct thread` | `priority` 改由 nice/recent_cpu 算出，`base_priority` 弃用 |
| `waiting_lock` / `donations` | 成员 | `struct thread` | 保留，MLFQS 下不参与调度 |
| `max_priority` / `in_donations` / `elem` | 成员 | `struct lock` | 同上 |

**给 Task 3 的三条结构性提醒**

1. **MLFQS 下整体关捐赠**：`if (!thread_mlfqs)` 要覆盖 `lock_acquire` 的捐赠分支、`lock_release` 的摘除与重算、`thread_set_priority`（原版直接 `ASSERT (!thread_mlfqs)`）、`thread_update_priority`（early-return）。⚠️ **只关捐赠，不关排序**——`ready_list` 与 `sema->waiters` 的有序插入在 MLFQS 下照样要用。
2. **换写者而非重写**：`priority` 的唯一写者从 `thread_update_priority`（= max(base, 捐赠)）换成 4 tick 的 MLFQS 重算（`PRI_MAX - recent_cpu/4 - nice*2`，clamp [0, 63]）。两个写者并存表现为数值抖动，**编译零 warning**，只能靠 `mlfqs-*` 的逐秒输出抓。
3. **定点数 + 批量重排是两件独立的事**：`1<<14` 表 1.0、先乘后除、中间量 int64、不用 double（溢出静默不报错）；重算后 `ready_list` 已无序，遍历 `all_list` 只改数值、重排对 `ready_list` 做整体重建——`running` 不在 `ready_list`，对它 `list_remove` 就是 2.8 坑 #7 的成环。

**判据**：开工前先回归 Task 2 那 12 条（捐赠 7 + 回归 5）保持全绿；改完看 `mlfqs-recent-1` / `mlfqs-load-avg` 的数值序列，不看 `Powering off...`。

> **一句话交接**：Task 2 交出「`priority` 单一写者 + 三个用途三个独立 `elem` + 不依赖中断抢占的唤醒链」，Task 3 不重写，只换写者、关捐赠、补定点数。

## 3. Task 3：多级反馈队列调度器（MLFQS）

### 3.0 一句话结论

把「每个线程自管 `priority` + 捐赠」改成「调度器每 4 tick 用公式 `PRI_MAX - recent_cpu/4 - nice*2` 统一重算优先级」，即 4.4BSD 高级调度器。

- **完成日期**：2026-10-22
- **起点 commit**：`task2-priority`
- **改动文件**：`threads/thread.h` / `threads/thread.c` / `threads/synch.c` / **新增** `threads/fixed-point.h`
  （改 3、增 1，无 `Makefile` 改动）

**最终成绩：27/27 全绿**

| 组 | 条数 | 结果 |
|---|---|---|
| MLFQS `mlfqs-*` | 9 | 全绿 |
| 回归 alarm（Task1） | 6 | 不退步 |
| 回归 priority（Task2） | 12 | 不退步 |

- MLFQS 9：`load-1` / `load-60` / `load-avg` / `recent-1` / `fair-2` / `fair-20` / `nice-2` / `nice-10` / `block`

与 Task 2 的关键差别：**Task 3 的��换写者」，不是「重写」**。
`priority` 字段还在、还被 `ready_list` / `sema->waiters` 用来排序，只是它的**唯一写者**从「`max(base, 捐赠)`」换成了「4 tick 公式」。
所以顺序必须是：**先让 MLFQS 与捐赠共存（只关捐赠、不关排序/唤醒/抢占），再换写者**。顺序搞反——一上来把排序或抢占也 `!thread_mlfqs` 关掉——会连 `mlfqs-block` 一起炸（见 3.6 坑②）。

---

### 3.1 可验收判据（五条，缺一不可）

| # | 判据 | 怎么查 |
|---|---|---|
| A | 9 条 `mlfqs-*` 全 pass | `make check` / 单跑 `.result` |
| B | alarm 6 + priority 12 全部不退步，总 27/27 | 同上 |
| C | 编译零 error，无 `multiple definition` / `unused-function` / `implicit-declaration` | 3.7 ① |
| D | **数值真的对**：`mlfqs-recent-1` / `mlfqs-load-avg` 逐秒序列匹配，不是只看 `Powering off...` | 3.7 ④ |
| E | **定点数正确**：`1.5×1.5=2.25`、`1.0/3.0≈0.333`、负数取整方向对、`100×100` 不溢出 | 3.9 |

> ⚠️ **判据 D 是 Task 3 最阴的一条**：全部公式写对、`Powering off...` 也会出现，但只要 `recent_cpu` 少衰减一次、`×100` 漏取整一个，**全程不 PANIC、不挂、就悄悄 FAIL**。唯一靠得住的是看 `mlfqs-recent-1` 每秒打印的数值序列。

---

### 3.2 设计决策（8 条语义约定，写代码前定死）

| # | 约定 | 内容 | 违反后的症状 |
|---|---|---|---|
| 1 | 定点数 16.14 | `f = 1<<14 = 16384`；**先乘后除**；中间量 `int64_t`；**绝不用浮点** | 溢出/精度静默丢失，不报错 |
| 2 | 三档更新节奏 | 每 tick（`recent_cpu += 1`）、每 4 tick（优先级）、每秒（`load_avg` + `recent_cpu` 衰减） | 数值爆炸或几乎不更新 |
| 3 | 更新顺序 | `+1` → 每秒（先 `load_avg` 后 `recent_cpu`）→ 每 4 tick（重算优先级） | 优先级用到旧 `recent_cpu` |
| 4 | idle 排除 | `recent_cpu += 1` 跳过 idle；`ready_threads` 不含 idle | `load_avg` / `recent_cpu` 偏高 |
| 5 | getter 返回 ×100 | `thread_get_recent_cpu` / `thread_get_load_avg` 返回 **100×、就近取整** | `recent_1` / `load_*` 全红 |
| 6 | `-mlfqs` 接线 | `thread_create` 忽略 `priority`、`thread_set_priority` 直接 return、`thread_get_priority` 返回 `priority` | 手工优先级与公式互抢 |
| 7 | 捐赠隔离 | `!thread_mlfqs` **只包捐赠**；排序/唤醒/抢占两模式都要 | 关多了 → 锁/唤醒在 mlfqs 下全废 |
| 8 | 单一写者 | mlfqs 下 `priority` 只由 `mlfqs_priority` 写，`thread_update_priority` 开头 early-return | 两写者并存 → 数值抖动、无 warning |

> **约定 7（本任务最大雷区）**：`list` / `sema->waiters` / `ready_list` 排序、`sema_up` 的「唤醒 + 抢占」都是**两模式都要**。只有「优先级捐赠」那套才 `!thread_mlfqs` 关掉。
> 一句话：**`if (!thread_mlfqs)` 永远不要包住 `thread_unblock` / `sema->value++` / `sema_down` / `lock->holder=` / `list_sort`。**

---

### 3.3 改动清单（文件 → 位置 → 改动）

| 文件 | 位置 | 改动 |
|---|---|---|
| `threads/fixed-point.h` | 新增 | 16.14 定点库：转换/加减乘除全套，全 `static inline`（不改 Makefile） |
| `threads/thread.h` | `struct thread` | 新增 `int nice;` `int recent_cpu;`（定点） |
| `threads/thread.h` | 声明区 | 新增 `thread_set_nice` / `thread_get_nice` / `thread_get_recent_cpu` / `thread_get_load_avg` 原型 |
| `threads/thread.c` | 顶部 | `#include "fixed-point.h"`、`#include "devices/timer.h"` |
| `threads/thread.c` | 文件作用域 | `static int load_avg = 0;`；`mlfqs_priority` 前向声明 |
| `threads/thread.c` | `init_thread()` | `t->nice = 0; t->recent_cpu = 0;` |
| `threads/thread.c` | 新增 `mlfqs_priority` | 公式 + trunc + clamp [0,63] |
| `threads/thread.c` | `thread_tick()` | 三档更新（`+1`／每秒／4 tick） |
| `threads/thread.c` | `thread_set_nice()` | 存 nice，mlfqs 下立即重算+重排+按需 yield |
| `threads/thread.c` | `thread_get_*` 4 个 | nice 直返；recent_cpu / load_avg 返回 ×100 就近取整 |
| `threads/thread.c` | `thread_create()` | mlfqs 下公式定初值；末尾抢占加 `!thread_mlfqs` |
| `threads/thread.c` | `thread_set_priority()` | 开头 `if (thread_mlfqs) return;` |
| `threads/thread.c` | `thread_update_priority()` | 开头 `if (thread_mlfqs) return;` |
| `threads/synch.c` | `sema_up()` | `unblock`+`value++` 两模式都跑；`need_yield` **两模式都判**；唤醒前 `list_sort` 重排 waiters |
| `threads/synch.c` | `lock_acquire()` | 捐赠块 `!thread_mlfqs`；`sema_down` 与 `holder=cur` **无条件** |
| `threads/synch.c` | `lock_release()` | 摘除/重算 `!thread_mlfqs`；`holder=NULL` 与 `sema_up` **无条件** |

> `devices/` / `lib/` / `tests/` / `init.c` / `palloc.c`：**零改动**。

---

### 3.4 关键代码（最终形态）

#### `threads/fixed-point.h`

```c
#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H
#include <stdint.h>

#define FP_SHIFT 14
#define FP_F     (1 << FP_SHIFT)        /* f = 16384 */

typedef int fp_t;

static inline fp_t int_to_fixed (int n)        { return n * FP_F; }
static inline int  fixed_to_int_trunc (fp_t x) { return x / FP_F; }
static inline int  fixed_to_int_round (fp_t x) {
  return (x >= 0) ? (x + FP_F / 2) / FP_F : (x - FP_F / 2) / FP_F;
}
static inline fp_t fixed_add (fp_t a, fp_t b)      { return a + b; }
static inline fp_t fixed_sub (fp_t a, fp_t b)      { return a - b; }
static inline fp_t fixed_add_int (fp_t x, int n)   { return x + n * FP_F; }
static inline fp_t fixed_sub_int (fp_t x, int n)   { return x - n * FP_F; }
static inline fp_t fixed_mul (fp_t a, fp_t b)      { return (fp_t)((int64_t)a * b / FP_F); }
static inline fp_t fixed_mul_int (fp_t x, int n)   { return x * n; }
static inline fp_t fixed_div (fp_t a, fp_t b)      { return (fp_t)((int64_t)a * FP_F / b); }
static inline fp_t fixed_div_int (fp_t x, int n)   { return x / n; }

#endif
```
优先级计算 + 三档更新
c
Copy
static int
mlfqs_priority (int recent_cpu, int nice)
{
  int p = PRI_MAX
        - fixed_to_int_trunc (fixed_div_int (recent_cpu, 4))
        - nice * 2;
  if (p < PRI_MIN) p = PRI_MIN;
  if (p > PRI_MAX) p = PRI_MAX;
  return p;
}

void
thread_tick (void)
{
  struct thread *t = thread_current ();
  /* ... 原有统计 / TIME_SLICE 抢占 ... */

  if (thread_mlfqs)
    {
      /* ① 每 tick：运行线程 recent_cpu += 1（非 idle） */
      if (t != idle_thread)
        t->recent_cpu = fixed_add_int (t->recent_cpu, 1);

      /* ② 每秒：先 load_avg，再衰减所有线程 recent_cpu */
      if (timer_ticks () % TIMER_FREQ == 0)
        {
          struct list_elem *e;
          int ready = list_size (&ready_list) + (t != idle_thread ? 1 : 0);

          load_avg = fixed_div_int (
                       fixed_add (fixed_mul_int (load_avg, 59),
                                  int_to_fixed (ready)), 60);

          for (e = list_begin (&all_list); e != list_end (&all_list);
               e = list_next (e))
            {
              struct thread *th = list_entry (e, struct thread, allelem);
              int coef = fixed_div (fixed_mul_int (load_avg, 2),
                                    fixed_add_int (fixed_mul_int (load_avg, 2), 1));
              th->recent_cpu = fixed_add_int (
                                 fixed_mul (coef, th->recent_cpu), th->nice);
            }
        }

      /* ③ 每 4 tick：重算优先级 + 重排 + 抢占 */
      if (timer_ticks () % 4 == 0)
        {
          struct list_elem *e;
          for (e = list_begin (&all_list); e != list_end (&all_list);
               e = list_next (e))
            {
              struct thread *th = list_entry (e, struct thread, allelem);
              th->priority = mlfqs_priority (th->recent_cpu, th->nice);
            }
          list_sort (&ready_list, priority_more, NULL);
          if (!list_empty (&ready_list)
              && list_entry (list_front (&ready_list), struct thread, elem)->priority
                 > t->priority)
            intr_yield_on_return ();
        }
    }
}
getter（×100 就近取整）+ set_nice
```c
int thread_get_nice (void)        { return thread_current ()->nice; }

int thread_get_recent_cpu (void) {
  return fixed_to_int_round (
           fixed_mul_int (thread_current ()->recent_cpu, 100));
}

int thread_get_load_avg (void) {
  return fixed_to_int_round (fixed_mul_int (load_avg, 100));
}

void
thread_set_nice (int nice)
{
  struct thread *t = thread_current ();
  t->nice = nice;
  if (thread_mlfqs)
    {
      t->priority = mlfqs_priority (t->recent_cpu, t->nice);
      if (!list_empty (&ready_list)
          && list_entry (list_front (&ready_list), struct thread, elem)->priority
             > t->priority)
        thread_yield ();
    }
}
```
-mlfqs 接线
```c
/* thread_create：忽略 priority 参数 */
if (thread_mlfqs)
  t->priority = mlfqs_priority (t->recent_cpu, t->nice);
...
thread_unblock (t);
if (!thread_mlfqs && t->priority > thread_current ()->priority)
  thread_yield ();

/* thread_set_priority / thread_update_priority：开头即返回 */
if (thread_mlfqs) return;

/* thread_get_priority：返回有效优先级（两模式通用） */
return thread_current ()->priority;
```
synch.c：只关捐赠，不关唤醒/排序/抢占
```c
void
sema_up (struct semaphore *sema)
{
  enum intr_level old_level;
  bool need_yield = false;

  ASSERT (sema != NULL);
  old_level = intr_disable ();
  if (!list_empty (&sema->waiters))
    {
      list_sort (&sema->waiters, priority_more, NULL);      /* 唤醒前重排 */
      struct thread *t = list_entry (list_pop_front (&sema->waiters),
                                     struct thread, elem);
      thread_unblock (t);
      if (t->priority > thread_current ()->priority)        /* 两模式都判 */
        need_yield = true;
    }
  sema->value++;
  intr_set_level (old_level);

  if (need_yield)
    {
      if (intr_context ())
        intr_yield_on_return ();
      else
        thread_yield ();
    }
}

void
lock_acquire (struct lock *lock)
{
  struct thread *cur = thread_current ();
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  if (!thread_mlfqs && lock->holder != NULL)                /* 只关捐赠 */
    {
      cur->waiting_lock = lock;
      if (lock->max_priority < cur->priority)
        lock->max_priority = cur->priority;
      thread_donate_priority (lock->holder, lock);
    }

  sema_down (&lock->semaphore);                            /* 无条件 */
  if (!thread_mlfqs)
    cur->waiting_lock = NULL;
  lock->holder = cur;                                      /* 无条件 */
}

void
lock_release (struct lock *lock)
{
  struct thread *cur = thread_current ();
  ASSERT (lock->holder == thread_current ());

  if (!thread_mlfqs)                                       /* 只关摘除/重算 */
    {
      if (lock->in_donations)
        { list_remove (&lock->elem); lock->in_donations = false; }
      lock->max_priority = 0;
    }
  lock->holder = NULL;                                     /* 无条件 */
  if (!thread_mlfqs)
    thread_update_priority (cur);
  sema_up (&lock->semaphore);                              /* 无条件 */
}
```
### 3.5 编译/链接期踩坑记录（7 条）

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 1 | `multiple definition of 'int_to_fixed' …`（链接期） | `fixed-point.h` 里的函数**没写 `static inline`**，每个 include 它的 `.c` 各生成一份外部符号 | 全部加 `static inline`（内部链接、各自一份、可内联） |
| 2 | 值全是垃圾 / 运行乱走 | 函数**漏 `return`**（`fp_t f (int n){ n*FP_F; }`） | 每个非 void 函数都要 `return` |
| 3 | `expected ':', ',', ';' … before 'int'` + 300+ 条 `'struct thread' has no member named ...` | `struct thread` 里新增成员 `int recent_cpu` **漏分号** → 整个结构体解析失败，所有成员"消失" | 补 `;`。见到成片 `no member named`，先回头看结构体定义 |
| 4 | `static declaration of 'priority_more' follows non-static declaration` | `priority_more` 已在 `thread.h` 声明（非 static），又在 `thread.c` 加了 `static` 前向声明 | 删掉 `thread.c` 里的 static 前向声明；已在头文件的可直接用 |
| 5 | `implicit declaration of function 'timer_ticks'` | `thread.c` 没 `#include "devices/timer.h"`，`timer_ticks()` 被当返回 `int`（实际 `int64_t`）→ 截断 | 顶部补 include |
| 6 | `expected ',' or ';' before 'void'` / `before 'if'` | 声明语句漏分号（`int priority = …` / `int load_avg = 0`） | 补分号；这是本项目三次踩到的同一坑 |
| 7 | `unused-function 'mlfqs_priority'` / `'wakeup_cmp'` | `-Wall` 的"写了半截"指纹：定义了没被调用 | 说明调用路径没接上，回去把 `thread_tick` 的 4-tick 块接上 |

> **判据**：`multiple definition` = 头文件函数没 `static inline`（链接期）；`implicit declaration` = 缺声明（编译期）；`expected …; before 'void'` = 上一条声明漏分号。
> **`thread.h` 一崩，全项目连带炸**，先修头文件再看别的。

---

### 3.6 运行时几大杀手（本任务核心教训）

#### 坑 ① `sema_up` 整个函数体被 `if (!thread_mlfqs)` 包住

| 项 | 内容 |
|---|---|
| 真相 | 为"隔离捐赠"，把 `sema_up` 的**整个 body**（含 `thread_unblock`、`sema->value++`）包进 `!thread_mlfqs` |
| 后果 | mlfqs 下 `sema_up` 什么都不做 → 等待者永不唤醒、`value` 不增 → 用到锁的 `mlfqs-block` **卡死 / 缺行**；其它 mlfqs 测试只用 `timer_sleep`（走睡眠队列）所以侥幸不炸 |
| 症状 | `mlfqs-block` 的 `.result` 缺失 `(mlfqs-block) ...got it.`（block 线程拿到锁后那行） |
| 修法 | 只包**捐赠**；`unblock`/`value++`/`sema_down`/`holder=` **两模式都要** |

#### 坑 ② `sema_up` 的抢占（`need_yield`）被 `!thread_mlfqs` 关掉

| 项 | 内容 |
|---|---|
| 真相 | 误以为"MLFQS 的抢占只由 4 tick 负责"，把 `if (!thread_mlfqs && t->priority > current)` 里的抢占关掉 |
| 后果 | main 释放锁唤醒 block 线程后不 yield → main 一路跑到 `end`，block 线程还没轮到 → 仍缺 `...got it.` |
| 关键 | **唤醒时的优先级抢占两种模式都需要**。4 tick 抢占是"重算后调整"，`sema_up` 是"唤醒即抢占"，两者不冲突 |
| 修法 | `if (t->priority > thread_current ()->priority) need_yield = true;`（去掉 `!thread_mlfqs`） |

> 坑 ① + ② 是"隔离过度"的一体两面：**只关捐赠，别关唤醒和抢占。**

#### 坑 ③ 捐赠改变优先级后 `sema->waiters` 没重排（`priority-donate-sema`）

| 项 | 内容 |
|---|---|
| 场景 | L(PRI+1) 先阻塞在 sema；M(PRI+3) 后阻塞 → 排到 L 前；H 把优先级**捐赠**给 L（L→PRI+5），但 **L 在 waiters 里的位置没变**，仍在 M 后 |
| 后果 | main 的 `sema_up` 弹出队首 = **M**（旧位置），L 醒不来 → 缺 `L downed semaphore / H acquired lock / H finished / L finished` |
| 铁证 | `.result` 里 `Thread M finished.` 出现在 `Thread L downed semaphore.` 之前 |
| 修法 | `sema_up` 唤醒前 `list_sort (&sema->waiters, priority_more, NULL);` —— 按**当前**优先级重排，队首才是真正最高者 |

> 这个坑不影响 mlfqs（捐赠在 mlfqs 下关闭），但会让 **Task2 的 `priority-donate-sema` 一直红**——它其实是 Task 2 遗留，改 Task 3 的 `sema_up` 时顺带修掉。

#### 坑 ④ 更新顺序错（先重算优先级、后更新 recent_cpu）

| 项 | 内容 |
|---|---|
| 后果 | 4 tick 重算用**上一秒的旧** `recent_cpu`，fair / nice 数值漂移 |
| 修法 | 严格按 `+1 → 每秒(load_avg → recent_cpu) → 4 tick(优先级)`，第二档必须在第三档之前 |

---

### 3.7 验收与证据

#### ① 编译（按 CI 方式建 build）

```bash
cd /root/pintos/src/threads
mkdir -p build/devices build/lib/kernel build/lib/user build/tests/threads build/threads
cp ../Makefile.build build/Makefile
cd build && make 2>&1 | grep -E "warning|error" \
  | grep -vE "noreturn|frame-address|nonnull-compare|format-truncation"
ls -l kernel.bin
```
应无输出（4 项基线噪音照旧忽略），kernel.bin 必须存在。 ⚠️ 改过 thread.h / synch.c 后做一次 rm -rf build 干净重建，防旧 .o 蒙混。

#### ② 单跑 MLFQS 9 条
```bash
export PATH=$PATH:/root/pintos/src/utils
cd /root/pintos/src/threads/build
for t in load-1 load-60 load-avg recent-1 fair-2 fair-20 nice-2 nice-10 block; do
  printf "%-10s " "$t"; timeout 60 pintos --qemu -- -mlfqs -q run mlfqs-$t 2>&1 | tail -1
done
```
期望 9 行全 Powering off...。注意 MLFQS 必须带 -mlfqs。

#### ③ 全套回归（27/27）
```bash
cd /root/pintos/src/threads
make check 2>&1 | tee /tmp/after-mlfqs.log
tail -1 /tmp/after-mlfqs.log          # 期望 0 of 27 tests failed.
```
#### ④ 数值判据（Task 3 的真正证据）
```bash
cd /root/pintos/src/threads/build
pintos -- -mlfqs -q run mlfqs-recent-1 2>&1 | grep recent_cpu
```
看逐秒序列：recent_cpu 单调上升、load_avg 缓慢趋近稳定值。 ⚠️ 数值不对就 FAIL，Powering off... 不代表通过。

.result 里若只有 Bochs 横幅、没有 Pintos 输出，说明该次运行没抓到 console（环境问题，假失败）——以单跑 + -v 实时输出为准。

### 3.8 语义层坑位清单（能编过但会跑错）

| # | 坑 | 症状 | 正确做法 |
|---|---|---|---|
| 1 | `×100` 漏取整 / 返回定点原值 | `recent-1` / `load-*` 数值全错 | `fixed_to_int_round(fixed_mul_int(x,100))` |
| 2 | 每秒更新少了 `% TIMER_FREQ` | 每 tick 都衰减 → 数值爆炸 | `timer_ticks() % TIMER_FREQ == 0` |
| 3 | `+1` / `+nice` 当普通整数加 | 少乘了 `f`，量纲错 | `fixed_add_int` / `int_to_fixed` |
| 4 | `recent_cpu/4` 用 `round` 而非 `trunc` | fair 微偏 | `fixed_to_int_trunc` |
| 5 | 乘/除没用 `int64_t` 中间量 | 溢出，数值变负/乱 | `fixed_mul` / `fixed_div`（库已处理） |
| 6 | 除法写成 `x/y*f` | 精度被砍 | `x*f/y` |
| 7 | `ready_threads` 漏了运行线程 / 算进 idle | `load_avg` 偏高偏低 | `list_size(ready) + (t!=idle?1:0)` |
| 8 | 两个写者并存（`thread_update_priority` 没 early-return） | 数值抖动，**编译无 warning** | mlfqs 下开头 `return;` |
| 9 | 4 tick 后没重排 `ready_list` | 高优先级不抢占 | `list_sort(&ready_list, priority_more, NULL)` |
| 10 | 对 `running`（不在 ready_list）做 `list_remove` | 链表成环 | 只重排 `ready_list` 成员，`running` 不在其中 |

> **判据小结**：1–3 属"数值错"，靠 `recent-1` / `load-*` 的逐秒序列抓；4–7 属"定点错"，靠手算对照抓；8–10 属"调度错"，靠 `fair` / `nice` 抓。

---

### 3.9 调试工具箱（在 1.8 / 2.9 基础上新增 4 条）

| 场景 | 命令 | 判读 |
|---|---|---|
| 看 MLFQS 逐秒数值 | `pintos -- -mlfqs -q run mlfqs-recent-1 2>&1 \| grep recent_cpu` | 序列单调上升、`load_avg` 缓升 = 对；否则查 3.8 |
| 定点数手算自测 | 临时 `printf` 打 `fixed_mul(int_to_fixed(3)/2, int_to_fixed(3)/2)` | `1.5×1.5` 应为 `36864`(2.25)；`100×100` 应为 `163840000`（验 int64） |
| 看真实内核输出 | `cat tests/threads/mlfqs-block.result` | 缺 `...got it.` → 坑①②；缺 `M/L/H` 行 → 坑③ |
| 确认 `sema_up` 没被误包 | `grep -n "thread_mlfqs" threads/synch.c` | 只应出现在**捐赠块**，不该包 `unblock` / `value++` / `sema_down` |
| 确认没留旧 busy-wait | `grep -rn "timer_elapsed" devices/timer.c` | 应为空（Task1 已清除） |

> ⚠️ `mlfqs-*` 的 `.ck` 是 **Perl 脚本**（`check_expected([<<'EOF'])`…`EOF;`），直接 `diff` 原始文件会满屏 Perl 干扰，看不出差异——看差异请用 `make check` 生成的 `.result` / `.errors`，或把 `EOF` 到 `EOF` 之间那段抽出来比。

**定点数手算对照表（自测用）**

| 表达式 | 期望真实值 | 期望定点值 |
|---|---|---|
| `int_to_fixed(1)` | 1.0 | 16384 |
| `fixed_mul(1.5, 1.5)` | 2.25 | 36864 |
| `fixed_mul(100, 100)` | 10000.0 | 163840000（验 int64 不溢出） |
| `fixed_div(1.0, 3.0)` | ≈0.333 | 5461 |
| `fixed_to_int_trunc(-7/2)` | -3 | — |
| `fixed_to_int_round(-7/2)` | -4 | — |

---

### 3.10 复用教训（带进今后）

1. **"隔离"要精准到子句**：`if (!thread_mlfqs)` 只包「捐赠」，不包「唤醒 / 排序 / 抢占 / 置 holder」。
   包大了 = 锁在 mlfqs 下全废（3.6 坑①②）。判据：问一句"这个动作是不是只在手工优先级下才有意义？"
2. **换写者 ≠ 重写**：`priority` 字段与 `ready_list` / `waiters` 排序全部沿用，只把"谁写它"换掉。
   一上来推翻重写必撞 Task2 的 12 条回归。
3. **数值型测试不能只看 exit**：`Powering off...` 只证明"跑完"，证明不了"算对"。
   逐秒序列 + 手算定点才是证据（3.1-D、3.7④）。
4. **`static inline` 是头文件函数默认形态**：不 `static` 会链接冲突（multiple definition），
   不 `inline` 会 `unused-function` 警告（`-Werror` 下即编译失败）。
5. **一次改动 = 一次干净重建**：改 `.h` 后旧 `.o` 会蒙混，`rm -rf build` 再编，别攒到最后。
6. **缺失行定位法**：`.result` 里"少了哪行 / 顺序反了"直接指向"哪个动作没发生"——
   缺 `...got it.` = 唤醒/抢占没发生；缺 `M/L/H` 行 = 唤醒到了错线程（重排问题）。

---

### 3.11 回滚点与坐标 / 移交

```bash
cd src/threads
git add -A
git commit -m "Task3: 4.4BSD MLFQS (fixed-point, recent_cpu/load_avg/nice, -mlfqs wiring)"
git tag -a task3-mlfqs -m "Task3 done: 27/27, mlfqs 9 + regress 18"

# 改动规模核对（应 4 files changed：增 fixed-point.h，改 thread.h/thread.c/synch.c）
git diff task2-priority..task3-mlfqs --stat

# 可选：独立 worktree 复验
git worktree add /tmp/task3-wt task3-mlfqs
cd /tmp/task3-wt/src/threads && make && make check 2>&1 | tail -3
```
坐标：task2-priority → task3-mlfqs（最终 27/27）
期望 --stat：threads/fixed-point.h（新增）/ threads/thread.h / threads/thread.c / threads/synch.c，4 个文件
期望 make check 尾行：0 of 27 tests failed.
新增/改动的对外符号

| 符号 | 类型 | 归属 | 说明 |
|---|---|---|---|
| mlfqs_priority | static 函数 | thread.c | 只本文件用；thread_tick 通过前向声明调用 |
| load_avg | static 变量 | hread.c | 系统级，仅 thread.c 可见，别放头文件 |
| thread_set_nice / thread_get_nice / thread_get_recent_cpu / thread_get_load_avg | 全局函数 | thread.c | 声明在 thread.h |
| nice / recent_cpu	成员 | struct thread | recent_cpu | 为定点（int，语义 16.14） |

