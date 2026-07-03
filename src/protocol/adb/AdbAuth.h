#pragma once
// ============================================================================
// AdbAuth —— ADB 的 RSA 密钥生成/签名/公钥编码
//
// 这部分严格照抄 AOSP system/core/adb/adb_auth_host.cpp +
// libcrypto_utils/android_pubkey.c 的公开实现逻辑，没有自己发明协议：
//
//   1. 密钥对：本地生成一个 2048位 RSA 密钥对，存到本地文件（类似官方 adb
//      的 ~/.android/adbkey / adbkey.pub），下次启动直接复用，不用每次都
//      让用户在手机上重新确认。
//
//   2. 签名：设备发来一个 20 字节的随机 token（AUTH 消息，arg0=TOKEN）。
//      主机要签名这个 token 并发回去（AUTH 消息，arg0=SIGNATURE）。
//      具体做法是调用 RSA_sign(NID_sha1, token, 20, ...) ——注意这里不是
//      "先算SHA1再签名"，是把这 20 字节 token 直接当成"已经算好的SHA1摘要"
//      去走标准 PKCS#1 v1.5 签名流程（OpenSSL 会自动补上 SHA1 的 ASN.1
//      DigestInfo 前缀）。这是协议本身的设计，AOSP 官方 adb 就是这么做的。
//
//   3. 如果设备不认识这个公钥（比如第一次连这台手机），设备签名验证会
//      失败，还会再发一次 AUTH TOKEN。这时主机改发公钥（AUTH 消息，
//      arg0=RSAPUBLICKEY），此时手机屏幕上会弹出"是否允许USB调试"，
//      用户手动点确认后，设备才会真正接受这个公钥并发 CNXN。
//      ——这一步没法自动化，也不应该自动化，是 Android 系统本身的安全设计。
//
// 公钥编码格式：ADB 用的不是标准 PEM/DER，而是 AOSP 自定义的一个精简二进制
// 结构（modulus_size_words + n0inv + modulus + RR + exponent），编码后再
// base64。这是历史遗留的私有格式但本身完全公开（AOSP 源码可查），第三方
// ADB 客户端库（Python adb-shell 等）都是照这个格式重新实现的。
// ============================================================================

#include <openssl/rsa.h>
#include <string>
#include <vector>
#include <cstdint>

namespace zhi::protocol::adb {

class AdbAuth {
public:
    // 从本地文件加载密钥对，不存在就生成一份新的并保存（PEM格式私钥）
    static RSA* LoadOrCreateKey(const std::wstring& privateKeyPath, std::string* errorOut = nullptr);

    // 对 20 字节 token 签名，返回签名结果（2048位密钥对应 256 字节签名）
    static bool SignToken(RSA* key, const uint8_t token[20], std::vector<uint8_t>* signatureOut,
                          std::string* errorOut = nullptr);

    // 把公钥编码成 ADB 要求的格式：base64(AOSP自定义二进制结构) + " user@host"
    static std::string EncodePublicKey(RSA* key, const std::string& userAtHost,
                                       std::string* errorOut = nullptr);

    static void FreeKey(RSA* key);
};

} // namespace zhi::protocol::adb
