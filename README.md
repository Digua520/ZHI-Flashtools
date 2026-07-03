# ZHI-Flashtools 通信层源码（第一版）

## 🔧 拿到 exe（不用装 Visual Studio）

这个仓库配了 `.github/workflows/build.yml`，用 GitHub 的免费云端 Windows 机器
自动编译，你不用在自己电脑上装任何东西。步骤：

1. 在 GitHub 上新建一个仓库（免费，public 或 private 都行）
2. 把这个项目的所有文件传上去（网页拖拽上传，或者用 `git push`）
3. 打开仓库页面的 **Actions** 标签页，应该会自动开始跑一个叫"编译 Windows exe"
   的流程（如果没自动跑，点 "Run workflow" 手动触发一次）
4. 等个几分钟（第一次要下载Qt/OpenSSL会慢一点，大概10-15分钟），跑完之后
   页面上会出现一个 **Artifacts** 区块，里面有个 `ZHI-Flashtools-windows`
   压缩包，下载解压，`ZHI-Flashtools.exe` 就在里面，双击运行

**老实提醒**：这套代码我这边从来没有真正编译过（沙盒环境没有 Windows 编译器），
第一次跑 Actions **大概率会报编译错误**——可能是某个头文件漏 include、
某个 API 参数类型不对、或者 CMake 配置细节没对上。这是正常的，写了这么多
代码第一次编译就全过的概率不高。**报错了怎么办**：打开 Actions 页面失败的
那次运行，把红色报错的那几行文字复制给我，我根据具体错误信息改代码，
比我在这边凭空猜要准得多。这个来回可能要走个两三轮才能编译通过，属于
正常开发流程，不代表代码架构有问题。

---


这是可编译进最终 exe 的**真实协议实现**，不是演示/占位代码。对应之前架构图里的：

```
USB通信层 (WinUSB)  →  src/usb/
Fastboot协议层       →  src/protocol/fastboot/
ADB协议层            →  src/protocol/adb/
高通EDL协议层        →  src/protocol/edl/  (Sahara握手 + Firehose读写)
日志层               →  src/core/
```

## 各模块说明

### `protocol/mtk/BromProtocol` + `firmware/ScatterParser`（本次新增）
联发科 BROM 握手协议 + Scatter 文件解析。**这部分的可信度和前面几个模块不一样**，
需要单独说明：

- **握手部分（`Handshake()`）** 依据的是社区工具多年验证过的公开协议
  （自动波特率探测的四步取反回显），这部分置信度较高。
- **`GetHwCode()`** 命令和响应格式也是社区广泛使用的公开信息，置信度较高。
- **`SendDA()`** 这一段我标了警告注释：不同 BROM 版本在签名长度字段位数、
  校验和算法上可能有差异，这是我按最常见的实现写的骨架，**没有真实设备
  验证过**。接入具体机型前，务必先在一台可以承受刷坏风险的测试机上跑通，
  不要直接用在唯一测试机或生产设备上。如果你有抓包工具（Wireshark USB
  抓包/USBPcap），跑一次真实的 SP Flash Tool 线刷过程抓个包，我可以照着
  真实数据把这几个字段调准。

`ScatterParser` 相对没有风险——就是纯文本解析，不涉及和设备的实时交互，
解析逻辑本身出错的话最多是解析结果不对，不会对设备产生任何影响。

### `protocol/mtk/DaProtocol`（本次新增）
BROM `JumpDA()` 之后，DA 自己的分区读写协议。**协议机制**（命令+回显、分块
传输、进度回调、校验和收尾）是完整可用的，跟 `BromProtocol` 同一套设计。
但 `DaOpcodes` 结构体里的具体命令字节是社区常见写法，**没有真机验证过**，
接入具体机型前务必核实（有目标DA文件的话用IDA/Ghidra看一下命令分发表最快，
或者抓一次真实SP Flash Tool线刷的USB包对照）。

### `protocol/adb/AdbAuth`（本次新增）
ADB 的 RSA 密钥生成 / 签名 / 公钥编码，严格照抄 AOSP `adb_auth_host.cpp` +
`android_pubkey.c` 的公开实现，这部分置信度高（跟 Sahara/Fastboot 协议一样
是核实过的公开协议，不是猜的）。`AdbProtocol::Connect()` 现在完整走通了
握手：先尝试用本地已保存的密钥签名，设备不认识就发公钥，**这一步手机屏幕
会弹"允许USB调试"确认框，需要用户手动点确认**——这是 Android 系统本身的
安全设计，没有办法也不应该绕过。密钥默认保存在调用方传入的 `keyPath`，
建议放在类似 `%APPDATA%\ZHI-Flashtools\adbkey` 的位置，下次启动直接复用，
不用每次都重新弹授权框。

**编译依赖**：这个模块需要 OpenSSL。Windows 上最省事的办法是用 vcpkg：
```
vcpkg install openssl:x64-windows
```
然后 CMake 配置时带上 `-DCMAKE_TOOLCHAIN_FILE=<vcpkg路径>/scripts/buildsystems/vcpkg.cmake`。

### `ui/MainWindow`（本次更新：菜单功能接线 + ADB/Fastboot面板加料 + 自动检测设备）

**这次更新是回应"按钮点了没反应"的问题**，具体做了：

- **自动检测设备，不用手动选**：加了个2秒轮询的 `QTimer`（`AutoScanDevices()`），
  程序一启动就自动扫，设备插拔也会自动更新下拉框，不再要求先点"刷新设备"再选。
  手动的"立即重新扫描"按钮还留着，作为需要马上刷新时的备用。
- **顶部菜单"高级重启"**：三个重启命令现在真的会走 `AdbProtocol::Shell("reboot xxx")`。
  注意这几个命令的前提是设备当前处于系统/ADB可访问状态——如果设备已经在
  Fastboot模式，"重启到xxx"这几个命令用不了，要用 Fastboot 面板自己的功能。
- **顶部菜单"高通功能"**：
  - **"EDL 备份基带分区"**：复用 EDL 面板里已经选好的 Loader/rawprogram，
    解析出分区列表后按分区名找含"modem"关键字的条目挨个读出来存成文件。
    **这是启发式匹配**，如果目标机型的基带分区不叫"modem"（比如叫fsg/fsc等
    其他命名），会找不到，日志里会提示；不同厂商命名差异较大，没有通用方案。
  - **"EDL 通用恢复出厂"**：范围收窄成"只清 userdata 分区"，不是全盘擦除
    （`EraseLun`那个是真的全LUN擦除，会连GPT和系统分区一起清掉，风险完全不
    是一个量级，UI上没有暴露那个操作）。加了二次确认弹窗。同样靠分区名精确
    匹配"userdata"，找不到会中止而不是瞎猜。**这里还处理了一个内存问题**：
    userdata 分区可能有几十GB，如果直接分配一块跟分区等大的零缓冲区会把内存
    吃爆，改成了按256MB窗口分块写零，内存占用可控。
- **ADB 面板加了"读取分区"**：走 `shell cat /proc/partitions`，解析成表格
  展示分区名+块数+换算后的大致大小。
- **ADB 面板加了"推送文件到设备"**：新增了 `AdbProtocol::Push()`
  （sync:服务的SEND/DATA/DONE子协议），选本地文件+填设备路径就能推。
- **Fastboot 面板加了"读取全部信息(getvar:all)"**：新增了
  `FastbootProtocol::GetAllVars()`，把设备一次性吐出来的所有INFO变量
  （包括各种 partition-type/partition-size 信息）解析成表格。

### `ui/MainWindow`（本次更新：补齐镜像解析/分区备份还原/QCN三个面板）

之前 HTML 原型设计了这三个导航项，但 Qt 版一直没做，这次补上：

- **镜像解析**：本地选一个 `rawprogram*.xml` 或 `scatter.txt`，自动识别类型
  （靠"含不含 `<program` 标签"这个简单启发式判断，不是很严谨但够用）并解析
  展示分区列表，不需要连设备。
- **分区备份还原**：管理各面板产生的 `.bin` 备份文件（选目录 → 列出文件名/
  大小/修改时间 → "打开文件夹"跳转到系统文件管理器），目前只做浏览管理，
  不解析备份文件内容。
- **QCN 功能**：**这里有个重要的老实说明，面板里也用黄色警告条写了**：
  这不是走高通官方 DIAG/QCDM 协议导出的标准 QCN 文件（那需要单独实现 DIAG
  协议，本项目没做），而是通过已经打通的 Firehose 通道把 `modemst1` +
  `modemst2` 这两个基带配置分区原始读出来存成 `.bin`——这是社区里常见的
  "平替"做法，不完全等价于 QFIL 等官方工具导出的真正 QCN，跨机型兼容性
  没有保证，同样靠分区名精确匹配，命名不同的机型会读取失败。

之前版本的说明（菜单功能接线、Mica效果、样式表、线程安全日志转发等）仍然有效，见下方：

Qt Widgets 主窗口，对应 HTML 原型的整体布局：左侧导航 + 中间内容区
（`QStackedWidget`切页）+ 右侧日志面板。这一版的目标是把几件"能跑通"的
事做对，不是像素级还原 HTML 原型的视觉效果（配色/圆角/玻璃效果留到样式表
阶段再抠，Qt 里用 `setStyleSheet` 基本都能还原）：

- 导航切页、日志面板订阅 `Logger` 实时显示（**注意线程安全**：协议层的
  操作要跑在工作线程里，`WireLoggerSink()` 里用了
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 把日志安全地
  转发回主线程，不能直接从工作线程操作 `QPlainTextEdit`）
- `OnRefreshDevicesClicked()` 现在真正接到了设备选择下拉框（`deviceCombo_`，
  ADB/Fastboot/EDL/MTK 面板共用一份扫描结果），`OnAdbConnectClicked()` /
  `OnFastbootGetVarClicked()` 会用 `SelectedDevicePath()` 拿到真实设备路径，
  不再是占位字符串，点击就能真的尝试打开设备、走协议。
- EDL 和 MTK 面板不再是占位说明文字，现在是真正能用的控件：文件选择
  （Loader/rawprogram/patch，或 Scatter/DA）、本地解析预览表格（选完
  rawprogram/scatter 立刻在表格里显示分区列表，不用连设备就能看）、
  进度条、"开始烧录"按钮。点击开始按钮会在工作线程里完整跑一遍
  `EdlFlashExample.cpp` 那套流程（EDL）或 BROM握手→SendDA→JumpDA→
  逐分区写入（MTK），进度实时反映到进度条上。
  **MTK 面板顶部有个黄色警告条**，提醒 `SendDA` 的加载地址等参数是占位值，
  不是通用值，首次对接新机型前务必核实。
- `ui/DwmEffects.h` 接了 Windows 11 的 Mica 毛玻璃背景（`EnableMicaBackdrop()`），
  `Theme.h` 配套加了 `glassMode` 开关（半透明版配色）。**这里有个已知的坑**：
  Mica 需要 Win11 22H2+，且 Qt 的不透明背景会完全挡住材质，必须同时
  `Qt::WA_TranslucentBackground` + 半透明 QSS 才能看见效果——代码里已经按
  "先探测 Mica 是否启用成功，再决定用哪套样式表"的顺序处理了，低版本系统
  会静默降级成普通不透明主题，不会崩，但具体透明度/模糊强度在 Qt 里显示
  是否符合预期，社区反馈这个组合有时会有渲染小毛病，需要你在真机上跑一遍
  才能确认效果，我这边没有 Windows 环境能实测。
- `ui/Theme.h` 全局样式表：浅色底、圆角、蓝色强调色、按钮做成胶囊形，
  配色跟 HTML 原型的 token 对齐，Mica 没启用成功时会自动退化成这套
  不透明版本。
- 驱动管理面板接了真实检测逻辑（`OnDetectDrivers()`），但**检测方式是有
  局限的老实说明**：判断依据是"当前有没有设备以对应驱动的WinUSB接口被
  枚举到"，不是查 Windows 驱动商店的安装记录，所以只有设备插着的时候
  结果才准，设备没插时会显示"未检测到"，不代表驱动真的没装——面板里
  已经把这个局限写成提示文字放在最上面了。

- **ADB/Fastboot 面板各自加了"仅本协议"检测按钮**（`OnAdbAutoDetect()`/
  `OnFastbootAutoDetect()`），跟顶部共享栏的区别是只扫描对应协议的设备。
  **这里有个必须说清楚的局限**：高通EDL有固定的VID/PID（05C6:9008），但
  ADB/Fastboot 没有——每个手机厂商用的都是自己的VID，同一厂商ADB模式和
  Fastboot模式的PID通常也不一样。目前 `AdbScanTargets()`/
  `FastbootScanTargets()` 只维护了几个常见厂商的示例值，**不可能覆盖所有
  机型**。更严谨的做法是按USB接口的Class/SubClass/Protocol匹配（ADB接口
  标准是0xFF/0x42/0x01），不依赖VID/PID，但这需要改
  `UsbDeviceEnumerator::Enumerate()` 的枚举方式（目前是按VID/PID查，不是
  按接口描述符查），列进了下面的待办清单。

**编译依赖**：需要 Qt6（Widgets 模块）。用官方在线安装器装 Qt6 for MSVC
即可，CMake 配置时指定 `-DCMAKE_PREFIX_PATH=<Qt安装路径>/6.x.x/msvc2019_64`。

### `ui/LoginDialog` + `ui/MainWindow` EDL面板改造（本次更新）

**EDL 面板改成三方案选择**（对应导航里"EDL 刷写"，原"9008 烧录面板"已改名）：
- **小米**：只需引导文件，按钮做"验证 Loader（Sahara握手）"
- **欧加**（OPPO/一加/realme系）：引导文件 + Digest + Signature 三个文件，
  走多镜像 Sahara 握手（`SaharaProtocol::PerformHandshakeMultiImage()`）。
  **老实提醒**：Sahara 协议规范没有公开规定"哪个 imageId 对应哪个文件"，
  这是厂商 PBL 私有约定，代码里按"引导→Digest→Signature"的顺序猜测分配，
  没有真机验证过，顺序不对会握手失败，需要抓包确认。
- **安卓通用**：只需引导文件，按钮"连接并读取分区表"，握手成功后从设备读
  GPT 分区表（GPT 解析逻辑还没写，目前是占位）。

三个方案都不再要求手动导入 rawprogram/patch。原来依赖 rawprogram 的三个
菜单功能（EDL备份基带/恢复出厂/QCN读取）改成了用到时临时弹框让用户选一个
rawprogram，不再从 EDL 面板固定读取。

**"生成 OCDT" 菜单已删除。**

**登录界面**（`ui/LoginDialog`）：程序启动先弹登录框，账号 `XIAOZHI` /
密码 `LOVE-XIAOZHI`，验证通过才进主界面。**必须说清楚的一点**：这个账号
密码是硬编码在程序里的，用 `strings` 之类的工具能直接从 exe 里读出明文，
所以这个登录框是"仪式感/门面"，拦不住真想绕过的人，不是真正的访问控制。
如果以后要做能真正管用户的登录（每人一个号、能封号），要接后端服务器验证
（项目外的 `zhi-auth-server` 就是为此准备的雏形），那才是密码不落到客户端
的正确做法。

### `protocol/edl/EdlTypes.h`（解决平台耦合问题）

之前 `ProgramEntry`/`StorageInfo` 这两个纯数据结构定义在 `FirehoseProtocol.h`
里，导致 `firmware/RawProgramParser.h` 只是想解析个 XML 文件，却因为引用
这两个结构体被迫连带 `#include` 整个 `FirehoseProtocol.h`，而那个文件又
依赖 `UsbDevice.h` → `windows.h`/`winusb.h`——意味着"解析一个rawprogram文件"
这种纯逻辑操作，在非Windows环境下根本编不过。现在拆成独立的
`EdlTypes.h`（无任何平台依赖），`firmware/` 整个目录不再依赖 Windows 头文件。

**这次真的在 Linux 上用 g++ 编译验证过了**（之前的说明都是"没编译过、
需要你在Windows上验证"，这次不一样）：

```
g++ -std=c++20 -c src/core/Logger.cpp -Isrc
g++ -std=c++20 -c src/firmware/XmlLite.cpp -Isrc
g++ -std=c++20 -c src/firmware/RawProgramParser.cpp -Isrc
g++ -std=c++20 -c src/firmware/PatchParser.cpp -Isrc
g++ -std=c++20 -c src/firmware/ScatterParser.cpp -Isrc
```

编译过程中**真的抓到两个之前一直没发现的bug**（不是环境问题，Windows上
编译也会报同样的错）：
1. `XmlLite.h` 漏了 `#include <cstdint>`，导致 `uint64_t`/`uint32_t` 未声明
2. `XmlLite.cpp` 里一个正则表达式用了默认分隔符的原始字符串字面量
   `R"((\w+)\s*=\s*"([^"]*)")"`，模式内容里的 `[^"]*)"`片段提前触发了默认
   终止符 `)"`，导致整个字符串字面量被截断——换成自定义分隔符
   `R"RGX(...)RGX"` 后修复。

修完之后 `tests/test_firmware_parsers.cpp` 和 `tests/test_scatter_parser.cpp`
（本次新增，收录进项目而不是用完就删）跑通了完整的功能验证：
`NUM_DISK_SECTORS-33.` 表达式换算正确、patch解析正确、scatter的YAML解析
（含 `is_download`/`physical_start_addr` 等字段类型转换）正确。用
`-DZHI_BUILD_TESTS=ON` 可以让 CMake 编出这两个测试程序（注意：CMake配置本身
在文件开头就限定了只能在Windows跑，这两个测试要在其他平台单独验证的话，
按上面那几条 g++ 命令直接编译，不走CMake）。

**没测的部分（还是需要你在Windows上验证）**：`protocol/adb/AdbAuth.cpp`
依赖 OpenSSL 开发头文件，这次环境里没有装（没网络装不了 `libssl-dev`），
所以 RSA签名/公钥编码这块**这次仍然没有编译验证过**，跟USB通信层、UI层
一样，还是"写了但没编译测试过"的状态。

### `firmware/RawProgramParser` + `firmware/PatchParser`（本次新增）
把 `rawprogram*.xml` / `patch*.xml` 解析成结构化数据，直接对接
`FirehoseProtocol::Program()` / `Patch()`。

**关于 `start_sector`/`num_partition_sectors` 的表达式问题**已经闭环：
`FirehoseProtocol::GetStorageInfo()` 会查询磁盘总容量，`EdlFlashExample.cpp`
演示了怎么把这个值传给 `RawProgramParser::Resolve()`。**但有个风险点要注意**：
`GetStorageInfo()` 里解析 `total_blocks`/`block_size` 字段名是按最常见的
情况写的，不同芯片平台（尤其 UFS 多LUN设备）返回的 JSON 字段可能不一样，
第一次对接新机型时建议先打印 `StorageInfo::rawJson` 看一眼真实返回格式，
再决定要不要调整字段名。

`src/examples/EdlFlashExample.cpp` 演示了怎么把 usb / Sahara / Firehose /
RawProgramParser 拼成一次完整的"识别设备→握手→查容量→写分区→打patch→复位"
流程，可以直接看这个文件理解各层怎么协作，UI层"开始全盘烧录"按钮的业务
逻辑基本就是照这个骨架填。

### `usb/UsbDevice`
对 WinUSB 的封装：打开设备、自动探测 Bulk IN/OUT 端点、收发字节流、
按 VID/PID 枚举设备、注册 USB 热插拔通知。所有协议层都只依赖这一层，
不直接碰 Windows API，方便以后如果要加 Linux/libusb 支持时替换底层实现。

### `protocol/fastboot/FastbootProtocol`
AOSP 公开的 Fastboot 协议：`getvar` / `download` / `flash` / `erase` /
`reboot*` / `set_active`。`FlashFile()` 是给 UI"写入分区"按钮直接调用的
组合方法（读文件 → download → flash）。

### `protocol/adb/AdbProtocol`
ADB 传输层协议（CNXN/OPEN/WRTE/OKAY/CLSE）。**注意**：`Connect()` 里握手
遇到 `AUTH` 命令时目前直接返回失败并提示"请在手机上确认调试授权"——
这是标准 ADB 安全机制（需要用户在设备屏幕上点确认，主机侧还需要
RSA 私钥签名挑战数据），我没有展开签名细节，建议直接复用 AOSP
`adb_auth` 的参考实现来补全这部分，而不是自己重新设计。

### `protocol/edl/SaharaProtocol` + `FirehoseProtocol`
高通 9008 EDL 的两阶段协议：
1. `SaharaProtocol::PerformHandshake()` —— 把 Firehose Loader 文件"喂"给
   刚进 EDL 的裸机设备，让它跑起来
2. `FirehoseProtocol` —— loader 跑起来之后，用 XML 指令做 Configure /
   Program（写分区）/ Read（读分区/备份）/ EraseLun（全盘擦除）/
   ReadGpt / Reset

`Program()` 里特意做了"镜像大小 > 分区容量则拒绝写入"的校验——这是防止
误刷把相邻分区写坏的关键防护，不要删掉这段。

## 还没做的部分（下一步建议顺序）

1. **MTK `DaOpcodes` + `SendDA`参数实机验证** —— 见上方模块说明，命令字节
   和加载地址等参数需要用真实DA或抓包数据核实，`MainWindow::OnMtkStartFlash()`
   里用的是占位值。
2. **Mica 效果实机调校** —— 见上方说明，需要在真实 Win11 机器上跑一遍
   确认透明度/渲染没有问题，我这边没有 Windows 环境实测。
3. **实机联调 `GetStorageInfo()` 字段名** —— 见前面说明，不同芯片平台
   `total_blocks`/`block_size` 的实际 JSON 字段可能需要调整。
4. **Fastboot/MTK 的 VID/PID 表按厂商细分** —— 目前 `RefreshDeviceCombo()`
   和 `OnDetectDrivers()` 里用的都是示例值，接实际产品前要换成目标机型
   准确的 VID/PID。
5. **"备份基带分区"/"通用恢复出厂"的分区名匹配** —— 目前是按分区名精确/
   模糊匹配"modem"/"userdata"字符串，不同厂商命名可能不一样，匹配不到会
   中止并提示，不会瞎猜，但也意味着遇到命名不同的机型需要手动改代码里
   的匹配关键字。
6. **`usb/`、`protocol/adb/`、`ui/` 这几个目录还没编译验证过** —— 前面
   `firmware/`+`core/`已经用g++在Linux上编译+运行测试过了，这几个目录依赖
   Windows API / OpenSSL / Qt6，只能在Windows上验证，是第一次编译时最可能
   报错的地方（尤其头文件遗漏、跨平台API差异这类小问题）。
7. **`UsbDeviceEnumerator` 改成按USB接口Class/SubClass/Protocol匹配** ——
   见上方 ADB/Fastboot 检测按钮的说明，目前按VID/PID表匹配的方式对ADB这种
   "每个厂商VID都不同"的场景覆盖不全，改成按接口描述符匹配会更通用，但
   `Enumerate()` 内部逻辑要跟着改（现在是先枚举再比对VID/PID，需要改成
   枚举时读取接口描述符里的Class/SubClass/Protocol字段）。

## 编译

```
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" ^
    -DCMAKE_TOOLCHAIN_FILE=<vcpkg路径>/scripts/buildsystems/vcpkg.cmake ^
    -DCMAKE_PREFIX_PATH=<Qt安装路径>/6.x.x/msvc2019_64
cmake --build . --config Release
```

需要：Windows SDK（WinUSB.lib/SetupAPI.lib 随SDK自带）、OpenSSL（建议用
vcpkg装）、Qt6 Widgets（官方在线安装器装 MSVC版）。
