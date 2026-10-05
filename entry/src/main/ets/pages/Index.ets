import { CameraService } from '../camera/CameraService';
import { RecorderService, RecordSize } from '../record/RecorderService';
import { StreamRecorderService } from '../record/StreamRecorderService';
import { RtspService } from '../record/RtspService';
import { LensInfo } from '../camera/CameraService';
import { SwimTimer, TimerState, formatTimer, formatLapLine, LapRecord } from '../timer/SwimTimer';
import { WatermarkOverlay } from '../watermark/WatermarkOverlay';
import { WatermarkStore } from '../watermark/WatermarkStore';
import { WatermarkItem, WatermarkKind } from '../watermark/WatermarkTypes';
import { SettingsSheet } from '../watermark/SettingsSheet';
import { GlyphFarm } from '../watermark/GlyphFarm';
import { BurnService } from '../burn/BurnService';
import { BurnGlyph, BurnStaticImage, BurnLapInfo } from '../burn/BurnTypes';
import { GalleryService } from '../gallery/GalleryService';
import { common } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import { abilityAccessCtrl, Permissions } from '@kit.AbilityKit';
import { camera } from '@kit.CameraKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG: string = 'Index';
const DOMAIN: number = 0x0001;

interface GlyphMetaObj {
  char: string;
  width: number;
  height: number;
  bufferIndex: number;
}

interface StaticMetaObj {
  anchor: number;
  marginXPx: number;
  marginYPx: number;
  width: number;
  height: number;
  displayHeightPx: number;
  opacity: number;
  bufferIndex: number;
}

interface WmTimerObj {
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

interface WmCfgObj {
  videoWidth: number;
  videoHeight: number;
  rotation: number;
  timer: WmTimerObj;
  glyphs: GlyphMetaObj[];
  statics: StaticMetaObj[];
}

type BurnPhase = 'idle' | 'raster' | 'wmRaster' | 'burning' | 'saving' | 'done' | 'error';

@Entry
@Component
struct Index {
  private xcController: XComponentController = new XComponentController();
  private cameraService: CameraService = new CameraService();
  private recorder: RecorderService = new RecorderService();
  private swimTimer: SwimTimer = new SwimTimer();
  private tickId: number = -1;
  private videoOutput?: camera.VideoOutput;
  private glyphFarm: GlyphFarm | null = null;
  private recStartTimerOffset: number = 0;
  private context: common.Context = this.getUIContext().getHostContext() as common.Context;
  /** 权限申请完成信号：相机打开前必须等待，避免权限弹窗未确认时开流失败 */
  private permsReady: Promise<void> = Promise.resolve();

  @State cameraReady: boolean = false;
  @State recording: boolean = false;
  @State recDurationText: string = '00:00';
  @State timerTotal: string = '00:00.00';
  @State timerLap: string = '';
  @State showSettings: boolean = false;
  @State watermarkItems: WatermarkItem[] = WatermarkStore.getItems();
  @State eisEnabled: boolean = false;
  @State profileText: string = '';
  @State frontCamera: boolean = false;
  @State lensList: LensInfo[] = [];
  @State currentLensId: string = '';
  /** 实验室开关：实时管线（阶段1：无水印直录；阶段2 起水印入流） */
  @State useStreamPipeline: boolean = true;
  @State rtspEnabled: boolean = false;
  @State banner: string = '';
  @State burnPhase: BurnPhase = 'idle';
  @State burnPct: number = 0;
  @State burnMsg: string = '';
  @State burnedPath: string = '';
  @State burnedSrcPath: string = '';
  @State gallerySaved: boolean = false;
  @State previewWidthVp: number = 360;
  @State previewHeightVp: number = 780;

  aboutToAppear(): void {
    this.permsReady = this.requestPermissions();
    this.tickId = setInterval(() => {
      this.refreshTimerTexts();
    }, 33);
  }

  aboutToDisappear(): void {
    if (this.tickId >= 0) {
      clearInterval(this.tickId);
    }
    StreamRecorderService.release();
    this.cameraService.releaseSession();
    this.recorder.release();
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      // 字形农场置于底层：自然布局保证 componentSnapshot 可用，
      // 烧录期间被全屏遮罩完全遮挡
      if (this.burnPhase === 'raster' || this.burnPhase === 'wmRaster') {
        GlyphFarm({
          chars: ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', ' '],
          fontPx: this.timerGlyphPx(),
          color: this.timerItem().textColor,
          outlineEnabled: this.timerItem().outline,
          showLapLabel: this.timerItem().showLap,
          staticItems: this.enabledTextItems(),
          staticFontPx: this.staticFontPxList(),
          onReady: (farm: GlyphFarm) => {
            this.glyphFarm = farm;
          }
        })
      }

      Column() {
        // 预览 + 水印叠加层
        Stack() {
          XComponent({
            id: 'sportCameraPreview',
            type: XComponentType.SURFACE,
            controller: this.xcController
          })
            .onLoad(() => {
              const sid = this.xcController.getXComponentSurfaceId();
              hilog.info(DOMAIN, TAG, 'xcomponent loaded, surface=%{public}s', sid);
              this.openCamera(sid);
            })

          WatermarkOverlay({
            items: this.watermarkItems,
            timerTotalText: this.timerTotal,
            timerLapText: this.timerLap
          })
        }
        .width('100%')
        .layoutWeight(1)
        .onAreaChange((_o, n) => {
          this.previewWidthVp = Number(n.width);
          this.previewHeightVp = Number(n.height);
        })

        if (this.lensList.length > 1) {
          this.LensBar()
        }

        this.BottomBar()
      }
      .width('100%')
      .height('100%')

      this.TopHud()

      if (this.banner.length > 0) {
        Text(this.banner)
          .fontSize(14)
          .fontColor('#FFFFFF')
          .backgroundColor('#CC000000')
          .borderRadius(8)
          .padding({ left: 16, right: 16, top: 8, bottom: 8 })
          .position({ x: 16, y: 120 })
          .onClick(() => {
            this.banner = '';
          })
      }

      if (this.burnPhase === 'burning') {
        this.BurnBadge()
      }
      if (this.burnPhase === 'wmRaster') {
        this.BurnBadge()
      }
      if (this.burnPhase === 'saving') {
        this.BurnMask()
      }

      if (this.burnPhase === 'done' || this.burnPhase === 'error') {
        this.BurnResult()
      }

      if (this.showSettings) {
        // 半屏设置面板
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
    .bindSheet($$this.showSettings, this.SettingsBuilder(), {
      detents: [SheetSize.MEDIUM, SheetSize.LARGE],
      blurStyle: BlurStyle.NONE,
      backgroundColor: '#00000000',
      showClose: false
    })
  }

  @Builder
  SettingsBuilder() {
    SettingsSheet({ show: this.showSettings })
  }

  @Builder
  TopHud() {
    Column() {
      Row() {
        if (this.recording) {
          Row() {
            Circle({ width: 10, height: 10 }).fill('#FF3B30')
            Text(this.recDurationText)
              .fontSize(14)
              .fontColor('#FFFFFF')
              .margin({ left: 6 })
          }
          .backgroundColor('#66000000')
          .borderRadius(14)
          .padding({ left: 10, right: 10, top: 4, bottom: 4 })
        }
        Blank()
        if (this.rtspEnabled) {
          Text('RTSP')
            .fontSize(12)
            .fontColor('#FFD200')
            .border({ width: 1, color: '#66FFD200' })
            .borderRadius(8)
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
            .margin({ right: 6 })
        }
        if (this.eisEnabled) {
          Text('防抖')
            .fontSize(12)
            .fontColor('#00E5FF')
            .border({ width: 1, color: '#6600E5FF' })
            .borderRadius(8)
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
        }
        if (this.profileText.length > 0) {
          Text(this.profileText)
            .fontSize(12)
            .fontColor('#AAAAAA')
            .margin({ left: 8 })
        }
      }
      .width('100%')
      .padding({ left: 16, right: 16 })
    }
    .width('100%')
    .padding({ top: 48 })
    .hitTestBehavior(HitTestMode.None)
  }

  @Builder
  LensBar() {
    Row({ space: 10 }) {
      ForEach(this.lensList, (lens: LensInfo) => {
        Text(lens.label)
          .fontSize(13)
          .fontColor(lens.cameraId === this.currentLensId ? '#FFFFFF' : '#BBBBBB')
          .backgroundColor(lens.cameraId === this.currentLensId ? '#FF5E14' : '#66000000')
          .borderRadius(14)
          .padding({ left: 14, right: 14, top: 6, bottom: 6 })
          .onClick(() => {
            this.pickLens(lens.cameraId);
          })
      }, (lens: LensInfo) => lens.cameraId)
    }
    .width('100%')
    .justifyContent(FlexAlign.Center)
    .padding({ bottom: 8 })
  }

  @Builder
  BottomBar() {
    Row() {
      // 水印设置
      Column() {
        Text('⚙').fontSize(22).fontColor('#FFFFFF')
        Text('水印').fontSize(11).fontColor('#CCCCCC').margin({ top: 4 })
      }
      .onClick(() => {
        this.showSettings = true;
      })

      Blank()

      // 计圈（录制中可用）
      Column() {
        Text('⏱').fontSize(22).fontColor(this.recording ? '#00E5FF' : '#666666')
        Text('计圈').fontSize(11).fontColor('#CCCCCC').margin({ top: 4 })
      }
      .onClick(() => {
        this.doLap();
      })

      // 录制按钮
      Column() {
        if (this.recording) {
          Rect({ width: 34, height: 34 })
            .fill('#FF3B30')
            .radius(8)
            .stroke('#FFFFFF')
            .strokeWidth(3)
        } else {
          Circle({ width: 66, height: 66 })
            .fill('#FF3B30')
            .stroke('#FFFFFF')
            .strokeWidth(4)
        }
      }
      .margin({ left: 24, right: 24 })
      .onClick(() => {
        if (this.recording) {
          this.stopRecording();
        } else {
          this.startRecording();
        }
      })

      // 计时重置（计时随录制自动开始/结束）
      Column() {
        Text('↺').fontSize(22).fontColor('#FFFFFF')
        Text('重置').fontSize(11).fontColor('#CCCCCC').margin({ top: 4 })
      }
      .onClick(() => {
        this.swimTimer.reset();
        this.refreshTimerTexts();
      })

      Blank()

      // RTSP 推流开关
      Column() {
        Text('📡').fontSize(22).fontColor(this.rtspEnabled ? '#FFD200' : '#FFFFFF')
        Text('推流').fontSize(11).fontColor('#CCCCCC').margin({ top: 4 })
      }
      .onClick(() => {
        if (this.rtspEnabled) {
          RtspService.stop();
          this.rtspEnabled = false;
          this.showBanner('RTSP 推流已停止');
        } else {
          if (RtspService.start(8554)) {
            this.rtspEnabled = true;
            this.showBanner('推流已开启：ONVIF 可自动发现，地址 rtsp://本机IP:8554/live');
          } else {
            this.showBanner('推流启动失败（端口被占用？）');
          }
        }
      })

      Blank()

      // 翻转镜头
      Column() {
        Text('⟳').fontSize(22).fontColor('#FFFFFF')
        Text('翻转').fontSize(11).fontColor('#CCCCCC').margin({ top: 4 })
      }
      .onClick(() => {
        this.switchCamera();
      })
    }
    .width('100%')
    .padding({ left: 28, right: 28, top: 12, bottom: 34 })
    .alignItems(VerticalAlign.Center)
  }

  @Builder
  BurnBadge() {
    Row({ space: 8 }) {
      LoadingProgress().width(18).height(18).color('#FF5E14')
      Text('水印写入中 ' + this.burnPct + '%')
        .fontSize(13)
        .fontColor('#FFFFFF')
    }
    .backgroundColor('#99000000')
    .borderRadius(16)
    .padding({ left: 14, right: 14, top: 6, bottom: 6 })
    .position({ x: '50%', y: 120 })
    .translate({ x: '-50%' })
    .hitTestBehavior(HitTestMode.None)
  }

  @Builder
  BurnMask() {
    Column({ space: 14 }) {
      LoadingProgress().width(44).height(44).color('#FF5E14')
      Text(this.burnPhase === 'saving' ? '正在保存到相册…' : '正在写入水印…')
        .fontSize(16)
        .fontColor('#FFFFFF')
      Progress({ value: this.burnPct, total: 100, type: ProgressType.Linear })
        .width('60%')
        .color('#FF5E14')
      Text(this.burnPct + '%').fontSize(13).fontColor('#AAAAAA')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#CC000000')
    .justifyContent(FlexAlign.Center)
  }

  @Builder
  BurnResult() {
    Column({ space: 16 }) {
      Text(this.burnPhase === 'done' ? '✓' : '!')
        .fontSize(40)
        .fontColor(this.burnPhase === 'done' ? '#00E576' : '#FF3B30')
      Text(this.burnMsg)
        .fontSize(15)
        .fontColor('#FFFFFF')
        .textAlign(TextAlign.Center)
        .padding({ left: 32, right: 32 })
      if (this.burnPhase === 'done' && !this.gallerySaved && this.burnedPath.length > 0) {
        SaveButton({ text: SaveDescription.SAVE_TO_GALLERY, icon: SaveIconStyle.FULL_FILLED,
          buttonType: ButtonType.Capsule })
          .onClick(async () => {
            if (this.burnPhase === 'saving') {
              return; // 防重复点击
            }
            try {
              // 文件存在性检查
              if (!this.fileExists(this.burnedPath)) {
                this.burnMsg = '录制文件不存在，请重新录制';
                this.burnPhase = 'done';
                return;
              }
              this.burnPhase = 'saving';
              await GalleryService.saveVideo(this.context, this.burnedPath);
              // 延迟清理：等文件系统 flush
              await this.sleep(500);
              this.cleanupFile(this.burnedSrcPath);
              this.cleanupFile(this.burnedPath);
              this.gallerySaved = true;
              this.burnMsg = '视频已保存到相册（含水印）';
              this.burnPhase = 'done';
            } catch (e) {
              hilog.error(DOMAIN, TAG, 'save to gallery failed: %{public}s', JSON.stringify(e));
              this.burnMsg = '保存失败，可重试或检查存储权限';
              this.burnPhase = 'done';
            }
          })
      }
      Button(this.gallerySaved ? '知道了' : '暂不保存')
        .backgroundColor('#333333')
        .fontColor('#FFFFFF')
        .margin({ top: 12 })
        .onClick(() => {
          if (!this.gallerySaved && this.burnedPath.length > 0) {
            // 暂不保存：清理临时文件
            this.cleanupFile(this.burnedSrcPath);
            this.cleanupFile(this.burnedPath);
          }
          this.burnPhase = 'idle';
          this.burnMsg = '';
          this.burnPct = 0;
          this.burnedPath = '';
          this.burnedSrcPath = '';
          this.gallerySaved = false;
        })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#E6000000')
    .justifyContent(FlexAlign.Center)
  }

  // ---------- 业务逻辑 ----------

  private async requestPermissions(): Promise<void> {
    const perms: Permissions[] = ['ohos.permission.CAMERA', 'ohos.permission.MICROPHONE'];
    try {
      const atm = abilityAccessCtrl.createAtManager();
      await atm.requestPermissionsFromUser(this.context, perms);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'request permissions failed');
    }
  }

  private openCameraCount: number = 0;

  private async openCamera(surfaceId: string): Promise<void> {
    this.openCameraCount++;
    hilog.info(DOMAIN, TAG, 'openCamera call #%{public}d', this.openCameraCount);
    try {
      await this.permsReady;
      let videoSurfaceId: string | undefined = undefined;
      if (this.useStreamPipeline) {
        const info0 = this.cameraService.getVideoProfileInfo();
        const rotation = this.frontCamera ? 270 : 90;
        videoSurfaceId = StreamRecorderService.prepare(
          info0.width, info0.height, info0.frameRate, 20000000, 0);
        hilog.info(DOMAIN, TAG, 'stream pipeline prepared, surface=%{public}s', videoSurfaceId);
      }
      await this.cameraService.open(this.context, surfaceId,
        camera.CameraPosition.CAMERA_POSITION_BACK, undefined, videoSurfaceId);
      const info = this.cameraService.getVideoProfileInfo();
      this.eisEnabled = info.stabilizationMode > 0;
      this.profileText = info.width + 'x' + info.height + '@' + info.frameRate;
      this.lensList = this.cameraService.getLensList();
      this.currentLensId = this.cameraService.getCurrentCameraId();
      this.cameraReady = true;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'openCamera failed: %{public}s', JSON.stringify(e));
      this.showBanner('相机打开失败，请检查相机权限');
    }
  }

  private async pickLens(cameraId: string): Promise<void> {
    if (cameraId === this.currentLensId) {
      return;
    }
    try {
      await this.cameraService.switchLens(cameraId);
      this.currentLensId = cameraId;
      this.lensList = this.cameraService.getLensList();
    } catch (e) {
      this.showBanner('切换镜头失败');
    }
  }

  private async switchCamera(): Promise<void> {
    if (!this.cameraReady) {
      return;
    }
    // 录制中允许切换：视频流 surface 常挂不受影响，画面会短暂卡顿后恢复
    const next = this.frontCamera
      ? camera.CameraPosition.CAMERA_POSITION_BACK
      : camera.CameraPosition.CAMERA_POSITION_FRONT;
    try {
      await this.cameraService.switchCamera(next);
      this.frontCamera = !this.frontCamera;
      this.lensList = this.cameraService.getLensList();
      this.currentLensId = this.cameraService.getCurrentCameraId();
      StreamRecorderService.setCameraFlip(this.frontCamera);
      this.showBanner('已切换到' + (this.frontCamera ? '前置' : '后置') + '摄像头');
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'switchCamera failed: %{public}s', JSON.stringify(e));
      this.showBanner('切换摄像头失败');
    }
  }


  /** 阶段2：栅格化水印资产并下发实时管线（复用 GlyphFarm 与烧录同款配置） */
  private async prepareWatermarkAssets(): Promise<boolean> {
    try {
      await this.sleep(400); // 等待 GlyphFarm 布局
      const farm = this.glyphFarm;
      if (farm === null) {
        return false;
      }
      const timerCfg = this.timerItem();
      const textItems = this.enabledTextItems();
      const glyphs: BurnGlyph[] = [];
      const staticsRaw: BurnGlyph[] = [];
      if (timerCfg.enabled) {
        const captured = await farm.captureGlyphs();
        for (const g of captured) {
          glyphs.push(g);
        }
      }
      const sc = await farm.captureStatics();
      for (const g of sc) {
        staticsRaw.push(g);
      }

      const buffers: ArrayBuffer[] = [];
      const glyphMeta: GlyphMetaObj[] = [];
      for (const g of glyphs) {
        buffers.push(g.rgba);
        const meta: GlyphMetaObj = {
          char: g.char, width: g.width, height: g.height, bufferIndex: buffers.length - 1,
        };
        glyphMeta.push(meta);
      }
      const staticsMeta: StaticMetaObj[] = [];
      for (let i = 0; i < staticsRaw.length && i < textItems.length; i++) {
        buffers.push(staticsRaw[i].rgba);
        const item = textItems[i];
        const meta: StaticMetaObj = {
          anchor: item.anchor,
          marginXPx: this.marginXPx(item.margin),
          marginYPx: this.marginYPx(item.margin),
          width: staticsRaw[i].width,
          height: staticsRaw[i].height,
          displayHeightPx: this.staticFontPxList()[i],
          opacity: item.opacity,
          bufferIndex: buffers.length - 1,
        };
        staticsMeta.push(meta);
      }

      const info = this.cameraService.getVideoProfileInfo();
      const rotation = this.frontCamera ? 270 : 90;
      const timerObj: WmTimerObj = {
        enabled: timerCfg.enabled,
        anchor: timerCfg.anchor,
        marginXPx: this.marginXPx(timerCfg.margin),
        marginYPx: this.marginYPx(timerCfg.margin),
        glyphHeightPx: this.timerGlyphPx(),
        chars: '0123456789:. ',
        lapLabelIndex: timerCfg.showLap && glyphs.length > 0 ? glyphs.length - 1 : -1,
        showLap: timerCfg.showLap,
        startOffsetMs: 0,
        laps: this.snapshotLaps() as object[],
      };
      const cfg: WmCfgObj = {
        videoWidth: info.width,
        videoHeight: info.height,
        rotation: rotation,
        timer: timerObj,
        glyphs: glyphMeta,
        statics: staticsMeta,
      };
      hilog.info(DOMAIN, TAG, 'wm cfg: anchor=%{public}d mx=%{public}d my=%{public}d',
        timerCfg.anchor, this.marginXPx(timerCfg.margin), this.marginYPx(timerCfg.margin));
      hilog.info(DOMAIN, TAG, 'watermark assets ready: %{public}d glyphs', glyphs.length);
      return true;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'prepareWatermarkAssets failed: %{public}s', JSON.stringify(e));
      return false;
    }
  }

  private async startRecording(): Promise<void> {
    if (!this.cameraReady || this.recording || this.burnPhase === 'burning' || this.burnPhase === 'raster' || this.burnPhase === 'wmRaster') {
      if (this.burnPhase === 'burning' || this.burnPhase === 'raster' || this.burnPhase === 'wmRaster') {
        this.showBanner('水印还在准备，稍等一下');
      }
      return;
    }
    try {
      if (this.useStreamPipeline) {
        const devRot = StreamRecorderService.getDeviceRotation();
        this.showBanner(devRot === 90 || devRot === 270 ? '横屏录制' : '竖屏录制');
        // 阶段2：录制前栅格化水印资产（~400ms），随后入流合成
        this.burnPhase = 'wmRaster';
        const ok = await this.prepareWatermarkAssets();
        this.burnPhase = 'idle';
        if (!ok) {
          this.showBanner('水印资产准备失败，本次录制不带水印');
        }
        StreamRecorderService.begin(this.context);
      } else {
        const profile = this.cameraService.getVideoProfileInfo();
        const rotation = this.frontCamera ? 270 : 90;
        await this.recorder.prepare(this.context, profile, rotation);
        const surfaceId = await this.recorder.getSurfaceId();
        this.videoOutput = await this.cameraService.attachVideoSurface(surfaceId);
      }

      // 计时与录制完全同步：每次录制都从 0 起跑
      this.swimTimer.reset();
      this.swimTimer.start();  // 实时管线水印 startOffsetMs=0（计时与首帧同源）
      // 首帧到达时刻记录计时基准，烧录时按 基准+PTS 逐帧还原读数（仅旧管线）
      this.recStartTimerOffset = this.swimTimer.elapsedMs();
      if (this.videoOutput !== undefined) {
        this.videoOutput.on('frameStart', () => {
          this.recStartTimerOffset = this.swimTimer.elapsedMs();
        });
      }

      if (!this.useStreamPipeline) {
        await this.recorder.start();
      }
      this.recording = true;
      this.startRecDurationTick();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'startRecording failed: %{public}s', JSON.stringify(e));
      this.showBanner('开始录制失败');
      await this.cleanupRecording();
    }
  }

  private async stopRecording(): Promise<void> {
    hilog.info(DOMAIN, TAG, 'stopRecording entry');
    if (!this.recording) {
      return;
    }
    try {
      this.recording = false;
      this.stopRecDurationTick();
      // 录制结束计时同步结束（冻结显示最终成绩）
      this.swimTimer.pause();

      if (this.useStreamPipeline) {
        // 实时管线：视频已带封装完成，直接进入保存（阶段2 起水印在流内）
        const stats = await StreamRecorderService.stop();
        const path = StreamRecorderService.getFilePath();
        hilog.info(DOMAIN, TAG, 'stream stop: dur=%{public}lldms frames=%{public}lld',
          stats.durationMs, stats.frames);
        this.burnedSrcPath = path;
        this.burnedPath = path;
        this.gallerySaved = false;
        this.burnPhase = 'done';
        this.burnMsg = '录制完成（' + Math.round(stats.durationMs / 1000) + ' 秒），点击保存到相册';
        return;
      }

      const srcPath = await this.recorder.stop();
      const lapsSnapshot = this.snapshotLaps();
      const offsetSnapshot = this.recStartTimerOffset;
      const recSize = this.recorder.getRecordingSize();
      const rotation = this.frontCamera ? 270 : 90;
      await this.cameraService.detachVideoOutput(this.videoOutput!);
      this.videoOutput = undefined;
      await this.recorder.release();

      // 烧录转后台：立即恢复拍摄，完成后弹保存面板
      this.runBurnPipeline(srcPath, recSize, rotation, offsetSnapshot, lapsSnapshot);
    } catch (e) {
      this.recording = false;
      this.stopRecDurationTick();
      this.showBanner('停止录制失败');
      await this.cleanupRecording();
    }
  }

  private async cleanupRecording(): Promise<void> {
    if (this.useStreamPipeline) {
      if (StreamRecorderService.isRecording()) {
        await StreamRecorderService.stop();
      }
      return;
    }
    if (this.videoOutput) {
      await this.cameraService.detachVideoOutput(this.videoOutput);
      this.videoOutput = undefined;
    }
    await this.recorder.release();
  }

  private doLap(): void {
    hilog.info(DOMAIN, TAG, 'doLap entry');
    if (!this.recording) {
      this.showBanner('录制中才能计圈');
      return;
    }
    const lap = this.swimTimer.lap();
    if (lap !== null) {
      this.refreshTimerTexts();
      if (this.useStreamPipeline) {
        StreamRecorderService.updateLaps(JSON.stringify(this.snapshotLaps()));
      }
    }
  }

  private refreshTimerTexts(): void {
    if (this.swimTimer.getState() === TimerState.IDLE) {
      this.timerTotal = '00:00.00';
      this.timerLap = '';
      return;
    }
    this.timerTotal = formatTimer(this.swimTimer.elapsedMs());
    const laps = this.swimTimer.getLaps();
    if (laps.length > 0) {
      this.timerLap = formatLapLine(laps.length, this.swimTimer.lastLapMs());
    }
  }

  private recStartMs: number = 0;
  private recTickId: number = -1;

  private startRecDurationTick(): void {
    this.recStartMs = new Date().getTime();
    this.recTickId = setInterval(() => {
      const sec = Math.floor((Date.now() - this.recStartMs) / 1000);
      const m = Math.floor(sec / 60) % 60;
      const s = sec % 60;
      const mm: string = m < 10 ? '0' + m : '' + m;
      const ss: string = s < 10 ? '0' + s : '' + s;
      this.recDurationText = mm + ':' + ss;
    }, 500);
  }

  private stopRecDurationTick(): void {
    if (this.recTickId >= 0) {
      clearInterval(this.recTickId);
      this.recTickId = -1;
    }
    this.recDurationText = '00:00';
  }

  // ---------- 烧录流水线 ----------

  private async runBurnPipeline(srcPath: string, recSize: RecordSize,
    rotation: number, timerOffsetMs: number, laps: BurnLapInfo[]): Promise<void> {
    try {
      this.burnPhase = 'raster';
      // 等待 GlyphFarm 完成布局
      await this.sleep(350);

      const timerCfg = this.timerItem();
      const glyphs: BurnGlyph[] = [];
      const staticsRaw: BurnGlyph[] = [];

      const farm = this.glyphFarm;
      if (farm !== null) {
        if (timerCfg.enabled) {
          const captured = await farm.captureGlyphs();
          for (const g of captured) {
            glyphs.push(g);
          }
        }
        const sc = await farm.captureStatics();
        for (const g of sc) {
          staticsRaw.push(g);
        }
      }

      const statics: BurnStaticImage[] = [];
      const textItems = this.enabledTextItems();
      for (let i = 0; i < staticsRaw.length && i < textItems.length; i++) {
        const item = textItems[i];
        const displayPx = this.staticFontPxList()[i];
        statics.push({
          rgba: staticsRaw[i].rgba,
          width: staticsRaw[i].width,
          height: staticsRaw[i].height,
          anchor: item.anchor,
          marginXPx: this.marginXPx(item.margin),
          marginYPx: this.marginYPx(item.margin),
          displayHeightPx: displayPx,
          opacity: item.opacity,
        } as BurnStaticImage);
      }

      // 视频显示空间尺寸（旋转后）
      const displayW = rotation === 90 || rotation === 270 ? recSize.height : recSize.width;
      const displayH = rotation === 90 || rotation === 270 ? recSize.width : recSize.height;

      this.burnPhase = 'burning';
      this.burnPct = 0;
      const ts = new Date().getTime();
      const dstPath = this.context.cacheDir + '/record/burned_' + ts + '.mp4';

      if (timerCfg.enabled) {
        await BurnService.burn(srcPath, dstPath, {
          videoWidth: recSize.width,
          videoHeight: recSize.height,
          rotation: rotation,
          timer: {
            enabled: true,
            anchor: timerCfg.anchor,
            marginXPx: this.marginXPx(timerCfg.margin),
            marginYPx: this.marginYPx(timerCfg.margin),
            glyphHeightPx: this.timerGlyphPx(),
            chars: '0123456789:. ',
            lapLabelIndex: glyphs.length > 0 && timerCfg.showLap ? glyphs.length - 1 : -1,
            showLap: timerCfg.showLap,
            startOffsetMs: timerOffsetMs,
            laps: laps,
          },
          statics: statics,
        }, glyphs, (pct: number) => {
          this.burnPct = pct;
        });
      } else {
        await BurnService.burn(srcPath, dstPath, {
          videoWidth: recSize.width,
          videoHeight: recSize.height,
          rotation: rotation,
          timer: {
            enabled: false,
            anchor: 0,
            marginXPx: 0,
            marginYPx: 0,
            glyphHeightPx: 0,
            chars: '',
            lapLabelIndex: -1,
            showLap: false,
            startOffsetMs: 0,
            laps: [],
          },
          statics: statics,
        }, [], (pct: number) => {
          this.burnPct = pct;
        });
      }

      // 媒体库写入必须由用户手势触发：交给结果面板的 SaveButton
      this.burnedSrcPath = srcPath;
      this.burnedPath = dstPath;
      this.gallerySaved = false;
      this.burnPhase = 'done';
      this.burnMsg = '水印已写入视频，点击下方按钮保存到相册';
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'burn pipeline error: %{public}s', JSON.stringify(e));
      this.burnPhase = 'error';
      this.burnMsg = '水印写入失败，原视频仍在应用缓存目录';
    }
  }

  // ---------- 几何换算（预览 vp → 视频显示空间 px） ----------

  /** 视频显示空间高度 px（旋转后） */
  private displayHeightPx(): number {
    const info = this.cameraService.getVideoProfileInfo();
    const rotation = this.frontCamera ? 270 : 90;
    return rotation === 90 || rotation === 270 ? info.width : info.height;
  }

  private displayWidthPx(): number {
    const info = this.cameraService.getVideoProfileInfo();
    const rotation = this.frontCamera ? 270 : 90;
    return rotation === 90 || rotation === 270 ? info.height : info.width;
  }

  /** 计时字形在视频坐标系下的高度 px */
  private timerGlyphPx(): number {
    const item = this.timerItem();
    const fontVp = 18 * item.fontScale;
    return Math.round(fontVp / this.previewHeightVp * this.displayHeightPx());
  }

  private staticFontPxList(): number[] {
    const out: number[] = [];
    const previewH = this.previewHeightVp;
    const videoH = this.displayHeightPx();
    const items = this.enabledTextItems();
    for (const item of items) {
      const fontVp = 18 * item.fontScale;
      out.push(Math.round(fontVp / previewH * videoH));
    }
    return out;
  }

  private marginXPx(marginVp: number): number {
    return Math.round(marginVp / this.previewWidthVp * this.displayWidthPx());
  }

  private marginYPx(marginVp: number): number {
    return Math.round(marginVp / this.previewHeightVp * this.displayHeightPx());
  }

  private timerItem(): WatermarkItem {
    for (const it of WatermarkStore.getItems()) {
      if (it.kind === WatermarkKind.TIMER) {
        return it;
      }
    }
    return WatermarkStore.getItems()[0];
  }

  private enabledTextItems(): WatermarkItem[] {
    const out: WatermarkItem[] = [];
    for (const it of WatermarkStore.getItems()) {
      if (it.kind === WatermarkKind.TEXT && it.enabled) {
        out.push(it);
      }
    }
    return out;
  }

  private snapshotLaps(): BurnLapInfo[] {
    const out: BurnLapInfo[] = [];
    const laps: LapRecord[] = this.swimTimer.getLaps();
    for (const l of laps) {
      out.push({ index: l.index, totalMs: l.totalMs, lapMs: l.lapMs } as BurnLapInfo);
    }
    return out;
  }

  private showBanner(msg: string): void {
    this.banner = msg;
    setTimeout(() => {
      if (this.banner === msg) {
        this.banner = '';
      }
    }, 2600);
  }


  private fileExists(path: string): boolean {
    try {
      return path.length > 0 && fileIo.accessSync(path);
    } catch (e) {
      return false;
    }
  }

  private cleanupFile(path: string): void {
    try {
      if (path.length > 0 && fileIo.accessSync(path)) {
        fileIo.unlinkSync(path);
      }
    } catch (e) {
      // 清理失败不影响主流程
    }
  }

  private sleep(ms: number): Promise<void> {
    return new Promise<void>((resolve) => {
      setTimeout(() => {
        resolve();
      }, ms);
    });
  }
}
