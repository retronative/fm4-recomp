# Secondary entries (patches/xenonrecomp/0021): a direct branch into another function's body runs
# that body from the target and returns to the branching function's caller (blr is a C return).
# test_owner's tail (addi; blr) is entered from two other functions.

test_owner:
  li r3, 10
1:
  addi r3, r3, 1
  blr
  #_ REGISTER_OUT r3 11

test_branch_into_owner:
  li r3, 5
  b 1b
  #_ REGISTER_OUT r3 6

test_conditional_into_owner_taken:
  #_ REGISTER_IN r4 0
  li r3, 20
  cmpwi cr0, r4, 0
  beq 1b
  li r3, 99
  blr
  #_ REGISTER_OUT r3 21

test_conditional_into_owner_not_taken:
  #_ REGISTER_IN r4 1
  li r3, 20
  cmpwi cr0, r4, 0
  beq 1b
  li r3, 99
  blr
  #_ REGISTER_OUT r3 99

# A call into another function's body runs its tail and returns after the call.
test_call_into_owner:
  li r3, 30
  bl 1b
  addi r3, r3, 100
  blr
  #_ REGISTER_OUT r3 131
