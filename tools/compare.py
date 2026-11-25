"""Run with numpy and pedalboard. Optional argument: aligned float32 AAX render."""
import sys
from pathlib import Path
import numpy as np
from pedalboard import load_plugin
from pedalboard.io import AudioFile

rate = 48000
t = np.arange(rate, dtype=np.float64) / rate
tone = np.sin(2 * np.pi * 997 * t)
source = np.tile(np.concatenate([tone * a for a in (0.01, 0.1, 0.5)]).astype("float32"), (2, 1))
folder = Path("build/comparison")
folder.mkdir(parents=True, exist_ok=True)

def save(name, samples):
    with AudioFile(str(folder / name), "w", rate, 2, bit_depth=32) as f:
        f.write(samples)

save("input.wav", source)
plugin = load_plugin(str(next(Path.cwd().glob("build/**/x86_64-win/Peacock.vst3"))))
assert np.array_equal(plugin(source, rate), source)
for db in (-6, 6):
    plugin.output_trim = db
    out = plugin(source, rate)
    measured = 20 * np.log10(np.linalg.norm(out[:, rate:]) / np.linalg.norm(source[:, rate:]))
    print(f"Trim {db:+d}: {measured:.3f} dB")
    assert abs(measured - db) < 0.01
plugin.output_trim = 0
plugin.dither = 100
out = plugin(source, rate)
assert np.array_equal(out * 32768, np.round(out * 32768))
plugin.dither = 0
plugin.harmonic, plugin.dynamic, plugin.color = 60, 67, "Fat"
out = plugin(source, rate)
assert np.isfinite(out).all() and not np.array_equal(out, source)
save("vst3.wav", out)
if len(sys.argv) > 1:
    with AudioFile(sys.argv[1]) as f:
        if f.samplerate != rate or f.num_channels != 2 or f.frames != source.shape[1]:
            raise ValueError("Expected aligned stereo 48 kHz, exactly three seconds")
        ref = f.read(f.frames)
    diff = out.astype("float64") - ref
    print("Residual RMS / peak (dBFS):", 20 * np.log10([np.sqrt(np.mean(diff**2)), np.max(np.abs(diff))]))
