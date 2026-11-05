# Update-form loads and stores (patches/xenonrecomp/0004), expected values by hand from the
# PowerPC ISA: EA = (rA) + d or (rA) + (rB); loads zero-extend (lha sign-extends); a store uses
# (rS) before rA is updated; then rA = EA. Floats: lfs widens single to double, stfs rounds
# double to single (0.1 -> 0x3DCCCCCD). Each test uses its own memory.

test_lbzux:
  #_ REGISTER_IN r4 0x1000
  #_ REGISTER_IN r5 0x10
  #_ MEMORY_IN 1010 9C
  lbzux r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x9C
  #_ REGISTER_OUT r4 0x1010

test_lhzu:
  #_ REGISTER_IN r4 0x1100
  #_ MEMORY_IN 1102 80 01
  lhzu r3, 2(r4)
  blr
  #_ REGISTER_OUT r3 0x8001
  #_ REGISTER_OUT r4 0x1102

test_lhzu_negative_displacement:
  #_ REGISTER_IN r4 0x1200
  #_ MEMORY_IN 11FE 12 34
  lhzu r3, -2(r4)
  blr
  #_ REGISTER_OUT r3 0x1234
  #_ REGISTER_OUT r4 0x11FE

test_lhzux_ra_equals_rb:
  #_ REGISTER_IN r4 0x900
  #_ MEMORY_IN 1200 FF FE
  lhzux r3, r4, r4
  blr
  #_ REGISTER_OUT r3 0xFFFE
  #_ REGISTER_OUT r4 0x1200

test_lhau:
  #_ REGISTER_IN r4 0x1400
  #_ MEMORY_IN 1402 80 01
  lhau r3, 2(r4)
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFF8001
  #_ REGISTER_OUT r4 0x1402

test_lwzux:
  #_ REGISTER_IN r4 0x1500
  #_ REGISTER_IN r5 8
  #_ MEMORY_IN 1508 89 AB CD EF
  lwzux r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x89ABCDEF
  #_ REGISTER_OUT r4 0x1508

test_ldux:
  #_ REGISTER_IN r4 0x1600
  #_ REGISTER_IN r5 8
  #_ MEMORY_IN 1608 01 23 45 67 89 AB CD EF
  ldux r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x0123456789ABCDEF
  #_ REGISTER_OUT r4 0x1608

test_stbux:
  #_ REGISTER_IN r3 0x1234
  #_ REGISTER_IN r4 0x1700
  #_ REGISTER_IN r5 1
  stbux r3, r4, r5
  blr
  #_ MEMORY_OUT 1700 00 34 00
  #_ REGISTER_OUT r4 0x1701

test_sthu:
  #_ REGISTER_IN r3 0xABCD
  #_ REGISTER_IN r4 0x1800
  sthu r3, 2(r4)
  blr
  #_ MEMORY_OUT 1802 AB CD
  #_ REGISTER_OUT r4 0x1802

test_sthu_rs_equals_ra:
  #_ REGISTER_IN r4 0x1900
  sthu r4, 2(r4)
  blr
  #_ MEMORY_OUT 1902 19 00
  #_ REGISTER_OUT r4 0x1902

test_sthux:
  #_ REGISTER_IN r3 0x5678
  #_ REGISTER_IN r4 0x1A00
  #_ REGISTER_IN r5 6
  sthux r3, r4, r5
  blr
  #_ MEMORY_OUT 1A06 56 78
  #_ REGISTER_OUT r4 0x1A06

test_stdux:
  #_ REGISTER_IN r3 0x0123456789ABCDEF
  #_ REGISTER_IN r4 0x1B00
  #_ REGISTER_IN r5 8
  stdux r3, r4, r5
  blr
  #_ MEMORY_OUT 1B08 01 23 45 67 89 AB CD EF
  #_ REGISTER_OUT r4 0x1B08

test_lfsu:
  #_ REGISTER_IN r4 0x1C00
  #_ MEMORY_IN 1C04 3F C0 00 00
  lfsu f1, 4(r4)
  blr
  #_ REGISTER_OUT f1 1.5
  #_ REGISTER_OUT r4 0x1C04

test_lfsux:
  #_ REGISTER_IN r4 0x1D00
  #_ REGISTER_IN r5 0x10
  #_ MEMORY_IN 1D10 C0 20 00 00
  lfsux f1, r4, r5
  blr
  #_ REGISTER_OUT f1 -2.5
  #_ REGISTER_OUT r4 0x1D10

test_lfdu:
  #_ REGISTER_IN r4 0x1E00
  #_ MEMORY_IN 1E08 40 09 21 FB 54 44 2D 18
  lfdu f1, 8(r4)
  blr
  #_ REGISTER_OUT f1 3.141592653589793
  #_ REGISTER_OUT r4 0x1E08

test_stfsu:
  #_ REGISTER_IN f1 0.1
  #_ REGISTER_IN r4 0x1F04
  stfsu f1, -4(r4)
  blr
  #_ MEMORY_OUT 1F00 3D CC CC CD
  #_ REGISTER_OUT r4 0x1F00

test_stfsux:
  #_ REGISTER_IN f1 -1.0
  #_ REGISTER_IN r4 0x2000
  #_ REGISTER_IN r5 0xC
  stfsux f1, r4, r5
  blr
  #_ MEMORY_OUT 200C BF 80 00 00
  #_ REGISTER_OUT r4 0x200C

test_stfdu:
  #_ REGISTER_IN f1 1.0
  #_ REGISTER_IN r4 0x2100
  stfdu f1, 8(r4)
  blr
  #_ MEMORY_OUT 2108 3F F0 00 00 00 00 00 00
  #_ REGISTER_OUT r4 0x2108
