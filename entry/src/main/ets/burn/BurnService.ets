import burn from 'libburn.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BurnConfig, BurnGlyph } from './BurnTypes';

const TAG: string = 'BurnService';
const DOMAIN: number = 0x0001;

/** 烧录配置 JSON 结构（与 native mini_json 解析一一对应） */
interface GlyphMetaJson {
  char: string;
  width: number;
  height: number;
  bufferIndex: number;
}

interface StaticMetaJson {
  anchor: number;
  marginXPx: number;
  marginYPx: number;
  width: number;
  height: number;
  displayHeightPx: number;
  opacity: number;
  bufferIndex: number;
}

interface TimerJson {
  enabled: boolean;
  anchor: number;
  marginXPx: number;
  marginYPx: number;
  glyphHeightPx: number;
  chars: string;
  lapLabelIndex: number;
  showLap: boolean;
  startOffsetMs: number;
  laps: object[];
}

interface BurnCfgJson {
  videoWidth: number;
  videoHeight: number;
  rotation: number;
  timer: TimerJson;
  glyphs: GlyphMetaJson[];
  statics: StaticMetaJson[];
}

/**
 * 烧录调度：把 ArkTS 侧配置（JSON）+ 字形/贴图缓冲（ArrayBuffer 列表）
 * 交给 native 引擎（libburn.so）执行 解码→合成→编码。
 */
export class BurnService {
  /**
   * @param glyphs 计时字符图集（含 LAP 标签块）
   * @param onProgress 进度 0-100
   * @return 烧录产物路径（dstPath）
   */
  static async burn(srcPath: string, dstPath: string, cfg: BurnConfig,
    glyphs: BurnGlyph[], onProgress: (pct: number) => void): Promise<string> {
    // buffers 布局：先 glyphs，再 statics
    const buffers: ArrayBuffer[] = [];
    const glyphMeta: GlyphMetaJson[] = [];
    for (const g of glyphs) {
      buffers.push(g.rgba);
      const meta: GlyphMetaJson = {
        char: g.char,
        width: g.width,
        height: g.height,
        bufferIndex: buffers.length - 1,
      };
      glyphMeta.push(meta);
    }
    const staticsMeta: StaticMetaJson[] = [];
    for (const s of cfg.statics) {
      buffers.push(s.rgba);
      const meta: StaticMetaJson = {
        anchor: s.anchor,
        marginXPx: s.marginXPx,
        marginYPx: s.marginYPx,
        width: s.width,
        height: s.height,
        displayHeightPx: s.displayHeightPx,
        opacity: s.opacity,
        bufferIndex: buffers.length - 1,
      };
      staticsMeta.push(meta);
    }

    const timerJson: TimerJson = {
      enabled: cfg.timer.enabled,
      anchor: cfg.timer.anchor,
      marginXPx: cfg.timer.marginXPx,
      marginYPx: cfg.timer.marginYPx,
      glyphHeightPx: cfg.timer.glyphHeightPx,
      chars: cfg.timer.chars,
      lapLabelIndex: cfg.timer.lapLabelIndex,
      showLap: cfg.timer.showLap,
      startOffsetMs: cfg.timer.startOffsetMs,
      laps: cfg.timer.laps as object[],
    };
    const cfgJson: BurnCfgJson = {
      videoWidth: cfg.videoWidth,
      videoHeight: cfg.videoHeight,
      rotation: cfg.rotation,
      timer: timerJson,
      glyphs: glyphMeta,
      statics: staticsMeta,
    };

    const code = await burn.startBurn(srcPath, dstPath, JSON.stringify(cfgJson), buffers, onProgress);
    if (code !== 0) {
      hilog.error(DOMAIN, TAG, 'burn failed, code=%{public}d', code);
      throw new Error('水印烧录失败（' + code + '）');
    }
    return dstPath;
  }
}
