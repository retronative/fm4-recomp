# A function ending in a call followed by padding: if the callee returns, the next word (zero) is
# an illegal instruction, so the translation stops there instead of continuing (patch 0019).
test_callee_returns:
  li r3, 1
  blr
  #_ REGISTER_OUT r3 1

test_call_then_padding:
  bl test_callee_returns
  .long 0
  #_ REGISTER_OUT r3 1
