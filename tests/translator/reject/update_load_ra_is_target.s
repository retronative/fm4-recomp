# lhzu with rA = rT is an invalid form (PowerPC ISA): rejected, not translated.
#! FATAL invalid form lhzu
test_update_load_ra_is_target:
  lhzu r3, 2(r3)
  blr
  #_ REGISTER_OUT r3 0
