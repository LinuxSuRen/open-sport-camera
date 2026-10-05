/**
 * 烧录配置模型（ArkTS 侧）。
 *
 * 原则：UI 预览层与烧录层共用同一份 WatermarkItem 配置 + 同一套字号/锚点换算，
 * 保证“预览所见 = 烧录所得”。
 *
 * 计时水印的按帧绘制：录制中每帧的计时值 = startOffsetMs + 帧PTS。
 * native 侧用字符图集（0-9 : . 空格 + “LAP” 标签块）逐字拼出
 * “mm:ss.cs” 与 “LAP nn mm:ss.cs”，无需每帧回调 ArkTS。
 */

/** 单个字形（RGBA，来自 componentSnapshot 栅格化） */
export interface BurnGlyph {
  char: string;
  rgba: ArrayBuffer;
  width: number;
  height: number;
}

/** 静态水印贴图（文本水印整体栅格化为一张图） */
export interface BurnStaticImage {
  rgba: ArrayBuffer;
  width: number;
  height: number;
  /** 九宫格锚点 0-5 */
  anchor: number;
  marginXPx: number;
  marginYPx: number;
  /** 烧入视频中时期望的显示高度 px */
  displayHeightPx: number;
  opacity: number;
}

export interface BurnLapInfo {
  index: number;
  totalMs: number;
  lapMs: number;
}

export interface BurnTimerConfig {
  enabled: boolean;
  anchor: number;
  marginXPx: number;
  marginYPx: number;
  /** 图集字高（px，对应视频分辨率坐标系） */
  glyphHeightPx: number;
  /** 图集覆盖的字符表（与 glyphs 数组一一对应） */
  chars: string;
  /** LAP 标签块在图集数组中的下标，-1 表示无 */
  lapLabelIndex: number;
  showLap: boolean;
  /** 录制开始时秒表已有读数 ms（通常 0） */
  startOffsetMs: number;
  laps: BurnLapInfo[];
}

export interface BurnConfig {
  videoWidth: number;
  videoHeight: number;
  /** 视频旋转元数据（度） */
  rotation: number;
  timer: BurnTimerConfig;
  statics: BurnStaticImage[];
}

export interface BurnBufferRef {
  bufferIndex: number;
}
