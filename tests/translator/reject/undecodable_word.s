# A non-zero word the decoder does not know is fatal, never skipped.
#! FATAL unable to decode instruction
test_undecodable:
  li r3, 1
  .long 0x00000001
  blr
