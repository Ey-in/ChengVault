# ChengVault UML 模型与报告插图

`ChengVault.mdj` 是含语义模型、关系和视图的 StarUML JSON 工程；并非把 PNG 放进工程冒充模型。包含用例、构件、类、三个顺序图和任务状态图，共 7 张图。每张图均提供同名 SVG 和 PNG，可插入报告与答辩材料。

| 图 | 对应实现 |
|---|---|
| 01-use-cases | GUI / CLI 中的备份、还原、自动任务、网盘、修复和管理员功能 |
| 02-components | 独立客户端、CLI、Agent、Server 与核心/平台模块 |
| 03-core-classes | include/chengvault 中真实的类、结构体和公共接口 |
| 04-backup-sequence | archive.cpp：捕获暂存 → 生成内层归档 → 压缩/加密外层 → 提交 |
| 05-restore-sequence | archive.cpp：认证解压内层临时文件 → 完整性校验 → fs::restore |
| 06-network-sequence | network.cpp：设备认证、登录、清单比对、偏移续传、提交 |
| 07-task-state | scheduler.cpp：waiting / running / retrying 和 enabled 标志 |

类图中的列表为阅读摘要；MDJ 语义层将数据成员与操作分别建为 UMLAttribute、UMLOperation。状态图的“禁用”表示 enabled=false，并非虚构一个持久化 state 字符串。自动任务失败重试和事件去抖的具体时间参数以源码为准。

重新生成：

```powershell
python docs/uml/generate_uml.py
powershell -NoProfile -ExecutionPolicy Bypass -File docs/uml/render_png.ps1
```

Python 仅依赖标准库。PNG 渲染使用 Windows 自带 System.Drawing，SVG 为独立矢量文件。生成器检查全部 ID 唯一、引用可解析、父关系无环和主视图坐标边界；检查结果保存于 `model-validation.json`。7 张 PNG 已在实际生成后逐图查看，并修正文字编码、折线标签和用例连线重叠。

模型/视图字段参考 [StarUML 官方文档](https://docs.staruml.io/user-guide/basic-concepts) 和官方 schema 示例；项目名称、类、关系、布局及文字均依据 ChengVault 原创设计生成，没有复用示例业务模型或 Aegis 材料。

已使用官方签名有效的 StarUML 7.1.1 原生加载本模型，并通过官方 API 读取全部 7 图、导出 7 张 PNG。执行前以无窗口子进程验证独立 Windows 桌面，应用仅在非活动桌面启动，未调用 SwitchDesktop；结束后关闭本次进程树、桌面并恢复原设置。最终证据 `evidence/uml-native/validation.json` 绑定实际模型 SHA-256、原生程序签名、图类型和各 PNG 摘要。

逐图目视复核发现未注册版本导出包含密集 `UNREGISTERED` 水印，影响连线阅读，因此只认定原生加载/渲染成功，不认定这些原生 PNG 为可出版插图，也未移除水印或修改许可。正式报告继续使用本目录原创 SVG/PNG。顺序图的自动编号曾与手工步骤号叠加，现按官方元模型设置 `showSequenceNumber=false`，保留与报告一致的单一编号；原生复导出已确认修正。

`model-validation.json` 和 `schema-validation.json` 中的 `nativeStarUmlOpenVerified=false` 是静态检查器执行范围说明：生成器不会把静态校验伪装成原生验证。最终原生结论以独立的 `evidence/uml-native/validation.json` 为准。可用 `scripts/native-staruml-check.ps1 -ProbeOnly` 先复核隔离，再省略该开关运行原生测试；未确认隔离时脚本不会启动 StarUML。

最终静态复审另外读取官方 [StarUML 元模型](https://github.com/staruml/staruml-samples/blob/main/StarUML-metamodel.mdj)，沿类继承与关联端推导允许字段及引用类型。发现并修复了初始化伪状态的标签类型（应为 `NodeLabelView`）和不适用的 `wordWrap` 字段。修复后 56 种模型/视图类型、1126 个对象、4241 个关联字段类型检查全部通过；3116 条引用可解析。结果与输入摘要记录在 `schema-validation.json`，不等同于原生应用渲染验证。

复现附加审查时，把官方元模型下载到任意本地路径，再执行：

```powershell
python docs/uml/validate_schema.py --metamodel .cache/staruml-official-metamodel.mdj
```

该审查仅把官方 MDJ 当作 schema 输入，不复用其示例业务模型。修复未改变独立 SVG/PNG 的内容和布局，因此未重新渲染图像。
