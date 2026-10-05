import { camera } from '@kit.CameraKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG: string = 'CameraService';
const DOMAIN: number = 0x0001;

/** 可切换镜头（多摄手机：主摄/超广/长焦） */
export interface LensInfo {
  cameraId: string;
  /** camera.CameraType 数值：0 主摄 1 广角 2 超广 3 长焦 */
  type: number;
  label: string;
}

export interface VideoProfileInfo {
  width: number;
  height: number;
  frameRate: number;
  /** 实际启用的防抖模式：0 关闭；1 LOW 2 MIDDLE 3 HIGH 4 AUTO */
  stabilizationMode: number;
}

/**
 * 相机会话封装：
 * - 后置优先打开，预览流绑定 XComponent surface
 * - 视频 profile 选择 1080p30；会话级启用 EIS 防抖（按设备支持度 AUTO→HIGH→MIDDLE→LOW 降级）
 * - 录制时动态挂载/卸载 VideoOutput（AVRecorder 的 surface）
 */
export class CameraService {
  private manager?: camera.CameraManager;
  private session?: camera.VideoSession;
  private input?: camera.CameraInput;
  private previewOutput?: camera.PreviewOutput;
  /** 常挂视频流（实时管线：开机挂载，永不重配） */
  private persistentVideoOutput?: camera.VideoOutput;
  private device?: camera.CameraDevice;
  private context?: common.Context;
  private previewSurfaceId: string = '';
  private position: camera.CameraPosition = camera.CameraPosition.CAMERA_POSITION_BACK;

  private chosenPreviewProfile?: camera.Profile;
  private chosenVideoProfile?: camera.VideoProfile;
  private chosenStabMode: number = 0;

  async open(context: common.Context, surfaceId: string,
    position: camera.CameraPosition, cameraId?: string,
    videoSurfaceId?: string): Promise<void> {
    this.context = context;
    this.previewSurfaceId = surfaceId;
    this.position = position;
    this.manager = camera.getCameraManager(context);

    const devices = this.manager.getSupportedCameras();
    if (devices.length === 0) {
      throw new Error('未找到可用摄像头');
    }
    if (cameraId !== undefined) {
      // 指定镜头精确切换
      for (const d of devices) {
        if (d.cameraId === cameraId) {
          this.device = d;
          break;
        }
      }
    }
    if (this.device === undefined) {
      this.device = this.pickDevice(devices, position);
    }

    this.input = this.manager.createCameraInput(this.device);
    await this.input.open();

    const capability = this.manager.getSupportedOutputCapability(this.device);
    this.chosenPreviewProfile = this.pickPreviewProfile(capability.previewProfiles);
    this.chosenVideoProfile = this.pickVideoProfile(capability.videoProfiles);

    this.previewOutput = this.manager.createPreviewOutput(
      this.chosenPreviewProfile, surfaceId);

    // NORMAL_VIDEO 场景的 VideoSession，支持会话级防抖设置。
    // 实测约束（SGT-AL50 / API24）：setVideoStabilizationMode 只能在首个
    // begin/commit 事务内调用；提交后另开事务设置会抛 7400103；
    // isVideoStabilizationModeSupported 必须在事务外（已提交态）调用。
    this.session = this.manager.createSession<camera.VideoSession>(camera.SceneMode.NORMAL_VIDEO);
    // 首个事务保持绝对干净：输入 + 预览输出 +（可选）常挂视频流。
    // 视频流开机常挂是实时管线的核心：相机拓扑此后永不变，
    // 根治动态挂流引发的预览冻结与录制花屏。
    this.session.beginConfig();
    this.session.addInput(this.input);
    this.session.addOutput(this.previewOutput);
    if (videoSurfaceId !== undefined && videoSurfaceId.length > 0) {
      this.persistentVideoOutput = this.manager.createVideoOutput(this.chosenVideoProfile,
        videoSurfaceId);
      this.session.addOutput(this.persistentVideoOutput);
    }
    await this.session.commitConfig();
    await this.session.start();
    if (this.persistentVideoOutput !== undefined) {
      await this.persistentVideoOutput.start();
    }
    // EIS 探测：在已提交的空闲态调用（无活事务，失败不损伤流状态）。
    // 真机教训：活事务内任何失败调用都会给流留下半损坏状态（预览冻结）。
    this.chosenStabMode = 0;
    // 诊断开关：EIS 在本设备固件上疑似使视频流停帧，暂关
    const eisEnabled = false;
    if (eisEnabled) {
      try {
        this.session.setVideoStabilizationMode(camera.VideoStabilizationMode.AUTO);
        this.chosenStabMode = camera.VideoStabilizationMode.AUTO as number;
      } catch (e) {
        hilog.info(DOMAIN, TAG, 'EIS not available on this device (outside-transaction probe)');
      }
    }
    hilog.info(DOMAIN, TAG,
      'camera session started, preview=%{public}dx%{public}d stab=%{public}d',
      this.chosenPreviewProfile.size.width, this.chosenPreviewProfile.size.height, this.chosenStabMode);
  }

  /** 切换前后摄：重建会话 */
  async switchCamera(position: camera.CameraPosition): Promise<void> {
    if (!this.manager || !this.context) {
      return;
    }
    await this.releaseSession();
    await this.open(this.context, this.previewSurfaceId, position);
  }

  getCameraPosition(): camera.CameraPosition {
    return this.position;
  }

  getPreviewSize(): camera.Size {
    if (this.chosenPreviewProfile) {
      return this.chosenPreviewProfile.size;
    }
    const s: camera.Size = { width: 1920, height: 1080 } as camera.Size;
    return s;
  }

  getVideoProfileInfo(): VideoProfileInfo {
    if (!this.chosenVideoProfile) {
      const info: VideoProfileInfo = {
        width: 1920, height: 1080, frameRate: 30, stabilizationMode: 0
      } as VideoProfileInfo;
      return info;
    }
    const p = this.chosenVideoProfile;
    const fps = p.frameRateRange.max > 0 ? p.frameRateRange.max : 30;
    const info: VideoProfileInfo = {
      width: p.size.width,
      height: p.size.height,
      frameRate: fps,
      stabilizationMode: this.chosenStabMode,
    } as VideoProfileInfo;
    return info;
  }

  /** 用 AVRecorder 提供的 surface 创建 VideoOutput 并挂到当前会话 */
  async attachVideoSurface(recorderSurfaceId: string): Promise<camera.VideoOutput> {
    if (!this.manager || !this.session || !this.chosenVideoProfile) {
      throw new Error('相机未就绪');
    }
    const videoOutput = this.manager.createVideoOutput(this.chosenVideoProfile, recorderSurfaceId);
    // 挂流事务保持纯净：仅 addOutput。任何失败调用（如设备不支持的 EIS）都会损坏流。
    this.session.beginConfig();
    this.session.addOutput(videoOutput);
    await this.session.commitConfig();
    // 动态加流后必须重启会话，否则预览流停在旧协商上（真机实测画面冻结）
    await this.session.start();
    await videoOutput.start();
    return videoOutput;
  }

  /** 录制结束：从会话卸载并释放 VideoOutput */
  async detachVideoOutput(videoOutput: camera.VideoOutput): Promise<void> {
    if (!this.session) {
      return;
    }
    try {
      await videoOutput.stop();
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'videoOutput.stop err %{public}d', err.code);
    }
    try {
      this.session.beginConfig();
      this.session.removeOutput(videoOutput);
      await this.session.commitConfig();
      await videoOutput.release();
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'detach videoOutput err %{public}d', err.code);
    }
  }

  async releaseSession(): Promise<void> {
    try {
      if (this.session) {
        await this.session.stop();
        await this.session.release();
      }
      if (this.previewOutput) {
        await this.previewOutput.release();
      }
      if (this.input) {
        await this.input.close();
      }
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'release err %{public}d', err.code);
    } finally {
      this.session = undefined;
      this.previewOutput = undefined;
      this.persistentVideoOutput = undefined;
      this.input = undefined;
      this.device = undefined;
    }
  }

  /**
   * 选型规则（多摄安全）：
   * - 仅内置摄像头；排除 TRUE_DEPTH（深度传感器不能录视频）
   * - 同位置多镜头时按 主摄 > 广角 > 超广 > 长焦 优先
   */
  private pickDevice(devices: camera.CameraDevice[],
    position: camera.CameraPosition): camera.CameraDevice {
    const typeScore = (t: camera.CameraType): number => {
      if (t === camera.CameraType.CAMERA_TYPE_DEFAULT) {
        return 4;
      }
      if (t === camera.CameraType.CAMERA_TYPE_WIDE_ANGLE) {
        return 3;
      }
      if (t === camera.CameraType.CAMERA_TYPE_ULTRA_WIDE) {
        return 2;
      }
      if (t === camera.CameraType.CAMERA_TYPE_TELEPHOTO) {
        return 1;
      }
      return -1; // 深度等弃用
    };
    let best: camera.CameraDevice | undefined = undefined;
    let bestScore = -1;
    for (const d of devices) {
      if (d.cameraPosition !== position) {
        continue;
      }
      if (d.connectionType !== camera.ConnectionType.CAMERA_CONNECTION_BUILT_IN) {
        continue;
      }
      const score = typeScore(d.cameraType);
      if (score > bestScore) {
        bestScore = score;
        best = d;
      }
    }
    if (best !== undefined) {
      return best;
    }
    // 兜底：位置匹配的第一个（个别设备类型标注缺失）
    for (const d of devices) {
      if (d.cameraPosition === position) {
        return d;
      }
    }
    return devices[0];
  }

  /** 当前位置可用镜头列表（主摄/超广/长焦，排除深度） */
  getLensList(position?: camera.CameraPosition): LensInfo[] {
    if (!this.manager) {
      return [];
    }
    const pos = position ?? this.position;
    const devices = this.manager.getSupportedCameras();
    const list: LensInfo[] = [];
    for (const d of devices) {
      if (d.cameraPosition !== pos) {
        continue;
      }
      if (d.connectionType !== camera.ConnectionType.CAMERA_CONNECTION_BUILT_IN) {
        continue;
      }
      if (d.cameraType === camera.CameraType.CAMERA_TYPE_TRUE_DEPTH) {
        continue;
      }
      let label = '镜头';
      if (d.cameraType === camera.CameraType.CAMERA_TYPE_DEFAULT) {
        label = '1x';
      } else if (d.cameraType === camera.CameraType.CAMERA_TYPE_WIDE_ANGLE) {
        label = '广角';
      } else if (d.cameraType === camera.CameraType.CAMERA_TYPE_ULTRA_WIDE) {
        label = '0.6x';
      } else if (d.cameraType === camera.CameraType.CAMERA_TYPE_TELEPHOTO) {
        label = '长焦';
      }
      const info: LensInfo = { cameraId: d.cameraId, type: d.cameraType as number, label: label };
      list.push(info);
    }
    return list;
  }

  getCurrentCameraId(): string {
    return this.device !== undefined ? this.device.cameraId : '';
  }

  /** 切换到指定镜头（重建会话） */
  async switchLens(cameraId: string): Promise<void> {
    if (!this.manager || !this.context) {
      return;
    }
    await this.releaseSession();
    await this.open(this.context, this.previewSurfaceId, this.position, cameraId);
  }

  /** 预览 profile：16:9 优先、宽度不超过 1920 中最大 */
  private pickPreviewProfile(profiles: Array<camera.Profile>): camera.Profile {
    let best: camera.Profile | undefined = undefined;
    let bestScore = -1;
    for (const p of profiles) {
      const w = p.size.width;
      const h = p.size.height;
      const ratioOk = Math.abs(w / h - 16 / 9) < 0.02;
      let score = 0;
      if (ratioOk) {
        score += 1000;
      }
      if (w <= 1920) {
        score += w;
      }
      if (score > bestScore) {
        bestScore = score;
        best = p;
      }
    }
    if (best === undefined && profiles.length > 0) {
      best = profiles[0];
    }
    if (best === undefined) {
      throw new Error('无可用预览档位');
    }
    return best;
  }

  /** 视频 profile：1080p30 优先 */
  private pickVideoProfile(profiles: Array<camera.VideoProfile>): camera.VideoProfile {
    let best: camera.VideoProfile | undefined = undefined;
    let bestScore = -1;
    for (const p of profiles) {
      const w = p.size.width;
      const h = p.size.height;
      const fps = p.frameRateRange.max;
      const is1080 = Math.abs(w - 1920) <= 64 && Math.abs(h - 1080) <= 64;
      let score = 0;
      if (is1080) {
        score += 100000;
      }
      if (fps >= 24 && fps <= 30) {
        score += 10000;
      }
      score += w * h / 10000;
      if (score > bestScore) {
        bestScore = score;
        best = p;
      }
    }
    if (best === undefined && profiles.length > 0) {
      best = profiles[0];
    }
    if (best === undefined) {
      throw new Error('无可用录制档位');
    }
    return best;
  }
}
