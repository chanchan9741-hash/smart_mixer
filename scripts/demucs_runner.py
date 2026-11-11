#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Smart Mixer - Direct PyTorch HTDemucs AI Stem Separation Runner
Zero dependency on ffmpeg / torchaudio / console encoding (cp949 safe).
Reads a standard 16-bit PCM WAV file prepared by the C++ Media Foundation decoder,
runs Meta's Hybrid Transformer Demucs (htdemucs) model directly in PyTorch,
reports real-time progress to <output_dir>/progress.txt,
and writes:
  - <track>_ai_vocal.wav
  - <track>_ai_beat.wav
  - <track>_ai_inst.wav
"""

import sys
import os
import wave
import traceback
from pathlib import Path


def write_progress(progress_file: Path, pct: int):
    try:
        progress_file.write_text(str(int(pct)), encoding="utf-8")
    except Exception:
        pass


def log_msg(log_file: Path, msg: str):
    try:
        with open(log_file, "a", encoding="utf-8", errors="replace") as f:
            f.write(msg + "\n")
    except Exception:
        pass


def read_wav_as_tensor(wav_path: Path):
    import numpy as np
    import torch

    with wave.open(str(wav_path), "rb") as w:
        nch = w.getnchannels()
        sr = w.getframerate()
        sw = w.getsampwidth()
        nframes = w.getnframes()
        raw = w.readframes(nframes)

    if sw == 2:
        data = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    elif sw == 4:
        data = np.frombuffer(raw, dtype=np.float32).copy()
    elif sw == 1:
        data = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    else:
        raise RuntimeError(f"Unsupported WAV sample width: {sw}")

    if nch == 1:
        stereo = np.stack([data, data], axis=0)
    else:
        reshaped = data.reshape(-1, nch)
        stereo = reshaped[:, :2].T.copy()  # shape: (2, T)

    return torch.from_numpy(stereo), sr


def write_stereo_wav(wav_path: Path, stereo_2xT, sr: int):
    import numpy as np

    interleaved = stereo_2xT.T.reshape(-1)
    pcm16 = np.clip(interleaved * 32767.0, -32768.0, 32767.0).astype(np.int16)
    with wave.open(str(wav_path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm16.tobytes())


def run_demucs(input_wav_path: str, output_dir_str: str, track_base_name: str = None, model_name: str = "htdemucs") -> bool:
    output_dir = Path(output_dir_str).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    log_file = output_dir / "demucs_log.txt"
    progress_file = output_dir / "progress.txt"

    try:
        log_file.write_text("", encoding="utf-8")
    except Exception:
        pass

    write_progress(progress_file, 2)
    log_msg(log_file, f"[DEMUCS] Starting HTDemucs separation")
    log_msg(log_file, f"[DEMUCS] Python executable: {sys.executable}")
    log_msg(log_file, f"[DEMUCS] Input WAV: {input_wav_path}")
    log_msg(log_file, f"[DEMUCS] Output Dir: {output_dir}")

    try:
        input_path = Path(input_wav_path).resolve()
        if not input_path.exists():
            log_msg(log_file, f"[ERROR] Input WAV file does not exist: {input_path}")
            return False

        if not track_base_name:
            stem = input_path.stem
            if stem.endswith("_demucs_input"):
                stem = stem[: -len("_demucs_input")]
            track_base_name = stem

        import numpy as np
        import torch
        from demucs.pretrained import get_model
        from demucs.apply import apply_model
        from demucs.audio import convert_audio

        num_threads = max(1, os.cpu_count() or 4)
        torch.set_num_threads(num_threads)
        device = "cuda" if torch.cuda.is_available() else "cpu"
        log_msg(log_file, f"[DEMUCS] Device: {device} (CPU threads: {num_threads})")

        write_progress(progress_file, 5)
        wav, sr = read_wav_as_tensor(input_path)
        total_frames = wav.shape[-1]
        log_msg(log_file, f"[DEMUCS] Loaded audio tensor: shape={tuple(wav.shape)}, sr={sr}")

        write_progress(progress_file, 10)
        model = get_model(model_name)
        model.eval()

        if sr != model.samplerate or wav.shape[0] != model.audio_channels:
            wav = convert_audio(wav, sr, model.samplerate, model.audio_channels)
            sr = model.samplerate
            total_frames = wav.shape[-1]

        ref = wav.mean(0)
        mean = ref.mean()
        std = ref.std() + 1e-8
        norm_wav = ((wav - mean) / std).unsqueeze(0)

        seg_sec = float(getattr(model, "segment", 7.8) or 7.8)
        seg_frames = int(seg_sec * sr)

        def on_progress(d):
            try:
                if d.get("state") == "end":
                    offset = int(d.get("segment_offset", 0))
                    done_ratio = min(1.0, float(offset + seg_frames) / max(1, total_frames))
                    pct = 12 + int(done_ratio * 80)
                    write_progress(progress_file, min(92, pct))
            except Exception:
                pass

        write_progress(progress_file, 12)
        log_msg(log_file, "[DEMUCS] Running neural network inference...")

        with torch.no_grad():
            out = apply_model(
                model,
                norm_wav,
                shifts=0,
                split=True,
                overlap=0.25,
                progress=False,
                device=device,
                callback=on_progress,
            )

        out = out * std + mean  # shape: (1, num_sources, 2, T)
        write_progress(progress_file, 94)

        sources = {}
        for idx, name in enumerate(model.sources):
            sources[name] = out[0, idx].cpu().numpy()

        vocal_np = sources["vocals"]
        beat_np = sources["drums"]
        inst_np = sources["other"] + sources["bass"]

        dst_vocal = output_dir / f"{track_base_name}_ai_vocal.wav"
        dst_beat = output_dir / f"{track_base_name}_ai_beat.wav"
        dst_inst = output_dir / f"{track_base_name}_ai_inst.wav"

        write_stereo_wav(dst_vocal, vocal_np, sr)
        write_progress(progress_file, 97)
        write_stereo_wav(dst_beat, beat_np, sr)
        write_progress(progress_file, 99)
        write_stereo_wav(dst_inst, inst_np, sr)
        write_progress(progress_file, 100)

        log_msg(log_file, "[SUCCESS] HTDemucs separation finished:")
        log_msg(log_file, f"  Vocal: {dst_vocal}")
        log_msg(log_file, f"  Beat:  {dst_beat}")
        log_msg(log_file, f"  Inst:  {dst_inst}")
        return True

    except Exception as e:
        log_msg(log_file, f"[ERROR] Exception: {e}\n{traceback.format_exc()}")
        return False


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(1)

    in_wav = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else str(Path(in_wav).resolve().parent / "demucs_stems")
    base_name = sys.argv[3] if len(sys.argv) > 3 else None

    ok = run_demucs(in_wav, out_dir, base_name)
    sys.exit(0 if ok else 1)
