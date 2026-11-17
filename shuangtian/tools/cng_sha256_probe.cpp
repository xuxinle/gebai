// 验证 Windows CNG（BCrypt）的 SHA-256 速度：框架自实现是 267MB/s，CNG 走 SHA-NI 应该快得多
#define NOMINMAX 1
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "st/core/fs.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1]
                                    : "D:/code/gebai-pro/packages/desktop/dist/gebai.exe";
  auto bytes = st::fs::read_bytes(path);
  if (!bytes) {
    std::printf("读失败\n");
    return 1;
  }
  std::printf("文件 %.1f MB\n\n", static_cast<double>(bytes->size()) / 1048576.0);

  BCRYPT_ALG_HANDLE alg = nullptr;
  NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (st != 0) {
    std::printf("BCryptOpenAlgorithmProvider 失败: 0x%08lX\n", static_cast<unsigned long>(st));
    return 1;
  }

  DWORD obj_size = 0;
  DWORD result = 0;
  BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_size),
                    sizeof(obj_size), &result, 0);
  DWORD hash_size = 0;
  BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                    sizeof(hash_size), &result, 0);

  std::vector<UCHAR> obj(obj_size);
  std::vector<UCHAR> digest(hash_size);

  auto t0 = Clock::now();
  BCRYPT_HASH_HANDLE hash = nullptr;
  BCryptCreateHash(alg, &hash, obj.data(), obj_size, nullptr, 0, 0);
  BCryptHashData(hash, bytes->data(), static_cast<ULONG>(bytes->size()), 0);
  BCryptFinishHash(hash, digest.data(), hash_size, 0);
  BCryptDestroyHash(hash);
  const double cng_ms = ms_since(t0);

  std::printf("CNG SHA-256: %8.1f ms  => ", cng_ms);
  for (DWORD i = 0; i < hash_size && i < 8; ++i) std::printf("%02x", digest[i]);
  std::printf("…\n");
  std::printf("吞吐 %.0f MB/s\n", (bytes->size() / 1048576.0) / (cng_ms / 1000.0));

  BCryptCloseAlgorithmProvider(alg, 0);
  return 0;
}
