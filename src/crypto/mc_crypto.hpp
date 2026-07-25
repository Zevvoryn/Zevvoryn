#pragma once
// ONLINE_V1 / ONLINE_HEADERONLY_V1: Windows-native crypto (CNG/bcrypt) + WinHTTP
// session auth for Mojang online-mode. Header-only so it does not need to be a
// separate compiled .cpp in the build (the project's CMake glob does not include
// crypto/). All definitions are inline and libs are auto-linked via #pragma comment.
#include "../core/types.hpp"
#include <vector>
#include <string>
#include <span>
#include <optional>
#include <cstddef>

namespace nc::crypto {

// Lazily-created 1024-bit RSA server keypair, shared by every connection.
class ServerKey {
public:
    static ServerKey& instance();
    bool valid() const { return keyHandle_ != nullptr; }
    // DER X.509 SubjectPublicKeyInfo (goes into EncryptionRequest).
    const std::vector<u8>& publicKeyDer() const { return publicDer_; }
    // RSA/PKCS#1 v1.5 decrypt with the private key. Empty vector on failure.
    std::vector<u8> decrypt(std::span<const u8> ciphertext) const;

    ServerKey(const ServerKey&) = delete;
    ServerKey& operator=(const ServerKey&) = delete;
private:
    ServerKey();
    void* algHandle_ = nullptr; // BCRYPT_ALG_HANDLE (kept alive for key lifetime)
    void* keyHandle_ = nullptr; // BCRYPT_KEY_HANDLE
    std::vector<u8> publicDer_;
};

// AES-128-CFB8 stream cipher; one instance per direction.
class AesCfb8 {
public:
    AesCfb8() = default;
    ~AesCfb8();
    AesCfb8(const AesCfb8&) = delete;
    AesCfb8& operator=(const AesCfb8&) = delete;

    // AES key AND the initial IV are both the 16-byte shared secret.
    bool init(std::span<const u8> key16);
    bool ready() const { return keyHandle_ != nullptr; }
    void encrypt(u8* data, std::size_t len);
    void decrypt(u8* data, std::size_t len);
private:
    void* keyHandle_ = nullptr; // BCRYPT_KEY_HANDLE (AES / CFB / block length 1)
    unsigned char iv_[16] = {};
};

// Minecraft "server hash": sha1(serverId || sharedSecret || pubKeyDer) rendered
// as a signed two's-complement big integer in hex (may start with '-').
std::string mcServerHash(std::span<const u8> serverIdAscii,
                         std::span<const u8> sharedSecret,
                         std::span<const u8> pubKeyDer);

// GET https://sessionserver.mojang.com/session/minecraft/hasJoined
// Returns the JSON body on HTTP 200, std::nullopt otherwise.
std::optional<std::string> hasJoined(const std::string& username,
                                     const std::string& serverHash);

// SKIN_OFFLINE_V1: generic HTTPS GET (used for Mojang profile/skin lookups so
// offline-mode players can still get their premium skin by name). Returns the
// response body on HTTP 200, std::nullopt otherwise.
std::optional<std::string> httpsGet(const std::string& host, const std::string& path);

// Cryptographically secure random bytes (verify token, etc.).
std::vector<u8> randomBytes(std::size_t n);

} // namespace nc::crypto

// ============================================================================
// Header-only implementation
// ============================================================================
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <mutex>
#include <cstring>
#include "../core/log.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace nc::crypto {
namespace detail {

inline std::wstring toW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// --- minimal DER encoders (RSA public key -> X.509 SubjectPublicKeyInfo) ---
inline void derLen(std::vector<u8>& out, size_t len) {
    if (len < 0x80) { out.push_back((u8)len); return; }
    u8 tmp[8]; int n = 0;
    while (len) { tmp[n++] = (u8)(len & 0xFF); len >>= 8; }
    out.push_back((u8)(0x80 | n));
    for (int i = n - 1; i >= 0; --i) out.push_back(tmp[i]);
}
inline void derInteger(std::vector<u8>& out, const u8* p, size_t n) {
    size_t i = 0; while (i + 1 < n && p[i] == 0) ++i;
    bool pad = (p[i] & 0x80) != 0;
    out.push_back(0x02);
    derLen(out, (n - i) + (pad ? 1u : 0u));
    if (pad) out.push_back(0x00);
    out.insert(out.end(), p + i, p + n);
}
inline std::vector<u8> derSeq(const std::vector<u8>& body) {
    std::vector<u8> out; out.push_back(0x30); derLen(out, body.size());
    out.insert(out.end(), body.begin(), body.end()); return out;
}
inline std::vector<u8> buildSpki(const u8* mod, size_t modLen, const u8* exp, size_t expLen) {
    std::vector<u8> ints; derInteger(ints, mod, modLen); derInteger(ints, exp, expLen);
    std::vector<u8> rsaPub = derSeq(ints);
    static const u8 algId[] = {0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x05,0x00};
    std::vector<u8> bitStr; bitStr.push_back(0x03);
    derLen(bitStr, rsaPub.size() + 1); bitStr.push_back(0x00);
    bitStr.insert(bitStr.end(), rsaPub.begin(), rsaPub.end());
    std::vector<u8> body(algId, algId + sizeof(algId));
    body.insert(body.end(), bitStr.begin(), bitStr.end());
    return derSeq(body);
}

inline BCRYPT_ALG_HANDLE g_aesAlg = nullptr;
inline std::once_flag g_aesOnce;
inline bool ensureAesAlg() {
    std::call_once(g_aesOnce, [](){
        if (NT_SUCCESS(BCryptOpenAlgorithmProvider(&g_aesAlg, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
            BCryptSetProperty(g_aesAlg, BCRYPT_CHAINING_MODE,
                (PUCHAR)BCRYPT_CHAIN_MODE_CFB, sizeof(BCRYPT_CHAIN_MODE_CFB), 0);
        }
    });
    return g_aesAlg != nullptr;
}

} // namespace detail

inline ServerKey& ServerKey::instance() { static ServerKey k; return k; }

inline ServerKey::ServerKey() {
    NC_INFO("Crypto", "Generating RSA-1024 keypair...");
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0))) {
        NC_ERROR("Crypto", "BCryptOpenAlgorithmProvider(RSA) failed"); return;
    }
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!NT_SUCCESS(BCryptGenerateKeyPair(alg, &hKey, 1024, 0)) ||
        !NT_SUCCESS(BCryptFinalizeKeyPair(hKey, 0))) {
        NC_ERROR("Crypto", "RSA key generation failed");
        if (hKey) BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(alg, 0);
        return;
    }
    ULONG cb = 0;
    BCryptExportKey(hKey, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &cb, 0);
    if (cb == 0) {
        NC_ERROR("Crypto", "BCryptExportKey(size) failed");
        BCryptDestroyKey(hKey); BCryptCloseAlgorithmProvider(alg, 0); return;
    }
    std::vector<u8> blob(cb);
    if (!NT_SUCCESS(BCryptExportKey(hKey, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), cb, &cb, 0))) {
        NC_ERROR("Crypto", "BCryptExportKey(public) failed");
        BCryptDestroyKey(hKey); BCryptCloseAlgorithmProvider(alg, 0); return;
    }
    auto* hdr = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    const u8* expPtr = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
    const u8* modPtr = expPtr + hdr->cbPublicExp;
    publicDer_ = detail::buildSpki(modPtr, hdr->cbModulus, expPtr, hdr->cbPublicExp);
    algHandle_ = alg;   // keep provider alive for the key's lifetime
    keyHandle_ = hKey;
    NC_INFO("Crypto", "RSA keypair generated ({}-byte X.509 public key).", (int)publicDer_.size());
}

inline std::vector<u8> ServerKey::decrypt(std::span<const u8> ct) const {
    if (!keyHandle_ || ct.empty()) return {};
    ULONG out = 0;
    if (!NT_SUCCESS(BCryptDecrypt((BCRYPT_KEY_HANDLE)keyHandle_, (PUCHAR)ct.data(), (ULONG)ct.size(),
                                  nullptr, nullptr, 0, nullptr, 0, &out, BCRYPT_PAD_PKCS1))) return {};
    std::vector<u8> res(out ? out : 1);
    if (!NT_SUCCESS(BCryptDecrypt((BCRYPT_KEY_HANDLE)keyHandle_, (PUCHAR)ct.data(), (ULONG)ct.size(),
                                  nullptr, nullptr, 0, res.data(), out, &out, BCRYPT_PAD_PKCS1))) return {};
    res.resize(out);
    return res;
}

inline AesCfb8::~AesCfb8() {
    if (keyHandle_) BCryptDestroyKey((BCRYPT_KEY_HANDLE)keyHandle_);
}
inline bool AesCfb8::init(std::span<const u8> key16) {
    if (key16.size() != 16 || !detail::ensureAesAlg()) return false;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(detail::g_aesAlg, &hKey, nullptr, 0,
            (PUCHAR)key16.data(), 16, 0))) return false;
    ULONG one = 1;
    BCryptSetProperty(hKey, BCRYPT_MESSAGE_BLOCK_LENGTH, (PUCHAR)&one, sizeof(one), 0);
    keyHandle_ = hKey;
    std::memcpy(iv_, key16.data(), 16);
    return true;
}
inline void AesCfb8::encrypt(u8* data, std::size_t len) {
    if (!keyHandle_ || len == 0) return;
    ULONG done = 0;
    BCryptEncrypt((BCRYPT_KEY_HANDLE)keyHandle_, data, (ULONG)len, nullptr,
                  iv_, 16, data, (ULONG)len, &done, 0);
}
inline void AesCfb8::decrypt(u8* data, std::size_t len) {
    if (!keyHandle_ || len == 0) return;
    ULONG done = 0;
    BCryptDecrypt((BCRYPT_KEY_HANDLE)keyHandle_, data, (ULONG)len, nullptr,
                  iv_, 16, data, (ULONG)len, &done, 0);
}

inline std::string mcServerHash(std::span<const u8> serverId, std::span<const u8> secret, std::span<const u8> pub) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) return {};
    BCRYPT_HASH_HANDLE h = nullptr;
    u8 digest[20] = {};
    bool ok = NT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
    if (ok) {
        if (!serverId.empty()) BCryptHashData(h, (PUCHAR)serverId.data(), (ULONG)serverId.size(), 0);
        if (!secret.empty())   BCryptHashData(h, (PUCHAR)secret.data(),   (ULONG)secret.size(),   0);
        if (!pub.empty())      BCryptHashData(h, (PUCHAR)pub.data(),      (ULONG)pub.size(),      0);
        BCryptFinishHash(h, digest, 20, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    bool neg = (digest[0] & 0x80) != 0;
    if (neg) {
        int carry = 1;
        for (int i = 19; i >= 0; --i) { int v = (digest[i] ^ 0xFF) + carry; digest[i] = (u8)(v & 0xFF); carry = v >> 8; }
    }
    static const char* hx = "0123456789abcdef";
    std::string s; s.reserve(41);
    for (int i = 0; i < 20; ++i) { s.push_back(hx[digest[i] >> 4]); s.push_back(hx[digest[i] & 0xF]); }
    size_t start = s.find_first_not_of('0');
    s = (start == std::string::npos) ? std::string("0") : s.substr(start);
    if (neg) s.insert(s.begin(), '-');
    return s;
}

inline std::optional<std::string> httpsGet(const std::string& host, const std::string& path) {
    std::wstring whost = detail::toW(host);
    std::wstring wpath = detail::toW(path);
    HINTERNET hSession = WinHttpOpen(L"NetherCraft/1.21.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return std::nullopt;
    std::optional<std::string> result;
    if (HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    std::string body;
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
                        std::string chunk; chunk.resize(avail);
                        DWORD readN = 0;
                        if (!WinHttpReadData(hReq, chunk.data(), avail, &readN) || readN == 0) break;
                        chunk.resize(readN);
                        body += chunk;
                    }
                    result = body;
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

inline std::optional<std::string> hasJoined(const std::string& username, const std::string& serverHash) {
    return httpsGet("sessionserver.mojang.com",
        "/session/minecraft/hasJoined?username=" + username + "&serverId=" + serverHash);
}

inline std::vector<u8> randomBytes(std::size_t n) {
    std::vector<u8> v(n);
    if (!NT_SUCCESS(BCryptGenRandom(nullptr, v.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        for (auto& b : v) b = (u8)(GetTickCount() ^ rand());
    }
    return v;
}

} // namespace nc::crypto

#else  // ---- non-Windows stub (online-mode unavailable) ----
#include <cstdlib>
namespace nc::crypto {
inline ServerKey& ServerKey::instance() { static ServerKey k; return k; }
inline ServerKey::ServerKey() {}
inline std::vector<u8> ServerKey::decrypt(std::span<const u8>) const { return {}; }
inline AesCfb8::~AesCfb8() {}
inline bool AesCfb8::init(std::span<const u8>) { return false; }
inline void AesCfb8::encrypt(u8*, std::size_t) {}
inline void AesCfb8::decrypt(u8*, std::size_t) {}
inline std::string mcServerHash(std::span<const u8>, std::span<const u8>, std::span<const u8>) { return {}; }
inline std::optional<std::string> hasJoined(const std::string&, const std::string&) { return std::nullopt; }
inline std::optional<std::string> httpsGet(const std::string&, const std::string&) { return std::nullopt; }
inline std::vector<u8> randomBytes(std::size_t n) { std::vector<u8> v(n); for (auto& b : v) b = (u8)rand(); return v; }
} // namespace nc::crypto
#endif
