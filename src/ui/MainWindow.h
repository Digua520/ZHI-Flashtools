#pragma once
// ============================================================================
// MainWindow —— 主窗口，对应之前 HTML 原型的整体布局：
//   左侧功能导航 + 中间内容区（QStackedWidget切页） + 右侧日志面板
//
// 这是第一版 UI 骨架，目标是把"导航能切页""日志面板能实时显示 Logger 输出"
// "能真正点按钮触发一次 ADB/Fastboot/EDL 操作"这几件事跑通，不是像素级
// 还原 HTML 原型的视觉效果（配色、圆角、玻璃效果这些留到样式表阶段再抠）。
// ============================================================================

#include <QMainWindow>
#include <QListWidget>
#include <QStackedWidget>
#include <QPlainTextEdit>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QProgressBar>
#include <QTimer>
#include <QStringList>
#include <memory>
#include "../usb/UsbDevice.h"

QT_BEGIN_NAMESPACE
class QMenuBar;
class QStatusBar;
QT_END_NAMESPACE

namespace zhi::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void OnNavItemChanged(int row);
    void OnRefreshDevicesClicked();
    void OnAdbConnectClicked();
    void OnFastbootGetVarClicked();
    void OnAdbAutoDetect();       // ADB面板专属的"仅ADB"扫描按钮
    void OnFastbootAutoDetect();  // Fastboot面板专属的"仅Fastboot"扫描按钮

    // 日志从后台线程（USB/协议操作都跑在工作线程，避免卡UI）通过信号发过来，
    // 这里统一在主线程里追加到日志面板
    void AppendLogLine(int level, const QString& text);

private:
    void BuildMenuBar();
    void BuildNavPanel();
    void BuildContentPanels();
    void BuildLogPanel();
    void WireLoggerSink();

    QWidget* BuildAdbPanel();
    QWidget* BuildFastbootPanel();
    QWidget* BuildEdlPanel();
    QWidget* BuildMtkPanel();
    QWidget* BuildDriverPanel();
    QWidget* BuildParsePanel();
    QWidget* BuildBackupPanel();
    QWidget* BuildQcnPanel();

    QListWidget*     nav_ = nullptr;
    QStackedWidget*   stack_ = nullptr;
    QPlainTextEdit*   logView_ = nullptr;
    QLabel*           connStatusLabel_ = nullptr;

    // 设备选择下拉框——ADB/Fastboot/EDL 面板共用一份扫描结果，
    // 下拉框里存的 userData 就是 UsbDeviceInfo::devicePath，
    // Open() 直接拿这个用，不用再解析字符串
    QComboBox* deviceCombo_ = nullptr;
    std::vector<zhi::usb::UsbDeviceInfo> lastScanResults_;

    void RefreshDeviceCombo();               // 重新扫描并填充下拉框
    std::wstring SelectedDevicePath() const; // 取当前选中项对应的设备路径，未选返回空

    // 自动检测：不需要用户手动点"刷新设备"，定时器每2秒静默扫描一次，
    // 只有设备列表真的变化了才更新下拉框/打日志，避免刷屏
    QTimer* autoScanTimer_ = nullptr;
    void AutoScanDevices();
    QStringList lastScanNames_; // 用来判断这次扫描结果跟上次是否一样

    // ADB 面板上的几个控件引用，按钮回调里要用
    QLabel* adbDeviceInfoLabel_ = nullptr;
    QTableWidget* adbPartitionTable_ = nullptr;
    QLineEdit* adbPushLocalPath_ = nullptr;
    QLineEdit* adbPushRemotePath_ = nullptr;
    void OnAdbReadPartitions();
    void OnAdbPushFile();

    // EDL 面板（三方案：小米=仅引导 / 欧加=引导+Digest+Signature / 安卓通用=仅引导）
    QString edlBrand_ = "generic"; // "xiaomi" / "oujia" / "generic"
    QLineEdit* edlLoaderPath_ = nullptr;
    QLineEdit* edlDigestPath_ = nullptr;    // 欧加专属
    QLineEdit* edlSignaturePath_ = nullptr; // 欧加专属
    QWidget* edlOujiaBox_ = nullptr;        // 包裹Digest+Signature两行，按品牌显隐
    QLabel* edlBrandNote_ = nullptr;
    QTableWidget* edlTable_ = nullptr;
    QProgressBar* edlProgress_ = nullptr;
    QPushButton* edlStartBtn_ = nullptr;
    void OnEdlBrandChanged(const QString& brand);
    void OnEdlStartFlash();
    void RefreshEdlTableFromDevice(); // 握手后从设备读GPT分区表填进表格

    // MTK 面板
    QLineEdit* mtkScatterPath_ = nullptr;
    QLineEdit* mtkDaPath_ = nullptr;
    QTableWidget* mtkTable_ = nullptr;
    QProgressBar* mtkProgress_ = nullptr;
    void OnMtkStartFlash();
    void RefreshMtkTable();

    // 驱动管理面板
    QTableWidget* driverTable_ = nullptr;
    void OnDetectDrivers();

    // Fastboot 面板
    QTableWidget* fastbootInfoTable_ = nullptr;
    void OnFastbootGetAllVars();

    // 顶部菜单：高级重启 / 高通功能 里那几个命令
    void OnMenuRebootTo(const std::string& target); // target: "bootloader"/"edl"/"recovery"（走ADB shell reboot）
    void OnEdlBackupModem();      // 高通功能 -> EDL 备份基带分区
    void OnEdlFactoryReset();     // 高通功能 -> EDL 通用恢复出厂（仅清userdata，不动其他分区）

    // 镜像解析面板：本地解析 rawprogram.xml 或 scatter.txt，不需要连设备
    QLineEdit* parseFilePath_ = nullptr;
    QTableWidget* parseTable_ = nullptr;
    QLabel* parseFileTypeLabel_ = nullptr;
    void OnParseFile();

    // 分区备份还原面板：管理之前各面板（EDL备份基带/读取分区等）产生的备份文件
    QLineEdit* backupDirPath_ = nullptr;
    QTableWidget* backupTable_ = nullptr;
    void OnBackupDirChanged();
    void OnBackupOpenFolder();

    // QCN 功能面板
    QTableWidget* qcnTable_ = nullptr;
    void OnQcnRead();
};

} // namespace zhi::ui
