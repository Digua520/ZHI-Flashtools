#include "MainWindow.h"
#include "Theme.h"
#include "DwmEffects.h"

#include "../core/Logger.h"
#include "../usb/UsbDevice.h"
#include "../protocol/fastboot/FastbootProtocol.h"
#include "../protocol/adb/AdbProtocol.h"
#include "../protocol/edl/SaharaProtocol.h"
#include "../protocol/edl/FirehoseProtocol.h"
#include "../protocol/mtk/BromProtocol.h"
#include "../protocol/mtk/DaProtocol.h"
#include "../firmware/RawProgramParser.h"
#include "../firmware/PatchParser.h"
#include "../firmware/ScatterParser.h"

#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QGroupBox>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QFileDialog>
#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QIcon>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <thread>

namespace zhi::ui {

using zhi::usb::UsbDevice;
using zhi::usb::UsbDeviceEnumerator;
using zhi::protocol::fastboot::FastbootProtocol;
using zhi::protocol::adb::AdbProtocol;
using zhi::protocol::edl::SaharaProtocol;
using zhi::protocol::edl::FirehoseProtocol;
using zhi::protocol::edl::StorageInfo;
using zhi::protocol::edl::ProgramEntry;
using zhi::protocol::mtk::BromProtocol;
using zhi::protocol::mtk::DaProtocol;
using zhi::protocol::mtk::HwInfo;
using zhi::firmware::RawProgramParser;
using zhi::firmware::PatchParser;
using zhi::firmware::ScatterParser;
using zhi::firmware::RawProgramRecord;
using zhi::firmware::ScatterPartition;

namespace {
std::string ReadFileToString(const QString& path) {
    std::ifstream f(path.toStdWString(), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("ZHI-Flashtools");
    resize(1280, 780);
    setWindowIcon(QIcon(":/resources/app-icon.ico")); // 需要在 .qrc 里登记，见 README

    // 先尝试启用 Mica 背景（需要 Win11 22H2+，低版本会静默失败返回 false），
    // 成功了才用"玻璃模式"样式表（半透明背景），否则退化成普通不透明浅色主题——
    // 这个顺序很重要：如果 Mica 没启用成功却用了半透明样式表，界面会变成
    // 一片灰蒙蒙很难看，所以必须先看 EnableMicaBackdrop() 的返回值再决定。
    setAttribute(Qt::WA_TranslucentBackground);
    bool micaOk = EnableMicaBackdrop(this, BackdropType::Mica, false);
    setStyleSheet(BuildStyleSheet(micaOk));
    if (!micaOk) {
        setAttribute(Qt::WA_TranslucentBackground, false); // 没启用成功就别留半透明属性，退回不透明
    }

    BuildMenuBar();

    auto* central = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    BuildNavPanel();
    BuildContentPanels();
    BuildLogPanel();

    // 设备选择是跨面板共享的：ADB/Fastboot/EDL/MTK 用的是同一个下拉框，
    // 扫描一次结果大家都能用，不用每个面板各自扫一遍
    auto* rightSide = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightSide);
    rightLayout->setContentsMargins(12, 10, 12, 10);
    rightLayout->setSpacing(8);

    auto* deviceBar = new QHBoxLayout();
    deviceCombo_ = new QComboBox(rightSide);
    deviceCombo_->setMinimumWidth(320);
    deviceCombo_->addItem("正在自动检测设备…");
    auto* refreshBtn = new QPushButton("立即重新扫描", rightSide);
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::OnRefreshDevicesClicked);
    deviceBar->addWidget(new QLabel("当前设备：", rightSide));
    deviceBar->addWidget(deviceCombo_, 1);
    deviceBar->addWidget(refreshBtn);
    rightLayout->addLayout(deviceBar);
    rightLayout->addWidget(stack_, 1);

    auto* splitter = new QSplitter(Qt::Horizontal, central);
    splitter->addWidget(nav_);
    splitter->addWidget(rightSide);
    splitter->addWidget(logView_->parentWidget()); // 日志面板整体（含标题/过滤条）
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    rootLayout->addWidget(splitter);

    setCentralWidget(central);

    connStatusLabel_ = new QLabel("设备：未连接", this);
    statusBar()->addPermanentWidget(connStatusLabel_);
    statusBar()->showMessage("就绪");

    WireLoggerSink();

    // 自动检测：不用用户手动点"刷新"，启动后立刻扫一次，然后每2秒静默扫一次，
    // 只有设备列表真的变化了才更新下拉框/打日志（AutoScanDevices内部做了去重判断）
    AutoScanDevices();
    autoScanTimer_ = new QTimer(this);
    connect(autoScanTimer_, &QTimer::timeout, this, &MainWindow::AutoScanDevices);
    autoScanTimer_->start(2000);

    zhi::Logger::Instance().Info("欢迎使用 ZHI-Flashtools");
    if (micaOk) {
        zhi::Logger::Instance().Info("Mica 背景已启用");
    } else {
        zhi::Logger::Instance().Info("当前系统不支持 Mica 背景（需要 Win11 22H2+），已使用普通主题");
    }
}

MainWindow::~MainWindow() {
    zhi::Logger::Instance().SetSink(nullptr); // 窗口销毁后不能再往已释放的UI里写日志
}

void MainWindow::WireLoggerSink() {
    // Logger 的调用方可能在工作线程（USB/协议操作跑在 std::thread 里），
    // 不能直接操作 QPlainTextEdit（Qt Widgets 不是线程安全的），
    // 用 QMetaObject::invokeMethod + Qt::QueuedConnection 把日志安全地
    // 转发回主线程再显示。
    zhi::Logger::Instance().SetSink([this](zhi::LogLevel level, const std::string& msg) {
        QMetaObject::invokeMethod(this, "AppendLogLine", Qt::QueuedConnection,
            Q_ARG(int, static_cast<int>(level)), Q_ARG(QString, QString::fromStdString(msg)));
    });
}

void MainWindow::AppendLogLine(int level, const QString& text) {
    QString prefix;
    switch (static_cast<zhi::LogLevel>(level)) {
        case zhi::LogLevel::Success: prefix = "[成功] "; break;
        case zhi::LogLevel::Warning: prefix = "[警告] "; break;
        case zhi::LogLevel::Error:   prefix = "[错误] "; break;
        default: prefix = "[信息] "; break;
    }
    logView_->appendPlainText(prefix + text);
}

void MainWindow::BuildMenuBar() {
    auto* mb = menuBar();

    auto* funcMenu = mb->addMenu("功能");
    funcMenu->addAction("ADB 功能", this, [this]{ nav_->setCurrentRow(0); });
    funcMenu->addAction("Fastboot 功能", this, [this]{ nav_->setCurrentRow(1); });
    funcMenu->addAction("9008 功能", this, [this]{ nav_->setCurrentRow(2); });
    funcMenu->addAction("MTK 线刷", this, [this]{ nav_->setCurrentRow(3); });
    funcMenu->addAction("镜像解析", this, [this]{ nav_->setCurrentRow(4); });
    funcMenu->addAction("分区备份还原", this, [this]{ nav_->setCurrentRow(5); });
    funcMenu->addAction("QCN 功能", this, [this]{ nav_->setCurrentRow(6); });

    auto* rebootMenu = mb->addMenu("高级重启");
    rebootMenu->addAction("重启到 Fastboot", this, [this]{ OnMenuRebootTo("bootloader"); });
    rebootMenu->addAction("重启到 EDL", this, [this]{ OnMenuRebootTo("edl"); });
    rebootMenu->addAction("重启到 Recovery", this, [this]{ OnMenuRebootTo("recovery"); });
    // 这几个命令走的是 ADB shell "reboot xxx"，前提是设备当前处于系统/ADB可访问状态；
    // 如果设备已经在 Fastboot 模式，请改用 Fastboot 面板自己的重启功能

    auto* qcMenu = mb->addMenu("高通功能");
    qcMenu->addAction("EDL 通用恢复出厂", this, &MainWindow::OnEdlFactoryReset);
    qcMenu->addAction("EDL 备份基带分区", this, &MainWindow::OnEdlBackupModem);
    // 这两个命令依赖 9008 EDL 面板里已经选好的 Loader + rawprogram.xml，
    // 用之前需要先去 EDL 面板选好这两个文件（复用面板里的状态，避免重复选择）

    auto* auxMenu = mb->addMenu("辅助功能");
    auxMenu->addAction("设备管理器", this, [this]{ nav_->setCurrentRow(7); });
    auxMenu->addAction("CMD 指令");
}

void MainWindow::BuildNavPanel() {
    nav_ = new QListWidget(this);
    nav_->setObjectName("navList");
    nav_->setFixedWidth(168);
    nav_->addItem("ADB 功能");
    nav_->addItem("Fastboot 功能");
    nav_->addItem("9008 EDL 烧录");
    nav_->addItem("MTK 线刷救砖");
    nav_->addItem("镜像解析");
    nav_->addItem("分区备份还原");
    nav_->addItem("QCN 功能");
    nav_->addItem("驱动管理");
    nav_->setCurrentRow(0);

    connect(nav_, &QListWidget::currentRowChanged, this, &MainWindow::OnNavItemChanged);
}

void MainWindow::BuildContentPanels() {
    stack_ = new QStackedWidget(this);
    stack_->addWidget(BuildAdbPanel());
    stack_->addWidget(BuildFastbootPanel());
    stack_->addWidget(BuildEdlPanel());
    stack_->addWidget(BuildMtkPanel());
    stack_->addWidget(BuildParsePanel());
    stack_->addWidget(BuildBackupPanel());
    stack_->addWidget(BuildQcnPanel());
    stack_->addWidget(BuildDriverPanel());
}

void MainWindow::BuildLogPanel() {
    auto* container = new QWidget(this);
    container->setObjectName("logPanel");
    container->setFixedWidth(300);
    auto* layout = new QVBoxLayout(container);

    auto* title = new QLabel("日志信息", container);
    title->setStyleSheet("font-weight:700; font-size:13px;");
    layout->addWidget(title);

    logView_ = new QPlainTextEdit(container);
    logView_->setObjectName("logView");
    logView_->setReadOnly(true);
    layout->addWidget(logView_);

    auto* clearBtn = new QPushButton("清空日志", container);
    connect(clearBtn, &QPushButton::clicked, this, [this]{ logView_->clear(); });
    layout->addWidget(clearBtn);
}

QWidget* MainWindow::BuildAdbPanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);

    auto* infoBox = new QGroupBox("ADB 设备信息", panel);
    auto* infoLayout = new QVBoxLayout(infoBox);
    adbDeviceInfoLabel_ = new QLabel("未连接", infoBox);
    infoLayout->addWidget(adbDeviceInfoLabel_);
    layout->addWidget(infoBox);

    auto* btnRow = new QHBoxLayout();
    auto* autoDetectBtn = new QPushButton("🔍 自动检测端口（仅ADB）", panel);
    connect(autoDetectBtn, &QPushButton::clicked, this, &MainWindow::OnAdbAutoDetect);
    auto* connectBtn = new QPushButton("连接设备（ADB鉴权）", panel);
    connectBtn->setObjectName("primary");
    connect(connectBtn, &QPushButton::clicked, this, &MainWindow::OnAdbConnectClicked);
    auto* readPartBtn = new QPushButton("读取分区", panel);
    connect(readPartBtn, &QPushButton::clicked, this, &MainWindow::OnAdbReadPartitions);
    btnRow->addWidget(autoDetectBtn);
    btnRow->addWidget(connectBtn);
    btnRow->addWidget(readPartBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    adbPartitionTable_ = new QTableWidget(0, 3, panel);
    adbPartitionTable_->setHorizontalHeaderLabels({"分区名称", "块数(1024字节/块)", "大小(约)"});
    adbPartitionTable_->horizontalHeader()->setStretchLastSection(true);
    adbPartitionTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(adbPartitionTable_, 1);

    auto* pushBox = new QGroupBox("推送文件到设备", panel);
    auto* pushLayout = new QVBoxLayout(pushBox);
    auto* localRow = new QHBoxLayout();
    localRow->addWidget(new QLabel("本地文件", pushBox));
    adbPushLocalPath_ = new QLineEdit(pushBox);
    adbPushLocalPath_->setReadOnly(true);
    adbPushLocalPath_->setPlaceholderText("未选择");
    auto* browseBtn = new QPushButton("浏览", pushBox);
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "选择要推送的文件");
        if (!path.isEmpty()) adbPushLocalPath_->setText(path);
    });
    localRow->addWidget(adbPushLocalPath_, 1);
    localRow->addWidget(browseBtn);
    pushLayout->addLayout(localRow);

    auto* remoteRow = new QHBoxLayout();
    remoteRow->addWidget(new QLabel("设备路径", pushBox));
    adbPushRemotePath_ = new QLineEdit(pushBox);
    adbPushRemotePath_->setPlaceholderText("/sdcard/文件名");
    auto* pushBtn = new QPushButton("推送", pushBox);
    pushBtn->setObjectName("primary");
    connect(pushBtn, &QPushButton::clicked, this, &MainWindow::OnAdbPushFile);
    remoteRow->addWidget(adbPushRemotePath_, 1);
    remoteRow->addWidget(pushBtn);
    pushLayout->addLayout(remoteRow);

    layout->addWidget(pushBox);
    return panel;
}

QWidget* MainWindow::BuildFastbootPanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);

    auto* btnRow = new QHBoxLayout();
    auto* autoDetectBtn = new QPushButton("🔍 自动检测端口（仅Fastboot）", panel);
    connect(autoDetectBtn, &QPushButton::clicked, this, &MainWindow::OnFastbootAutoDetect);
    auto* getVarBtn = new QPushButton("读取设备信息 (getvar:product)", panel);
    connect(getVarBtn, &QPushButton::clicked, this, &MainWindow::OnFastbootGetVarClicked);
    auto* getAllBtn = new QPushButton("读取全部信息 (getvar:all)", panel);
    getAllBtn->setObjectName("primary");
    connect(getAllBtn, &QPushButton::clicked, this, &MainWindow::OnFastbootGetAllVars);
    btnRow->addWidget(autoDetectBtn);
    btnRow->addWidget(getVarBtn);
    btnRow->addWidget(getAllBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    fastbootInfoTable_ = new QTableWidget(0, 2, panel);
    fastbootInfoTable_->setHorizontalHeaderLabels({"变量名", "值"});
    fastbootInfoTable_->horizontalHeader()->setStretchLastSection(true);
    fastbootInfoTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(fastbootInfoTable_, 1);

    return panel;
}

QWidget* MainWindow::BuildEdlPanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);

    // ---- 机型方案选择 ----
    auto* brandRow = new QHBoxLayout();
    brandRow->addWidget(new QLabel("机型方案：", panel));
    auto* xiaomiBtn = new QPushButton("小米", panel);
    auto* oujiaBtn = new QPushButton("欧加", panel);
    auto* genericBtn = new QPushButton("安卓通用", panel);
    xiaomiBtn->setCheckable(true);
    oujiaBtn->setCheckable(true);
    genericBtn->setCheckable(true);
    genericBtn->setChecked(true);
    // 用 objectName 让 QSS 能给选中态上色（在 Theme.h 里配了 QPushButton:checked 样式）
    xiaomiBtn->setObjectName("brandBtn");
    oujiaBtn->setObjectName("brandBtn");
    genericBtn->setObjectName("brandBtn");
    connect(xiaomiBtn, &QPushButton::clicked, this, [this, xiaomiBtn, oujiaBtn, genericBtn]() {
        xiaomiBtn->setChecked(true); oujiaBtn->setChecked(false); genericBtn->setChecked(false);
        OnEdlBrandChanged("xiaomi");
    });
    connect(oujiaBtn, &QPushButton::clicked, this, [this, xiaomiBtn, oujiaBtn, genericBtn]() {
        xiaomiBtn->setChecked(false); oujiaBtn->setChecked(true); genericBtn->setChecked(false);
        OnEdlBrandChanged("oujia");
    });
    connect(genericBtn, &QPushButton::clicked, this, [this, xiaomiBtn, oujiaBtn, genericBtn]() {
        xiaomiBtn->setChecked(false); oujiaBtn->setChecked(false); genericBtn->setChecked(true);
        OnEdlBrandChanged("generic");
    });
    brandRow->addWidget(xiaomiBtn);
    brandRow->addWidget(oujiaBtn);
    brandRow->addWidget(genericBtn);
    brandRow->addStretch();
    layout->addLayout(brandRow);

    // ---- 文件选择区 ----
    auto* fileBox = new QGroupBox("固件文件", panel);
    auto* fileLayout = new QVBoxLayout(fileBox);

    auto addFileRow = [&](QWidget* parentBox, QVBoxLayout* parentLayout,
                          const QString& label, QLineEdit*& target, const QString& filter) {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label, parentBox));
        target = new QLineEdit(parentBox);
        target->setReadOnly(true);
        target->setPlaceholderText("未选择");
        auto* browseBtn = new QPushButton("浏览", parentBox);
        connect(browseBtn, &QPushButton::clicked, this, [this, target, filter]() {
            QString path = QFileDialog::getOpenFileName(this, "选择文件", QString(), filter);
            if (!path.isEmpty()) target->setText(path);
        });
        row->addWidget(target, 1);
        row->addWidget(browseBtn);
        parentLayout->addLayout(row);
    };

    addFileRow(fileBox, fileLayout, "引导文件", edlLoaderPath_, "Loader (*.elf *.mbn);;所有文件 (*)");

    // 欧加专属：Digest + Signature，放在一个可显隐的子容器里
    edlOujiaBox_ = new QWidget(fileBox);
    auto* oujiaLayout = new QVBoxLayout(edlOujiaBox_);
    oujiaLayout->setContentsMargins(0, 0, 0, 0);
    addFileRow(edlOujiaBox_, oujiaLayout, "Digest", edlDigestPath_, "Digest 文件 (*);;所有文件 (*)");
    addFileRow(edlOujiaBox_, oujiaLayout, "Signature", edlSignaturePath_, "签名文件 (*);;所有文件 (*)");
    edlOujiaBox_->setVisible(false);
    fileLayout->addWidget(edlOujiaBox_);

    edlBrandNote_ = new QLabel(
        "三种方案都只需要引导文件（欧加额外需要 Digest + Signature），不需要单独导入"
        " rawprogram/patch——分区信息由引导文件握手后直接从设备读取。", fileBox);
    edlBrandNote_->setStyleSheet("color:#6b7280; font-size:11px;");
    edlBrandNote_->setWordWrap(true);
    fileLayout->addWidget(edlBrandNote_);

    layout->addWidget(fileBox);

    // ---- 分区表（握手后从设备读，不再本地解析rawprogram）----
    edlTable_ = new QTableWidget(0, 5, panel);
    edlTable_->setHorizontalHeaderLabels({"分区", "起始扇区", "扇区数", "LUN", "状态"});
    edlTable_->horizontalHeader()->setStretchLastSection(true);
    edlTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(edlTable_, 1);

    edlProgress_ = new QProgressBar(panel);
    edlProgress_->setRange(0, 100);
    layout->addWidget(edlProgress_);

    auto* btnRow = new QHBoxLayout();
    edlStartBtn_ = new QPushButton("连接并读取分区表", panel);
    edlStartBtn_->setObjectName("primary");
    connect(edlStartBtn_, &QPushButton::clicked, this, &MainWindow::OnEdlStartFlash);
    btnRow->addWidget(edlStartBtn_);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    return panel;
}

void MainWindow::OnEdlBrandChanged(const QString& brand) {
    edlBrand_ = brand;
    edlOujiaBox_->setVisible(brand == "oujia");

    if (brand == "xiaomi") {
        edlStartBtn_->setText("验证 Loader（Sahara握手）");
        edlBrandNote_->setText("小米方案只需要引导文件，点击按钮做 Sahara 握手验证，"
            "配合菜单栏「高级功能」里的小米相关操作使用。");
    } else if (brand == "oujia") {
        edlStartBtn_->setText("验证 Loader（含Digest+Signature）");
        edlBrandNote_->setText("欧加（OPPO/一加/realme系）方案需要引导文件 + Digest + Signature "
            "三个文件，走多镜像 Sahara 握手。注意：imageId 与文件的对应顺序尚未真机验证，"
            "见下方日志，如握手卡住需按抓包结果调整顺序。");
    } else {
        edlStartBtn_->setText("连接并读取分区表");
        edlBrandNote_->setText("安卓通用方案只需要引导文件，握手成功后从设备读取 GPT 分区表。");
    }
}

void MainWindow::RefreshEdlTableFromDevice() {
    // 占位：真机联通后，这里应该用 FirehoseProtocol 读设备的 GPT 分区表
    // （<read> 出 primary GPT，解析成分区列表填进表格）。GPT 解析逻辑还没写，
    // 先留接口，握手打通后补。
    zhi::Logger::Instance().Info("（分区表读取功能待补：需要解析设备 primary GPT）");
}

void MainWindow::OnEdlStartFlash() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先在顶部选中一个 9008 EDL 设备");
        return;
    }
    if (edlLoaderPath_->text().isEmpty()) {
        zhi::Logger::Instance().Warning("请先选择引导文件（Firehose Loader）");
        return;
    }

    QString brand = edlBrand_;
    QString loaderPath = edlLoaderPath_->text();
    QString digestPath = edlDigestPath_ ? edlDigestPath_->text() : QString();
    QString signaturePath = edlSignaturePath_ ? edlSignaturePath_->text() : QString();

    if (brand == "oujia" && (digestPath.isEmpty() || signaturePath.isEmpty())) {
        zhi::Logger::Instance().Warning("欧加方案需要同时选择 Digest 和 Signature 文件");
        return;
    }

    std::thread([this, devicePath, brand, loaderPath, digestPath, signaturePath]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        SaharaProtocol sahara(device);
        bool handshakeOk = false;

        if (brand == "oujia") {
            // 欧加：多镜像握手，按 引导→Digest→Signature 顺序（顺序未真机验证）
            std::vector<std::wstring> images = {
                loaderPath.toStdWString(),
                digestPath.toStdWString(),
                signaturePath.toStdWString(),
            };
            handshakeOk = sahara.PerformHandshakeMultiImage(images, &err);
        } else {
            // 小米 / 安卓通用：单文件握手
            handshakeOk = sahara.PerformHandshake(loaderPath.toStdWString(), &err);
        }

        if (!handshakeOk) {
            zhi::Logger::Instance().Error("Sahara 握手失败: " + err);
            return;
        }

        FirehoseProtocol firehose(device);
        if (!firehose.Configure(1048576, &err)) {
            zhi::Logger::Instance().Error("Firehose Configure 失败: " + err);
            return;
        }

        StorageInfo storageInfo;
        if (firehose.GetStorageInfo(0, &storageInfo, &err)) {
            zhi::Logger::Instance().Success("Loader 验证成功，设备磁盘总容量 " +
                std::to_string(storageInfo.totalBlocks) + " 块");
        } else {
            zhi::Logger::Instance().Warning("已握手但读取磁盘信息失败: " + err);
        }

        if (brand == "xiaomi") {
            zhi::Logger::Instance().Success("小米 Loader 握手验证完成，可继续使用高级功能里的小米操作");
        } else if (brand == "oujia") {
            zhi::Logger::Instance().Success("欧加多镜像 Loader 握手验证完成");
        } else {
            zhi::Logger::Instance().Success("Loader 握手成功，接下来可读取分区表进行刷写");
            QMetaObject::invokeMethod(this, [this]{ RefreshEdlTableFromDevice(); }, Qt::QueuedConnection);
        }
    }).detach();
}

QWidget* MainWindow::BuildMtkPanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);

    auto* warnLabel = new QLabel(
        "⚠ DA 命令字节（DaOpcodes）尚未实机验证，见 README 说明，"
        "首次对接新机型建议先只测到\"识别芯片\"这一步。", panel);
    warnLabel->setStyleSheet("color:#c2760a; font-size:11.5px;");
    layout->addWidget(warnLabel);

    auto* fileBox = new QGroupBox("固件文件", panel);
    auto* fileLayout = new QVBoxLayout(fileBox);

    auto addFileRow = [&](const QString& label, QLineEdit*& target, const QString& filter) {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label, fileBox));
        target = new QLineEdit(fileBox);
        target->setReadOnly(true);
        target->setPlaceholderText("未选择");
        auto* browseBtn = new QPushButton("浏览", fileBox);
        connect(browseBtn, &QPushButton::clicked, this, [this, target, filter]() {
            QString path = QFileDialog::getOpenFileName(this, "选择文件", QString(), filter);
            if (!path.isEmpty()) {
                target->setText(path);
                if (target == mtkScatterPath_) RefreshMtkTable();
            }
        });
        row->addWidget(target, 1);
        row->addWidget(browseBtn);
        fileLayout->addLayout(row);
    };

    addFileRow("Scatter 文件", mtkScatterPath_, "Scatter (*.txt)");
    addFileRow("DA 文件", mtkDaPath_, "DA (*.bin);;所有文件 (*)");
    layout->addWidget(fileBox);

    mtkTable_ = new QTableWidget(0, 4, panel);
    mtkTable_->setHorizontalHeaderLabels({"分区", "物理地址", "大小", "镜像文件"});
    mtkTable_->horizontalHeader()->setStretchLastSection(true);
    mtkTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(mtkTable_, 1);

    mtkProgress_ = new QProgressBar(panel);
    mtkProgress_->setRange(0, 100);
    layout->addWidget(mtkProgress_);

    auto* btnRow = new QHBoxLayout();
    auto* startBtn = new QPushButton("开始线刷", panel);
    startBtn->setObjectName("primary");
    connect(startBtn, &QPushButton::clicked, this, &MainWindow::OnMtkStartFlash);
    btnRow->addWidget(startBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    return panel;
}

void MainWindow::RefreshMtkTable() {
    if (mtkScatterPath_->text().isEmpty()) return;

    std::string content = ReadFileToString(mtkScatterPath_->text());
    auto partitions = ScatterParser::Parse(content);

    mtkTable_->setRowCount(static_cast<int>(partitions.size()));
    for (int i = 0; i < static_cast<int>(partitions.size()); ++i) {
        const auto& p = partitions[i];
        mtkTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(p.partitionName)));
        mtkTable_->setItem(i, 1, new QTableWidgetItem(
            QString("0x%1").arg(p.physicalStartAddr, 0, 16)));
        mtkTable_->setItem(i, 2, new QTableWidgetItem(
            QString("0x%1").arg(p.partitionSize, 0, 16)));
        mtkTable_->setItem(i, 3, new QTableWidgetItem(
            p.isDownload ? QString::fromStdString(p.fileName) : "（不下载）"));
    }
    zhi::Logger::Instance().Info("Scatter 预览加载完成，共 " + std::to_string(partitions.size()) + " 个分区");
}

void MainWindow::OnMtkStartFlash() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先在顶部选中一个 BROM 设备");
        return;
    }
    if (mtkScatterPath_->text().isEmpty() || mtkDaPath_->text().isEmpty()) {
        zhi::Logger::Instance().Warning("请先选择 Scatter 文件和 DA 文件");
        return;
    }

    QString scatterPath = mtkScatterPath_->text();
    QString daPath = mtkDaPath_->text();

    std::thread([this, devicePath, scatterPath, daPath]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        BromProtocol brom(device);
        if (!brom.Handshake(20, &err)) {
            zhi::Logger::Instance().Error("BROM 握手失败: " + err);
            return;
        }

        HwInfo hwInfo;
        if (brom.GetHwCode(&hwInfo, &err)) {
            zhi::Logger::Instance().Success("芯片识别 HW_CODE=0x" + std::to_string(hwInfo.hwCode));
        }

        // 读取 DA 文件
        std::ifstream daFile(daPath.toStdWString(), std::ios::binary | std::ios::ate);
        if (!daFile) {
            zhi::Logger::Instance().Error("无法打开 DA 文件");
            return;
        }
        size_t daSize = static_cast<size_t>(daFile.tellg());
        daFile.seekg(0);
        std::vector<uint8_t> daData(daSize);
        daFile.read(reinterpret_cast<char*>(daData.data()), daSize);

        // 注意：loadAddress / signatureLength 是机型相关的参数，这里用的是
        // 常见占位值，实机对接前务必核实（见 README 的 DaOpcodes 说明）
        const uint32_t kDaLoadAddress = 0x40000000;
        const uint32_t kSignatureLength = 0;

        if (!brom.SendDA(daData, kDaLoadAddress, kSignatureLength, &err)) {
            zhi::Logger::Instance().Error("SEND_DA 失败: " + err +
                "（这一步的参数需要按实际机型核实，不是通用值）");
            return;
        }
        if (!brom.JumpDA(kDaLoadAddress, &err)) {
            zhi::Logger::Instance().Error("JUMP_DA 失败: " + err);
            return;
        }

        DaProtocol da(device);
        std::string scatterContent = ReadFileToString(scatterPath);
        auto partitions = ScatterParser::Parse(scatterContent);

        uint64_t total = partitions.size();
        uint64_t done = 0;

        for (const auto& p : partitions) {
            if (!p.isDownload || p.fileName.empty()) { done++; continue; }

            std::ifstream imgFile(p.fileName, std::ios::binary | std::ios::ate);
            if (!imgFile) {
                zhi::Logger::Instance().Error("找不到镜像文件: " + p.fileName +
                    "，跳过分区 " + p.partitionName);
                done++;
                continue;
            }
            size_t size = static_cast<size_t>(imgFile.tellg());
            imgFile.seekg(0);
            std::vector<uint8_t> data(size);
            imgFile.read(reinterpret_cast<char*>(data.data()), size);

            zhi::protocol::mtk::DaPartitionTarget target;
            target.name = p.partitionName;
            target.physicalAddr = p.physicalStartAddr;
            target.length = data.size();

            std::string label = p.partitionName;
            uint64_t doneSnapshot = done;
            bool ok = da.WritePartition(target, data.data(), data.size(),
                [this, doneSnapshot, total](uint64_t sent, uint64_t chunkTotal) {
                    int pct = total == 0 ? 0 :
                        static_cast<int>((doneSnapshot * 100 +
                            (chunkTotal ? sent * 100 / chunkTotal : 0)) / std::max<uint64_t>(1, total));
                    QMetaObject::invokeMethod(this, [this, pct]{ mtkProgress_->setValue(pct); },
                        Qt::QueuedConnection);
                }, &err);

            if (!ok) {
                zhi::Logger::Instance().Error("分区 " + label + " 写入失败: " + err);
                return;
            }
            done++;
        }

        da.Finish(true, &err);
        QMetaObject::invokeMethod(this, [this]{ mtkProgress_->setValue(100); }, Qt::QueuedConnection);
        zhi::Logger::Instance().Success("MTK 线刷流程结束");
    }).detach();
}

QWidget* MainWindow::BuildParsePanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);

    auto* note = new QLabel(
        "选择一个 rawprogram*.xml（高通EDL固件包）或 scatter.txt（MTK线刷包），"
        "本地解析预览分区结构，不需要连设备。", panel);
    note->setStyleSheet("color:#6b7280; font-size:11.5px;");
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* fileRow = new QHBoxLayout();
    parseFilePath_ = new QLineEdit(panel);
    parseFilePath_->setReadOnly(true);
    parseFilePath_->setPlaceholderText("未选择");
    auto* browseBtn = new QPushButton("选择文件", panel);
    browseBtn->setObjectName("primary");
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "选择固件描述文件",
            "", "固件描述文件 (*.xml *.txt);;所有文件 (*)");
        if (!path.isEmpty()) {
            parseFilePath_->setText(path);
            OnParseFile();
        }
    });
    fileRow->addWidget(new QLabel("文件：", panel));
    fileRow->addWidget(parseFilePath_, 1);
    fileRow->addWidget(browseBtn);
    layout->addLayout(fileRow);

    parseFileTypeLabel_ = new QLabel("", panel);
    parseFileTypeLabel_->setStyleSheet("color:#2454d6; font-weight:700; font-size:11.5px;");
    layout->addWidget(parseFileTypeLabel_);

    parseTable_ = new QTableWidget(0, 4, panel);
    parseTable_->setHorizontalHeaderLabels({"分区/标签", "地址/扇区", "大小", "镜像文件"});
    parseTable_->horizontalHeader()->setStretchLastSection(true);
    parseTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(parseTable_, 1);

    return panel;
}

QWidget* MainWindow::BuildBackupPanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);

    auto* note = new QLabel(
        "管理各面板产生的备份文件（EDL备份基带分区 / QCN读取 / ADB读取分区 等\n"
        "都是存成 .bin 文件，这里只是浏览管理，不做备份内容解析）。", panel);
    note->setStyleSheet("color:#6b7280; font-size:11.5px;");
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* dirRow = new QHBoxLayout();
    backupDirPath_ = new QLineEdit(panel);
    backupDirPath_->setReadOnly(true);
    backupDirPath_->setPlaceholderText("未选择备份目录");
    auto* chooseDirBtn = new QPushButton("选择备份目录", panel);
    chooseDirBtn->setObjectName("primary");
    connect(chooseDirBtn, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, "选择备份目录");
        if (!dir.isEmpty()) {
            backupDirPath_->setText(dir);
            OnBackupDirChanged();
        }
    });
    auto* openFolderBtn = new QPushButton("打开文件夹", panel);
    connect(openFolderBtn, &QPushButton::clicked, this, &MainWindow::OnBackupOpenFolder);
    dirRow->addWidget(new QLabel("目录：", panel));
    dirRow->addWidget(backupDirPath_, 1);
    dirRow->addWidget(chooseDirBtn);
    dirRow->addWidget(openFolderBtn);
    layout->addLayout(dirRow);

    backupTable_ = new QTableWidget(0, 3, panel);
    backupTable_->setHorizontalHeaderLabels({"文件名", "大小", "修改时间"});
    backupTable_->horizontalHeader()->setStretchLastSection(true);
    backupTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(backupTable_, 1);

    return panel;
}

QWidget* MainWindow::BuildQcnPanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);

    auto* note = new QLabel(
        "⚠ 老实说明：这里的\"QCN\"不是走高通官方 DIAG/QCDM 协议导出的那种标准\n"
        "QCN文件（那需要单独实现 DIAG 协议，工作量不小，本项目还没做），而是通过\n"
        "已经打通的 EDL/Firehose 通道，把 modemst1 + modemst2 这两个基带配置分区\n"
        "原始读出来拼在一起，是社区里常见的\"平替\"做法，不完全等价于QFIL等官方\n"
        "工具导出的QCN，跨机型/跨基带版本的兼容性没有保证。", panel);
    note->setStyleSheet("color:#c2760a; font-size:11px;");
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* btnRow = new QHBoxLayout();
    auto* readBtn = new QPushButton("读取 QCN（modemst1+modemst2）", panel);
    readBtn->setObjectName("primary");
    connect(readBtn, &QPushButton::clicked, this, &MainWindow::OnQcnRead);
    btnRow->addWidget(readBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    qcnTable_ = new QTableWidget(0, 3, panel);
    qcnTable_->setHorizontalHeaderLabels({"配置项", "当前值", "备注"});
    qcnTable_->horizontalHeader()->setStretchLastSection(true);
    qcnTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(qcnTable_, 1);

    return panel;
}

QWidget* MainWindow::BuildDriverPanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);

    auto* note = new QLabel(
        "说明：这里的检测方式是\"扫描当前是否有设备以对应驱动的接口被枚举到\"，\n"
        "不是查 Windows 驱动商店的安装记录——所以结果只在设备已插入时才准确，\n"
        "设备没插的时候会显示\"未检测到\"，这不代表驱动没装，只是当前没法判断。", panel);
    note->setStyleSheet("color:#6b7280; font-size:11px;");
    note->setWordWrap(true);
    layout->addWidget(note);

    driverTable_ = new QTableWidget(3, 3, panel);
    driverTable_->setHorizontalHeaderLabels({"驱动名称", "用途", "状态"});
    driverTable_->horizontalHeader()->setStretchLastSection(true);
    driverTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    const char* names[3] = {"Google USB Driver", "MediaTek USB VCOM", "Qualcomm QDLoader 9008"};
    const char* purposes[3] = {"ADB / Fastboot 通信", "MTK BROM 端口识别", "EDL 端口识别"};
    for (int i = 0; i < 3; ++i) {
        driverTable_->setItem(i, 0, new QTableWidgetItem(names[i]));
        driverTable_->setItem(i, 1, new QTableWidgetItem(purposes[i]));
        driverTable_->setItem(i, 2, new QTableWidgetItem("未检测"));
    }
    layout->addWidget(driverTable_, 1);

    auto* detectBtn = new QPushButton("检测驱动状态", panel);
    detectBtn->setObjectName("primary");
    connect(detectBtn, &QPushButton::clicked, this, &MainWindow::OnDetectDrivers);
    layout->addWidget(detectBtn);

    return panel;
}

void MainWindow::OnDetectDrivers() {
    // 三行分别对应：Fastboot(Google示例VID/PID) / MTK BROM(社区常见VID/PID) / 高通EDL
    // 同前面反复提醒的一样，Fastboot 和 MTK 的 VID/PID 因厂商/机型而异，
    // 这里只是能跑通检测流程的示例表，接实际产品前要按目标机型换成准确值。
    struct Row { int index; std::vector<UsbDeviceEnumerator::VidPid> targets; };
    std::vector<Row> rows = {
        {0, {{0x18D1, 0x4EE0}}},
        {1, {{0x0E8D, 0x0003}, {0x0E8D, 0x2000}}},
        {2, {{0x05C6, 0x9008}}},
    };

    for (const auto& row : rows) {
        auto found = UsbDeviceEnumerator::Enumerate(row.targets);
        QString status = found.empty() ? "未检测到（设备未插入或驱动未装）" : "已检测到设备，接口正常";
        driverTable_->item(row.index, 2)->setText(status);
    }
    zhi::Logger::Instance().Info("驱动检测完成");
}

void MainWindow::OnNavItemChanged(int row) {
    if (row >= 0) stack_->setCurrentIndex(row);
}

void MainWindow::OnRefreshDevicesClicked() {
    RefreshDeviceCombo();
}

// targets 表在几个地方共用（手动刷新/自动扫描/驱动检测），抽成一个小helper，
// 免得改一处漏改另一处
static std::vector<UsbDeviceEnumerator::VidPid> ScanTargets() {
    // 示例：同时探测 Fastboot 常见 VID/PID 和高通 9008、MTK BROM。
    // 真实项目里这张表要按厂商细分，不同品牌 Fastboot 接口的 VID/PID 不一样，
    // 这里先给一个能跑通流程的最小示例，接实际产品前要按目标机型核实。
    return {
        {0x18D1, 0x4EE0}, // Google 常见 Fastboot 接口示例
        {0x05C6, 0x9008}, // 高通 EDL
        {0x0E8D, 0x0003}, // MTK BROM 示例
        {0x0E8D, 0x2000}, // MTK BROM 示例
    };
}

// !! 重要老实话 !!
// 跟高通EDL固定是 05C6:9008 不一样，ADB/Fastboot 没有统一的 VID/PID——
// 每个手机厂商用的都是自己申请的 VID（小米/三星/OnePlus/华为各不相同），
// 同一个厂商 ADB 模式和 Fastboot 模式的 PID 通常也不一样。所以"仅ADB扫描"
// "仅Fastboot扫描"这种按VID/PID过滤的方式，本质上只能维护一张常见厂商表，
// 不可能覆盖所有机型——更严谨的做法是按 USB 接口的 Class/SubClass/Protocol
// （ADB接口标准是 0xFF/0x42/0x01）匹配，不看VID/PID，但那需要改
// UsbDeviceEnumerator::Enumerate() 的枚举方式（目前是按VID/PID查，不是按
// 接口描述符查），这是后续可以做的改进，先用VID/PID表顶上能跑通流程。
static std::vector<UsbDeviceEnumerator::VidPid> AdbScanTargets() {
    return {
        {0x18D1, 0x4EE7}, // Google/AOSP 设备 ADB 接口示例
        {0x2717, 0xFF40}, // 小米 ADB 接口示例
        {0x22D9, 0x2764}, // OPPO/OnePlus 系 ADB 接口示例（数值需核实）
        {0x04E8, 0x6860}, // 三星 ADB 接口示例
    };
}

static std::vector<UsbDeviceEnumerator::VidPid> FastbootScanTargets() {
    return {
        {0x18D1, 0x4EE0}, // Google/AOSP 设备 Fastboot 接口示例
        {0x2717, 0xFF80}, // 小米 Fastboot 接口示例（数值需核实）
        {0x05C6, 0x9008}, // 部分高通平台 Fastboot 场景也可能复用EDL通道，具体因机型而异
    };
}

void MainWindow::RefreshDeviceCombo() {
    lastScanResults_ = UsbDeviceEnumerator::Enumerate(ScanTargets());

    deviceCombo_->clear();
    if (lastScanResults_.empty()) {
        deviceCombo_->addItem("未检测到设备");
        zhi::Logger::Instance().Warning("未扫描到匹配设备，请检查驱动是否安装、设备是否已进入对应模式");
        lastScanNames_.clear();
        return;
    }

    lastScanNames_.clear();
    for (const auto& d : lastScanResults_) {
        QString label = QString::fromStdWString(d.friendlyName);
        deviceCombo_->addItem(label);
        lastScanNames_.push_back(label);
    }
    zhi::Logger::Instance().Success("扫描到 " + std::to_string(lastScanResults_.size()) + " 个设备");
}

void MainWindow::AutoScanDevices() {
    // 静默扫描：跟上次结果一样就什么都不做，不刷屏、不重建下拉框
    // （重建下拉框会打断用户正在选的项，所以"没变化就不动"很重要）
    auto results = UsbDeviceEnumerator::Enumerate(ScanTargets());

    QStringList names;
    for (const auto& d : results) names.push_back(QString::fromStdWString(d.friendlyName));

    if (names == lastScanNames_) return; // 没变化，跳过

    QString previousSelection = deviceCombo_->currentText();

    lastScanResults_ = results;
    lastScanNames_ = names;

    deviceCombo_->clear();
    if (results.empty()) {
        deviceCombo_->addItem("未检测到设备");
    } else {
        for (const auto& n : names) deviceCombo_->addItem(n);
        // 尽量保留用户之前选的那个设备（如果它还在新的列表里）
        int keepIdx = deviceCombo_->findText(previousSelection);
        deviceCombo_->setCurrentIndex(keepIdx >= 0 ? keepIdx : 0);
    }

    if (!results.empty()) {
        zhi::Logger::Instance().Info("设备列表变化：当前检测到 " + std::to_string(results.size()) + " 个设备");
    } else {
        zhi::Logger::Instance().Info("设备已断开");
    }
}

std::wstring MainWindow::SelectedDevicePath() const {
    int idx = deviceCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(lastScanResults_.size())) return L"";
    return lastScanResults_[idx].devicePath;
}

void MainWindow::OnAdbConnectClicked() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先点击\"刷新设备\"并选中一个设备");
        return;
    }

    std::thread([this, devicePath]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        AdbProtocol adb(device);
        std::string banner;
        std::wstring keyPath = L"adbkey"; // 实际应放到 %APPDATA%\ZHI-Flashtools\ 下
        if (!adb.Connect(keyPath, "unknown@zhi-flashtools", &banner, &err)) {
            zhi::Logger::Instance().Error("ADB 连接失败: " + err);
            return;
        }

        QString bannerQ = QString::fromStdString(banner);
        QMetaObject::invokeMethod(this, [this, bannerQ]{
            adbDeviceInfoLabel_->setText(bannerQ);
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::OnAdbAutoDetect() {
    zhi::Logger::Instance().Info("正在扫描 ADB 端口…");

    auto found = UsbDeviceEnumerator::Enumerate(AdbScanTargets());

    if (found.empty()) {
        zhi::Logger::Instance().Warning("未检测到 ADB 设备（设备需处于系统模式且已开启USB调试；"
            "注意 ADB 的VID/PID因厂商而异，当前只覆盖了几个常见厂商的示例值，"
            "如果确认设备已连接但扫不到，可能是VID/PID表里没有你的机型）");
        return;
    }

    // 找到了就直接更新共享下拉框并自动选中第一个匹配项，省得用户再手动点"刷新设备"
    lastScanResults_ = found;
    deviceCombo_->clear();
    lastScanNames_.clear();
    for (const auto& d : found) {
        QString label = QString::fromStdWString(d.friendlyName);
        deviceCombo_->addItem(label);
        lastScanNames_.push_back(label);
    }
    deviceCombo_->setCurrentIndex(0);

    zhi::Logger::Instance().Success("ADB 端口检测到 " + std::to_string(found.size()) + " 个设备，已自动选中");
}

void MainWindow::OnFastbootGetVarClicked() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先点击\"刷新设备\"并选中一个设备");
        return;
    }

    std::thread([this, devicePath]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        FastbootProtocol fb(device);
        std::string product;
        if (!fb.GetVar("product", &product)) {
            zhi::Logger::Instance().Error("getvar:product 失败");
            return;
        }
        zhi::Logger::Instance().Success("product = " + product);
    }).detach();
}

void MainWindow::OnFastbootAutoDetect() {
    zhi::Logger::Instance().Info("正在扫描 Fastboot 端口…");

    auto found = UsbDeviceEnumerator::Enumerate(FastbootScanTargets());

    if (found.empty()) {
        zhi::Logger::Instance().Warning("未检测到 Fastboot 设备（设备需处于 Bootloader 模式；"
            "同样提醒：Fastboot的VID/PID因厂商而异，当前只覆盖了几个常见厂商的示例值）");
        return;
    }

    lastScanResults_ = found;
    deviceCombo_->clear();
    lastScanNames_.clear();
    for (const auto& d : found) {
        QString label = QString::fromStdWString(d.friendlyName);
        deviceCombo_->addItem(label);
        lastScanNames_.push_back(label);
    }
    deviceCombo_->setCurrentIndex(0);

    zhi::Logger::Instance().Success("Fastboot 端口检测到 " + std::to_string(found.size()) + " 个设备，已自动选中");
}

void MainWindow::OnFastbootGetAllVars() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先选中一个 Fastboot 设备");
        return;
    }

    std::thread([this, devicePath]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        FastbootProtocol fb(device);
        std::vector<std::pair<std::string, std::string>> vars;
        if (!fb.GetAllVars(&vars)) {
            zhi::Logger::Instance().Error("getvar:all 失败");
            return;
        }

        QMetaObject::invokeMethod(this, [this, vars]() {
            fastbootInfoTable_->setRowCount(static_cast<int>(vars.size()));
            for (int i = 0; i < static_cast<int>(vars.size()); ++i) {
                fastbootInfoTable_->setItem(i, 0, new QTableWidgetItem(
                    QString::fromStdString(vars[i].first)));
                fastbootInfoTable_->setItem(i, 1, new QTableWidgetItem(
                    QString::fromStdString(vars[i].second)));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::OnAdbReadPartitions() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先选中一个 ADB 设备");
        return;
    }

    std::thread([this, devicePath]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        AdbProtocol adb(device);
        std::string banner;
        std::wstring keyPath = L"adbkey";
        if (!adb.Connect(keyPath, "unknown@zhi-flashtools", &banner, &err)) {
            zhi::Logger::Instance().Error("ADB 连接失败: " + err);
            return;
        }

        std::string output;
        if (!adb.Shell("cat /proc/partitions", &output, &err)) {
            zhi::Logger::Instance().Error("读取分区信息失败: " + err);
            return;
        }

        // /proc/partitions 格式：major minor  #blocks  name（首行是表头，忽略）
        struct PartRow { QString name; QString blocks; QString sizeApprox; };
        std::vector<PartRow> rows;
        std::istringstream iss(output);
        std::string line;
        bool firstLine = true;
        while (std::getline(iss, line)) {
            if (firstLine) { firstLine = false; continue; } // 跳过表头
            std::istringstream ls(line);
            std::string major, minor, blocks, name;
            if (!(ls >> major >> minor >> blocks >> name)) continue;
            if (name.empty()) continue;

            uint64_t blockCount = strtoull(blocks.c_str(), nullptr, 10);
            double mb = static_cast<double>(blockCount) / 1024.0; // 1块=1024字节，换算成MB
            QString sizeStr = QString::number(mb, 'f', 1) + " MB";

            rows.push_back({QString::fromStdString(name), QString::fromStdString(blocks), sizeStr});
        }

        QMetaObject::invokeMethod(this, [this, rows]() {
            adbPartitionTable_->setRowCount(static_cast<int>(rows.size()));
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                adbPartitionTable_->setItem(i, 0, new QTableWidgetItem(rows[i].name));
                adbPartitionTable_->setItem(i, 1, new QTableWidgetItem(rows[i].blocks));
                adbPartitionTable_->setItem(i, 2, new QTableWidgetItem(rows[i].sizeApprox));
            }
        }, Qt::QueuedConnection);

        zhi::Logger::Instance().Success("分区信息读取完成，共 " + std::to_string(rows.size()) + " 条");
    }).detach();
}

void MainWindow::OnAdbPushFile() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先选中一个 ADB 设备");
        return;
    }
    if (adbPushLocalPath_->text().isEmpty() || adbPushRemotePath_->text().isEmpty()) {
        zhi::Logger::Instance().Warning("请先选择本地文件并填写设备路径");
        return;
    }

    QString localPath = adbPushLocalPath_->text();
    QString remotePath = adbPushRemotePath_->text();

    std::thread([this, devicePath, localPath, remotePath]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        AdbProtocol adb(device);
        std::string banner;
        std::wstring keyPath = L"adbkey";
        if (!adb.Connect(keyPath, "unknown@zhi-flashtools", &banner, &err)) {
            zhi::Logger::Instance().Error("ADB 连接失败: " + err);
            return;
        }

        if (!adb.Push(localPath.toStdWString(), remotePath.toStdString(), 0100644, &err)) {
            zhi::Logger::Instance().Error("推送失败: " + err);
            return;
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// 顶部菜单：高级重启 / 高通功能
// ---------------------------------------------------------------------------
void MainWindow::OnMenuRebootTo(const std::string& target) {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先在顶部选中一个设备");
        return;
    }

    std::thread([this, devicePath, target]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        AdbProtocol adb(device);
        std::string banner;
        std::wstring keyPath = L"adbkey";
        if (!adb.Connect(keyPath, "unknown@zhi-flashtools", &banner, &err)) {
            zhi::Logger::Instance().Error("ADB 连接失败，无法执行重启命令: " + err +
                "（这个命令假设设备当前是系统/ADB可访问状态，如果设备已经在"
                "Fastboot模式，请改用Fastboot面板自己的重启按钮）");
            return;
        }

        std::string output;
        adb.Shell("reboot " + target, &output, &err);
        zhi::Logger::Instance().Success("已发送重启命令: reboot " + target);
    }).detach();
}

void MainWindow::OnEdlBackupModem() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先在顶部选中一个 9008 设备");
        return;
    }
    if (edlLoaderPath_->text().isEmpty()) {
        zhi::Logger::Instance().Warning("请先到「EDL 刷写」面板选好引导文件");
        return;
    }
    // 备份基带需要知道分区的扇区布局，这个信息来自 rawprogram——EDL面板已经
    // 不再固定要求rawprogram了，所以这里临时让用户选一个（有的话），用来定位分区
    QString rawprogramPath = QFileDialog::getOpenFileName(this,
        "选择 rawprogram.xml（用于定位基带分区的扇区布局）", QString(), "XML 文件 (*.xml)");
    if (rawprogramPath.isEmpty()) {
        zhi::Logger::Instance().Warning("未选择 rawprogram，无法定位基带分区位置，已取消");
        return;
    }

    QString saveDir = QFileDialog::getExistingDirectory(this, "选择基带分区备份保存位置");
    if (saveDir.isEmpty()) return;

    QString loaderPath = edlLoaderPath_->text();

    std::thread([this, devicePath, loaderPath, rawprogramPath, saveDir]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        SaharaProtocol sahara(device);
        if (!sahara.PerformHandshake(loaderPath.toStdWString(), &err)) {
            zhi::Logger::Instance().Error("Sahara 握手失败: " + err);
            return;
        }

        FirehoseProtocol firehose(device);
        if (!firehose.Configure(1048576, &err)) {
            zhi::Logger::Instance().Error("Firehose Configure 失败: " + err);
            return;
        }

        StorageInfo storageInfo;
        uint64_t totalDiskSectors = 0;
        if (firehose.GetStorageInfo(0, &storageInfo, &err)) {
            totalDiskSectors = storageInfo.totalBlocks;
        }

        std::ifstream f(rawprogramPath.toStdWString(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();

        auto records = RawProgramParser::Parse(ss.str());
        auto entries = RawProgramParser::Resolve(records, totalDiskSectors, nullptr);

        int backedUp = 0;
        for (const auto& entry : entries) {
            // 基带相关分区常见命名：modem / modemst1 / modemst2（含 "modem" 关键字），
            // 不同厂商可能还有 fsg / fsc 等，这里先覆盖最常见的
            std::string labelLower = entry.label;
            std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(), ::tolower);
            if (labelLower.find("modem") == std::string::npos) continue;

            std::vector<uint8_t> data;
            if (!firehose.Read(entry, &data, nullptr, &err)) {
                zhi::Logger::Instance().Error("分区 " + entry.label + " 读取失败: " + err);
                continue;
            }

            QString outPath = saveDir + "/" + QString::fromStdString(entry.label) + ".bin";
            std::ofstream out(outPath.toStdWString(), std::ios::binary);
            out.write(reinterpret_cast<char*>(data.data()), data.size());
            zhi::Logger::Instance().Success("已备份: " + entry.label + " -> " + outPath.toStdString());
            backedUp++;
        }

        if (backedUp == 0) {
            zhi::Logger::Instance().Warning("rawprogram 里没有找到基带相关分区"
                "（分区名不含 modem），本机型可能用了不同命名，需要手动确认分区名");
        }
    }).detach();
}

void MainWindow::OnEdlFactoryReset() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先在顶部选中一个 9008 设备");
        return;
    }
    if (edlLoaderPath_->text().isEmpty()) {
        zhi::Logger::Instance().Warning("请先到「EDL 刷写」面板选好引导文件");
        return;
    }

    auto reply = QMessageBox::warning(this, "确认恢复出厂设置",
        "即将清空 userdata 分区（用户数据/已安装应用/照片等都会丢失），\n"
        "此操作不可撤销，确定继续吗？",
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (reply != QMessageBox::Yes) return;

    QString rawprogramPath = QFileDialog::getOpenFileName(this,
        "选择 rawprogram.xml（用于定位 userdata 分区位置）", QString(), "XML 文件 (*.xml)");
    if (rawprogramPath.isEmpty()) {
        zhi::Logger::Instance().Warning("未选择 rawprogram，无法定位 userdata 分区，已取消");
        return;
    }

    QString loaderPath = edlLoaderPath_->text();

    std::thread([this, devicePath, loaderPath, rawprogramPath]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        SaharaProtocol sahara(device);
        if (!sahara.PerformHandshake(loaderPath.toStdWString(), &err)) {
            zhi::Logger::Instance().Error("Sahara 握手失败: " + err);
            return;
        }

        FirehoseProtocol firehose(device);
        if (!firehose.Configure(1048576, &err)) {
            zhi::Logger::Instance().Error("Firehose Configure 失败: " + err);
            return;
        }

        StorageInfo storageInfo;
        uint64_t totalDiskSectors = 0;
        if (firehose.GetStorageInfo(0, &storageInfo, &err)) {
            totalDiskSectors = storageInfo.totalBlocks;
        }

        std::ifstream f(rawprogramPath.toStdWString(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();

        auto records = RawProgramParser::Parse(ss.str());
        auto entries = RawProgramParser::Resolve(records, totalDiskSectors, nullptr);

        // 只清 userdata 这一个分区，不碰其他分区，也不用 EraseLun（那个是整LUN擦除，
        // 会连GPT/系统分区一起清掉，风险完全不是一个量级）
        const ProgramEntry* target = nullptr;
        for (const auto& entry : entries) {
            if (entry.label == "userdata") { target = &entry; break; }
        }

        if (!target) {
            zhi::Logger::Instance().Error("rawprogram 里没有找到名为 userdata 的分区，"
                "本机型可能用了不同命名，已中止（避免猜错分区导致误清数据）");
            return;
        }

        uint64_t totalBytes = target->numSectors * target->sectorSize;

        zhi::Logger::Instance().Warning("开始清空 userdata 分区，共 " +
            std::to_string(totalBytes) + " 字节，请勿断开设备");

        // 注意：userdata 分区可能有几十GB大，不能像 Program() 平时写镜像文件那样
        // 一次性把"整分区大小"的数据都塞进内存（那样会直接把内存吃爆）。
        // 这里改成按 256MB 一段的窗口循环调用 Program()，每段单独指定
        // start_sector/numSectors，复用同一块 256MB 的零缓冲区，内存占用可控。
        const uint64_t windowBytes = 256ull * 1024 * 1024;
        uint64_t windowSectors = windowBytes / target->sectorSize;
        std::vector<uint8_t> zeroChunk(static_cast<size_t>(windowSectors * target->sectorSize), 0);

        uint64_t sectorsDone = 0;
        bool ok = true;
        while (sectorsDone < target->numSectors) {
            uint64_t thisWindowSectors = std::min<uint64_t>(windowSectors, target->numSectors - sectorsDone);
            uint64_t thisWindowBytes = thisWindowSectors * target->sectorSize;

            ProgramEntry chunkEntry = *target;
            chunkEntry.startSector = target->startSector + sectorsDone;
            chunkEntry.numSectors = thisWindowSectors;

            if (!firehose.Program(chunkEntry, zeroChunk.data(), thisWindowBytes, nullptr, &err)) {
                zhi::Logger::Instance().Error("清空 userdata 失败（进度 " +
                    std::to_string(sectorsDone) + "/" + std::to_string(target->numSectors) +
                    " 扇区）: " + err);
                ok = false;
                break;
            }
            sectorsDone += thisWindowSectors;
        }

        if (!ok) return;

        firehose.Reset(&err);
        zhi::Logger::Instance().Success("userdata 已清空，设备即将重启（恢复出厂设置完成）");
    }).detach();
}

// ---------------------------------------------------------------------------
// 镜像解析面板
// ---------------------------------------------------------------------------
void MainWindow::OnParseFile() {
    QString path = parseFilePath_->text();
    if (path.isEmpty()) return;

    std::ifstream f(path.toStdWString(), std::ios::binary);
    if (!f) {
        zhi::Logger::Instance().Error("无法打开文件");
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    parseTable_->setRowCount(0);

    // 简单启发式判断文件类型：含 "<program" 标签当rawprogram处理，
    // 否则按scatter(YAML)处理；两种都解析不出内容就提示用户手动确认格式
    if (content.find("<program") != std::string::npos) {
        parseFileTypeLabel_->setText("识别为: rawprogram (高通 EDL)");
        auto records = RawProgramParser::Parse(content);
        parseTable_->setRowCount(static_cast<int>(records.size()));
        for (int i = 0; i < static_cast<int>(records.size()); ++i) {
            const auto& r = records[i];
            parseTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(r.label)));
            parseTable_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(r.startSectorRaw)));
            parseTable_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(r.numSectorsRaw)));
            parseTable_->setItem(i, 3, new QTableWidgetItem(QString::fromStdString(r.filename)));
        }
        zhi::Logger::Instance().Info("解析完成，共 " + std::to_string(records.size()) + " 条分区记录");
    } else {
        auto partitions = ScatterParser::Parse(content);
        if (partitions.empty()) {
            parseFileTypeLabel_->setText("未能识别文件格式（既不像rawprogram也不像scatter）");
            zhi::Logger::Instance().Warning("文件格式无法识别，请确认这是 rawprogram*.xml 或 scatter.txt");
            return;
        }
        parseFileTypeLabel_->setText("识别为: scatter (MTK)");
        parseTable_->setRowCount(static_cast<int>(partitions.size()));
        for (int i = 0; i < static_cast<int>(partitions.size()); ++i) {
            const auto& p = partitions[i];
            parseTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(p.partitionName)));
            parseTable_->setItem(i, 1, new QTableWidgetItem(QString("0x%1").arg(p.physicalStartAddr, 0, 16)));
            parseTable_->setItem(i, 2, new QTableWidgetItem(QString("0x%1").arg(p.partitionSize, 0, 16)));
            parseTable_->setItem(i, 3, new QTableWidgetItem(
                p.isDownload ? QString::fromStdString(p.fileName) : "（不下载）"));
        }
        zhi::Logger::Instance().Info("解析完成，共 " + std::to_string(partitions.size()) + " 个分区");
    }
}

// ---------------------------------------------------------------------------
// 分区备份还原面板
// ---------------------------------------------------------------------------
void MainWindow::OnBackupDirChanged() {
    QString dirPath = backupDirPath_->text();
    if (dirPath.isEmpty()) return;

    QDir dir(dirPath);
    QFileInfoList files = dir.entryInfoList({"*.bin", "*.img", "*.qcn"}, QDir::Files, QDir::Time);

    backupTable_->setRowCount(files.size());
    for (int i = 0; i < files.size(); ++i) {
        const auto& fi = files[i];
        double mb = fi.size() / (1024.0 * 1024.0);
        backupTable_->setItem(i, 0, new QTableWidgetItem(fi.fileName()));
        backupTable_->setItem(i, 1, new QTableWidgetItem(QString::number(mb, 'f', 2) + " MB"));
        backupTable_->setItem(i, 2, new QTableWidgetItem(
            fi.lastModified().toString("yyyy-MM-dd hh:mm:ss")));
    }
    zhi::Logger::Instance().Info("备份目录扫描完成，共 " + std::to_string(files.size()) + " 个文件");
}

void MainWindow::OnBackupOpenFolder() {
    QString dirPath = backupDirPath_->text();
    if (dirPath.isEmpty()) {
        zhi::Logger::Instance().Warning("请先选择备份目录");
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath));
}

// ---------------------------------------------------------------------------
// QCN 功能面板
// ---------------------------------------------------------------------------
void MainWindow::OnQcnRead() {
    std::wstring devicePath = SelectedDevicePath();
    if (devicePath.empty()) {
        zhi::Logger::Instance().Warning("请先在顶部选中一个 9008 设备");
        return;
    }
    if (edlLoaderPath_->text().isEmpty()) {
        zhi::Logger::Instance().Warning("请先到「EDL 刷写」面板选好引导文件");
        return;
    }

    QString rawprogramPath = QFileDialog::getOpenFileName(this,
        "选择 rawprogram.xml（用于定位 modemst 分区位置）", QString(), "XML 文件 (*.xml)");
    if (rawprogramPath.isEmpty()) {
        zhi::Logger::Instance().Warning("未选择 rawprogram，无法定位 modemst 分区，已取消");
        return;
    }

    QString saveDir = QFileDialog::getExistingDirectory(this, "选择 QCN 保存位置");
    if (saveDir.isEmpty()) return;

    QString loaderPath = edlLoaderPath_->text();

    std::thread([this, devicePath, loaderPath, rawprogramPath, saveDir]() {
        UsbDevice device;
        std::string err;
        if (!device.Open(devicePath, &err)) {
            zhi::Logger::Instance().Error("打开设备失败: " + err);
            return;
        }

        SaharaProtocol sahara(device);
        if (!sahara.PerformHandshake(loaderPath.toStdWString(), &err)) {
            zhi::Logger::Instance().Error("Sahara 握手失败: " + err);
            return;
        }

        FirehoseProtocol firehose(device);
        if (!firehose.Configure(1048576, &err)) {
            zhi::Logger::Instance().Error("Firehose Configure 失败: " + err);
            return;
        }

        StorageInfo storageInfo;
        uint64_t totalDiskSectors = 0;
        if (firehose.GetStorageInfo(0, &storageInfo, &err)) {
            totalDiskSectors = storageInfo.totalBlocks;
        }

        std::ifstream f(rawprogramPath.toStdWString(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        auto records = RawProgramParser::Parse(ss.str());
        auto entries = RawProgramParser::Resolve(records, totalDiskSectors, nullptr);

        struct QcnRow { QString label; QString sizeStr; };
        std::vector<QcnRow> uiRows;
        int found = 0;

        for (const char* wanted : {"modemst1", "modemst2"}) {
            const ProgramEntry* target = nullptr;
            for (const auto& e : entries) {
                if (e.label == wanted) { target = &e; break; }
            }
            if (!target) {
                zhi::Logger::Instance().Warning(std::string("未找到分区: ") + wanted + "（本机型可能命名不同）");
                continue;
            }

            std::vector<uint8_t> data;
            if (!firehose.Read(*target, &data, nullptr, &err)) {
                zhi::Logger::Instance().Error(std::string("读取 ") + wanted + " 失败: " + err);
                continue;
            }

            QString outPath = saveDir + "/" + QString::fromStdString(target->label) + ".bin";
            std::ofstream out(outPath.toStdWString(), std::ios::binary);
            out.write(reinterpret_cast<char*>(data.data()), data.size());

            double mb = data.size() / (1024.0 * 1024.0);
            uiRows.push_back({QString::fromStdString(target->label), QString::number(mb, 'f', 2) + " MB"});
            zhi::Logger::Instance().Success(std::string(wanted) + " 已保存到 " + outPath.toStdString());
            found++;
        }

        QMetaObject::invokeMethod(this, [this, uiRows]() {
            qcnTable_->setRowCount(static_cast<int>(uiRows.size()));
            for (int i = 0; i < static_cast<int>(uiRows.size()); ++i) {
                qcnTable_->setItem(i, 0, new QTableWidgetItem(uiRows[i].label));
                qcnTable_->setItem(i, 1, new QTableWidgetItem(uiRows[i].sizeStr));
                qcnTable_->setItem(i, 2, new QTableWidgetItem("已读取并保存为.bin"));
            }
        }, Qt::QueuedConnection);

        if (found == 0) {
            zhi::Logger::Instance().Error("modemst1/modemst2 都没找到，QCN读取失败，"
                "本机型的基带配置分区可能用了不同命名");
        }
    }).detach();
}

} // namespace zhi::ui
