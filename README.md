# jellyfin-rkmpp-wrapper

RK3399 上跑 Jellyfin（jellyfin-ffmpeg 的 rkmpp 硬件加速）会撞上两个绕不开的问题：

1. Jellyfin 给 rkmpp 硬解硬编码了 `-afbc rga` 参数，但 RK3399 的 RGA 引擎解析不了 AFBC 帧，导致转码直接起不来，日志里报 `Output format 'nv12' with AFBC modifier is not supported by RGA2`。
2. Android 客户端播放时画面整体发绿。原因在字幕烧录：Jellyfin 把 ASS 字幕渲染成 BGRA 图层后，用 `overlay_rkrga` 在 RGA2 硬件里做 YUV 主画面 + RGB 字幕层 → YUV 输出的一次性混合。RGA2 硬件做不了这种单趟混合——前半程的 YUV→RGB 转换和后半程的 RGB→YUV 转换冲突，后半程被硬件静默丢掉，混合完的 RGB 数据被原样写进 YUV 平面，色度全坏。网页端不做字幕烧录，所以只有 Android 中招。

## 工作方式

问题 2 的修复分两半：**librga 补丁**（本仓库 `librga.so.2.1.0.rga-split`，源码改动见 `librga-rga-split-RockchipRga.cpp`）和 **wrapper v2**（本脚本）。

librga 补丁在 `RkRgaBlit` 里拦下"YUV 主画面 + RGB 图层 → YUV 输出且要求混合"这种 RGA2 单趟做不了的任务，拆成两趟硬件操作，全程不过 CPU：

```
第 1 趟：YUV 主画面 → RGB 暂存缓冲        （硬件 YUV→RGB，不混合）
第 2 趟：RGB 暂存 + RGB 字幕 → YUV 输出   （硬件混合 + RGB→YUV）
```

两趟的色彩矩阵用 BT.601 一对（实测往返 53.5 dB，基本无损；BT.709 的两向矩阵在这批硬件上不是严格互逆，往返只有 32 dB）。暂存缓冲按需从 dma-heap 分配并按尺寸缓存复用。Jellyfin 的 ffmpeg 动态链接 `librga.so.2`，换掉这个 .so 就生效，不用重编 ffmpeg。

wrapper v2 做的事就简单了：剥掉 `-afbc <val>` 全局参数，再把 `overlay_rkrga` 滤镜参数里的 `:afbc=N` 去掉（RGA2 既吃不了也吐不出 AFBC），滤镜图原样透传——字幕烧录完整留在 RGA 硬件上，零 CPU 开销。

```
Jellyfin → ffmpeg(= wrapper 脚本) → 剥 afbc 参数 → ffmpeg.bin(真二进制)
                                        ↓
                            librga(补丁版) 拆两趟硬件混合
```

注意：**wrapper v2 必须搭配补丁版 librga 使用**。只装 wrapper 不换 librga，绿屏会原样回来。

## 安装

纯 bash 脚本 + 一个预编译 .so，没有编译环节。想检查语法的话：

```bash
bash -n jellyfin-rkmpp-wrapper.sh
```

### 一键安装

把仓库放到板子上（scp 或 U 盘都行），中文字体可选：

```bash
sudo ./install.sh
```

`install.sh` 会：把真二进制保留为 `ffmpeg.bin`（幂等，重复跑没问题）；把 wrapper 装到 `/usr/lib/jellyfin-ffmpeg/ffmpeg`；把补丁版 librga 装到 `/usr/lib/jellyfin-ffmpeg/lib/librga.so.2.1.0`（原版备份为 `.orig`）；如果脚本同目录放了 `DroidSansFallbackFull.ttf`，顺带装进 `/usr/share/fonts/truetype/droid/` 并刷新字体缓存；最后重启 jellyfin 服务。字体这步建议做——系统里没有 CJK 字体的话，烧录出来的中文字幕全是方块。

### 手动安装

不想跑脚本的话，等价的几步：

```bash
FFDIR=/usr/lib/jellyfin-ffmpeg

# 1. 真二进制改名保留（只做一次）
sudo cp -a $FFDIR/ffmpeg $FFDIR/ffmpeg.bin

# 2. 装 wrapper
sudo install -m 755 jellyfin-rkmpp-wrapper.sh $FFDIR/ffmpeg

# 3. 换补丁版 librga（原版留档，方便回滚）
sudo cp -a $FFDIR/lib/librga.so.2.1.0 $FFDIR/lib/librga.so.2.1.0.orig
sudo cp librga.so.2.1.0.rga-split $FFDIR/lib/librga.so.2.1.0

# 4. CJK 字体（中文字幕需要）
sudo mkdir -p /usr/share/fonts/truetype/droid
sudo cp DroidSansFallbackFull.ttf /usr/share/fonts/truetype/droid/
sudo fc-cache -f

# 5. 重启服务
sudo systemctl restart jellyfin
```

### 卸载

```bash
sudo ./uninstall.sh    # 把 ffmpeg.bin 改回 ffmpeg，重启服务
```

librga 单独回滚：把 `librga.so.2.1.0.orig` 复制回 `librga.so.2.1.0`。

## 验证装好了没

用 Android 客户端放一段带字幕、需要转码的视频，然后看日志：

```bash
grep -m1 "ffmpeg" /var/log/jellyfin/FFmpeg.Transcode-*.log | head -1
```

命令开头是 wrapper、`-afbc` 不见了、`overlay_rkrga` 的 `:afbc=1` 被剥掉，同时 dmesg 里能看到 `rga split overlay: scratch fd=...`——这就是补丁在拆两趟跑。画面颜色正常、中文字幕正常，就是装好了。

## 性能

1080p HEVC Main10 → H264：字幕烧录走 RGA 硬件两趟混合后，转码速度和不烧字幕基本持平（约 3.6 倍实时）；对比早期"软件 overlay"方案的 0.86 倍实时，字幕烧录不再是瓶颈。画质上与真值逐帧对比 60 帧：对同管线真值（硬件解码+缩放）51.0–51.9 dB，对纯软解真值 47.8–48.4 dB，全部合格；修复前只有 14–16 dB，肉眼看不出差异。

## 已知边界

- BT.709 片源会按 BT.601 矩阵做这两趟转换，色度有极小的系统性偏移（53.5 dB 量级），肉眼不可见。根因是这批 RGA2 的 709 双向矩阵不互逆，属硬件行为，无法在驱动层修。
- 补丁只拦"YUV + RGB → YUV 混合"这一种组合，其他组合硬件本来就支持，直接走原路径，零影响。
