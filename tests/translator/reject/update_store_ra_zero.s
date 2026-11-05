# An update form with rA = 0 is an invalid form (PowerPC ISA): rejected, not translated.
# Encoded directly: sthu r3, 2(r0) is 0xB4600002 (opcode 45, rS 3, rA 0, d 2).
#! FATAL invalid form sthu
test_update_store_ra_zero:
  .long 0xB4600002
  blr
  #_ REGISTER_OUT r3 0
