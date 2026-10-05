# 贡献指南

感谢关注 Open Sport Camera！欢迎通过 Issue 与 PR 参与共建。

## 开发环境

二选一：

- **DevEco Studio**（推荐图形界面开发者）：直接打开本仓库即可
- **Command Line Tools**（命令行 / CI）：
  1. 从[华为开发者下载中心](https://developer.huawei.com/consumer/cn/download/)安装 Command Line Tools（含 SDK / ohpm / hvigorw / hdc）
  2. 手机开启「开发者模式 + USB 调试」

## 构建与验证

```bash
ohpm install
hvigorw assembleHap --mode module -p product=default -p buildMode=debug --no-daemon
```

产物：`entry/build/default/outputs/default/entry-default-unsigned.hap`

安装到已连接设备（需要调试签名，见下）：

```bash
hdc install entry/build/default/outputs/default/*.hap
```

> 注意：真机安装需要 AGC 调试证书签名；CI 产物为未签名 HAP，仅供验证编译。

## 真机调试签名（个人开发者）

1. 用 SDK `toolchains/lib/hap-sign-tool.jar` 生成本地密钥与 CSR（参考 `.signing/gen-csr.sh`，目录不入库）
2. 到 [AppGallery Connect](https://developer.huawei.com/consumer/cn/agconnect/) 申请**调试证书**（.cer）与**调试 Profile**（.p7b，绑定设备 UDID 与包名 `com.linuxsuren.opensportcamera`）
3. 在 `build-profile.json5` 配置 signingConfigs 后以 debug 模式构建

## 分支与提交规范

- 新需求/修复从最新 `master` 切出 `feat/xxx`、`fix/xxx` 分支，通过 PR 合入
- 提交信息使用**中文 + Conventional Commits**（如 `feat: 支持前置摄像头录制`、`fix: 修复计圈时间漂移`）
- PR 描述请说明改动动机、影响范围与自测结论

## 代码约定

- ArkTS 遵循[方舟开发语言规范](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/typescript-to-arkts-migration-guide)（严格模式：禁止 any、模板字符串等受限语法）
- C++ 侧保持零第三方依赖；新增源文件需同步更新 `entry/src/main/cpp/CMakeLists.txt`
- UI 文案与代码注释使用中文；对外标识符使用英文

## 行为准则

友善、就事论事、聚焦技术。对事不对人。
