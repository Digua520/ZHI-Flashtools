#include "AdbProtocol.h"
#include "AdbAuth.h"
#include "../../core/Logger.h"
#include <cstring>
#include <fstream>
#include <ctime>
#include <algorithm>

namespace zhi::protocol::adb {

using zhi::Logger;

uint32_t AdbProtocol::Crc32(const uint8_t* data, size_t len) {
    // ADB 协议里的 data_crc32 其实是简单字节累加和（不是标准 CRC32，
    // 这是 AOSP 协议历史遗留的命名误导，多数实现也确实按累加和处理）
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += data[i];
    return sum;
}

bool AdbProtocol::SendMessage(uint32_t command, uint32_t arg0, uint32_t arg1,
                               const uint8_t* data, size_t len, std::string* errorOut) {
    AdbMessageHeader header{};
    header.command = command;
    header.arg0 = arg0;
    header.arg1 = arg1;
    header.dataLength = static_cast<uint32_t>(len);
    header.dataCrc32 = data ? Crc32(data, len) : 0;
    header.magic = command ^ 0xFFFFFFFFu;

    if (!device_.BulkWrite(reinterpret_cast<const uint8_t*>(&header), sizeof(header))) {
        if (errorOut) *errorOut = "发送 ADB 消息头失败";
        return false;
    }
    if (len > 0) {
        if (!device_.BulkWrite(data, len)) {
            if (errorOut) *errorOut = "发送 ADB payload 失败";
            return false;
        }
    }
    return true;
}

bool AdbProtocol::ReadMessage(AdbMessage* out, std::string* errorOut, DWORD timeoutMs) {
    AdbMessageHeader header{};
    size_t got = 0;

    if (!device_.BulkRead(reinterpret_cast<uint8_t*>(&header), sizeof(header), &got, timeoutMs) ||
        got != sizeof(header)) {
        if (errorOut) *errorOut = "读取 ADB 消息头失败/不完整";
        return false;
    }

    out->header = header;
    out->payload.clear();

    if (header.dataLength > 0) {
        if (header.dataLength > MAX_PAYLOAD) {
            if (errorOut) *errorOut = "ADB payload 超过最大限制";
            return false;
        }
        out->payload.resize(header.dataLength);
        size_t payloadGot = 0;
        if (!device_.BulkRead(out->payload.data(), out->payload.size(), &payloadGot, timeoutMs) ||
            payloadGot != header.dataLength) {
            if (errorOut) *errorOut = "读取 ADB payload 失败/不完整";
            return false;
        }
    }
    return true;
}

bool AdbProtocol::Connect(const std::wstring& keyPath, const std::string& userAtHost,
                          std::string* deviceBanner, std::string* errorOut) {
    std::string identity = "host::features=cmd,shell_v2,stat_v2";
    std::vector<uint8_t> payload(identity.begin(), identity.end());
    payload.push_back(0);

    if (!SendMessage(A_CNXN, A_VERSION, MAX_PAYLOAD, payload.data(), payload.size(), errorOut)) {
        return false;
    }

    RSA* key = nullptr;
    bool triedPublicKey = false;

    while (true) {
        AdbMessage resp;
        DWORD waitTimeout = triedPublicKey ? 60000 : 8000; // 公钥发送后等用户点手机，给60秒
        if (!ReadMessage(&resp, errorOut, waitTimeout)) {
            if (key) AdbAuth::FreeKey(key);
            return false;
        }

        if (resp.header.command == A_CNXN) {
            std::string banner(resp.payload.begin(), resp.payload.end());
            if (deviceBanner) *deviceBanner = banner;
            Logger::Instance().Success("ADB 连接建立: " + banner);
            if (key) AdbAuth::FreeKey(key);
            return true;
        }

        if (resp.header.command != A_AUTH || resp.header.arg0 != ADB_AUTH_TOKEN) {
            if (errorOut) *errorOut = "握手阶段收到意外消息";
            if (key) AdbAuth::FreeKey(key);
            return false;
        }

        // 收到 20 字节挑战 token
        if (resp.payload.size() != 20) {
            if (errorOut) *errorOut = "AUTH TOKEN 长度异常: " + std::to_string(resp.payload.size());
            if (key) AdbAuth::FreeKey(key);
            return false;
        }

        if (!key) {
            key = AdbAuth::LoadOrCreateKey(keyPath, errorOut);
            if (!key) {
                if (key) AdbAuth::FreeKey(key);
                return false;
            }
        }

        if (!triedPublicKey) {
            // 第一次收到 token：先尝试用已有私钥签名（如果这台手机之前已经
            // 记住过这个公钥，设备会直接认，不用再弹授权框）
            std::vector<uint8_t> signature;
            if (!AdbAuth::SignToken(key, resp.payload.data(), &signature, errorOut)) {
                AdbAuth::FreeKey(key);
                return false;
            }
            if (!SendMessage(A_AUTH, ADB_AUTH_SIGNATURE, 0, signature.data(), signature.size(), errorOut)) {
                AdbAuth::FreeKey(key);
                return false;
            }
            triedPublicKey = true; // 下一轮如果设备还发 TOKEN，说明签名没被接受，改发公钥
            continue;
        }

        // 签名没被接受（设备不认识这个公钥），改发公钥，此时手机屏幕会弹授权确认框
        Logger::Instance().Warning("设备未识别该密钥，正在发送公钥，请在手机屏幕上确认"
            "\"允许 USB 调试\"授权");

        std::string pubkeyBlob = AdbAuth::EncodePublicKey(key, userAtHost, errorOut);
        if (pubkeyBlob.empty()) {
            AdbAuth::FreeKey(key);
            return false;
        }
        std::vector<uint8_t> pubkeyPayload(pubkeyBlob.begin(), pubkeyBlob.end());
        pubkeyPayload.push_back(0);

        if (!SendMessage(A_AUTH, ADB_AUTH_RSAPUBLICKEY, 0,
                         pubkeyPayload.data(), pubkeyPayload.size(), errorOut)) {
            AdbAuth::FreeKey(key);
            return false;
        }

        // 接下来等用户在手机上点确认——这里的读超时要给长一点，
        // 不然用户还没来得及看手机就超时失败了
    }
}

bool AdbProtocol::OpenStream(const std::string& service, uint32_t* localIdOut,
                              uint32_t* remoteIdOut, std::string* errorOut) {
    uint32_t localId = nextLocalId_++;

    std::vector<uint8_t> payload(service.begin(), service.end());
    payload.push_back(0);

    if (!SendMessage(A_OPEN, localId, 0, payload.data(), payload.size(), errorOut)) return false;

    AdbMessage resp;
    if (!ReadMessage(&resp, errorOut)) return false;

    if (resp.header.command != A_OKAY) {
        if (errorOut) *errorOut = "打开流失败: " + service;
        return false;
    }

    if (localIdOut) *localIdOut = localId;
    if (remoteIdOut) *remoteIdOut = resp.header.arg0; // 设备侧分配的远端流ID
    return true;
}

bool AdbProtocol::Write(uint32_t localId, uint32_t remoteId, const uint8_t* data, size_t len,
                         std::string* errorOut) {
    if (!SendMessage(A_WRTE, localId, remoteId, data, len, errorOut)) return false;

    AdbMessage resp;
    if (!ReadMessage(&resp, errorOut)) return false;
    if (resp.header.command != A_OKAY) {
        if (errorOut) *errorOut = "写入未被设备确认";
        return false;
    }
    return true;
}

bool AdbProtocol::CloseStream(uint32_t localId, uint32_t remoteId) {
    std::string err;
    return SendMessage(A_CLSE, localId, remoteId, nullptr, 0, &err);
}

bool AdbProtocol::Shell(const std::string& command, std::string* outputOut, std::string* errorOut) {
    uint32_t localId = 0, remoteId = 0;
    if (!OpenStream("shell:" + command, &localId, &remoteId, errorOut)) return false;

    std::string output;
    AdbMessage msg;
    while (true) {
        if (!ReadMessage(&msg, errorOut)) return false;

        if (msg.header.command == A_WRTE) {
            output.append(msg.payload.begin(), msg.payload.end());
            // 收到数据要回 OKAY，否则设备会停止发送（简单滑动窗口协议）
            SendMessage(A_OKAY, localId, remoteId, nullptr, 0, errorOut);
        } else if (msg.header.command == A_CLSE) {
            break; // 设备主动关闭流，说明命令执行完毕
        }
    }

    if (outputOut) *outputOut = output;
    CloseStream(localId, remoteId);
    return true;
}

bool AdbProtocol::Push(const std::wstring& localPath, const std::string& remotePath,
                       int mode, std::string* errorOut) {
    // ADB 的文件推送不是走 shell:，是单独的 sync: 服务，协议是
    // SEND/DATA/DONE 这几个子帧（帧头4字节ASCII + 4字节小端长度），
    // 这几个子帧本身是通过已经打开的 sync: 流用 WRTE 消息承载的——
    // 不是新的 ADB 顶层命令，是在流内部再套一层"同步协议"。
    std::ifstream file(localPath, std::ios::binary | std::ios::ate);
    if (!file) {
        if (errorOut) *errorOut = "无法打开本地文件";
        return false;
    }
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);

    uint32_t localId = 0, remoteId = 0;
    if (!OpenStream("sync:", &localId, &remoteId, errorOut)) return false;

    // ---- SEND 帧：路径+权限字符串，格式 "remotePath,mode" ----
    std::string sendArg = remotePath + "," + std::to_string(mode);
    std::vector<uint8_t> sendFrame;
    sendFrame.insert(sendFrame.end(), {'S', 'E', 'N', 'D'});
    uint32_t sendArgLen = static_cast<uint32_t>(sendArg.size());
    sendFrame.push_back(static_cast<uint8_t>(sendArgLen & 0xFF));
    sendFrame.push_back(static_cast<uint8_t>((sendArgLen >> 8) & 0xFF));
    sendFrame.push_back(static_cast<uint8_t>((sendArgLen >> 16) & 0xFF));
    sendFrame.push_back(static_cast<uint8_t>((sendArgLen >> 24) & 0xFF));
    sendFrame.insert(sendFrame.end(), sendArg.begin(), sendArg.end());

    if (!Write(localId, remoteId, sendFrame.data(), sendFrame.size(), errorOut)) {
        CloseStream(localId, remoteId);
        return false;
    }

    // ---- DATA 帧：分块发送文件内容，每块 <= 64KB ----
    const size_t chunkSize = 64 * 1024;
    std::vector<uint8_t> buffer(chunkSize);
    size_t sent = 0;

    while (sent < size) {
        size_t thisChunk = std::min(chunkSize, size - sent);
        file.read(reinterpret_cast<char*>(buffer.data()), thisChunk);

        std::vector<uint8_t> dataFrame;
        dataFrame.insert(dataFrame.end(), {'D', 'A', 'T', 'A'});
        uint32_t len = static_cast<uint32_t>(thisChunk);
        dataFrame.push_back(static_cast<uint8_t>(len & 0xFF));
        dataFrame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        dataFrame.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        dataFrame.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        dataFrame.insert(dataFrame.end(), buffer.begin(), buffer.begin() + thisChunk);

        if (!Write(localId, remoteId, dataFrame.data(), dataFrame.size(), errorOut)) {
            CloseStream(localId, remoteId);
            return false;
        }
        sent += thisChunk;
    }

    // ---- DONE 帧：文件修改时间（这里直接用当前时间）----
    std::vector<uint8_t> doneFrame;
    doneFrame.insert(doneFrame.end(), {'D', 'O', 'N', 'E'});
    uint32_t mtime = static_cast<uint32_t>(std::time(nullptr));
    doneFrame.push_back(static_cast<uint8_t>(mtime & 0xFF));
    doneFrame.push_back(static_cast<uint8_t>((mtime >> 8) & 0xFF));
    doneFrame.push_back(static_cast<uint8_t>((mtime >> 16) & 0xFF));
    doneFrame.push_back(static_cast<uint8_t>((mtime >> 24) & 0xFF));

    if (!Write(localId, remoteId, doneFrame.data(), doneFrame.size(), errorOut)) {
        CloseStream(localId, remoteId);
        return false;
    }

    // 设备会再回一条 WRTE，内容是 "OKAY" 或 "FAIL"+错误信息，确认传输结果
    AdbMessage resp;
    if (!ReadMessage(&resp, errorOut)) {
        CloseStream(localId, remoteId);
        return false;
    }
    // 收到设备的确认帧后，标准流程还要回一次 OKAY 表示"我收到你的确认了"
    SendMessage(A_OKAY, localId, remoteId, nullptr, 0, errorOut);
    CloseStream(localId, remoteId);

    if (resp.payload.size() >= 4 && memcmp(resp.payload.data(), "FAIL", 4) == 0) {
        std::string reason(resp.payload.begin() + 4, resp.payload.end());
        if (errorOut) *errorOut = "设备拒绝写入: " + reason;
        return false;
    }

    Logger::Instance().Success("文件推送完成: " + remotePath + " (" + std::to_string(size) + " 字节)");
    return true;
}

} // namespace zhi::protocol::adb
