# lwbrx (patches/xenonrecomp/0033), expected values by hand from the PowerPC ISA (Book I): lwbrx
# loads the word at (rA|0) + (rB) with its bytes reversed and zero-extends it into rT. It is
# emitted through PPC_MM_LOAD_U32 (the runtime's device-register hook; the default is the plain
# load), which the generated code must name.
#! EMITS __builtin_bswap32(PPC_MM_LOAD_U32(

test_lwbrx:
  #_ REGISTER_IN r4 0x4000
  #_ REGISTER_IN r5 4
  #_ MEMORY_IN 4004 12 34 56 78
  lwbrx r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x78563412

test_lwbrx_ra_zero:
  #_ REGISTER_IN r0 0x4000
  #_ REGISTER_IN r4 0x4100
  #_ MEMORY_IN 4100 80 00 00 01
  lwbrx r3, 0, r4
  blr
  #_ REGISTER_OUT r3 0x01000080

test_lwbrx_zero_extends:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r4 0x4200
  #_ REGISTER_IN r5 0
  #_ MEMORY_IN 4200 01 02 03 F4
  lwbrx r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0xF4030201
