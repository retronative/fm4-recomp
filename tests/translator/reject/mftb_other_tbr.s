# mftb from a TBR other than 268 (time base) or 269 (its upper word) is rejected.
#! FATAL mftb from TBR 270
test_mftb_other_tbr:
  .long 0x7C6E42E6
  blr
  #_ REGISTER_OUT r3 0
