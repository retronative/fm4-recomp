# A branch to a zero word that lies in no function stops (FM4 Recomp; patch 0021's zero-word target
# path). The zero word is placed before the test function, so it cannot be inside it (a forward
# target would be, and would trap through 0004's rule instead). b .-4 jumps back to it.
  .long 0
test_branch_back_to_zero_word:
  #_ REGISTER_IN r3 1
  b .-4
  #_ REGISTER_OUT r3 1
