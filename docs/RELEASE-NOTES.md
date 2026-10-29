## New in this build

A hotfix of v20261029. Your precompiled shaders stay valid: the shaders are the same as in v20261029.

- **Textures and lights flickering every other frame** (#242, a v20261029 regression): v20261029 decided its memory
  savings by the RAM free for the emulator at start instead of the PC's total, and on a 24 or 32 GB PC with other
  programs open that turned on read guards, which v20261028 never used there. When the RAM then ran full, things the
  game reads back from the graphics card were sometimes read old: textures, lights and objects there one frame and gone
  the next. Read guards go by the PC's total RAM again (20 GB or less); the other savings keep the new rule (#228).
- **Frame generation turning off** (#241): looking at the Nexus floor or at a wall close up, frame generation turned
  itself off until the view changed: its camera came only from a few of the game's shaders, which such views don't
  draw. Those frames now keep the last camera. The log says when frame generation turns on or off and why
  (`Frame generation: DLSS-G off: 30 frames without inputs (no camera)`). In the 120 fps mode a frame needs one draw of
  the world instead of 64 to count as 3D, so close views are no longer held to 60.
- **Microsoft Defender** (#240): the README in the package explains that Defender's scans of the game's files cost
  frame time while the game streams (@DevList1454 measured the 99th percentile frame time 31.5 -> 26 ms with the folder
  excluded), and that a `.zar` copy of the game (one file) or a folder exclusion avoids them.
- **Thanks** to @luizhtss, @javsp, @zioaster and @DevList1454 for their reports and measurements.

## New in v20261029

Your precompiled shaders stay valid: the shaders are the same as in v20261028.

- **Steam Deck package and guide**: releases now have a third package, `KytyPS5-steamdeck-<version>.zip`, the emulator
  built for the Steam Deck, with `run-deck.sh` (starts the game with the Deck's settings) and a guide, `STEAM-DECK.md`
  (Chinese: `STEAM-DECK.zh-CN.md`; both also in the Linux package): copying the game and the emulator over, starting it
  from Steam with a 1300 MHz GPU profile, and what it runs at. On a Steam Deck LCD with `run-deck.sh`'s settings (the
  Lowest preset, the 3D at 640x360, no projected decals, sun shadows within 20 m) the worlds run at about 26-30 fps and
  the Nexus at 40. The Steam Deck build keeps a CPU core for the render thread, which also makes the upload copies the
  upload worker has not reached; it watches the GPU's memory with userfaultfd, which costs the Deck's 4 cores less than
  mprotect calls; its translated shaders share address page walks; its sound goes through ALSA (about a third of a CPU
  less; `SDL_AUDIODRIVER=pulseaudio` for the old way); the mouse cursor is hidden (`KYTY_HIDE_CURSOR=0` shows it); and the
  keys Steam's desktop layout sends are ignored (`KYTY_DECK_KEYBOARD=1` keeps them; Space no longer pauses the game).
  The Linux package with `run-deck.sh` measured the same frame rate on the Deck. Start it once only: two copies at once
  share the Deck.
- **Linux and the Steam Deck**: a hang at a world load is fixed (one page's writes faulted forever in the Steam Deck
  build). Memory the game frees now goes back to the system on Linux PCs with 16 GB or less, as on Windows (a Deck tour:
  5.2 -> 3.0 GB in swap, frames over 50 ms 7-8 -> 1). With an integrated GPU the upload ring is 128 MB instead of 512 MB
  (about 0.4 GB more RAM for the game on a Deck).
- **Less GPU time checking the GPU's reads** (every PC): after each submission the emulator read its whole record of
  the guest memory the GPU touched outside its buffers (8 MB, 7 times a frame); it now reads only the part over
  memory the game has committed. On a Steam Deck at Latria that is 0.98 -> 0.03 ms of GPU time a frame.
- **Loading screens' fog** (#234): the fog of the loading screens flickered in every setting. The game's frames are
  10-bit; the emulator copied them into the 8-bit picture on the screen, each pixel rounded, and the dark, slowly moving
  fog showed bands that crawled. The copy now adds a fine fixed dither (under half a step, the same every frame), and
  the bands are gone. HDR output (10-bit) is unchanged. `-Set KYTY_PRESENT_DITHER=0` goes back to the old copy.
- **Flames, smoke and glows with DLSS or FSR 3** (#232, #85): v20261028's texture LOD bias (DLSS or FSR 3 below the
  output) applied to every texture, and flames, smoke and glows flickered. It now applies only to the surfaces'
  materials (the passes that draw the scene's surfaces and depth), which keep nearly all of its sharpness;
  `-Set KYTY_UPSCALE_LOD_BIAS_TARGETS=0` applies it to every pixel shader again. DLSS and FSR 3 also get the game's
  reactive mask now, as Sony's upscaler does: the pixels to take from the new frame rather than the history (fire,
  smoke, the soul form's glow). `-Set KYTY_SR_REACTIVE=0` leaves it out. (Linux DLSS does not take it yet.)
- **Xbox and other controllers vibrate** (#236): the game's vibration is made for the DualSense's haptics; pads without
  them (an Xbox controller, the Steam Deck's) now rumble with it. `-Set KYTY_PAD_RUMBLE_GAIN=1` is gentler (default 2,
  0 off). Not tried with a real pad on our side yet: please tell us how it feels.
- **Lowest preset: no projected decals**: the Lowest graphics preset also turns off the decals laid onto surfaces
  (blood, scorch marks). On an RTX 5090 at Boletaria: +12% fps standing, +7% walking, the graphics card's work a frame
  -22%.
- **PCs with few CPU cores**: when the PC's cores are busy, the render thread now makes the upload copies the busy
  upload worker has not reached (on 4 CPUs of the test PC +9.5% fps at Boletaria, on 6 CPUs +3%; with cores to spare
  nothing changes). `-Set KYTY_UPLOAD_HELP=0` turns it off.
- **Memory savings by the RAM free for the emulator** (#228): the savings for PCs short of RAM now go by the RAM the
  emulator gets at start (the PC's RAM less what other programs hold), not by the PC's RAM: a 24 GB PC whose programs
  hold 10 GB gets them now. The log's first line says how much it had.
- **Sharpening for FSR 3** (#227): the launcher's Graphics page has one "Sharpening" row (Off, Low, Medium, High) for the
  chosen upscaler: DLSS (Off by default) or FSR 3 (Medium by default, as before).
- **VRR displays in the launcher** (#221): "Flip frames when ready (VRR displays)" on the launcher's Experimental page
  (`KYTY_VRR=1`): on a G-SYNC or FreeSync display each frame is shown as soon as it is done.
- **8-12 GB cards: fewer stutters after a load** (#129): the start-up warmup now makes the map's compute pipelines up
  to a quarter of the video memory budget (was a sixteenth).
- **120 fps mode** (#231): the start-up logos no longer play at double speed.
- **GPUs without fragment shader barycentrics** (PR #235, @kleinergoo7): no longer refused at start (AMD Polaris and
  RDNA1 on RADV, a GTX 970). Shaders that read per-vertex attributes still need them.
- **Linux: CPU and RAM on the detailed FPS panel** (PR #237, @intellq).
- **Precompile** (#225): a damaged part no longer fails the whole merge (its few pipelines are left out and compiled in
  the game), and a crash of the precompile says where in its log.
- **Back to Big Picture** (#217): the focus is also handed over as Alt+Tab does.
- **F9 removed**: the debug snapshot (which paused the game) is gone; F9 is free for keymaps.
- **Thanks** to @kleinergoo7 for PR #235 and @intellq for PR #237, and to @nzc20090108, @forensicssix-debug,
  @brokeneggg, @Ayonily, @KhadaZane, @theunholy, @crni20zd, @2mg, @theraineydaze, @javsp, @Psychowarden and
  @DevList1454 for their reports and tests.

## 本版本的变化

v20261029 的修正版。已有的预编译着色器继续有效：着色器与 v20261029 相同。

- **纹理和光照隔帧闪烁**（#242，v20261029 引入的问题）：v20261029 改为按启动时模拟器可用的内存（而不是电脑的总内存）决定省内存措施，
  在开着其他程序的 24 或 32 GB 电脑上因此开启了"读保护"（v20261028 在这些电脑上从不使用）。内存用满时，游戏从显卡读回的数据有时会
  读到旧的：纹理、光照和物体这一帧在、下一帧没了。读保护重新按电脑的总内存决定（20 GB 及以下）；其他省内存措施仍按新规则（#228）。
- **插帧自己关闭**（#241）：看着要石神殿的地面或贴近墙壁时，插帧会自己关闭，直到视角改变：它的相机信息只来自游戏的少数几个着色器，
  这类视角里没有绘制它们。这些帧现在沿用上一次的相机。日志会写出插帧何时开启、关闭以及原因
  （`Frame generation: DLSS-G off: 30 frames without inputs (no camera)`）。120 帧模式下，一帧只要有一次世界的绘制就算 3D 画面
  （原为 64 次），贴近的视角不再被限制在 60 帧。
- **Microsoft Defender**（#240）：包内的 README 说明了 Defender 扫描游戏文件会在游戏读取资源时占用帧时间（@DevList1454 测得排除
  游戏文件夹后，99% 帧时间从 31.5 ms 降到 26 ms），使用 `.zar` 格式的游戏（一个文件）或把游戏文件夹加入排除项可以避免。
- **感谢** @luizhtss、@javsp、@zioaster 和 @DevList1454 的反馈与测量。

## v20261029 的变化

已有的预编译着色器继续有效：着色器与 v20261028 相同。

- **Steam Deck 版的包和说明**：发布页现在有第三个包 `KytyPS5-steamdeck-<版本>.zip`，是为 Steam Deck 编译的模拟器，附带
  `run-deck.sh`（用 Deck 的设置启动游戏）和说明 `STEAM-DECK.zh-CN.md`（英文 `STEAM-DECK.md`；Linux 包里也有）：怎样把游戏和
  模拟器拷到 Deck、怎样从 Steam 启动并使用 1300 MHz 的 GPU 配置，以及能跑多少帧。在 Steam Deck LCD 上用 `run-deck.sh` 的设置
  （「最低」画质预设、3D 画面 640x360、关闭投影贴花、太阳阴影只到 20 米），各个世界约 26-30 fps，要石神殿约 40 fps。Steam
  Deck 版给渲染线程单独留一个 CPU 核心，渲染线程也会帮上传线程做它还没轮到的上传复制；用 userfaultfd 监视显卡用到的内存，在
  Deck 的 4 个核心上比 mprotect 调用省得多；翻译后的着色器共享地址的页表查找；声音改走 ALSA（约少用三分之一个 CPU；
  `SDL_AUDIODRIVER=pulseaudio` 恢复旧方式）；鼠标指针隐藏（`KYTY_HIDE_CURSOR=0` 显示）；Steam 桌面布局发出的键盘按键被忽略
  （`KYTY_DECK_KEYBOARD=1` 保留；空格键不再暂停游戏）。在 Deck 上用 Linux 包加 `run-deck.sh` 测得的帧率相同。请只启动一份：
  同时运行两份会分享 Deck 的性能。
- **Linux 和 Steam Deck**：修复了载入世界时卡死（Steam Deck 版中某一页的写入无限次触发异常）。在内存 16 GB 及以下的 Linux
  电脑上，游戏释放的内存现在会归还给系统，与 Windows 相同（Deck 上跑一圈世界：交换区 5.2 -> 3.0 GB，超过 50 ms 的帧
  7-8 -> 1）。使用集成显卡时上传环形缓冲区为 128 MB（原为 512 MB），Deck 上给游戏多留约 0.4 GB 内存。
- **检查显卡读取所用的 GPU 时间更少**（所有电脑）：模拟器每次提交后都要读取显卡在缓冲区之外访问过的客户机内存记录（8 MB，
  每帧 7 次）；现在只读取游戏已提交内存所对应的部分。在 Steam Deck 的拉特利亚，每帧 GPU 时间从 0.98 ms 降到 0.03 ms。
- **加载界面的雾气**（#234）：加载界面的雾气在任何设置下都会闪烁。游戏的画面是 10 位色深，模拟器把它复制到屏幕上 8 位的画面
  时每个像素四舍五入，又暗又缓慢移动的雾气上就出现一圈圈爬动的色阶条纹。现在复制时加入一层细微的固定抖动（小于半级，每帧
  相同），条纹消失了。HDR 输出（10 位）不变。`-Set KYTY_PRESENT_DITHER=0` 恢复原来的复制方式。
- **DLSS 或 FSR 3 下的火焰、烟雾和光晕**（#232、#85）：v20261028 的纹理 LOD 偏移（DLSS 或 FSR 3 渲染低于输出时）作用于所有
  纹理，火焰、烟雾和光晕因此闪烁。现在只作用于场景表面的材质（绘制场景表面和深度的渲染过程），清晰度几乎不变；
  `-Set KYTY_UPSCALE_LOD_BIAS_TARGETS=0` 恢复为对所有像素着色器生效。DLSS 和 FSR 3 现在也会拿到游戏的反应遮罩（reactive
  mask），和索尼的超分一样：哪些像素应取自新的一帧而不是历史（火、烟、灵体的光晕）。`-Set KYTY_SR_REACTIVE=0` 不传给它们。
  （Linux 的 DLSS 暂未使用。）
- **Xbox 等手柄也会震动**（#236）：游戏的震动是为 DualSense 的触觉反馈做的；没有触觉反馈的手柄（Xbox 手柄、Steam Deck
  自带的手柄）现在会随之震动。`-Set KYTY_PAD_RUMBLE_GAIN=1` 更轻（默认 2，0 关闭）。我们这边还没有用真手柄试过：欢迎告诉
  我们手感如何。
- **「最低」预设关闭投影贴花**：「最低」画质预设现在也会关闭贴在表面上的贴花（血迹、焦痕）。RTX 5090 在波雷塔利亚：站立
  +12% fps，行走 +7%，显卡每帧的工作量 -22%。
- **CPU 核心少的电脑**：CPU 忙不过来时，渲染线程现在会帮忙完成上传线程还没轮到的上传复制（测试电脑限 4 个 CPU 时，波雷
  塔利亚 +9.5% fps；6 个 CPU +3%；CPU 有富余时没有变化）。`-Set KYTY_UPLOAD_HELP=0` 关闭。
- **按模拟器可用的内存决定省内存措施**（#228）：为内存不足的电脑准备的省内存措施，现在按模拟器启动时可用的内存（电脑内存减去
  其他程序占用的部分）决定，而不是按电脑的总内存：一台 24 GB、其他程序占了 10 GB 的电脑现在也会启用它们。日志第一行会写出
  可用的内存。
- **FSR 3 的锐化**（#227）：启动器「画质」页改为一行「锐化」（关、低、中、高），对应所选的超分：DLSS（默认关）或 FSR 3
  （默认中，与以前相同）。
- **启动器里的 VRR 选项**（#221）：启动器「实验性」页新增「画面完成即翻转（VRR 显示器）」（`KYTY_VRR=1`）：在 G-SYNC 或
  FreeSync 显示器上，每一帧画完就立即显示。
- **8-12 GB 显卡载入后卡顿更少**（#129）：启动时的预热现在最多用显存预算的四分之一来生成地图的计算管线（原为十六分之一）。
- **120 fps 模式**（#231）：启动时的标志动画不再以两倍速度播放。
- **不支持片段着色器重心坐标的显卡**（PR #235，@kleinergoo7）：启动时不再被拒绝（RADV 上的 AMD Polaris、RDNA1，GTX 970）。
  读取逐顶点属性的着色器仍然需要它。
- **Linux：详细帧率面板显示 CPU 和内存**（PR #237，@intellq）。
- **预编译**（#225）：某一部分损坏时不再让整个合并失败（其中少量管线被跳过，游戏中用到时再编译）；预编译程序崩溃时，日志里
  会写出崩溃的位置。
- **回到 Big Picture**（#217）：焦点交还时也会像 Alt+Tab 那样切换。
- **移除 F9**：调试快照（会暂停游戏）已移除；F9 可以用于按键映射。
- **感谢** @kleinergoo7 的 PR #235 和 @intellq 的 PR #237，以及 @nzc20090108、@forensicssix-debug、@brokeneggg、@Ayonily、
  @KhadaZane、@theunholy、@crni20zd、@2mg、@theraineydaze、@javsp、@Psychowarden 和 @DevList1454 的反馈与测试。
