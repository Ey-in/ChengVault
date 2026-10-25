# 澄匣备份需求与评分追踪

需求以课程 0.pdf 及两份教师补充说明为基准；分数由教师认定，本文不预记得分。

| 编号 | 能力 | 验收条件 | 主要模块 |
|---|---|---|---|
| B01 | 目录树备份 | 保留普通文件、空文件、空目录、中文路径及层次结构 | filesystem.cpp |
| B02 | 指定位置恢复 | 恢复字节一致，默认不覆盖，失败保护已有文件 | filesystem.cpp |
| E01 | NTFS 特殊文件 | 符号链接、硬链接、Junction、稀疏文件、文件及目录 ADS | filesystem.cpp |
| E02 | NTFS 元数据 | FILETIME、属主和组 SID、DACL 及继承、普通属性 | filesystem.cpp |
| E03 | 路径筛选 | 包含和排除相对路径；排除优先 | filesystem.cpp |
| E04 | 类型筛选 | 对象类型与扩展名组合 | filesystem.cpp |
| E05 | 名字筛选 | 名字通配符与祖先目录保留 | filesystem.cpp |
| E06 | 时间筛选 | 创建 访问 修改时间的包含式区间 | filesystem.cpp |
| E07 | 尺寸筛选 | 逻辑字节数上下界均包含边界 | filesystem.cpp |
| E08 | 用户筛选 | 文件属主 SID 或 Windows 账户名 | filesystem.cpp |
| E09 | 额外筛选 | 属性、深度、UTF8 内容、相对快照变化 | filesystem.cpp |
| E10 | 索引打包解包 | 内容块索引、去重、独立单文件完整恢复 | archive.cpp |
| E11 | PAX 打包解包 | 真实 TAR 成员与 NTFS 扩展，独立读取器验证 | archive.cpp |
| E12 | Huffman 压缩解压 | 自主前缀编码，频数与边界校验 | compression.cpp |
| E13 | LZ77 压缩解压 | 自主滑动窗口匹配与重叠回溯 | compression.cpp |
| E14 | AES256 加密解密 | 自主 AES256 CTR 与 HMAC 认证，用户指定密码 | crypto.cpp |
| E15 | ChaCha20 加密解密 | 自主 ChaCha20 与 HMAC 认证，拒绝篡改 | crypto.cpp |
| E16 | GUI | 八页中文界面，异步耗时操作、进度及错误信息 | main_window.cpp |
| E17 | 定时及淘汰 | 间隔 每日 每周任务、漏跑补偿、版本保留 | scheduler.cpp |
| E18 | 实时备份 | Windows 事件、合并去抖、增量提交 | scheduler.cpp |
| E19 | 网盘式备份 | 独立服务端、上传、列举、下载并恢复 | network.cpp |
| E20 | 用户管理 | 创建 禁用 密码重置 配额 设备授权与撤销 | network.cpp |
| E21 | 远程元数据 | 用户隔离索引；文件级清单保持加密 | network.cpp |
| E22 | 加密传输 | 设备预共享密钥认证、方向密钥、序号防重放 | network.cpp |
| E23 | 增量及续传 | 只发送缺失密文对象；中断按偏移续传 | network.cpp |
| E24 | 冗余纠错扩展 | 自主 RS12 10，单组至多两个已定位分片损坏可修复 | repair.cpp |
| Q01 | 健壮性和安全 | 越界、路径穿越、错误密码、取消、占用均明确失败 | filesystem.cpp |
| Q02 | 可构建与可维护 | CMake Ninja 自动构建，模块化、命名及原创注释 | CMakeLists.txt |
| Q03 | 真实验收证据 | 白盒测试及至少八类测试，证据可重跑 | test_main.cpp |
| D01 | 课程完整交付 | 源码 程序 构建脚本 模板报告 PPT 视频 UML | docs |

## 自主实现边界

Qt/Win32 提供控件、文件与网络I/O、线程、系统事件和随机数。归档、压缩、密码原语及认证封装、快照增量、调度协议、纠错有限域算法由项目源码实现。DPAPI只保护当前用户本机凭据，不替代备份内容加密。

## 必须保留的真实限制

NTFS不映射Linux FIFO或设备节点。符号链接依赖系统权限；SKIP不得写作PASS。严格元数据权限不足失败，宽松模式告警。变化筛选基于元数据。通配解析和迭代匹配自主实现，支持* ? []范围及取反，使用逐码点Unicode simple fold；畸形集合和操作预算超限明确拒绝。纠错每组最多两个已定位损坏片，不代表任意20%损坏可恢复。网络预共享密钥协议没有前向保密。

## 人工课程材料

姓名学号、成员提交、lg.team活动、证书、考勤、答辩参与和视频上传链接由小组真实补充。
