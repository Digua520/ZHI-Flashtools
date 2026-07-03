#include "FastbootProtocol.h"
#include "../../core/Logger.h"
#include <cstring>
#include <cstdio>
#include <fstream>

namespace zhi::protocol::fastboot {

using zhi::Logger;

ResponseType FastbootProtocol::ParsePrefix(const char* buf4) {
    if (memcmp(buf4, "OKAY", 4) == 0) return ResponseType::Okay;
    if (memcmp(buf4, "FAIL", 4) == 0) return ResponseType::Fail;
    if (memcmp(buf4, "DATA", 4) == 0) return ResponseType::Data;
    if (memcmp(buf4, "INFO", 4) == 0) return ResponseType::Info;
    return ResponseType::Unknown;
}

bool FastbootProtocol::ReadResponse(FastbootResponse* out, std::string* errorOut) {
    uint8_t buf[256] = {0};
    size_t got = 0;

    if (!device_.BulkRead(buf, sizeof(buf) - 1, &got, 8000)) {
        if (errorOut) *errorOut = "读取 Fastboot 响应超时/失败";
        return false;
    }
    if (got < 4) {
        if (errorOut) *errorOut = "响应长度异常（不足4字节）";
        return false;
    }

    out->type = ParsePrefix(reinterpret_cast<const char*>(buf));
    std::string payload(reinterpret_cast<const char*>(buf + 4), got - 4);

    if (out->type == ResponseType::Data) {
        // payload 是 8 位十六进制长度，例如 "05000000" 表示 0x05000000 字节
        out->dataLen = static_cast<uint32_t>(strtoul(payload.substr(0, 8).c_str(), nullptr, 16));
    } else {
        out->message = payload;
    }
    return true;
}

bool FastbootProtocol::SendCommand(const std::string& command, FastbootResponse* outResponse,
                                    std::string* errorOut) {
    if (command.size() > 64) {
        if (errorOut) *errorOut = "命令超过 64 字节限制: " + command;
        return false;
    }

    if (!device_.BulkWrite(reinterpret_cast<const uint8_t*>(command.data()), command.size())) {
        if (errorOut) *errorOut = "发送命令失败: " + command;
        return false;
    }

    // INFO 类响应表示"过程信息，继续等下一条"，一直读到 OKAY/FAIL/DATA 为止
    FastbootResponse resp;
    while (true) {
        if (!ReadResponse(&resp, errorOut)) return false;

        if (resp.type == ResponseType::Info) {
            Logger::Instance().Info("[fastboot] " + resp.message);
            continue;
        }
        break;
    }

    if (outResponse) *outResponse = resp;

    if (resp.type == ResponseType::Fail) {
        Logger::Instance().Error("[fastboot] 命令失败: " + command + " -> " + resp.message);
        if (errorOut) *errorOut = resp.message;
        return false;
    }
    return true;
}

bool FastbootProtocol::GetVar(const std::string& varName, std::string* valueOut) {
    FastbootResponse resp;
    std::string err;
    if (!SendCommand("getvar:" + varName, &resp, &err)) return false;
    if (valueOut) *valueOut = resp.message;
    return true;
}

bool FastbootProtocol::GetAllVars(std::vector<std::pair<std::string, std::string>>* varsOut) {
    if (!device_.BulkWrite(reinterpret_cast<const uint8_t*>("getvar:all"), 10)) {
        Logger::Instance().Error("发送 getvar:all 失败");
        return false;
    }

    varsOut->clear();
    FastbootResponse resp;
    std::string err;

    while (true) {
        if (!ReadResponse(&resp, &err)) {
            Logger::Instance().Error("读取 getvar:all 响应失败: " + err);
            return false;
        }

        if (resp.type == ResponseType::Info) {
            // INFO 消息格式一般是 "key:value"，用第一个冒号切分
            auto pos = resp.message.find(':');
            if (pos != std::string::npos) {
                varsOut->emplace_back(resp.message.substr(0, pos), resp.message.substr(pos + 1));
            } else if (!resp.message.empty()) {
                varsOut->emplace_back(resp.message, "");
            }
            continue;
        }
        break; // OKAY/FAIL，getvar:all 结束
    }

    if (resp.type == ResponseType::Fail) {
        Logger::Instance().Error("getvar:all 失败: " + resp.message);
        return false;
    }

    Logger::Instance().Success("getvar:all 完成，共 " + std::to_string(varsOut->size()) + " 项");
    return true;
}

bool FastbootProtocol::Reboot() {
    FastbootResponse resp;
    return SendCommand("reboot", &resp);
}

bool FastbootProtocol::RebootBootloader() {
    FastbootResponse resp;
    return SendCommand("reboot-bootloader", &resp);
}

bool FastbootProtocol::RebootFastbootD() {
    FastbootResponse resp;
    return SendCommand("reboot-fastboot", &resp);
}

bool FastbootProtocol::RebootEdl() {
    // 注意：标准 AOSP fastboot 协议本身没有定义 "reboot-edl"，
    // 高通平台设备通常通过 "oem edl" 或厂商私有 OEM 命令实现，具体命令字符串
    // 因厂商/机型而异，需要针对目标机型做适配表，这里先给出最常见的一种。
    FastbootResponse resp;
    return SendCommand("oem edl", &resp);
}

bool FastbootProtocol::Erase(const std::string& partition) {
    FastbootResponse resp;
    return SendCommand("erase:" + partition, &resp);
}

bool FastbootProtocol::SetActiveSlot(char slot) {
    FastbootResponse resp;
    std::string cmd = "set_active:";
    cmd += slot;
    return SendCommand(cmd, &resp);
}

bool FastbootProtocol::Download(const uint8_t* data, size_t len, ProgressCallback onProgress,
                                 std::string* errorOut) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "download:%08zx", len);

    FastbootResponse resp;
    if (!SendCommand(cmd, &resp, errorOut)) return false;
    if (resp.type != ResponseType::Data) {
        if (errorOut) *errorOut = "设备未返回 DATA，无法开始下载";
        return false;
    }

    // 分块写入，每块建议 <= 16KB，避免单次 USB 传输超时或驱动缓冲区限制
    const size_t chunkSize = 16 * 1024;
    size_t sent = 0;
    while (sent < len) {
        size_t thisChunk = std::min(chunkSize, len - sent);
        if (!device_.BulkWrite(data + sent, thisChunk)) {
            if (errorOut) *errorOut = "数据传输中断，已发送 " + std::to_string(sent) + " / " + std::to_string(len);
            return false;
        }
        sent += thisChunk;
        if (onProgress) onProgress(sent, len);
    }

    // 传输完成后设备会再发一条 OKAY
    if (!ReadResponse(&resp, errorOut)) return false;
    if (resp.type != ResponseType::Okay) {
        if (errorOut) *errorOut = "下载未被设备确认: " + resp.message;
        return false;
    }
    return true;
}

bool FastbootProtocol::Flash(const std::string& partition, std::string* errorOut) {
    FastbootResponse resp;
    return SendCommand("flash:" + partition, &resp, errorOut);
}

bool FastbootProtocol::FlashFile(const std::string& partition, const std::wstring& imagePath,
                                  ProgressCallback onProgress, std::string* errorOut) {
    std::ifstream file(imagePath, std::ios::binary | std::ios::ate);
    if (!file) {
        if (errorOut) *errorOut = "无法打开镜像文件";
        return false;
    }

    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);

    Logger::Instance().Info("开始下载镜像到设备缓冲区，大小 " + std::to_string(size) + " 字节");
    if (!Download(buffer.data(), buffer.size(), onProgress, errorOut)) return false;

    Logger::Instance().Info("下载完成，写入分区: " + partition);
    if (!Flash(partition, errorOut)) return false;

    Logger::Instance().Success("分区 " + partition + " 写入成功");
    return true;
}

} // namespace zhi::protocol::fastboot
