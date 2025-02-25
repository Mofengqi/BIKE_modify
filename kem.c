/*
 * Copyright 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 * http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 * The license is detailed in the file LICENSE.md, and applies to this file.
 *
 * Written by Nir Drucker, Shay Gueron, and Dusan Kostic,
 * AWS Cryptographic Algorithms Group.
 * (ndrucker@amazon.com, gueron@amazon.com, dkostic@amazon.com)
 */

#include "kem.h"
#include "decode.h"
#include "gf2x.h"
#include "sampling.h"
#include "sha.h"

_INLINE_ void
split_e(OUT split_e_t *splitted_e, IN const e_t *e)
{
  // Copy lower bytes (e0)
  memcpy(splitted_e->val[0].raw, e->raw, R_SIZE);

  // Now load second value
  for(uint32_t i = R_SIZE; i < N_SIZE; ++i)
  {
    splitted_e->val[1].raw[i - R_SIZE] =
        ((e->raw[i] << LAST_R_BYTE_TRAIL) | (e->raw[i - 1] >> LAST_R_BYTE_LEAD));
  }

  // Fix corner case
  if(N_SIZE < (2ULL * R_SIZE))
  {
    splitted_e->val[1].raw[R_SIZE - 1] = (e->raw[N_SIZE - 1] >> LAST_R_BYTE_LEAD);
  }

  // Fix last value
  splitted_e->val[0].raw[R_SIZE - 1] &= LAST_R_BYTE_MASK;
  splitted_e->val[1].raw[R_SIZE - 1] &= LAST_R_BYTE_MASK;
}

_INLINE_ void
translate_hash_to_ss(OUT ss_t *ss, IN sha_hash_t *hash)
{
  bike_static_assert(sizeof(*hash) >= sizeof(*ss), hash_size_lt_ss_size);
  memcpy(ss->raw, hash->u.raw, sizeof(*ss));
}

_INLINE_ void
translate_hash_to_seed(OUT seed_t *seed, IN sha_hash_t *hash)
{
  bike_static_assert(sizeof(*hash) >= sizeof(*seed), hash_size_lt_seed_size);
  memcpy(seed->raw, hash->u.raw, sizeof(*seed));
}

_INLINE_ ret_t
calc_pk(OUT pk_t *pk, IN const seed_t *g_seed, IN const pad_sk_t p_sk)
{
  // PK is dbl padded because modmul require some scratch space for the
  // multiplication result
  dbl_pad_pk_t p_pk = {0};

  // Intialized padding to zero
  DEFER_CLEANUP(padded_r_t g = {0}, padded_r_cleanup);

  // sample g make sure g is odd weight
  // ----> g 采样位置 <----
  GUARD(sample_uniform_r_bits(&g.val, g_seed, MUST_BE_ODD));

  // Calculate (f0, f1) = (g*h1, g*h0)
  // 多项式运算在 gf2x 文件中被定义
  GUARD(gf2x_mod_mul((uint64_t *)&p_pk[0], (const uint64_t *)&g,
                     (const uint64_t *)&p_sk[1]));
  GUARD(gf2x_mod_mul((uint64_t *)&p_pk[1], (const uint64_t *)&g,
                     (const uint64_t *)&p_sk[0]));

  // Copy the data to the output parameters.
  pk->val[0] = p_pk[0].val;
  pk->val[1] = p_pk[1].val;

  print("\ng:  ", (uint64_t *)g.val.raw, R_BITS);
  print("f0: ", (uint64_t *)&p_pk[0], R_BITS);
  print("f1: ", (uint64_t *)&p_pk[1], R_BITS);

  return SUCCESS;
}

// The function H is required by BIKE-1- Round 2 variant. It uses the
// extract-then-expand paradigm, based on SHA384 and AES256-CTR PRNG, to produce e
// from (m*f0, m*f1):
// BIKE-1- Round 2 变体需要函数 H。 它使用基于 SHA384 和 AES256-CTR PRNG 的
// extract-then-expand 范例从 (m*f0, m*f1) 生成 e：
_INLINE_ ret_t
function_h(OUT split_e_t *splitted_e, IN const r_t *in0, IN const r_t *in1)
{
  DEFER_CLEANUP(generic_param_n_t tmp, generic_param_n_cleanup);
  DEFER_CLEANUP(sha_hash_t hash_seed = {0}, sha_hash_cleanup);
  DEFER_CLEANUP(seed_t seed_for_hash, seed_cleanup);
  DEFER_CLEANUP(aes_ctr_prf_state_t prf_state = {0}, finalize_aes_ctr_prf);

  tmp.val[0] = *in0; // mf0
  tmp.val[1] = *in1; // mf1

  // Hash (m*f0, m*f1) to generate a seed:
  sha(&hash_seed, sizeof(tmp), (uint8_t *)&tmp);

  // Format the seed as a 32-bytes input:
  translate_hash_to_seed(&seed_for_hash, &hash_seed);

  // Use the seed to generate a sparse error vector e:
  DMSG("    Generating random error.\n");
  GUARD(init_aes_ctr_prf_state(&prf_state, MAX_AES_INVOKATION, &seed_for_hash));

  DEFER_CLEANUP(padded_e_t e, padded_e_cleanup);
  DEFER_CLEANUP(compressed_idx_t_t dummy, compressed_idx_t_cleanup);

  // (e0, e1) = H(mf0, mf1) where wt(e0) + wt(e1) = t
  GUARD(generate_sparse_rep((uint64_t *)&e, dummy.val, T1, N_BITS, sizeof(e),
                            &prf_state));

  // 对 e 进行 split 为 e0 和 e1
  split_e(splitted_e, &e.val);

  return SUCCESS;
}

_INLINE_ ret_t
encrypt(OUT ct_t *ct, OUT split_e_t *mf, IN const pk_t *pk, IN const seed_t *seed)
{
  DEFER_CLEANUP(padded_r_t m = {0}, padded_r_cleanup);

  DMSG("    Sampling m.\n");

  // Sampling m
  // ----> m 采样位置 <----
  GUARD(sample_uniform_r_bits(&m.val, seed, NO_RESTRICTION));

  // 输出 m 的值
  print("\nm: ", (uint64_t *)m.val.raw, R_BITS);

  // Pad the public key
  pad_pk_t p_pk = {0};
  p_pk[0].val   = pk->val[0];
  p_pk[1].val   = pk->val[1];

  // Pad the ciphertext
  pad_ct_t p_ct = {0};
  p_ct[0].val   = ct->val[0];
  p_ct[1].val   = ct->val[1];

  DEFER_CLEANUP(dbl_pad_ct_t mf_int = {0}, dbl_pad_ct_cleanup);

  DMSG("    Computing m*f0 and m*f1.\n");
  // 计算 mf0, mf1
  GUARD(
      gf2x_mod_mul((uint64_t *)&mf_int[0], (uint64_t *)&m, (uint64_t *)&p_pk[0]));
  GUARD(
      gf2x_mod_mul((uint64_t *)&mf_int[1], (uint64_t *)&m, (uint64_t *)&p_pk[1]));

  DEFER_CLEANUP(split_e_t splitted_e, split_e_cleanup);

  // split_e_t->val[0] and split_e_t->val[1] include e0 and e1
  // ----> 错误向量 e 获取位置 <----
  DMSG("    Computing the hash function e <- H(m*f0, m*f1).\n");
  GUARD(function_h(&splitted_e, &mf_int[0].val, &mf_int[1].val));

  //  (c0, c1) = (mf0 + e0, mf1 + e1)
  // 多项式加法相当于异或 ^
  // ----> c0, c1 计算位置 <----
  DMSG("    Addding Error to the ciphertext.\n");
  GUARD(gf2x_add(p_ct[0].val.raw, mf_int[0].val.raw, splitted_e.val[0].raw,
                 R_SIZE));
  GUARD(gf2x_add(p_ct[1].val.raw, mf_int[1].val.raw, splitted_e.val[1].raw,
                 R_SIZE));

  // Copy the data to the output parameters.
  ct->val[0] = p_ct[0].val;
  ct->val[1] = p_ct[1].val;

  // Copy the internal mf to the output parameters.
  mf->val[0] = mf_int[0].val;
  mf->val[1] = mf_int[1].val;

  print("e0: ", (uint64_t *)splitted_e.val[0].raw, R_BITS);
  print("e1: ", (uint64_t *)splitted_e.val[1].raw, R_BITS);
  print("c0: ", (uint64_t *)p_ct[0].val.raw, R_BITS);
  print("c1: ", (uint64_t *)p_ct[1].val.raw, R_BITS);

  return SUCCESS;
}

_INLINE_ ret_t
reencrypt(OUT pad_ct_t ce,
          OUT split_e_t *e2,
          IN const split_e_t *e,
          IN const ct_t *l_ct)
{
  // Compute (c0 + e0') and (c1 + e1')
  GUARD(gf2x_add(ce[0].val.raw, l_ct->val[0].raw, e->val[0].raw, R_SIZE));
  GUARD(gf2x_add(ce[1].val.raw, l_ct->val[1].raw, e->val[1].raw, R_SIZE));

  // (e0'', e1'') <-- H(c0 + e0', c1 + e1')
  GUARD(function_h(e2, &ce[0].val, &ce[1].val));

  return SUCCESS;
}

// Generate the Shared Secret K(mf0, mf1, c) by either
// K(c0+e0', c1+e1', c) or K(sigma0, sigma1, c)
_INLINE_ void
get_ss(OUT ss_t *out, IN const r_t *in0, IN const r_t *in1, IN const ct_t *ct)
{
  DMSG("    Enter get_ss.\n");

  uint8_t tmp[4 * R_SIZE];
  memcpy(tmp, in0->raw, R_SIZE);
  memcpy(tmp + R_SIZE, in1->raw, R_SIZE);
  memcpy(tmp + 2 * R_SIZE, ct, sizeof(*ct));

  // Calculate the hash digest
  DEFER_CLEANUP(sha_hash_t hash = {0}, sha_hash_cleanup);
  sha(&hash, sizeof(tmp), tmp);

  // Truncate the resulting digest, to produce the key K, by copying only the
  // desired number of LSBs.
  translate_hash_to_ss(out, &hash);

  secure_clean(tmp, sizeof(tmp));
  DMSG("    Exit get_ss.\n");
}

////////////////////////////////////////////////////////////////////////////////
// The three APIs below (keypair, encapsulate, decapsulate) are defined by NIST:
////////////////////////////////////////////////////////////////////////////////
int
crypto_kem_keypair(OUT unsigned char *pk, OUT unsigned char *sk)
{
  // Convert to this implementation types
  // 数据类型转换
  sk_t *l_sk = (sk_t *)sk;
  pk_t *l_pk = (pk_t *)pk;

  // For DRBG and AES_PRF
  // 预清理
  DEFER_CLEANUP(seeds_t seeds = {0}, seeds_cleanup);
  DEFER_CLEANUP(aes_ctr_prf_state_t h_prf_state = {0}, aes_ctr_prf_state_cleanup);

  // For sigma0/1/2
  DEFER_CLEANUP(aes_ctr_prf_state_t s_prf_state = {0}, aes_ctr_prf_state_cleanup);

  // Padded for internal use only (the padded data is not released).
  DEFER_CLEANUP(pad_sk_t p_sk = {0}, pad_sk_cleanup);

  // Get the entropy seeds.
  // 获取随机数种子
  get_seeds(&seeds);

  DMSG("  Enter crypto_kem_keypair.\n");
  DMSG("    Calculating the secret key.\n");

  // h0 and h1 use the same context
  GUARD(init_aes_ctr_prf_state(&h_prf_state, MAX_AES_INVOKATION, &seeds.seed[0]));

  // sigma0/1/2 use the same context.
  GUARD(init_aes_ctr_prf_state(&s_prf_state, MAX_AES_INVOKATION, &seeds.seed[2]));

  // Make sure that the wlists are zeroed for the KATs.
  // 确认 wlist 使用前被清0
  memset(l_sk, 0, sizeof(sk_t));

  // 获取 wlist[0] 的值
  GUARD(generate_sparse_rep((uint64_t *)&p_sk[0], l_sk->wlist[0].val, DV, R_BITS,
                            sizeof(p_sk[0]), &h_prf_state));

  printf("\nl_sk->wlist[0]的索引值(h0中1的位置): \n");
  for(uint32_t y = 0; y < 71; y++){
    printf("%u\n",(l_sk->wlist[0].val)[y]);
  }

  // Copy data
  l_sk->bin[0] = p_sk[0].val;

  // Sample the sigmas
  // ----> sigmas 采样位置 <----
  GUARD(sample_uniform_r_bits_with_fixed_prf_context(&l_sk->sigma0, &s_prf_state,
                                                     NO_RESTRICTION));
  GUARD(sample_uniform_r_bits_with_fixed_prf_context(&l_sk->sigma1, &s_prf_state,
                                                     NO_RESTRICTION));

  // 获取 wlist[1] 的值
  GUARD(generate_sparse_rep((uint64_t *)&p_sk[1], l_sk->wlist[1].val, DV, R_BITS,
                            sizeof(p_sk[1]), &h_prf_state));

  printf("\nl_sk->wlist[1]的索引值(h1中1的位置): \n");
  for(uint32_t z = 0; z < 71; z++){
    printf("%u\n",(l_sk->wlist[1].val)[z]);
  }

  // Copy data
  l_sk->bin[1] = p_sk[1].val;

  DMSG("    Calculating the public key.\n");
  
  // 计算 pk (f0, f1) = (gh1, gh0)
  // 此函数包含对 g 采样
  GUARD(calc_pk(l_pk, &seeds.seed[1], p_sk));

  print("h0: ", (uint64_t *)&l_sk->bin[0], R_BITS);
  // for(uint16_t i_test = 0; i_test < 100; i_test++){
  //   printf("l_sk->bin[0].raw[%u] 的值为: %u \n\n", i_test, l_sk->bin[0].raw[i_test]);
  // }
  print("h1: ", (uint64_t *)&l_sk->bin[1], R_BITS);
  print("h0c:", (uint64_t *)&l_sk->wlist[0], SIZEOF_BITS(compressed_idx_dv_t));
  print("h1c:", (uint64_t *)&l_sk->wlist[1], SIZEOF_BITS(compressed_idx_dv_t));
  print("sigma0: ", (uint64_t *)l_sk->sigma0.raw, R_BITS);
  print("sigma1: ", (uint64_t *)l_sk->sigma1.raw, R_BITS);

  DMSG("  Exit crypto_kem_keypair.\n");

  return SUCCESS;
}

// Encapsulate - pk is the public key,
//               ct is a key encapsulation message (ciphertext),
//               ss is the shared secret.
int
crypto_kem_enc(OUT unsigned char *     ct,
               OUT unsigned char *     ss,
               IN const unsigned char *pk)
{
  DMSG("  Enter crypto_kem_enc.\n");

  // Convert to the types that are used by this implementation
  const pk_t *l_pk = (const pk_t *)pk;
  ct_t *      l_ct = (ct_t *)ct;
  ss_t *      l_ss = (ss_t *)ss;

  // For NIST DRBG_CTR
  DEFER_CLEANUP(seeds_t seeds = {0}, seeds_cleanup);

  // Get the entropy seeds.
  get_seeds(&seeds);

  DMSG("    Encrypting.\n");
  // In fact, seed[0] should be used.
  // Here, we stay consistent with BIKE's reference code
  // that chooses the seconde seed.
  DEFER_CLEANUP(split_e_t mf, split_e_cleanup);

  // m <- R
  // (e0, e1) = H(mf0, mf1) where wt(e0) + wt(e1) = t
  // (c0, c1) = (mf0 + e0, mf1 + e1)
  // 此函数包含 m 的随机采样
  GUARD(encrypt(l_ct, &mf, l_pk, &seeds.seed[1]));
  
  // 获取共享密钥 k
  DMSG("    Generating shared secret.\n");
  get_ss(l_ss, &mf.val[0], &mf.val[1], l_ct);

  print("ss: ", (uint64_t *)l_ss->raw, SIZEOF_BITS(*l_ss));
  DMSG("  Exit crypto_kem_enc.\n");
  return SUCCESS;
}

// Decapsulate - ct is a key encapsulation message (ciphertext),
//               sk is the private key,
//               ss is the shared secret
int
crypto_kem_dec(OUT unsigned char *     ss,
               IN const unsigned char *ct,
               IN const unsigned char *sk)
{
  DMSG("\n  Enter crypto_kem_dec(译码开始).\n");

  // Convert to the types used by this implementation
  const sk_t *l_sk = (const sk_t *)sk;
  const ct_t *l_ct = (const ct_t *)ct;
  ss_t *      l_ss = (ss_t *)ss;

  // Force zero initialization.
  DEFER_CLEANUP(syndrome_t syndrome = {0}, syndrome_cleanup);
  DEFER_CLEANUP(split_e_t e, split_e_cleanup);

  DMSG("  Computing s.\n");
  // Compute the syndrome s = c0h0 + c1h1
  // 计算初始校验子 s
  GUARD(compute_syndrome(&syndrome, l_ct, l_sk));

  // // -- test --
  // for(uint16_t i_qw = 0; i_qw < 555; i_qw++)
  // {
  //   printf("第 %u 个 syndrome->qw 的值为: %lu\n", i_qw, syndrome.qw[i_qw]);
  // }

  DMSG("  Decoding.\n"); // 使用黑灰译码，IN syndrome, l_ct and l_sk, OUT e
  uint32_t dec_ret = decode(&e, &syndrome, l_ct, l_sk) != SUCCESS ? 0 : 1;

  DEFER_CLEANUP(split_e_t e2, split_e_cleanup);
  DEFER_CLEANUP(pad_ct_t ce, pad_ct_cleanup);

  // 此处将计算 (c0 + e0')=mf0' and (c1 + e1')=mf1'
  // e2 包含使用 mf0' 和 mf1' 通过哈希函数H获得的最新(e0'',e1'')
  // ce 包含 mf0' 和 mf1'
  GUARD(reencrypt(ce, &e2, &e, l_ct));

  // Check if the decoding is successful.
  // Check if the error weight equals T1.
  // Check if (e0', e1') == (e0'', e1'').
  volatile uint32_t success_cond;
  success_cond = dec_ret;
  success_cond &= secure_cmp32(T1, r_bits_vector_weight(&e.val[0]) +
                                       r_bits_vector_weight(&e.val[1]));
  success_cond &= secure_cmp((uint8_t *)&e, (uint8_t *)&e2, sizeof(e));

  ss_t ss_succ = {0};
  ss_t ss_fail = {0};

  get_ss(&ss_succ, &ce[0].val, &ce[1].val, l_ct);
  get_ss(&ss_fail, &l_sk->sigma0, &l_sk->sigma1, l_ct);

  uint8_t mask = ~secure_l32_mask(0, success_cond);
  for(uint32_t i = 0; i < sizeof(*l_ss); i++)
  {
    l_ss->raw[i] = (mask & ss_succ.raw[i]) | (~mask & ss_fail.raw[i]);
  }

  DMSG("  Exit crypto_kem_dec(译码结束).\n");
  return SUCCESS;
}
