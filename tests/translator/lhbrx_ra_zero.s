# lhbrx with rA = 0 (FM4 Recomp; patch 0011). PowerPC ISA: b <- (rA|0), so rA = 0 means the base is
# the literal 0, not r0's contents: EA = (rB). r0 holds a decoy that would move EA to 0x3100 (zeros).
# Halfword 12 34 byte-reversed and zero-extended: 0x3412.

test_lhbrx_ra_zero:
  #_ REGISTER_IN r0 0x100
  #_ REGISTER_IN r5 0x3000
  #_ MEMORY_IN 3000 12 34
  lhbrx r3, 0, r5
  blr
  #_ REGISTER_OUT r3 0x3412
