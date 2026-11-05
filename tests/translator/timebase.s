# mftb (patches/xenonrecomp/0015): TBR 268 reads the whole 64-bit time base (64-bit mode), TBR
# 269 (mftbu) its upper word (PowerPC ISA, mftb). The value comes from the runtime's
# PpcQueryTimebase(); tests/translator/harness_doubles.h returns 0x0123456789ABCDEF. Encoded as
# FM4's classic form (31 << 26 | rT << 21 | TBR split low/high 5 bits at 16/11 | 371 << 1);
# llvm-mc would emit mfspr instead.

test_mftb:
  .long 0x7C6C42E6
  blr
  #_ REGISTER_OUT r3 0x0123456789ABCDEF

test_mftbu:
  .long 0x7C6D42E6
  blr
  #_ REGISTER_OUT r3 0x01234567
