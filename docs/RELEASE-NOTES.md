## New in this build

Your precompiled shaders stay valid: the shaders are the same as in v20261030.

- **21:9 and 32:9: movies and menus at 16:9 between black bars** (#250): a new option on the launcher's Display page,
  "Movies and menus at 16:9 (black bars)". With 21:9 or 32:9 the intro, the movies, the title screen, the loading
  screens and the menus opened in play stay at 16:9 in the middle, with black bars at the sides, instead of being
  stretched to the width. It works with DLSS or FSR 3 and with the game's own anti-aliasing alike; the 3D view stays
  wide.
- **21:9 and 32:9 with FSR 3 frame generation** (#250): the HUD no longer leaves a ghost while you move, and the start-up
  logos are no longer cut off at the right.
- **Frame generation with "Direct presentation" on** (#236): the game ran at half the display's rate (30 fps on an RTX
  4090 at 120 Hz with FSR 3 frame generation), and each frame waited a frame longer to be shown. That Experimental
  switch is now ignored while frame generation is on.
- **AMD: smoother menus with FSR 3 frame generation and low latency** (#252): AMD Anti-Lag stays off while frame
  generation is on. With both, an RX 6650 XT's menus and shops dropped to 5-6 fps.
- **Fewer exits when video memory runs out** (#252): when the graphics card has no room for an image the game needs (an
  8 GB card with frame generation at 3840x1200), the emulator now also frees old render targets, saving their contents
  first, and tries again for up to 10 seconds before it gives up.
- **Intel Arc: no more full freezes** (#255): an Arc B580 froze completely after a while in 1-2 and 3-1. The data the
  emulator reads back from the graphics card is now copied on its main queue (`-Set KYTY_READBACK_QUEUE=1` goes back to
  the copy queue).
- **Xbox and other non-PlayStation pads** (#236): running no longer breaks off: the stick counts as fully pushed from 90%
  of its travel (`-Set KYTY_PAD_DEADZONE_OUTER=<percent>`). The long rumble at the title screen, at Continue and at each
  spawn now fades out in about half a second, while hits still rumble fully (`-Set KYTY_PAD_RUMBLE_SUSTAIN_MS=<ms>`, 0
  as before). The Steam Deck's rumble too.
- **Button presses timed in the log** (#236): every 30 seconds with presses, an `Input:` line says how long a press
  waited before the emulator took it and before the game read it, for reports of input lag.
- **Launcher: more render heights for DLSS and FSR 3** (#129): Balanced at 4K (58%, 1256 lines) and Ultra Quality (77%:
  1664 at 4K, 1104 at 1440p, 832 at 1080p).
- **Linux: DualSense haptics over USB**, as an option (PR #254, credit @ponhei9502): `KYTY_DUALSENSE_HAPTICS=1` plays
  the game's haptics on the pad's actuators (off by default).
- **Thanks** to @ponhei9502 for PR #254, and to @H0A0M0mer, @Ayonily, @labs20, @STALKERSK, @thxnk420, @markusdev and
  @2mg for their reports and tests.

## New in v20261030

Your precompiled shaders stay valid: for as long as you keep them, the shaders are translated as before. Your next
precompile (a new install, or after a graphics driver update) brings two shader fixes (last items); the launcher says
when that precompile compiles all of them again.

- **FSR 3 upscaling: no more grid of blocks and sparkling edges** (#244, a v20261029 regression): v20261029 handed the
  game's reactive mask to FSR 3, which took whole areas from the current frame alone. Every FSR 3 upscaler user had it
  (AMD cards, and RTX 20/30 with FSR frame generation).
- **DLSS: no more Streamline error lines flooding the log** (#129, #85, a v20261029 regression): Streamline refused the
  reactive mask's tag every frame (`Cannot have undefined format`, 100+ lines a second). The mask is no longer handed to
  either upscaler, and a Streamline error that repeats is printed 3 times, then every 1000th with its count.
- **Loads stuck in the fog** (#249): on some PCs a world load never finished (the fog moving, the game reading nothing
  more) once the emulator's read-ahead had about 65 thousand of the game's files open, apparently all the drive takes.
  The read-ahead now stops at 48 thousand, and if an open of the game's fails it gives back the files it opened ahead
  and tries again (the log says so). `-Set KYTY_FILE_FOLLOW=0`, which worked around it, is no longer needed.
- **F11 with FSR 3 frame generation** (#174): leaving or entering full screen with F11 could lose the graphics device (a
  crash), seen on an RTX 2060 SUPER at 165 Hz: the old swapchain's parts were freed while FSR's own presents still used
  them.
- **DLSS upscaling with AMD FSR 3 frame generation** (#244): an RTX 20 or 30 card has no DLSS frame generation, so with
  frame generation on it ran FSR 3's, and FSR 3 upscaling replaced DLSS. Now DLSS upscales and AMD's runtime generates
  the frames. `-Set KYTY_FSR_FG_DLSS=0` goes back to FSR 3 upscaling.
- **DualSense: no rumble for the whole of a weapon enchantment** (#239): the enchantment's quiet, continuous haptic
  effect stays on the haptic actuators, as on a PS5; hits rumble as before (`KYTY_DUALSENSE_VIBRATION_MOTOR_ONSET`, and
  `KYTY_PAD_RUMBLE_ONSET` for other pads).
- **Xbox and other non-PlayStation pads** (#236): their sticks get a round dead zone of 12% (the character no longer
  keeps walking slowly by itself after a run; `-Set KYTY_PAD_DEADZONE=<percent>`, 0 off), and their rumble from the
  game's vibration is half as strong by default (`KYTY_PAD_RUMBLE_GAIN`, now 1).
- **The frame rate panel shows shader compiles** (#129): "Compiling shaders: N (X s)" while the game compiles pipelines
  the precompiled set doesn't have, the stutter of a first use.
- **NVIDIA Low Latency Mode Ultra** (#129): it makes the emulator's presents wait (uneven frame times, RTSS and the panel
  disagree, frames lost). The emulator notices it (NVIDIA card, VSync off) and says so in the log and on the game for
  20 s: set Low Latency Mode to Off or On for `kyty_emulator.exe`.
- **Launcher: the precompile says when it redoes everything** (#246): "precompile again (this time all of them, once)"
  for a store made before a translation that changes nearly every shader, instead of "only what is new".
- **Launcher: DLSS 4.5's presets M and L** in the DLSS model choice (#248); `-Set KYTY_DLSS_PRESET=M` or `L` also works.
- **With your next precompile**: the loading screen's fog no longer flashes brown patches for a frame (PR #245, credit
  @DevList1454: four multiply-add instructions rounded as the hardware does; `KYTY_UNFUSED_MAD=0/1`), and pixel shaders'
  derivatives are right at triangle edges (95 shaders: the debug menu font's lines; PR #243, credit @DevList1454;
  `KYTY_PIXEL_QUAD_READS=0/1`).
- **Thanks** to @Fantomas3228, @2mg, @brokeneggg, @jrodriguezml, @zioaster, @markusdev, @dimitrios1977ZXZ, @ft1704,
  @Ayonily and @DevList1454 for their reports, tests and pull requests.

## 本版本的变化

已有的预编译着色器继续有效：着色器与 v20261030 相同。

- **21:9 和 32:9：过场动画与菜单保持 16:9（黑边）**（#250）：启动器「画面」页新增选项「过场动画与菜单保持 16:9（黑边）」。
  选 21:9 或 32:9 时，片头、过场动画、标题画面、加载画面以及游戏中打开的菜单都保持 16:9 居中，两侧为黑边，不再拉伸到整个
  宽度。使用 DLSS、FSR 3 或游戏自带的抗锯齿时都有效；3D 画面仍是宽屏。
- **21:9 和 32:9 + FSR 3 插帧**（#250）：移动时 HUD 不再留下残影，启动时的标志动画右侧也不再被裁掉。
- **插帧 + 开启「直接呈现」**（#236）：游戏只能跑到显示器刷新率的一半（RTX 4090、120 Hz、FSR 3 插帧时为 30 fps），每一帧也要多等
  一帧才显示。现在开启插帧时会忽略这个「实验性」开关。
- **AMD：FSR 3 插帧 + 低延迟时菜单更流畅**（#252）：开启插帧时 AMD Anti-Lag 保持关闭。两者同时开启时，RX 6650 XT 在菜单和商店
  里会掉到 5-6 fps。
- **显存不足时更少退出**（#252）：显卡放不下游戏需要的图像时（例如 8 GB 显卡在 3840x1200 下开启插帧），模拟器现在还会释放旧的
  渲染目标（先保存其内容），并在放弃之前最多重试 10 秒。
- **Intel Arc：不再整个卡死**（#255）：Arc B580 在 1-2 和 3-1 玩一段时间后会完全卡死。模拟器从显卡读回的数据现在在主队列上复制
  （`-Set KYTY_READBACK_QUEUE=1` 恢复为复制队列）。
- **Xbox 等非 PlayStation 手柄**（#236）：跑步不再中断：摇杆推到 90% 即视为推满（`-Set KYTY_PAD_DEADZONE_OUTER=<百分比>`）。
  标题画面、「继续」和每次复活时的长时间震动现在约半秒内渐弱，受击等仍会完整震动（`-Set KYTY_PAD_RUMBLE_SUSTAIN_MS=<毫秒>`，
  0 恢复原样）。Steam Deck 的震动也一样。
- **日志记录按键耗时**（#236）：有按键时每 30 秒一行 `Input:`，写出按键等了多久才被模拟器接收、又过了多久被游戏读取，便于反馈
  输入延迟。
- **启动器：DLSS 和 FSR 3 增加渲染高度**（#129）：4K 下的「平衡」（58%，1256 行）和「超级质量」（77%：4K 为 1664，1440p 为
  1104，1080p 为 832）。
- **Linux：DualSense 通过 USB 的触觉反馈**（可选，PR #254，感谢 @ponhei9502）：`KYTY_DUALSENSE_HAPTICS=1` 让游戏的触觉效果在
  手柄的触觉马达上播放（默认关闭）。
- **感谢** @ponhei9502 的 PR #254，以及 @H0A0M0mer、@Ayonily、@labs20、@STALKERSK、@thxnk420、@markusdev 和 @2mg 的反馈与测试。

## v20261030 的变化

已有的预编译着色器继续有效：只要保留它们，着色器就按原来的方式翻译。下一次预编译（新安装，或更新显卡驱动之后）会带来两项
着色器修正（见最后几项）；如果那次预编译要把全部着色器重新编译一遍，启动器会说明。

- **FSR 3 超分：不再出现方格和闪烁的边缘**（#244，v20261029 引入的问题）：v20261029 把游戏的反应遮罩（reactive mask）交给了
  FSR 3，它因此把大片区域只取自当前帧。所有用 FSR 3 超分的玩家都受影响（AMD 显卡，以及开启 FSR 插帧的 RTX 20/30）。
- **DLSS：日志不再被 Streamline 错误刷屏**（#129、#85，v20261029 引入的问题）：Streamline 每帧都拒绝反应遮罩的标记
  （`Cannot have undefined format`，每秒上百行）。现在两种超分都不再拿到这个遮罩；同一条 Streamline 错误重复出现时只打印
  3 次，之后每 1000 次打印一次并附上次数。
- **加载时卡在雾气画面**（#249）：在一些电脑上，模拟器的预读打开了游戏的约 6.5 万个文件之后（似乎是那块硬盘能同时打开的
  上限），切换世界的加载就再也完成不了（雾气在动，游戏却不再读取任何文件）。现在预读最多只打开 4.8 万个；游戏自己打开
  文件失败时，会先归还预读打开的文件再试一次（日志会写出）。之前用来绕过的 `-Set KYTY_FILE_FOLLOW=0` 不再需要。
- **开启 FSR 3 插帧时按 F11**（#174）：用 F11 进入或退出全屏可能导致显卡设备丢失（崩溃），在 RTX 2060 SUPER + 165 Hz
  屏幕上出现：旧交换链的部件在 FSR 自己的呈现还在使用时就被释放了。
- **DLSS 超分 + AMD FSR 3 插帧**（#244）：RTX 20、30 系显卡没有 DLSS 插帧，开启插帧时用的是 FSR 3 的，之前 FSR 3 超分也会
  随之顶替 DLSS。现在由 DLSS 负责超分、AMD 的运行库负责插帧。`-Set KYTY_FSR_FG_DLSS=0` 恢复为 FSR 3 超分。
- **DualSense：附魔武器期间不再一直震动**（#239）：附魔时那种安静、持续的触觉效果只在触觉反馈上播放，和 PS5 上一样；受击等
  仍像以前一样震动（`KYTY_DUALSENSE_VIBRATION_MOTOR_ONSET`，其他手柄为 `KYTY_PAD_RUMBLE_ONSET`）。
- **Xbox 等非 PlayStation 手柄**（#236）：摇杆增加 12% 的圆形死区（跑步之后角色不再自己慢慢走动；
  `-Set KYTY_PAD_DEADZONE=<百分比>`，0 关闭），随游戏震动产生的手柄震动默认减半（`KYTY_PAD_RUMBLE_GAIN`，现在为 1）。
- **帧率面板显示着色器编译**（#129）：游戏现场编译预编译集合里没有的管线时（首次出现的卡顿），面板下方会显示
  “Compiling shaders: N (X s)”。
- **NVIDIA 低延迟模式“超高”**（#129）：它会让模拟器的呈现等待（帧时间不均匀、RTSS 和面板的数值不一致、掉帧）。模拟器会
  检测到这种情况（NVIDIA 显卡、关闭垂直同步时），在日志里和游戏画面上提示 20 秒：请在 NVIDIA 控制面板里为
  `kyty_emulator.exe` 把低延迟模式设为“关”或“开”。
- **启动器：预编译会说明何时要全部重新编译**（#246）：对于在新的着色器翻译方式（几乎会改变所有着色器）之前做的预编译，
  启动器会显示“着色器需要重新预编译（这次会全部重新编译一遍）”，而不是“只编译新增部分”。
- **启动器：DLSS 4.5 的预设 M 和 L** 加入“DLSS 模型”选项（#248）；`-Set KYTY_DLSS_PRESET=M` 或 `L` 也可以。
- **下一次预编译之后**：加载画面的雾气不再闪出一帧的棕色斑块（PR #245，感谢 @DevList1454：四条乘加指令按硬件的方式舍入；
  `KYTY_UNFUSED_MAD=0/1`），像素着色器在三角形边缘的导数也正确了（95 个着色器：调试菜单文字的线条；PR #243，感谢
  @DevList1454；`KYTY_PIXEL_QUAD_READS=0/1`）。
- **感谢** @Fantomas3228、@2mg、@brokeneggg、@jrodriguezml、@zioaster、@markusdev、@dimitrios1977ZXZ、@ft1704、@Ayonily 和
  @DevList1454 的反馈、测试与贡献。
