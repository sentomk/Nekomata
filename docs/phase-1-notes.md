# Phase 1 实施记录（单函数替换原型）

* 状态：完成，验收通过（见下）
* 对应提案：`docs/proposal.md` §阶段一
* 版本：0.2.0

## 验收证据（提案原文三条标准 → 实测）

1. **修改某函数后，运行中进程的下一次调用走新逻辑** —
   `hello_reload` 演示中，`tick()` 从 `++g_counter` 改为 `g_counter += 10`，
   同一 pid 的进程在热替换后输出 `[v2] ... jumped to 15`（5 + 10），无重启。
2. **全局变量值保持** — `g_counter` 5 → 15（旧值 + 新逻辑步长）、
   文件内静态 `s_calls` #5 → #6（继续 +1），均跨替换连续。
3. **仓库含一键复现的演示** — `ctest -R hello_reload` 或直接
   `bash build/debug/examples/hello_reload/run_demo.sh`；
   预期输出见 `examples/hello_reload/expected_output.txt`。

## 交付内容

| 模块 | 文件 | 说明 |
|---|---|---|
| 内核编排 | `include/neko/session.hpp`, `src/session.cpp` | Live++ 式 3 行接入：创建会话、`watch()`、每帧 `update()` |
| ELF 解析 | `backends/elf/object_file.*` | 最小 ELF64 relocatable 读取器（sections/symbols/rela，无外部依赖） |
| 进程符号 | `backends/elf/process_symbols.*` | `/proc/self/exe` 的 `.symtab`（`-no-pie`：链接期地址即运行期地址）；同时实现 `symbol_provider` + `state_manager` |
| 代码页 | `backends/elf/code_pages.*` | 就近 mmap（±2GB rel32 约束）、先写后改 RX、入口 `E9 rel32` 改写（带 `-O0` 序言模式校验） |
| 运行时迷你链接 | `backends/elf/loader.*` | 布局、符号三级解析（旧状态 > arena > 外部）、重定位应用、重定向清单 |
| 演示 | `examples/hello_reload/` | hot.cpp + runner + 一键脚本 + 预期输出；同时是 ctest 集成测试 |

## 接口形状验证结论（"验证四个接口的形状"交付物）

提案草案的四个接口经一次真实端到端实现检验，修订如下：

1. `reserve_code(bytes)` → **`reserve_code_near(hint, bytes)`**：
   5 字节 `jmp rel32` 只有 ±2GiB 射程，分配位置必须以被替换函数为提示。
2. 草案的 `code_substituter::relocate()` **移除**，新增第五个接口
   **`object_loader`**：重定位需要符号上下文（旧状态 / arena / 外部三级解析），
   本质是目标格式（ELF/PE）的工作，不属于"写代码 + 改入口"的抽象。
3. `symbol_provider` 增加 **`function_by_name` / `global_by_name`**：
   装载器按（mangled）名字匹配新旧符号，全量枚举无法支撑这个查询模式。
4. `state_manager::map_global(global_id)` → **`map_global(name)`**：
   数值句柄没有生产者；名字查找才是装载器真实需要的。
5. `patch_planner` 形状成立，Phase 1 实现为内核的 `trivial_planner`
   （每个改动文件 = 一个待重编 TU）；真实依赖图是 Phase 2。

`layout_of(type_id)` 保持草案原样，Phase 1 未行使（Phase 5 领地）。

## 机制层发现（写代码前不知道、写完才知道的事）

1. **外部函数必须走 arena 内建 PLT**：libc 距离新代码 ~127GB，`call rel32`
   够不着。装载器为每个被调用的未定义符号生成 10 字节
   `movabs rax, imm64; jmp rax` trampoline——这就是"运行时迷你链接"的核心一环。
2. **静态变量的访问被汇编器折叠成段符号引用**：`s_calls` 的读写不是
   `_ZL7s_calls + addend`，而是 `.bss + 0`（st_value 与 RIP 修正在 addend 里
   相互抵消）。装载器用"数据段锚点"解决：以段内有名字且有旧存储的符号反推
   段基址（`old_base = old_addr(sym) − sym.value`），锚点不一致即布局漂移，
   明确报错拒绝（顺带免费得到了布局变更检测）。
3. **symtab 0 号空符号必须保留**：重定位按符号表原始索引引用符号，
   解析时跳过 0 号会整体偏移一位（实测踩坑）。
4. **ASan 会平移全局变量布局**（redzone 插入），导致新旧布局漂移被锚点检查
   拦截。修正：sanitizer 编译标志 PRIVATE（只插桩库自身）+ 链接标志
   INTERFACE（消费方仅链接运行时）——工具库插桩、宿主 demo 的 TU 不插桩。
5. **`-O0` 序言有两种形态**：`55 48 89 E5`（push rbp; mov rbp,rsp）与 CET 的
   `F3 0F 1E FA 55`（endbr64; push rbp）。前者覆盖 5 字节会切断下一条指令的
   编码——安全的前提是 `-O0` 下函数前 8 字节内不会有内部跳转目标（循环标签
   出现在栈帧建立之后）。入口 patch 前校验两种模式 + `st_size ≥ 8`。
6. **`PLT32` 是调用、`PC32` 对未定义符号是数据引用**（playground demo 实测
   踩坑）：`fflush(stdout)` 中 `stdout` 编译为 `mov rdi,[rip+stdout]`
   （R_X86_64_PC32），若与调用一并路由到 trampoline，新代码会把 trampoline
   的机器码字节当 `FILE*` 解引用（`__GI__IO_fflush` 段错误）。规则修正为：
   只有 `PLT32` 走 trampoline；`PC32` 数据引用必须解析到**主程序内**的
   copy-relocation 拷贝（静态链接器早已把 `stdout` 等拷进可执行文件数据段，
   这是 rel32 唯一可达的地址；libc 里的原件在 0x7f... 区，超射程）。
   `hello_reload` 的 `tick()` 已加入 `fflush(stdout)` 作为该缺陷类的永久回归
   防线。
7. **重复热替换同一函数需要识别自己的跳转**（playground 用户实测发现）：
   第一次替换后入口已是 `E9 rel32`，序言校验会拒绝第二次 patch（"failed to
   patch entry"）。修复：`code_pages` 登记所有 arena 范围，入口若以 `E9` 开头
   且目标落在自有 arena 内，即可证明是我们上一次的重定向，安全改写。真正的
   `-O0` 序言不会以 jmp 开头，无人向我们的 arena 跳转。`run_demo.sh` 现做
   **两次**热替换（v1→v2→v3）作为该缺陷类的永久回归防线。

## 已知边界（Phase 1 明确剪枝，运行时报错信息可见）

| 边界 | 行为 |
|---|---|
| PIE 二进制 | 拒绝并提示（Phase 2 经由 load-base 检测支持） |
| 新增可变全局/静态 | 报错（无既有存储可映射；Phase 2） |
| 跨翻译单元引用（UND 且非 libc/已加载符号） | 报错（Phase 2） |
| `.rela.data`（数据内指针初始化） | 跳过（演示 TU 无此形态；Phase 2） |
| GOT 类重定位（`-fpic` 代码） | 报错并提示需要 `-fno-pic` |
| 多线程替换 | 未做（提案允许：单线程、固定时刻替换） |
| 回滚 | 未做 |

## 复现

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug                      # 2 tests: neko.smoke + neko.hello_reload
# 或直接看演示：
bash build/debug/examples/hello_reload/run_demo.sh
```

已在以下环境全绿（构建 + 2 项测试）：veLinux 2 (Debian 12) / GCC 12.2；
Clang 14.0.6；Clang + ASan/UBSan；Release。macOS 构建仅含内核（后端按设计
门控），smoke 通过。
