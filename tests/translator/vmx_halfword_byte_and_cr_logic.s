# Instructions used by FM4's XMediaFacade module (patches/xenonrecomp/0032). Expected values from
# the PowerPC/AltiVec definitions with an integer model (guest element 0 is the first REGISTER
# value; halfword 0 is the high half of word 0). Counts are the low 4 (halfword) or 3 (byte) bits
# of each element of vB. CR results are read with mfcr and rotated into r3's low 4 bits
# (lt, gt, eq, so). vpkswus128 is encoded by hand (registers below 32) and confirmed by the
# decoder's disassembly comment.

# vslh: counts 0, 1, 2, 3, 4, 15, 16 (as 0), 17 (as 1)
test_vslh:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [00000001, 00020003, 0004000F, 00100011]
  vslh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [01238ACE, 26AC6F78, EDC00000, 76546420]

# vsrh: logical
test_vsrh:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [00000001, 00020003, 0004000F, 00100011]
  vsrh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [012322B3, 226A19BD, 0FED0001, 76541908]

# vsrah: arithmetic (the sign fills)
test_vsrah:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [00000001, 00020003, 0004000F, 00100011]
  vsrah v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [012322B3, E26AF9BD, FFEDFFFF, 76541908]

# vrlh: rotate left
test_vrlh:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [00000001, 00020003, 0004000F, 00100011]
  vrlh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [01238ACE, 26AE6F7E, EDCF5D4C, 76546420]

# vsrab: byte i >> (i mod 8), arithmetic
test_vsrab:
  #_ REGISTER_IN v1 [80FF7F01, 40C0817E, 01234567, 89ABCDEF]
  #_ REGISTER_IN v2 [00010203, 04050607, 08090A0B, 0C0D0E0F]
  vsrab v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [80FF1F00, 04FEFE00, 0111110C, F8FDFFFF]

# vmaxsh: signed halfword maximum (0x7FFF > 0x8000)
test_vmaxsh:
  #_ REGISTER_IN v1 [7FFF8000, 0001FFFF, 12348001, 0000FFFE]
  #_ REGISTER_IN v2 [80007FFF, FFFF0001, 12358000, FFFFFFFD]
  vmaxsh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [7FFF7FFF, 00010001, 12358001, 0000FFFE]

# vminsh: signed halfword minimum
test_vminsh:
  #_ REGISTER_IN v1 [7FFF8000, 0001FFFF, 12348001, 0000FFFE]
  #_ REGISTER_IN v2 [80007FFF, FFFF0001, 12358000, FFFFFFFD]
  vminsh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [80008000, FFFFFFFF, 12348000, FFFFFFFD]

# vavguh: (a + b + 1) >> 1 in 17 bits
test_vavguh:
  #_ REGISTER_IN v1 [FFFF0000, 00018000, 1234FFFE, 00037FFF]
  #_ REGISTER_IN v2 [FFFF0001, 00028000, 4321FFFF, 00048000]
  vavguh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [FFFF0001, 00028000, 2AABFFFF, 00048000]

# vsubshs: signed halfword subtract, saturating to [-32768, 32767]
test_vsubshs:
  #_ REGISTER_IN v1 [7FFF8000, 00058000, 12340000, 7FFFFFFF]
  #_ REGISTER_IN v2 [FFFF0001, 00078000, 02348000, 80007FFF]
  vsubshs v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [7FFF8000, FFFE0000, 10007FFF, 7FFF8000]

# vsubsbs: signed byte subtract, saturating to [-128, 127]
test_vsubsbs:
  #_ REGISTER_IN v1 [7F800580, 12007FFF, 10203040, 50607080]
  #_ REGISTER_IN v2 [FF010780, 0280807F, 01020304, 05060708]
  vsubsbs v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [7F80FE00, 107F7F80, 0F1E2D3C, 4B5A6980]

# vsububm: byte subtract modulo 2^8
test_vsububm:
  #_ REGISTER_IN v1 [000180FF, 10203040, 7F8000FF, 12345678]
  #_ REGISTER_IN v2 [010101FF, 2010313F, 807FFF00, 78563412]
  vsububm v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [FF007F00, F010FF01, FF0101FF, 9ADE2266]

# vcmpgtsh: signed halfword greater-than
test_vcmpgtsh:
  #_ REGISTER_IN v1 [0001FFFF, 80007FFF, 12341234, 00008001]
  #_ REGISTER_IN v2 [00000000, 7FFF8000, 12341233, FFFF8000]
  vcmpgtsh v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [FFFF0000, 0000FFFF, 0000FFFF, FFFFFFFF]

# vcmpgtsh.: every element true sets CR6 lt (1000)
test_vcmpgtsh_record_all:
  #_ REGISTER_IN v1 [00010001, 00010001, 00010001, 00010001]
  #_ REGISTER_IN v2 [00000000, 00000000, 0000FFFF, 80000000]
  vcmpgtsh. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 8
  #_ REGISTER_OUT v3 [FFFFFFFF, FFFFFFFF, FFFFFFFF, FFFFFFFF]

# vcmpgtsh.: some true, some false: CR6 clear (0000)
test_vcmpgtsh_record_mixed:
  #_ REGISTER_IN v1 [0001FFFF, 80007FFF, 12341234, 00008001]
  #_ REGISTER_IN v2 [00000000, 7FFF8000, 12341233, FFFF8000]
  vcmpgtsh. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 0

# vcmpgtsw: signed word greater-than
test_vcmpgtsw:
  #_ REGISTER_IN v1 [00000001, FFFFFFFF, 80000000, 7FFFFFFF]
  #_ REGISTER_IN v2 [00000000, 00000000, 7FFFFFFF, 80000000]
  vcmpgtsw v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [FFFFFFFF, 00000000, 00000000, FFFFFFFF]

# vcmpgtsw.: no element true sets CR6 eq (0010)
test_vcmpgtsw_record_none:
  #_ REGISTER_IN v1 [00000001, FFFFFFFF, 80000000, 7FFFFFFF]
  #_ REGISTER_IN v2 [00000001, 00000000, 80000000, 7FFFFFFF]
  vcmpgtsw. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 2
  #_ REGISTER_OUT v3 [00000000, 00000000, 00000000, 00000000]

# vpkswss: vA's words then vB's, each to a signed halfword with saturation
test_vpkswss:
  #_ REGISTER_IN v1 [00007FFF, 00008000, FFFF8000, FFFF7FFF]
  #_ REGISTER_IN v2 [00000001, FFFFFFFF, 7FFFFFFF, 80000000]
  vpkswss v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [7FFF7FFF, 80008000, 0001FFFF, 7FFF8000]

# vpkswus128 v3, v1, v2 (encoded): signed words to unsigned halfwords (negative -> 0)
test_vpkswus128:
  #_ REGISTER_IN v1 [00007FFF, 00008000, FFFF8000, FFFF7FFF]
  #_ REGISTER_IN v2 [00000001, FFFFFFFF, 7FFFFFFF, 80000000]
  .long 0x146112C0
  blr
  #_ REGISTER_OUT v3 [7FFF8000, 00000000, 00010000, FFFF0000]

# vpkshss: vA's halfwords then vB's, each to a signed byte with saturation
test_vpkshss:
  #_ REGISTER_IN v1 [007F0080, FF80FF7F, 0001FFFF, 7FFF8000]
  #_ REGISTER_IN v2 [00000010, FFF00100, FE000042, FFBF0055]
  vpkshss v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [7F7F8080, 01FF7F80, 0010F07F, 8042BF55]

# mulhd: high 64 bits of the signed 128-bit product; 2^32 * (3 * 2^32) = 3 * 2^64
test_mulhd_positive:
  #_ REGISTER_IN r4 0x0000000100000000
  #_ REGISTER_IN r5 0x0000000300000000
  mulhd r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x0000000000000003

# mulhd: -1 * 5 = -5, whose high half is all ones
test_mulhd_negative:
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 0x0000000000000005
  mulhd r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFF

# mulhd: (-2^63) * (-2^63) = 2^126
test_mulhd_minimum:
  #_ REGISTER_IN r4 0x8000000000000000
  #_ REGISTER_IN r5 0x8000000000000000
  mulhd r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x4000000000000000

# cror/crorc on CR bit numbers (4 * field + lt 0, gt 1, eq 2, so 3). cr1 = gt (r5 = 1 > 0),
# cr2 = eq (r6 = 0): cr3.lt = 0 | 0, cr3.gt = 0 | 1, cr3.eq = 0 | !0, cr3.so = 0 | !1 -> 0110
test_cror_crorc:
  #_ REGISTER_IN r5 1
  #_ REGISTER_IN r6 0
  cmpwi cr1, r5, 0
  cmpwi cr2, r6, 0
  cror 12, 4, 8
  cror 13, 4, 5
  crorc 14, 4, 8
  crorc 15, 4, 5
  mfcr r3
  rlwinm r3, r3, 16, 28, 31
  blr
  #_ REGISTER_OUT r3 6

# vpkshss128 v3, v1, v2 (encoded): as vpkshss
test_vpkshss128:
  #_ REGISTER_IN v1 [007F0080, FF80FF7F, 0001FFFF, 7FFF8000]
  #_ REGISTER_IN v2 [00000010, FFF00100, FE000042, FFBF0055]
  .long 0x14611200
  blr
  #_ REGISTER_OUT v3 [7F7F8080, 01FF7F80, 0010F07F, 8042BF55]

# vcmpgtuh.: unsigned (0x8000 > 0x7FFF); every element true sets CR6 lt (1000)
test_vcmpgtuh_record_all:
  #_ REGISTER_IN v1 [80008000, FFFF0001, 00020003, 7FFF0100]
  #_ REGISTER_IN v2 [7FFF0000, FFFE0000, 00010002, 7FFE00FF]
  vcmpgtuh. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 8
  #_ REGISTER_OUT v3 [FFFFFFFF, FFFFFFFF, FFFFFFFF, FFFFFFFF]

# vcmpgtuh.: one element true: CR6 clear (0000)
test_vcmpgtuh_record_mixed:
  #_ REGISTER_IN v1 [80007FFF, 12345678, 00000000, FFFFFFFF]
  #_ REGISTER_IN v2 [7FFF8000, 12345678, 00000000, FFFFFFFF]
  vcmpgtuh. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT v3 [FFFF0000, 00000000, 00000000, 00000000]

# vcmpequh.: no element equal sets CR6 eq (0010)
test_vcmpequh_record_none:
  #_ REGISTER_IN v1 [00010002, 00030004, 00050006, 00070008]
  #_ REGISTER_IN v2 [00000000, 00000000, 00000000, 00000000]
  vcmpequh. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 2
  #_ REGISTER_OUT v3 [00000000, 00000000, 00000000, 00000000]

# vcmpequh.: every element equal sets CR6 lt (1000)
test_vcmpequh_record_all:
  #_ REGISTER_IN v1 [12345678, 9ABCDEF0, 00000000, FFFFFFFF]
  #_ REGISTER_IN v2 [12345678, 9ABCDEF0, 00000000, FFFFFFFF]
  vcmpequh. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 8
  #_ REGISTER_OUT v3 [FFFFFFFF, FFFFFFFF, FFFFFFFF, FFFFFFFF]
