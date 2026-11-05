# A branch to a zero word of the code outside any function (patch 0021): the word is an illegal
# instruction, so the translation stops there.
test_branch_to_zero_word:
  b 1f
  #_ REGISTER_OUT r3 0

1:
  .long 0
