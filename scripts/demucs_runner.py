#!/usr/bin/env python3
"""
Smart Mixer - Demucs (HTDemucs) AI Stem Separation Runner
Runs Meta's Hybrid Transformer Demucs model to separate audio into:
- Vocal Stem (_vocal.wav)
- Beat/Drums Stem (_beat.wav)
- Inst/Bass/Other Stem (_inst.wav)
"""

import sys
import os
import shutil
import subprocess
from pathlib import Path

def run_demucs(input_audio_path, output_dir=None, model="htdemucs"):
    input_path = Path(input_audio_path).resolve()
    if not input_path.exists():
        print(f"[ERROR] Input file not found: {input_path}", flush=True)
        return False

    if output_dir is None:
        output_dir = input_path.parent / "demucs_stems"
    else:
        output_dir = Path(output_dir).resolve()

    output_dir.mkdir(parents=True, exist_ok=True)
    track_name = input_path.stem

    print(f"[DEMUCS] Starting AI separation for: {input_path.name}", flush=True)
    print(f"[DEMUCS] Model: {model} | Output: {output_dir}", flush=True)

    # Command: python -m demucs.separate -n htdemucs -o <output_dir> --filename "{track}/{stem}.{ext}" <input>
    cmd = [
        sys.executable, "-m", "demucs.separate",
        "-n", model,
        "-o", str(output_dir),
        "--filename", "{track}/{stem}.{ext}",
        str(input_path)
    ]

    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1
        )

        for line in proc.stdout:
            line_str = line.strip()
            if line_str:
                print(f"[DEMUCS] {line_str}", flush=True)

        proc.wait()
        if proc.returncode != 0:
            print(f"[ERROR] Demucs exited with code {proc.returncode}", flush=True)
            return False

        # Demucs output folder: <output_dir>/<model>/<track_name>/
        model_out = output_dir / model / track_name
        vocals_file = model_out / "vocals.wav"
        drums_file = model_out / "drums.wav"
        bass_file = model_out / "bass.wav"
        other_file = model_out / "other.wav"

        if not vocals_file.exists():
            print(f"[ERROR] Expected output not found: {vocals_file}", flush=True)
            return False

        # Final destination paths in output_dir:
        dst_vocal = output_dir / f"{track_name}_vocal.wav"
        dst_beat  = output_dir / f"{track_name}_beat.wav"
        dst_inst  = output_dir / f"{track_name}_inst.wav"

        # Copy vocals -> _vocal.wav
        shutil.copyfile(vocals_file, dst_vocal)

        # Merge drums + bass into _beat.wav, or drums -> _beat.wav
        # Using soundfile / wave / simple mix:
        import wave
        import numpy as np

        def read_wav(p):
            with wave.open(str(p), 'rb') as w:
                nch = w.getnchannels()
                sr = w.getframerate()
                nframes = w.getnframes()
                frames = w.readframes(nframes)
                data = np.frombuffer(frames, dtype=np.int16).astype(np.float32)
                return data, nch, sr

        def write_wav(p, data, nch, sr):
            clamped = np.clip(data, -32768, 32767).astype(np.int16)
            with wave.open(str(p), 'wb') as w:
                w.setnchannels(nch)
                w.setsampwidth(2)
                w.setframerate(sr)
                w.writeframes(clamped.tobytes())

        drums_data, nch, sr = read_wav(drums_file)
        bass_data, _, _ = read_wav(bass_file)
        other_data, _, _ = read_wav(other_file)

        min_len = min(len(drums_data), len(bass_data), len(other_data))
        drums_data = drums_data[:min_len]
        bass_data = bass_data[:min_len]
        other_data = other_data[:min_len]

        # Beat = Drums (pure percussion & rhythmic beat)
        beat_data = drums_data
        # Inst = Other + Bass (complete melodic & harmonic accompaniment)
        inst_data = other_data + bass_data

        write_wav(dst_beat, beat_data, nch, sr)
        write_wav(dst_inst, inst_data, nch, sr)

        print(f"[SUCCESS] AI Stems created successfully:", flush=True)
        print(f"  - Vocal: {dst_vocal}", flush=True)
        print(f"  - Beat:  {dst_beat}", flush=True)
        print(f"  - Inst:  {dst_inst}", flush=True)
        return True

    except Exception as e:
        print(f"[ERROR] Exception during Demucs separation: {e}", flush=True)
        return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python demucs_runner.py <input_audio_file> [output_dir]")
        sys.exit(1)

    in_file = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else None
    success = run_demucs(in_file, out_dir)
    sys.exit(0 if success else 1)

