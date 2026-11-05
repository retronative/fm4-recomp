# twllei (TO 6) traps when rA == 0: the harness must stop.
test_twllei_zero:
  #_ REGISTER_IN r3 0
  twi 6, r3, 0
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7
