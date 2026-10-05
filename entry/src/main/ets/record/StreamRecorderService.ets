import burn from 'libburn.so';
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';

export interface StreamRecordStats {
  durationMs: number;
  frames: number;
}

/**
 * 实时录制管线（阶段1）：native 四段式封装。
 * 相机会话创建时即挂载视频流（prepareRecord 的 surfaceId），
 * 录制启停只控制编码器——相机拓扑永不变，根治动态重配引发的
 * 预览冻结与录制花屏。
 */
export class StreamRecorderService {
  private static prepared = false;
  private static recording = false;
  private static filePath = '';

  static isPrepared(): boolean {
    return StreamRecorderService.prepared;
  }

  static isRecording(): boolean {
    return StreamRecorderService.recording;
  }

  /** 相机会话创建前调用：返回视频流 surfaceId（传给相机 open 常挂） */
  static prepare(width: number, height: number, fps: number, bitrate: number,
    rotation: number): string {
    if (StreamRecorderService.prepared) {
      burn.releaseRecord();
      StreamRecorderService.prepared = false;
    }
    const sid = burn.prepareRecord(width, height, fps, bitrate, rotation);
    StreamRecorderService.prepared = true;
    return sid;
  }

  static getFilePath(): string {
    return StreamRecorderService.filePath;
  }

  static begin(context: common.Context): string {
    if (!StreamRecorderService.prepared) {
      throw new Error('实时管线未就绪');
    }
    const dir = context.cacheDir + '/record';
    if (!fs.accessSync(dir)) {
      fs.mkdirSync(dir, true);
    }
    const ts = new Date().getTime();
    StreamRecorderService.filePath = dir + '/osc_' + ts + '.mp4';
    const code = burn.beginRecord(StreamRecorderService.filePath);
    if (code !== 0) {
      throw new Error('开始录制失败（' + code + '）');
    }
    StreamRecorderService.recording = true;
    return StreamRecorderService.filePath;
  }

  static async stop(): Promise<StreamRecordStats> {
    const stats = await burn.stopRecord();
    StreamRecorderService.recording = false;
    return stats as StreamRecordStats;
  }

  /** 阶段2：录制前设置水印资产（rgba 在 native 侧拷贝） */
  static setWatermark(cfgJson: string, buffers: ArrayBuffer[]): void {
    burn.setWatermarkAssets(cfgJson, buffers);
  }

  /** 切换摄像头时设置水平镜像 */
  static setCameraFlip(flip: boolean): void {
    burn.setCameraFlip(flip);
  }

  /** 录制中更新计圈 */
  static updateLaps(lapsJson: string): void {
    burn.updateWatermarkLaps(lapsJson);
  }

  static release(): void {
    if (StreamRecorderService.prepared) {
      burn.releaseRecord();
      StreamRecorderService.prepared = false;
      StreamRecorderService.recording = false;
    }
  }
}
