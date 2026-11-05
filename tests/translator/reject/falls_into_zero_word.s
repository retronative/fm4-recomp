# Code that runs into a zero word before the function's last instruction is rejected. The beq
# target keeps the function going past the zero word.
#! FATAL falls into a zero word
test_falls_into_zero_word:
  cmpwi cr0, r3, 0
  beq 1f
  li r3, 1
  .long 0
1:
  li r3, 2
  blr
  #_ REGISTER_OUT r3 2
