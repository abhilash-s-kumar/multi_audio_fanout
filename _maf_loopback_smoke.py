import threading
import time

import numpy as np
import soundcard as sc


speaker = next(s for s in sc.all_speakers() if "MAF Virtual Audio Output" in s.name)
microphone = next(
    m for m in sc.all_microphones(include_loopback=True) if "MAF Virtual Audio Output" in m.name
)

sample_rate = 48000
t = np.arange(sample_rate // 2) / sample_rate
tone = (0.2 * np.sin(2 * np.pi * 440 * t)).astype("float32")
stereo = np.column_stack([tone, tone])


def play() -> None:
    time.sleep(0.1)
    with speaker.player(samplerate=sample_rate, channels=2, blocksize=960) as player:
        player.play(stereo)


thread = threading.Thread(target=play)
thread.start()
with microphone.recorder(samplerate=sample_rate, channels=2, blocksize=960) as recorder:
    data = recorder.record(numframes=sample_rate)
thread.join()

print(
    "rms",
    float(np.sqrt(np.mean(data.astype("float64") ** 2))),
    "peak",
    float(np.max(np.abs(data))),
)
