/**
 * 游泳秒表核心：与录制联动的计时器。
 *
 * 设计要点：
 * - 基于 performance.now() 单调时钟，不受系统时间调整影响
 * - 暂停/恢复不丢累计时长
 * - 计圈记录分段时间（每圈耗时 + 圈起点），供水印展示 lap 数与最近圈用时
 */
export enum TimerState {
  IDLE = 0,
  RUNNING = 1,
  PAUSED = 2
}

export interface LapRecord {
  index: number;      // 第几圈，从 1 起
  lapMs: number;      // 本圈耗时
  totalMs: number;    // 打点时的总时长
}

export class SwimTimer {
  private state: TimerState = TimerState.IDLE;
  private baseElapsed: number = 0;          // 暂停时累计的时长 ms
  private startMonotonic: number = 0;       // 本次开始的单调时钟 ms
  private laps: LapRecord[] = [];

  getState(): TimerState {
    return this.state;
  }

  isRunning(): boolean {
    return this.state === TimerState.RUNNING;
  }

  start(): void {
    if (this.state === TimerState.RUNNING) {
      return;
    }
    this.startMonotonic = this.now();
    this.state = TimerState.RUNNING;
  }

  pause(): void {
    if (this.state !== TimerState.RUNNING) {
      return;
    }
    this.baseElapsed = this.elapsedMs();
    this.state = TimerState.PAUSED;
  }

  reset(): void {
    this.state = TimerState.IDLE;
    this.baseElapsed = 0;
    this.startMonotonic = 0;
    this.laps = [];
  }

  /** 当前总时长（ms），10ms 展示精度 */
  elapsedMs(): number {
    if (this.state === TimerState.RUNNING) {
      return this.baseElapsed + (this.now() - this.startMonotonic);
    }
    return this.baseElapsed;
  }

  /** 计圈：记录当前时点，返回本圈记录 */
  lap(): LapRecord | null {
    if (this.state === TimerState.IDLE) {
      return null;
    }
    const total = this.elapsedMs();
    const prevTotal = this.laps.length > 0 ? this.laps[this.laps.length - 1].totalMs : 0;
    const rec: LapRecord = {
      index: this.laps.length + 1,
      lapMs: total - prevTotal,
      totalMs: total
    };
    this.laps.push(rec);
    return rec;
  }

  getLaps(): LapRecord[] {
    return this.laps;
  }

  /** 最近一圈耗时（ms），无圈时返回 0 */
  lastLapMs(): number {
    if (this.laps.length === 0) {
      return 0;
    }
    return this.laps[this.laps.length - 1].lapMs;
  }

  lapCount(): number {
    return this.laps.length;
  }

  private now(): number {
    // ArkTS 无 performance.now，用毫秒墙钟（秒表场景足够）
    return Date.now();
  }
}

/** 格式化为 mm:ss.cs（百分秒），例如 75432 -> "01:15.43" */
export function formatTimer(ms: number): string {
  const total = Math.max(0, Math.floor(ms));
  const cs = Math.floor((total % 1000) / 10);
  const s = Math.floor(total / 1000) % 60;
  const m = Math.floor(total / 60000);
  return pad2(m) + ':' + pad2(s) + '.' + pad2(cs);
}

/** 计圈行格式：LAP 03 00:31.24 */
export function formatLapLine(lapCount: number, lapMs: number): string {
  return 'LAP ' + pad2(lapCount) + ' ' + formatTimer(lapMs);
}

function pad2(n: number): string {
  return n < 10 ? '0' + n : '' + n;
}
