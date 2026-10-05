import { CameraService } from '../camera/CameraService';
import { RecorderService, RecordSize } from '../record/RecorderService';
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

type BurnPhase = 'idle' | 'raster' | 'burning' | 'saving' | 'done' | 'error';

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
  @State banner: string = '';
  @State burnPhase: BurnPhase = 'idle';
  @State burnPct: number = 0;
  @State burnMsg: string = '';
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
    this.cameraService.releaseSession();
    this.recorder.release();
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
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

      if (this.burnPhase === 'raster') {
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

      if (this.burnPhase === 'burning' || this.burnPhase === 'saving') {
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

      // 计时器启停
      Column() {
        Text(this.swimTimer.getState() === TimerState.RUNNING ? '⏸' : '▶')
          .fontSize(22)
          .fontColor(this.swimTimer.getState() === TimerState.IDLE ? '#666666' : '#FFFFFF')
        Text('计时').fontSize(11).fontColor('#CCCCCC').margin({ top: 4 })
      }
      .onClick(() => {
        if (this.swimTimer.getState() === TimerState.RUNNING) {
          this.swimTimer.pause();
        } else if (this.swimTimer.getState() === TimerState.PAUSED) {
          this.swimTimer.start();
        } else {
          this.showBanner('录制开始时会自动计时，也可点这里先开始');
          this.swimTimer.start();
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
      Button('知道了')
        .backgroundColor('#FF5E14')
        .fontColor('#FFFFFF')
        .onClick(() => {
          this.burnPhase = 'idle';
          this.burnMsg = '';
          this.burnPct = 0;
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

  private async openCamera(surfaceId: string): Promise<void> {
    try {
      await this.permsReady;
      await this.cameraService.open(this.context, surfaceId,
        camera.CameraPosition.CAMERA_POSITION_BACK);
      const info = this.cameraService.getVideoProfileInfo();
      this.eisEnabled = info.stabilizationMode > 0;
      this.profileText = info.width + 'x' + info.height + '@' + info.frameRate;
      this.cameraReady = true;
    } catch (e) {
      this.showBanner('相机打开失败，请检查相机权限');
    }
  }

  private async switchCamera(): Promise<void> {
    if (this.recording || !this.cameraReady) {
      return;
    }
    const next = this.frontCamera
      ? camera.CameraPosition.CAMERA_POSITION_BACK
      : camera.CameraPosition.CAMERA_POSITION_FRONT;
    try {
      await this.cameraService.switchCamera(next);
      this.frontCamera = !this.frontCamera;
    } catch (e) {
      this.showBanner('切换摄像头失败');
    }
  }

  private async startRecording(): Promise<void> {
    if (!this.cameraReady || this.recording || this.burnPhase === 'burning') {
      return;
    }
    try {
      const profile = this.cameraService.getVideoProfileInfo();
      const rotation = this.frontCamera ? 270 : 90;
      await this.recorder.prepare(this.context, profile, rotation);
      const surfaceId = await this.recorder.getSurfaceId();
      this.videoOutput = await this.cameraService.attachVideoSurface(surfaceId);

      // 计时与录制联动：录制启动时秒表自动起步（已有读数则续计）
      if (this.swimTimer.getState() === TimerState.IDLE) {
        this.swimTimer.start();
      } else if (this.swimTimer.getState() === TimerState.PAUSED) {
        this.swimTimer.start();
      }
      // 首帧到达时刻记录计时基准，烧录时按 基准+PTS 逐帧还原读数
      this.recStartTimerOffset = this.swimTimer.elapsedMs();
      this.videoOutput.on('frameStart', () => {
        this.recStartTimerOffset = this.swimTimer.elapsedMs();
      });

      await this.recorder.start();
      this.recording = true;
      this.startRecDurationTick();
    } catch (e) {
      this.showBanner('开始录制失败');
      await this.cleanupRecording();
    }
  }

  private async stopRecording(): Promise<void> {
    if (!this.recording) {
      return;
    }
    try {
      const srcPath = await this.recorder.stop();
      const lapsSnapshot = this.snapshotLaps();
      const offsetSnapshot = this.recStartTimerOffset;
      const recSize = this.recorder.getRecordingSize();
      const rotation = this.frontCamera ? 270 : 90;
      this.recording = false;
      this.stopRecDurationTick();

      await this.cameraService.detachVideoOutput(this.videoOutput!);
      this.videoOutput = undefined;
      await this.recorder.release();

      await this.runBurnPipeline(srcPath, recSize, rotation, offsetSnapshot, lapsSnapshot);
    } catch (e) {
      this.recording = false;
      this.stopRecDurationTick();
      this.showBanner('停止录制失败');
      await this.cleanupRecording();
    }
  }

  private async cleanupRecording(): Promise<void> {
    if (this.videoOutput) {
      await this.cameraService.detachVideoOutput(this.videoOutput);
      this.videoOutput = undefined;
    }
    await this.recorder.release();
  }

  private doLap(): void {
    if (!this.recording) {
      this.showBanner('录制中才能计圈');
      return;
    }
    const lap = this.swimTimer.lap();
    if (lap !== null) {
      this.refreshTimerTexts();
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
      const sec = Math.floor((new Date().getTime() - this.recStartMs) / 1000);
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

      this.burnPhase = 'saving';
      await GalleryService.saveVideo(this.context, dstPath);
      this.cleanupFile(srcPath);
      this.cleanupFile(dstPath);
      this.burnPhase = 'done';
      this.burnMsg = '视频已保存到相册（含水印）';
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
    for (const item of this.enabledTextItems()) {
      const fontVp = 18 * item.fontScale;
      out.push(Math.round(fontVp / this.previewHeightVp * this.displayHeightPx()));
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
