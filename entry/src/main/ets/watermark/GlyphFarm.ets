import { componentSnapshot } from '@kit.ArkUI';
import { image } from '@kit.ImageKit';
import { WatermarkItem } from './WatermarkTypes';
import { BurnGlyph } from '../burn/BurnTypes';

/**
 * 字形/贴图栅格化农场。
 *
 * 作用：烧录前把计时字符图集（0-9 : . 空格 与 “LAP” 标签）和静态水印文本
 * 用 UI Text 按视频坐标系字号（px）渲染，再通过 componentSnapshot 逐节点
 * 截取为 PixelMap，读出 RGBA 缓冲交给 native 作为 OpenGL 纹理。
 *
 * 组件本身 1x1 裁剪隐藏，不参与可见布局；Text 的 fontSize 用 px 单位，
 * 保证快照像素尺寸与视频分辨率坐标一致。
 */
@Component
export struct GlyphFarm {
  /** 计时字符表（单字符） */
  @Prop chars: string[] = [];
  @Prop fontPx: number = 96;
  @Prop color: string = '#FFFFFF';
  @Prop outlineEnabled: boolean = true;
  @Prop showLapLabel: boolean = true;
  /** 静态文本水印（已启用项） */
  @Prop staticItems: WatermarkItem[] = [];
  /** 每个静态项的字号（px，视频坐标系），与 staticItems 一一对应 */
  @Prop staticFontPx: number[] = [];
  /** 挂载后把自身交给宿主页面，供烧录前调用 capture* 方法 */
  onReady: (farm: GlyphFarm) => void = (_f: GlyphFarm) => {
  };
  private prefix: string = 'osc_' + new Date().getTime();

  aboutToAppear(): void {
    this.onReady(this);
  }

  build() {
    Stack() {
      Row() {
        ForEach(this.chars, (c: string, i: number) => {
          Text(c)
            .id(this.prefix + '_c' + i)
            .fontSize(this.fontPx + 'px')
            .fontWeight(FontWeight.Bold)
            .fontColor(this.color)
            .textShadow(this.shadowOf(this.outlineEnabled, this.fontPx))
        }, (c: string, i: number) => this.prefix + '_c' + i)

        if (this.showLapLabel) {
          Text('LAP')
            .id(this.prefix + '_lap')
            .fontSize(this.fontPx + 'px')
            .fontWeight(FontWeight.Bold)
            .fontColor(this.color)
            .textShadow(this.shadowOf(this.outlineEnabled, this.fontPx))
            .margin({ left: this.fontPx * 0.3 + 'px' })
        }
      }

      Column() {
        ForEach(this.staticItems, (item: WatermarkItem, i: number) => {
          Text(item.text)
            .id(this.prefix + '_s' + i)
            .fontSize(this.staticFontPx.length > i ? this.staticFontPx[i] : 64 + 'px')
            .fontWeight(FontWeight.Medium)
            .fontColor(item.textColor)
            .opacity(item.opacity)
            .textShadow(this.shadowOf(item.outline, this.staticFontPx.length > i ? this.staticFontPx[i] : 64))
        }, (item: WatermarkItem, i: number) => this.prefix + '_s' + i + '_' + item.text)
      }
    }
    .hitTestBehavior(HitTestMode.None)
  }

  private shadowOf(outline: boolean, fontPx: number): ShadowOptions | ShadowOptions[] {
    const o = fontPx * 0.03;
    if (!outline) {
      return {
        radius: 1,
        color: '#66000000',
        offsetX: 1,
        offsetY: 1,
      } as ShadowOptions;
    }
    return [
      { radius: 0, offsetX: -o, offsetY: 0, color: '#E6000000' },
      { radius: 0, offsetX: o, offsetY: 0, color: '#E6000000' },
      { radius: 0, offsetX: 0, offsetY: -o, color: '#E6000000' },
      { radius: 0, offsetX: 0, offsetY: o, color: '#E6000000' },
    ] as ShadowOptions[];
  }

  /** 截取计时字符图集（含 LAP 标签，若有） */
  async captureGlyphs(): Promise<BurnGlyph[]> {
    const glyphs: BurnGlyph[] = [];
    for (let i = 0; i < this.chars.length; i++) {
      const pm = await componentSnapshot.get(this.prefix + '_c' + i);
      const g = await GlyphFarm.pmToGlyph(this.chars[i], pm);
      glyphs.push(g);
    }
    if (this.showLapLabel) {
      const pm = await componentSnapshot.get(this.prefix + '_lap');
      const g = await GlyphFarm.pmToGlyph('LAP', pm);
      glyphs.push(g);
    }
    return glyphs;
  }

  /** 截取静态文本水印贴图（与 staticItems 顺序一致） */
  async captureStatics(): Promise<BurnGlyph[]> {
    const outs: BurnGlyph[] = [];
    for (let i = 0; i < this.staticItems.length; i++) {
      const pm = await componentSnapshot.get(this.prefix + '_s' + i);
      const g = await GlyphFarm.pmToGlyph(this.staticItems[i].text, pm);
      outs.push(g);
    }
    return outs;
  }

  static async pmToGlyph(tag: string, pm: image.PixelMap): Promise<BurnGlyph> {
    const info = await pm.getImageInfo();
    const size = info.size.width * info.size.height * 4;
    const buf = new ArrayBuffer(size);
    await pm.readPixelsToBuffer(buf);
    return {
      char: tag,
      rgba: buf,
      width: info.size.width,
      height: info.size.height,
    } as BurnGlyph;
  }
}
