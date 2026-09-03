# nekomata（真-原生 C++ 热重载）前置知识上手指南

> 配套：[cpp-native-hot-reload-roadmap.md](./cpp-native-hot-reload-roadmap.md)（项目提案，含阶段划分与 API 取舍）
> 适用对象：发起人本人——C++ 工程与构建系统熟练、二进制与符号体系偏弱。

## 先分清两类学习，再安排时间

前置知识分两类，混在一起学是这类项目最常见的失败方式：

* **动手前必须掌握**：一条心智模型，加两个知识域（目标文件与符号、x86-64 机器码补丁）。没有它们写不出第一行有效代码。合计约 1–2 周。
* **随阶段同步补**：其余知识域。只在对应阶段开工前一周补齐，不提前学。对应关系见文末表格。

一条纪律：**任何时候预研时间连续超过动手时间，立即停下，回到练习**。本文的练习阶梯本身就是学习路径，不存在"先学完再动手"的版本。

## 动手前：建立唯一的心智模型

用一句话记住全部机制，之后每个知识点都往这句话上挂：

```
热重载 = 重编译 + 重定位 + 重定向
```

* **重编译**：改完源码重新编译出 `.o`。`.o` 里的地址全是 0 基的，链接/加载时才最终确定——所以你做的每件事本质上都是"一次运行时的迷你链接"。
* **重定位**：把新 `.o` 里的地址引用修正为运行中进程的真实地址。这是一切的核心概念。
* **重定向**：函数调用就是几个字节。把原函数入口改写成一条 5 字节 `jmp`，下一次调用就走新代码——全部魔法的本质。

建议动作用：通读 [CSAPP 第 7 章 Linking（免费预览版 PDF）](https://csapp.cs.cmu.edu/2e/ch7-preview.pdf)。这一章同时覆盖心智模型和下文的"目标文件与符号"域，是性价比最高的一次投入。

## 动手前：必修的两个知识域

### 域一 · 目标文件与符号（ELF 优先，PE 后补）

为什么必修：定位"这个函数在进程里的地址、多大"是一切替换的前提；重定位表读不懂就无法修正新代码里的引用。

必须掌握：

* `readelf -s / -S / -r` 每一列的含义；`st_value`（地址）与 `st_size`（大小）
* 重定位条目怎么读（`R_X86_64_PC32` / `PLT32` / `ABS64` 各自修什么）
* section 与 segment 的区别；`.text` 页为什么是只读 + 可执行
* PIE/ASLR：为什么每次运行地址不同，如何从 `/proc/<pid>/maps` 拿到加载基址

验收问题（答得上来即过关）：为什么 `.o` 有重定位表，而非 PIE 的最终可执行文件几乎没有？

### 域二 · x86-64 机器码与直接补丁

为什么必修：这是"替换"的手艺本身，没有替代路径。

必须掌握：

* 指令编码骨架（前缀 / opcode / ModRM / disp / imm）与**变长指令**——覆盖字节必须落在完整指令边界上
* 三种跳转：`EB rel8`（2 字节）、`E9 rel32`（5 字节，**关键数字**）、`movabs + jmp`（约 12 字节，远跳兜底）
* `/hotpatch` 约定（MSVC `/FUNCTIONPADMIN`）：入口前预留 5 字节、首条指令至少 2 字节
* trampoline："偷字节"= 把被覆盖的旧指令按完整指令搬走，尾接跳回原函数的 jmp
* 两套调用约定：SysV（`rdi, rsi, rdx, rcx, r8, r9`）与 Win64（`rcx, rdx, r8, r9` + shadow space）；16 字节栈对齐
* `mmap` / `mprotect`：怎么给新代码要一块可执行内存、怎么把函数页临时改可写

验收问题：为什么偷字节必须按完整指令搬、不能按固定字节数切？

## 开工门槛：三个练习

做完这三个练习即达到开工标准，可以正式启动提案的阶段一。每个练习都有明确验收。

**练习一 · 读符号（1–2 天）**
写一个两文件程序，用 `readelf -s` 找出某函数的地址与大小，用 gdb 的 `info symbol` 交叉验证。
为什么先做它：把域一从"读过"变成"用过"，且一天内必然完成，用于建立信心。

**练习二 · 迷你 JIT（2–3 天）**
在进程内 `mmap` 一块 RWX 内存，`memcpy` 一段手写机器码（如 `mov eax, 42; ret` 即 `b8 2a 00 00 00 c3`），强转函数指针调用并断言返回 42。
为什么做它：一次性打通"内存里可以有可执行代码"这个认知，域二的全部操作都建立在它之上。

**练习三 · 第一次补丁与恢复（3–5 天）**
对某函数 `mprotect` 改页可写 → 写入 `E9 rel32` 跳到一个 stub → 调用行为改变；然后反向做 trampoline：搬走被覆盖指令 + 跳回，恢复原行为。
为什么它最重要：这一步跨过整个项目最难的心理门槛。做完它，"真-原生热重载"从概念变成你亲手做过的操作。

## 随阶段同步补的知识域

不要提前学。在对应阶段开工前一周，按下表补齐：

| 开工前要补的域 | 对应阶段 | 为什么在那个节点学 |
|---|---|---|
| DWARF 基础（`DW_TAG_subprogram` 的 pc 区间、`llvm-dwarfdump` 用法） | 阶段二 · 真实可用 | 符号表给不出精确函数区间时才需要它；阶段一用不到 |
| 构建依赖图（`.d` 文件、compile_commands.json、`-ffunction-sections`） | 阶段二 | 决定"改了文件要重编谁"，是 patch_planner 的输入；blink 的替代方案（从调试信息提取原编译命令）也在此阶段评估 |
| 线程安全点（暂停线程查 RIP vs 空闲点替换） | 阶段二 | 阶段一允许固定时刻替换，回避了这个问题 |
| PDB 经 DIA SDK 读取、PE 基址重定位、`/hotpatch` 构建约定 | 阶段三 · Windows | 只在 Windows 后端启动时学 |
| `.eh_frame` 栈展开 | 阶段三 | 补丁后栈回溯必须仍正确 |
| DWARF 内联单元（`DW_TAG_inlined_subroutine`）、COMDAT 折叠 | 阶段四 · 优化构建 | 全项目最难的域，严禁提前投入 |
| C++ ABI（mangling、vtable、类布局）、对象追踪 | 阶段五 · 类布局迁移 | 加成员后老对象怎么办，是那一阶段的全部难点 |

## 明确不学的（防止兔子洞）

以下每一项都有"看起来该学、实际上会拖死项目"的诱惑，在到达对应阶段前一律不碰：

* **PDB 二进制格式内部**——永远用 DIA SDK 读，自研解析器是数月级的兔子洞，Live++ 博客 Phase 1 也直接用微软的 Dia2Dump。
* **写一个完整链接器**——你只需要"单个翻译单元的运行时迷你链接"。
* **ptrace / 外部进程注入**——进程内 Agent 已够用，外部注入引入的复杂度与收益不成比例。
* **ARM/AArch64 指令编码**——x86-64 之外的目标在 Non-Goals 里；ARM 的 icache flush 语义是另一套知识。
* **内联重建、类布局迁移的提前研究**——分别是阶段四、五的核心难题，提前学没有可验证的落地对象，纯属空转。

## 必读资料（按此顺序）

1. [CSAPP 第 7 章 Linking](https://csapp.cs.cmu.edu/2e/ch7-preview.pdf) —— 动手前唯一必读，一次覆盖心智模型 + 域一。
2. [Live++ 官方博客（2026-01 起连载）](https://liveplusplus.tech/blog/index.html) —— 作者 Stefan Reinalter 正在从零解密实现：[Introduction](https://liveplusplus.tech/blog/posts/2026-01-26-introduction.html) → [Phase 0: Motivation and goals](https://liveplusplus.tech/blog/posts/2026-02-09-phase_0_motivation_goals.html) → [Phase 1: Build information](https://liveplusplus.tech/blog/posts/2026-02-23-phase_1_build_information.html)。这是本提案的"作者亲授"，追更它。
3. [Live++: A Bag of Tricks（Guerrilla Games 2026 幻灯片）](https://liveplusplus.tech/downloads/Guerrilla_Games_2026_Live++_A_Bag_of_Tricks.pdf) —— 做完练习三后读，理解工程细节的深水区。
4. [Eli Bendersky：Load-time relocation of shared libraries](https://eli.thegreenplace.net/2011/08/25/load-time-relocation-of-shared-libraries) 及其同站 PIC 系列 —— 域一的重定位部分加深。
5. [ELF Nightmares: GOTs, PLTs, and Relocations（BSDCan 2025）](https://www.bsdcan.org/2025/talks/elf-nightmares.pdf) —— 进阶二页，阶段二前读。
6. [Microsoft Detours](https://github.com/microsoft/Detours) —— trampoline 实现的经典参考，练习三遇到边界问题时对照源码。
7. [LLVM ORC 文档](https://llvm.org/docs/ORCv2.html) —— 运行时重定位/内存管理的同源机制，阶段二设计 `code_substituter` 时读。
8. 同类开源的 README 与设计文档：**blink 的 README「Concept」一节是"运行时链接"端到端最简明的单页讲义**（找 PDB → 重跑原编译命令 → 拷节进进程 → 按旧地址保全局 → 修重定位 → 函数入口写 jmp），做完练习三后精读；jet-live 的 "How it works"（compile_commands.json 与 .d 依赖图的实际用法）、cr.h、RCC++ wiki —— 对照它们各自的取舍，反推自己的设计要点。
9. 《Linkers and Loaders》（John R. Levine）—— 域一的系统性加深，可无限后置，不阻塞任何阶段。

## 每日工具

* `readelf` / `objdump -d` / `nm` —— 看节、符号、重定位、反汇编
* `llvm-dwarfdump` —— 阶段二起读 DWARF
* gdb —— `x/i` 看指令、`disassemble`、`info symbol <addr>`、直接改内存验证补丁
* `/proc/<pid>/maps` —— 查真实加载基址
* [godbolt.org](https://godbolt.org) —— 直观对比不同优化级别的代码生成与内联行为（阶段四前就会频繁用到它做观察）
