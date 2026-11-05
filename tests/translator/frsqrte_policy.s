# frsqrte under the accepted project policy (FM4 Recomp patch 0034, the approach
# of ShiftGlue): frD = frB < 0.0 ? quiet NaN : 1.0 / sqrt(frB), in double precision, never rounded
# through float. NOT the Xbox 360 hardware estimate and not guaranteed bit-exact with it.
# Expected values are IEEE-754 results worked out by hand (registers given as raw bits):
#   4.0 -> 0.5; 16.0 -> 0.25; 0.25 -> 2.0 (exact square roots)
#   1 + 2^-40 (0x3FF0000000001000) -> 1 - 2^-41 (0x3FEFFFFFFFFFF000): sqrt = 1 + 2^-41 exactly,
#     1/(1 + 2^-41) = 1 - 2^-41 + 2^-82 rounds to 1 - 2^-41; rounding the input through float
#     would give 1.0 -> 1.0
#   +0 -> +inf; -0 -> -inf (sqrt(-0) = -0, 1/-0 = -inf; -0 < 0.0 is false)
#   -4.0, -inf -> quiet NaN 0x7FF8000000000000; +inf -> +0; quiet NaN 0x7FF8000000000000 -> itself
#   2^-1074 (smallest subnormal, 0x1) -> 2^537 (0x6180000000000000): subnormals are not flushed
#   2^-1022 (smallest normal) -> 2^511 (0x5FE0000000000000)

test_frsqrte_four:
  #_ REGISTER_IN f1 0x4010000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x3FE0000000000000

test_frsqrte_sixteen:
  #_ REGISTER_IN f1 0x4030000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x3FD0000000000000

test_frsqrte_quarter:
  #_ REGISTER_IN f1 0x3FD0000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x4000000000000000

test_frsqrte_no_float_rounding:
  #_ REGISTER_IN f1 0x3FF0000000001000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x3FEFFFFFFFFFF000

test_frsqrte_plus_zero:
  #_ REGISTER_IN f1 0x0000000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x7FF0000000000000

test_frsqrte_minus_zero:
  #_ REGISTER_IN f1 0x8000000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0xFFF0000000000000

test_frsqrte_negative:
  #_ REGISTER_IN f1 0xC010000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x7FF8000000000000

test_frsqrte_minus_infinity:
  #_ REGISTER_IN f1 0xFFF0000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x7FF8000000000000

test_frsqrte_plus_infinity:
  #_ REGISTER_IN f1 0x7FF0000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x0000000000000000

test_frsqrte_quiet_nan:
  #_ REGISTER_IN f1 0x7FF8000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x7FF8000000000000

test_frsqrte_smallest_subnormal:
  #_ REGISTER_IN f1 0x0000000000000001
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x6180000000000000

test_frsqrte_smallest_normal:
  #_ REGISTER_IN f1 0x0010000000000000
  frsqrte f3, f1
  blr
  #_ REGISTER_OUT f3 0x5FE0000000000000
