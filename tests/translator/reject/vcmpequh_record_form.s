# vcmpequh. (record form) must write CR6; patch 0008 translates only vcmpequh (FM4 uses no record
# form), so 0003's record-form check rejects it instead of dropping the CR update.
#! FATAL record form vcmpequh. at
test_vcmpequh_record_form:
  vcmpequh. v3, v1, v2
  blr
  #_ REGISTER_OUT r3 0
