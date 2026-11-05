# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
"""Mutants for tests/mutate.py: one planted fault each, the test that must catch it, and the
check it is expected to fail (a substring of a line of that test's output that starts with
"FAIL" or "ERROR"). `file` is a repository path; tools/XenonRecomp/... is the patched XenonRecomp tree
(patches/xenonrecomp), copied with the rest into build/mutate/.

Every new or changed behaviour gets mutants here, with tests whose expected values are derived
independently (the PowerPC ISA, Xenia cited by commit, XDK documentation, hand computation),
never from our own code. A mutant that cannot change behaviour is listed with `equivalent="why"`.
When code is rewritten, re-express its mutants for the same fault; never drop them.

Design after reFrozen tests/mutants.py (dict entries, TESTS map).
"""

PY = ["/opt/homebrew/bin/python3", "-B", "-I"]

# key -> command run from the scratch tree's root, its timeout (s), whether it needs the scratch
# XenonRecomp build, and a regex that shows the test ran (absent => INVALID, never a kill).
TESTS = {
    "xexinfo": dict(cmd=PY + ["tests/test_xexinfo.py"], timeout=60, build=False, ran=r"^Ran \d+ tests?"),
    "stfsinfo": dict(cmd=PY + ["tests/test_stfsinfo.py"], timeout=60, build=False, ran=r"^Ran \d+ tests?"),
    "loader": dict(cmd=["bash", "tests/run_loader_tests.sh"], timeout=900, build=True, ran=r"^(pass|FAIL): xex_"),
    "mutate": dict(cmd=PY + ["tests/test_mutate.py"], timeout=300, build=False, ran=r"^Ran \d+ tests?"),
}

XEXINFO = "survey/xexinfo.py"
STFSINFO = "survey/stfsinfo.py"
XEX_CPP = "tools/XenonRecomp/XenonUtils/xex.cpp"
GATE = "tests/mutate.py"

MUTANTS = [
    # ---- M1: the gate itself, tests/mutate.py (tests/test_mutate.py, toy tree) ----
    dict(id="G1", milestone="M1", file=GATE, test="mutate", fault="any FAIL line counts as the expected check",
         old="hit = [l for l in fails if m[\"expect\"] in l]", new="hit = fails",
         expect="test_killed_other"),
    dict(id="G2", milestone="M1", file=GATE, test="mutate", fault="a mutant equal to the original is run",
         old="    if hashlib.sha256(mutated.encode()).digest() == hashlib.sha256(original.encode()).digest():\n        return \"INVALID\", \"the mutant equals the original\"\n",
         new="", expect="test_invalid_cases"),
    dict(id="G3", milestone="M1", file=GATE, test="mutate", fault="a test that did not run can count as a kill",
         old="    if not re.search(test[\"ran\"], out, re.M):\n        return \"INVALID\", \"the test did not run\"\n",
         new="", expect="test_invalid_cases"),
    dict(id="G4", milestone="M1", file=GATE, test="mutate", fault="timeout kills the leader only, not its process group",
         old="            os.killpg(p.pid, signal.SIGTERM)\n            time.sleep(1)\n            os.killpg(p.pid, signal.SIGKILL)",
         new="            p.kill()", expect="test_timeout_kills_process_group"),
    dict(id="G5", milestone="M1", file=GATE, test="mutate", fault="the scratch file is not restored after a mutant",
         old="    finally:\n        with open(path, \"w\") as f:\n            f.write(original)\n",
         new="    finally:\n        pass\n", expect="test_restores_and_never_touches_the_real_tree"),
    dict(id="G6", milestone="M1", file=GATE, test="mutate", fault="ERROR lines not counted as failures",
         old='if l.strip().startswith(("FAIL", "ERROR"))]', new='if l.strip().startswith("FAIL")]',
         expect="test_classify_shapes"),
    dict(id="G7", milestone="M1", file=GATE, test="mutate", fault="survivor of a documented-equivalent reported as SURVIVED",
         old='return ("EQUIVALENT", m["equivalent"]) if m.get("equivalent") else ("SURVIVED", "the test passed")',
         new='return ("SURVIVED", "the test passed")', expect="test_equivalent_and_wrong_equivalent"),
    # ---- M0: survey/xexinfo.py (tests/test_xexinfo.py; synthetic XEX2 from Xenia xex2_info.h) ----
    dict(id="X1", milestone="M0", file=XEXINFO, test="xexinfo", fault="page count not shifted out of the descriptor word",
         old="size, kind = (word >> 4) * page, word & 0xF", new="size, kind = word * page, word & 0xF",
         expect="test_security_and_page_runs_64k"),
    dict(id="X2", milestone="M0", file=XEXINFO, test="xexinfo", fault="inline rule drops key low byte 0x01",
         old="(key & 0xFF) in (0x00, 0x01)", new="(key & 0xFF) in (0x00,)",
         expect="test_inline_optional_headers"),
    dict(id="X3", milestone="M0", file=XEXINFO, test="xexinfo", fault="4 KB page flag ignored",
         old="page = 0x1000 if image_flags", new="page = 0x10000 if image_flags",
         expect="test_page_runs_4k_flag"),
    dict(id="X4", milestone="M0", file=XEXINFO, test="xexinfo", fault="version major read from the low nibble (Xenia's bitfield order)",
         old='return "%d.%d.%d.%d" % (v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF)',
         new='return "%d.%d.%d.%d" % (v & 0xF, (v >> 4) & 0xF, (v >> 8) & 0xFFFF, v >> 24)',
         expect="test_execution_info"),
    dict(id="X5", milestone="M0", file=XEXINFO, test="xexinfo", fault="adjacent page runs of one kind not merged",
         old='runs[-1]["kind_id"] == kind and runs[-1]["end"] == addr', new="False",
         expect="test_security_and_page_runs_64k"),
    dict(id="X6", milestone="M0", file=XEXINFO, test="xexinfo", fault="import string-table padding not skipped",
         old="while p < end and r.u8(p) == 0:", new="while False:",
         expect="test_import_libraries"),
    dict(id="X7", milestone="M0", file=XEXINFO, test="xexinfo", fault="title ID read at +0x8",
         old='"title_id": r.u32(o + 0xC)', new='"title_id": r.u32(o + 0x8)',
         expect="test_execution_info"),
    dict(id="X8", milestone="M0", file=XEXINFO, test="xexinfo", fault="bounds check removed",
         old="if off < 0 or off + struct.calcsize(fmt) > len(self.d):", new="if False:",
         expect="test_rejects_truncated_file"),
    dict(id="X9", milestone="M0", file=XEXINFO, test="xexinfo", fault="magic check removed",
         old='if r.bytes(0, 4) != b"XEX2":', new="if False:",
         expect="test_rejects_bad_magic"),

    # ---- M0: survey/stfsinfo.py (tests/test_stfsinfo.py; synthetic packages from Xenia stfs_xbox.h) ----
    dict(id="S1", milestone="M0", file=STFSINFO, test="stfsinfo", fault="one hash table per level always",
         old="self.per_table = 1 if self.read_only else 2", new="self.per_table = 1",
         expect="test_two_tables_per_level"),
    dict(id="S2", milestone="M0", file=STFSINFO, test="stfsinfo", fault="block offset ignores level-0 tables",
         old="block += ((index + level_base) // level_base) * self.per_table", new="block += 0",
         expect="test_read_only_block_to_offset"),
    dict(id="S3", milestone="M0", file=STFSINFO, test="stfsinfo", fault="no break below the level base",
         old="            if index < level_base:\n                break\n        return self.base",
         new="        return self.base",
         expect="test_read_only_block_to_offset"),
    dict(id="S4", milestone="M0", file=STFSINFO, test="stfsinfo", fault="file-table count read big-endian",
         old='"file_table_block_count": h[v + 3] | (h[v + 4] << 8)',
         new='"file_table_block_count": (h[v + 3] << 8) | h[v + 4]',
         expect="test_little_endian_table_count"),
    dict(id="S5", milestone="M0", file=STFSINFO, test="stfsinfo", fault="data base not rounded to a block",
         old='self.base = (info["header_size"] + BLOCK - 1) & ~(BLOCK - 1)', new='self.base = info["header_size"]',
         expect="test_read_only_block_to_offset"),
    dict(id="S6", milestone="M0", file=STFSINFO, test="stfsinfo", fault="root_active_index ignored",
         old="secondary = BLOCK if self.root_active else 0", new="secondary = 0",
         expect="test_root_active_index_selects_secondary_table"),
    dict(id="S7", milestone="M0", file=STFSINFO, test="stfsinfo", fault="level-1 active bit ignored",
         old="secondary = BLOCK if info_raw & 0x40000000 else 0", new="secondary = 0",
         expect="test_level1_active_bit_selects_level0_table"),
    dict(id="S8", milestone="M0", file=STFSINFO, test="stfsinfo", fault="next block read from record 0",
         old="record = index % PER_LEVEL[0] if level == 0", new="record = 0 if level == 0",
         expect="test_chained_file_table"),
    dict(id="S9", milestone="M0", file=STFSINFO, test="stfsinfo", fault="directory flag read from bit 6",
         old='"directory": bool(flags & 0x80)', new='"directory": bool(flags & 0x40)',
         expect="test_listing"),
    dict(id="S10", milestone="M0", file=STFSINFO, test="stfsinfo", fault="name length mask 0x7F (includes the contiguous bit)",
         old="name = e[:flags & 0x3F]", new="name = e[:flags & 0x7F]",
         expect="test_listing"),
    dict(id="S11", milestone="M0", file=STFSINFO, test="stfsinfo", fault="short file-table chain accepted",
         old="            if index == END_OF_CHAIN:\n                raise ValueError(",
         new="            if index == END_OF_CHAIN:\n                break\n                raise ValueError(",
         expect="test_rejects_short_chain"),
    dict(id="S12", milestone="M0", file=STFSINFO, test="stfsinfo", fault="descriptor length unchecked",
         old='if s["descriptor_length"] != 0x24:', new="if False:",
         expect="test_rejects_bad_descriptor"),
    dict(id="S13", milestone="M0", file=STFSINFO, test="stfsinfo", fault="display name offset +1",
         old="OFF_DISPLAY_NAME = 0x411", new="OFF_DISPLAY_NAME = 0x412",
         expect="test_header_fields"),
    dict(id="S14", milestone="M0", file=STFSINFO, test="stfsinfo", fault="title ID read at +0x8",
         old='"title_id": self.be32(e + 0xC)', new='"title_id": self.be32(e + 0x8)',
         expect="test_header_fields"),
    dict(id="S15", milestone="M0", file=STFSINFO, test="stfsinfo", fault="level-0 hash table number for blocks < 28900 falls through",
         old="            if index < PER_LEVEL[1]:\n                return block\n", new="",
         expect="test_read_only_hash_tables"),

    # ---- M1: patches/xenonrecomp/0002 (tests/run_loader_tests.sh: xex_section_*_test) ----
    dict(id="B1", milestone="M1", file=XEX_CPP, test="loader", fault="section name read as a C string past its 8-byte field",
         old="image.Map(std::string_view(name, strnlen(name, sizeof(section.Name))), rva",
         new="image.Map(std::string_view(name), rva",
         expect="xex_section_name_test"),
    dict(id="B2", milestone="M1", file=XEX_CPP, test="loader", fault="data section past the image keeps its full size",
         old="            size = rva < image.size ? image.size - rva : 0;\n", new="",
         expect="xex_section_extent_test"),
    dict(id="B3", milestone="M1", file=XEX_CPP, test="loader", fault="code section past the image accepted",
         old="            if (flags & SectionFlags_Code)\n                return {};\n", new="",
         expect="xex_section_extent_test"),
    dict(id="B4", milestone="M1", file=XEX_CPP, test="loader", fault="data pointer past the buffer for a section beyond the image",
         old="rva < image.size ? image.data.get() + rva : nullptr);", new="image.data.get() + rva);",
         expect="xex_section_beyond_test"),
    dict(id="B5", milestone="M1", file=XEX_CPP, test="loader", fault="only the start is bounded, not the end",
         old="if (rva > image.size || size > image.size - rva)", new="if (rva > image.size)",
         expect="xex_section_extent_test"),

    # ---- M0: patches/xenonrecomp/0001 (tests/run_loader_tests.sh; synthetic XEX, xex_fixture.h) ----
    dict(id="L1", milestone="M0", file=XEX_CPP, test="loader", fault="allocate the expanded size (upstream behaviour)",
         old="result = std::make_unique<uint8_t[]>(security->imageSize.get());",
         new="result = std::make_unique<uint8_t[]>(imageSize);",
         expect="xex_basic_allocation_test"),
    dict(id="L2", milestone="M0", file=XEX_CPP, test="loader", fault="block sizes summed in 32 bits",
         old="expandedSize += uint64_t(blocks[i].dataSize) + blocks[i].zeroSize;",
         new="expandedSize = uint32_t(expandedSize + blocks[i].dataSize + blocks[i].zeroSize);",
         expect="xex_basic_wrap_test"),
    dict(id="L3", milestone="M0", file=XEX_CPP, test="loader", fault="no check that block data fits in the file",
         old=" || sourceSize > dataSize - header->headerSize)", new=")",
         expect="xex_basic_overread_test"),
    dict(id="L4", milestone="M0", file=XEX_CPP, test="loader", fault="no check that blocks fit the declared size",
         old="if (expandedSize > security->imageSize.get() || sourceSize", new="if (sourceSize",
         expect="xex_basic_allocation_test"),
    dict(id="L5", milestone="M0", file=XEX_CPP, test="loader", fault="no check for a format header without a block",
         old="if (fileFormatInfo->infoSize < sizeof(Xex2FileBasicCompressionInfo))\n                return {};\n", new="",
         expect="xex_basic_short_format_test"),
    dict(id="L6", milestone="M0", file=XEX_CPP, test="loader", fault="allocation not zero-filled",
         old="result = std::make_unique<uint8_t[]>(security->imageSize.get());",
         new="result = std::unique_ptr<uint8_t[]>(new uint8_t[security->imageSize.get()]);",
         expect="xex_basic_allocation_test"),  # re-expressed: make_unique_for_overwrite needs C++20; the CMake build is not
]
