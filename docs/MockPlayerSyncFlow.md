# SyncCinema MockPlayer 同步链路学习文档

这份文档只解释当前代码已经实现的 MockPlayer 同步链路，不继续开发新功能。

当前项目的核心思想是：server 和 client 各自打开同一个本地视频路径，网络里只传播放控制命令，不传视频文件。默认编译时使用 `ConsoleMockPlayer`，所以现在看到的是控制台打印，而不是真实视频窗口。

## 1. 当前文件结构和职责

### 入口层

`SyncCinema.cpp`

负责程序入口和模式分发：

- 打印启动标题。
- 解析命令行参数。
- 支持 `--server <videoPath>` 和 `--client <videoPath>`。
- 把视频路径传给 `runTcpServer(videoPath)` 或 `runTcpClient(videoPath)`。

这个文件不应该关心 TCP 细节，也不应该关心播放器细节。

`SyncCinema.h`

目前只是项目默认头文件，暂时没有重要业务逻辑。

### 协议层

`Protocol.h` / `Protocol.cpp`

负责“同步消息”和“同步状态”：

- `PlaybackState`：播放状态，例如 `Stopped`、`Playing`、`Paused`。
- `SyncState`：当前同步状态，包含播放状态和播放位置。
- `MessageType`：网络命令类型，例如 `Play`、`Pause`、`Seek`。
- `SyncMessage`：结构化同步消息。
- `messageToString()`：把 `SyncMessage` 序列化成 TCP 文本，例如 `SEEK 123\n`。
- `stringToMessage()`：把 TCP 文本反序列化成 `SyncMessage`。
- `applyMessageToState()`：根据消息更新 `SyncState`。
- `stateResponseToString()`：把当前状态变成 server 返回给 client 的文本。

协议层的价值是：业务代码不要到处直接处理 `"PLAY"`、`"PAUSE"`、`"SEEK"` 这些字符串。先把字符串变成结构化的 `SyncMessage`，后面的代码更清楚，也更不容易写错。

### 播放器抽象层

`PlayerController.h` / `PlayerController.cpp`

定义播放器接口：

- `openMedia(path)`
- `play()`
- `pause()`
- `seek(seconds)`
- `getPositionSeconds()`

还定义了 `applyMessageToPlayer()`，负责把 `SyncMessage` 转换成播放器操作：

- `MessageType::Play` 调用 `player.play()`
- `MessageType::Pause` 调用 `player.pause()`
- `MessageType::Seek` 调用 `player.seek(seconds)`

这个函数很重要，因为它把“协议消息”和“播放器控制”连接起来。

### Mock 播放器

`MockPlayer.h` / `MockPlayer.cpp`

定义 `ConsoleMockPlayer`。它继承自 `PlayerController`，但是不真的播放视频，只打印日志：

- `[MockPlayer] open: <path>`
- `[MockPlayer] play`
- `[MockPlayer] pause`
- `[MockPlayer] seek to <seconds>`

它还维护一个简单的 `positionSeconds_`，让 `getPositionSeconds()` 可以返回当前模拟播放位置。

### 真实播放器封装

`LibVlcPlayer.h` / `LibVlcPlayer.cpp`

定义 `LibVlcPlayer`。它也继承自 `PlayerController`，接口和 `ConsoleMockPlayer` 一样，但内部用 libVLC 控制真实本地视频。

当前默认不会编译它。只有打开 `USE_LIBVLC` 并配置 VLC SDK 路径后，才会启用它。

### TCP client

`TcpClient.h` / `TcpClient.cpp`

负责 client 端网络和命令行循环：

- 创建播放器对象。
- 调用 `player.openMedia(videoPath)`。
- 初始化 Winsock。
- 创建 TCP socket。
- 连接 `127.0.0.1:9000`。
- 循环读取用户输入。
- 把 `play`、`pause`、`seek <seconds>` 转成 `SyncMessage`。
- 先控制 client 本地播放器。
- 再把消息序列化成字符串，通过 TCP 发给 server。
- 接收并打印 server 返回的状态。

### TCP server

`TcpServer.h` / `TcpServer.cpp`

负责 server 端网络接收和状态更新：

- 创建播放器对象。
- 调用 `player.openMedia(videoPath)`。
- 初始化 Winsock。
- 创建监听 socket。
- 绑定 9000 端口。
- 调用 `listen()` 等待连接。
- 调用 `accept()` 接收一个 client。
- 循环 `recv()` TCP 数据。
- 按 `\n` 拆成一条条消息。
- 反序列化成 `SyncMessage`。
- 控制 server 本地播放器。
- 更新 server 的 `SyncState`。
- 把当前状态返回给 client。

### 构建配置

`CMakeLists.txt`

负责把源文件加入可执行程序，链接 `ws2_32`，并提供 `USE_LIBVLC` 开关。

默认 `USE_LIBVLC=OFF`，所以使用 `ConsoleMockPlayer`，项目不依赖 VLC SDK 也能编译。

## 2. client 输入 `seek 123` 后的完整调用链

下面按时间顺序追踪一次完整流程。

### 第 0 步：启动时先打开媒体

server 启动：

```powershell
SyncCinema.exe --server "D:\videos\test.mp4"
```

调用链：

```text
main()
  -> runServer(videoPath)
  -> runTcpServer(videoPath)
  -> ActivePlayer player
  -> player.openMedia(videoPath)
```

默认情况下：

```cpp
using ActivePlayer = ConsoleMockPlayer;
```

所以 server 控制台会打印：

```text
[MockPlayer] open: D:\videos\test.mp4
```

client 启动：

```powershell
SyncCinema.exe --client "D:\videos\test.mp4"
```

调用链：

```text
main()
  -> runClient(videoPath)
  -> runTcpClient(videoPath)
  -> ActivePlayer player
  -> player.openMedia(videoPath)
```

client 也会打印：

```text
[MockPlayer] open: D:\videos\test.mp4
```

这一步说明两端都“打开了同一个本地路径”。目前 MockPlayer 不检查文件是否真实存在，它只记录并打印路径。

### 第 1 步：client 读取用户输入

用户在 client 输入：

```text
seek 123
```

`TcpClient.cpp` 中的 `runTcpClient()` 正在循环：

```text
std::getline(std::cin, line)
```

此时：

```text
line = "seek 123"
```

然后代码用 `std::istringstream` 取出第一个单词：

```text
command = "seek"
```

因为 `seek` 是需要同步的播放控制命令，所以会继续处理。

### 第 2 步：client 把输入变成 `SyncMessage`

调用：

```text
buildSyncMessageFromInput(line, message, errorMessage)
```

它会解析：

```text
seek 123
```

得到：

```cpp
message.type = MessageType::Seek;
message.positionSeconds = 123;
```

这里有一个初学者要注意的点：命令行输入是字符串，但后面的业务逻辑不应该一直拿字符串做判断。转换成 `SyncMessage` 后，代码知道这是一条“跳转到 123 秒”的结构化消息。

如果用户输入：

```text
seek abc
seek -1
seek 123 extra
```

就不会生成合法消息，而是打印错误提示。

### 第 3 步：client 先控制本地播放器

调用：

```text
applyMessageToPlayer(message, player)
```

`applyMessageToPlayer()` 看到：

```cpp
message.type == MessageType::Seek
```

于是调用：

```cpp
player.seek(message.positionSeconds);
```

当前默认 `player` 是 `ConsoleMockPlayer`，所以实际调用：

```cpp
ConsoleMockPlayer::seek(123)
```

它会做两件事：

```cpp
positionSeconds_ = 123;
std::cout << "[MockPlayer] seek to 123\n";
```

client 控制台会打印：

```text
[MockPlayer] seek to 123
```

这一步对应真实播放器版本里的“client 本地视频跳到 123 秒附近”。

### 第 4 步：client 更新本地同步状态

调用：

```text
applyMessageToState(message, localState)
```

`applyMessageToState()` 看到 `MessageType::Seek`，于是更新：

```cpp
localState.positionSeconds = 123;
```

注意：`seek` 只改变播放位置，不改变 `Playing` 或 `Paused` 状态。比如原来是播放中，seek 后仍然是播放中；原来是暂停中，seek 后仍然是暂停中。

### 第 5 步：client 把消息序列化成 TCP 文本

调用：

```text
messageToString(message)
```

输入是：

```cpp
SyncMessage { MessageType::Seek, 123 }
```

输出是：

```text
SEEK 123\n
```

最后的 `\n` 很重要。TCP 是字节流，不会自动保留“这一条 send 就是一条消息”的边界。

比如 client 连续发送：

```text
PLAY\n
SEEK 123\n
PAUSE\n
```

server 的一次 `recv()` 可能收到：

```text
PLAY\nSEEK 123\nPAUSE\n
```

也可能只收到：

```text
SEE
```

下一次才收到：

```text
K 123\n
```

所以项目约定：每条消息以 `\n` 结束。server 只要按换行拆分，就能从字节流里恢复出一条条命令。

### 第 6 步：client 通过 TCP 发送

调用：

```text
sendAll(clientSocket, tcpMessage)
```

这里的 `tcpMessage` 是：

```text
SEEK 123\n
```

`sendAll()` 内部调用 Winsock 的 `send()`。

它用循环发送，是因为 `send()` 返回的字节数可能小于你想发送的总字节数。对初学者来说，可以先记住一句话：

```text
send() 不是“保证一次发完所有数据”的函数。
```

所以 `sendAll()` 会一直发送，直到整条字符串都发完，或者遇到错误。

client 控制台会打印：

```text
sent: SEEK 123
```

### 第 7 步：server 从 TCP 收到字节

server 在 `handleClient()` 里循环调用：

```text
recv(clientSocket, buffer, sizeof(buffer), 0)
```

收到字节后，先追加到：

```cpp
receiveBuffer
```

然后查找：

```cpp
receiveBuffer.find('\n')
```

找到换行后，拆出一行：

```text
SEEK 123
```

接着调用：

```text
processOneLine(clientSocket, line, serverState, player)
```

### 第 8 步：server 反序列化消息

`processOneLine()` 先打印原始消息：

```text
received raw: SEEK 123
```

然后调用：

```text
stringToMessage(line)
```

输入：

```text
SEEK 123
```

输出：

```cpp
message.type = MessageType::Seek;
message.positionSeconds = 123;
```

server 控制台会打印：

```text
parsed message: SEEK 123
```

### 第 9 步：server 控制自己的播放器

server 调用：

```text
applyMessageToPlayer(message, player)
```

和 client 一样，这会走到：

```cpp
player.seek(123)
```

默认 `player` 是 `ConsoleMockPlayer`，所以打印：

```text
[MockPlayer] seek to 123
```

这一步对应真实播放器版本里的“server 本地视频也跳到 123 秒附近”。

### 第 10 步：server 更新自己的同步状态

server 调用：

```text
applyMessageToState(message, serverState)
```

于是：

```cpp
serverState.positionSeconds = 123;
```

server 控制台打印：

```text
server status: state=Playing, position=123 seconds
```

如果 seek 前状态是 `Paused`，这里会显示：

```text
server status: state=Paused, position=123 seconds
```

### 第 11 步：server 返回状态给 client

server 调用：

```text
stateResponseToString(serverState)
```

比如当前状态是播放中，位置 123 秒，返回：

```text
STATE Playing 123\n
```

然后通过：

```text
sendAll(clientSocket, response)
```

发回 client。

### 第 12 步：client 接收 server 响应

client 调用：

```text
receiveLine(clientSocket, receiveBuffer, serverResponse)
```

它同样按 `\n` 拆分 server 响应。收到后打印：

```text
server response: STATE Playing 123
```

到这里，`seek 123` 这条命令的完整链路结束。

## 3. 为什么要抽象 `PlayerController`

如果没有 `PlayerController`，TCP 代码可能会直接写成这样：

```cpp
if (message.type == MessageType::Seek)
{
    libvlc_media_player_set_time(...);
}
```

这样会有几个问题：

1. TCP 代码会被 libVLC API 污染。
2. 想用 MockPlayer 做测试会很麻烦。
3. 以后想换播放器库，需要改很多网络代码。
4. 初学时很难分清“网络同步”和“播放器控制”两个概念。

现在有了 `PlayerController`：

```cpp
PlayerController& player
```

TCP 层只知道：

```cpp
player.play();
player.pause();
player.seek(seconds);
```

它不关心底层到底是：

- 控制台打印。
- libVLC 播放窗口。
- 将来可能的其他播放器。

这就是接口抽象的意义：调用方依赖稳定的小接口，不依赖复杂的具体实现。

## 4. MockPlayer 和 LibVlcPlayer 的关系

`ConsoleMockPlayer` 和 `LibVlcPlayer` 是同一套接口的两个实现。

它们都继承：

```cpp
PlayerController
```

所以它们都必须实现：

```cpp
openMedia()
play()
pause()
seek()
getPositionSeconds()
```

区别是：

`ConsoleMockPlayer`

- 默认启用。
- 不需要任何第三方库。
- 不打开真实视频窗口。
- 只打印日志。
- 适合学习 TCP 链路和调试同步逻辑。

`LibVlcPlayer`

- 需要手动打开 `USE_LIBVLC`。
- 需要配置 VLC SDK。
- 会控制真实本地视频播放。
- 适合 MockPlayer 链路跑通后再接入。

可以把它们理解成：

```text
PlayerController 是插座规格
ConsoleMockPlayer 是一个测试插头
LibVlcPlayer 是一个真实播放器插头
```

TCP 层只认插座规格，不关心插头内部怎么工作。

## 5. `USE_LIBVLC` 开关的作用

`USE_LIBVLC` 是 CMake 编译开关。

默认：

```cmake
option(USE_LIBVLC "Use libVLC as the real local video player" OFF)
```

也就是默认不启用 libVLC。此时 `TcpClient.cpp` 和 `TcpServer.cpp` 会选择：

```cpp
using ActivePlayer = ConsoleMockPlayer;
```

如果编译时打开：

```text
-DUSE_LIBVLC=ON
```

并配置好：

```text
VLC_INCLUDE_DIR
VLC_LIBRARY
VLC_CORE_LIBRARY
```

那么代码会选择：

```cpp
using ActivePlayer = LibVlcPlayer;
```

这意味着 TCP 同步链路不变，只是播放器实现从“打印日志”换成“真实控制视频”。

当前阶段不用深究 libVLC 的内部 API。你只需要先理解：`USE_LIBVLC` 是用来在 Mock 播放器和真实播放器之间切换的。

## 6. 现在最应该学习的 5 个函数

### 1. `runTcpClient(const std::string& videoPath)`

位置：`TcpClient.cpp`

这是 client 的主流程。建议重点看：

- 如何打开播放器。
- 如何连接 server。
- 如何读取用户输入。
- 如何把命令转成消息。
- 如何先控制本地播放器，再发送 TCP 消息。

### 2. `buildSyncMessageFromInput(...)`

位置：`TcpClient.cpp`

这是“用户输入字符串”到“结构化协议消息”的转换点。

重点理解：

- 为什么 `seek 123` 要解析成 `MessageType::Seek` 和 `positionSeconds = 123`。
- 为什么非法参数要提前拦住。

### 3. `messageToString()` 和 `stringToMessage()`

位置：`Protocol.cpp`

这两个函数是一对：

- `messageToString()`：结构体到字符串。
- `stringToMessage()`：字符串到结构体。

它们是 TCP 文本协议的核心。

### 4. `applyMessageToPlayer(...)`

位置：`PlayerController.cpp`

这是协议层和播放器层之间的桥。

重点理解：

- `Play` 对应 `player.play()`。
- `Pause` 对应 `player.pause()`。
- `Seek` 对应 `player.seek(seconds)`。

### 5. `processOneLine(...)`

位置：`TcpServer.cpp`

这是 server 收到一条完整消息后的处理中心。

它按顺序做了：

1. 打印原始消息。
2. 反序列化成 `SyncMessage`。
3. 控制 server 本地播放器。
4. 更新 server 状态。
5. 返回当前状态给 client。

这个函数把 server 的同步逻辑串起来了，非常值得慢慢读。

## 7. 暂时不用深究的代码

### `LibVlcPlayer.cpp`

现在默认使用 MockPlayer。libVLC 是下一阶段的真实播放器细节，先不用急着研究。

你只需要知道它也实现了 `PlayerController`。

### CMake 里的 VLC 路径配置

`VLC_INCLUDE_DIR`、`VLC_LIBRARY`、`VLC_CORE_LIBRARY` 是为了以后启用真实播放器准备的。

当前学习 Mock 链路时，保持 `USE_LIBVLC=OFF` 即可。

### `sendAll()` 的所有边界细节

你现在需要知道它是“尽量把整个字符串发完”的 helper 函数。

里面的循环和错误处理很重要，但可以等你对 TCP 更熟后再深究。

### `receiveLine()` 和 `handleClient()` 中的半包、粘包处理

现在先记住：

```text
TCP 是字节流，没有消息边界，所以项目用 '\n' 分隔消息。
```

至于一次 `recv()` 为什么可能收到半条或多条消息，可以等你开始系统学习 TCP 时再展开。

### Winsock 初始化和清理细节

`WSAStartup()` 和 `WSACleanup()` 是 Windows socket 编程的固定准备和收尾动作。

目前先记住：

- 使用 socket 前调用 `WSAStartup()`。
- 程序结束网络使用时调用 `WSACleanup()`。

## 8. 用一句话总结当前链路

client 输入：

```text
seek 123
```

会变成：

```text
用户输入
  -> SyncMessage
  -> client MockPlayer seek
  -> "SEEK 123\n"
  -> TCP send
  -> server recv
  -> SyncMessage
  -> server MockPlayer seek
  -> server SyncState 更新
  -> "STATE ... 123\n"
  -> client 打印响应
```

这就是当前 SyncCinema 的 MockPlayer 同步链路。

