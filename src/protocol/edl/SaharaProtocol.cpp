#include "SaharaProtocol.h"
#include "../../core/Logger.h"
#include <fstream>
#include <cstring>

namespace zhi::protocol::edl {

using zhi::Logger;

bool SaharaProtocol::WaitHello(SaharaHelloPacket* out, std::string* errorOut) {
    uint8_t buf[256] = {0};
    size_t got = 0;

    // 设备刚进 EDL 会主动、持续地发 HELLO，直到主机响应，超时给长一点
    if (!device_.BulkRead(buf, sizeof(buf), &got, 10000)) {
        if (errorOut) *errorOut = "等待 Sahara HELLO 超时（检查设备是否已进入 9008 模式）";
        return false;
    }
    if (got < sizeof(SaharaHelloPacket)) {
        if (errorOut) *errorOut = "HELLO 包长度异常";
        return false;
    }

    memcpy(out, buf, sizeof(SaharaHelloPacket));

    if (out->header.command != static_cast<uint32_t>(SaharaCommand::Hello)) {
        if (errorOut) *errorOut = "首包不是 HELLO，命令ID=" + std::to_string(out->header.command);
        return false;
    }

    Logger::Instance().Success("收到 Sahara HELLO，version=" + std::to_string(out->version) +
        " mode=" + std::to_string(out->mode));
    return true;
}

bool SaharaProtocol::SendHelloResponse(std::string* errorOut) {
    SaharaHelloResponsePacket resp{};
    resp.header.command = static_cast<uint32_t>(SaharaCommand::HelloResponse);
    resp.header.length = sizeof(resp);
    resp.version = 2;
    resp.versionCompatible = 1;
    resp.status = 0; // 0 = 成功，接受进入 image transfer 阶段
    resp.mode = 0;   // 0 = SAHARA_MODE_IMAGE_TX_PENDING

    if (!device_.BulkWrite(reinterpret_cast<uint8_t*>(&resp), sizeof(resp))) {
        if (errorOut) *errorOut = "发送 HELLO_RESPONSE 失败";
        return false;
    }
    return true;
}

bool SaharaProtocol::ServeReadRequests(const std::vector<uint8_t>& loaderData, std::string* errorOut) {
    uint8_t buf[64] = {0};

    while (true) {
        size_t got = 0;
        if (!device_.BulkRead(buf, sizeof(buf), &got, 8000)) {
            if (errorOut) *errorOut = "等待设备读取请求超时";
            return false;
        }
        if (got < sizeof(SaharaHeader)) {
            if (errorOut) *errorOut = "收到异常短包";
            return false;
        }

        SaharaHeader header;
        memcpy(&header, buf, sizeof(header));

        if (header.command == static_cast<uint32_t>(SaharaCommand::ReadData)) {
            SaharaReadDataPacket req;
            memcpy(&req, buf, sizeof(req));

            if (req.offset + req.length > loaderData.size()) {
                if (errorOut) *errorOut = "设备请求的偏移超出 loader 文件范围";
                return false;
            }

            Logger::Instance().Info("Sahara READ_DATA offset=0x" +
                std::to_string(req.offset) + " length=" + std::to_string(req.length));

            if (!device_.BulkWrite(loaderData.data() + req.offset, req.length)) {
                if (errorOut) *errorOut = "发送 loader 数据块失败";
                return false;
            }
            continue;
        }

        if (header.command == static_cast<uint32_t>(SaharaCommand::EndOfImageTransfer)) {
            SaharaEndOfImageTransferPacket endPkt;
            memcpy(&endPkt, buf, sizeof(endPkt));

            if (endPkt.status != 0) {
                if (errorOut) *errorOut = "设备报告镜像传输失败，status=" + std::to_string(endPkt.status);
                return false;
            }

            // 回确认包（复用同结构，status=0 表示主机确认成功）
            SaharaEndOfImageTransferPacket ack = endPkt;
            device_.BulkWrite(reinterpret_cast<uint8_t*>(&ack), sizeof(ack));

            Logger::Instance().Success("Sahara 镜像传输完成 (END_OF_IMAGE_TRANSFER)");
            return true;
        }

        if (errorOut) *errorOut = "收到未处理的 Sahara 命令: " + std::to_string(header.command);
        return false;
    }
}

bool SaharaProtocol::PerformHandshake(const std::wstring& loaderPath, std::string* errorOut) {
    std::ifstream file(loaderPath, std::ios::binary | std::ios::ate);
    if (!file) {
        if (errorOut) *errorOut = "无法打开 Firehose Loader 文件";
        return false;
    }
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> loaderData(size);
    file.read(reinterpret_cast<char*>(loaderData.data()), size);

    Logger::Instance().Info("开始 Sahara 握手，loader 大小 " + std::to_string(size) + " 字节");

    SaharaHelloPacket hello{};
    if (!WaitHello(&hello, errorOut)) return false;
    if (!SendHelloResponse(errorOut)) return false;
    if (!ServeReadRequests(loaderData, errorOut)) return false;

    // 最后设备会发 DONE，主机回 DONE_RESPONSE，握手正式结束
    uint8_t buf[32] = {0};
    size_t got = 0;
    if (!device_.BulkRead(buf, sizeof(buf), &got, 5000)) {
        if (errorOut) *errorOut = "等待 DONE 超时";
        return false;
    }

    SaharaHeader doneResp{};
    doneResp.command = static_cast<uint32_t>(SaharaCommand::DoneResponse);
    doneResp.length = sizeof(doneResp);
    device_.BulkWrite(reinterpret_cast<uint8_t*>(&doneResp), sizeof(doneResp));

    Logger::Instance().Success("Sahara 握手完成，Firehose Loader 已在设备端启动");
    return true;
}

bool SaharaProtocol::ServeReadRequestsMultiImage(std::vector<std::vector<uint8_t>>& pendingFiles,
                                                  std::string* errorOut) {
    uint8_t buf[64] = {0};
    // key = 设备的imageId，value = 已经分配给这个imageId的文件数据（第一次遇到才分配）
    std::map<uint32_t, std::vector<uint8_t>*> imageIdToData;
    std::vector<std::vector<uint8_t>> allocated; // 持有实际数据，防止指针悬空

    size_t nextFileIndex = 0;

    while (true) {
        size_t got = 0;
        if (!device_.BulkRead(buf, sizeof(buf), &got, 8000)) {
            if (errorOut) *errorOut = "等待设备读取请求超时";
            return false;
        }
        if (got < sizeof(SaharaHeader)) {
            if (errorOut) *errorOut = "收到异常短包";
            return false;
        }

        SaharaHeader header;
        memcpy(&header, buf, sizeof(header));

        if (header.command == static_cast<uint32_t>(SaharaCommand::ReadData)) {
            SaharaReadDataPacket req;
            memcpy(&req, buf, sizeof(req));

            // 这个imageId第一次出现：从待分配队列里取下一个文件给它
            if (imageIdToData.find(req.imageId) == imageIdToData.end()) {
                if (nextFileIndex >= pendingFiles.size()) {
                    if (errorOut) *errorOut = "设备请求了第" + std::to_string(nextFileIndex + 1) +
                        "个镜像（imageId=" + std::to_string(req.imageId) + "），但只提供了" +
                        std::to_string(pendingFiles.size()) + "个文件——多镜像的文件数量或顺序"
                        "可能不对，需要用抓包确认真实的imageId分配规律";
                    return false;
                }
                imageIdToData[req.imageId] = &pendingFiles[nextFileIndex];
                Logger::Instance().Info("Sahara 分配第 " + std::to_string(nextFileIndex + 1) +
                    " 个文件给 imageId=" + std::to_string(req.imageId));
                nextFileIndex++;
            }

            std::vector<uint8_t>* imageData = imageIdToData[req.imageId];
            if (req.offset + req.length > imageData->size()) {
                if (errorOut) *errorOut = "设备请求的偏移超出对应文件范围（imageId=" +
                    std::to_string(req.imageId) + "）";
                return false;
            }

            if (!device_.BulkWrite(imageData->data() + req.offset, req.length)) {
                if (errorOut) *errorOut = "发送镜像数据块失败";
                return false;
            }
            continue;
        }

        if (header.command == static_cast<uint32_t>(SaharaCommand::EndOfImageTransfer)) {
            SaharaEndOfImageTransferPacket endPkt;
            memcpy(&endPkt, buf, sizeof(endPkt));

            if (endPkt.status != 0) {
                if (errorOut) *errorOut = "设备报告镜像传输失败，imageId=" +
                    std::to_string(endPkt.imageId) + " status=" + std::to_string(endPkt.status);
                return false;
            }

            SaharaEndOfImageTransferPacket ack = endPkt;
            device_.BulkWrite(reinterpret_cast<uint8_t*>(&ack), sizeof(ack));

            // 所有文件都被请求过一轮之后，如果设备紧接着不再发新的 READ_DATA
            // 而是直接进入 DONE 阶段，PerformHandshakeMultiImage 那边会处理；
            // 这里只要有一个镜像完成传输就继续等下一条消息，直到外层判断完成
            if (nextFileIndex >= pendingFiles.size()) {
                Logger::Instance().Success("Sahara 全部 " + std::to_string(pendingFiles.size()) +
                    " 个镜像传输完成");
                return true;
            }
            continue;
        }

        if (errorOut) *errorOut = "收到未处理的 Sahara 命令: " + std::to_string(header.command);
        return false;
    }
}

bool SaharaProtocol::PerformHandshakeMultiImage(const std::vector<std::wstring>& imagePathsInOrder,
                                                 std::string* errorOut) {
    if (imagePathsInOrder.empty()) {
        if (errorOut) *errorOut = "至少需要提供一个镜像文件";
        return false;
    }

    std::vector<std::vector<uint8_t>> files;
    for (const auto& path : imagePathsInOrder) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            if (errorOut) *errorOut = "无法打开文件";
            return false;
        }
        size_t size = static_cast<size_t>(file.tellg());
        file.seekg(0);
        std::vector<uint8_t> data(size);
        file.read(reinterpret_cast<char*>(data.data()), size);
        files.push_back(std::move(data));
    }

    Logger::Instance().Info("开始多镜像 Sahara 握手，共 " + std::to_string(files.size()) + " 个文件");

    SaharaHelloPacket hello{};
    if (!WaitHello(&hello, errorOut)) return false;
    if (!SendHelloResponse(errorOut)) return false;
    if (!ServeReadRequestsMultiImage(files, errorOut)) return false;

    uint8_t buf[32] = {0};
    size_t got = 0;
    if (!device_.BulkRead(buf, sizeof(buf), &got, 5000)) {
        if (errorOut) *errorOut = "等待 DONE 超时";
        return false;
    }

    SaharaHeader doneResp{};
    doneResp.command = static_cast<uint32_t>(SaharaCommand::DoneResponse);
    doneResp.length = sizeof(doneResp);
    device_.BulkWrite(reinterpret_cast<uint8_t*>(&doneResp), sizeof(doneResp));

    Logger::Instance().Success("多镜像 Sahara 握手完成");
    return true;
}

} // namespace zhi::protocol::edl
