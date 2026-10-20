# 逐軌量峰值 / RMS，驗「混音軌有兩邊、分軌各自只有一邊」。
# 用法: python track_levels.py <file.mp4>
import sys, numpy as np, av

path = sys.argv[1]
c = av.open(path)
print(f"{path}")
for i, st in enumerate(c.streams.audio):
    c2 = av.open(path)
    t2 = c2.streams.audio[i]
    chunks = []
    for frame in c2.decode(t2):
        a = frame.to_ndarray()
        if a.ndim > 1:
            a = a.reshape(-1) if a.shape[0] > a.shape[1] else a.mean(axis=0)
        chunks.append(a.astype(np.float32))
    pcm = np.concatenate(chunks) if chunks else np.zeros(0, np.float32)
    if pcm.size and np.abs(pcm).max() > 2.0:
        pcm /= 32768.0
    if pcm.size == 0:
        print(f"  track {i}: EMPTY")
        continue
    peak = float(np.abs(pcm).max())
    rms = float(np.sqrt(np.mean(pcm * pcm)))
    pdb = 20 * np.log10(peak) if peak > 1e-9 else -999.0
    rdb = 20 * np.log10(rms) if rms > 1e-9 else -999.0
    print(f"  track {i}: {pcm.size / t2.rate:6.2f}s  peak={pdb:7.1f} dBFS  rms={rdb:7.1f} dBFS")
