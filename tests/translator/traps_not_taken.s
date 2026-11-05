# Trap instructions whose condition is false continue (patches/xenonrecomp/0003). Conditions
# from the PowerPC ISA's tw/td definition: TO bits 16 signed <, 8 signed >, 4 ==, 2 unsigned <,
# 1 unsigned >; tw compares the low 32 bits of rA, td all 64, with the sign-extended SIMM.

# twllei (TO 6): 1 is neither == 0 nor unsigned < 0.
test_twllei_continues:
  #_ REGISTER_IN r3 1
  twi 6, r3, 0
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

# twlgei (TO 5): 4 is neither == 5 nor unsigned > 5.
test_twlgei_continues:
  #_ REGISTER_IN r3 4
  twi 5, r3, 5
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

# tdllei (TO 6) compares 64 bits: 0x100000000 is not <= 0 (its low word is 0, so tw would trap).
test_tdllei_uses_64_bits:
  #_ REGISTER_IN r3 0x100000000
  tdi 6, r3, 0
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

# tdlgei (TO 5): 4 is neither == 5 nor unsigned > 5.
test_tdlgei_continues:
  #_ REGISTER_IN r3 4
  tdi 5, r3, 5
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

# twi 17 (signed < or unsigned >) against -1: 0 is neither signed < -1 nor unsigned > 0xFFFFFFFF.
test_twi_signedness:
  #_ REGISTER_IN r3 0
  twi 17, r3, -1
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

