# A branch whose target is in no function (here, past the end of the code) is rejected.
#! FATAL is not a function
test_branch_outside_functions:
  b .+0x1000
  #_ REGISTER_OUT r3 0
