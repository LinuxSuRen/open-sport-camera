name: Bug 反馈
about: 报告功能异常或崩溃
labels: bug
body:
  - type: textarea
    id: what-happened
    attributes:
      label: 问题描述
      description: 发生了什么？预期是什么？
    validations:
      required: true
  - type: textarea
    id: repro
    attributes:
      label: 复现步骤
      placeholder: |
        1. 打开 App，开始录制
        2. 计圈两次
        3. 停止录制……
  - type: input
    id: device
    attributes:
      label: 设备与系统版本
      placeholder: 如：Mate 60 Pro，HarmonyOS 6.x（API 20+）
  - type: textarea
    id: logs
    attributes:
      label: 日志（可选）
      description: 可通过 `hilog` 或 `hdc hilog` 抓取
      render: shell
