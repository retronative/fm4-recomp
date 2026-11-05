# vupkd3d128 with UIMM & 3 != 0 (FM4 Recomp; patch 0012) is not a type this translator decodes:
# rejected, never translated as another type. vupkd3d128 v3, v1, 21: ForzaRecomp's 0x18740FF0 (UIMM
# 20, in the rA field, bits 16-20) with UIMM 21 = 0x18750FF0.
#! FATAL vupkd3d128 immediate 21 at
test_vupkd3d128_immediate_low_bits:
  .long 0x18750FF0
  blr
  #_ REGISTER_OUT r3 0
