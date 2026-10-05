import { WatermarkItem, WatermarkKind, WatermarkAnchor } from './WatermarkTypes';

const BASE_FONT_VP: number = 18;

/**
 * 水印预览叠加层：与烧录层共用 WatermarkItem 配置（所见即所得）。
 * 锚点定位：position 定锚边，translate 做自身尺寸反向偏移（居中/贴边）。
 * 注：按水印类型拆成无条件的专用 @Builder（条件语句在 @Builder 内
 * 与参数化调用组合时存在 ets 转换限制）。
 */
@Component
export struct WatermarkOverlay {
  @Prop items: WatermarkItem[] = [];
  @Prop timerTotalText: string = '00:00.00';
  @Prop timerLapText: string = '';

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      ForEach(this.items, (item: WatermarkItem) => {
        if (item.enabled) {
          if (item.kind === WatermarkKind.TIMER) {
            this.TimerSlot(item)
          } else {
            this.TextSlot(item)
          }
        }
      }, (item: WatermarkItem) => item.id + '/' + item.kind + '/' + item.anchor + '/' + item.enabled + '/' + item.text)
    }
    .width('100%')
    .height('100%')
    .hitTestBehavior(HitTestMode.None)
  }

  @Builder
  TimerSlot(item: WatermarkItem) {
    Column() {
      Text(this.timerTotalText)
        .fontSize(BASE_FONT_VP * item.fontScale)
        .fontWeight(FontWeight.Bold)
        .fontColor(item.textColor)
        .opacity(item.opacity)
        .textShadow(this.shadowOf(item.outline))
      if (item.showLap && this.timerLapText.length > 0) {
        Text(this.timerLapText)
          .fontSize(BASE_FONT_VP * item.fontScale * 0.6)
          .fontWeight(FontWeight.Medium)
          .fontColor(item.textColor)
          .opacity(item.opacity)
          .textShadow(this.shadowOf(item.outline))
          .margin({ top: 2 })
      }
    }
    .alignItems(HorizontalAlign.Start)
    .position(this.posOf(item))
    .translate(this.translateOf(item))
  }

  @Builder
  TextSlot(item: WatermarkItem) {
    Column() {
      Text(item.text)
        .fontSize(BASE_FONT_VP * item.fontScale)
        .fontWeight(FontWeight.Medium)
        .fontColor(item.textColor)
        .opacity(item.opacity)
        .textShadow(this.shadowOf(item.outline))
    }
    .alignItems(HorizontalAlign.Start)
    .position(this.posOf(item))
    .translate(this.translateOf(item))
  }

  /** 水平锚：左=m 中=50% 右=calc(100%-m)；垂直锚：上=m 下=100% */
  private posOf(item: WatermarkItem): Position {
    const m = item.margin;
    const isTop = item.anchor === WatermarkAnchor.TOP_LEFT ||
      item.anchor === WatermarkAnchor.TOP_CENTER || item.anchor === WatermarkAnchor.TOP_RIGHT;
    let x: string = m + 'vp';
    if (item.anchor === WatermarkAnchor.TOP_CENTER || item.anchor === WatermarkAnchor.BOTTOM_CENTER) {
      x = '50%';
    } else if (item.anchor === WatermarkAnchor.TOP_RIGHT || item.anchor === WatermarkAnchor.BOTTOM_RIGHT) {
      x = 'calc(100% - ' + m + 'vp)';
    }
    const y: string = isTop ? m + 'vp' : '100%';
    const pos: Position = { x: x, y: y } as Position;
    return pos;
  }

  /** 水平：中=-50% 右=-100%；垂直：下=-100%-m */
  private translateOf(item: WatermarkItem): TranslateOptions {
    const m = item.margin;
    const isTop = item.anchor === WatermarkAnchor.TOP_LEFT ||
      item.anchor === WatermarkAnchor.TOP_CENTER || item.anchor === WatermarkAnchor.TOP_RIGHT;
    let tx: string = '0vp';
    if (item.anchor === WatermarkAnchor.TOP_CENTER || item.anchor === WatermarkAnchor.BOTTOM_CENTER) {
      tx = '-50%';
    } else if (item.anchor === WatermarkAnchor.TOP_RIGHT || item.anchor === WatermarkAnchor.BOTTOM_RIGHT) {
      tx = '-100%';
    }
    const ty: string = isTop ? '0vp' : 'calc(-100% - ' + m + 'vp)';
    const t: TranslateOptions = { x: tx, y: ty } as TranslateOptions;
    return t;
  }

  private shadowOf(outline: boolean): ShadowOptions | ShadowOptions[] {
    if (!outline) {
      const s: ShadowOptions = {
        radius: 1,
        color: '#66000000',
        offsetX: 1,
        offsetY: 1,
      } as ShadowOptions;
      return s;
    }
    const o = 1.5;
    const arr: ShadowOptions[] = [
      { radius: 0, offsetX: -o, offsetY: 0, color: '#E6000000' } as ShadowOptions,
      { radius: 0, offsetX: o, offsetY: 0, color: '#E6000000' } as ShadowOptions,
      { radius: 0, offsetX: 0, offsetY: -o, color: '#E6000000' } as ShadowOptions,
      { radius: 0, offsetX: 0, offsetY: o, color: '#E6000000' } as ShadowOptions,
    ];
    return arr;
  }
}
