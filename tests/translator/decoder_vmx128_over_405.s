# Decoder: on Xenon, opcode-4 words with the VMX128 bit are VMX128 instructions, never PowerPC
# 405 ones (patches/xenonrecomp/0009). vsldoi128 v3, v1, v34, 12 has the low 11 bits 0x311 of 405
# mullhwu. (XO 392, Rc 1): VB128 = 2 + 32 (its bit 5 is word bit 0), SHB = 12 (word bits 6-9).
# vsldoi128: the 16 bytes of (vA || vB) starting at byte SHB.

test_vsldoi128_not_mullhwu:
  #_ REGISTER_IN v1 [00010203, 04050607, 08090A0B, 0C0D0E0F]
  #_ REGISTER_IN v34 [10111213, 14151617, 18191A1B, 1C1D1E1F]
  .long 0x10611311
  blr
  #_ REGISTER_OUT v3 [0C0D0E0F, 10111213, 14151617, 18191A1B]
