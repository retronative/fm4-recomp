# twi 31 always traps.
test_twi_unconditional:
  #_ REGISTER_IN r3 1
  twi 31, r0, 22
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7
