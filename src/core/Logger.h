#pragma once
#include <string>
#include <functional>
#include <mutex>

namespace zhi {

enum class LogLevel {
    Info,
    Success,
    Warning,
    Error
};

// 日志回调：UI 层订阅这个回调，把日志实时显示到右侧日志面板
using LogSink = std::function<void(LogLevel level, const std::string& message)>;

// 全局日志器，单例。协议层/USB层只管调用 Logger::Instance().Log(...)，
// 不关心日志最终显示在哪，UI 初始化时注册一个 sink 即可。
class Logger {
public:
    static Logger& Instance();

    void SetSink(LogSink sink);

    void Info(const std::string& msg);
    void Success(const std::string& msg);
    void Warning(const std::string& msg);
    void Error(const std::string& msg);

    // 十六进制转储，方便调试协议报文（Sahara/Firehose 联调时非常需要）
    void HexDump(const std::string& tag, const uint8_t* data, size_t len);

private:
    void Emit(LogLevel level, const std::string& msg);

    std::mutex mutex_;
    LogSink sink_;
};

} // namespace zhi
