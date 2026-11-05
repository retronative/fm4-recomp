# A function whose last instruction (not a call) falls into padding is rejected.
#! FATAL falls through past its end
test_falls_into_padding:
  li r3, 1
  .long 0
  #_ REGISTER_OUT r3 1
