#include "AdbAuth.h"
#include "../../core/Logger.h"

#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>

namespace zhi::protocol::adb {

using zhi::Logger;

namespace {

constexpr int kKeyBits = 2048;
constexpr int kRsaWords = kKeyBits / 32; // RSANUMWORDS，AOSP 命名

// AOSP RSAPublicKey 结构体的字段顺序（详见 android_pubkey.c），
// 全部按小端序写入这个 vector：
//   uint32 modulus_size_words
//   uint32 n0inv
//   uint8  modulus[RSANUMWORDS*4]   （小端）
//   uint8  rr[RSANUMWORDS*4]        （小端，RR = R^2 mod n，R = 2^(RSANUMWORDS*32)）
//   uint32 exponent
bool EncodeAndroidPubkeyStruct(RSA* rsa, std::vector<uint8_t>* out) {
    const BIGNUM* n = nullptr;
    const BIGNUM* e = nullptr;
    RSA_get0_key(rsa, &n, &e, nullptr);

    if (BN_num_bits(n) != kKeyBits) {
        return false; // 只支持标准2048位密钥，跟AOSP默认一致
    }

    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* r = BN_new();
    BIGNUM* rr = BN_new();
    BIGNUM* n0 = BN_new();
    BIGNUM* n0inv = BN_new();

    // n0inv = -(n^-1) mod 2^32
    BN_set_bit(r, 32);
    BN_mod(n0, n, r, ctx);
    BN_mod_inverse(n0inv, n0, r, ctx);
    BN_sub(n0inv, r, n0inv);

    // rr = R^2 mod n， R = 2^(kRsaWords*32) = 2^2048
    BIGNUM* bigR = BN_new();
    BN_set_bit(bigR, kRsaWords * 32);
    BN_mod_sqr(rr, bigR, n, ctx);

    auto appendU32LE = [&](std::vector<uint8_t>& buf, uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };

    // BIGNUM -> 固定长度小端字节数组（BN_bn2binpad 是大端，所以先大端导出再反转）
    auto bnToLittleEndianFixed = [&](const BIGNUM* bn, int byteLen) -> std::vector<uint8_t> {
        std::vector<uint8_t> be(byteLen, 0);
        BN_bn2binpad(bn, be.data(), byteLen);
        std::reverse(be.begin(), be.end());
        return be;
    };

    out->clear();
    appendU32LE(*out, static_cast<uint32_t>(kRsaWords));

    uint32_t n0invWord = static_cast<uint32_t>(BN_get_word(n0inv));
    appendU32LE(*out, n0invWord);

    auto modulusLE = bnToLittleEndianFixed(n, kRsaWords * 4);
    out->insert(out->end(), modulusLE.begin(), modulusLE.end());

    auto rrLE = bnToLittleEndianFixed(rr, kRsaWords * 4);
    out->insert(out->end(), rrLE.begin(), rrLE.end());

    uint32_t expWord = static_cast<uint32_t>(BN_get_word(e));
    appendU32LE(*out, expWord);

    BN_free(r);
    BN_free(rr);
    BN_free(n0);
    BN_free(n0inv);
    BN_free(bigR);
    BN_CTX_free(ctx);
    return true;
}

// 极简 base64 编码（避免再引入额外依赖，标准字母表，无换行）
std::string Base64Encode(const std::vector<uint8_t>& data) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t n = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += table[(n >> 6) & 0x3F];
        out += table[n & 0x3F];
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = data[i] << 16;
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (data[i] << 16) | (data[i+1] << 8);
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += table[(n >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

} // namespace

RSA* AdbAuth::LoadOrCreateKey(const std::wstring& privateKeyPath, std::string* errorOut) {
    // 先尝试从已有文件加载
    FILE* f = _wfopen(privateKeyPath.c_str(), L"rb");
    if (f) {
        RSA* rsa = PEM_read_RSAPrivateKey(f, nullptr, nullptr, nullptr);
        fclose(f);
        if (rsa) {
            Logger::Instance().Info("已加载本地 ADB 密钥");
            return rsa;
        }
        // 文件存在但解析失败，继续走"生成新密钥"分支
    }

    Logger::Instance().Info("本地未找到可用的 ADB 密钥，正在生成新的 2048 位 RSA 密钥对……");

    BIGNUM* bne = BN_new();
    BN_set_word(bne, RSA_F4); // 标准公钥指数 65537，跟 AOSP 一致

    RSA* rsa = RSA_new();
    if (!RSA_generate_key_ex(rsa, kKeyBits, bne, nullptr)) {
        if (errorOut) *errorOut = "RSA 密钥生成失败";
        RSA_free(rsa);
        BN_free(bne);
        return nullptr;
    }
    BN_free(bne);

    FILE* out = _wfopen(privateKeyPath.c_str(), L"wb");
    if (out) {
        PEM_write_RSAPrivateKey(out, rsa, nullptr, nullptr, 0, nullptr, nullptr);
        fclose(out);
        Logger::Instance().Success("新密钥已生成并保存");
    } else {
        Logger::Instance().Warning("密钥生成成功但保存到本地失败，下次启动需要重新生成"
            "（意味着需要在手机上重新确认一次调试授权）");
    }

    return rsa;
}

bool AdbAuth::SignToken(RSA* key, const uint8_t token[20], std::vector<uint8_t>* signatureOut,
                        std::string* errorOut) {
    signatureOut->resize(RSA_size(key));
    unsigned int sigLen = 0;

    // 关键点：这里传的 NID_sha1 只是告诉 OpenSSL"按SHA1的DigestInfo前缀格式做PKCS1填充"，
    // 不会对 token 再做一次真正的哈希运算——token 本身就是协议规定要直接签的20字节数据。
    int ok = RSA_sign(NID_sha1, token, 20, signatureOut->data(), &sigLen, key);
    if (!ok) {
        if (errorOut) *errorOut = "RSA_sign 失败";
        return false;
    }
    signatureOut->resize(sigLen);
    return true;
}

std::string AdbAuth::EncodePublicKey(RSA* key, const std::string& userAtHost, std::string* errorOut) {
    std::vector<uint8_t> structBytes;
    if (!EncodeAndroidPubkeyStruct(key, &structBytes)) {
        if (errorOut) *errorOut = "公钥编码失败（可能不是2048位密钥）";
        return "";
    }

    std::string b64 = Base64Encode(structBytes);
    return b64 + " " + userAtHost;
}

void AdbAuth::FreeKey(RSA* key) {
    if (key) RSA_free(key);
}

} // namespace zhi::protocol::adb
