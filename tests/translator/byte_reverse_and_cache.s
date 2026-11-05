# lhbrx and dcbst (patches/xenonrecomp/0010), expected values by hand from the PowerPC ISA:
# lhbrx loads the halfword at (rA|0) + (rB) with its bytes swapped, zero-extended; dcbst has no
# architected effect on registers or memory contents.

test_lhbrx:
  #_ REGISTER_IN r4 0x4000
  #_ REGISTER_IN r5 2
  #_ MEMORY_IN 4002 80 01
  lhbrx r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x0180

test_lhbrx_high_bit:
  #_ REGISTER_IN r4 0x4100
  #_ REGISTER_IN r5 0
  #_ MEMORY_IN 4100 34 92
  lhbrx r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x9234

test_dcbst:
  #_ REGISTER_IN r3 0x4200
  #_ REGISTER_IN r4 0x40
  #_ MEMORY_IN 4240 12 34
  dcbst r3, r4
  lhz r5, 0x40(r3)
  blr
  #_ REGISTER_OUT r5 0x1234
  #_ REGISTER_OUT r3 0x4200
  #_ REGISTER_OUT r4 0x40
