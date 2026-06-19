"""Transcription backends behind a small interface (D-OPEN-3 stays configurable).

Default: faster-whisper (pip, runs CPU/Metal, zero compile). Alternative:
whisper.cpp via subprocess, to reuse the Control-Plane-delivered binary. Both
take a WAV path so the same plumbing serves either; the app writes a temp WAV
per utterance from the PCM payload.
"""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Protocol

from .config import TranscribeConfig


class Transcriber(Protocol):
    def transcribe_file(self, wav_path: Path) -> str: ...


class NullTranscriber:
    """No-op backend (engine = "none"): stores empty transcripts.

    Lets the transport + storage path be exercised end-to-end on the Mac without
    installing a whisper backend (build plan step 2).
    """

    def transcribe_file(self, wav_path: Path) -> str:
        return ""


class FasterWhisperTranscriber:
    """faster-whisper (CTranslate2). Model is loaded once, lazily."""

    def __init__(self, model: str, language: str) -> None:
        self.model_name = model
        self.language = language
        self._model = None

    def _ensure_model(self):
        if self._model is None:
            from faster_whisper import WhisperModel  # lazy: optional dependency

            self._model = WhisperModel(self.model_name, compute_type="int8")
        return self._model

    def transcribe_file(self, wav_path: Path) -> str:
        model = self._ensure_model()
        segments, _info = model.transcribe(str(wav_path), language=self.language)
        return " ".join(seg.text.strip() for seg in segments).strip()


class WhisperCppTranscriber:
    """whisper.cpp via subprocess. Reuses a CP-delivered binary + model file."""

    def __init__(self, binary: str, model: str, language: str) -> None:
        self.binary = binary
        self.model = model
        self.language = language

    def transcribe_file(self, wav_path: Path) -> str:
        # -nt: no timestamps (plain text to stdout). Adjust flags to your build.
        result = subprocess.run(
            [self.binary, "-m", self.model, "-l", self.language, "-nt", "-f", str(wav_path)],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip()


def build_transcriber(config: TranscribeConfig) -> Transcriber:
    if config.engine == "none":
        return NullTranscriber()
    if config.engine == "faster-whisper":
        return FasterWhisperTranscriber(config.model, config.language)
    if config.engine == "whisper-cpp":
        if not config.whisper_cpp_bin:
            raise ValueError("transcribe.whisper_cpp_bin is required for the whisper-cpp engine")
        return WhisperCppTranscriber(config.whisper_cpp_bin, config.model, config.language)
    raise ValueError(f"unknown transcribe engine: {config.engine!r}")
