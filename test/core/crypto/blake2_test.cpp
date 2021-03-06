/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include <stdio.h>

#include "crypto/blake2/blake2b160.hpp"
#include "testutil/literals.hpp"

// Deterministic sequences (Fibonacci generator).

static void selftest_seq(uint8_t *out, size_t len, size_t seed) {
  size_t i;
  uint32_t t, a, b;

  a = static_cast<uint32_t>(0xDEAD4BAD * seed);  // prime
  b = 1;

  for (i = 0; i < len; i++) {  // fill the buf
    t = a + b;
    a = b;
    b = t;
    out[i] = static_cast<uint8_t>((t >> 24) & 0xFF);
  }
}

TEST(Blake2b, Correctness) {
  // grand hash of hash results
  auto blake2b_res =
      "C23A7800D98123BD10F506C61E29DA5603D763B8BBAD2E737F5E765A7BCCD475"_unhex;
  // parameter sets
  const ssize_t b2b_md_len[4] = {20, 32, 48, 64};
  const ssize_t b2b_in_len[6] = {0, 3, 128, 129, 255, 1024};

  size_t i, j;
  uint8_t in[1024], md[64], key[64];
  fc::crypto::blake2b::Ctx ctx(32);

  for (i = 0; i < 4; i++) {
    auto outlen{b2b_md_len[i]};
    for (j = 0; j < 6; j++) {
      auto inlen{b2b_in_len[j]};

      selftest_seq(in, inlen, inlen);  // unkeyed hash
      fc::crypto::blake2b::hashn({md, outlen}, {in, inlen}, {});
      ctx.update({md, outlen});  // hash the hash

      selftest_seq(key, outlen, outlen);  // keyed hash
      fc::crypto::blake2b::hashn({md, outlen}, {in, inlen}, {key, outlen});
      ctx.update({md, outlen});  // hash the hash
    }
  }

  // compute and compare the hash of hashes
  ctx.final(md);

  EXPECT_EQ(memcmp(md, blake2b_res.data(), 32), 0) << "hashes are different";
}
