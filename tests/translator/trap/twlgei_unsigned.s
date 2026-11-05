# twlgei (TO 5) is unsigned: -1 (0xFFFFFFFF) is > 5, so it traps.
test_twlgei_unsigned:
  #_ REGISTER_IN r3 0xFFFFFFFF
  twi 5, r3, 5
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7
