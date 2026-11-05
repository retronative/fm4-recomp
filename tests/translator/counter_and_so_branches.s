# bdzf, bdnzt, bso and bns (patches/xenonrecomp/0005), expected values by hand from the PowerPC
# ISA: bdzf BI decrements CTR, then branches if CTR = 0 and CR bit BI is clear; bdnzt if CTR != 0
# and the bit is set. BI = 4 * field + bit (lt 0, gt 1, eq 2, so 3). bso/bns test bit 3: after
# fcmpu it is "unordered", after cmpw a copy of XER[SO] (bit 0x80000000 of mtxer's value).
# test_helper_nine returns 9; the others return 7 when not taken. A double decrement or none would
# flip the CTR = 1 / CTR = 2 pairs.

test_helper_nine:
  li r3, 9
  blr
  #_ REGISTER_OUT r3 9

test_bdzf_taken:
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 1
  mtctr r4
  cmpwi cr0, r5, 0
  bdzf eq, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9

test_bdzf_ctr_not_zero:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 1
  mtctr r4
  cmpwi cr0, r5, 0
  bdzf eq, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

test_bdzf_bit_set:
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 0
  mtctr r4
  cmpwi cr0, r5, 0
  bdzf eq, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

# BI 24 is cr6.lt; a branch inside the function. -1 < 0 sets lt, so not taken.
test_bdzf_cr6_lt_local_not_taken:
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 0xFFFFFFFFFFFFFFFF
  mtctr r4
  cmpwi cr6, r5, 0
  bdzf 24, 1f
  li r3, 7
  blr
1:
  li r3, 9
  blr
  #_ REGISTER_OUT r3 7

test_bdzf_cr6_lt_local_taken:
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 1
  mtctr r4
  cmpwi cr6, r5, 0
  bdzf 24, 1f
  li r3, 7
  blr
1:
  li r3, 9
  blr
  #_ REGISTER_OUT r3 9

test_bdnzt_taken:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 0
  mtctr r4
  cmpwi cr0, r5, 0
  bdnzt eq, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9

test_bdnzt_bit_clear:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 1
  mtctr r4
  cmpwi cr0, r5, 0
  bdnzt eq, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

test_bdnzt_ctr_zero:
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 0
  mtctr r4
  cmpwi cr0, r5, 0
  bdnzt eq, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

test_bso_unordered:
  #_ REGISTER_IN f1 1.0
  #_ REGISTER_IN f2 0x7FF8000000000000
  fcmpu cr1, f1, f2
  bso cr1, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9

test_bso_ordered:
  #_ REGISTER_IN f1 1.0
  #_ REGISTER_IN f2 2.0
  fcmpu cr1, f1, f2
  bso cr1, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

test_bns_ordered_local:
  #_ REGISTER_IN f1 1.0
  #_ REGISTER_IN f2 1.0
  fcmpu cr0, f1, f2
  bns 1f
  li r3, 7
  blr
1:
  li r3, 9
  blr
  #_ REGISTER_OUT r3 9

test_bns_unordered_local:
  #_ REGISTER_IN f1 0x7FF8000000000000
  #_ REGISTER_IN f2 1.0
  fcmpu cr0, f1, f2
  bns 1f
  li r3, 7
  blr
1:
  li r3, 9
  blr
  #_ REGISTER_OUT r3 7

test_bso_integer_xer_so_set:
  #_ REGISTER_IN r5 0x80000000
  #_ REGISTER_IN r6 1
  mtxer r5
  cmpwi cr0, r6, 0
  bso test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9

test_bns_integer_xer_so_clear:
  #_ REGISTER_IN r5 0
  #_ REGISTER_IN r6 1
  mtxer r5
  cmpwi cr0, r6, 0
  bns test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9
