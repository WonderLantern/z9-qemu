# WonderLantern Z9 QEMU

**2026-10-23：shared441 开发源码更新。** 新功能、双机1/1000连拍和ZR双格式录像的本地验证，以及尚未通过的预拍摄，统一见[最新状态与限制](docs/shared441.md)。当前公开重构建不自动继承旧整机验收；用户自行提供固件，原厂DSP/ISP/编码算法尚未实现。

## 以下为 shared221 及更早版本的使用说明与历史验收

[Historical shared221 Z9 two-photo qualification / 旧版双拍验收](docs/photo-qualification221.json)


[Historical shared221 ZR recording qualification / 旧版录制验收](docs/recording-qualification221.json)

**shared221共享源码更新。** ZR C1.11的两段30秒H.265与R3D有完整原生及独立文件证据；不能据此宣称Z9录像通过。以下两次Z9照片数据保留shared146构建范围。详见[本轮实现、验证与限制](docs/shared221.md)。


**2026-10-14 开发快照：shared146。** 已同步最新共享输入、所有权、功能 lossless14 及视频模型。**H.265/R3D 相机录像仍未完成。** 当前公开 lossless14 配置已通过两次完整拍照、逐次新取景和菜单恢复，以及全部 91,411,200 个 RAW 样本独立比对；其他新选项保留各自验证范围。见 [当前快照](docs/development-snapshot.md) 和 [ZR 仓库](https://github.com/WonderLantern/zr-qemu)。


[English](README.md) · [构建](docs/build.md) · [固件输入](docs/firmware.md) · [RAW 拍摄](docs/capture.zh-CN.md) · [原理](docs/architecture.md) · [测试边界](docs/testing.md)

本项目提供 QEMU 10.2.4 的实验性 Z9 C 5.32 板级模型、极简本地网页控制器，
以及 Bayer → 功能性 ISP → 固件提交的缓冲区 → LCD 的图像路径、
完整 RAW 拍摄和虚拟 NVMe/exFAT 写卡路径。

**固件由用户自行提供。** 仓库不包含尼康固件、解密后的私有程序、密钥、机身转储、
字体、图标位图、校准数据、私人照片或原厂界面截图，也不会自动下载尼康固件。
这是 WonderLantern 独立开展的研究与学习项目，与尼康没有隶属或背书关系。

当前公开 lossless14 配置在同一原固件进程和虚拟卡中完成两次释放快门拍摄，
每张 **8280×5520、14 位 NEF** 拍后均恢复新取景并完成两次 PHOTO 菜单往返。
RawPy/LibRaw 独立解码的 **91,411,200 个 RAW 样本**全部匹配各自 guest 生产缓冲，
每张三路 JPEG 预览匹配当次编码输出，两份 F0 数据均排除未压缩尺寸回退。
旧取景启动器曾通过三次菜单往返及四次独立像素校验。具体证据见 [测试说明](docs/testing.md)。本地模型
能够运行原厂英文菜单、拍摄界面和 RAW 图像路径；ISO、快门、黑色描边及红色无卡
图标来自固件。网页仅展示 QEMU LCD，不自行绘制相机界面。

## 使用

在 Linux 或明确指定的 WSL 环境中运行。此项目只使用 `ubuntu-noble-nikon`，
不会调用或修改作业专用的 `ubuntu-noble`。构建依赖及安装命令见英文首页。

```sh
python tools/build.py --work-dir "$HOME/wonderlantern-z9-build" --jobs 2
python tools/make_cmos.py
python tools/run.py --firmware /绝对路径/z9-main-payload.bin
```

打开 `http://127.0.0.1:8789/`。等待原厂画面与 RAW/LCD 计数出现后操作。
MENU 进入菜单，Half shutter hold 半按返回拍摄，Release 释放半按。Ctrl+C 退出。
需要自己的图片时，先运行 `python tools/make_cmos.py --image /路径/图片.jpg`。

拍摄完整 RAW 时先退出取景进程，再运行：

```sh
python -m pip install -r requirements-capture.txt
python tools/make_cmos.py --width 8256 --height 5504 --bits 14 \
    --output .local/z9-14bit.raw
python tools/capture.py --firmware /绝对路径/z9-main-payload.bin \
    --output "$HOME/wonderlantern-z9-runs/capture-001" --audit
```

此命令通过原生按键路径选择 RAW、拍摄一张照片、等待恢复取景并验证两轮菜单，
退出 QEMU 后才从实际虚拟卡提取 NEF 和校验。通常需数分钟。
网页仍提供取景和菜单操作；快门流程用独立命令执行。输出目录必须是新的，
建议置于 Linux 文件系统。具体格式、依赖、输出及边界见 [RAW 拍摄说明](docs/capture.zh-CN.md)。

唯一支持的 MAIN 文件为 81,823,629 字节，SHA-256：

```text
6380ded18395ce11cb327c766721274c85127d2cf1a1e887ee60e3d156810bdd
```

请勿把升级包直接当 MAIN。仓库不提供提取服务或密钥。固件始终以只读输入打开。
运行生成的数据和截图留在本地，不能连同代码上传。

## 必须理解的边界

- 从 MAIN 入口启动，采用推断的内存布局、空存储模型和显式外设输入，并非真实上电。
- 部分常驻资源缺失。模型提供少量明确标注的虚拟 ROM 输入，参数默认值由用户固件
  在本机生成；这些不是恢复出的工厂校准数据。
- GPIO/MCU 服务使用硬件断点协调和外部事件输入。没有通过修改 PC、任务结果、
  OS 标志或看门狗让界面跳过流程。
- 网页默认输入为 640×480、12-bit RGGB/u16，完整拍摄使用全尺寸 14 位源；
  这些都是实验格式，不代表已验证真实 Z9 CMOS 协议。
- ISP 使用通用插值和颜色转换。看到 ISO 数字不等于真实自动曝光已实现。
- 服务核使用有限的 WFE/SEV 等待模型，配合指令时钟和虚拟时间外部输入；这不是
  完整 ARM 事件实现或实测硬件时序。恢复运行请冷启动，整机热复位尚未支持。
- 已验证功能性 NEF 写卡与拍后恢复；AF/PDAF、机械时序、录像、RAW 预拍摄、
  完整 GPU 和真实 CMOS 协议尚未验收。
- 本项目不访问 USB 相机、不刷机，也不是可安装进相机的软件。Z7 和 ZR 独立推进。

[研究笔记](docs/research-notes.md)记录可跨机型复用的方法；寄存器地址、事件位和
固件对象布局必须按机型、版本重新建立证据，不能只因共用 EXPEED 便直接套用。

当前 shared146 的共享输入/所有权模型、整轮录像失败结果及复用更正见 [shared146 notes](docs/shared146.md)。H.265／R3D 录像和原机算法仍未验收。

本次共享实现、整轮录像失败结果与复用更正见 [shared146 中文说明](docs/shared146.zh-CN.md)。
