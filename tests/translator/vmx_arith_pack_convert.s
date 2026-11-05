# VMX integer arithmetic, packs and float-to-unsigned conversion (patches/xenonrecomp/0008).
# Expected values from the AltiVec definitions with an integer model (guest element 0 is the
# first REGISTER value). VMX128 forms are encoded by hand (registers below 32) and confirmed by
# the decoder's disassembly comment.

# vaddsbs: signed byte add, saturating to [-128, 127]
test_vaddsbs:
  #_ REGISTER_IN v1 [7F8001FF, 40C0007E, 8110F020, 50B07F80]
  #_ REGISTER_IN v2 [01FF7FFF, 40C00001, 80F01060, 50B0807F]
  vaddsbs v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [7F807FFE, 7F80007F, 8000007F, 7F80FFFF]

# vaddsws: signed word add, saturating
test_vaddsws:
  #_ REGISTER_IN v1 [7FFFFFFF, 80000000, 12345678, FFFFFFFF]
  #_ REGISTER_IN v2 [00000001, FFFFFFFF, 11111111, FFFFFFFE]
  vaddsws v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [7FFFFFFF, 80000000, 23456789, FFFFFFFD]

# vsubuwm: word subtract modulo 2^32
test_vsubuwm:
  #_ REGISTER_IN v1 [00000000, 00000005, 80000000, 12345678]
  #_ REGISTER_IN v2 [00000001, 00000007, 00000001, 12345679]
  vsubuwm v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [FFFFFFFF, FFFFFFFE, 7FFFFFFF, FFFFFFFF]

# vmaxuh: unsigned halfword maximum (0x8000 > 0x7FFF)
test_vmaxuh:
  #_ REGISTER_IN v1 [80007FFF, 0001FFFF, 12340000, ABCD5555]
  #_ REGISTER_IN v2 [7FFF8000, 00020000, 1234FFFF, ABCCAAAA]
  vmaxuh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [80008000, 0002FFFF, 1234FFFF, ABCDAAAA]

# vminuh: unsigned halfword minimum
test_vminuh:
  #_ REGISTER_IN v1 [80007FFF, 0001FFFF, 12340000, ABCD5555]
  #_ REGISTER_IN v2 [7FFF8000, 00020000, 1234FFFF, ABCCAAAA]
  vminuh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [7FFF7FFF, 00010000, 12340000, ABCC5555]

# vcmpequh: 0xFFFF where the halfwords are equal
test_vcmpequh:
  #_ REGISTER_IN v1 [80007FFF, 0001FFFF, 12340000, ABCD5555]
  #_ REGISTER_IN v2 [80007FFE, 0001FFFF, 12350000, ABCD5554]
  vcmpequh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [FFFF0000, FFFFFFFF, 0000FFFF, FFFF0000]

# vspltish: -16 sign-extended into every halfword
test_vspltish_negative:
  vspltish v3, -16
  blr
  #_ REGISTER_OUT v3 [FFF0FFF0, FFF0FFF0, FFF0FFF0, FFF0FFF0]

# vspltish: 15 in every halfword
test_vspltish_positive:
  vspltish v3, 15
  blr
  #_ REGISTER_OUT v3 [000F000F, 000F000F, 000F000F, 000F000F]

# vpkswss128 v3, v1, v2 (encoded): signed words to signed halfwords, vA first
test_vpkswss128:
  #_ REGISTER_IN v1 [00012345, FFFF0000, 00000005, FFFFFFFB]
  #_ REGISTER_IN v2 [00007FFF, FFFF8000, 80000000, 00008000]
  .long 0x14611280
  blr
  #_ REGISTER_OUT v3 [7FFF8000, 0005FFFB, 7FFF8000, 80007FFF]

# vpkuwum128 v1, v1, v2 (encoded): low halfwords, vA first; vD = vA
test_vpkuwum128_alias:
  #_ REGISTER_IN v1 [00012345, FFFF8001, 80000000, 00001234]
  #_ REGISTER_IN v2 [DEADBEEF, 0000FFFF, 00010000, 7FFF8000]
  .long 0x14211380
  blr
  #_ REGISTER_OUT v1 [23458001, 00001234, BEEFFFFF, 00008000]

# vpkuwus128 v3, v1, v2 (encoded): unsigned words to unsigned halfwords, saturating (0x80000000 -> 0xFFFF)
test_vpkuwus128:
  #_ REGISTER_IN v1 [00012345, FFFF8001, 80000000, 00001234]
  #_ REGISTER_IN v2 [DEADBEEF, 0000FFFF, 00010000, 7FFF8000]
  .long 0x146113C0
  blr
  #_ REGISTER_OUT v3 [FFFFFFFF, FFFF1234, FFFFFFFF, FFFFFFFF]

# vpkuhus128 v3, v1, v2 (encoded): unsigned halfwords to unsigned bytes, saturating (0x8000 -> 0xFF)
test_vpkuhus128:
  #_ REGISTER_IN v1 [800000FF, 01000012, FFFF0000, 007F0080]
  #_ REGISTER_IN v2 [00017FFF, 00FE8001, 020000AB, 000500FF]
  .long 0x14611340
  blr
  #_ REGISTER_OUT v3 [FFFFFF12, FF007F80, 01FFFEFF, FFAB05FF]

# vcfpuxws128 v3, v1, 0 (encoded): 1.5 -> 1, -1 -> 0, NaN -> 0, 5e9 -> 0xFFFFFFFF
test_vcfpuxws128:
  #_ REGISTER_IN v1 [3FC00000, BF800000, 7FC00000, 4F9502F9]
  .long 0x18600A70
  blr
  #_ REGISTER_OUT v3 [00000001, 00000000, 00000000, FFFFFFFF]

# vcfpuxws128 v3, v1, 4 (encoded): times 16: 8, 48, 2^32 saturates, 0.5 truncates to 0
test_vcfpuxws128_scaled:
  #_ REGISTER_IN v1 [3F000000, 40400000, 4D800000, 3D000000]
  .long 0x18640A70
  blr
  #_ REGISTER_OUT v3 [00000008, 00000030, FFFFFFFF, 00000000]
