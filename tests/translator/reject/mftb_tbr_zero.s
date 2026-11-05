# mftb with TBR 0 is not the time base, although the decoder reports TBR 268 as 0: rejected.
#! FATAL mftb from TBR 0
test_mftb_tbr_zero:
  .long 0x7C6002E6
  blr
  #_ REGISTER_OUT r3 0
