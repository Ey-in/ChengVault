// Editable defense deck. All measurements are loaded from actual project evidence.
import fs from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const root=path.resolve(process.argv[2]||process.cwd());
const runtime=process.env.ARTIFACT_RUNTIME||path.join(process.env.USERPROFILE||process.cwd(),'.cache','artifact-runtime');
process.env.RUNTIME_NODE_MODULES=path.join(runtime,'dependencies/node/node_modules');
process.env.PYTHONUTF8='1';
const skill=process.env.ARTIFACT_SKILL||path.join(process.env.USERPROFILE||process.cwd(),'.cache','artifact-runtime','skills','presentations');
const {Presentation,PresentationFile}=await import(pathToFileURL(path.join(runtime,'dependencies/node/node_modules/@oai/artifact-tool/dist/artifact_tool.mjs')).href);
const {finalizePresentation}=await import(pathToFileURL(path.join(skill,'container_tools/artifact_tool_utils.mjs')).href);
const work=path.join(root,'.cache/presentation');await fs.mkdir(work,{recursive:true});
await fs.mkdir(path.join(work,'final'),{recursive:true});
const read=async p=>JSON.parse(await fs.readFile(path.join(root,p),'utf8'));
const evidence=await read('evidence/test-results.json');
const gui=await read('evidence/gui-test-results.json');
const background=await read('evidence/background-test-results.json');
const benchmark=await read('evidence/benchmark-results.json');
const network=await read('evidence/network-smoke-results.json');
const coverage=await read('evidence/coverage/summary.json');
const auxiliary=await read('evidence/coverage/auxiliary-summary.json');
const color={ink:'#16373A',green:'#28775D',muted:'#607570',light:'#F4F6F4',line:'#CCD9D2',gold:'#B98C3C',red:'#B85443'};
const font='Microsoft YaHei';
const presentation=Presentation.create({slideSize:{width:1280,height:720}});
let notes=[];
function rect(s,x,y,w,h,fill=color.green){return s.shapes.add({geometry:'rect',position:{left:x,top:y,width:w,height:h},fill,line:{fill:'none',width:0}});}
function text(s,value,x,y,w,h,size=25,bold=false,ink=color.ink){const a=s.shapes.add({geometry:'textbox',position:{left:x,top:y,width:w,height:h},fill:'none',line:{fill:'none',width:0}});a.text=value;a.text.style={typeface:font,fontSize:size,bold,color:ink};a.text.insets=0;return a;}
function slide(title,subtitle='',note=''){const s=presentation.slides.add();s.background.fill=color.light;text(s,title,64,45,1152,64,38,true);if(subtitle)text(s,subtitle,64,113,1150,50,21,false,color.muted);rect(s,64,673,1152,1,color.line);text(s,String(presentation.slides.items.length).padStart(2,'0'),1164,685,52,22,14,false,color.muted);s.speakerNotes.text=note;notes.push({title,note});return s;}
function table(s,headers,rows,x=64,y=185,w=1152,h=420,widths){const values=[headers,...rows];const t=s.tables.add({rows:values.length,columns:headers.length,left:x,top:y,width:w,height:h,values,...(widths?{columnWidths:widths}:{})});t.borders.assign({fill:color.line,width:1,style:'solid'});for(let r=0;r<values.length;r++)for(let c=0;c<headers.length;c++){const cell=t.getCell(r,c);cell.fill=r===0?color.ink:(r%2?'#FFFFFF':'#EAF0EB');cell.text.style={typeface:font,fontSize:21,bold:r===0,color:r===0?'#FFFFFF':color.ink};}return t;}
function box(s,label,x,y,w=220,h=80,fill=color.green){rect(s,x,y,w,h,fill);text(s,label,x+18,y+20,w-36,h-26,25,true,'#FFFFFF');}
function arrow(s,x,y,w=48){s.shapes.add({geometry:'rightArrow',position:{left:x,top:y,width:w,height:26},fill:color.muted,line:{fill:'none',width:0}});}
function line(s,number,title,body,y){text(s,number,64,y,64,55,34,true,color.green);text(s,title,145,y,1065,40,27,true);text(s,body,145,y+46,1065,62,23,false,color.muted);}
async function picture(s,name,x,y,w,h){s.images.add({blob:new Uint8Array(await fs.readFile(path.join(root,'evidence/gui',name+'.png'))),contentType:'image/png',alt:'ChengVault 实际运行界面 '+name,fit:'contain',position:{left:x,top:y,width:w,height:h}});}

{
const s=slide('澄匣备份 ChengVault','','介绍项目定位：面向Windows NTFS的数据备份与恢复软件。重点展示独立实现的算法、特殊文件与元数据、自动化、远程增量，以及新增的RS纠错。姓名与学号由小组在提交前真实填写。');
text(s,'保存数据，也保存恢复数据所需的信息。',64,150,1120,80,34,false,color.green);
text(s,'软件开发综合实验 · C++20 / Qt 6 Widgets',64,252,1100,45,27);
box(s,'目录树与元数据',64,370,300);arrow(s,384,397,60);box(s,'加密备份 / 版本仓库',464,370,330);arrow(s,814,397,60);box(s,'验证与完整恢复',894,370,320);
text(s,'小组及成员：待填写     学号：待填写',64,566,1120,36,23,false,color.muted);
}
{
const s=slide('以可验证的恢复结果覆盖课程要求','基础、扩展与新增功能对应同一套核心库','逐项说明评分对应，强调功能支持要有还原结果证明。打包、压缩、加密各提供两种算法，筛选覆盖六类。具体分数由教师根据实现及演示认定，项目不预先宣称满分。');
table(s,['要求','实现','验收依据'],[['基础备份 / 还原','目录树、空对象、中文与大文件','内容哈希、结构一致'],['特殊文件 / 元数据','NTFS 5类对象；SID、DACL、时间','文件标识、链接目标、稀疏范围'],['自主算法','2种归档、2种压缩、2种加密','独立向量 + 算法组合恢复'],['自动 / 网络','后台调度、实时、账户、加密增量','真实事件、TCP与分块上传量'],['新增纠错','RS(12,10) .cvr 修复文件','全部78种单片/双片组合']],64,184,1152,420,[270,470,412]);
}
{
const s=slide('一次备份，两个恢复入口','单文件便于携带；版本仓库复用未变化的数据块','展示实际GUI。单文件归档由恢复入口自动识别其压缩和加密参数；仓库先选择版本再恢复。默认拒绝覆盖已有文件，界面同时提供筛选预览、进度和取消。');
await picture(s,'backup',550,172,666,460);text(s,'单文件归档',64,210,460,45,30,true);text(s,'选择来源 → 设置算法 → 生成归档\n输入密码后自动解密、解压、解包',64,274,460,110,24);text(s,'多版本仓库',64,426,460,45,30,true);text(s,'完整版本清单 + 1 MiB 数据块\n选择任意保留版本进行恢复',64,490,460,100,24);
}
{
const s=slide('核心库复用，界面和算法分离','四个程序共用数据格式、恢复规则和错误处理','说明模块边界。GUI负责交互，CLI负责脚本，Agent负责后台调度，Server负责用户隔离和密文存储。核心库提供捕获恢复、归档压缩加密、仓库与纠错。Qt/WinAPI仅用于基础设施。');
['GUI','CLI','Agent','Server'].forEach((x,i)=>box(s,x,64+i*294,186,270,68,i===3?color.gold:color.green));
rect(s,64,291,1152,125,'#DCE9E1');text(s,'chengvault_core',90,311,1095,38,29,true);text(s,'扫描与筛选    ·    NTFS 捕获 / 恢复    ·    仓库与归档',90,363,1095,36,25);
box(s,'Huffman / LZ77',64,451,360,70);box(s,'AES / ChaCha20 / HMAC',460,451,360,70);box(s,'RS(12,10)',856,451,360,70);
text(s,'Qt：界面、线程与TCP连接     Windows API：文件、事件、权限与DPAPI',64,574,1152,52,24,false,color.muted);
}
{
const s=slide('特殊文件必须恢复为原来的类型','按 Windows / NTFS 平台定义支持范围','硬链接按文件标识和链接计数验证；目录联接按reparse target验证；稀疏文件检查逻辑长度、稀疏属性与分配范围；ADS逐流比较。符号链接代码已实现，但本机缺创建权限，自动测试诚实跳过，需有权限Windows复验。');
table(s,['对象','保留内容','实测状态'],[['硬链接','共享文件标识与对应路径','通过'],['目录联接 Junction','重解析点与目标目录','通过'],['稀疏文件','逻辑长度、空洞和有效数据','超过4 GiB通过'],['备用数据流 ADS','文件流与目录流的名称、数据','通过'],['文件 / 目录符号链接','链接类型与原始目标','当前用户权限不足，待复验']],64,187,1152,395,[300,500,352]);
text(s,'Windows 不声称原样还原 Linux FIFO 或设备节点。',64,611,1152,38,22,false,color.muted);
}
{
const s=slide('内容恢复之后，最后恢复元数据','权限与目录时间不能被后续写入再次改变','解释恢复顺序：目录和文件内容先完成，再建立链接，最终回写元数据。DACL恢复采用不会向子对象重新传播ACE的句柄。严格模式会明确报告权限不足；宽松模式的警告不能算完整元数据恢复通过。');
table(s,['元数据','持久化表示','恢复校验'],[['属主 / 主组','SID字符串','读取恢复结果的SID对比'],['访问控制','DACL + 继承控制位','ACL内容、保护与继承状态'],['三类时间','FILETIME十进制字符串','创建、访问、修改时间分别比较'],['文件属性','Windows属性位','只读、隐藏、稀疏等逐项检查']],64,185,1152,340,[300,390,462]);
text(s,'目录 / 数据  →  链接关系  →  文件元数据  →  目录时间与权限',64,575,1152,60,27,true,color.green);
}
{
const s=slide('筛选规则采用“排除优先、条件同时满足”','预览显示是否命中以及原因，保留必要的祖先目录','课程六类筛选独立实现；路径、名字、扩展名与属主通配匹配采用自行解析的星号、问号、字符集合及范围，使用迭代回退算法，不调用Qt通配转正则或正则匹配引擎。问号匹配一个Unicode码点，异常模式和超出复杂度限制明确报错。附加属性、深度、文本内容和相对快照变化筛选。变化筛选基于记录元数据，不能宣称对所有保留相同元数据的内容修改都能识别；仓库块查重则计算内容标识。');
table(s,['路径 / 类型 / 名字','时间 / 尺寸 / 用户'],[['目录包含与排除、相对路径通配符','创建 / 修改 / 访问时间区间'],['普通文件、目录、链接、扩展名','逻辑大小的上下限'],['文件名包含、排除与通配符','Windows属主账户或SID']],64,194,1152,290,[576,576]);
text(s,'自研通配 + 附加条件',64,530,1152,40,28,true);text(s,'属性位    ·    递归深度    ·    文本内容    ·    相对快照变化',64,586,1152,50,26,false,color.green);
}
{
const s=slide('两种归档实现都能解包','内层负责组织文件，外层统一压缩、加密和完整性保护','索引归档以显式长度和索引记录组织对象；PAX/TAR按512字节记录及PAX扩展组织目录树。未包装的PAX已由系统tar独立读取验证。压缩或加密后的完整成品使用ChengVault自动解码。');
table(s,['比较','索引归档','PAX / TAR'],[['组织方式','内容对象 + 路径索引','顺序记录 + PAX扩展'],['共享数据','复用重复块','按归档逻辑流写入'],['兼容性验证','版本、长度与边界检查','系统tar独立读取内层'],['还原结果','目录 / 内容 / 特殊对象 / 元数据','目录 / 内容 / 特殊对象 / 元数据']],64,187,1152,350,[240,456,456]);
text(s,'显式整数宽度、字节序与校验；不直接写出 C++ 结构体内存。',64,583,1152,50,25,false,color.muted);
}
{
const s=slide('自主实现 Huffman 与 LZ77','每个解压块都验证长度上限与数据摘要','Huffman根据频率建立前缀树并编码位流；LZ77在滑动窗口中寻找重复串，写出距离和长度。若压缩后更大则采用原始块回退。报告区分压缩收益与索引/仓库去重收益，不把两者混为一谈。');
line(s,'01','Huffman：频率 → 树 → 前缀码','适用于符号分布不均的数据；解码按树逐位遍历。',195);
line(s,'02','LZ77：重复串 → 距离 / 长度引用','适用于存在重复片段的数据；解码验证回溯范围。',345);
line(s,'03','不膨胀策略与解码边界','编码收益不足时存原始块；限制输出长度并检查摘要。',495);
}
{
const s=slide('两套加密算法，共用认证与密钥分层','AES-256-CTR / ChaCha20 + HMAC-SHA256','AES与ChaCha20不是仅往返验证，还对照NIST/RFC标准向量。随机盐和PBKDF2派生密码密钥，封装随机仓库主密钥；对象用HKDF派生独立密钥，HMAC覆盖头部和密文。自研密码实现用于课程，尚无独立安全审计。来源：NIST SP800-38A；RFC8439、4231、5869。');
box(s,'用户密码',64,202,240);arrow(s,321,229,46);box(s,'PBKDF2 / 随机盐',387,202,340);arrow(s,747,229,46);box(s,'封装随机主密钥',813,202,403);
box(s,'对象独立密钥',64,363,330);arrow(s,415,390,48);box(s,'AES-256 / ChaCha20',484,363,360);arrow(s,866,390,48);box(s,'HMAC认证',934,363,282);
text(s,'先验证认证标签，再向恢复流程交付明文。清单与文件内容均加密。',64,525,1152,72,28,true,color.green);
text(s,'DPAPI仅保护本机后台任务的主密钥，不替代备份加密算法。',64,614,1152,35,21,false,color.muted);
}
{
const s=slide('完整版本清单 + 固定 1 MiB 数据块','后续版本只保存新增或变化的数据块','每个快照保存完整引用清单，因此淘汰早期版本不会破坏后续版本。仓库使用专属HMAC块标识，查重范围限定在单仓库。只有所有引用已经持久化后才发布快照。');
text(s,'版本 1',64,208,160,45,28,true);text(s,'版本 2',64,340,160,45,28,true);
['A','B','C','D'].forEach((v,i)=>box(s,v,260+i*220,190,190,85));['A','B′','C','D'].forEach((v,i)=>box(s,v,260+i*220,322,190,85,i===1?color.gold:color.green));
text(s,'仅新增 B′；其余引用复用。',260,454,930,50,29,true,color.gold);
text(s,'淘汰先更新版本集合，再清理无引用块；至少保留一个成功版本。',64,566,1152,55,26,false,color.muted);
}
{
const s=slide('后台调度与实时备份共用仓库流程','界面关闭后，由独立 Agent 继续处理任务','间隔、每日、每周计划持久化下次运行时间；过期计划在启动后补跑，失败按上限退避。实时变化来自ReadDirectoryChangesW，自行去抖和全树重扫。后台密钥由当前用户DPAPI保护。已加入无GUI运行、淘汰、实时写入及失败重试的真实集成测试。');
box(s,'等待 / 下次运行',64,216,320);arrow(s,405,243,48);box(s,'扫描与生成快照',475,216,330);arrow(s,827,243,48);box(s,'成功 / 版本淘汰',897,216,319);
rect(s,475,380,330,90,color.gold);text(s,'失败 → 有界重试',493,407,294,46,25,true,'#FFFFFF');
text(s,'实时事件',64,493,300,45,28,true);text(s,'新增 · 修改 · 删除 · 重命名 → 去抖 → 稳定读取 → 完整版本',64,555,1152,60,26,false,color.green);
}
{
const s=slide('设备配对 + 双向认证 + 会话加密','默认只监听本机；显式配置后提供局域网服务','握手使用设备配对密钥、双方随机数与认证标签。按方向派生会话密钥，消息含严格递增序号，登录位于加密通道内。篡改、重放、错误密钥和超长帧均有实际TCP测试。服务器不持有备份仓库主密钥。');
line(s,'01','设备配对与随机握手','预先授权设备密钥；双方验证握手认证数据。',190);
line(s,'02','按方向派生会话密钥','加密消息体，校验认证标签、长度和消息序号。',340);
line(s,'03','账户登录后访问密文仓库','服务器管理用户、配额、设备与仓库索引；清单在客户端解密。',490);
}
{
const s=slide('用户隔离与增量上传经过独立进程验证','真实服务器和客户端通过 TCP 传输 8 MiB 测试数据','网络冒烟测试启动独立Server进程，创建两个账户和各自设备，验证另一账户不能列出或下载前者仓库。初传约8.39MB，不变版本新增payload0B，单字节修改上传约1.05MB即1块加1清单。控制报文仍会产生传输量，不能称完全零流量。');
const transfer=network.records.find(x=>x.name==='network-transfer-comparison');
table(s,['操作','新增载荷','验证结果'],[['首次上传',transfer.initialPayloadBytes.toLocaleString('en-US')+' B','块与加密清单上传完成'],['原样重复上传',transfer.repeatPayloadBytes+' B（仍有控制报文）','复用已存在对象'],['单字节局部修改',transfer.incrementalPayloadBytes.toLocaleString('en-US')+' B','1个变化块 + 1份新清单'],['第二账户访问','拒绝跨用户下载','账户仓库隔离'],['下载后恢复','内容 SHA-256 一致','服务器不需备份密码']],64,188,1152,420,[340,390,422]);
}
{
const s=slide('新增扩展：RS(12,10) 冗余纠错','10 个数据片 + 2 个校验片；每片 1 MiB','GF256采用0x11D多项式，系统化Vandermonde矩阵生成校验，自行实现消元求逆。SHA256先将错误定位为擦除，再选10个有效片恢复。全部12种单片和66种双片擦除组合通过。来源：RFC5510。不能宣传为任意20%损坏可修复。');
for(let i=0;i<12;i++){const x=64+i*96;rect(s,x,218,78,140,i<10?(i===2||i===7?color.red:color.green):color.gold);text(s,i<10?'D'+i:'P'+(i-10),x+8,263,62,50,27,true,'#FFFFFF');}
text(s,'摘要定位损坏 → 选取 10 个有效片 → 矩阵求逆 → 重建与复核',64,418,1152,70,28,true,color.green);
text(s,'每组最多 2 个损坏 / 缺失片，数据与校验共同计数。\n完整分组冗余为20%；小文件、末组和索引占用更高。',64,527,1152,110,25);
}
{
const s=slide('修复完成后，仍须原密码才能恢复','只写入新归档，整包 SHA-256 一致后才发布','演示先对加密归档生成cvr，再修改归档字节，使认证校验失败。用对应纠错文件修复到新路径，确认整包摘要一致，最后输入正确密码解密恢复并比较源文件哈希。cvr保护最终密文字节，不携带解密密码。');
await picture(s,'repair',590,175,626,440);
text(s,'损坏归档',64,215,480,45,30,true,color.red);text(s,'检查：分片摘要不匹配',64,273,480,50,24);
text(s,'修复到新文件',64,363,480,45,30,true,color.green);text(s,'整包摘要一致 → 输入原密码\n解密 / 解压 / 解包 → 内容哈希一致',64,426,480,100,24);
text(s,'头尾元数据至少一份有效；两份冲突或同组超过能力时拒绝。',64,613,1152,38,21,false,color.muted);
}
{
const cases=[...evidence.cases,...gui.cases,...background.cases];const passed=cases.filter(x=>x.result==='PASS').length,failed=cases.filter(x=>x.result==='FAIL').length,skipped=cases.filter(x=>x.result==='SKIP').length;
const s=slide('测试结论来自可复现执行','核心自动化、真实GUI、独立网络进程与性能脚本','本页由核心、GUI和独立后台进程的JSON执行记录自动生成，任何失败不会被改写为通过。RS组合是算法空间遍历，不替代文件级损坏恢复验证。符号链接权限不足单独标记SKIP。覆盖率另由GCC/gcovr真实采集，目标与实测必须区分。');
text(s,String(passed),64,194,320,100,78,true,color.green);text(s,'通过',64,308,320,40,28);text(s,String(failed),484,194,300,100,78,true,failed?color.red:color.green);text(s,'失败',484,308,300,40,28);text(s,String(skipped),904,194,300,100,78,true,color.gold);text(s,'权限跳过',904,308,300,40,28);
text(s,'验证内容',64,409,1152,40,29,true);text(s,'标准密码向量 · 18种归档组合 · NTFS实际对象\n路径攻击与篡改 · 后台调度 · 双账户网络 · GUI操作',64,474,1152,110,28);
text(s,`核心行 ${coverage.line_percent}%  /  原始分支 ${coverage.branch_percent}%  /  排除编译器异常分支 ${auxiliary.branch_percent}%`,64,613,1152,35,21,false,color.muted);
s.speakerNotes.text += ' 统计包含核心、GUI和真实后台进程测试，不重复累计覆盖率重跑。完整raw分支保留；辅助分支仅排除编译器异常/不可达边，不等于原始75%目标全部达到。';
notes[notes.length-1].note=s.speakerNotes.text;
}
{
const choose=['large-archive-backup','large-archive-restore','small-files-backup','small-files-restore','over-4gib-sparse-backup','over-4gib-sparse-restore'];
const labels=['1 GiB 索引归档备份','1 GiB 索引归档恢复','5000小文件备份','5000小文件恢复','>4 GiB 稀疏 PAX 备份','>4 GiB 稀疏 PAX 恢复'];
const selected=benchmark.records.filter(x=>choose.includes(x.name));
const rows=selected.map(x=>[labels[choose.indexOf(x.name)]||x.name,(x.durationMs/1000).toFixed(3)+' s',(x.peakWorkingSetBytes/1048576).toFixed(1)+' MiB']);
const s=slide('性能记录来自本机真实数据','Windows 11 25H2 / Qt 6.11.2 / GCC 16.2 Release','数据来自默认benchmark脚本。1GiB测试采用重复的1MiB混合块，索引和仓库去重会显著降低存储量，所以不能将最终比例宣传成纯压缩率或随机数据表现。峰值内存为子进程工作集。完整平台和数据生成步骤见原始JSON与脚本。');
table(s,['负载','耗时','峰值工作集'],rows,64,184,1152,390,[600,276,276]);
text(s,'1 GiB 数据含重复块；存储比例包含去重收益。结果不代表所有文件类型。',64,607,1152,46,22,false,color.muted);
}
{
const s=slide('缺陷修复与仍需复验的边界','保留失败记录，区分实现、测试通过与外部条件','本轮使用重新计算校验的恶意归档检查格式语义，补上PAX扩展头非零填充拒绝及Junction实际跳转目标校验。后台测试补上坏配置隔离、重试计数封顶、配置目录不可用和运行中删除任务保护。发布进程测试还发现版本列表与淘汰竞争，现为公共列表读取增加仓库锁，锁内校验和淘汰复用私有读取函数。符号链接需权限复验；独立Windows机器和真实课程协作记录由小组补齐。');
table(s,['项目','处理与状态'],[['归档格式与链接目标','验证PAX零填充、Junction实际跳转路径'],['后台任务错误处理','隔离坏配置；重试封顶；配置不可用时安全返回'],['仓库并发读取','列表与淘汰共用仓库锁，忙碌明确返回'],['权限与平台','本机符号链接权限不足；NTFS范围明确'],['一致性与安全边界','无VSS全目录快照；自研密码尚无独立审计'],['独立环境与课程过程','需另一Windows环境复验；成员/协作/考勤真实补齐']],64,184,1152,420,[350,802]);
}
{
const s=slide('答辩演示：从损坏归档到验证恢复','交付源码、四个程序、构建脚本、报告、PPT与证据','演示控制在三分钟内，先展示真实源文件与加密备份，再损坏归档并检查，利用cvr修复到新文件，最后输入正确密码恢复并检查哈希。交付目录包含源码和构建脚本，教师可使用CLI和自动测试复现。姓名学号、lg.team截图、证书考勤与视频上传链接只由真实成员填写。');
line(s,'01','建立备份并生成纠错数据','展示源目录、密码与输出文件。',190);line(s,'02','损坏检测与修复','拒绝损坏归档，修复到新的归档路径。',337);line(s,'03','恢复与核验','输入原密码，比较文件内容与恢复元数据。',484);
}

const candidate=path.join(work,'candidate.pptx');await(await PresentationFile.exportPptx(presentation)).save(candidate);
const final=path.join(work,'final','ChengVault-defense-'+Date.now()+'.pptx');
const tableOwners=[2,5,6,7,8,14,18,19];
const result=await finalizePresentation({workspaceDir:root,candidatePath:candidate,finalPath:final,pythonExecutable:path.join(runtime,'dependencies/python/python.exe'),integrityValidatorPath:path.join(skill,'container_tools/inspect_presentation_package_integrity.py'),layoutValidatorPath:path.join(skill,'container_tools/inspect_presentation_layout_geometry.py'),layoutArgs:['--expected-slide-size-emu','12192000,6858000','--validate-bullet-geometry','--validate-heading-fit',...tableOwners.flatMap(n=>['--require-native-table-slide',String(n)])],explicitTotalSlideCount:20,requiredNativeTableOwnerSlides:tableOwners,fontPolicy:{basis:'design',families:[font]},verifyArtifactToolImport:true,receiptPath:path.join(work,'validation-'+Date.now()+'.json')});
await fs.copyFile(final,path.resolve(root,'../PPT/ChengVault项目答辩.pptx'));
await fs.writeFile(path.resolve(root,'../PPT/逐页讲稿.txt'),notes.map((x,i)=>`${i+1}. ${x.title}\n${x.note}\n`).join('\n'),'utf8');
await fs.writeFile(path.join(work,'slides.json'),JSON.stringify(notes,null,2),'utf8');
for(let i=0;i<presentation.slides.items.length;i++){const blob=await presentation.export({slide:presentation.slides.items[i],format:'png',scale:1});await fs.writeFile(path.join(work,`slide-${String(i+1).padStart(2,'0')}.png`),new Uint8Array(await blob.arrayBuffer()));}
console.log(JSON.stringify({final,slideCount:presentation.slides.items.length,result}));
