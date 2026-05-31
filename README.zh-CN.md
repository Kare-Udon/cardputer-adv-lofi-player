# Cardputer Adv Lo-Fi Player

[English](README.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md)

Cardputer Adv Lo-Fi Player 是给 M5Stack Cardputer Adv 用的本地音乐播放器固件。把音乐放进 microSD 卡，就可以在设备上浏览、播放，并切换几种 Lo-Fi 音色。

![Cardputer Adv Lo-Fi Player demo](assets/readme/cardputer-adv-lofi-demo.gif)

## 安装

打开 WebFlash 工具：

[Cardputer Adv Lo-Fi Player WebFlash](https://kare-udon.github.io/cardputer-adv-lofi-player/webflash/)

点击页面里的工具，然后按照提示刷写固件。

## 功能

- 本地音乐浏览和播放
- 播放队列、上一首/下一首、播放/暂停、进度和音量控制
- Repeat 和 Shuffle
- 几种 Lo-Fi 音色预设
- 播放状态保存和恢复
- 支持 `mp3`、`m4a`、`aac`、`wav`

## 支持硬件

目前仅在 M5Stack Cardputer Adv 上测试过。普通 Cardputer 和其他 ESP32-S3 板卡暂不保证可用。

## microSD 目录

使用 FAT32 格式 microSD 卡，把音乐放在 `/Music` 下。

推荐目录结构：

```text
/Music/Artist/Album/Track.m4a
/Music/Artist/Track.mp3
/Music/Track.wav
```

固件会在这里保存索引和播放状态：

```text
/Music/LOFI/INDEX.TXT
/Music/LOFI/STATE.TXT
```

如果想重新扫描音乐或重置播放状态，可以删除这两个文件。

## 从源码构建

先安装并进入 ESP-IDF 环境。本项目使用 ESP-IDF `v5.4.1`。

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

把 `PORT` 换成你的实际串口，例如 `/dev/ttyUSB0`、`/dev/cu.usbmodemXXXX` 或 `COM3`。

## 基本操作

- `Enter`：确认 / 打开选中项
- `Space`：播放 / 暂停
- 方向键：移动和调整
- `B`：返回
- `M`：菜单或当前页面操作
- `H`：打开当前页面的按键帮助菜单

屏幕底部会显示当前页面可用的软键提示。

## 项目结构

```text
main/                 app 入口、板级初始化、UI、音频任务
src/                  播放核心、存储、输入、Lo-Fi DSP
assets/               图标和字体资源
partitions.csv        flash 分区表
sdkconfig.defaults    默认 ESP-IDF 配置
dependencies.lock     依赖锁定文件
```

## 反馈

提交 issue 时请尽量附上：

- 设备型号
- 固件 commit 或版本
- 音乐目录结构
- 串口日志或复现步骤

## 许可证

MIT License。详见 [LICENSE](LICENSE)。

第三方字体和资源保留其各自许可证；如有随资源附带的许可证文件，以 `assets/` 下对应文件为准。
