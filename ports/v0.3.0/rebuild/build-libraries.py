#!/usr/bin/env python3
"""Build the FM4 player runtime's three replaceable third-party libraries on macOS."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

NAMES = {
    "libavutil": "libavutil.56.dylib",
    "libavcodec": "libavcodec.58.dylib",
    "mspack": "libshiftglue_lzx.dylib",
}


def build(args):
    root = Path(__file__).resolve().parent
    files = json.loads((root / "library-sources.json").read_text())
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    platform = ["-arch", "arm64", "-isysroot", sdk, "-mmacosx-version-min=14.0"]
    common = ["-O3", "-DNDEBUG", "-fPIC", "-mcmodel=small", "-fno-strict-aliasing",
              "-ffp-model=strict", "-fno-char8_t", "-w", "-DREX_PLATFORM_MAC=1",
              '-DREXGLUE_BUILD_CONFIG="Release"']
    ff = args.ffmpeg.resolve()
    mspack = args.mspack.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        common.extend([f"-ffile-prefix-map={ff}=FFmpeg",
                       f"-ffile-prefix-map={mspack}=libmspack",
                       f"-ffile-prefix-map={work}=build"])
        overlay = work / "overlay" / "libavcodec"
        overlay.mkdir(parents=True)
        for name in ("bsf_list.c", "codec_list.c", "parser_list.c"):
            shutil.copyfile(args.overlay / name, overlay / name)
        for target, filename in NAMES.items():
            commands = []
            objects = []
            for index, unit in enumerate(files[target]):
                flags = []
                if target == "mspack":
                    source = mspack / unit
                    flags += ["-I", str(mspack / "cabextract" / "mspack")]
                else:
                    source = ff / unit
                    if target == "libavcodec":
                        flags += ["-I", str(overlay.parent)]
                    flags += ["-DHAVE_ARMV8=1", "-DHAVE_AV_CONFIG_H", "-DHAVE_INLINE_ASM=1",
                              "-DHAVE_NEON=1", "-D_USE_MATH_DEFINES", "-I", str(ff),
                              "-I", str(ff / "compat/atomics/gcc")]
                if source.suffix == ".S":
                    flags += ["-march=armv8-a"]
                obj = work / (target + "_" + str(index) + ".o")
                objects.append(str(obj))
                commands.append(["xcrun", "clang", *platform, *common, *flags, "-c", str(source), "-o", str(obj)])
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                list(pool.map(run, commands))
            inputs = []
            if target == "libavcodec":
                # Link the player decoder roots like the SDK. The static SDK archive
                # also contains unused pieces of video decoders it never enabled.
                archive = work / "codec.a"
                run(["xcrun", "ar", "rcs", str(archive), *objects])
                roots = ["avcodec_find_decoder", "avcodec_alloc_context3", "avcodec_free_context",
                         "avcodec_open2", "avcodec_receive_frame", "avcodec_send_packet",
                         "av_packet_alloc", "avcodec_version", "avcodec_find_decoder_by_name",
                         "av_parser_close", "av_parser_init", "av_parser_parse2"]
                objects = ["-Wl,-u,_" + name for name in roots] + [str(archive)]
                inputs = [str(out / NAMES["libavutil"])]
            run(["xcrun", "clang", *platform, "-dynamiclib", "-Wl,-headerpad_max_install_names",
                 "-Wl,-rpath,@loader_path", "-install_name", "@rpath/" + filename, *objects,
                 *inputs, "-o", str(out / filename)])
            run(["codesign", "--force", "--sign", "-", str(out / filename)])
            print(filename, flush=True)


def run(command):
    subprocess.run(command, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["ffmpeg", "mspack", "overlay", "output"]:
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    build(parser.parse_args())
