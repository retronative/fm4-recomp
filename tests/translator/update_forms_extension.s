# Update-form loads zero-extend into all 64 bits of rT (FM4 Recomp; complements ForzaRecomp's
# update_forms.s, whose rT always starts at 0). PowerPC ISA: lbzux rT <- 56 zeros || MEM(EA,1);
# lhzu rT <- 48 zeros || MEM(EA,2); lwzux rT <- 32 zeros || MEM(EA,4); then rA <- EA. rT is preloaded
# with all ones, so a lowering that writes only the low bits leaves ones above the loaded value.
# Whether rA's high word survives the update (32-bit EA) is INFERRED and not pinned here.

test_lbzux_zero_extends:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r4 0x2000
  #_ REGISTER_IN r5 0x10
  #_ MEMORY_IN 2010 9C
  lbzux r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x9C
  #_ REGISTER_OUT r4 0x2010

test_lhzu_zero_extends:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r4 0x2100
  #_ MEMORY_IN 2102 80 01
  lhzu r3, 2(r4)
  blr
  #_ REGISTER_OUT r3 0x8001
  #_ REGISTER_OUT r4 0x2102

test_lwzux_zero_extends:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r4 0x2200
  #_ REGISTER_IN r5 8
  #_ MEMORY_IN 2208 89 AB CD EF
  lwzux r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x89ABCDEF
  #_ REGISTER_OUT r4 0x2208
