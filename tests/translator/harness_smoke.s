# Harness smoke test (FM4 Recomp): the smallest translatable function. It needs no translator
# patch, so it isolates the harness build itself: every harness compiles with
# -Werror=asm-operand-widths, which fails until patches/xenonrecomp/0003 (FPCR from a 64-bit
# register; ForzaRecomp 0013) is applied. Expected value: li r3, 7 sets r3 = 7 (PowerPC ISA, addi
# with rA = 0).
test_harness_smoke:
  li r3, 7
  blr
  #_ REGISTER_OUT r3 7
