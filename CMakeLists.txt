cmake_minimum_required(VERSION 3.20)
project(ZHI-Flashtools CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "本项目依赖 WinUSB / SetupAPI，只能在 Windows 上编译")
endif()

# ---- 协议与通信层：先编成静态库，UI 层（Qt/WPF）单独链接 ----
find_package(OpenSSL REQUIRED)

add_library(zhi_core STATIC
    src/core/Logger.cpp
    src/usb/UsbDevice.cpp
    src/protocol/fastboot/FastbootProtocol.cpp
    src/protocol/adb/AdbProtocol.cpp
    src/protocol/adb/AdbAuth.cpp
    src/protocol/edl/SaharaProtocol.cpp
    src/protocol/edl/FirehoseProtocol.cpp
    src/protocol/mtk/BromProtocol.cpp
    src/protocol/mtk/DaProtocol.cpp
    src/firmware/XmlLite.cpp
    src/firmware/RawProgramParser.cpp
    src/firmware/PatchParser.cpp
    src/firmware/ScatterParser.cpp
)

target_include_directories(zhi_core PUBLIC src)
target_link_libraries(zhi_core PUBLIC winusb setupapi OpenSSL::Crypto)

# ---- 可选：固件解析层的独立测试（不依赖Windows/USB，任何平台都能跑）----
# 这几个测试在 Linux 上用 g++ 实测跑通过，用来验证 rawprogram/patch/scatter
# 解析逻辑本身没问题（跟USB通信、Windows API完全无关）。
option(ZHI_BUILD_TESTS "编译firmware层的独立测试" OFF)
if(ZHI_BUILD_TESTS)
    add_executable(test_firmware_parsers
        tests/test_firmware_parsers.cpp
        src/core/Logger.cpp
        src/firmware/XmlLite.cpp
        src/firmware/RawProgramParser.cpp
        src/firmware/PatchParser.cpp
    )
    target_include_directories(test_firmware_parsers PRIVATE src)

    add_executable(test_scatter_parser
        tests/test_scatter_parser.cpp
        src/core/Logger.cpp
        src/firmware/ScatterParser.cpp
    )
    target_include_directories(test_scatter_parser PRIVATE src)
endif()

# ---- UI 层：Qt Widgets 可执行文件 ----
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(ZHI-Flashtools WIN32
    src/ui/main.cpp
    src/ui/MainWindow.cpp
    src/ui/LoginDialog.cpp
    resources/app.rc              # 把图标编译进exe本体（任务栏/文件属性显示用）
    resources/resources.qrc       # 把图标登记为Qt资源(:/resources/...)，代码里setWindowIcon要用
)

target_link_libraries(ZHI-Flashtools PRIVATE zhi_core Qt6::Widgets dwmapi)
