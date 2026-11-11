# The instructions SpeechFacade_default.xex needs (FM4 Recomp patch 0037; survey: 34 rldicl., 10 rlwimi., 9 lfdux,
# 4 vcmpgtuw, 2 stfdux). Expected values from the PowerPC ISA (Book I) worked by hand; Xenon runs in 64-bit mode, so a record
# form sets CR0 by a signed comparison of the whole 64-bit result with zero (LT 8, GT 4, EQ 2 in the CR0 nibble; SO is 0 here).
# CR0 is read with mfcr and rlwinm r3, r3, 4, 28, 31 (CR0 to the low nibble); CR6 with rlwinm r3, r3, 28, 28, 31.

# rldicl. rA, rS, 0, 32 (disassembled as its alias clrldi.) keeps the low word: 0xFFFFFFFF80000000 -> 0x0000000080000000, positive as 64 bits: GT (a 32-bit
# comparison would say LT).
test_rldicl_record_64bit_positive:
  #_ REGISTER_IN r4 0xFFFFFFFF80000000
  rldicl. r5, r4, 0, 32
  mfcr r3
  rlwinm r3, r3, 4, 28, 31
  blr
  #_ REGISTER_OUT r3 4
  #_ REGISTER_OUT r5 0x80000000

# rldicl. rA, rS, 4, 32 (no alias: sh and mb non-zero, sh + mb != 64): 0x0000000008000000 rotated left 4 is 0x0000000080000000,
# the low word kept: 0x0000000080000000, positive as 64 bits: GT (a 32-bit comparison would say LT).
test_rldicl_record_plain_form:
  #_ REGISTER_IN r4 0x08000000
  rldicl. r5, r4, 4, 32
  mfcr r3
  rlwinm r3, r3, 4, 28, 31
  blr
  #_ REGISTER_OUT r3 4
  #_ REGISTER_OUT r5 0x80000000

# rldicl. rA, rS, 8, 56 (the srdi alias): rotate left 8, keep the low byte. 0x00FFFFFFFFFFFFFF rotated is 0xFFFFFFFFFFFFFF00: low byte 0: EQ.
test_rldicl_record_zero:
  #_ REGISTER_IN r4 0x00FFFFFFFFFFFFFF
  rldicl. r5, r4, 8, 56
  mfcr r3
  rlwinm r3, r3, 4, 28, 31
  blr
  #_ REGISTER_OUT r3 2
  #_ REGISTER_OUT r5 0

# rldicl. rA, rS, 1, 0 (the rotldi alias): rotate left 1, no mask: 0x4000000000000000 -> 0x8000000000000000: LT.
test_rldicl_record_negative:
  #_ REGISTER_IN r4 0x4000000000000000
  rldicl. r5, r4, 1, 0
  mfcr r3
  rlwinm r3, r3, 4, 28, 31
  blr
  #_ REGISTER_OUT r3 8
  #_ REGISTER_OUT r5 0x8000000000000000

# rlwimi. rA, rS, 0, 24, 31 inserts rS's low byte into rA bits 56-63 and keeps the rest of rA: rA 0x0000000180000000, rS 0
# -> 0x0000000180000000: positive as 64 bits: GT (its low word, 0x80000000, would be LT as 32 bits).
test_rlwimi_record_64bit_positive:
  #_ REGISTER_IN r5 0x0000000180000000
  #_ REGISTER_IN r4 0
  rlwimi. r5, r4, 0, 24, 31
  mfcr r3
  rlwinm r3, r3, 4, 28, 31
  blr
  #_ REGISTER_OUT r3 4
  #_ REGISTER_OUT r5 0x0000000180000000

# rlwimi. with an insert: rA 0, rS 0x5A -> 0x5A: GT.
test_rlwimi_record_insert:
  #_ REGISTER_IN r5 0
  #_ REGISTER_IN r4 0x5A
  rlwimi. r5, r4, 0, 24, 31
  mfcr r3
  rlwinm r3, r3, 4, 28, 31
  blr
  #_ REGISTER_OUT r3 4
  #_ REGISTER_OUT r5 0x5A

# lfdux frT, rA, rB: EA = rA + rB = 0x2010, frT = the big-endian doubleword there (1.0), then rA = EA.
test_lfdux_loads_and_updates:
  #_ REGISTER_IN r4 0x2000
  #_ REGISTER_IN r5 0x10
  #_ MEMORY_IN 2010 3F F0 00 00 00 00 00 00
  lfdux f3, r4, r5
  blr
  #_ REGISTER_OUT f3 0x3FF0000000000000
  #_ REGISTER_OUT r4 0x2010

# stfdux frS, rA, rB: EA = 0x2000 + 0x20 = 0x2020 gets 2.0 (0x4000000000000000) big-endian, then rA = EA; read back as the
# high word (lwz: 0x40000000) and the whole doubleword (lfd).
test_stfdux_stores_and_updates:
  #_ REGISTER_IN f1 0x4000000000000000
  #_ REGISTER_IN r4 0x2000
  #_ REGISTER_IN r5 0x20
  stfdux f1, r4, r5
  lwz r6, 0(r4)
  lfd f3, 0(r4)
  blr
  #_ REGISTER_OUT r4 0x2020
  #_ REGISTER_OUT r6 0x40000000
  #_ REGISTER_OUT f3 0x4000000000000000

# vcmpgtuw: unsigned word compare per lane. 0x80000000 > 1 and 0xFFFFFFFF > 0xFFFFFFFE are true unsigned (the first is false
# signed); 1 > 0x80000000 and 5 > 5 are false. Mixed lanes: CR6 is 0 (neither all true, 8, nor all false, 2).
test_vcmpgtuw_unsigned_lanes:
  #_ REGISTER_IN v1 [80000000, 00000001, FFFFFFFF, 00000005]
  #_ REGISTER_IN v2 [00000001, 80000000, FFFFFFFE, 00000005]
  vcmpgtuw. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT v3 [FFFFFFFF, 00000000, FFFFFFFF, 00000000]

# All lanes true: CR6 = 8.
test_vcmpgtuw_record_all_true:
  #_ REGISTER_IN v1 [00000002, 00000003, 00000004, 80000000]
  #_ REGISTER_IN v2 [00000001, 00000002, 00000003, 00000001]
  vcmpgtuw. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 8
  #_ REGISTER_OUT v3 [FFFFFFFF, FFFFFFFF, FFFFFFFF, FFFFFFFF]

# All lanes false (equal vectors): CR6 = 2.
test_vcmpgtuw_record_all_false:
  #_ REGISTER_IN v1 [00000007, 80000000, FFFFFFFF, 00000000]
  #_ REGISTER_IN v2 [00000007, 80000000, FFFFFFFF, 00000000]
  vcmpgtuw. v3, v1, v2
  mfcr r3
  rlwinm r3, r3, 28, 28, 31
  blr
  #_ REGISTER_OUT r3 2
  #_ REGISTER_OUT v3 [00000000, 00000000, 00000000, 00000000]

# The plain form leaves CR6 alone: the lanes as above, CR6 stays 0 from the prior state.
test_vcmpgtuw_plain:
  #_ REGISTER_IN v1 [00000002, 00000001, 00000000, FFFFFFFF]
  #_ REGISTER_IN v2 [00000001, 00000002, 00000000, 7FFFFFFF]
  vcmpgtuw v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [FFFFFFFF, 00000000, 00000000, FFFFFFFF]
