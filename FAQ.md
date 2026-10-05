# 常见问题（FAQ）

来自 SGT-AL50 / HarmonyOS 6.1.0（API 24）真机实测，遇到同类问题先查这里。

## 权限

**Q：相机画面黑屏 / 权限弹窗不出现？**

锁屏状态下权限弹窗会被系统拦截。解锁手机后重启应用；或手动授权：设置 → 隐私与安全 → 权限管理 → 运动相机 → 相机/麦克风 → 允许。

**Q：`hdc shell atm dump -t -b com.linuxsuren.opensportcamera` 显示 grantStatus=0，但应用明明能用相机？**

atm 的显示语义与实际运行时授权不完全一致，以应用实际行为为准。

**Q：保存到相册报错 code 201？**

鸿蒙规定媒体库写入必须由用户手势触发。本应用在录制完成后弹出「保存到相册」按钮（SaveButton 安全组件），点击即保存；任何后台自动保存都会被拒。

**Q：麦克风权限被拒会影响录制吗？**

会。AVRecorder 配置 MIC 音源时权限不足会导致 `start()` 被拒。应用侧已做降级：检测到无麦克风权限时自动切换无声录制。

## 签名与安装

**Q：`hdc install` 报 `9568320 no signature file`？**

零售机强制校验签名。走调试签名流程（见 [CONTRIBUTING.md](CONTRIBUTING.md)）：
1. `bash .signing/gen-csr.sh` 生成本地密钥与 CSR
2. AGC 申请调试证书（.cer）与调试 Profile（.p7b，绑定设备 UDID 与包名）放入 `.signing/`
3. `bash .signing/sign.sh` 签名后安装

**Q：hap-sign-tool 报 keySize 参数错误？**

ECC 密钥长度要写 `NIST-P-256` 而非 `256`；签名模式是 `localSign`（旧文档的 `localappsign` 已失效）。

## 构建陷阱（重要）

**Q：改了 C++ 代码但真机行为没变？**

hvigor 增量构建可能打包**陈旧的 .so**（曾掩盖编译错误数小时）。native 改动后必须全清重建：

```bash
rm -rf entry/build entry/.cxx
hvigorw assembleHap --mode module -p product=default -p buildMode=debug --no-daemon
```

部署前验证产物：`unzip -p <HAP> libs/arm64-v8a/libburn.so | strings | grep <新日志字符串>`。

## 真机行为差异

**Q：视频画面绿色 / 无内容？**

两类根因（都已修复，供排查同类问题）：
1. **avcC 格式错误**：muxer 的 `OH_MD_KEY_CODEC_CONFIG` 必须是 avcC 记录格式（ISO 14496-15，长度前缀），传 annex-B 起始码流会生成损坏的 avcC，播放器解码失败显示绿屏。ffmpeg 报 `non-existing PPS 0 referenced` 可确证。
2. **NV12 布局错位**：编解码缓冲有对齐 stride（实测编码器 Y 区多 2560 字节），UV 偏移按缓冲实际容量反推，不可假设紧凑布局。

**Q：预览冻结 / 录制的视频有叠影和黑白块？**

动态挂流（录制时 addOutput + `session.start()` 重启）在部分机型上会损坏流状态。根治方案是实时管线的**视频流开机常挂**（相机拓扑永不变化）。经验规则：
- 配置事务（beginConfig/commitConfig）内不要调用任何可能失败的 API，失败调用会留下半损坏的流
- `isVideoStabilizationModeSupported` 在事务外调用；`setVideoStabilizationMode` 在事务内调用
- EIS 依赖机型固件标定参数，缺失时设置会失败（属设备限制，非代码问题）

**Q：编解码 Query 接口永远查不到缓冲？**

轮询式 `QueryInputBuffer/QueryOutputBuffer` 需要配置 `OH_MD_KEY_ENABLE_SYNC_MODE=1`（API 20+）。同步模式下编码器结束用「空缓冲 + EOS 标记」推送，`NotifyEndOfStream` 会被拒。

**Q：surface 模式编码器 RegisterParameterCallback 报 code=8（INVALID_STATE）？**

本机型实测无论在 Configure 前还是后注册都可能失败；该回调是 surface 模式必需的输入参数回推通道，失败时编码器不消费输入。处理见 [record_stream.cpp](entry/src/main/cpp/record_stream.cpp)。

## 推流与 ONVIF 发现

**Q：如何在电脑上看手机实时画面？**

1. 手机与电脑连同一 Wi-Fi
2. App 底栏点「推流」（出现 RTSP 徽标）
3. 点「录制」开始供流（推流内容随录制编码流）
4. 电脑 VLC 打开 `rtsp://<手机IP>:8554/live`

**Q：不想手输 IP？**

开启推流后手机同时提供 ONVIF 服务（自动发现 + GetStreamUri）：
- 支持 ONVIF 的 NVR/软件（如 ONVIF Device Manager、蓝鲨等）可自动发现设备「OpenSportCamera」并直接取得 RTSP 地址
- SOAP 服务端口 8000，WS-Discovery 组播 239.255.255.250:3702

## 录制没有声音？

部分机型的 native AVCodecService 只注册了视频编解码器（如 SGT-AL50 只有 OMX.hisi.video.encoder.avc），没有 native AAC 音频编码器。应用会自动降级为无声录制，不影响视频质量。待设备/SDK 更新后可恢复。

## 调试工具

```bash
HDC=~/Library/Huawei/command-line-tools/sdk/default/openharmony/toolchains/hdc
$HDC list targets                                      # 设备连接
$HDC shell atm dump -t -b com.linuxsuren.opensportcamera  # 权限状态
$HDC hilog | grep -E "CameraService|RecordStream|BurnEngine"  # 日志
$HDC shell snapshot_display -f /data/local/tmp/s.jpeg   # 截图
$HDC shell uitest dumpLayout -p /data/local/tmp/l.json  # UI 树（找按钮坐标）
$HDC shell uitest uiInput click 648 2500                # 自动点击
$HDC file recv /data/local/tmp/s.jpeg /tmp/             # 拉文件
$HDC shell ls /data/app/el2/100/base/com.linuxsuren.opensportcamera/haps/entry/cache/record/  # 录制产物
```

真机验证视频质量：拉回 mp4 后用 `ffmpeg -i x.mp4 -ss 2 -frames:v 1 f.png` 抽帧检查。
