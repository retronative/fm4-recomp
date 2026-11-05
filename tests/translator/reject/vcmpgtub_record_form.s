# vcmpgtub. (record form) must write CR6; only the plain form is translated (no FM4 module uses
# the record form), so 0003's record-form check rejects it instead of dropping the CR update.
#! FATAL record form vcmpgtub. at
test_vcmpgtub_record_form:
  vcmpgtub. v3, v1, v2
  blr
  #_ REGISTER_OUT r3 0
