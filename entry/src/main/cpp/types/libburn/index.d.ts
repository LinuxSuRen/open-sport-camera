/**
 * libburn.so native 接口声明。
 *
 * startBurn:
 * - src/dst: mp4 文件沙箱路径
 * - cfgJson: 烧录配置（见 BurnService 组装格式）
 * - buffers: RGBA 纹理缓冲（字形图集 + 静态水印），cfgJson 中以 bufferIndex 引用
 * - onProgress: 进度回调 0-100（native 线程回调）
 * @return 0 成功，负数为错误码
 */
export const startBurn: (src: string, dst: string, cfgJson: string,
  buffers: ArrayBuffer[], onProgress: (pct: number) => void) => Promise<number>;

/**
 * 实时录制管线（阶段1）：相机帧 → GL → 硬编码 → MP4。
 * 四段式：prepareRecord（相机会话创建时挂视频流，拓扑此后不变）
 *        → beginRecord（启编码器+渲染循环）→ stopRecord（完成封装）
 *        → releaseRecord（相机关闭时释放）
 */
export const prepareRecord: (width: number, height: number, fps: number,
  bitrate: number, rotation: number) => string;

export const beginRecord: (outPath: string) => number;

/** 停止录制并完成 MP4 封装（异步，不阻塞 UI） */
export const stopRecord: () => Promise<RecordStats>;

export const releaseRecord: () => void;

export interface RecordStats {
  durationMs: number;
  frames: number;
}

/** 阶段2：录制前设置水印资产（字形图集+静态贴图+计时配置，与烧录配置同构） */
export const setWatermarkAssets: (cfgJson: string, buffers: ArrayBuffer[]) => number;

/** 录制中更新计圈数据 */
export const updateWatermarkLaps: (lapsJson: string) => number;

/** 阶段3：启动 RTSP 推流服务器（局域网内 VLC 可拉流） */
export const startRtsp: (port: number) => number;

/** 停止 RTSP 服务器 */
export const stopRtsp: () => void;

export interface RtspStatus {
  running: boolean;
  hasClient: boolean;
  port: number;
}

/** RTSP 服务器状态 */
export const rtspStatus: () => RtspStatus;

/** 切换前后摄像头时设置水平镜像（前置=true 后置=false） */
export const setCameraFlip: (flip: boolean) => void;

/** 挂载 muxer 开始录文件（编码管线须已在运行，推流不断） */
export const startRecordingFile: (outPath: string) => number;

/** 卸载 muxer 停止录文件（编码管线继续运行） */
export const stopRecordingFile: () => RecordStats;
