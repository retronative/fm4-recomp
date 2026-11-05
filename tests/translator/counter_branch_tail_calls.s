# Counter branches whose target is another function are tail calls (patches/xenonrecomp/0003).
# test_helper_nine returns 9 in r3; the others return 7 when the branch is not taken. (Every label is a harness test, so the helper
# carries its own expectation.)
# Expected values worked out by hand from the PowerPC ISA (bdz: CTR decremented, branch if 0;
# bdnz: branch if not 0; bdnzf BI: branch if CTR != 0 and CR bit BI is clear). The harness cannot
# check CTR; the taken/not-taken pairs pin a single decrement (CTR 2 would reach 0 under bdz, and
# CTR 1 stay non-zero under bdnz, if it were decremented twice or not at all).

test_helper_nine:
  li r3, 9
  blr
  #_ REGISTER_OUT r3 9

test_bdz_taken:
  #_ REGISTER_IN r4 1
  mtctr r4
  bdz test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9

test_bdz_not_taken:
  #_ REGISTER_IN r4 2
  mtctr r4
  bdz test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

test_bdnz_taken:
  #_ REGISTER_IN r4 2
  mtctr r4
  bdnz test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9

test_bdnz_not_taken:
  #_ REGISTER_IN r4 1
  mtctr r4
  bdnz test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

test_bdnzf_taken:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 1
  mtctr r4
  cmpwi cr0, r5, 0
  bdnzf eq, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9

test_bdnzf_not_taken_eq:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 0
  mtctr r4
  cmpwi cr0, r5, 0
  bdnzf eq, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

# bdnzf tests the CR bit it names, not eq: cmpwi of 1 against 0 sets gt and clears eq. BI 5 is
# cr1.gt (4 * field + bit; lt 0, gt 1, eq 2, so 3).
test_bdnzf_gt_not_taken:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 1
  mtctr r4
  cmpwi cr0, r5, 0
  bdnzf gt, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7

test_bdnzf_cr1_gt_taken:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 0
  mtctr r4
  cmpwi cr1, r5, 0
  bdnzf 5, test_helper_nine
  li r3, 7
  blr
  #_ REGISTER_OUT r3 9

# The same inside the function (a goto): cr1.gt set, so the branch is not taken.
test_bdnzf_cr1_gt_local:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 1
  mtctr r4
  cmpwi cr1, r5, 0
  bdnzf 5, 1f
  li r3, 7
  blr
1:
  li r3, 9
  blr
  #_ REGISTER_OUT r3 7
