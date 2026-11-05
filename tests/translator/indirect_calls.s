# blrl (patches/xenonrecomp/0012), expected values by hand from the PowerPC ISA: the branch target
# is LR before the instruction, and LR becomes the address after it. tests/translator/harness_doubles.h
# records the target in r9 instead of calling it. The function is first in the file: mtlr is at
# 0x10000, blrl at 0x10004, so LR is 0x10008.

test_blrl:
  #_ REGISTER_IN r3 0x12340
  mtlr r3
  blrl
  mflr r5
  blr
  #_ REGISTER_OUT r9 0x12340
  #_ REGISTER_OUT r5 0x10008
