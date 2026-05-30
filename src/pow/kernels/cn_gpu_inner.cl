// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT
//
// Conceal CN-GPU inner loop — semantic match for inner_hash_3_avx (cn_slow_hash_intel_avx2.cpp).
// 256-bit AVX state is modeled as (lo, hi) float4/int4 pairs in AVX memory order.
#pragma OPENCL FP_CONTRACT OFF

#define CN_GPU_MEMORY (2 * 1024 * 1024)
#define CN_GPU_ITER 49152
#define CN_GPU_MASK (((CN_GPU_MEMORY - 1) >> 6) << 6)

/* scratchpad_ptr(idx, n) == lpad + (idx & MASK) + n*16 — same as cn_slow_hash.hpp */
inline __global char* scratchpad_ptr(uint idx, uint n, __global int* lpad)
{
  return (__global char*)lpad + (idx & CN_GPU_MASK) + n * 16;
}

inline float4 f4_and(float4 a, int m) { return as_float4(as_int4(a) & (int4)(m)); }
inline float4 f4_or(float4 a, int m) { return as_float4(as_int4(a) | (int4)(m)); }

inline float4 f4_fma_break(float4 x) { return f4_or(f4_and(x, 0xFEFFFFFF), 0x00800000); }

inline float4 f4_cvtepi32_ps(int4 v) { return convert_float4(v); }

inline int4 f4_cvttps_epi32(float4 v) { return convert_int4_rtz(v); }

/* imm[1:0] / imm[5:4]: 0=lo(a) 1=hi(a) 2=lo(b) 3=hi(b) */
inline void f4_permute2f128(float4 a_lo, float4 a_hi, float4 b_lo, float4 b_hi, int imm,
                          float4* r_lo, float4* r_hi)
{
  const int sel_lo = imm & 3;
  const int sel_hi = (imm >> 4) & 3;
  float4 al = a_lo, ah = a_hi, bl = b_lo, bh = b_hi;
  *r_lo = sel_lo == 0 ? al : sel_lo == 1 ? ah : sel_lo == 2 ? bl : bh;
  *r_hi = sel_hi == 0 ? al : sel_hi == 1 ? ah : sel_hi == 2 ? bl : bh;
}

inline void i4_permute2x128(int4 a_lo, int4 a_hi, int4 b_lo, int4 b_hi, int imm, int4* r_lo,
                            int4* r_hi)
{
  const int sel_lo = imm & 3;
  const int sel_hi = (imm >> 4) & 3;
  int4 al = a_lo, ah = a_hi, bl = b_lo, bh = b_hi;
  *r_lo = sel_lo == 0 ? al : sel_lo == 1 ? ah : sel_lo == 2 ? bl : bh;
  *r_hi = sel_hi == 0 ? al : sel_hi == 1 ? ah : sel_hi == 2 ? bl : bh;
}

inline void prep_dv_avx_equiv(__global char* idx, int4* v_lo, int4* v_hi, float4* n_lo,
                              float4* n_hi)
{
  *v_lo = vload4(0, (__global int*)idx);
  *v_hi = vload4(0, (__global int*)(idx + 16));
  *n_lo = f4_cvtepi32_ps(*v_lo);
  *n_hi = f4_cvtepi32_ps(*v_hi);
}

inline void store_v256(__global char* idx, int4 v_lo, int4 v_hi)
{
  vstore4(v_lo, 0, (__global int*)idx);
  vstore4(v_hi, 0, (__global int*)(idx + 16));
}

/* Per-lane double division — matches consensus note for AMDGPU vs x86 divps */
inline float4 f4_div_consensus(float4 n, float4 d)
{
  return (float4)((float)((double)n.s0 / (double)d.s0), (float)((double)n.s1 / (double)d.s1),
                  (float)((double)n.s2 / (double)d.s2), (float)((double)n.s3 / (double)d.s3));
}

inline void sub_round(float4 n0, float4 n1, float4 n2, float4 n3, float4 rnd_c, float4* n,
                      float4* d, float4* c)
{
  float4 nn = n0 * (*c);
  nn = (n1 + (*c)) * (nn * nn);
  nn = f4_fma_break(nn);
  *n = *n + nn;

  float4 dd = n2 * (*c);
  dd = (n3 - (*c)) * (dd * dd);
  dd = f4_fma_break(dd);
  *d = *d + dd;

  *c = *c + rnd_c;
  *c = *c + (float4)(0.734375f);
  float4 r = nn + dd;
  r = f4_and(r, 0x807FFFFF);
  r = f4_or(r, 0x40000000);
  *c = *c + r;
}

inline void round_compute(float4 n0, float4 n1, float4 n2, float4 n3, float4 rnd_c, float4* c,
                          float4* r)
{
  float4 n = (float4)(0.0f);
  float4 d = (float4)(0.0f);
  sub_round(n0, n1, n2, n3, rnd_c, &n, &d, c);
  sub_round(n1, n2, n3, n0, rnd_c, &n, &d, c);
  sub_round(n2, n3, n0, n1, rnd_c, &n, &d, c);
  sub_round(n3, n0, n1, n2, rnd_c, &n, &d, c);
  sub_round(n3, n2, n1, n0, rnd_c, &n, &d, c);
  sub_round(n2, n1, n0, n3, rnd_c, &n, &d, c);
  sub_round(n1, n0, n3, n2, rnd_c, &n, &d, c);
  sub_round(n0, n3, n2, n1, rnd_c, &n, &d, c);
  d = f4_and(d, 0xFF7FFFFF);
  d = f4_or(d, 0x40000000);
  *r = *r + f4_div_consensus(n, d);
}

/* _mm256_bslli_epi128 / _mm256_bsrli_epi128: byte rotate within each 128-bit lane */
inline int4 rotate_i4_lane(int4 r, int rot)
{
  if (rot == 0)
    return r;
  uchar u[16];
  for (int i = 0; i < 16; ++i)
    u[i] = ((uchar*)&r)[i];
  uchar o[16];
  for (int i = 0; i < 16; ++i)
    o[i] = u[(i + rot) & 15];
  int4 res;
  for (int i = 0; i < 16; ++i)
    ((uchar*)&res)[i] = o[i];
  return res;
}

inline void rotate_v256(int4* lo, int4* hi, int rot)
{
  if (rot == 0)
    return;
  *lo = rotate_i4_lane(*lo, rot);
  *hi = rotate_i4_lane(*hi, rot);
}

inline void double_compute(float4 n0_lo, float4 n0_hi, float4 n1_lo, float4 n1_hi, float4 n2_lo,
                           float4 n2_hi, float4 n3_lo, float4 n3_hi, float lcnt, float hcnt,
                           float4 rc_lo, float4 rc_hi, int add, float4* sum_lo, float4* sum_hi,
                           int4* rr_lo, int4* rr_hi)
{
  float4 c_lo = (float4)(lcnt);
  float4 c_hi = (float4)(hcnt);
  float4 r_lo = (float4)(0.0f);
  float4 r_hi = (float4)(0.0f);

  round_compute(n0_lo, n1_lo, n2_lo, n3_lo, rc_lo, &c_lo, &r_lo);
  round_compute(n0_hi, n1_hi, n2_hi, n3_hi, rc_hi, &c_hi, &r_hi);
  round_compute(n0_lo, n1_lo, n2_lo, n3_lo, rc_lo, &c_lo, &r_lo);
  round_compute(n0_hi, n1_hi, n2_hi, n3_hi, rc_hi, &c_hi, &r_hi);
  round_compute(n0_lo, n1_lo, n2_lo, n3_lo, rc_lo, &c_lo, &r_lo);
  round_compute(n0_hi, n1_hi, n2_hi, n3_hi, rc_hi, &c_hi, &r_hi);
  round_compute(n0_lo, n1_lo, n2_lo, n3_lo, rc_lo, &c_lo, &r_lo);
  round_compute(n0_hi, n1_hi, n2_hi, n3_hi, rc_hi, &c_hi, &r_hi);

  r_lo = f4_and(r_lo, 0x807FFFFF);
  r_lo = f4_or(r_lo, 0x40000000);
  r_hi = f4_and(r_hi, 0x807FFFFF);
  r_hi = f4_or(r_hi, 0x40000000);

  if (add)
  {
    *sum_lo = *sum_lo + r_lo;
    *sum_hi = *sum_hi + r_hi;
  }
  else
  {
    *sum_lo = r_lo;
    *sum_hi = r_hi;
  }

  r_lo = r_lo * (float4)(536870880.0f);
  r_hi = r_hi * (float4)(536870880.0f);
  *rr_lo = f4_cvttps_epi32(r_lo);
  *rr_hi = f4_cvttps_epi32(r_hi);
}

inline void double_compute_wrap(int rot, float4 n0_lo, float4 n0_hi, float4 n1_lo, float4 n1_hi,
                                float4 n2_lo, float4 n2_hi, float4 n3_lo, float4 n3_hi, float lcnt,
                                float hcnt, float4 rc_lo, float4 rc_hi, float4* sum_lo,
                                float4* sum_hi, int4* out_lo, int4* out_hi)
{
  const int add = rot & 1;
  int4 rr_lo, rr_hi;
  double_compute(n0_lo, n0_hi, n1_lo, n1_hi, n2_lo, n2_hi, n3_lo, n3_hi, lcnt, hcnt, rc_lo, rc_hi,
                 add, sum_lo, sum_hi, &rr_lo, &rr_hi);
  if (rot != 0)
    rotate_v256(&rr_lo, &rr_hi, rot);
  *out_lo = *out_lo ^ rr_lo;
  *out_hi = *out_hi ^ rr_hi;
}

__kernel void cn_gpu_inner(__global ulong* spads, __global int* lpads, uint job_count, uint max_iter)
{
  const uint gid = get_global_id(0);
  if (gid >= job_count)
    return;

  __global ulong* spad = spads + (size_t)gid * 25;
  __global int* lpad = lpads + (size_t)gid * (CN_GPU_MEMORY / 4);

  uint s = ((__global uint*)spad)[0] >> 8;
  float4 sum0_lo = (float4)(0.0f);
  float4 sum0_hi = (float4)(0.0f);
  const uint iter_limit = max_iter == 0u ? CN_GPU_ITER : min(max_iter, (uint)CN_GPU_ITER);

  __global char* idx0 = scratchpad_ptr(s, 0, lpad);
  __global char* idx2 = scratchpad_ptr(s, 2, lpad);

  for (uint i = 0; i < iter_limit; ++i)
  {
    float4 rc_lo = sum0_lo;
    float4 rc_hi = sum0_hi;

    int4 v01_lo, v01_hi, v23_lo, v23_hi;
    float4 n01_lo, n01_hi, n23_lo, n23_hi;
    prep_dv_avx_equiv(idx0, &v01_lo, &v01_hi, &n01_lo, &n01_hi);
    prep_dv_avx_equiv(idx2, &v23_lo, &v23_hi, &n23_lo, &n23_hi);

    float4 n10_lo, n10_hi, n22_lo, n22_hi, n33_lo, n33_hi;
    f4_permute2f128(n01_lo, n01_hi, n01_lo, n01_hi, 0x01, &n10_lo, &n10_hi);
    f4_permute2f128(n23_lo, n23_hi, n23_lo, n23_hi, 0x00, &n22_lo, &n22_hi);
    f4_permute2f128(n23_lo, n23_hi, n23_lo, n23_hi, 0x11, &n33_lo, &n33_hi);

    int4 out_lo = (int4)(0);
    int4 out_hi = (int4)(0);
    float4 suma_lo = (float4)(0.0f);
    float4 suma_hi = (float4)(0.0f);
    float4 sumb_lo = (float4)(0.0f);
    float4 sumb_hi = (float4)(0.0f);

    double_compute_wrap(0, n01_lo, n01_hi, n10_lo, n10_hi, n22_lo, n22_hi, n33_lo, n33_hi,
                        1.3437500f, 1.4296875f, rc_lo, rc_hi, &suma_lo, &suma_hi, &out_lo, &out_hi);
    double_compute_wrap(1, n01_lo, n01_hi, n22_lo, n22_hi, n33_lo, n33_hi, n10_lo, n10_hi,
                        1.2812500f, 1.3984375f, rc_lo, rc_hi, &suma_lo, &suma_hi, &out_lo, &out_hi);
    double_compute_wrap(2, n01_lo, n01_hi, n33_lo, n33_hi, n10_lo, n10_hi, n22_lo, n22_hi,
                        1.3593750f, 1.3828125f, rc_lo, rc_hi, &sumb_lo, &sumb_hi, &out_lo, &out_hi);
    double_compute_wrap(3, n01_lo, n01_hi, n33_lo, n33_hi, n22_lo, n22_hi, n10_lo, n10_hi,
                        1.3671875f, 1.3046875f, rc_lo, rc_hi, &sumb_lo, &sumb_hi, &out_lo, &out_hi);

    store_v256(idx0, v01_lo ^ out_lo, v01_hi ^ out_hi);
    sum0_lo = suma_lo + sumb_lo;
    sum0_hi = suma_hi + sumb_hi;
    int4 out2_lo = out_lo;
    int4 out2_hi = out_hi;

    float4 n11_lo, n11_hi, n02_lo, n02_hi, n30_lo, n30_hi;
    f4_permute2f128(n01_lo, n01_hi, n01_lo, n01_hi, 0x11, &n11_lo, &n11_hi);
    f4_permute2f128(n01_lo, n01_hi, n23_lo, n23_hi, 0x20, &n02_lo, &n02_hi);
    f4_permute2f128(n01_lo, n01_hi, n23_lo, n23_hi, 0x03, &n30_lo, &n30_hi);

    out_lo = (int4)(0);
    out_hi = (int4)(0);
    suma_lo = (float4)(0.0f);
    suma_hi = (float4)(0.0f);
    sumb_lo = (float4)(0.0f);
    sumb_hi = (float4)(0.0f);

    double_compute_wrap(0, n23_lo, n23_hi, n11_lo, n11_hi, n02_lo, n02_hi, n30_lo, n30_hi,
                        1.4140625f, 1.3203125f, rc_lo, rc_hi, &suma_lo, &suma_hi, &out_lo, &out_hi);
    double_compute_wrap(1, n23_lo, n23_hi, n02_lo, n02_hi, n30_lo, n30_hi, n11_lo, n11_hi,
                        1.2734375f, 1.3515625f, rc_lo, rc_hi, &suma_lo, &suma_hi, &out_lo, &out_hi);
    double_compute_wrap(2, n23_lo, n23_hi, n30_lo, n30_hi, n11_lo, n11_hi, n02_lo, n02_hi,
                        1.2578125f, 1.3359375f, rc_lo, rc_hi, &sumb_lo, &sumb_hi, &out_lo, &out_hi);
    double_compute_wrap(3, n23_lo, n23_hi, n30_lo, n30_hi, n02_lo, n02_hi, n11_lo, n11_hi,
                        1.2890625f, 1.4609375f, rc_lo, rc_hi, &sumb_lo, &sumb_hi, &out_lo, &out_hi);

    store_v256(idx2, v23_lo ^ out_lo, v23_hi ^ out_hi);
    float4 sum1_lo = suma_lo + sumb_lo;
    float4 sum1_hi = suma_hi + sumb_hi;

    out2_lo ^= out_lo;
    out2_hi ^= out_hi;
    int4 out2_x_lo, out2_x_hi;
    i4_permute2x128(out2_lo, out2_hi, out2_lo, out2_hi, 0x41, &out2_x_lo, &out2_x_hi);
    out2_lo ^= out2_x_lo;
    out2_hi ^= out2_x_hi;

    float4 suma_comb_lo, suma_comb_hi, sumb_comb_lo, sumb_comb_hi;
    f4_permute2f128(sum0_lo, sum0_hi, sum1_lo, sum1_hi, 0x30, &suma_comb_lo, &suma_comb_hi);
    f4_permute2f128(sum0_lo, sum0_hi, sum1_lo, sum1_hi, 0x21, &sumb_comb_lo, &sumb_comb_hi);
    sum0_lo = suma_comb_lo + sumb_comb_lo;
    sum0_hi = suma_comb_hi + sumb_comb_hi;

    float4 sum0_sw_lo, sum0_sw_hi;
    f4_permute2f128(sum0_lo, sum0_hi, sum0_lo, sum0_hi, 0x41, &sum0_sw_lo, &sum0_sw_hi);
    sum0_lo += sum0_sw_lo;
    sum0_hi += sum0_sw_hi;

    float4 sum = sum0_lo;
    sum = as_float4(as_int4(sum) & (int4)0x7FFFFFFF);
    int4 v0 = f4_cvttps_epi32(sum * (float4)(16777216.0f));
    v0 ^= out2_lo;

    int4 v1 = (int4)(v0.s3, v0.s2, v0.s1, v0.s0);
    v0 ^= v1;
    v1 = (int4)(v0.s1, v0.s0, v0.s1, v0.s0);
    v0 ^= v1;

    sum /= (float4)(64.0f);
    sum0_lo = sum;
    sum0_hi = sum;

    s = (uint)v0.s0;
    idx0 = scratchpad_ptr(s, 0, lpad);
    idx2 = scratchpad_ptr(s, 2, lpad);
  }
}
