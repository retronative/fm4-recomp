# VMX shifts, logic, select and memory-hint forms (patches/xenonrecomp/0007). Expected values
# from the PowerPC/AltiVec definitions with a 128-bit big-endian integer model (guest element 0 is
# the first REGISTER value, guest byte 0 the most significant). VMX128 forms have no assembler
# support: they are encoded as (op << 26) | (vD << 21) | (vA or rA << 16) | (vB or rB << 11) | xo
# for registers below 32, and the generated code's disassembly comment confirms each one.

# vsl: (vA) << 3 bits, 128-bit
test_vsl:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [03030303, 03030303, 03030303, 03030303]
  vsl v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [091A2B3C, 4D5E6F7F, F6E5D4C3, B2A19080]

# vslo: (vA) << 3 octets; only guest byte 15 (0x18) holds the count, the others would mean 10
test_vslo:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [50505050, 50505050, 50505050, 50505018]
  vslo v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [6789ABCD, EFFEDCBA, 98765432, 10000000]

# vslo128 v3, v1, v2 (encoded): (vA) << 3 octets
test_vslo128:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [50505050, 50505050, 50505050, 50505018]
  .long 0x14611390
  blr
  #_ REGISTER_OUT v3 [6789ABCD, EFFEDCBA, 98765432, 10000000]

# vsro128 v3, v1, v2 (encoded): (vA) >> 3 octets
test_vsro128:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [50505050, 50505050, 50505050, 50505018]
  .long 0x146113D0
  blr
  #_ REGISTER_OUT v3 [00000001, 23456789, ABCDEFFE, DCBA9876]

# vsrb: guest byte i >> (i mod 8)
test_vsrb:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [00010203, 04050607, 00010203, 04050607]
  vsrb v3, v1, v2
  blr
  #_ REGISTER_OUT v3 [0111110C, 08050301, FE6E2E13, 07020000]

# vnor128 v3, v1, v2 (encoded): ~(vA | vB)
test_vnor128:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [0F0F0F0F, 00000000, FFFF0000, 12345678]
  .long 0x14611290
  blr
  #_ REGISTER_OUT v3 [F0D0B090, 76543210, 00004567, 898B8987]

# vsel128 v3, v1, v2, v3 (encoded): (vA & ~vD) | (vB & vD), the mask is the old vD
test_vsel128:
  #_ REGISTER_IN v1 [01234567, 89ABCDEF, FEDCBA98, 76543210]
  #_ REGISTER_IN v2 [0F0F0F0F, 00000000, FFFF0000, 12345678]
  #_ REGISTER_IN v3 [FFFFFFFF, 00000000, FF00FF00, 0000FFFF]
  .long 0x14611350
  blr
  #_ REGISTER_OUT v3 [0F0F0F0F, 89ABCDEF, FFDC0098, 76545678]

# lvxl128 v3, r4, r5 (encoded): the quadword at (r4 + r5) & ~15
test_lvxl128:
  #_ REGISTER_IN r4 0x3000
  #_ REGISTER_IN r5 4
  #_ MEMORY_IN 3000 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F
  .long 0x10642AC3
  blr
  #_ REGISTER_OUT v3 [10111213, 14151617, 18191A1B, 1C1D1E1F]

# lvehx: the halfword at 0x3206 is guest element 3 (bytes 6-7); the other elements, which the ISA leaves undefined, hold the rest of the quadword as with lvx
test_lvehx:
  #_ REGISTER_IN r4 0x3200
  #_ REGISTER_IN r5 6
  #_ MEMORY_IN 3200 40 41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F
  lvehx v3, r4, r5
  blr
  #_ REGISTER_OUT v3 [40414243, 44454647, 48494A4B, 4C4D4E4F]

# stvlxl128 v3, r4, r5 (encoded): guest bytes 0-11 to 0x3304..0x330F; 0x3300..0x3303 untouched
test_stvlxl128:
  #_ REGISTER_IN v3 [A0A1A2A3, B0B1B2B3, C0C1C2C3, D0D1D2D3]
  #_ REGISTER_IN r4 0x3300
  #_ REGISTER_IN r5 4
  .long 0x10642F03
  blr
  #_ MEMORY_OUT 3300 00 00 00 00 A0 A1 A2 A3 B0 B1 B2 B3 C0 C1 C2 C3
