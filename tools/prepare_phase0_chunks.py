#!/usr/bin/env python3
import argparse
import json
import os
import struct
from pathlib import Path


def parse_wav_header(path: Path):
    with path.open("rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[0:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise RuntimeError("Not a RIFF/WAVE file")

        fmt = None
        data_offset = None
        data_size = None

        while True:
            chunk = f.read(8)
            if len(chunk) < 8:
                break
            cid, csize = struct.unpack("<4sI", chunk)
            cstart = f.tell()
            if cid == b"fmt ":
                raw = f.read(csize)
                if len(raw) < 16:
                    raise RuntimeError("Invalid fmt chunk")
                audio_format, channels, sample_rate, byte_rate, block_align, bits_per_sample = struct.unpack(
                    "<HHIIHH", raw[:16]
                )
                fmt = {
                    "audio_format": audio_format,
                    "channels": channels,
                    "sample_rate": sample_rate,
                    "byte_rate": byte_rate,
                    "block_align": block_align,
                    "bits_per_sample": bits_per_sample,
                }
            elif cid == b"data":
                data_offset = cstart
                data_size = csize
                break
            else:
                f.seek(csize, os.SEEK_CUR)

            if csize % 2 == 1:
                f.seek(1, os.SEEK_CUR)

        if fmt is None or data_offset is None:
            raise RuntimeError("Missing fmt/data chunks")

        file_size = path.stat().st_size
        readable_data_size = max(0, file_size - data_offset)
        effective_data_size = min(data_size, readable_data_size)
        if effective_data_size <= 0:
            raise RuntimeError("No readable data bytes")

        return fmt, data_offset, effective_data_size


def write_wav_pcm(path: Path, fmt: dict, data: bytes):
    block_align = int(fmt["block_align"])
    if block_align <= 0:
        raise RuntimeError("Invalid block align")
    payload = len(data) - (len(data) % block_align)
    data = data[:payload]

    fmt_chunk = struct.pack(
        "<HHIIHH",
        int(fmt["audio_format"]),
        int(fmt["channels"]),
        int(fmt["sample_rate"]),
        int(fmt["byte_rate"]),
        int(fmt["block_align"]),
        int(fmt["bits_per_sample"]),
    )
    riff_size = 4 + (8 + len(fmt_chunk)) + (8 + len(data))

    with path.open("wb") as out:
        out.write(b"RIFF")
        out.write(struct.pack("<I", riff_size))
        out.write(b"WAVE")
        out.write(b"fmt ")
        out.write(struct.pack("<I", len(fmt_chunk)))
        out.write(fmt_chunk)
        out.write(b"data")
        out.write(struct.pack("<I", len(data)))
        out.write(data)


def main() -> int:
    p = argparse.ArgumentParser(description="Prepare Phase 0 WAV chunks and local manifest")
    p.add_argument("--input-wav", required=True, help="Source WAV path")
    p.add_argument("--out-dir", required=True, help="Output directory for chunk WAV files")
    p.add_argument("--manifest-out", required=True, help="Output local manifest JSON path")
    p.add_argument("--chunk-sec", type=float, default=20.0, help="Chunk duration seconds")
    p.add_argument("--count", type=int, default=30, help="Max number of chunks")
    p.add_argument("--start-sec", type=float, default=0.0, help="Start offset in source WAV")
    p.add_argument("--prefix", default="phase0", help="File id prefix")
    args = p.parse_args()

    input_wav = Path(args.input_wav)
    out_dir = Path(args.out_dir)
    manifest_out = Path(args.manifest_out)

    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_out.parent.mkdir(parents=True, exist_ok=True)

    fmt, data_offset, data_size = parse_wav_header(input_wav)
    framerate = int(fmt["sample_rate"])
    block_align = int(fmt["block_align"])
    total_frames = data_size // block_align

    chunk_frames = max(1, int(round(args.chunk_sec * framerate)))
    start_frame = max(0, int(round(args.start_sec * framerate)))

    files = []
    made = 0
    with input_wav.open("rb") as f:
        for i in range(args.count):
            s = start_frame + i * chunk_frames
            if s >= total_frames:
                break
            e = min(total_frames, s + chunk_frames)
            length = e - s
            if length <= 0:
                break

            byte_start = data_offset + s * block_align
            byte_len = length * block_align
            f.seek(byte_start)
            data = f.read(byte_len)
            if len(data) <= 0:
                break

            file_id = f"{args.prefix}_{i+1:03d}"
            out_wav = out_dir / f"{file_id}.wav"
            write_wav_pcm(out_wav, fmt, data)

            files.append(
                {
                    "file_id": file_id,
                    "wav_path": str(out_wav).replace("\\", "/"),
                    "duration_sec": round(length / framerate, 6),
                    "annotations": [],
                }
            )
            made += 1

    manifest = {
        "dataset_name": "jndb_gold_phase0_local_chunks",
        "version": "0.1",
        "notes": "Auto-generated local chunk dataset. Fill annotations before final baseline.",
        "files": files,
    }

    with manifest_out.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print("Chunk preparation completed")
    print(f"- source: {input_wav}")
    print(f"- chunks: {made}")
    print(f"- out_dir: {out_dir}")
    print(f"- manifest: {manifest_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
