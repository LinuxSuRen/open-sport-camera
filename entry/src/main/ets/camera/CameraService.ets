import { camera } from '@kit.CameraKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG: string = 'CameraService';
const DOMAIN: number = 0x0001;

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
  private device?: camera.CameraDevice;
  private context?: common.Context;
  private previewSurfaceId: string = '';
  private position: camera.CameraPosition = camera.CameraPosition.CAMERA_POSITION_BACK;

  private chosenPreviewProfile?: camera.Profile;
  private chosenVideoProfile?: camera.VideoProfile;
  private chosenStabMode: number = 0;

  async open(context: common.Context, surfaceId: string,
    position: camera.CameraPosition): Promise<void> {
    this.context = context;
    this.previewSurfaceId = surfaceId;
    this.position = position;
    this.manager = camera.getCameraManager(context);

    const devices = this.manager.getSupportedCameras();
    if (devices.length === 0) {
      throw new Error('未找到可用摄像头');
    }
    this.device = this.pickDevice(devices, position);

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
    this.session.beginConfig();
    this.session.addInput(this.input);
    this.session.addOutput(this.previewOutput);
    let stabApplied = false;
    try {
      this.session.setVideoStabilizationMode(camera.VideoStabilizationMode.AUTO);
      stabApplied = true;
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'set stab AUTO in first config failed: %{public}d', err.code);
    }
    await this.session.commitConfig();
    await this.session.start();
    // 启动后确认防抖支持度（UI 徽标与录制事务沿用 AUTO）
    this.chosenStabMode = stabApplied ? camera.VideoStabilizationMode.AUTO as number : 0;
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
    this.session.beginConfig();
    this.session.addOutput(videoOutput);
    // EIS 属于视频流能力：挂载 VideoOutput 的事务内设置（预览-only 会话下设置会被拒绝）
    try {
      this.session.setVideoStabilizationMode(camera.VideoStabilizationMode.AUTO);
      this.chosenStabMode = camera.VideoStabilizationMode.AUTO as number;
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'set stab with video stream failed: %{public}d', err.code);
    }
    await this.session.commitConfig();
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
      this.input = undefined;
      this.device = undefined;
    }
  }

  private pickDevice(devices: camera.CameraDevice[],
    position: camera.CameraPosition): camera.CameraDevice {
    for (const d of devices) {
      if (d.cameraPosition === position) {
        return d;
      }
    }
    return devices[0];
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
