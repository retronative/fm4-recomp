# vpkd3d128 with vD = vB (FM4 Recomp; patch 0012). The float16_2 pack writes the upper half of host
# word 3 and then re-reads that word's sign bit, so it must read a copy of vB taken before writing:
# that is why patch 0012 copies the source. Encoding as ForzaRecomp's 0x186D0ED0 (type 3, kind 1,
# position 3) with vB = 3: 0x186D1ED0 (vB field, bits 11-15, = 3; the VMX128 high vB bits 0-1 = 0).
# x = -1.0 -> half 0xBC00 (sign kept), y = 2.0 -> 0x4000 (IEEE binary16: 1.0 = 0x3C00, 2.0 = 0x4000);
# packed into guest element 0 as x:y; the other elements keep vD (= vB) unchanged.

test_vpkd3d128_float16_2_aliased:
  #_ REGISTER_IN v3 [BF800000, 40000000, CCCCCCCC, DDDDDDDD]
  .long 0x186D1ED0
  blr
  #_ REGISTER_OUT v3 [BC004000, 40000000, CCCCCCCC, DDDDDDDD]
