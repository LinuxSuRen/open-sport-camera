/**
 * 水印系统数据模型。
 *
 * 所有水印项（计时、文本、日期、Logo……）统一为 WatermarkItem，
 * 由 WatermarkOverlay 在预览层实时渲染；烧录引擎按相同模型合成进视频。
 * 新增水印类型 = 新增一个 WatermarkKind + 对应渲染组件 + 烧录侧绘制实现。
 */

export enum WatermarkKind {
  /** 游泳计时：总时长 + 可选计圈行 */
  TIMER = 'timer',
  /** 自定义文本 */
  TEXT = 'text',
}

/** 九宫格锚点 */
export enum WatermarkAnchor {
  TOP_LEFT = 0,
  TOP_CENTER = 1,
  TOP_RIGHT = 2,
  BOTTOM_LEFT = 3,
  BOTTOM_CENTER = 4,
  BOTTOM_RIGHT = 5,
}

export const WATERMARK_ANCHOR_NAMES: string[] = [
  '左上', '上中', '右上', '左下', '下中', '右下'
];

export interface WatermarkItem {
  id: string;
  kind: WatermarkKind;
  enabled: boolean;
  /** 锚点（九宫格） */
  anchor: WatermarkAnchor;
  /** 距边缘间距 vp */
  margin: number;
  /** 字号缩放（1 = 基准 18vp） */
  fontScale: number;
  /** 文字颜色 #RRGGBB */
  textColor: string;
  /** 是否黑色描边（浅色背景下保证可读） */
  outline: boolean;
  /** 不透明度 0~1 */
  opacity: number;
  /** TEXT 专属：内容 */
  text: string;
  /** TIMER 专属：是否显示计圈行 */
  showLap: boolean;
}

export function newTimerWatermark(): WatermarkItem {
  return {
    id: 'timer-default',
    kind: WatermarkKind.TIMER,
    enabled: true,
    anchor: WatermarkAnchor.TOP_LEFT,
    margin: 16,
    fontScale: 1.6,
    textColor: '#FFFFFF',
    outline: true,
    opacity: 1.0,
    text: '',
    showLap: true,
  };
}

export function newTextWatermark(): WatermarkItem {
  return {
    id: 'text-default',
    kind: WatermarkKind.TEXT,
    enabled: false,
    anchor: WatermarkAnchor.BOTTOM_RIGHT,
    margin: 16,
    fontScale: 0.9,
    textColor: '#FFFFFF',
    outline: true,
    opacity: 0.9,
    text: '@linuxsuren',
    showLap: false,
  };
}
