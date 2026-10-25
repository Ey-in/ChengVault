"""Generate the original ChengVault UML model and matching standalone SVGs.

Only Python's standard library is required. Model/view field names were checked
against the official StarUML schema examples; no example domain model is reused.
The generator validates every reference, containment parent, ID and view bound.
"""
from pathlib import Path
import json
import math
import uuid
from html import escape

ROOT = Path(__file__).resolve().parent
FONT = "Microsoft YaHei;15;0"
NAMESPACE = uuid.UUID("218d3c0f-5a63-4985-82c5-5f591e1a9dbf")


def ident(key):
    return "CV_" + uuid.uuid5(NAMESPACE, key).hex


def ref(element):
    return {"$ref": element["_id"]}


def element(kind, key, parent=None, name=None, **fields):
    result = {"_type": kind, "_id": ident(key)}
    if parent is not None:
        result["_parent"] = ref(parent)
    if name is not None:
        result["name"] = name
    result.update(fields)
    return result


PROJECT = element("Project", "project", name="澄匣备份 ChengVault", ownedElements=[])
PROJECT["documentation"] = "原创课程设计模型。匹配本仓库当前公共接口；源码是最终行为依据。"
MODEL = element("UMLModel", "model", PROJECT, "ChengVault 软件设计", ownedElements=[])
PROJECT["ownedElements"].append(MODEL)
DIAGRAMS = []


def label(parent, key, text, x, y, width, visible=True, kind="LabelView"):
    return element(kind, key, parent, font=FONT, left=x, top=y, width=width,
                   height=22, text=text, visible=visible)


def name_compartment(view, model, key, x, y, width, title):
    part = element("UMLNameCompartmentView", key + "/name", view, model=ref(model),
                   font=FONT, left=x, top=y, width=width, height=34, subViews=[])
    for index, field in enumerate(("stereotypeLabel", "nameLabel", "namespaceLabel", "propertyLabel")):
        text = title if index == 1 else ""
        child = label(part, key + "/" + field, text, x + 6, y + 6, width - 12, index == 1)
        part["subViews"].append(child)
        part[field] = ref(child)
    view["subViews"].append(part)
    view["nameCompartment"] = ref(part)
    return part


def node_view(diagram, model, node, key):
    kind = node.get("kind", "class")
    mapping = {"class": "UMLClassView", "component": "UMLComponentView", "actor": "UMLActorView",
               "usecase": "UMLUseCaseView", "state": "UMLStateView", "lifeline": "UMLSeqLifelineView", "initial": "UMLPseudostateView"}
    view = element(mapping[kind], key, diagram, model=ref(model), font=FONT,
                   left=node["x"], top=node["y"], width=node["w"], height=node["h"],
                   subViews=[], containerChangeable=True)
    if kind == "initial":
        # Pseudostates inherit node-parasitic labels, not plain LabelView.
        # Their metamodel does not define the classifier wordWrap property.
        for field in ("nameLabel", "stereotypeLabel", "propertyLabel"):
            child = label(view, key + "/" + field, "", node["x"], node["y"], node["w"], False,
                          kind="NodeLabelView")
            child.update(model=ref(model), alpha=2.356194490192345, distance=20)
            view["subViews"].append(child)
            view[field] = ref(child)
        return view
    view["wordWrap"] = True
    name_compartment(view, model, key, node["x"], node["y"], node["w"], node["title"])
    if kind == "lifeline":
        line = element("UMLLinePartView", key + "/line", view, model=ref(model), font=FONT,
                       left=node["x"] + node["w"] / 2, top=node["y"] + 60, width=1, height=node["h"] - 60)
        view["subViews"].append(line)
        view["linePart"] = ref(line)
    elif kind == "state":
        for field, subtype in (("internalActivityCompartment", "UMLInternalActivityCompartmentView"),
                               ("internalTransitionCompartment", "UMLInternalTransitionCompartmentView"),
                               ("decompositionCompartment", "UMLDecompositionCompartmentView")):
            part = element(subtype, key + "/" + field, view, model=ref(model), font=FONT, visible=False,
                           left=node["x"], top=node["y"] + 34, width=node["w"], height=0)
            view["subViews"].append(part)
            view[field] = ref(part)
    else:
        for field, subtype in (("attributeCompartment", "UMLAttributeCompartmentView"),
                               ("operationCompartment", "UMLOperationCompartmentView"),
                               ("receptionCompartment", "UMLReceptionCompartmentView"),
                               ("templateParameterCompartment", "UMLTemplateParameterCompartmentView")):
            part = element(subtype, key + "/" + field, view, model=ref(model), font=FONT,
                           left=node["x"], top=node["y"] + 34, width=node["w"], height=0, visible=False)
            view["subViews"].append(part)
            view[field] = ref(part)
        view["suppressAttributes"] = True
        view["suppressOperations"] = kind != "class"
        if kind == "usecase":
            part = element("UMLExtensionPointCompartmentView", key + "/extensions", view,
                           model=ref(model), visible=False, left=node["x"], top=node["y"] + 34,
                           width=node["w"], height=0)
            view["subViews"].append(part)
            view["extensionPointCompartment"] = ref(part)
        if kind == "class":
            view["suppressAttributes"] = False
            next_top = node["y"] + 34
            for position, collection, row_kind in ((1, "attributes", "UMLAttributeView"), (2, "operations", "UMLOperationView")):
                part = view["subViews"][position]
                part["visible"] = bool(model[collection])
                part["top"] = next_top
                part["height"] = 22 * len(model[collection]) + (8 if model[collection] else 0)
                part["subViews"] = []
                for index, member in enumerate(model[collection]):
                    text = member.get("documentation", member["name"])
                    child = label(part, key + "/" + collection + str(index), "+ " + text,
                                  node["x"] + 8, next_top + 4 + index * 22, node["w"] - 16, kind=row_kind)
                    child["model"] = ref(member)
                    child["horizontalAlignment"] = 0
                    part["subViews"].append(child)
                next_top += part["height"]
            view["height"] = max(node["h"], next_top - node["y"])
    return view


def endpoints(a, b, edge):
    if "points" in edge:
        return edge["points"]
    ax, ay = a["x"] + a["w"] / 2, a["y"] + a["h"] / 2
    bx, by = b["x"] + b["w"] / 2, b["y"] + b["h"] / 2
    if abs(bx - ax) >= abs(by - ay):
        return [[a["x"] + (a["w"] if bx >= ax else 0), ay], [b["x"] + (0 if bx >= ax else b["w"]), by]]
    return [[ax, a["y"] + (a["h"] if by >= ay else 0)], [bx, b["y"] + (0 if by >= ay else b["h"])]]


def edge_view(diagram, model, edge, views, key, points):
    kinds = {"dependency": "UMLDependencyView", "include": "UMLIncludeView", "association": "UMLAssociationView",
             "transition": "UMLTransitionView", "message": "UMLSeqMessageView"}
    kind = edge.get("kind", "dependency")
    source = views[edge["from"]]
    target = views[edge["to"]]
    view = element(kinds[kind], key, diagram, model=ref(model), font=FONT, subViews=[],
                   tail=source.get("linePart", ref(source)), head=target.get("linePart", ref(target)),
                   points=";".join(f"{int(x)}:{int(y)}" for x, y in points), lineStyle=1)
    mid = points[len(points) // 2]
    for index, field in enumerate(("nameLabel", "stereotypeLabel", "propertyLabel")):
        text = edge.get("label", "") if index == 0 else ("«include»" if kind == "include" and index == 1 else "")
        child = label(view, key + "/" + field, text, mid[0] - 80, mid[1] - 30 - 20 * index,
                      160, bool(text), "EdgeLabelView")
        child.update(model=ref(model), hostEdge=ref(view), edgePosition=1, distance=18 + index * 18, alpha=1.5707963)
        view["subViews"].append(child)
        view[field] = ref(child)
    if kind == "association":
        for field in ("tailRoleNameLabel", "tailPropertyLabel", "tailMultiplicityLabel", "headRoleNameLabel", "headPropertyLabel", "headMultiplicityLabel"):
            child = label(view, key + "/" + field, "", mid[0], mid[1], 0, False, "EdgeLabelView")
            child.update(model=ref(model), hostEdge=ref(view), edgePosition=1)
            view["subViews"].append(child)
            view[field] = ref(child)
        for field in ("tailQualifiersCompartment", "headQualifiersCompartment"):
            child = element("UMLQualifierCompartmentView", key + "/" + field, view, model=ref(model), visible=False,
                            left=mid[0], top=mid[1], width=0, height=0)
            view["subViews"].append(child)
            view[field] = ref(child)
    if kind == "message":
        activation = element("UMLActivationView", key + "/activation", view, model=ref(model),
                             left=points[-1][0] - 7, top=points[-1][1], width=14, height=22,
                             font=FONT, visible=not edge.get("reply", False))
        view["subViews"].append(activation)
        view["activation"] = ref(activation)
    return view


def add_diagram(key, title, dtype, nodes, edges, height=1000, note=""):
    package = element("UMLPackage", key + "/package", MODEL, title, ownedElements=[])
    MODEL["ownedElements"].append(package)
    owner = package
    if dtype == "UMLSequenceDiagram":
        collaboration = element("UMLCollaboration", key + "/collaboration", package, title, ownedElements=[], attributes=[])
        owner = element("UMLInteraction", key + "/interaction", collaboration, title, ownedElements=[], participants=[], messages=[])
        package["ownedElements"].append(collaboration)
        collaboration["ownedElements"].append(owner)
    elif dtype == "UMLStatechartDiagram":
        owner = element("UMLStateMachine", key + "/machine", package, "自动任务 Engine", ownedElements=[], regions=[])
        package["ownedElements"].append(owner)
        region = element("UMLRegion", key + "/region", owner, "任务行为", vertices=[], transitions=[])
        owner["regions"].append(region)
    diagram = element(dtype, key + "/diagram", owner, title, ownedViews=[])
    if dtype == "UMLSequenceDiagram":
        # Labels already carry the same explicit step numbers as the report
        # illustrations. Disable StarUML's additional automatic prefix.
        diagram["showSequenceNumber"] = False
    owner["ownedElements"].append(diagram)
    models, views = {}, {}
    types = {"class": "UMLClass", "component": "UMLComponent", "actor": "UMLActor", "usecase": "UMLUseCase", "state": "UMLState", "lifeline": "UMLLifeline", "initial": "UMLPseudostate"}
    for node in nodes:
        nkey = key + "/" + node["id"]
        kind = node.get("kind", "class")
        parent = region if kind in ("state", "initial") else owner
        model = element(types[kind], nkey + "/model", parent, node["title"], ownedElements=[])
        if kind == "lifeline":
            attr = element("UMLAttribute", nkey + "/role", collaboration, node["title"])
            collaboration["attributes"].append(attr)
            model["represent"] = ref(attr)
            owner["participants"].append(model)
        elif kind in ("state", "initial"):
            region["vertices"].append(model)
            if kind == "initial": model["kind"] = "initial"
        else:
            owner["ownedElements"].append(model)
        if kind == "class":
            model["operations"], model["attributes"] = [], []
            for summary in node.get("lines", []):
                for signature in summary.split(" / "):
                    if "(" in signature:
                        member = element("UMLOperation", nkey + "/operation" + str(len(model["operations"])), model,
                                         signature.split("(")[0], visibility="public", documentation=signature)
                        model["operations"].append(member)
                    else:
                        # Keep data members as attributes, never fake methods.
                        # Summary text is preserved in documentation for review.
                        member = element("UMLAttribute", nkey + "/attribute" + str(len(model["attributes"])), model,
                                         signature.split()[-1], visibility="public", documentation=signature)
                        model["attributes"].append(member)
        models[node["id"]] = model
        view = node_view(diagram, model, node, nkey + "/view")
        views[node["id"]] = view
        diagram["ownedViews"].append(view)
    node_map = {n["id"]: n for n in nodes}
    for i, edge in enumerate(edges):
        ekey = key + "/edge" + str(i)
        kind = edge.get("kind", "dependency")
        parent = region if kind == "transition" else owner
        etype = {"dependency": "UMLDependency", "include": "UMLInclude", "association": "UMLAssociation", "transition": "UMLTransition", "message": "UMLMessage"}[kind]
        model = element(etype, ekey + "/model", parent, edge.get("label", ""))
        if kind == "association":
            model["end1"] = element("UMLAssociationEnd", ekey + "/end1", model, reference=ref(models[edge["from"]]))
            model["end2"] = element("UMLAssociationEnd", ekey + "/end2", model, reference=ref(models[edge["to"]]))
        else:
            model.update(source=ref(models[edge["from"]]), target=ref(models[edge["to"]]))
        if kind == "transition":
            region["transitions"].append(model)
        elif kind == "message":
            model["messageSort"] = "reply" if edge.get("reply") else "synchCall"
            owner["messages"].append(model)
        else:
            owner["ownedElements"].append(model)
        points = endpoints(node_map[edge["from"]], node_map[edge["to"]], edge)
        edge["points"] = points
        diagram["ownedViews"].append(edge_view(diagram, model, edge, views, ekey + "/view", points))
    DIAGRAMS.append(dict(key=key, title=title, type=dtype, nodes=nodes, edges=edges, width=1500, height=height, note=note))


def node(key, title, x, y, w=250, h=100, kind="component", lines=None):
    return dict(id=key, title=title, x=x, y=y, w=w, h=h, kind=kind, lines=lines or [])


def edge(source, target, label="", kind="dependency", **extra):
    return dict({"from": source, "to": target, "label": label, "kind": kind}, **extra)


use_nodes = [node("user", "备份用户", 30, 320, 120, 100, "actor"), node("admin", "管理员", 30, 820, 120, 100, "actor")]
for key, name, y in (("backup", "UC01 创建备份", 150), ("restore", "UC02 还原文件", 310), ("jobs", "UC03 管理自动任务", 470), ("network", "UC04 远程网盘", 630), ("repair", "UC05 检查与修复归档", 790), ("adminuc", "UC06 管理账户/设备", 950)):
    use_nodes.append(node(key, name, 310, y, 300, 85, "usecase"))
for key, name, y in (("scan", "UC07 按规则扫描", 150), ("verify", "UC08 校验并安全恢复", 310), ("auth", "UC09 用户/设备认证", 630), ("detect", "UC10 定位损坏分片", 790)):
    use_nodes.append(node(key, name, 960, y, 330, 85, "usecase"))
use_edges = [edge("user", key, kind="association", points=[[150, 370], [240, 370], [240, y + 42.5], [310, y + 42.5]])
             for key, y in (("backup", 150), ("restore", 310), ("jobs", 470), ("network", 630), ("repair", 790))]
use_edges += [edge("admin", "adminuc", kind="association"), edge("backup", "scan", kind="include"), edge("restore", "verify", kind="include"), edge("network", "auth", kind="include"), edge("repair", "detect", kind="include")]
add_diagram("01-use-cases", "用例图｜用户目标与包含关系", "UMLUseCaseDiagram", use_nodes, use_edges, 1140, "六类必需筛选与四类额外筛选属于 UC07；权限不足必须明确报告。")

components = [node("gui", "Qt Widgets 客户端", 70, 150), node("cli", "命令行工具 CLI", 625, 150), node("agent", "后台任务 Agent", 1180, 150),
              node("archive", "归档服务 archive", 70, 390), node("repo", "版本仓库 Repository", 625, 390), node("net", "网络客户端 Client", 1180, 390),
              node("fs", "NTFS 扫描与恢复", 70, 650), node("algo", "压缩 / 密码 / RS", 625, 650), node("server", "独立网盘 Server", 1180, 650),
              node("win", "Win32 文件与事件 API", 70, 870), node("storage", "归档 / 加密对象 / 快照", 625, 870), node("remote", "用户隔离的远端仓库", 1180, 870)]
component_edges = [edge("gui", "archive"), edge("cli", "repo"), edge("agent", "repo"), edge("gui", "repo"), edge("gui", "net"), edge("archive", "fs"), edge("repo", "fs"), edge("archive", "algo"), edge("repo", "algo"), edge("fs", "win"), edge("repo", "storage"), edge("net", "server", "认证加密 TCP"), edge("server", "remote"), edge("algo", "storage")]
add_diagram("02-components", "构件图｜界面、核心与平台职责", "UMLComponentDiagram", components, component_edges, 1080, "算法由项目自主实现；Qt / Win32 提供界面、I/O、线程、事件、网络及安全随机数。")

classes = [node("plan", "BackupPlan", 40, 150, 300, 145, "class", ["sources / destination", "format / compression / cipher", "filters / strictMetadata"]),
           node("repository", "Repository", 585, 150, 330, 195, "class", ["init() / backup(plan)", "manifest(id) / restore(id)", "verify() / prune()", "changePassword(password)"]),
           node("client", "network::Client", 1160, 150, 300, 145, "class", ["upload(path) / download(id)", "list() / admin(action)", "RemoteOptions options_"]),
           node("context", "Context", 40, 460, 300, 135, "class", ["cancelled / progress", "check() / update()"]),
           node("engine", "schedule::Engine", 585, 460, 330, 170, "class", ["start() / stop() / tick(now)", "launch(job, now)", "running_ / watches_ / dirty_"]),
           node("server", "network::Server", 1160, 460, 300, 170, "class", ["configureUser() / configureDevice()", "listen() / stop()", "unique_ptr<Impl> impl_"]),
           node("restore", "RestoreOptions", 40, 800, 300, 130, "class", ["overwrite / strictMetadata", "selectedPaths"]),
           node("watch", "DirectoryWatch", 585, 800, 330, 130, "class", ["DirectoryWatch(paths)", "takeChanged()"]),
           node("sha", "crypto::Sha256", 1160, 800, 300, 130, "class", ["update(bytes) / finish()", "state_[8] / pending_ / length_"])]
class_edges = [edge("repository", "plan", "读取计划"), edge("repository", "context", "进度 / 取消"), edge("repository", "restore", "恢复选项", points=[[585, 300], [400, 300], [400, 860], [340, 860]]), edge("engine", "repository", "执行备份"), edge("engine", "watch", "持有多个监听器"), edge("client", "server", "协议调用"), edge("repository", "sha", "对象摘要", points=[[915, 305], [1035, 305], [1035, 865], [1160, 865]])]
add_diagram("03-core-classes", "类图｜公共类型与主要协作", "UMLClassDiagram", classes, class_edges, 1040, "cv::fs / archive / compression / repair 是命名空间服务；本图只把真实类与结构体建为 Class。")


def sequence(key, title, names, messages, note):
    width = 1500
    gap = (width - 220) / len(names)
    nodes = [node(str(i), name, 110 + i * gap, 130, gap - 35, 830, "lifeline") for i, name in enumerate(names)]
    edges = []
    for i, (source, target, text, reply) in enumerate(messages):
        y = 255 + i * 61
        sx = nodes[source]["x"] + nodes[source]["w"] / 2
        tx = nodes[target]["x"] + nodes[target]["w"] / 2
        edges.append(edge(str(source), str(target), f"{i + 1}. {text}", "message", reply=reply, points=[[sx, y], [tx, y]]))
    add_diagram(key, title, "UMLSequenceDiagram", nodes, edges, 1050, note)


sequence("04-backup-sequence", "顺序图｜独立归档备份", ["GUI / CLI", "archive::backup", "fs::capture", "压缩 / 加密", "临时区 / QSaveFile"], [
    (0, 1, "提交 BackupPlan", False), (1, 2, "capture(plan, storeChunk)", False), (2, 1, "逐块回调 storeChunk(bytes)", False),
    (1, 4, "数据块暂存 private spool", False), (2, 1, "返回文件清单及元数据", True), (1, 4, "生成 PAX/TAR 或索引内层", False),
    (1, 4, "逐块读取完整内层归档", False), (1, 3, "encode → seal（启用时）", False), (3, 1, "返回认证数据块", True),
    (1, 4, "写外层 / 终止校验 / commit", False), (1, 0, "TaskResult 与统计", True)],
    "先完成稳定快照，再打包与转换；按块处理限制内存，但暂存区需要额外磁盘空间。")
sequence("05-restore-sequence", "顺序图｜认证后解压解包并恢复", ["GUI / CLI", "archive::restore", "open / decode", "临时内层归档", "fs::restore", "NTFS 目标"], [
    (0, 1, "口令 + 恢复选项", False), (1, 2, "解锁主密钥 / 逐块认证解压", False), (2, 1, "返回已认证的明文块", True),
    (1, 3, "写入临时内层归档", False), (1, 3, "验证终止记录 / 解析清单", False), (1, 4, "restore(manifest, loadChunk)", False),
    (4, 1, "按需 loadChunk(id)", False), (1, 3, "按索引读取并验证 SHA256", False), (1, 4, "返回明文数据块", True),
    (4, 5, "安全写入 / 链接 / 元数据", False), (4, 1, "TaskResult 与明细", True), (1, 0, "最终恢复结果", True)],
    "默认不覆盖；完整外层校验通过后才恢复用户文件；目录元数据在子对象处理后恢复。")
sequence("06-network-sequence", "顺序图｜认证与可续传增量上传", ["network::Client", "network::Server", "账户 / 设备库", "用户隔离仓库"], [
    (0, 1, "hello：用户 / 设备 / nonce", False), (1, 2, "查设备配对密钥", False), (1, 0, "server nonce + HMAC proof", True),
    (0, 1, "client proof / 加密 login", False), (1, 2, "验证口令与启用状态", False), (0, 1, "begin：对象清单", False),
    (1, 3, "比对对象 / 暂存偏移", False), (1, 0, "缺失对象与 offset", True), (0, 1, "加密 put：从 offset 续传", False),
    (1, 3, "摘要校验 / 原子提交索引", False), (1, 0, "带认证的成功响应", True)],
    "每帧绑定会话和递增序号；用户路径不由请求任意指定；服务器不掌握备份仓库主密钥。")

states = [node("initial", "", 50, 125, 24, 24, "initial"), node("waiting", "waiting｜等待到期 / 事件", 100, 180, 380, 110, "state"), node("running", "running｜后台备份", 950, 180, 380, 110, "state"),
          node("retry", "retrying｜延迟重试", 950, 580, 380, 110, "state"), node("disabled", "禁用｜enabled=false", 100, 580, 380, 110, "state")]
state_edges = [edge("initial", "waiting", "", "transition", points=[[62, 149], [62, 170], [130, 170], [130, 180]]),
               edge("waiting", "running", "到期 / 首次 / 事件稳定 ≥ 2秒", "transition"),
               edge("running", "waiting", "成功：记录结果并计算 nextRun", "transition", points=[[1140, 180], [1140, 100], [290, 100], [290, 180]], labelPosition=[700, 112]),
               edge("running", "retry", "失败：failures+1", "transition"),
               edge("retry", "running", "重试到期", "transition", points=[[1330, 635], [1430, 635], [1430, 235], [1330, 235]], labelPosition=[1370, 450]),
               edge("waiting", "disabled", "用户禁用", "transition"),
               edge("disabled", "waiting", "用户启用", "transition", points=[[100, 635], [40, 635], [40, 235], [100, 235]], labelPosition=[130, 450]),
               edge("retry", "disabled", "用户禁用", "transition")]
add_diagram("07-task-state", "状态图｜自动任务执行与重试", "UMLStatechartDiagram", states, state_edges, 850,
            "禁用由 enabled 标志表示；事件去抖由 dirty_ 时间记录；同一 Engine 同时仅运行一个任务。")


def svg_for(diagram):
    w, h = diagram["width"], diagram["height"]
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">',
           '<defs><marker id="arrow" markerWidth="11" markerHeight="11" refX="9" refY="5" orient="auto"><path d="M0,0 L10,5 L0,10" fill="none" stroke="#53657c" stroke-width="1.7"/></marker></defs>',
           '<rect width="100%" height="100%" fill="#ffffff"/>',
           '<style>text{font-family:"Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#172c42} .small{font-size:16px} .title{font-size:29px;font-weight:700} .label{font-size:17px} .node{font-size:19px;font-weight:600}</style>',
           f'<text x="35" y="48" class="title">{escape(diagram["title"])}</text>',
           '<path d="M35 70 H1465" stroke="#2aa89b" stroke-width="5"/>']
    if diagram["type"] == "UMLUseCaseDiagram":
        out += ['<rect x="210" y="95" width="1220" height="965" fill="#f8fbfd" stroke="#9fb3c7" stroke-width="2" rx="8"/>',
                '<text x="235" y="125" class="small">系统边界：澄匣备份 ChengVault</text>']
    for e in diagram["edges"]:
        pts = e["points"]
        dashed = e["kind"] in ("dependency", "include") or e.get("reply", False)
        arrow = '' if e["kind"] == "association" else ' marker-end="url(#arrow)"'
        dash = ' stroke-dasharray="7 5"' if dashed else ''
        text = e.get("label", "") or ("«include»" if e["kind"] == "include" else "")
        out.append(f'<polyline points="{" ".join(f"{x},{y}" for x,y in pts)}" fill="none" stroke="#53657c" stroke-width="2"{dash}{arrow}/>')
        if text:
            x, y = (pts[0][0] + pts[1][0]) / 2, (pts[0][1] + pts[1][1]) / 2 - 13
            if "labelPosition" in e: x, y = e["labelPosition"]
            out.append(f'<text x="{x}" y="{y}" text-anchor="middle" class="label" stroke="white" stroke-width="5" paint-order="stroke">{escape(text)}</text>')
    for n in diagram["nodes"]:
        x, y, nw, nh = n["x"], n["y"], n["w"], n["h"]
        kind = n["kind"]
        if kind == "initial":
            out.append(f'<circle cx="{x+nw/2}" cy="{y+nh/2}" r="{nw/2}" fill="#172c42"/>')
            continue
        if kind == "actor":
            cx = x + nw/2
            out += [f'<circle cx="{cx}" cy="{y+14}" r="12" fill="white" stroke="#294b68" stroke-width="2"/>',
                    f'<path d="M{cx} {y+26} V{y+60} M{cx-24} {y+40} H{cx+24} M{cx} {y+60} L{cx-23} {y+87} M{cx} {y+60} L{cx+23} {y+87}" fill="none" stroke="#294b68" stroke-width="2"/>',
                    f'<text x="{cx}" y="{y+113}" text-anchor="middle" class="node">{escape(n["title"])}</text>']
            continue
        if kind == "usecase":
            out.append(f'<ellipse cx="{x+nw/2}" cy="{y+nh/2}" rx="{nw/2}" ry="{nh/2}" fill="#edf7fb" stroke="#2b6d8a" stroke-width="2"/>')
        else:
            visible_height = 60 if kind == "lifeline" else nh
            radius = 20 if kind == "state" else 7
            out.append(f'<rect x="{x}" y="{y}" width="{nw}" height="{visible_height}" rx="{radius}" fill="#edf7fb" stroke="#2b6d8a" stroke-width="2"/>')
        text_y = y + (nh / 2 + 7 if kind in ("usecase", "state") else 35)
        out.append(f'<text x="{x+nw/2}" y="{text_y}" text-anchor="middle" class="node">{escape(n["title"])}</text>')
        if kind == "class":
            out.append(f'<path d="M{x} {y+48} H{x+nw}" stroke="#2b6d8a"/>')
            for i, line in enumerate(n["lines"]):
                out.append(f'<text x="{x+12}" y="{y+75+i*27}" class="small">{escape(line)}</text>')
        elif kind == "lifeline":
            out.append(f'<path d="M{x+nw/2} {y+60} V{y+nh}" stroke="#8ca3b8" stroke-width="1.5" stroke-dasharray="7 5"/>')
    out.append(f'<text x="35" y="{h-28}" class="small">{escape(diagram["note"])}</text>')
    out.append('</svg>')
    return "\n".join(out)


def validate(project):
    ids, references, parents = {}, [], []
    def visit(value):
        if isinstance(value, dict):
            if "_id" in value:
                assert value["_id"] not in ids, "duplicate model ID"
                ids[value["_id"]] = value
                if "_parent" in value:
                    parents.append((value["_id"], value["_parent"]["$ref"]))
            if "$ref" in value:
                references.append(value["$ref"])
            for child in value.values(): visit(child)
        elif isinstance(value, list):
            for child in value: visit(child)
    visit(project)
    assert all(key in ids for key in references), "dangling model/view reference"
    for child, parent in parents:
        assert child != parent, "self-parent"
        seen = {child}
        while parent in ids:
            assert parent not in seen, "containment cycle"
            seen.add(parent)
            parent = ids[parent].get("_parent", {}).get("$ref")
    for d in DIAGRAMS:
        for n in d["nodes"]:
            assert n["x"] >= 0 and n["y"] >= 0
            assert n["x"] + n["w"] <= d["width"] and n["y"] + n["h"] <= d["height"]
    return {"modelsAndViews": len(ids), "references": len(references), "diagrams": len(DIAGRAMS),
            "uniqueIds": True, "resolvedReferences": True, "acyclicParents": True, "viewBounds": True,
            "nativeStarUmlOpenVerified": False,
            "scope": "STATIC_STRUCTURE_ONLY; this check does not launch StarUML",
            "nativeValidationEvidence": "../../evidence/uml-native/validation.json"}


ROOT.mkdir(parents=True, exist_ok=True)
validation = validate(PROJECT)
(ROOT / "ChengVault.mdj").write_text(json.dumps(PROJECT, ensure_ascii=False, indent=2), encoding="utf-8")
for d in DIAGRAMS:
    (ROOT / (d["key"] + ".svg")).write_text(svg_for(d), encoding="utf-8")
(ROOT / "diagram-layouts.json").write_text(json.dumps(DIAGRAMS, ensure_ascii=False, indent=2), encoding="utf-8")
(ROOT / "model-validation.json").write_text(json.dumps(validation, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(validation, ensure_ascii=False))
