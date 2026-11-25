#!/usr/bin/env python3
"""Generate and validate the Vulkan FHD shaders in a patched, private SDK overlay.

Apply native-fhd-shaders.patch first. Only generated headers are written to the
overlay. --list prints the SDK-relative files needed by a sparse build overlay.
"""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

SHADERS = Path("src/graphics/shaders")
HEADERS = SHADERS / "vulkan_spirv"
GAMMA = tuple("apply_gamma_" + ramp + luma + "_cs"
              for ramp in ("pwl", "table") for luma in ("", "_fxaa_luma"))
MASK = "hdr_hud_mask_cs"
CP = Path("src/graphics/vulkan/command_processor.cpp")


def native_variants():
    for source in ("color", "depth", "uint"):
        for multisample in ("", "_ms"):
            settings = [] if source == "color" else ["FH1_SOURCE_" + source.upper() + "=1"]
            if multisample:
                settings.append("FH1_SOURCE_MSAA=1")
            tag = source + multisample
            for target in ("memory", "image"):
                defines = settings + (["FH1_DEST_IMAGE=1"] if target == "image" else [])
                yield f"fh1_native_resolve_{target}_{tag}_cs", "fh1_native_resolve_memory.cs.hlsl", defines
            yield f"fh1_native_transfer_words_{tag}_cs", "fh1_native_transfer_words.cs.hlsl", settings
            for kind, target in enumerate(("color", "depth", "stencil", "uint", "depth_stencil")):
                for target_ms in ("", "_dms"):
                    defines = settings + [f"FH1_DEST_KIND={kind}"]
                    if target_ms:
                        defines.append("FH1_DEST_MSAA=1")
                    name = f"fh1_native_transfer_{target}{target_ms}_from_{tag}_ps"
                    yield name, "fh1_native_transfer.ps.hlsl", defines


def call(args, data=None):
    result = subprocess.run(args, input=data, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed: {(result.stderr + result.stdout).decode(errors='replace')}")
    return result.stdout


def read_binary(header):
    words = [int(s, 16) for s in re.findall(r"0x[0-9a-fA-F]{8}\b", header.read_text())]
    if not words or words[0] != 0x07230203:
        raise ValueError(f"Invalid shader header: {header}")
    return struct.pack(f"<{len(words)}I", *words)


def write_header(binary, name, path):
    data = binary.read_bytes()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [f"#pragma once\nconst uint32_t {name}[] = {{"]
    for offset in range(0, len(words), 8):
        lines.append("    " + ", ".join(f"0x{w:08x}" for w in words[offset:offset+8]) + ",")
    path.write_text("\n".join([*lines, "};", ""]))


def native_gamma(header, binary):
    # Upstream stores these four shaders only as optimized SPIR-V. Preserve its
    # arithmetic and descriptor ABI, and change only the size test and the 2D
    # source fetch. Gamma-ramp buffer fetches and output stores stay unchanged.
    assembly = call(["spirv-dis", "-"], read_binary(header)).decode()
    bounds = list(re.finditer(r"^\s*(%\w+) = OpUGreaterThanEqual %v2bool (%\w+) (%\w+)$", assembly, re.M))
    fetch = list(re.finditer(r"^(\s*%\w+ = OpImageFetch %v4float %\w+ )(%\w+)( Lod %int_0)$", assembly, re.M))
    function = re.search(r"^\s*%\w+ = OpFunction ", assembly, re.M)
    if len(bounds) != 1 or len(fetch) != 1 or not function:
        raise ValueError(f"Unrecognized gamma instructions in {header}")
    check, pos, size = bounds[0].groups()
    constants = """
%fm4_mask_x = OpConstant %uint 2147483647
%fm4_mask_y = OpConstant %uint 4294967295
%fm4_bounds_mask = OpConstantComposite %v2uint %fm4_mask_x %fm4_mask_y
%fm4_highbit = OpConstant %uint 2147483648
%fm4_two = OpConstant %uint 2
%fm4_three = OpConstant %uint 3
%fm4_four = OpConstant %uint 4
%fm4_twos = OpConstantComposite %v2uint %fm4_two %fm4_two
%fm4_threes = OpConstantComposite %v2uint %fm4_three %fm4_three
%fm4_fours = OpConstantComposite %v2uint %fm4_four %fm4_four
"""
    check_size = f"""
%fm4_bounds = OpBitwiseAnd %v2uint {size} %fm4_bounds_mask
{check} = OpUGreaterThanEqual %v2bool {pos} %fm4_bounds
"""
    sample = f"""
%fm4_width = OpCompositeExtract %uint {size} 0
%fm4_active = OpUGreaterThanEqual %bool %fm4_width %fm4_highbit
%fm4_active_xy = OpCompositeConstruct %v2bool %fm4_active %fm4_active
%fm4_product = OpIMul %v2uint {pos} %fm4_fours
%fm4_numerator = OpIAdd %v2uint %fm4_product %fm4_twos
%fm4_virtual = OpUDiv %v2uint %fm4_numerator %fm4_threes
%fm4_read_xy = OpSelect %v2uint %fm4_active_xy %fm4_virtual {pos}
%fm4_read = OpBitcast %v2int %fm4_read_xy
{fetch[0][1]}%fm4_read{fetch[0][3]}
"""
    # Reverse order keeps all disassembler spans valid.
    for start, end, replacement in sorted([
            (*fetch[0].span(), sample), (*bounds[0].span(), check_size),
            (function.start(), function.start(), constants)], reverse=True):
        assembly = assembly[:start] + replacement + assembly[end:]
    call(["spirv-as", "--target-env", "vulkan1.0", "-o", binary, "-"], assembly.encode())


def generate(sdk, overlay):
    if sdk.resolve() == overlay.resolve():
        raise ValueError("A separate output overlay is required")
    destination = overlay / HEADERS
    destination.mkdir(parents=True, exist_ok=True)
    variant_list = list(native_variants())
    with tempfile.TemporaryDirectory(prefix="fm4-fhd-shaders-") as directory:
        work = Path(directory)
        sources = {v[1] for v in variant_list}
        sources.update(("fh1_native_edram.hlsli", "hdr_hud_mask.cs.glsl"))
        for filename in sources:
            shutil.copyfile(overlay / SHADERS / filename, work / filename)
        shutil.copyfile(sdk / SHADERS / "fh1_push_constants.hlsli", work / "fh1_push_constants.hlsli")
        for name, source, defines in variant_list:
            binary = work / (name + ".spv")
            stage = "frag" if name.endswith("_ps") else "comp"
            call(["glslangValidator", "-D", "-V", "-S", stage, "-e", "main", "-DFH1_SPIRV=1",
                  *["-D" + d for d in defines], "--hlsl-iomap", "--shift-texture-binding", "16",
                  "--shift-UAV-binding", "32", "-o", binary, work / source])
            if name.startswith("fh1_native_transfer_depth_stencil"):
                header = work / (name + ".h")
                call([sys.executable, "-I", "-B", sdk / SHADERS / "spirv_stencil_export.py",
                      binary, header, name])
                binary.write_bytes(read_binary(header))
            call(["spirv-val", "--target-env", "vulkan1.0", binary])
            write_header(binary, name, work / (name + ".h"))
        for name in (*GAMMA, MASK):
            binary = work / (name + ".spv")
            if name == MASK:
                call(["glslangValidator", "-V", "-S", "comp", "-o", binary, work / "hdr_hud_mask.cs.glsl"])
            else:
                native_gamma(sdk / HEADERS / (name + ".h"), binary)
            call(["spirv-val", "--target-env", "vulkan1.0", binary])
            write_header(binary, name, work / (name + ".h"))
        cp = (overlay / CP).read_text()
        embedded = re.findall(r'GetSwapApplyGamma\w*ComputeRbSwapSource\(\) \{\n  return R"\((.*?)\)";', cp, re.S)
        if len(embedded) != 4:
            raise ValueError("Expected four runtime-compiled R/B gamma shaders")
        for index, glsl in enumerate(embedded):
            source = work / f"rb{index}.comp"
            source.write_text(glsl)
            binary = work / f"rb{index}.spv"
            call(["glslangValidator", "-V", "-S", "comp", "-o", binary, source])
            call(["spirv-val", "--target-env", "vulkan1.0", binary])
        # Only publish results once every variant validates.
        for name in [*(v[0] for v in variant_list), *GAMMA, MASK]:
            src = work / (name + ".h")
            target = destination / src.name
            if not target.is_file() or src.read_bytes() != target.read_bytes():
                shutil.copyfile(src, target)
    print(f"{len(variant_list)+5} embedded shaders and 4 runtime GLSL shaders validated")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--overlay", type=Path)
    parser.add_argument("--list", action="store_true", help="list generated headers only")
    args = parser.parse_args()
    if args.list:
        for name in [*(v[0] for v in native_variants()), *GAMMA, MASK]:
            print(HEADERS / (name + ".h"))
    elif args.sdk is None or args.overlay is None:
        parser.error("--sdk and --overlay are required")
    else:
        generate(args.sdk.resolve(), args.overlay.resolve())
