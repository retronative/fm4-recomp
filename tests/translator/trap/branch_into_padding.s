# A branch to a zero word (an illegal instruction) stops instead of continuing past it.
test_branch_into_padding:
  #_ REGISTER_IN r3 0
  cmpwi cr0, r3, 0
  beq 1f
  li r3, 7
  blr
1:
  .long 0
  #_ REGISTER_OUT r3 7
