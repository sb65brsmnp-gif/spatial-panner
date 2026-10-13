// Meter ballistics for the editor's displays. The engines report the peak
// since the last read; a host that processes in bursts (Logic) leaves some
// reads empty, so the raw values flicker between full and nothing. Each
// shown value holds its peak for a moment, then falls at a steady rate.
export class MeterBallistics {
  private shown: number[] = [];
  private heldUntil: number[] = [];

  constructor(private holdMs = 150, private fallDbPerSec = 30, private floorDb = -120) {}

  // `raw`: the peaks reported since the last call, in dBFS. Returns what to
  // show now (`nowMs`: a monotonic clock).
  update(raw: number[], nowMs: number): number[] {
    if (this.shown.length !== raw.length) {
      this.shown = raw.map((v) => Math.max(this.floorDb, v));
      this.heldUntil = raw.map(() => nowMs + this.holdMs);
      this.last = nowMs;
      return [...this.shown];
    }
    for (let i = 0; i < raw.length; ++i) {
      const v = Math.max(this.floorDb, raw[i]);
      if (v >= this.shown[i]) {
        this.shown[i] = v;
        this.heldUntil[i] = nowMs + this.holdMs;
      } else if (nowMs >= this.heldUntil[i]) {
        // Falling since the hold ended (or the last update, if later).
        const dt = Math.max(0, Math.min(0.5, (nowMs - Math.max(this.last, this.heldUntil[i])) / 1000));
        this.shown[i] = Math.max(v, this.shown[i] - this.fallDbPerSec * dt);
      }
    }
    this.last = nowMs;
    return [...this.shown];
  }

  private last = 0;
}
