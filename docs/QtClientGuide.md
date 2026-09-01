# SyncCinema Qt Client MVP

## 1. 当前界面包含什么

`SyncCinemaQt.exe` 是 Windows 桌面客户端。第一版以播放器常见布局为基础：

- 顶部：媒体文件/HTTP URL、文件选择按钮、server 地址、连接状态。
- 中部：libVLC 原生视频画面。
- 底部：播放/暂停、进度条、时间、音量、全屏。

界面只负责展示和接收用户操作。TCP、协议解析、初始房间快照、心跳响应和
进度上报都由不依赖 Qt 的 `SyncClientSession` 负责。

## 2. 构建

当前开发机可以使用以下 preset：

```bat
cmake --preset x64-qt-vlc-release
cmake --build out\build\x64-qt-vlc-release --target SyncCinemaQt
```

如果普通命令行找不到 `cmake`，可以直接在 Visual Studio 中选择
`x64 Release with Qt and libVLC` 配置后执行“生成全部”。

构建产物：

```text
out\build\x64-qt-vlc-release\SyncCinemaQt.exe
```

CMake 会在构建后调用 `windeployqt`，并把 Qt DLL、Qt platform plugin、
libVLC DLL 和 VLC plugins 复制到可执行文件所在目录。将来发给测试用户时，
应打包整个目录，不能只发送一个 `.exe`。

## 3. 运行

1. 确认云服务器上的 `SyncCinemaServer` 正在运行，安全组已放行 TCP 9000。
2. 启动 `SyncCinemaQt.exe`。
3. 在“媒体文件或 URL”中填写双方相同的媒体来源，例如：

```text
http://<server-ip>/videos/test.mp4
```

4. 在“服务器地址”中填写公网 IP 或域名，不要带 `http://` 和端口。
5. 点击“连接”。
6. 连接成功后可使用播放、暂停、拖动进度、音量和全屏。

连接时 client 会先发送当前媒体的稳定身份：

- 空房间由第一个 client 建立媒体会话，并从 `Stopped / 0` 开始。
- 后续 client 只有打开同一媒体时才能加入。
- 如果房间仍有人观看另一媒体，连接会明确提示媒体不匹配。
- 最后一个 client 离开后，本次媒体会话结束；下一部媒体不会继承旧进度。

播放请求由 libVLC 异步处理，界面会在视频区域显示“正在打开媒体”和实时缓冲百分比。
如果 URL 不存在、格式无法播放或媒体服务器异常，client 会显示明确错误并自动离开房间，
避免黑屏客户端继续上报无效进度。播放结束时视频区域会显示“播放结束”。

媒体地址、server 地址和音量会通过 `QSettings` 保存在本机，下次启动自动恢复。

## 4. 代码结构

- `QtClientMain.cpp`：Qt 应用入口。
- `QtClientWindow.h/.cpp`：窗口布局、控件状态和用户交互。
- `QtClientController.h/.cpp`：把 Qt 信号/槽适配到纯 C++ 会话层。
- `SyncClientSession.h/.cpp`：TCP 生命周期、协议消息、播放器互斥和后台线程。
- `PlayerController.h`：Qt 与命令行共同依赖的播放器控制和异步事件抽象。
- `LibVlcPlayer.h/.cpp`：libVLC 播放、原生窗口嵌入、进度、时长和音量。

## 5. 当前边界

- 第一版只有一个固定房间，server 端口仍是 9000。
- 暂无账号、房间码、聊天、弹幕和自动更新。
- 同步校正策略仍是 server 端只读建议，不会自动改变客户端进度。
- 当前媒体身份由规范化来源字符串的稳定哈希生成；它用于一致性判断，不是安全认证。
- 当前 preset 使用本机已有 Qt/VLC SDK 路径；发布前应改为正式 Qt SDK 和可复现的打包流程。
- `openMedia()` 只创建媒体对象；远端资源是否可播放，要在真正开始播放后由异步事件确认。
