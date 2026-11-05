# frsqrte. (record form) must write CR1 from FPSCR, which is not modelled: rejected (FM4 Recomp;
# FM4 uses only the plain form at its nine sites).
#! FATAL record form frsqrte. at
test_frsqrte_record_form:
  frsqrte. f3, f1
  blr
  #_ REGISTER_OUT r3 0
