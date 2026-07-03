#pragma once
// ============================================================================
// DwmEffects —— 给主窗口接 Windows 11 的 Mica 毛玻璃背景
//
// 老实话先放前面：Qt + DWM Mica 这个组合社区里公认比较"闹脾气"——
// DWM 的 Mica/Acrylic 是"系统在窗口背后画一层材质，应用只负责别把它挡住"，
// 但 Qt Widgets 默认会把整个客户区涂成不透明色（尤其是我们在 Theme.h 里
// 写的那些 background-color: #ffffff 之类的规则），这些不透明背景会直接把
// Mica 效果完全盖住，肉眼看不出任何区别。
//
// 要让 Mica 真正透出来，需要两件事同时做对：
//   1. 调用本文件里的 EnableMicaBackdrop()，告诉 DWM 这个窗口要用 Mica
//   2. 窗口自身的背景必须是透明/半透明的（QWidget::setAttribute(
//      Qt::WA_TranslucentBackground) + Theme.h 里把纯色背景换成半透明
//      rgba()，这一步我在 Theme.h 里加了一个可选的"玻璃模式"开关，见下方）
//
// 第2步做完之后，观感上会从"完全不透明的浅色界面"变成"能看到桌面壁纸
// 透进来的模糊材质"，具体透明度/模糊强度是系统控制的，应用程序控制不了
// 具体数值，只能选 Mica / Acrylic / Tabbed 三种材质之一。
//
// 系统要求：Windows 11 22H2 (build 22621) 及以上，更早版本调用会静默失败
// （返回非S_OK但不会崩溃），建议做个版本判断或者干脆忽略返回值——
// 低版本系统下退化成普通不透明窗口是可以接受的降级行为。
// ============================================================================

#include <QWidget>
#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace zhi::ui {

// 部分较旧的 Windows SDK 头文件里没有这几个常量，手动补上，
// 数值来自微软官方文档 (dwmapi.h in Windows 11 SDK)
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

enum class BackdropType {
    None = 1,
    Mica = 2,     // 桌面壁纸材质,跟开始菜单一个质感,最推荐用在主窗口
    Acrylic = 3,  // 更强的模糊+更高透明度,常用在弹出面板/侧边栏
    MicaAlt = 4,  // Mica的变体(Tabbed),分层窗口用得多
};

// 给窗口启用 Mica/Acrylic 背景。必须在窗口已经创建了原生句柄之后调用
// （winId() 会强制创建，构造函数里 resize()/setWindowTitle() 之后调用即可，
// 不需要等 show()）。deepDark 参数控制是否同时启用深色标题栏文字配色。
inline bool EnableMicaBackdrop(QWidget* window, BackdropType type = BackdropType::Mica,
                                bool deepDark = false) {
    if (!window) return false;

    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd) return false;

    int backdropValue = static_cast<int>(type);
    HRESULT hr1 = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                         &backdropValue, sizeof(backdropValue));

    BOOL darkMode = deepDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    // 让 DWM 的非客户区绘制延伸到整个窗口（配合 Qt::WA_TranslucentBackground
    // 使用效果最好；不加这一步，部分 Windows 版本上 Mica 只会出现在标题栏）
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    return SUCCEEDED(hr1);
}

} // namespace zhi::ui
