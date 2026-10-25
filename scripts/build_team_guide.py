"""Build the three-person collaboration guide without modifying the course report.

Run with the bundled Python runtime. This script authors DOCX and a matching CSV;
rendering and visual review are separate, explicit release steps.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4

from docx import Document
from docx.enum.table import WD_TABLE_ALIGNMENT, WD_CELL_VERTICAL_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Cm, Pt, RGBColor

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT.parent / '报告文档' / 'ChengVault三人分工与提交指南.docx'
HEADERS = ['任务编号', '负责人', '复核人', '阶段', '具体工作', '模块代码文件', '交接产物', '验收条件', '状态']
TASKS = [
    ['A01','成员A 待填','成员B','初始模块搭建','负责GUI与CLI交互 基本备份恢复 NTFS类型和元数据 全部筛选 两种归档','src/gui/main_window.cpp;src/gui/main_window.hpp;src/gui/theme.cpp;src/gui/theme.hpp;src/apps/gui_main.cpp;src/apps/cli_main.cpp;src/platform/filesystem.cpp;src/core/archive.cpp;include/chengvault/filesystem.hpp;include/chengvault/archive.hpp'],
    ['B01','成员B 待填','成员C','初始模块搭建','负责两种压缩 两种密码及认证派生 RS纠错 公共安全基础和算法测试','src/core/compression.cpp;src/core/crypto.cpp;src/core/repair.cpp;src/core/common.cpp;include/chengvault/compression.hpp;include/chengvault/crypto.hpp;include/chengvault/repair.hpp;include/chengvault/common.hpp;tests/algorithms_tests.cpp;tests/test_main.cpp;tests/test_support.hpp'],
    ['C01','成员C 待填','成员A','初始模块搭建','负责版本仓库 定时实时与后台 网络用户和设备管理 传输与增量 发布','src/core/repository.cpp;src/core/scheduler.cpp;src/core/network.cpp;src/apps/agent_main.cpp;src/apps/server_main.cpp;include/chengvault/repository.hpp;include/chengvault/scheduler.hpp;include/chengvault/network.hpp',],
    ['A02','成员A 待填','成员B','补充提交','补充A的测试；实际复现问题后改进交互或恢复边界并附回归','tests/filesystem_tests.cpp;tests/filesystem_adversarial_tests.cpp;tests/gui_smoke.cpp'],
    ['B02','成员B 待填','成员C','补充提交','从实际覆盖报告补有独立预期的安全边界测试，发现缺陷再修复','tests/algorithms_tests.cpp;scripts/coverage.ps1;scripts/collect_gcov.py;scripts/quality.ps1;scripts/comment_audit.py'],
    ['C02','成员C 待填','成员A','补充提交','补充调度网络测试，复验后修真实诊断或并发问题并附回归','tests/scheduler_tests.cpp;tests/background_process.cpp;tests/network_repository_tests.cpp;tests/network_repository_cases.hpp'],
    ['A03','成员A 待填','成员C','剩余验收','在新UI与第二台独立Windows复验基本备份恢复和部署','scripts/deployment-smoke.ps1;scripts/validate-ui.ps1'],
    ['A05','成员A 待填','成员B','剩余验收','获授权并具备权限后复验符号链接与严格元数据，B复核权限安全','scripts/symlink-acceptance.ps1;scripts/metadata-time-smoke.ps1'],
    ['C03','成员C 待填','成员B','剩余验收','组织两台真实Windows机器LAN服务端客户端验收','scripts/external-acceptance.ps1;scripts/network-smoke.ps1;docs/external-acceptance.md'],
    ['B04','成员B 待填','成员A','剩余验收','按最终成员改动重测覆盖和回归，如实保留原始分支未达75%的差距','scripts/coverage.ps1;scripts/collect_gcov.py;docs/quality-and-coverage.md'],
    ['C04','成员C 待填','成员B','交付维护','负责构建测试和发布入口，复验后台与淘汰后剩余版本','CMakeLists.txt;scripts/bootstrap.ps1;scripts/build.ps1;scripts/test.ps1;scripts/package.ps1;scripts/verify-package.ps1'],
    ['A04','成员A 待填','成员B','文档维护','负责需求设计和界面文档；集成真实审查后的分支并同步图文','scripts/build_report.py;docs/requirements.md;docs/manifest.md;docs/ntfs-support.md;docs/uml'],
    ['B05','成员B 待填','成员C','文档维护','负责算法测试安全论述与追踪关系，准备代码问答','docs/algorithms.md;docs/traceability.csv;docs/quality-and-coverage.md'],
    ['C05','成员C 待填','成员A','交付维护','统筹PPT视频和最终交付；三人各写本人模块讲稿','scripts/build_presentation.mjs;scripts/record-demo.ps1;docs/delivery-checklist.md'],
    ['ALL01','A B C 各自','三人交叉复核','个人记录','本人账号克隆分支提交 维护lg.team 填身份 补证书考勤AI说明','docs/team-work-plan.csv;各自真实任务和评审记录'],
    ['ALL02','A B C 各自','三人交叉复核','答辩','各讲已分配模块，互问完整恢复链路和本人真实修改','各自模块源码 测试与分段讲稿'],
]


def read_json(relative):
    return json.loads((ROOT / relative).read_text(encoding='utf-8-sig'))


def style_run(run, size=10.5, bold=False, font='Microsoft YaHei', color='000000'):
    run.font.name = font
    run.font.size = Pt(size)
    run.bold = bold
    run.font.color.rgb = RGBColor.from_string(color)
    fonts = run._element.get_or_add_rPr().get_or_add_rFonts()
    fonts.set(qn('w:eastAsia'), 'Microsoft YaHei')
    return run


def paragraph(doc, text, size=10.5, after=6, bold=False):
    p = doc.add_paragraph()
    p.paragraph_format.space_after = Pt(after)
    p.paragraph_format.line_spacing = 1.15
    p.paragraph_format.widow_control = True
    style_run(p.add_run(text), size, bold)
    return p


def heading(doc, text, level=1):
    p = doc.add_paragraph(style=f'Heading {level}')
    style_run(p.add_run(text), 16 if level == 1 else 12, True)
    p.paragraph_format.space_before = Pt(10 if level == 2 else 0)
    p.paragraph_format.space_after = Pt(7)
    p.paragraph_format.keep_with_next = True
    return p


def code(doc, text):
    p = doc.add_paragraph()
    p.paragraph_format.space_before = Pt(3)
    p.paragraph_format.space_after = Pt(7)
    p.paragraph_format.line_spacing = 1.05
    p.paragraph_format.keep_together = True
    p.paragraph_format.left_indent = Cm(.18)
    p.paragraph_format.right_indent = Cm(.18)
    shade = OxmlElement('w:shd'); shade.set(qn('w:fill'), 'F2F4F6')
    p._p.get_or_add_pPr().append(shade)
    style_run(p.add_run(text), 9, font='Consolas')
    return p


def table(doc, titles, rows, widths, size=9.5):
    t = doc.add_table(rows=1, cols=len(titles))
    t.alignment = WD_TABLE_ALIGNMENT.CENTER
    t.autofit = False
    for column, width in zip(t.columns, widths):
        column.width = Cm(width)
    borders = OxmlElement('w:tblBorders')
    for edge in ['top','left','bottom','right','insideH','insideV']:
        b = OxmlElement('w:'+edge)
        b.set(qn('w:val'),'single'); b.set(qn('w:sz'),'5'); b.set(qn('w:color'),'D9D9D9')
        borders.append(b)
    t._tbl.tblPr.append(borders)
    margins = OxmlElement('w:tblCellMar')
    for edge, value in [('top',95),('bottom',95),('left',95),('right',95)]:
        e = OxmlElement('w:'+edge); e.set(qn('w:w'),str(value)); e.set(qn('w:type'),'dxa'); margins.append(e)
    t._tbl.tblPr.append(margins)
    for index, values in enumerate([titles] + list(rows)):
        row = t.rows[0] if index == 0 else t.add_row()
        row._tr.get_or_add_trPr().append(OxmlElement('w:cantSplit'))
        if index == 0:
            row._tr.get_or_add_trPr().append(OxmlElement('w:tblHeader'))
        for cell, value, width in zip(row.cells, values, widths):
            cell.width = Cm(width)
            cell.vertical_alignment = WD_CELL_VERTICAL_ALIGNMENT.CENTER
            p = cell.paragraphs[0]
            p.paragraph_format.space_after = Pt(0)
            p.paragraph_format.line_spacing = 1.1
            style_run(p.add_run(str(value)), size, index == 0, color='FFFFFF' if index == 0 else '000000')
            fill = '203B50' if index == 0 else ('F1F5F8' if index % 2 == 0 else 'FFFFFF')
            shade = OxmlElement('w:shd'); shade.set(qn('w:fill'),fill); cell._tc.get_or_add_tcPr().append(shade)
    paragraph(doc, '', 3, after=1)
    return t


def new_page(doc, title):
    doc.add_page_break()
    heading(doc, title)


def write_plan(reset_plan=False):
    """Preserve members' updates by default; resetting requires an exact backup."""
    intended_docs = ROOT.resolve() / 'docs'
    if intended_docs.resolve() != intended_docs:
        raise ValueError('Refusing to write a task plan through a redirected docs directory')
    csv_path = intended_docs / 'team-work-plan.csv'
    if csv_path.resolve() != csv_path or csv_path.resolve().parent != intended_docs:
        raise ValueError('Task plan must resolve directly inside the project docs directory')
    if csv_path.exists() and not reset_plan:
        return {'written':False,'reason':'Existing task plan retained','path':str(csv_path)}
    backup = None
    if csv_path.exists():
        # Keep the exact prior bytes, including member identities and completed
        # work. Exclusive creation prevents a backup from replacing another one.
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        backup = intended_docs / f'team-work-plan.backup-{stamp}-{uuid4().hex[:8]}.csv'
        if backup.resolve().parent != intended_docs:
            raise ValueError('Task plan backup escaped the project docs directory')
        with backup.open('xb') as f:
            f.write(csv_path.read_bytes())
    with csv_path.open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.writer(f); writer.writerow(HEADERS); writer.writerows(TASKS)
    return {'written':True,'path':str(csv_path),'backup':str(backup) if backup else None}


def build(reset_plan=False):
    coverage = read_json('evidence/coverage/summary.json')
    auxiliary = read_json('evidence/coverage/auxiliary-summary.json')
    doc = Document()
    section = doc.sections[0]
    section.page_width = Cm(21); section.page_height = Cm(29.7)
    section.top_margin = Cm(1.8); section.bottom_margin = Cm(1.7)
    section.left_margin = Cm(2); section.right_margin = Cm(2)
    section.footer_distance = Cm(.7)
    for name in ['Normal','Title','Subtitle','Heading 1','Heading 2']:
        s = doc.styles[name]
        s.font.name = 'Microsoft YaHei'; s.font.color.rgb = RGBColor(0,0,0)
        s._element.get_or_add_rPr().get_or_add_rFonts().set(qn('w:eastAsia'),'Microsoft YaHei')
        # The bundled default Title style contains a blue bottom rule. Remove
        # the inherited decoration so hierarchy is conveyed by type and space.
        for border in s._element.xpath('./w:pPr/w:pBdr'):
            border.getparent().remove(border)
    doc.styles['Normal'].font.size = Pt(10.5)
    footer = section.footer.paragraphs[0]; footer.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    style_run(footer.add_run('ChengVault  三人协作指南   '), 8)
    field = OxmlElement('w:fldSimple'); field.set(qn('w:instr'),'PAGE'); footer._p.append(field)
    doc.core_properties.title = 'ChengVault 三人分工与提交指南'
    doc.core_properties.author = ''
    doc.core_properties.subject = '模块分工 复核与提交记录 课程交付'

    p = doc.add_paragraph(style='Title'); style_run(p.add_run('ChengVault\n三人分工与提交指南'), 24, True)
    p.paragraph_format.space_after = Pt(10)
    paragraph(doc, '模块分工、复核与个人提交', 12, after=12)
    paragraph(doc, '将 ChengVault 项目分为三块：A 负责客户端、文件系统、归档和集成，B 负责压缩、密码、纠错、安全和测试，C 负责仓库、自动任务、网络和交付。本文明确每人的代码范围、复核标准、答辩内容和真实 Git 记录方式；剩余补验放在后部。', 11)
    table(doc, ['成员','姓名和学号','主责及交叉复核'], [
        ['A','姓名 待填\n学号 待填','客户端 文件系统 归档 集成\n复核 C 的代码和交付'],
        ['B','姓名 待填\n学号 待填','算法 安全 测试\n复核 A 的代码和界面'],
        ['C','姓名 待填\n学号 待填','自动任务 网络 交付\n复核 B 的算法和测试'],
    ], [1.4,4.4,11.2], 10)
    heading(doc, '责任归属与历史作者分别记录', 2)
    paragraph(doc, '仓库包含 C++20 与 Qt 6 软件、自动化证据、模板报告、答辩材料和构建脚本。成员按模块负责整理、复核与提交；不写无法证实的“从零开发”。姓名学号及认领日期由本人填写；具体贡献依据本人实际差异、测试和评审填写。')
    paragraph(doc, f'当前核心行覆盖率为 {coverage["line_percent"]:.1f}%，原始 GCC 分支覆盖率为 {coverage["branch_percent"]:.1f}%，尚未达到原计划 75% 的分支目标。辅助口径为 {auxiliary["branch_percent"]:.1f}%，不能替代原始分支值。独立 Windows 机器、真实局域网及符号链接授权环境仍需小组补验。')
    paragraph(doc, '本轮新界面的本机复验由项目更新过程完成；三人仍应在自己的运行环境复验，并理解每项操作背后的代码。最终状态以 evidence 下对应版本的真实结果为准。课程得分与扩展认定由教师决定。')

    new_page(doc, '一 模块由谁负责')
    paragraph(doc, '下列路径均相对项目根目录。这是模块维护、验收与讲解责任表。公开接口或跨模块改动由双方讨论，不能各自修改同一契约后直接合并。', 10)
    table(doc, ['成员','现有源码与接口','配套测试'], [
        ['A', 'src/gui/main_window.cpp 和 .hpp\nsrc/gui/theme.cpp 和 .hpp\nsrc/apps/gui_main.cpp 与 cli_main.cpp\nsrc/platform/filesystem.cpp\nsrc/core/archive.cpp\ninclude/chengvault/filesystem.hpp 与 archive.hpp', 'tests/gui_smoke.cpp\ntests/filesystem_tests.cpp\ntests/filesystem_adversarial_tests.cpp'],
        ['B', 'src/core/compression.cpp\nsrc/core/crypto.cpp\nsrc/core/repair.cpp\nsrc/core/common.cpp\ninclude/chengvault/ 下同名 .hpp', 'tests/algorithms_tests.cpp\ntests/test_main.cpp\ntests/test_support.hpp'],
        ['C', 'src/core/repository.cpp\nsrc/core/scheduler.cpp\nsrc/core/network.cpp\nsrc/apps/agent_main.cpp 与 server_main.cpp\ninclude/chengvault/ 下 repository.hpp、scheduler.hpp、network.hpp', 'tests/scheduler_tests.cpp\ntests/background_process.cpp\ntests/network_repository_tests.cpp\ntests/network_repository_cases.hpp'],
    ], [1.5,9.3,6.2], 9.1)
    heading(doc, '已有功能的责任边界', 2)
    paragraph(doc, 'A：GUI 和 CLI、基本备份还原、全部筛选、自主通配匹配、NTFS 特殊类型与元数据、索引归档和 PAX 打包解包。A 统筹需求、设计、界面截图及集成，由 B 复核。', 10)
    paragraph(doc, 'B：Huffman 与 LZ77 压缩解压、AES256 与 ChaCha20 加解密、SHA256/HMAC/PBKDF2/HKDF、RS 冗余和修复、安全共用代码及算法测试。B 统筹覆盖率与质量复核，由 C 复核。', 10)
    paragraph(doc, 'C：完整快照与块增量、定时实时备份、后台、网络服务器、账户与设备、认证传输、配额和淘汰。C 统筹 CMake、构建测试打包脚本、PPT 视频与交付，由 A 复核。', 10)
    paragraph(doc, '精确脚本和文档清单见 docs/team-work-plan.csv。common 接口由 B 主责；GUI 对接仓库或调度由 A 与 C 会审；文档各写本人模块，A 汇总报告、C 汇总演示，不能由一人代写三人的经历。', 9.5)

    new_page(doc, '二 本人工作与提交')
    table(doc, ['成员','必须理解并能现场回答','本人工作与验收'], [
        ['A','筛选预览如何进入 capture？解密 解压 解包顺序如何安排？为什么目录权限最后设置？如何拒绝路径逃逸并避免覆盖已有文件？','从按钮定位核心函数；恢复中文路径、空目录、ADS、硬链接和稀疏文件；比较内容和元数据，保留权限限制。'],
        ['B','Huffman频数表与LZ77重叠引用如何解码？认证先于解密输出为何重要？标准向量为何比仅往返更可靠？RS为何每组最多两个已定位坏片？','复现密码标准向量、畸形压缩输入和错密码；说明RS的12种单片与66种双片测试，并核对真实覆盖报告。'],
        ['C','GUI退出后谁执行任务？漏跑 重试与去抖如何工作？重放如何拒绝？重复块为何不再上传？列表和淘汰为何需要同一仓库锁？','运行后台、仓库和网络用例；对比首次重复上传与局部修改；验证关闭GUI后的新快照及淘汰后版本。'],
    ], [1.1,8.4,7.5], 9.5)
    heading(doc, '个人 Git 记录怎样产生', 2)
    paragraph(doc, '先阅读并验证自己的模块，再对真实问题或缺口做小步改动。A 可改经复现的错误提示或恢复边界并补测试；B 可补一个有独立预期的未覆盖安全分支，发现缺陷再修；C 可改经复现的后台或网络诊断、并发行为并补回归。已满足要求的功能不为凑数重写。')
    heading(doc, '每次交接留下可复核记录', 2)
    code(doc, '任务 A02  名称 待填写实际问题\n负责人 A  复核人 B\n分支 feature/member-a/restore-feedback\n提交ID 待本人实际提交后填写\n输入 步骤 预期 实际结果 证据路径 待填写\n未解决事项与复核意见 待同伴重现后填写')
    paragraph(doc, '表格中的“模块现状”描述软件范围，“本人工作”描述成员的整理、复核与提交；不写无法证实的“从零开发”。三人各自更新 CSV 与 lg.team，记录本人执行和评审，不让一人替全组制造过程。', 10)

    new_page(doc, '三 建立小组公开仓库')
    paragraph(doc, '以下是 PowerShell 命令示例，文档不会执行上传。由 B 创建 GitHub 公开仓库并邀请 A/C；建议目标为空仓库，避免自动 README 产生无关历史。地址、姓名、邮箱均由本人提供；示例目录须不存在或确认为自己的新目录。')
    heading(doc, '由 B 创建空仓库', 2)
    code(doc, "GitHub -> New repository -> ChengVault -> Public -> 不勾选 README/.gitignore -> Create\nSettings -> Collaborators -> 邀请 A/C")
    heading(doc, '成员克隆并配置身份', 2)
    code(doc, "$teamRemote = Read-Host '粘贴小组公开仓库HTTPS地址'\ngit clone $teamRemote 'D:\\Codes\\soft-project\\ChengVault-team'\nSet-Location 'D:\\Codes\\soft-project\\ChengVault-team'\ngit config --local user.name '真实姓名'\ngit config --local user.email 'GitHub邮箱或noreply'\ngit switch master")
    paragraph(doc, '仓库为 Public 时，首次 push 前先检查 evidence 和 scripts 是否含主机名、SID、用户名或绝对用户路径；公开后这些内容会留在 Git 历史里。')
    heading(doc, '参考与项目入口', 2)
    paragraph(doc, 'Git 官方手册：https://git-scm.com/docs/git-config；https://git-scm.com/docs/git-merge。', 9.5)
    new_page(doc, '四 每个人的分支 提交与评审流程')
    paragraph(doc, 'A、B、C 各自 clone 小组仓库到独立目录。下面用 A 举例；B、C 使用自己的目录、身份和 feature/member-b 或 feature/member-c 分支前缀。Git 本地身份只影响之后的提交，不会改写已有提交。')
    code(doc, "$teamRemote = Read-Host '粘贴小组公开仓库HTTPS地址'\ngit clone $teamRemote 'D:\\Codes\\soft-project\\ChengVault-A'\nSet-Location 'D:\\Codes\\soft-project\\ChengVault-A'\n$memberName = Read-Host '输入你的真实姓名'\n$memberEmail = Read-Host '输入你的Git邮箱'\ngit config --local user.name $memberName\ngit config --local user.email $memberEmail\ngit switch master\ngit pull --ff-only origin master\ngit switch -c feature/member-a/restore-feedback")
    paragraph(doc, '完成一个实际改动后先测试，再核对差异并只暂存本次文件。下列路径和提交说明仅用于展示格式，须改成真实改动；没有差异或未执行测试就不要照抄提交。不要使用空提交凑数量。')
    code(doc, "git status --short\ngit diff --check\ngit diff\ngit add -- src/gui/main_window.cpp tests/gui_smoke.cpp\ngit diff --cached\ngit commit -m 'fix: clarify the verified restore conflict case'\ngit push -u origin feature/member-a/restore-feedback")
    paragraph(doc, '提交或评审说明包含触发问题、修改后的行为、实际测试和剩余限制。A 的分支交 B，B 交 C，C 交 A；必要时共同讨论。review 后由本人修正，再提交。不要将三个人的补丁集中给一个人以其身份提交，也不要批量 squash 掩盖每个人的真实提交。')
    heading(doc, '集成者在评审通过后保留提交合并', 2)
    code(doc, "git fetch origin\ngit switch master\ngit pull --ff-only origin master\ngit merge --no-ff origin/feature/member-a/restore-feedback\n# 运行所需集成检查并确认通过后\ngit push origin master\ngit log --graph --oneline --decorate -15")
    paragraph(doc, 'merge --no-ff 保留分支原提交并增加合并记录。若发生冲突，和文件负责人逐项解决、重新验证再完成合并；不理解时用 git merge --abort 返回合并前状态。不要改历史作者或日期，不对共享 master 强推。', 9.8)

    new_page(doc, '五 仍需补做的验收')
    table(doc, ['事项','负责人及复核','实际完成标准'], [
        ['新UI复验','A 主责 B 复核','本人操作备份 恢复 错密码 冲突 取消和修复；比较恢复内容。截图只证明所拍页面。'],
        ['第二台独立Windows','A 主责 C 复核','另一真实机器 无开发工具PATH 启动四程序并备份恢复；记录机器标识 版本和程序摘要。'],
        ['真实局域网','C 主责 B 复核','两台机器真实TCP 用户隔离 首次与重复上传 局部变化 下载恢复；本机自测不能替代。'],
        ['符号链接与元数据','A 主责 B 复核','当前1314表示缺少权限。只在获授权且具备权限的环境重试；比较链接类型和目标及实际SID 时间 DACL。'],
        ['分支覆盖缺口','B 主责 A 复核','针对可达异常 安全和边界分支补有效输入，重新测量并保存原始分子分母；未达目标如实写明。'],
        ['后台与版本淘汰','C 主责 A 复核','关闭GUI后仍产生成功快照，保留版本可验证恢复；记录失败重试与仓库忙行为。'],
    ], [3.2,3.1,10.7], 9.5)
    heading(doc, '脚本入口与证据保存', 2)
    paragraph(doc, '使用 PowerShell 7.2 或更新版本，在个人 clone 根目录按 scripts/bootstrap.ps1 配好同一 UCRT64 工具链，再构建、测试并生成供权限验收使用的便携包。脚本会更新本 clone 的 evidence；保留旧记录，给新日志注明提交、机器和时间，不抹去失败历史。')
    code(doc, "pwsh -File .\\scripts\\build.ps1 -Configuration Release\npwsh -File .\\scripts\\test.ps1 -Configuration Release\npwsh -File .\\scripts\\package.ps1\npwsh -File .\\scripts\\symlink-acceptance.ps1")
    paragraph(doc, '两机验收入口为 scripts/external-acceptance.ps1，按 docs/external-acceptance.md 的 Server 与 Client 步骤执行，并保留双方结果。不要把 AllowSameMachineSelfTest 当成独立机器证明。权限脚本不代替授权，也不应为拿分擅自修改系统安全策略。')
    paragraph(doc, f'基线：行 {coverage["line_covered"]}/{coverage["line_total"]}={coverage["line_percent"]:.1f}%；原始分支 {coverage["branch_covered"]}/{coverage["branch_total"]}={coverage["branch_percent"]:.1f}%。辅助 {auxiliary["branch_percent"]:.1f}% 与判定覆盖 {coverage["decision_percent"]:.1f}% 另列。后续改动后以新测量为准，不能把最高口径写成原始分支达标。', 10)

    new_page(doc, '六 提交材料与答辩准备')
    table(doc, ['交付内容','主责与复核','提交前检查'], [
        ['报告与成员信息','A 汇总 B 复核','填真实姓名 学号 实验室 日期；需求 设计 测试与最终版本一致；教师签名区域保持空白。'],
        ['测试与算法资料','B 汇总 C 复核','标准向量来源清楚；RS 12种单片加66种双片组合可解释；FAIL SKIP与权限限制如实保留。'],
        ['源码与可执行程序','C 打包 A 复核','保留CMake与脚本 Git历史 当前源版本；在另一机器验证依赖和运行；程序包哈希可核对。'],
        ['PPT 讲稿与演示视频','三人共同 C 汇总','截图与最终UI一致；视频在课程时间上限内，真实运行；每人有讲解段落并参加问答。'],
        ['过程材料与上传','各自补齐 C 核对','本人lg.team任务 提交与review记录 证书 考勤 AI使用说明 视频链接及提交回执。'],
    ], [3.5,3.2,10.3], 9.5)
    heading(doc, '三人分别准备的答辩内容', 2)
    paragraph(doc, 'A 讲需求、界面、筛选、NTFS 和两套归档；B 讲压缩、密码认证、纠错与测试；C 讲仓库、后台、远程认证、增量和交付。先讲本人负责模块的原理与实测证据，再讲本人做的真实修改。三人都要能沿按钮、核心函数、磁盘产物和测试解释完整恢复链路。')
    paragraph(doc, '排练时互问：错误密码为什么不能触碰目标；权限不足怎样呈现；重复上传为何仍有控制流量；RS为何不是任意20%损坏都能修复；原始分支覆盖为何仍未达到75%。使用自己的理解作答，不能只背 PPT。')
    heading(doc, '最终共同确认', 2)
    paragraph(doc, '逐项确认成员资料真实、三人贡献和计划分开、测试证据绑定最终版本、外部未验项明确、提交目录和链接完整。个人心得、证书、考勤、任务活动和签名只能由相关成员按真实经历补充。扩展算法分别计分及新增纠错加分由教师认定，任何文档都不保证满分。')

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    doc.save(OUTPUT)
    plan_result = write_plan(reset_plan)
    result = {'document':str(OUTPUT),'sha256':hashlib.sha256(OUTPUT.read_bytes()).hexdigest(),
              'taskCount':len(TASKS),'members':['A','B','C'],'coverageSource':'evidence/coverage/summary.json',
              'renderReviewRequired':True,'taskPlan':plan_result}
    cache=ROOT / '.cache/team-guide'; cache.mkdir(parents=True,exist_ok=True)
    (cache/'build-result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reset-plan', action='store_true',
                        help='Back up the existing CSV inside docs, then reset it to the module-responsibility and takeover plan')
    build(parser.parse_args().reset_plan)
