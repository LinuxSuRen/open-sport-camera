import { common } from '@kit.AbilityKit';
import { WatermarkItem, newTimerWatermark, newTextWatermark } from './WatermarkTypes';

const STORE_KEY: string = 'watermark_items_json';

/**
 * 水印配置存储：PersistentStorage 持久化一个 JSON 数组，应用内通过 AppStorage 响应式共享。
 * UI 修改 -> setItems -> 触发订阅刷新 -> 烧录时读取同一份配置，保证“所见即所得”。
 */
export class WatermarkStore {
  private static items: WatermarkItem[] = [newTimerWatermark(), newTextWatermark()];
  private static inited: boolean = false;

  static init(context: common.Context): void {
    if (WatermarkStore.inited) {
      return;
    }
    WatermarkStore.inited = true;
    PersistentStorage.persistProp<string>(STORE_KEY, WatermarkStore.serialize(WatermarkStore.items));
    const saved = AppStorage.get<string>(STORE_KEY);
    if (saved !== undefined && saved.length > 0) {
      const parsed = WatermarkStore.deserialize(saved);
      if (parsed.length > 0) {
        WatermarkStore.items = parsed;
      }
    }
  }

  static getItems(): WatermarkItem[] {
    return WatermarkStore.items;
  }

  /** 返回启用项（渲染与烧录都只用启用项） */
  static getEnabled(): WatermarkItem[] {
    const result: WatermarkItem[] = [];
    for (const it of WatermarkStore.items) {
      if (it.enabled) {
        result.push(it);
      }
    }
    return result;
  }

  static setItems(items: WatermarkItem[]): void {
    WatermarkStore.items = items;
    AppStorage.setOrCreate<string>(STORE_KEY, WatermarkStore.serialize(items));
  }

  static updateItem(item: WatermarkItem): void {
    const next: WatermarkItem[] = [];
    for (const it of WatermarkStore.items) {
      if (it.id === item.id) {
        next.push(item);
      } else {
        next.push(it);
      }
    }
    WatermarkStore.setItems(next);
  }

  private static serialize(items: WatermarkItem[]): string {
    return JSON.stringify(items);
  }

  private static deserialize(json: string): WatermarkItem[] {
    const result: WatermarkItem[] = [];
    try {
      const arr: object[] = JSON.parse(json) as object[];
      for (const o of arr) {
        result.push(WatermarkStore.coerce(o));
      }
    } catch (e) {
      // 兼容旧数据失败则回退默认
    }
    return result;
  }

  private static coerce(o: object): WatermarkItem {
    const src = o as Record<string, Object>;
    const fallbackT = newTimerWatermark();
    const fallbackX = newTextWatermark();
    const kind = (src['kind'] as string) === 'text' ? 'text' : 'timer';
    const base = kind === 'text' ? fallbackX : fallbackT;
    const item: WatermarkItem = {
      id: (src['id'] as string) ?? base.id,
      kind: base.kind,
      enabled: src['enabled'] === true,
      anchor: (src['anchor'] as number) ?? base.anchor,
      margin: (src['margin'] as number) ?? base.margin,
      fontScale: (src['fontScale'] as number) ?? base.fontScale,
      textColor: (src['textColor'] as string) ?? base.textColor,
      outline: src['outline'] === true,
      opacity: (src['opacity'] as number) ?? base.opacity,
      text: (src['text'] as string) ?? base.text,
      showLap: src['showLap'] === true,
    };
    return item;
  }
}
