import burn from 'libburn.so';
import { hilog } from '@kit.PerformanceAnalysisKit';

/**
 * RTSP 推流控制（阶段3）：局域网实时观看。
 * VLC/ffplay 打开 rtsp://<手机IP>:8554/live
 */
export class RtspService {
  private static started = false;

  static start(port: number): boolean {
    if (RtspService.started) {
      return true;
    }
    const code = burn.startRtsp(port);
    RtspService.started = code === 0;
    hilog.info(0x0001, 'RtspService', 'start rtsp port=%{public}d code=%{public}d', port, code);
    return RtspService.started;
  }

  static stop(): void {
    burn.stopRtsp();
    RtspService.started = false;
  }

  static isRunning(): boolean {
    return RtspService.started;
  }

  static status(): boolean {
    return RtspService.started;
  }
}
