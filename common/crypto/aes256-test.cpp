#include "common/crypto/aes256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <type_traits>

#include <gtest/gtest.h>

#include "common/crypto/aes256-aarch64.h"
#include "common/crypto/aes256-generic.h"
#include "common/crypto/aes256-x86_64.h"

template<size_t plaintext_bytes, size_t iv_bytes, size_t key_bytes>
class AESCtx : public ::testing::Test {
protected:
  static constexpr std::size_t bits_in_uint8 = std::numeric_limits<std::uint8_t>::digits;
  static constexpr std::size_t aes_ctx_plaintext_bytes = plaintext_bytes;
  static constexpr std::size_t aes_ctx_iv_bytes = iv_bytes;
  static constexpr std::size_t aes_ctx_key_bytes = key_bytes;
  static constexpr std::size_t aes_ctx_key_bits = key_bytes * bits_in_uint8;

  void SetUp() override {
    std::independent_bits_engine<std::default_random_engine, bits_in_uint8, std::uint8_t> engine;
    std::generate(std::begin(random_key_), std::end(random_key_), std::ref(engine));
    std::generate(std::begin(random_iv_), std::end(random_iv_), std::ref(engine));
    random_iv_initial_ = random_iv_;
    std::generate(std::begin(random_payload_), std::end(random_payload_), std::ref(engine));
  }

  void TearDown() override {}

  std::array<std::uint8_t, plaintext_bytes> random_payload_;
  std::array<std::uint8_t, iv_bytes> random_iv_;
  std::array<std::uint8_t, iv_bytes> random_iv_initial_;
  std::array<std::uint8_t, key_bytes> random_key_;
};

using AES256_cbc_random = AESCtx<1024, 16, 32>;
TEST_F(AES256_cbc_random, random_cbc) {
  vk_aes_ctx_t ctx;
  vk_aes_set_encrypt_key(&ctx, random_key_.data(), aes_ctx_key_bits);

  std::array<std::uint8_t, aes_ctx_plaintext_bytes> ciphertext;
  ctx.cbc_crypt(&ctx, random_payload_.data(), ciphertext.data(), aes_ctx_plaintext_bytes, random_iv_.data());

  vk_aes_set_decrypt_key(&ctx, random_key_.data(), aes_ctx_key_bits);
  std::array<std::uint8_t, aes_ctx_plaintext_bytes> plaintext;
  ctx.cbc_crypt(&ctx, ciphertext.data(), plaintext.data(), aes_ctx_plaintext_bytes, random_iv_initial_.data());

  EXPECT_EQ(plaintext, random_payload_);
}

TEST(AES256_cbc, test_vector) {
  {
    constexpr std::size_t key_bits = AES256_KEY_BITS;
    const std::array<std::uint8_t, 32> key = {{0x60, 0x3D, 0xEB, 0x10, 0x15, 0xCA, 0x71, 0xBE, 0x2B, 0x73, 0xAE, 0xF0, 0x85, 0x7D, 0x77, 0x81,
                                               0x1F, 0x35, 0x2C, 0x07, 0x3B, 0x61, 0x08, 0xD7, 0x2D, 0x98, 0x10, 0xA3, 0x09, 0x14, 0xDF, 0xF4}};
    const std::array<std::uint8_t, 16> iv = {{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}};
    const std::array<std::uint8_t, 16> plaintext = {{0x6B, 0xC1, 0xBE, 0xE2, 0x2E, 0x40, 0x9F, 0x96, 0xE9, 0x3D, 0x7E, 0x11, 0x73, 0x93, 0x17, 0x2A}};
    const std::array<std::uint8_t, 16> ciphertext = {{0xF5, 0x8C, 0x4C, 0x04, 0xD6, 0xE5, 0xF1, 0xBA, 0x77, 0x9E, 0xAB, 0xFB, 0x5F, 0x7B, 0xFB, 0xD6}};

    std::remove_const<decltype(ciphertext)>::type actual_ciphertext;
    auto iv_copy = iv;
    vk_aes_ctx_t ctx;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.cbc_crypt(&ctx, plaintext.data(), actual_ciphertext.data(), plaintext.size(), iv_copy.data());
    EXPECT_EQ(ciphertext, actual_ciphertext);

    std::remove_const<decltype(ciphertext)>::type actual_plaintext;
    iv_copy = iv;
    vk_aes_set_decrypt_key(&ctx, key.data(), key_bits);
    ctx.cbc_crypt(&ctx, ciphertext.data(), actual_plaintext.data(), ciphertext.size(), iv_copy.data());
    EXPECT_EQ(plaintext, actual_plaintext);
  }
  {
    constexpr std::size_t key_bits = AES256_KEY_BITS;
    const std::array<std::uint8_t, 32> key = {{0x60, 0x3D, 0xEB, 0x10, 0x15, 0xCA, 0x71, 0xBE, 0x2B, 0x73, 0xAE, 0xF0, 0x85, 0x7D, 0x77, 0x81,
                                               0x1F, 0x35, 0x2C, 0x07, 0x3B, 0x61, 0x08, 0xD7, 0x2D, 0x98, 0x10, 0xA3, 0x09, 0x14, 0xDF, 0xF4}};
    const std::array<std::uint8_t, 16> iv = {{0xF5, 0x8C, 0x4C, 0x04, 0xD6, 0xE5, 0xF1, 0xBA, 0x77, 0x9E, 0xAB, 0xFB, 0x5F, 0x7B, 0xFB, 0xD6}};
    const std::array<std::uint8_t, 16> plaintext = {{0xAE, 0x2D, 0x8A, 0x57, 0x1E, 0x03, 0xAC, 0x9C, 0x9E, 0xB7, 0x6F, 0xAC, 0x45, 0xAF, 0x8E, 0x51}};
    const std::array<std::uint8_t, 16> ciphertext = {{0x9C, 0xFC, 0x4E, 0x96, 0x7E, 0xDB, 0x80, 0x8D, 0x67, 0x9F, 0x77, 0x7B, 0xC6, 0x70, 0x2C, 0x7D}};

    std::remove_const<decltype(ciphertext)>::type actual_ciphertext;
    auto iv_copy = iv;
    vk_aes_ctx_t ctx;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.cbc_crypt(&ctx, plaintext.data(), actual_ciphertext.data(), plaintext.size(), iv_copy.data());
    EXPECT_EQ(ciphertext, actual_ciphertext);

    std::remove_const<decltype(ciphertext)>::type actual_plaintext;
    iv_copy = iv;
    vk_aes_set_decrypt_key(&ctx, key.data(), key_bits);
    ctx.cbc_crypt(&ctx, ciphertext.data(), actual_plaintext.data(), ciphertext.size(), iv_copy.data());
    EXPECT_EQ(plaintext, actual_plaintext);
  }
  {
    constexpr std::size_t key_bits = AES256_KEY_BITS;
    const std::array<std::uint8_t, 32> key = {{0x60, 0x3D, 0xEB, 0x10, 0x15, 0xCA, 0x71, 0xBE, 0x2B, 0x73, 0xAE, 0xF0, 0x85, 0x7D, 0x77, 0x81,
                                               0x1F, 0x35, 0x2C, 0x07, 0x3B, 0x61, 0x08, 0xD7, 0x2D, 0x98, 0x10, 0xA3, 0x09, 0x14, 0xDF, 0xF4}};
    const std::array<std::uint8_t, 16> iv = {{0x9C, 0xFC, 0x4E, 0x96, 0x7E, 0xDB, 0x80, 0x8D, 0x67, 0x9F, 0x77, 0x7B, 0xC6, 0x70, 0x2C, 0x7D}};
    const std::array<std::uint8_t, 16> plaintext = {{0x30, 0xC8, 0x1C, 0x46, 0xA3, 0x5C, 0xE4, 0x11, 0xE5, 0xFB, 0xC1, 0x19, 0x1A, 0x0A, 0x52, 0xEF}};
    const std::array<std::uint8_t, 16> ciphertext = {{0x39, 0xF2, 0x33, 0x69, 0xA9, 0xD9, 0xBA, 0xCF, 0xA5, 0x30, 0xE2, 0x63, 0x04, 0x23, 0x14, 0x61}};

    std::remove_const<decltype(ciphertext)>::type actual_ciphertext;
    auto iv_copy = iv;
    vk_aes_ctx_t ctx;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.cbc_crypt(&ctx, plaintext.data(), actual_ciphertext.data(), plaintext.size(), iv_copy.data());
    EXPECT_EQ(ciphertext, actual_ciphertext);

    std::remove_const<decltype(ciphertext)>::type actual_plaintext;
    iv_copy = iv;
    vk_aes_set_decrypt_key(&ctx, key.data(), key_bits);
    ctx.cbc_crypt(&ctx, ciphertext.data(), actual_plaintext.data(), ciphertext.size(), iv_copy.data());
    EXPECT_EQ(plaintext, actual_plaintext);
  }
  {
    constexpr std::size_t key_bits = AES256_KEY_BITS;
    const std::array<std::uint8_t, 32> key = {{0x60, 0x3D, 0xEB, 0x10, 0x15, 0xCA, 0x71, 0xBE, 0x2B, 0x73, 0xAE, 0xF0, 0x85, 0x7D, 0x77, 0x81,
                                               0x1F, 0x35, 0x2C, 0x07, 0x3B, 0x61, 0x08, 0xD7, 0x2D, 0x98, 0x10, 0xA3, 0x09, 0x14, 0xDF, 0xF4}};
    const std::array<std::uint8_t, 16> iv = {{0x39, 0xF2, 0x33, 0x69, 0xA9, 0xD9, 0xBA, 0xCF, 0xA5, 0x30, 0xE2, 0x63, 0x04, 0x23, 0x14, 0x61}};
    const std::array<std::uint8_t, 16> plaintext = {{0xF6, 0x9F, 0x24, 0x45, 0xDF, 0x4F, 0x9B, 0x17, 0xAD, 0x2B, 0x41, 0x7B, 0xE6, 0x6C, 0x37, 0x10}};
    const std::array<std::uint8_t, 16> ciphertext = {{0xB2, 0xEB, 0x05, 0xE2, 0xC3, 0x9B, 0xE9, 0xFC, 0xDA, 0x6C, 0x19, 0x07, 0x8C, 0x6A, 0x9D, 0x1B}};

    std::remove_const<decltype(ciphertext)>::type actual_ciphertext;
    auto iv_copy = iv;
    vk_aes_ctx_t ctx;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.cbc_crypt(&ctx, plaintext.data(), actual_ciphertext.data(), plaintext.size(), iv_copy.data());
    EXPECT_EQ(ciphertext, actual_ciphertext);

    std::remove_const<decltype(ciphertext)>::type actual_plaintext;
    iv_copy = iv;
    vk_aes_set_decrypt_key(&ctx, key.data(), key_bits);
    ctx.cbc_crypt(&ctx, ciphertext.data(), actual_plaintext.data(), ciphertext.size(), iv_copy.data());
    EXPECT_EQ(plaintext, actual_plaintext);
  }
}

using AES256_ige_random = AESCtx<1024, 32, 32>;
TEST_F(AES256_ige_random, random_ige) {
  vk_aes_ctx_t ctx;
  vk_aes_set_encrypt_key(&ctx, random_key_.data(), aes_ctx_key_bits);

  std::array<std::uint8_t, aes_ctx_plaintext_bytes> ciphertext;
  ctx.ige_crypt(&ctx, random_payload_.data(), ciphertext.data(), aes_ctx_plaintext_bytes, random_iv_.data());

  vk_aes_set_decrypt_key(&ctx, random_key_.data(), aes_ctx_key_bits);
  std::array<std::uint8_t, aes_ctx_plaintext_bytes> plaintext;
  ctx.ige_crypt(&ctx, ciphertext.data(), plaintext.data(), aes_ctx_plaintext_bytes, random_iv_initial_.data());

  EXPECT_EQ(plaintext, random_payload_);
}

TEST(AES256_ige, test_vector) {
  {
    constexpr std::size_t key_bits = AES256_KEY_BITS;
    const std::array<std::uint8_t, 32> key = {{0x58, 0x0a, 0x06, 0xe9, 0x97, 0x07, 0x59, 0x5c, 0x9e, 0x19, 0xd2, 0xa7, 0xbb, 0x40, 0x2b, 0x7a,
                                               0xc7, 0xd8, 0x11, 0x9e, 0x4c, 0x51, 0x35, 0x75, 0x64, 0x28, 0x0f, 0x23, 0xad, 0x74, 0xac, 0x37}};
    const std::array<std::uint8_t, 64> iv = {{0x80, 0x3d, 0xbd, 0x4c, 0xe6, 0x7b, 0x06, 0xa9, 0x53, 0x35, 0xd5, 0x7e, 0x71, 0xc1, 0x70, 0x70,
                                              0x74, 0x9a, 0x00, 0x28, 0x0c, 0xbf, 0x6c, 0x42, 0x9b, 0xa4, 0xdd, 0x65, 0x11, 0x77, 0x7c, 0x67,
                                              0xfe, 0x76, 0x0a, 0xf0, 0xd5, 0xc6, 0x6e, 0x6a, 0xe7, 0x5e, 0x4c, 0xf2, 0x7e, 0x9e, 0xf9, 0x20,
                                              0x0e, 0x54, 0x6f, 0x2d, 0x8a, 0x8d, 0x7e, 0xbd, 0x48, 0x79, 0x37, 0x99, 0xff, 0x27, 0x93, 0xa3}};
    const std::array<std::uint8_t, 64> plaintext = {{0xf1, 0x54, 0x3d, 0xca, 0xfe, 0xb5, 0xef, 0x1c, 0x4f, 0xa6, 0x43, 0xf6, 0xe6, 0x48, 0x57, 0xf0,
                                                     0xee, 0x15, 0x7f, 0xe3, 0xe7, 0x2f, 0xd0, 0x2f, 0x11, 0x95, 0x7a, 0x17, 0x00, 0xab, 0xa7, 0x0b,
                                                     0xbe, 0x44, 0x09, 0x9c, 0xcd, 0xac, 0xa8, 0x52, 0xa1, 0x8e, 0x7b, 0x75, 0xbc, 0xa4, 0x92, 0x5a,
                                                     0xab, 0x46, 0xd3, 0x3a, 0xa0, 0xd5, 0x35, 0x1c, 0x55, 0xa4, 0xb3, 0xa8, 0x40, 0x81, 0xa5, 0x0b}};
    const std::array<std::uint8_t, 64> ciphertext = {{0xaf, 0x30, 0x1a, 0x5a, 0x18, 0x3f, 0x13, 0x18, 0x43, 0xe,  0xac, 0xe3, 0x47, 0x88, 0xc,  0xcd,
                                                      0xd0, 0xcd, 0x2e, 0x81, 0x8,  0x2d, 0x9e, 0x6c, 0x96, 0xed, 0x12, 0x81, 0x13, 0x85, 0x5e, 0x16,
                                                      0xa2, 0x6f, 0x7d, 0x2,  0xab, 0xa2, 0xac, 0x90, 0x60, 0x20, 0x17, 0x58, 0x9d, 0x32, 0xf3, 0x2f,
                                                      0x35, 0x49, 0x3f, 0xca, 0x7b, 0x76, 0xb,  0xce, 0x84, 0x54, 0xa3, 0xf1, 0xec, 0xed, 0xef, 0xab}};

    std::remove_const<decltype(ciphertext)>::type actual_ciphertext;
    auto iv_copy = iv;
    vk_aes_ctx_t ctx;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.ige_crypt(&ctx, plaintext.data(), actual_ciphertext.data(), plaintext.size(), iv_copy.data());
    EXPECT_EQ(ciphertext, actual_ciphertext);

    std::remove_const<decltype(ciphertext)>::type actual_plaintext;
    iv_copy = iv;
    vk_aes_set_decrypt_key(&ctx, key.data(), key_bits);
    ctx.ige_crypt(&ctx, ciphertext.data(), actual_plaintext.data(), ciphertext.size(), iv_copy.data());
    EXPECT_EQ(plaintext, actual_plaintext);
  }
}

using AES256_ctr_random = AESCtx<1024, 16, 32>;
TEST_F(AES256_ctr_random, random_ctr) {
  vk_aes_ctx_t ctx;
  vk_aes_set_encrypt_key(&ctx, random_key_.data(), aes_ctx_key_bits);

  std::array<std::uint8_t, aes_ctx_plaintext_bytes> ciphertext;
  ctx.ctr_crypt(&ctx, random_payload_.data(), ciphertext.data(), aes_ctx_plaintext_bytes, random_iv_.data(), 0);

  vk_aes_set_encrypt_key(&ctx, random_key_.data(), aes_ctx_key_bits);
  std::array<std::uint8_t, aes_ctx_plaintext_bytes> plaintext;
  ctx.ctr_crypt(&ctx, ciphertext.data(), plaintext.data(), aes_ctx_plaintext_bytes, random_iv_initial_.data(), 0);

  EXPECT_EQ(plaintext, random_payload_);
}

TEST(AES256_ctr, test_vector) {
  {
    constexpr std::size_t key_bits = AES256_KEY_BITS;
    const std::array<std::uint8_t, 32> key = {{0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
                                               0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4}};
    const std::array<std::uint8_t, 16> iv = {{0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff}};
    const std::array<std::uint8_t, 16> plaintext = {{0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a}};
    const std::array<std::uint8_t, 16> ciphertext = {{0x60, 0x1e, 0xc3, 0x13, 0x77, 0x57, 0x89, 0xa5, 0xb7, 0xa7, 0xf5, 0x04, 0xbb, 0xf3, 0xd2, 0x28}};
    constexpr uint64_t counter = 0;

    std::remove_const<decltype(ciphertext)>::type actual_ciphertext;
    auto iv_copy = iv;
    vk_aes_ctx_t ctx;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.ctr_crypt(&ctx, plaintext.data(), actual_ciphertext.data(), plaintext.size(), iv_copy.data(), counter);
    EXPECT_EQ(ciphertext, actual_ciphertext);

    std::remove_const<decltype(ciphertext)>::type actual_plaintext;
    iv_copy = iv;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.ctr_crypt(&ctx, ciphertext.data(), actual_plaintext.data(), ciphertext.size(), iv_copy.data(), counter);
    EXPECT_EQ(plaintext, actual_plaintext);
  }
  {
    constexpr std::size_t key_bits = AES256_KEY_BITS;
    const std::array<std::uint8_t, 32> key = {{0x77, 0x6B, 0xEF, 0xF2, 0x85, 0x1D, 0xB0, 0x6F, 0x4C, 0x8A, 0x05, 0x42, 0xC8, 0x69, 0x6F, 0x6C,
                                               0x6A, 0x81, 0xAF, 0x1E, 0xEC, 0x96, 0xB4, 0xD3, 0x7F, 0xC1, 0xD6, 0x89, 0xE6, 0xC1, 0xC1, 0x04}};
    const std::array<std::uint8_t, 16> iv = {{0x00, 0x00, 0x00, 0x60, 0xDB, 0x56, 0x72, 0xC9, 0x7A, 0xA8, 0xF0, 0xB2, 0x00, 0x00, 0x00, 0x01}};
    const std::array<std::uint8_t, 16> plaintext = {{0x53, 0x69, 0x6E, 0x67, 0x6C, 0x65, 0x20, 0x62, 0x6C, 0x6F, 0x63, 0x6B, 0x20, 0x6D, 0x73, 0x67}};
    const std::array<std::uint8_t, 16> ciphertext = {{0x14, 0x5A, 0xD0, 0x1D, 0xBF, 0x82, 0x4E, 0xC7, 0x56, 0x08, 0x63, 0xDC, 0x71, 0xE3, 0xE0, 0xC0}};
    constexpr uint64_t counter = 0;

    std::remove_const<decltype(ciphertext)>::type actual_ciphertext;
    auto iv_copy = iv;
    vk_aes_ctx_t ctx;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.ctr_crypt(&ctx, plaintext.data(), actual_ciphertext.data(), plaintext.size(), iv_copy.data(), counter);
    EXPECT_EQ(ciphertext, actual_ciphertext);

    std::remove_const<decltype(ciphertext)>::type actual_plaintext;
    iv_copy = iv;
    vk_aes_set_encrypt_key(&ctx, key.data(), key_bits);
    ctx.ctr_crypt(&ctx, ciphertext.data(), actual_plaintext.data(), ciphertext.size(), iv_copy.data(), counter);
    EXPECT_EQ(plaintext, actual_plaintext);
  }
}

#ifdef __x86_64__

TEST(crypto_x86_64_aesni256_set_encrypt_key, basic) {
  if (crypto_x86_64_has_aesni_extension()) {
    std::array<std::uint8_t, 32> key;
    std::iota(key.begin(), key.end(), 0);

    std::array<std::uint8_t, 240> expected_key = {
      {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
       0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0xa5, 0x73, 0xc2, 0x9f, 0xa1, 0x76, 0xc4, 0x98, 0xa9, 0x7f, 0xce, 0x93, 0xa5, 0x72, 0xc0, 0x9c,
       0x16, 0x51, 0xa8, 0xcd, 0x02, 0x44, 0xbe, 0xda, 0x1a, 0x5d, 0xa4, 0xc1, 0x06, 0x40, 0xba, 0xde, 0xae, 0x87, 0xdf, 0xf0, 0x0f, 0xf1, 0x1b, 0x68,
       0xa6, 0x8e, 0xd5, 0xfb, 0x03, 0xfc, 0x15, 0x67, 0x6d, 0xe1, 0xf1, 0x48, 0x6f, 0xa5, 0x4f, 0x92, 0x75, 0xf8, 0xeb, 0x53, 0x73, 0xb8, 0x51, 0x8d,
       0xc6, 0x56, 0x82, 0x7f, 0xc9, 0xa7, 0x99, 0x17, 0x6f, 0x29, 0x4c, 0xec, 0x6c, 0xd5, 0x59, 0x8b, 0x3d, 0xe2, 0x3a, 0x75, 0x52, 0x47, 0x75, 0xe7,
       0x27, 0xbf, 0x9e, 0xb4, 0x54, 0x07, 0xcf, 0x39, 0x0b, 0xdc, 0x90, 0x5f, 0xc2, 0x7b, 0x09, 0x48, 0xad, 0x52, 0x45, 0xa4, 0xc1, 0x87, 0x1c, 0x2f,
       0x45, 0xf5, 0xa6, 0x60, 0x17, 0xb2, 0xd3, 0x87, 0x30, 0x0d, 0x4d, 0x33, 0x64, 0x0a, 0x82, 0x0a, 0x7c, 0xcf, 0xf7, 0x1c, 0xbe, 0xb4, 0xfe, 0x54,
       0x13, 0xe6, 0xbb, 0xf0, 0xd2, 0x61, 0xa7, 0xdf, 0xf0, 0x1a, 0xfa, 0xfe, 0xe7, 0xa8, 0x29, 0x79, 0xd7, 0xa5, 0x64, 0x4a, 0xb3, 0xaf, 0xe6, 0x40,
       0x25, 0x41, 0xfe, 0x71, 0x9b, 0xf5, 0x00, 0x25, 0x88, 0x13, 0xbb, 0xd5, 0x5a, 0x72, 0x1c, 0x0a, 0x4e, 0x5a, 0x66, 0x99, 0xa9, 0xf2, 0x4f, 0xe0,
       0x7e, 0x57, 0x2b, 0xaa, 0xcd, 0xf8, 0xcd, 0xea, 0x24, 0xfc, 0x79, 0xcc, 0xbf, 0x09, 0x79, 0xe9, 0x37, 0x1a, 0xc2, 0x3c, 0x6d, 0x68, 0xde, 0x36
    }};
    vk_aes_ctx_t ctx;
    crypto_x86_64_aesni256_set_encrypt_key(&ctx, key.data());
    EXPECT_FALSE(std::memcmp(expected_key.data(), align16(&ctx.u.ctx.a), expected_key.size()));
  }
}

TEST(crypto_x86_64_aesni256_set_decrypt_key, basic) {
  if (crypto_x86_64_has_aesni_extension()) {
    std::array<std::uint8_t, 32> key;
    std::iota(key.begin(), key.end(), 0);

    std::array<std::uint8_t, 240> expected_key = {
      {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x1a, 0x1f, 0x18, 0x1d, 0x1e, 0x1b, 0x1c, 0x19,
       0x12, 0x17, 0x10, 0x15, 0x16, 0x13, 0x14, 0x11, 0x2a, 0x28, 0x40, 0xc9, 0x24, 0x23, 0x4c, 0xc0, 0x26, 0x24, 0x4c, 0xc5, 0x20, 0x27, 0x48, 0xc4,
       0x7f, 0xd7, 0x85, 0x0f, 0x61, 0xcc, 0x99, 0x16, 0x73, 0xdb, 0x89, 0x03, 0x65, 0xc8, 0x9d, 0x12, 0x15, 0xc6, 0x68, 0xbd, 0x31, 0xe5, 0x24, 0x7d,
       0x17, 0xc1, 0x68, 0xb8, 0x37, 0xe6, 0x20, 0x7c, 0xae, 0xd5, 0x58, 0x16, 0xcf, 0x19, 0xc1, 0x00, 0xbc, 0xc2, 0x48, 0x03, 0xd9, 0x0a, 0xd5, 0x11,
       0xde, 0x69, 0x40, 0x9a, 0xef, 0x8c, 0x64, 0xe7, 0xf8, 0x4d, 0x0c, 0x5f, 0xcf, 0xab, 0x2c, 0x23, 0xf8, 0x5f, 0xc4, 0xf3, 0x37, 0x46, 0x05, 0xf3,
       0x8b, 0x84, 0x4d, 0xf0, 0x52, 0x8e, 0x98, 0xe1, 0x3c, 0xa6, 0x97, 0x15, 0xd3, 0x2a, 0xf3, 0xf2, 0x2b, 0x67, 0xff, 0xad, 0xe4, 0xcc, 0xd3, 0x8e,
       0x74, 0xda, 0x7b, 0xa3, 0x43, 0x9c, 0x7e, 0x50, 0xc8, 0x18, 0x33, 0xa0, 0x9a, 0x96, 0xab, 0x41, 0xb5, 0x70, 0x8e, 0x13, 0x66, 0x5a, 0x7d, 0xe1,
       0x4d, 0x3d, 0x82, 0x4c, 0xa9, 0xf1, 0x51, 0xc2, 0xc8, 0xa3, 0x05, 0x80, 0x8b, 0x3f, 0x7b, 0xd0, 0x43, 0x27, 0x48, 0x70, 0xd9, 0xb1, 0xe3, 0x31,
       0x5e, 0x16, 0x48, 0xeb, 0x38, 0x4c, 0x35, 0x0a, 0x75, 0x71, 0xb7, 0x46, 0xdc, 0x80, 0xe6, 0x84, 0x34, 0xf1, 0xd1, 0xff, 0xbf, 0xce, 0xaa, 0x2f,
       0xfc, 0xe9, 0xe2, 0x5f, 0x25, 0x58, 0x01, 0x6e, 0x24, 0xfc, 0x79, 0xcc, 0xbf, 0x09, 0x79, 0xe9, 0x37, 0x1a, 0xc2, 0x3c, 0x6d, 0x68, 0xde, 0x36,
    }};
    vk_aes_ctx_t ctx;
    crypto_x86_64_aesni256_set_decrypt_key(&ctx, key.data());
    EXPECT_FALSE(std::memcmp(expected_key.data(), align16(&ctx.u.ctx.a), expected_key.size()));
  }
}

#endif // __x86_64__

#ifdef __aarch64__

TEST(crypto_aarch64_aes256_set_encrypt_key, basic) {
  if (crypto_aarch64_has_aes_extension()) {
    std::array<std::uint8_t, 32> key;
    std::iota(key.begin(), key.end(), 0);

    std::array<std::uint8_t, 240> expected_key = {
      {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
       0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0xa5, 0x73, 0xc2, 0x9f, 0xa1, 0x76, 0xc4, 0x98, 0xa9, 0x7f, 0xce, 0x93, 0xa5, 0x72, 0xc0, 0x9c,
       0x16, 0x51, 0xa8, 0xcd, 0x02, 0x44, 0xbe, 0xda, 0x1a, 0x5d, 0xa4, 0xc1, 0x06, 0x40, 0xba, 0xde, 0xae, 0x87, 0xdf, 0xf0, 0x0f, 0xf1, 0x1b, 0x68,
       0xa6, 0x8e, 0xd5, 0xfb, 0x03, 0xfc, 0x15, 0x67, 0x6d, 0xe1, 0xf1, 0x48, 0x6f, 0xa5, 0x4f, 0x92, 0x75, 0xf8, 0xeb, 0x53, 0x73, 0xb8, 0x51, 0x8d,
       0xc6, 0x56, 0x82, 0x7f, 0xc9, 0xa7, 0x99, 0x17, 0x6f, 0x29, 0x4c, 0xec, 0x6c, 0xd5, 0x59, 0x8b, 0x3d, 0xe2, 0x3a, 0x75, 0x52, 0x47, 0x75, 0xe7,
       0x27, 0xbf, 0x9e, 0xb4, 0x54, 0x07, 0xcf, 0x39, 0x0b, 0xdc, 0x90, 0x5f, 0xc2, 0x7b, 0x09, 0x48, 0xad, 0x52, 0x45, 0xa4, 0xc1, 0x87, 0x1c, 0x2f,
       0x45, 0xf5, 0xa6, 0x60, 0x17, 0xb2, 0xd3, 0x87, 0x30, 0x0d, 0x4d, 0x33, 0x64, 0x0a, 0x82, 0x0a, 0x7c, 0xcf, 0xf7, 0x1c, 0xbe, 0xb4, 0xfe, 0x54,
       0x13, 0xe6, 0xbb, 0xf0, 0xd2, 0x61, 0xa7, 0xdf, 0xf0, 0x1a, 0xfa, 0xfe, 0xe7, 0xa8, 0x29, 0x79, 0xd7, 0xa5, 0x64, 0x4a, 0xb3, 0xaf, 0xe6, 0x40,
       0x25, 0x41, 0xfe, 0x71, 0x9b, 0xf5, 0x00, 0x25, 0x88, 0x13, 0xbb, 0xd5, 0x5a, 0x72, 0x1c, 0x0a, 0x4e, 0x5a, 0x66, 0x99, 0xa9, 0xf2, 0x4f, 0xe0,
       0x7e, 0x57, 0x2b, 0xaa, 0xcd, 0xf8, 0xcd, 0xea, 0x24, 0xfc, 0x79, 0xcc, 0xbf, 0x09, 0x79, 0xe9, 0x37, 0x1a, 0xc2, 0x3c, 0x6d, 0x68, 0xde, 0x36,
    }};
    vk_aes_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    crypto_aarch64_aes256_set_encrypt_key(&ctx, key.data());

    EXPECT_FALSE(std::memcmp(expected_key.data(), align16(&ctx.u.ctx.a), expected_key.size()));
  }
}

TEST(crypto_aarch64_aes256_set_decrypt_key, basic) {
  if (crypto_aarch64_has_aes_extension()) {
    std::array<std::uint8_t, 32> key;
    std::iota(key.begin(), key.end(), 0);

    std::array<std::uint8_t, 240> expected_key = {
      {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x1a, 0x1f, 0x18, 0x1d, 0x1e, 0x1b, 0x1c, 0x19,
       0x12, 0x17, 0x10, 0x15, 0x16, 0x13, 0x14, 0x11, 0x2a, 0x28, 0x40, 0xc9, 0x24, 0x23, 0x4c, 0xc0, 0x26, 0x24, 0x4c, 0xc5, 0x20, 0x27, 0x48, 0xc4,
       0x7f, 0xd7, 0x85, 0x0f, 0x61, 0xcc, 0x99, 0x16, 0x73, 0xdb, 0x89, 0x03, 0x65, 0xc8, 0x9d, 0x12, 0x15, 0xc6, 0x68, 0xbd, 0x31, 0xe5, 0x24, 0x7d,
       0x17, 0xc1, 0x68, 0xb8, 0x37, 0xe6, 0x20, 0x7c, 0xae, 0xd5, 0x58, 0x16, 0xcf, 0x19, 0xc1, 0x00, 0xbc, 0xc2, 0x48, 0x03, 0xd9, 0x0a, 0xd5, 0x11,
       0xde, 0x69, 0x40, 0x9a, 0xef, 0x8c, 0x64, 0xe7, 0xf8, 0x4d, 0x0c, 0x5f, 0xcf, 0xab, 0x2c, 0x23, 0xf8, 0x5f, 0xc4, 0xf3, 0x37, 0x46, 0x05, 0xf3,
       0x8b, 0x84, 0x4d, 0xf0, 0x52, 0x8e, 0x98, 0xe1, 0x3c, 0xa6, 0x97, 0x15, 0xd3, 0x2a, 0xf3, 0xf2, 0x2b, 0x67, 0xff, 0xad, 0xe4, 0xcc, 0xd3, 0x8e,
       0x74, 0xda, 0x7b, 0xa3, 0x43, 0x9c, 0x7e, 0x50, 0xc8, 0x18, 0x33, 0xa0, 0x9a, 0x96, 0xab, 0x41, 0xb5, 0x70, 0x8e, 0x13, 0x66, 0x5a, 0x7d, 0xe1,
       0x4d, 0x3d, 0x82, 0x4c, 0xa9, 0xf1, 0x51, 0xc2, 0xc8, 0xa3, 0x05, 0x80, 0x8b, 0x3f, 0x7b, 0xd0, 0x43, 0x27, 0x48, 0x70, 0xd9, 0xb1, 0xe3, 0x31,
       0x5e, 0x16, 0x48, 0xeb, 0x38, 0x4c, 0x35, 0x0a, 0x75, 0x71, 0xb7, 0x46, 0xdc, 0x80, 0xe6, 0x84, 0x34, 0xf1, 0xd1, 0xff, 0xbf, 0xce, 0xaa, 0x2f,
       0xfc, 0xe9, 0xe2, 0x5f, 0x25, 0x58, 0x01, 0x6e, 0x24, 0xfc, 0x79, 0xcc, 0xbf, 0x09, 0x79, 0xe9, 0x37, 0x1a, 0xc2, 0x3c, 0x6d, 0x68, 0xde, 0x36 }};
    vk_aes_ctx_t ctx;
    crypto_aarch64_aes256_set_decrypt_key(&ctx, key.data());
    EXPECT_FALSE(std::memcmp(expected_key.data(), align16(&ctx.u.ctx.a), expected_key.size()));
  }
}

#endif // __aarch64__
