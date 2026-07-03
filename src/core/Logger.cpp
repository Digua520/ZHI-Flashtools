#include "Logger.h"
#include <sstream>
#include <iomanip>

namespace zhi {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::Info(const std::string& msg)    { Emit(LogLevel::Info, msg); }
void Logger::Success(const std::string& msg) { Emit(LogLevel::Success, msg); }
void Logger::Warning(const std::string& msg) { Emit(LogLevel::Warning, msg); }
void Logger::Error(const std::string& msg)   { Emit(LogLevel::Error, msg); }

void Logger::Emit(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sink_) sink_(level, msg);
}

void Logger::HexDump(const std::string& tag, const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << "[" << tag << "] " << len << " bytes:\n";
    for (size_t i = 0; i < len; i += 16) {
        oss << std::setw(4) << std::setfill('0') << std::hex << i << "  ";
        for (size_t j = i; j < i + 16; ++j) {
            if (j < len) oss << std::setw(2) << std::setfill('0') << std::hex
                              << static_cast<int>(data[j]) << " ";
            else oss << "   ";
        }
        oss << " ";
        for (size_t j = i; j < i + 16 && j < len; ++j) {
            char c = static_cast<char>(data[j]);
            oss << (isprint((unsigned char)c) ? c : '.');
        }
        oss << "\n";
    }
    Info(oss.str());
}

} // namespace zhi
