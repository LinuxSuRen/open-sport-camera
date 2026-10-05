import { media } from '@kit.MediaKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { VideoProfileInfo } from '../camera/CameraService';
import { abilityAccessCtrl } from '@kit.AbilityKit';

const TAG: string = 'RecorderService';
const DOMAIN: number = 0x0001;

export interface RecordSize {
  width: number;
  height: number;
}

/**
 * AVRecorder 录制封装：
 * - 输出到应用沙箱 mp4（H.264 + AAC）
 * - 视频 surface 由 getSurfaceId() 提供给相机会话挂载
 * - rotation=90 适配竖屏拍摄（后置横置传感器）
 */
export class RecorderService {
  private static contextValue?: common.Context;

  /** 麦克风权限检查：失败按无权限处理（无声降级） */
  static async checkMic(): Promise<boolean> {
    try {
      const bundleCtx = RecorderService.contextValue;
      if (bundleCtx === undefined) {
        return false;
      }
      const atm = abilityAccessCtrl.createAtManager();
      const token = bundleCtx.applicationInfo.accessTokenId;
      const status = await atm.checkAccessToken(token, 'ohos.permission.MICROPHONE');
      return status === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED;
    } catch (e) {
      return false;
    }
  }

  private recorder?: media.AVRecorder;
  private fileFd: number = -1;
  private filePath: string = '';
  private videoHeightInProfile: number = 1080;
  private videoWidthInProfile: number = 1920;

  getFilePath(): string {
    return this.filePath;
  }

  getRecordingSize(): RecordSize {
    const size: RecordSize = { width: this.videoWidthInProfile, height: this.videoHeightInProfile };
    return size;
  }

  async prepare(context: common.Context, profile: VideoProfileInfo,
    rotation: number): Promise<string> {
    RecorderService.contextValue = context;
    const dir = context.cacheDir + '/record';
    if (!fs.accessSync(dir)) {
      fs.mkdirSync(dir, true);
    }
    const ts = new Date().getTime();
    this.filePath = dir + '/osc_' + ts + '.mp4';
    const file = fs.openSync(this.filePath,
      fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    this.fileFd = file.fd;

    this.videoWidthInProfile = profile.width;
    this.videoHeightInProfile = profile.height;

    // 麦克风未授权时降级为无声录制（游泳视频通常无需音频，且避免 start() 被拒）
    const micGranted = await RecorderService.checkMic();
    hilog.info(DOMAIN, TAG, 'mic granted: %{public}s', micGranted ? 'yes' : 'no');
    const profileCfg: media.AVRecorderProfile = {
      fileFormat: media.ContainerFormatType.CFT_MPEG_4,
      videoBitrate: 20000000,
      videoCodec: media.CodecMimeType.VIDEO_AVC,
      videoFrameWidth: profile.width,
      videoFrameHeight: profile.height,
      videoFrameRate: profile.frameRate,
    } as media.AVRecorderProfile;
    if (micGranted) {
      profileCfg.audioBitrate = 96000;
      profileCfg.audioChannels = 2;
      profileCfg.audioCodec = media.CodecMimeType.AUDIO_AAC;
      profileCfg.audioSampleRate = 48000;
    }
    const avConfig: media.AVRecorderConfig = {
      audioSourceType: micGranted ? media.AudioSourceType.AUDIO_SOURCE_TYPE_MIC : undefined,
      videoSourceType: media.VideoSourceType.VIDEO_SOURCE_TYPE_SURFACE_YUV,
      profile: profileCfg,
      url: 'fd://' + this.fileFd,
      rotation: rotation,
    } as media.AVRecorderConfig;

    this.recorder = await media.createAVRecorder();
    await this.recorder.prepare(avConfig);
    hilog.info(DOMAIN, TAG, 'recorder prepared: %{public}s rotation=%{public}d',
      this.filePath, rotation);
    return this.filePath;
  }

  async getSurfaceId(): Promise<string> {
    if (!this.recorder) {
      throw new Error('录制器未初始化');
    }
    return await this.recorder.getInputSurface();
  }

  async start(): Promise<void> {
    if (!this.recorder) {
      throw new Error('录制器未初始化');
    }
    await this.recorder.start();
    hilog.info(DOMAIN, TAG, 'recorder started');
  }

  async stop(): Promise<string> {
    if (!this.recorder) {
      throw new Error('录制器未初始化');
    }
    await this.recorder.stop();
    hilog.info(DOMAIN, TAG, 'recorder stopped: %{public}s', this.filePath);
    return this.filePath;
  }

  async release(): Promise<void> {
    if (this.recorder) {
      try {
        await this.recorder.release();
      } catch (e) {
        const err = e as BusinessError;
        hilog.warn(DOMAIN, TAG, 'recorder release err %{public}d', err.code);
      }
      this.recorder = undefined;
    }
    if (this.fileFd >= 0) {
      try {
        fs.closeSync(this.fileFd);
      } catch (e) {
        // fd 可能已被 recorder 释放
      }
      this.fileFd = -1;
    }
  }
}
