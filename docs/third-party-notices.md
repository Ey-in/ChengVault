# 基础设施依赖与原创范围

ChengVault 的归档、压缩、密码、增量、筛选、调度、网络业务协议与纠错核心逻辑由项目代码实现。Aegis 只作为课程产品调研背景，不作为源码、界面或报告复制来源。

| 依赖 | 使用范围 | 不承担的核心功能 |
|---|---|---|
| C++ 标准库、GCC/UCRT 运行时 | 容器、异常、线程和基础运行环境 | 不提供备份算法 |
| Qt Core | 字符串、JSON、路径、原子文件提交、进程与事件循环 | 不调用 QCryptographicHash 或 qCompress 实现备份算法 |
| Qt Widgets / GUI | 中文桌面界面、字体和图像显示 | 不替代业务实现 |
| Qt Network | TCP 套接字 | 不调用 TLS/SSL 直接实现传输加密 |
| Qt Concurrent | GUI 后台任务 | 不实现备份调度规则 |
| Windows API | 文件类型/元数据访问、文件事件、随机源、DPAPI 本机凭据保护 | 不替代备份加密、压缩或纠错 |

发布脚本使用 `windeployqt6` 收集必要 Qt 基础设施，并递归解析所有程序、插件和 DLL 的 PE 导入表。它排除 TLS、SQL、网络信息等非必要插件，并拒绝部署 `libssl`、`libcrypto`。`runtime-dependencies.csv` 为实际导入关系，`runtime-packages.json` 为对应软件包版本，`licenses/` 保留这些包的原始许可文本。

Qt 的二进制基础设施会间接依赖 zlib、zstd、Brotli、字体、图片和 Unicode 支持库。这些依赖不被 ChengVault 的打包、压缩、加密和纠错接口调用。评审可检查 `src/core/compression.cpp`、`crypto.cpp`、`repair.cpp` 及真实 import 图，避免把“运行目录存在 Qt 的传递依赖 DLL”误当作使用其直接完成扩展算法。

项目使用动态链接的 Qt 6 开源构建，并保留可替换的 Qt DLL、完整应用源码和构建脚本。Qt 对应原始许可位于 `licenses/qt6-base/`；Qt 项目源代码与版本资料见 [Qt 官方项目](https://code.qt.io/cgit/qt/qtbase.git/)，本发行包使用的 MSYS2 构建配方见 [MSYS2 Qt6 Base 包](https://github.com/msys2/MINGW-packages/tree/master/mingw-w64-qt6-base)。实际版本以运行包记录为准。

原始软件包中的第三方版权和许可文本不属于 ChengVault 自主代码。项目自身的成员署名与对外授权由真实作者确定，见 `PROJECT_NOTICE.txt`。
