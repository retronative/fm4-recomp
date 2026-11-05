# D3D float16 unpack (types 3 and 5) and float16_2 pack (type 3) (patches/xenonrecomp/0011).
# Format mapping from Xenia (REFERENCE_DERIVED): FLOAT16_2 is guest word 3,
# FLOAT16_4 guest words 2-3, high half first. Unpack values: IEEE binary16 (Python struct 'e').
# Pack values: a model of upstream's float16 rules (truncate; NaN and magnitudes above 65504 give
# 0x7FFF, with the sign). Encoded: vupkd3d128 (6 << 26) | vD << 21 | UIMM << 16 | vB << 11 | 0x7F0;
# vpkd3d128 (6 << 26) | vD << 21 | type << 18 | kind << 16 | vB << 11 | position << 6 | 0x610.

# vupkd3d128 v3, v1, 20 (type 5): guest words 2-3 hold halves 1.0, -2.0, 2^-24 (smallest subnormal), 65504
test_vupkd3d128_float16_4:
  #_ REGISTER_IN v1 [11111111, 22222222, 3C00C000, 00017BFF]
  .long 0x18740FF0
  blr
  #_ REGISTER_OUT v3 [3F800000, C0000000, 33800000, 477FE000]

# vupkd3d128 v1, v1, 20: -0, the largest subnormal, 0x3555, and 0x7C00 (infinity: pins the reference-derived exponent-31 choice, not hardware); vD = vB
test_vupkd3d128_float16_4_alias:
  #_ REGISTER_IN v1 [33333333, 44444444, 800003FF, 35557C00]
  .long 0x18340FF0
  blr
  #_ REGISTER_OUT v1 [80000000, 387FC000, 3EAAA000, 7F800000]

# vupkd3d128 v3, v1, 12 (type 3): guest word 3 holds 0.5 and -1.0; result (0.5, -1.0, 0, 1.0)
test_vupkd3d128_float16_2:
  #_ REGISTER_IN v1 [55555555, 66666666, 77777777, 3800BC00]
  .long 0x186C0FF0
  blr
  #_ REGISTER_OUT v3 [3F000000, BF800000, 00000000, 3F800000]

# vpkd3d128 v3, v1, 3, 1, 3 (type 3, kind 1, position 3): (x, y) = (1.0, 0x3F801FFF) to guest element 0; y truncates to 0x3C00 (round-to-nearest would give 0x3C01); the other elements keep vD
test_vpkd3d128_float16_2:
  #_ REGISTER_IN v1 [3F800000, 3F801FFF, 40E00000, 41100000]
  #_ REGISTER_IN v3 [AAAAAAAA, BBBBBBBB, CCCCCCCC, DDDDDDDD]
  .long 0x186D0ED0
  blr
  #_ REGISTER_OUT v3 [3C003C00, BBBBBBBB, CCCCCCCC, DDDDDDDD]

# vpkd3d128 v3, v1, 3, 1, 3: -70000 saturates to 0xFFFF (0x7FFF with the sign), NaN to 0x7FFF
test_vpkd3d128_float16_2_saturate:
  #_ REGISTER_IN v1 [C788B800, 7FC00000, 00000000, 00000000]
  #_ REGISTER_IN v3 [AAAAAAAA, BBBBBBBB, CCCCCCCC, DDDDDDDD]
  .long 0x186D0ED0
  blr
  #_ REGISTER_OUT v3 [FFFF7FFF, BBBBBBBB, CCCCCCCC, DDDDDDDD]
