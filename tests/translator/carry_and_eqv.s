# addc, addme, subfze and eqv (patches/xenonrecomp/0006), expected values by hand from the
# PowerPC ISA with CA and CR0 taken from the low word (32-bit mode, as upstream's adde/addze).
# CA is read back with "li r7, 0; addze r7, r7"; CR with mfcr (CR0 lt 0x80000000, eq 0x20000000).
# CA is set to 1 with "li r9, -1; addic r9, r9, 1" (0xFFFFFFFF + 1 carries); it starts at 0.

test_addc_carries:
  #_ REGISTER_IN r4 0xFFFFFFFF
  #_ REGISTER_IN r5 1
  addc r3, r4, r5
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 0x100000000
  #_ REGISTER_OUT r7 1

# CA in is ignored by addc
test_addc_no_carry:
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 2
  li r9, -1
  addic r9, r9, 1
  addc r3, r4, r5
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 3
  #_ REGISTER_OUT r7 0

# CR0 from the low word: 0x80000000 is negative (lt)
test_addc_record_negative:
  #_ REGISTER_IN r4 0x7FFFFFFF
  #_ REGISTER_IN r5 1
  addc. r3, r4, r5
  mfcr r8
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r8 0x80000000
  #_ REGISTER_OUT r3 0x80000000
  #_ REGISTER_OUT r7 0

test_addc_alias:
  #_ REGISTER_IN r4 0xFFFFFFFF
  #_ REGISTER_IN r5 2
  addc r4, r4, r5
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r4 0x100000001
  #_ REGISTER_OUT r7 1

test_addme_zero_no_ca:
  #_ REGISTER_IN r4 0
  addme r3, r4
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r7 0

test_addme_nonzero_no_ca:
  #_ REGISTER_IN r4 5
  addme r3, r4
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 4
  #_ REGISTER_OUT r7 1

test_addme_zero_ca:
  #_ REGISTER_IN r4 0
  li r9, -1
  addic r9, r9, 1
  addme r3, r4
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT r7 1

test_subfze_zero_no_ca:
  #_ REGISTER_IN r4 0
  subfze r3, r4
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r7 0

test_subfze_zero_ca:
  #_ REGISTER_IN r4 0
  li r9, -1
  addic r9, r9, 1
  subfze r3, r4
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT r7 1

test_subfze_nonzero_ca:
  #_ REGISTER_IN r4 5
  li r9, -1
  addic r9, r9, 1
  subfze r3, r4
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFB
  #_ REGISTER_OUT r7 0

# Result 0: CR0 eq
test_subfze_record_zero:
  #_ REGISTER_IN r4 0
  li r9, -1
  addic r9, r9, 1
  subfze. r3, r4
  mfcr r8
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r8 0x20000000
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT r7 1

test_subfze_alias:
  #_ REGISTER_IN r4 1
  subfze r4, r4
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFE
  #_ REGISTER_OUT r7 0

test_eqv:
  #_ REGISTER_IN r4 0xF0F0
  #_ REGISTER_IN r5 0xFF00
  eqv r3, r4, r5
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFF00F
  #_ REGISTER_OUT r7 0

# All ones: -1 in the low word, CR0 lt
test_eqv_record_all_ones:
  #_ REGISTER_IN r4 0x1234
  #_ REGISTER_IN r5 0x1234
  eqv. r3, r4, r5
  mfcr r8
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r8 0x80000000
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r7 0

# rD = rA: CA must come from the old (rA) = 1 (non-zero, so 1), not from the result 0.
test_addme_alias:
  #_ REGISTER_IN r4 1
  addme r4, r4
  li r7, 0
  addze r7, r7
  blr
  #_ REGISTER_OUT r4 0
  #_ REGISTER_OUT r7 1
