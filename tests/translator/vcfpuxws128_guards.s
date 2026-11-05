# vcfpuxws128 must guard its float -> unsigned conversion (FM4 Recomp; patch 0009). In C++,
# uint32_t(float) is undefined for NaN, negative values and values >= 2^32. ARM64's fcvtzu happens
# to give 0, 0 and 0xFFFFFFFF, so the values alone cannot show a missing guard on this host: the
# EMITS lines check that the guards are in the generated code. Values (vcfpuxws: NaN -> 0, below 0
# saturates to 0, at or above 2^32 saturates to 0xFFFFFFFF, else truncate): NaN, -1.0, 5e9, 1.5.
#! EMITS __builtin_isnan(value) || value <= 0.0f ? 0u
#! EMITS value >= 4294967296.0f ? 0xFFFFFFFFu

# vcfpuxws128 v3, v1, 0 (encoded 0x18600A70, as in vmx_arith_pack_convert.s)
test_vcfpuxws128_guarded:
  #_ REGISTER_IN v1 [7FC00000, BF800000, 4F9502F9, 3FC00000]
  .long 0x18600A70
  blr
  #_ REGISTER_OUT v3 [00000000, 00000000, FFFFFFFF, 00000001]
