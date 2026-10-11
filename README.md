# jellyfin-rkmpp-wrapper

RK3399 上跑 Jellyfin（jellyfin-ffmpeg 的 rkmpp 硬件加速）会撞上两个绕不开的问题：

1. Jellyfin 给 rkmpp 硬解硬编码了 `-afbc rga` 参数，但 RK3399 的 RGA 引擎解析不了 AFBC 帧，导致转码直接起不来，日志里报 `Output format 'nv12' with AFBC modifier is not supported by RGA2`。
2. Android 客户端播放时画面整体发绿。原因在字幕烧录：Jellyfin 把 ASS 字幕渲染成 BGRA 图层后，用 `overlay_rkrga` 在 RGA2 硬件里做 BGRA→NV12 混合，这条硬件混合通路写出的 UV 分量是错的。网页端不做字幕烧录，所以只有 Android 中招。

## 工作方式

Jellyfin 调 ffmpeg 走的是固定路径 `/usr/lib/jellyfin-ffmpeg/ffmpeg`。装上 wrapper 后，调用链变成：

```
Jellyfin → ffmpeg(= wrapper 脚本) → 过滤参数 / 重写滤镜图 → ffmpeg.bin(真二进制)
```

脚本做两件事：把 `-afbc <val>` 从参数里剥掉；发现 `-filter_complex` 里有 `overlay_rkrga` 时，用 sed 把滤镜图改写成软件合成：

```
scale_rkrga=format=nv12:afbc=0 → hwdownload,format=nv12
→ overlay（软件合成字幕） → format=nv12 → hwupload=derive_device=rkmpp
```

解码和编码照旧走硬件，只有字幕合成这一步落在 CPU。别删那级 `scale_rkrga`：rkmpp 解码器原始帧的 `hwdownload` 对所有格式都会被拒，过一道 `scale_rkrga` 之后的帧才下载得动，它顺带把 10bit 转成了 NV12。

其他不含 `overlay_rkrga` 的命令原样透传，网页端路径不受任何影响。如果以后 Jellyfin 改了滤镜图的拼法导致 sed 匹配不上，命令也会原样透传（代价只是绿屏回来，不会更糟）。

## 安装

纯 bash 脚本，没有编译环节。想检查语法的话：

```bash
bash -n jellyfin-rkmpp-wrapper.sh
shellcheck jellyfin-rkmpp-wrapper.sh   # 装了 shellcheck 的话
```

### 一键安装

把仓库放到板子上（scp 或 U 盘都行），中文字体可选：

```bash
sudo ./install.sh
```

`install.sh` 会：把真二进制保留为 `ffmpeg.bin`（幂等，重复跑没问题）；把 wrapper 装到 `/usr/lib/jellyfin-ffmpeg/ffmpeg`；如果脚本同目录放了 `DroidSansFallbackFull.ttf`，顺带装进 `/usr/share/fonts/truetype/droid/` 并刷新字体缓存；最后重启 jellyfin 服务。字体这步建议做——系统里没有 CJK 字体的话，烧录出来的中文字幕全是方块。

### 手动安装

不想跑脚本的话，等价的四步：

```bash
FFDIR=/usr/lib/jellyfin-ffmpeg

# 1. 真二进制改名保留（只做一次）
sudo cp -a $FFDIR/ffmpeg $FFDIR/ffmpeg.bin

# 2. 装 wrapper
sudo install -m 755 jellyfin-rkmpp-wrapper.sh $FFDIR/ffmpeg

# 3. CJK 字体（中文字幕需要）
sudo mkdir -p /usr/share/fonts/truetype/droid
sudo cp DroidSansFallbackFull.ttf /usr/share/fonts/truetype/droid/
sudo fc-cache -f

# 4. 重启服务
sudo systemctl restart jellyfin
```

### 卸载

```bash
sudo ./uninstall.sh    # 把 ffmpeg.bin 改回 ffmpeg，重启服务
```

## 验证装好了没

用 Android 客户端放一段带字幕、需要转码的视频，然后看日志：

```bash
grep -m1 "ffmpeg" /var/log/jellyfin/FFmpeg.Transcode-*.log | head -1
```

命令开头应该是 wrapper 而不是真二进制，`-afbc` 不见了，原来 `overlay_rkrga` 的位置变成了 `scale_rkrga=...hwdownload...overlay...hwupload`。画面颜色正常、中文字幕正常，就是装好了。

性能参考（1080p HEVC Main10 → H264 实测）：不带字幕烧录约 3.6 倍实时；带字幕烧录约 0.86 倍实时，掉的这部分是每帧两次内存搬运加一次 CPU 合成。1080p 24fps 还能实时，4K 或高帧率素材建议关掉字幕烧录或降分辨率。
