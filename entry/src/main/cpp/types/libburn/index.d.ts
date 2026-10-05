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
