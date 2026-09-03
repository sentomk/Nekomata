# 提案：nekomata —— 开源"真-原生 C++ 热重载"工具

* 日期：2026-09（同月完成命名与定位修订）
* 状态：草案（待设计评审）
* 名称：nekomata（双尾猫；命名理由与撞名核查见 FAQ）
* 定位句：Native hot-reload for C/C++ — code that lives long enough grows a second tail. No restarts, no refactor.
* 配套文档：[cpp-hot-reload-prerequisite-knowledge.md](./prerequisite-knowledge.md)（前置知识上手指南）
* 立项依据：对 Live++ 及全部主流开源替代的调研（数据见 Motivation，GitHub 提交时间经 API 于 2026-09 核实）

## 目录

* [Summary](#summary)
* [Motivation](#motivation)
    * [商业工具闭源且收费](#商业工具闭源且收费)
    * [开源替代：停更的、受限的，恰好绕开了目标位置](#开源替代停更的受限的恰好绕开了目标位置)
    * ["真-原生"的可衡量定义](#真-原生的可衡量定义)
    * [需求不止于游戏引擎](#需求不止于游戏引擎)
* [Goals / Non-Goals](#goals--non-goals)
* [Detailed Design](#detailed-design)
    * [总体架构：内核 + 可插拔后端](#总体架构内核--可插拔后端)
    * [实现机制：重编译 + 重定位 + 重定向](#实现机制重编译--重定位--重定向)
    * [对标工具的用法模式与本项目的 API 取舍](#对标工具的用法模式与本项目的-api-取舍)
    * [阶段划分与验收标准](#阶段划分与验收标准)
    * [Open Areas](#open-areas)
* [Risks and Mitigations](#risks-and-mitigations)
* [Drawbacks](#drawbacks)
* [Alternatives](#alternatives)
* [Adoption Strategy](#adoption-strategy)
* [Credits and Prior Art](#credits-and-prior-art)
* [FAQ](#faq)

# Summary

本提案计划启动一个开源项目，实现与 Live++ 同类的"真-原生"C++ 热重载：**不改造现有源码、不引入函数指针间接层，在进程运行中直接替换机器码，并保留程序状态**。

关键主张：

* **切入点真实存在**：截至 2026-09，活跃方案中 jet-live 已做到低侵入，但作者明示仅支持 `-O0` 调试构建、多线程重载不可靠、无 Windows；其余活跃项目要求拆库/插件化；零侵入二进制补丁的 blink 已停更。"低侵入 + 优化构建 + 多线程安全 + 跨平台 + 活跃维护"五项交集为空，这正是 Live++ 独占而开源空缺的规格（数据见 Motivation）。
* **技术路线**：运行时重编译受影响的翻译单元，按运行中进程的真实地址做重定位，再将被替换函数的入口改写为跳转（recompile + relocate + redirect）。
* **架构**：平台无关内核 + 可插拔后端。内核只依赖四个接口（`SymbolProvider` / `CodeSubstituter` / `PatchPlanner` / `StateManager`），ELF/DWARF 与 PE/PDB 分别作为后端实现。
* **首个可交付**：Linux/ELF 上的单函数替换原型，单人 4–8 周可完成，完成后即开源。
* **诚实的能力边界**：优化构建下的内联处理与类布局迁移放在后期阶段，首版不承诺（见 Non-Goals）。
* **目标不止游戏**：热重载价值 ≈ 重启成本 × 迭代频率 × 保住的不可重建状态 − 接入与信任成本。非游戏世界已有大量付费先例（内核 livepatch、JRebel、HMR、Flutter），nekomata 面向"任何重启昂贵的 C++ 工作负载"（见 Motivation 与 Adoption Strategy）。
* **对快速链接器的正面回答**：mold/lld 把重编+重启压到秒级，是真正的头号竞品；nekomata 的差异化不是"快"而是"保状态"——再快的重编也回不到加载 40 分钟的现场（见 Alternatives）。

# Motivation

### 商业工具闭源且收费

Live++（Molecular Matters GmbH，创始人 Stefan Reinalter）是这个方向唯一成熟的实现：官方页面显示超过 100 家公司在自有引擎和框架上使用，客户包括 id Software、Blizzard、Epic、EA、Ubisoft、Valve、Riot 等；30 天免费试用后需商业授权。它验证了需求真实存在且规模不小，但闭源意味着这个能力无法被自由使用、审计或扩展——尤其是对无法采购商业工具的个人开发者和中小团队。

值得注意的是，Live++ 的技术门槛并非不可逾越：其作者从 2026 年 1 月起在官方博客从零公开实现思路（见 [Prior Art](#credits-and-prior-art)），说明核心机制是可被独立复现的。

### 开源替代：停更的、受限的，恰好绕开了目标位置

2026-09 经 GitHub API 核实的各项目最近一次代码提交（`pushed_at`）：

| 项目 | 路线 | 最近提交 | 星数 | 与本提案目标的差距 |
|---|---|---|---|---|
| [RuntimeCompiledCPlusPlus](https://github.com/RuntimeCompiledCPlusPlus/RuntimeCompiledCPlusPlus) | 运行时重编译 | 2025-10-31（活跃） | — | 要求按 RCC++ 范式组织代码；优化构建支持有限 |
| [crosire/blink](https://github.com/crosire/blink) | 二进制补丁（最接近目标） | 2023-12-18（半停更） | 1179 | 仅 Windows/MSVC；对优化代码与部分函数类型变动不可靠 |
| [fungos/cr](https://github.com/fungos/cr) | 动态库换库 | 2026-06-23（活跃） | 1801 | 面向 C；每库仅一个固定入口函数，C++ 需包成插件 |
| [ddovod/jet-live](https://github.com/ddovod/jet-live) | 函数级重载（低侵入） | 2026-03-02（活跃） | 456 | 仅 Linux(x86_64)/macOS(arm)；仅 `-O0` 非 strip 调试构建；多线程重载不可靠；带捕获 lambda 不可靠 |
| [jheruty/hscpp](https://github.com/jheruty/hscpp) | 运行时重编译 | 2022-08-15（停更约 4 年） | 129 | 单维护者，已无维护 |
| [Naios/idle](https://github.com/Naios/idle) | 组件框架 | 2021-10-03（停更约 5 年） | 213 | OSGi 式框架，接入=重构整个应用 |
| [gayafhannah/hotreloadcpp](https://github.com/gayafhannah/hotreloadcpp) | 动态库换库 | 2024-11-27 | 2 | 演示级，非生产可用 |
| [Pagghiu/SaneCppLibraries](https://github.com/Pagghiu/SaneCppLibraries) | 插件框架 | 2026-08-31（活跃） | 627 | 是插件体系而非二进制替换 |

把上表按三个维度切——是否低侵入、是否支持优化构建与多线程、是否活跃维护——空位就出现了：jet-live 做到了低侵入 + 活跃，但作者明说优化构建"大概率完全不工作"、多线程重载不可靠、无 Windows；blink 做到了零侵入 + 二进制补丁，但仅 Windows/MSVC 且 2023-12 后无提交。**低侵入 + 优化构建 + 多线程安全 + 跨平台 + 活跃维护**的五项交集为空——这是 Live++ 独占的规格，也就是 nekomata 要占的位置。

### "真-原生"的可衡量定义

为避免定位漂移，"真-原生"定义为四条可验收的标准：

1. **零代码改造**：接入后不需要把函数或类改成插件、接口或函数指针（对比 cr.h 的单入口、jet-live 的拆库）。
2. **运行中替换**：修改源码后触发重载，进程不重启，下一次调用即走新代码。
3. **状态保留**：全局与静态变量在新旧代码之间保持一致或正确迁移。
4. **多平台、多编译器**：至少 Linux/ELF + Windows/PE，支持 Clang 与 MSVC。

### 需求不止于游戏引擎

热重载的价值可以写成一条公式：

    价值 ≈ 重启成本 × 迭代频率 × 保住的不可重建状态 − 接入与信任成本

游戏引擎只是"重启成本极高"的最典型样本。非游戏世界早已在为热重载付费，C++ 世界的缺席不是因为不需要，而是缺可用工具：

| 领域 | 证据 |
|---|---|
| Linux 内核 | kpatch（Red Hat）、Ksplice（Oracle）、SUSE/Canonical 内核热补丁——用函数级二进制补丁不打重启修 CVE，均为收费企业服务，机制与本项目同源 |
| 电信 | Erlang/BEAM 的热代码加载是"九个九"可用性的支柱 |
| Java 企业 | JRebel（ZeroTurnaround，已被 Perforce 收购）——商业模式与 Live++ 完全同构 |
| 前端 | 现代 Web 开发整体建立在 HMR 上 |
| 移动 / .NET | Flutter 把亚秒级热重载当核心卖点；微软把 Hot Reload 做进 VS 2022 |

对应的 C++ 高重启成本场景（按代价排序）：带大量内存状态的长生命周期进程（2015 年 Redis Lua 漏洞期间，安全研究者甚至用漏洞本身演示了对生产进程做 ELF 热补丁——这一需求最极端的注脚）、CAD/CAE/EDA/科学仿真（网格与数据集加载数十分钟）、机器人/HIL/自动驾驶仿真、DCC 插件开发（Blender/Maya/Houdini，改一行重启宿主）、音频插件（VST/JUCE），以及被低估的调试场景——"保住 bug 现场改代码"：这不是快慢问题，是现场不可重建。

# Goals / Non-Goals

**Goals**

* 证明"零改造 + 运行中替换 + 状态保留"在开源、可复现的条件下成立
* 以平台无关内核 + 后端插件的架构组织代码，使 ELF 与 PE 共享同一套编排逻辑
* 每个阶段独立可交付、可演示，首个原型完成后立即开源

**Non-Goals**（首版与近期明确不做的）

* 优化构建（`-O2`）下的内联函数替换——这是后期阶段，见 [阶段划分](#阶段划分与验收标准)
* 类布局变化后的对象迁移与 vtable 更新——同上
* ARM/AArch64 支持（x86-64 优先）
* 调试器集成、远程/网络编辑、跨进程 Broker 等 Live++ 的高级周边

**何时不该用 nekomata**（将写入 README 的边界声明）

* 重启成本几秒内的工作负载——用 mold/lld + 增量构建即可，且更可复现
* 无状态、可滚动重启的服务——热重载是伪需求
* 合规禁止自修改代码的生产运行时（金融/航空/医疗）——nekomata 是开发期工具，不做内核 livepatch 那类生产热补丁
* 需要可复现干净构建的验证与发布环节——热重载只服务于迭代，不进入构建产物

# Detailed Design

### 总体架构：内核 + 可插拔后端

内核不含任何平台代码，只依赖四个抽象接口：

```cpp
namespace neko {

// 符号与调试信息：函数地址、边界、类型布局、内联单元
class SymbolProvider {
  virtual std::vector<FunctionInfo> allFunctions() const = 0;
  virtual TypeLayout layoutOf(TypeId) const = 0;
};

// 代码替换：写入新代码、生成入口跳转、执行重定位
class CodeSubstituter {
  virtual void* reserveCode(uint64_t bytes) = 0;
  virtual bool patchEntry(const Patch& p) = 0;
  virtual bool relocate(const Relocation& r) = 0;
};

// 变化规划：改动文件 -> 需要重编的翻译单元 -> 需要替换的函数
class PatchPlanner {
  virtual std::vector<PatchPlan> plan(const ChangeSet&) const = 0;
};

// 状态管理：全局/静态变量映射与（后期）布局迁移
class StateManager {
  virtual void* mapGlobal(GlobalId g) = 0;
};
}
```

后端分四类，各自独立成目录与测试：

| 后端 | Linux 侧 | Windows 侧 |
|---|---|---|
| 编译前端 | Clang（要求 ≥14） | MSVC |
| 符号后端 | DWARF（配合 ELF `.symtab`） | PDB（经 MS DIA SDK 读取，不自研解析器） |
| 二进制后端 | ELF | PE |
| 运行时代理 | 进程内 Agent（`mmap`/`mprotect`） | 进程内 Agent（`VirtualProtect`） |

选择这个架构的直接原因是发起人背景：构建系统与工程化是强项、二进制是弱项。抽象层优先允许先用接口和测试骨架交付工程价值，再逐个啃后端；ELF 后端（DWARF/重定位）与 PE 后端（DIA/`/hotpatch`）的难度被隔离在各自目录内，不会互相阻塞。

### 实现机制：重编译 + 重定位 + 重定向

一次热重载的完整数据流：

```
修改 .cpp
  -> 编译前端产出新 .o（内含重定位表，地址全是 0 基）
  -> 符号后端给出函数地址/大小
  -> 按运行中进程的真实地址修正重定位（等价于一次运行时迷你链接）
  -> 二进制后端把新函数体写入 mprotect 为可执行的保留页
  -> 将原函数入口改写为跳转
  -> 下一次调用走新代码
```

入口改写的具体形式（也是为什么 5 字节是关键数字）：

```
原函数入口:  push rbp        ; 55
patch 后:    e9 xx xx xx xx  ; jmp rel32 —— 恰好 5 字节，覆盖 >=5 字节的指令序列
```

* 若函数以 `/hotpatch` 方式构建（MSVC `/FUNCTIONPADMIN`），入口前预留了 5 字节、首条指令至少 2 字节，可在不动函数体的情况下插入跳转，天然可恢复。
* 若无预留，则采用 trampoline：把被覆盖的旧指令**按完整指令边界**搬到新内存，尾接一条跳回"原函数 + 被覆盖字节数"的 jmp。旧线程若正执行在函数内部，走的仍是搬走的旧代码，这天然缓解了并发替换的风险。

### 对标工具的用法模式与本项目的 API 取舍

对各工具实际用法的一手调研（README 原文）归纳出四种模式：

| 模式 | 代表 | 侵入性 | 取舍 |
|---|---|---|---|
| 外部进程 attach | blink：`blink.exe PID`，零行代码 | 零 | 无法做进程内安全点与状态钩子 |
| 进程内 Agent + 每帧 tick | Live++：`EnableModules` + 每帧 `UpdateAgent`；jet-live：`update()` + `tryReload()` | 2–3 行 | 有同步点，是后续多线程安全与状态钩子的结构前提 |
| 宿主/插件拆分 | cr.h：`cr_main` 生命周期 + 自动回滚 | 架构级 | 最稳，但违背零改造目标 |
| 对象热交换框架 | RCC++：`IObject` + 序列化；hscpp：`HSCPP_TRACK` | 接口级 | 即"换库路线"，非本项目路线 |

nekomata 的 API 取第二种（Live++ 型：注册模块 + 每帧一个同步点，2–3 行接入），因为同步点是多线程安全替换（自阶段二起）与状态钩子的结构前提。同时吸收 blink 的两个技巧：从调试信息中提取原始编译命令（避免强依赖 compile_commands.json）、全局变量按既有地址重定位以保状态。触发模型参照 jet-live 的分层——编译自动、重载由用户显式确认，避免半成品代码被意外换入。

### 阶段划分与验收标准

每个阶段独立可交付；验收不通过不进入下一阶段。

**阶段一 · 单函数替换原型（Linux/ELF，`-O0`，预计 4–8 周）**

* 范围：一个翻译单元内的少量函数；无内联、无跨单元引用数据。
* 验收：修改某函数后，运行中进程的下一次调用走新逻辑；全局变量值保持；仓库含一键复现的演示（脚本 + 预期输出）。
* 剪枝：完整重定位、DWARF、线程安全点全部延后——此阶段允许"单线程、固定时刻替换"。
* 意义：证明机制成立，验证四个接口的形状，产出开源首秀。

**阶段二 · 真实可用（预计 8–12 周）**

* 范围：DWARF 解析函数区间；整翻译单元多函数替换；依赖图决定波及面（复用编译器 `.d` 依赖文件）；固定安全点替换。
* 验收：在一个第三方真实项目上完成一次"改函数不重启"演示，无需修改该项目任何代码。

**阶段三 · Windows/PE/PDB（预计 8–12 周）**

* 范围：DIA SDK 读 PDB；MSVC + `/hotpatch` 构建；PE 重定位。
* 验收：与阶段二同口径的演示在 Windows 上通过。

**阶段四 · 优化构建（高风险，长期）**

* 范围：`-O2` 下被全内联的函数没有独立函数体，需从 DWARF `DW_TAG_inlined_subroutine` 重建内联单元并逐点补丁；处理 COMDAT 折叠。
* 定位：这是与所有现有开源拉开差距的分水岭，也是 Live++ 护城河所在。前三阶段完成前不启动。

**阶段五 · 类布局迁移（长期）**

* 范围：给类增加成员后，对已分配对象做布局迁移与 vtable 指针更新。
* 前置：类型级信息（DWARF/AST）+ 运行时对象追踪，是全项目最难的部分，仅在前四阶段站稳后立项。

### Open Areas

以下问题在原型阶段用最简答案，进入对应阶段前再重新评估：

* DWARF 内联单元重建的具体算法（阶段四的核心未知项）
* PDB 读取：DIA SDK 与未来自研解析器的接口边界如何划
* 进程模型：是否需要 Live++ 式的常驻 Broker 进程来缓存 `.pdb`/`.obj`（其文档确认 Broker 用于缓存以加速加载）
* 许可证选择（倾向 MIT 或 Apache-2.0，需在首个外部贡献出现前定）
* 线程安全点的实现策略（信号暂停 + RIP 检查 vs 仅在空闲点替换）

# Risks and Mitigations

| 风险 | 缓解 |
|---|---|
| PDB 与内联处理（阶段三/四）深度超出个人能力 | 发起人二进制偏弱是已知事实：阶段一/二刻意只用 ELF + symtab + DWARF 基础部分；阶段三前寻找熟悉 DIA/PDB 的协作者；Live++ 博客正在连载实现细节可作路线参考 |
| 项目半途而废（此类项目常见结局） | 每阶段独立可交付并开源；阶段一仅 4–8 周，用最小成本验证方向 |
| 单人维护的长尾（编译器版本差异、发行版差异） | CI 从第一天起覆盖多编译器版本；README 明确标注已验证的组合 |
| 运行时代理被安全软件/反作弊误报 | 文档明示这是开发期工具而非发布组件；提供环境变量开关 |

# Drawbacks

* 即使完成前三个阶段，与 Live++ 仍有显著差距：无优化构建支持（阶段四前）、无调试器集成、无远程编辑。
* 进程内改写代码本质上是"官方支持的自修改代码"，与 W^X 等安全策略相悖，注定只能作为开发期工具存在。
* 维护成本长尾真实存在：每个编译器大版本都可能改变代码生成约定，需要持续跟进测试。

# Alternatives

以下路线均被评估并否决：

* **复活 blink**：定位最接近，但仅支持 Windows/MSVC，且 2023-12 后无提交，尝试联系维护者风险高于重写；其"runtime linker"思路已被本提案的机制部分吸收。
* **LLVM ORC JIT 路线**：把程序当 JIT 对象增量编译。机制上同源，但要求代码以 JIT 模式构建运行，违背"零改造、面向现有二进制"的目标定义。
* **调试器路线**（lldbhotreload 式，经 LLDB 注入）：依赖调试会话与 `-rdynamic`，无法作为日常开发流；且该类实现（2025-11 出现，0 星）尚属实验性质。
* **快速链接器路线**（mold/lld + ccache + 增量构建）：把全量重编+重启压到秒级，是 C++ 世界应对迭代痛的主流解法，也是本项目真正的头号竞品。关系是互补而非替代——链接器再快也回不到加载 40 分钟的现场，nekomata 卖"保状态"而非"快"。README 将提供两者的选择指南（含"何时该用 mold 而非 nekomata"）。
* **动态库换库路线**（cr.h / RCC++ / SaneCppLibraries）：已被多个活跃项目良好解决，再做没有差异化；其拆库/插件化的限制正是本提案要消除的痛点。jet-live 虽属低侵入路线，但仅 `-O0`、无 Windows、多线程不可靠，未占住目标位置。

# Adoption Strategy

* **首个里程碑即开源**：阶段一完成即建公开仓库，附演示视频与一键复现脚本——对这类工具，"眼见为实"是唯一的传播方式。
* **诚实标注能力边界**：README 首屏同时写定位句（Native hot-reload for C/C++ — code that lives long enough grows a second tail. No restarts, no refactor.）与当前能力（Linux/ELF、`-O0`、单翻译单元），避免用户以 Live++ 的完成度预期评价早期版本；并附"何时不该用"清单（见 Non-Goals）——诚实的边界声明本身就是用户筛选器。
* **首批目标用户**：以游戏引擎为滩头（付费意愿已被 Live++ 的 100+ 客户证明），定位句则面向"任何重启昂贵的 C++ 工作负载"——仿真/EDA/CAD、DCC 与音频插件开发、长生命周期有状态进程是免费工具能吃到而商业工具覆盖不足的长尾。从 Linux 起步也规避了 Windows 反作弊环境的干扰。
* **借势社区窗口**：Live++ 作者博客 2026-01 起正在连载实现解密，社区对该主题关注度处于高位，是发布与招募协作者的时机。

# Credits and Prior Art

* **Live++**（Molecular Matters GmbH）——唯一成熟的同类实现与本项目的主要参照。作者 Stefan Reinalter 的公开材料：
  * 官方博客（2026-01 起连载）：[Introduction](https://liveplusplus.tech/blog/posts/2026-01-26-introduction.html)、[Phase 0: Motivation and goals](https://liveplusplus.tech/blog/posts/2026-02-09-phase_0_motivation_goals.html)、[Phase 1: Build information](https://liveplusplus.tech/blog/posts/2026-02-23-phase_1_build_information.html)
  * 演讲幻灯片：[Live++: A Bag of Tricks（Guerrilla Games, 2026）](https://liveplusplus.tech/downloads/Guerrilla_Games_2026_Live++_A_Bag_of_Tricks.pdf)、[Supercharge your workflow（Mojang, 2025）](https://liveplusplus.tech/downloads/Mojang_2025_Supercharge_your_workflow_using_Live++.pdf)
* **RuntimeCompiledCPlusPlus**（Doug Binks 等）——运行时重编译路线的开创者，本提案的依赖图思路受其启发
* **crosire/blink、fungos/cr、ddovod/jet-live、jheruty/hscpp、Pagghiu/SaneCppLibraries**——调研中的对照系，各自的取舍见 Motivation 表格
* **Microsoft Detours**——函数拦截与 trampoline"偷字节"技术的经典实现
* **LLVM ORC**——运行时重定位与内存管理的同源机制

# FAQ

**为什么不直接用 dlopen 换库？**
那是插件路线，要求把可热重载的代码拆成共享库并走间接层，违背目标定义第 1 条（零改造）；且该路线上已有多个活跃的开源实现，再做没有差异化。

**什么时候支持 Windows？**
阶段三。Windows 的主要成本在 PDB（经 DIA SDK 读取）与 `/hotpatch` 构建约定，架构上已为其预留后端位置。

**和 blink 的区别？**
blink 仅支持 Windows/MSVC 且 2023-12 后无提交；本项目跨平台、从 Linux 起步、按阶段推进，且二进制补丁之外还解决"哪些函数需要重编"的构建信息问题（对应 Live++ 博客 Phase 1 的主题）。

**会做成 Live++ 的完整替代吗？**
不是本提案的目标。前三个阶段交付的是"零改造 + 运行中替换 + 状态保留"的最小完整闭环；优化构建与类布局迁移是长期方向，不构成早期承诺。

**为什么叫 nekomata？**
猫又：活了够久的猫长出第二条尾巴。一条尾巴跑旧代码、一条长出新代码——恰是 trampoline 的工作方式；猫有九命，你的进程一条都不用。2026-09 撞名核查：C++/系统工具领域无占用，PyPI/crates.io 的同名项目属无关领域（生物信息学包、游戏引擎名占位）；GitHub 仓库名不允许 `+`，故仓库与 CLI 用 nekomata，品牌可写作 Nekomata，双尾猫即项目形象。
