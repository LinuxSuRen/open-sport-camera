import { WatermarkItem, WatermarkKind, WatermarkAnchor, WATERMARK_ANCHOR_NAMES } from './WatermarkTypes';
import { WatermarkStore } from './WatermarkStore';

/**
 * 水印自定义设置面板：
 * - 计时水印：开关 / 锚点 / 字号 / 边距 / 计圈行 / 颜色
 * - 文本水印：开关 / 内容 / 锚点 / 字号 / 边距 / 颜色 / 透明度
 * 修改即时生效并持久化，预览叠加层与烧录共用此配置。
 */
@Component
export struct SettingsSheet {
  @Link show: boolean;
  @State items: WatermarkItem[] = WatermarkStore.getItems();

  build() {
    Column() {
      // 标题栏
      Row() {
        Text('水印设置')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor('#FFFFFF')
        Blank()
        Button('完成')
          .backgroundColor('#FF5E14')
          .fontColor('#FFFFFF')
          .onClick(() => {
            this.show = false;
          })
      }
      .width('100%')
      .padding({ left: 20, right: 20, top: 16, bottom: 12 })

      Scroll() {
        Column() {
          ForEach(this.items, (item: WatermarkItem, idx: number) => {
            this.ItemEditor(item, idx)
          }, (item: WatermarkItem) => item.id)
        }
        .width('100%')
        .padding({ bottom: 32 })
      }
      .layoutWeight(1)
      .scrollBar(BarState.Off)
    }
    .width('100%')
    .height('72%')
    .backgroundColor('#121212')
    .borderRadius({ topLeft: 20, topRight: 20 })
  }

  @Builder
  ItemEditor(item: WatermarkItem, idx: number) {
    Column() {
      Row() {
        Text(item.kind === WatermarkKind.TIMER ? '游泳计时' : '文本水印')
          .fontSize(17)
          .fontWeight(FontWeight.Bold)
          .fontColor('#FFFFFF')
        Blank()
        Toggle({ type: ToggleType.Switch, isOn: item.enabled })
          .selectedColor('#FF5E14')
          .onChange((on: boolean) => {
            this.updateItem(idx, (it: WatermarkItem): WatermarkItem => {
              const n = this.clone(it);
              n.enabled = on;
              return n;
            });
          })
      }
      .width('100%')
      .padding({ left: 20, right: 20, top: 14, bottom: 6 })

      if (item.kind === WatermarkKind.TEXT) {
        Row() {
          TextInput({ text: item.text, placeholder: '水印文本' })
            .fontSize(16)
            .fontColor('#FFFFFF')
            .placeholderColor('#888888')
            .backgroundColor('#2A2A2A')
            .borderRadius(8)
            .height(44)
            .layoutWeight(1)
            .onChange((v: string) => {
              this.updateItem(idx, (it: WatermarkItem): WatermarkItem => {
                const n = this.clone(it);
                n.text = v;
                return n;
              });
            })
        }
        .width('100%')
        .padding({ left: 20, right: 20, top: 4, bottom: 8 })
      }

      if (item.kind === WatermarkKind.TIMER) {
        this.LabeledToggle('显示计圈行', item.showLap, (on: boolean) => {
          this.updateItem(idx, (it: WatermarkItem): WatermarkItem => {
            const n = this.clone(it);
            n.showLap = on;
            return n;
          });
        })
      }

      this.AnchorRow(item, idx)

      this.SliderRow('字号', item.fontScale, 0.5, 3.0, (v: number) => {
        this.updateItem(idx, (it: WatermarkItem): WatermarkItem => {
          const n = this.clone(it);
          n.fontScale = Math.round(v * 10) / 10;
          return n;
        });
      }, item.fontScale.toFixed(1) + 'x')

      this.SliderRow('边距', item.margin, 0, 64, (v: number) => {
        this.updateItem(idx, (it: WatermarkItem): WatermarkItem => {
          const n = this.clone(it);
          n.margin = Math.round(v);
          return n;
        });
      }, item.margin + 'vp')

      this.ColorRow(item, idx)

      if (item.kind === WatermarkKind.TEXT) {
        this.SliderRow('透明度', item.opacity, 0.2, 1.0, (v: number) => {
          this.updateItem(idx, (it: WatermarkItem): WatermarkItem => {
            const n = this.clone(it);
            n.opacity = Math.round(v * 10) / 10;
            return n;
          });
        }, item.opacity.toFixed(1))
      }

      Divider().color('#333333').margin({ left: 16, right: 16, top: 10 })
    }
    .width('100%')
  }

  @Builder
  LabeledToggle(label: string, on: boolean, cb: (v: boolean) => void) {
    Row() {
      Text(label).fontSize(15).fontColor('#CCCCCC')
      Blank()
      Toggle({ type: ToggleType.Switch, isOn: on })
        .selectedColor('#FF5E14')
        .onChange(cb)
    }
    .width('100%')
    .padding({ left: 20, right: 20, top: 6, bottom: 6 })
  }

  @Builder
  AnchorRow(item: WatermarkItem, idx: number) {
    Column() {
      Text('位置').fontSize(15).fontColor('#CCCCCC')
      Grid() {
        ForEach(WATERMARK_ANCHOR_NAMES, (name: string, ai: number) => {
          GridItem() {
            Text(name)
              .fontSize(13)
              .fontColor(item.anchor === ai ? '#FFFFFF' : '#999999')
              .backgroundColor(item.anchor === ai ? '#FF5E14' : '#2A2A2A')
              .borderRadius(14)
              .padding({ left: 14, right: 14, top: 6, bottom: 6 })
              .onClick(() => {
                this.updateItem(idx, (it: WatermarkItem): WatermarkItem => {
                  const n = this.clone(it);
                  n.anchor = ai;
                  return n;
                });
              })
          }
        }, (name: string) => name)
      }
      .columnsTemplate('1fr 1fr 1fr')
      .columnsGap(10)
      .rowsGap(10)
      .width('100%')
      .height(76)
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding({ left: 20, right: 20, top: 6, bottom: 6 })
  }

  @Builder
  SliderRow(label: string, value: number, min: number, max: number,
    cb: (v: number) => void, valueText: string) {
    Row() {
      Text(label).fontSize(15).fontColor('#CCCCCC')
      Slider({ value: value, min: min, max: max, step: 0.1 })
        .layoutWeight(1)
        .selectedColor('#FF5E14')
        .blockColor('#FF5E14')
        .margin({ left: 12, right: 12 })
        .onChange(cb)
      Text(valueText).fontSize(13).fontColor('#888888').width(52)
    }
    .width('100%')
    .padding({ left: 20, right: 20, top: 6, bottom: 6 })
  }

  @Builder
  ColorRow(item: WatermarkItem, idx: number) {
    Column() {
      Text('颜色').fontSize(15).fontColor('#CCCCCC')
      Row() {
        ForEach(['#FFFFFF', '#FFD200', '#00E5FF', '#FF5E14', '#000000'], (c: string) => {
          Text('')
            .width(30)
            .height(30)
            .borderRadius(15)
            .backgroundColor(c)
            .border({
              width: item.textColor === c ? 3 : 1,
              color: item.textColor === c ? '#FF5E14' : '#555555'
            })
            .margin({ right: 14, top: 8, bottom: 8 })
            .onClick(() => {
              this.updateItem(idx, (it: WatermarkItem): WatermarkItem => {
                const n = this.clone(it);
                n.textColor = c;
                return n;
              });
            })
        }, (c: string) => c)
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding({ left: 20, right: 20, top: 6, bottom: 6 })
  }

  private updateItem(idx: number, mutator: (it: WatermarkItem) => WatermarkItem): void {
    const next: WatermarkItem[] = [];
    for (let i = 0; i < this.items.length; i++) {
      if (i === idx) {
        next.push(mutator(this.items[i]));
      } else {
        next.push(this.items[i]);
      }
    }
    this.items = next;
    WatermarkStore.setItems(next);
  }

  private clone(it: WatermarkItem): WatermarkItem {
    const n: WatermarkItem = {
      id: it.id,
      kind: it.kind,
      enabled: it.enabled,
      anchor: it.anchor,
      margin: it.margin,
      fontScale: it.fontScale,
      textColor: it.textColor,
      outline: it.outline,
      opacity: it.opacity,
      text: it.text,
      showLap: it.showLap,
    };
    return n;
  }
}
