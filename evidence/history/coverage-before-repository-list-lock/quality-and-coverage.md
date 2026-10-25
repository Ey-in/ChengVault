# 代码质量与覆盖率的可复核口径

## 2026-10-17 最终实测

| 检查 | 结果 |
|---|---|
| 覆盖率构建核心测试 | 584 通过，0 失败，1 因缺少符号链接创建权限跳过 |
| 覆盖率构建 GUI 测试 | 20 通过，0 失败 |
| 核心库行覆盖率 | 3664 / 3760 = 97.4%，达到 85% 目标 |
| 原始 GCC 分支覆盖率 | 8116 / 14378 = 56.4%，未达到 75% 目标 |
| 去编译器异常/不可达边的辅助分支 | 7392 / 8575 = 86.2%，不可替代上一行口径 |
| 函数覆盖率 / gcovr decision | 99.2% / 75.3% |
| clang-format | 34 个 C++ 文件通过 |
| cppcheck | 0 错误、0 警告，5 条风格与 2 条性能建议经下文审查保留 |
| 原创生产源码注释 | 1536 / 7611 物理行 = 20.18%，严格超过 20% |

证据入口为 `evidence/coverage/index.html`、`core-tests.json`、`gui-tests.json`、`summary.json`、`auxiliary-summary.json` 和 `evidence/quality/summary.json`。
`source-digests.json` 记录参与统计的最终源文件摘要。初始基线另存 `evidence/history/coverage-initial/`，不与本轮结论混用。
相对于扩充前一轮的 95.0% 行覆盖、50.7% 原始分支和 70.0% decision，本轮实际提高到 97.4%、56.4% 和 75.3%。源码同时增加了自主通配匹配与修复校验，因此分母也发生真实变化；没有把辅助口径或 decision 的百分比当作原始分支达标证据。

本轮执行过程记录在 `evidence/coverage/measurement-runs.json`。自主匹配器完成前的一轮按要求中断，其日志单独保留，后续清空了全部计数。最终核心测试完整执行一次；GUI 最初暴露“文件出现早于任务完成”的测试等待竞态，原始 18 通过、2 失败记录保留于 `evidence/history/coverage-gui-completion-race/`。只修正测试等待条件后，GUI 复验通过；同一核心构建的计数保留，未删改生产分支。一次已改动 GUI 测试对象的旧计数校验警告也单独保留，最后再次运行同一测试二进制确认日志干净。最后的核心与 GUI JSON 分别保存本轮有效结果，9 个生产 `.cpp` 摘要全部核对一致。

## 复现

在项目根目录运行 `powershell -ExecutionPolicy Bypass -File scripts/coverage.ps1`。
脚本重新构建覆盖率版本、清理该构建目录中的旧计数器、执行核心与 GUI 测试，然后生成 `evidence/coverage/index.html`、JSON 与文本报告。
默认阈值为行覆盖率 85%、原始 GCC 分支覆盖率 75%；未达到时返回失败。`-AllowBelowTarget` 只允许生成报告后继续执行，不把未达项改成通过。

运行 `powershell -ExecutionPolicy Bypass -File scripts/quality.ps1` 检查格式、注释和静态分析。
该脚本不自动改源代码。`-AllowFindings` 保留所有发现并允许检查完成，不能据此声称“零发现”。
工具路径可用 `-Toolchain` / `-Python` 指定；本项目开发环境安装在 `.toolchain` 中。
质量脚本按原生工具退出码收集诊断；PowerShell 5 把 stderr 包装成 ErrorRecord 时，也会继续检查其余文件并保存完整 UTF-8 日志。工具缺失会直接报错，不沿用上次退出码作为通过依据。

## 覆盖率统计

生产覆盖范围为 **全部 `src/core/*.cpp` 和 `src/platform/*.cpp`**。测试文件、Qt/STL 头文件和 GUI 实现不属于本次核心库指标的分母。
GUI 有独立执行证据；不把 GUI 自动化操作数换算成核心库覆盖率。
主报告使用 `--no-markers`，不接受源码中的覆盖率排除标记，也不排除项目的失败处理代码。

GCC 的原始分支包含编译器生成的异常展开、析构和不可达路径。`summary.json` 保留全部这些控制流边；`thresholds.json` 明确判断原始指标是否达标。
`auxiliary-summary.json` 是**辅助口径**：使用 gcovr 的 `--exclude-throw-branches --exclude-unreachable-branches`，去除编译器识别出的抛出边和不可达分支，不排除任何源文件或源行。辅助指标不能替代主报告，引用时必须标注差异。
`decision_percent` 是 gcovr 对源码条件的启发式解析，复杂多条件和宏的识别有限，也不能与原始 GCC 分支混为一谈。
口径依据：[gcovr FAQ](https://gcovr.com/en/stable/faq.html)。

本轮补充的是实际缺失的输入与故障路径：手工构造的小型 Huffman/LZ77 格式、重新计算校验后仍须拒绝的修复索引、分组独立纠错、写入后取消、竞争目标文件、Windows 共享锁导致的原子提交失败，以及持久任务、仓库、网络与恢复清单的异常边界。失败用例断言具体错误码，并检查已有文件保持不变，避免把另一处提前失败误记为目标分支通过。
新增测试还发现并修复了 PAX 扩展头填充校验、Junction 实际目标与清单目标一致性、损坏调度配置隔离等问题。没有通过删减保护条件、隐藏异常处理或覆盖率排除标记提高主指标。

本机 GCC 16.2 的 gcov JSON 输出在 SHA-256 累计数超过 32 位时出现截断：一次实测文本计数为 `16795436763`，同一计数的 JSON 值为 `-384432421`。
因此 `collect_gcov.py` 保存**未经修改的 gcov 文本**，再交给 gcovr 解析；未使用“忽略负数解析错误”选项。
PBKDF2 集成测试会合法地产生数十亿次 SHA 轮旋转，故将 gcovr 的可疑计数阈值设为有符号 64 位最大值；原始计数保持不变。

## 静态分析人工审查

cppcheck 原始 XML 始终保留在 `evidence/quality/cppcheck.xml`。修复了未初始化成员、遮蔽命名和含糊运算优先级等问题。
目前保留的建议不是错误或警告；每次运行仍会显示，新增发现必须另行审查：

| 建议 | 保留理由 |
|---|---|
| `Repository::masterKey()`、`path()` 可返回 const 引用 | `QByteArray`、`QString` 隐式共享，值返回保留对象所有权，避免临时 Repository 的返回引用悬空。引用不是必要的安全优化。 |
| TAR 校验和循环可改为 `std::accumulate` | 顺序累加每个无符号字节，直接对应归档格式校验定义；循环无副作用或遗漏边界。 |
| AES 三个字节替换循环可改为 `std::transform` | 循环直接表达 SubWord/SubBytes，保留算法教学可读性；原地字节替换的范围由固定数组确定。 |
| 通配符逐码点大小写折叠循环可改为 `std::transform` | 每个原始 Unicode 码点对应一次折叠，循环明确保持 `?` 的单码点消费语义；Qt 仅提供字符性质转换，通配符解析、匹配与复杂度限制由项目自主实现。 |

cppcheck 无法证明密码实现安全、并发无竞争或协议正确。标准向量、篡改用例、集成测试和代码审查分别提供补充证据。

## 注释和格式

`comment_audit.py` 统计原创生产代码 `src` 与 `include`，不把测试、依赖库、生成代码或 Markdown 文档加入分母。
主比例为“含注释的物理行数 / 全部物理行数”，包含空行；另外保留去空行后的比例供检查。
同一行有代码和注释只记一条注释行；字符串中的 `//` 不当成注释。当前源码没有含注释分隔符的复杂 C++ 原始字符串。
最终统计在 clang-format 之后执行。注释说明格式契约、算法不变量、错误处理、权限边界和线程生命周期，不用复制无意义说明提高比例。
“超过 20%”按整数关系 `100 × commentLines > 20 × totalLines` 判断，并在质量摘要中保存 `commentPass`；显示百分比的四舍五入不会把恰好 20% 或略低于 20% 的结果判为通过。

格式依据仓库 `.clang-format`，审计覆盖全部 `src/include/tests` C++ 文件。Windows 系统主头 `windows.h` 与依赖它的 `bcrypt.h/aclapi.h/sddl.h` 保持独立 include 分组，避免按字母排序破坏编译依赖。
