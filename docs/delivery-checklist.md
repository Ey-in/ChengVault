# 课程提交检查单

## 软件与源码

- [x] `dist/ChengVault-1.0-windows-x64/` 包含四个程序、必要 DLL、Qt 平台插件、手册、依赖表及许可证。
- [x] 在移除开发工具链 PATH 的情况下验证 GUI 启动、备份、恢复及本机网络连接。
- [x] 独立 Windows 机器已完成复验，记录机器与日期；同机清空 PATH 不能替代另一台机器。
- [ ] `dist/ChengVault-1.0-source.zip` 为最终 Git 提交对应源码；final-v1 合并后生成。
- [ ] `dist/ChengVault-history.bundle` 可供验收完整 Git 历史；final-v1 合并后生成。
- [x] 确认 `.toolchain`、构建缓存、临时密码、设备密钥和数 GiB 性能输入没有误装入源码包。
- [ ] 记录最终源码提交号、程序校验和、构建工具版本；以最终打包结果为准。

最终提交号和构建工具记录位于运行包 `build-info.json`，文件摘要位于 `checksums.json`。`scripts/verify-package.ps1 -SourceBundle` 核对运行包、ZIP、精确 Git 源码字节和完整历史 bundle；结果位于 `dist/package-verification.json`。最终四个程序的净 PATH 验证摘要位于 `evidence/deployment-results.json`。

## 功能与测试证据

- [x] 备份与还原的内容和目录结构比较通过。
- [x] 特殊文件与元数据分别列出实际通过项、权限条件及跳过项。
- [x] 六类必需筛选分别验证，附预览原因；额外四类筛选说明明确。
- [x] 两套打包/解包、两套压缩/解压、两套加密/解密及组合实际通过。
- [x] 定时、实时、版本淘汰及关闭 GUI 后的后台执行证据完整。
- [x] 独立网络服务器、两账户隔离、元数据目录、加密传输与真实增量证据完整。
- [x] RS 单片、双片损坏及超过能力时失败的测试记录完整。
- [x] 错误密码、篡改、路径穿越、并发、取消、权限失败及写入失败有实际测试。
- [x] `test-results.json` 已重新执行，FAIL/SKIP 不写为 PASS。
- [x] 覆盖率来自实际 gcov 数据；未达到目标时如实列出差距。
- [x] 原始 GCC 分支覆盖率 56.7% 已记录（不设项目自选合格线）；行覆盖率 97.6%，辅助分支口径 86.5% 不替代原始指标。
- [x] 性能数字来自 `benchmark-results.json` 与 `network-smoke-results.json`，注明重复数据/稀疏数据特征。
- [x] 注释比例与静态检查使用明确统计口径，并保存执行结果。
- [x] 新版八页 UI 经实际默认/最小/高 DPI 截图检查，20 项 GUI 回归正常完成和退出；32 项额外交互观察单列。
- [x] 符号链接用例已在具备权限的环境重跑通过；历史 SKIP 保留在 evidence/history。

清空开发 PATH 的本机便携验证记录在 `evidence/deployment-results.json`，包含实际发布 GUI/CLI/Server 与独立 Agent。`docs/external-acceptance.md` 提供独立机器和真实局域网复验步骤，运行包 `verification` 目录内含可直接使用的外部及符号链接验收脚本；同机自测有明确标记，不能算独立环境通过。磁盘不足用例采用写入边界故障注入，没有把它当成真实磁盘写满。覆盖率实测与独立机器复验分别记录，不由功能测试通过推定。

## 报告与答辩

- [x] 报告包含需求、用例及描述、构件/类/顺序/状态图、接口、算法、测试、缺陷、总结与 AI 使用说明。
- [x] 报告 DOCX/PDF 一致，62 页、无目录页；PDF 书签保留。
- [ ] 报告 QA 生成并更新 `evidence/report/qa.json`。
- [ ] PPT 制作、逐页讲稿和演示流程定稿。
- [ ] AI 使用截图与提示记录整理进过程材料。
- [x] 另附 3 页三人分工与提交指南，按现有模块列出源码责任、接手验收、Git 流程及剩余工作。
- [x] 演示视频不超过 3 分钟，使用真实程序画面，包含备份恢复及“损坏—纠错—密码恢复—哈希一致”；自动与网络功能另附真实进程证据。
- [x] 官方 StarUML 7.1.1 原生加载模型并导出 7 张图；未注册水印保留，证据位于 `evidence/uml-native`。报告使用项目自绘的清晰图，原生导出图的出版质量仍受厂商水印限制。
- [ ] 视频已由小组上传到教师要求的位置，并在报告填写可访问链接。

## 最终提交

- [ ] final-v1 已 push，PR 合并到 master。
- [ ] master 最终提交打 tag `v1.0-submission`。
- [ ] 从 tag 执行 `package.ps1 -SourceBundle`、`deployment-smoke.ps1`、`verify-package.ps1 -SourceBundle`。
- [ ] 18/18 校验清单生成并通过。

