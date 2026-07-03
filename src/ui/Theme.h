#pragma once
// ============================================================================
// Theme —— 全局样式表，把 HTML 原型里定下的配色/圆角/字体规范搬到 Qt 里
//
// 老实话：QSS（Qt Style Sheet）能做圆角、配色、内边距这些，但做不出真正的
// 毛玻璃模糊效果（backdrop-filter那种）——那个需要接 Windows DWM 的
// Mica/Acrylic 合成 API（DwmSetWindowAttribute + DWMWA_SYSTEMBACKDROP_TYPE，
// Win11 22H2+），是另一块工作量，这一版先做"浅色+圆角+强调色"的扁平化风格，
// 视觉上和 HTML 原型的骨架、配色是对齐的，玻璃模糊效果留到后面单独做。
// ============================================================================

#include <QString>

namespace zhi::ui {

// glassMode=false：普通不透明浅色主题（默认，任何 Windows 版本都能看，稳）
// glassMode=true ：配合 DwmEffects::EnableMicaBackdrop() 使用，把原本
//   纯色的背景换成半透明 rgba()，让 Mica 材质能透出来。
//   只在确认 EnableMicaBackdrop() 调用成功（Win11 22H2+）时才建议开启，
//   否则退化成"半透明但背后没有Mica材质"，会变得不好看（一片灰蒙蒙）。
inline QString BuildStyleSheet(bool glassMode = false) {
    const QString bgMain      = glassMode ? "rgba(244,245,248,60)"  : "#f4f5f8";
    const QString bgPanel     = glassMode ? "rgba(255,255,255,90)"  : "#ffffff";
    const QString bgNav       = glassMode ? "rgba(250,251,252,70)"  : "#fafbfc";
    const QString bgMenuBar   = glassMode ? "rgba(255,255,255,110)" : "#ffffff";
    const QString bgLogPanel  = glassMode ? "rgba(250,251,252,70)"  : "#fafbfc";
    const QString bgLogView   = glassMode ? "rgba(255,255,255,140)" : "#ffffff";
    const QString bgStatusBar = glassMode ? "rgba(250,251,252,90)"  : "#fafbfc";
    const QString bgButton    = glassMode ? "rgba(255,255,255,120)" : "#ffffff";

    return QString(R"(
        * {
            font-family: "Microsoft YaHei", "PingFang SC", "Heiti SC", sans-serif;
        }

        QMainWindow, QWidget {
            background-color: %1;
            color: #14161b;
        }

        /* ---- 菜单栏 ---- */
        QMenuBar {
            background-color: %2;
            border-bottom: 1px solid #e3e6ec;
            padding: 4px 6px;
            font-weight: 700;
            font-size: 12px;
        }
        QMenuBar::item {
            padding: 6px 13px;
            border-radius: 999px;
            background: transparent;
        }
        QMenuBar::item:selected {
            background-color: #eaf0ff;
            color: #1a3fae;
        }
        QMenu {
            background-color: #ffffff;
            border: 1px solid #c9cedb;
            border-radius: 10px;
            padding: 6px;
        }
        QMenu::item {
            padding: 7px 16px;
            border-radius: 6px;
            font-size: 12.5px;
        }
        QMenu::item:selected {
            background-color: #eaf0ff;
            color: #1a3fae;
        }

        /* ---- 左侧导航 ---- */
        QListWidget#navList {
            background-color: %3;
            border: none;
            border-right: 1px solid #e3e6ec;
            padding: 8px 6px;
            outline: none;
        }
        QListWidget#navList::item {
            padding: 8px 12px;
            margin: 2px 2px;
            border-radius: 8px;
            color: #6b7280;
            font-size: 12.5px;
        }
        QListWidget#navList::item:hover {
            background-color: #f0f2f6;
            color: #14161b;
        }
        QListWidget#navList::item:selected {
            background-color: #eaf0ff;
            color: #1a3fae;
            font-weight: 700;
        }

        /* ---- 内容区 ---- */
        QStackedWidget {
            background-color: %4;
        }

        QGroupBox {
            border: 1px solid #e3e6ec;
            border-radius: 12px;
            margin-top: 10px;
            padding: 10px;
            font-weight: 700;
            font-size: 12.5px;
            background-color: %4;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 6px;
            color: #6b7280;
        }

        /* ---- 按钮：胶囊形，跟原型一致 ---- */
        QPushButton {
            background-color: %8;
            border: 1px solid #c9cedb;
            border-radius: 15px;
            padding: 7px 16px;
            font-size: 12px;
            font-weight: 700;
            color: #14161b;
        }
        QPushButton:hover {
            border-color: #2454d6;
            color: #2454d6;
        }
        QPushButton:pressed {
            background-color: #eaf0ff;
        }
        QPushButton#primary {
            background-color: #2454d6;
            border-color: #2454d6;
            color: #ffffff;
        }
        QPushButton#primary:hover {
            background-color: #1a3fae;
        }
        QPushButton#brandBtn:checked {
            background-color: #2454d6;
            border-color: #2454d6;
            color: #ffffff;
            font-weight: 800;
        }
        QPushButton#danger {
            color: #d3271f;
            border-color: #d3271f;
        }
        QPushButton#danger:hover {
            background-color: #d3271f;
            color: #ffffff;
        }

        /* ---- 表格 ---- */
        QTableWidget {
            background-color: %4;
            border: 1px solid #e3e6ec;
            border-radius: 10px;
            gridline-color: #e3e6ec;
            font-size: 12px;
        }
        QHeaderView::section {
            background-color: %3;
            border: none;
            border-bottom: 1px solid #e3e6ec;
            padding: 6px 10px;
            font-weight: 700;
            font-size: 11px;
            color: #6b7280;
        }

        /* ---- 日志面板 ---- */
        QWidget#logPanel {
            background-color: %5;
            border-left: 1px solid #e3e6ec;
        }
        QPlainTextEdit#logView {
            background-color: %6;
            border: 1px solid #e3e6ec;
            border-radius: 10px;
            padding: 8px;
            font-family: "JetBrains Mono", "Consolas", monospace;
            font-size: 11px;
            color: #14161b;
        }

        /* ---- 状态栏 ---- */
        QStatusBar {
            background-color: %7;
            border-top: 1px solid #e3e6ec;
            font-size: 11px;
            color: #6b7280;
        }

        /* ---- 分隔条：细一点，不要太突兀 ---- */
        QSplitter::handle {
            background-color: #e3e6ec;
            width: 1px;
        }
    )").arg(bgMain, bgMenuBar, bgNav, bgPanel, bgLogPanel, bgLogView, bgStatusBar, bgButton);
}

} // namespace zhi::ui
