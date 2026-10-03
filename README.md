# UE5 Multiplayer FPS — GAS 玩法 Demo 源码

> UE 5.4 / C++20 / GameplayAbilitySystem 的多人竞技 FPS Demo（Valorant 玩法向）**完整游戏源码**：198 个文件 / 约 4.4 万行。
> 在课程 Blaster 多人框架之上二次开发 —— 课程框架提供角色、武器、GameMode 等基础类，**玩法系统（技能 / 回合 / 经济 / 大厅 / 小地图 / 自绘 HUD / 服务器回溯判定）全部自研**。

**▶ 试玩演示视频（3 分钟，真实对局录制）**：https://space.bilibili.com/240746100
> 内容顺序：大厅建房与选边选英雄 → 武器手感与击杀反馈 → 爆能器拾取 → 安包 / 两段拆包完整回合 → Phoenix 火焰技能 → Clove 封烟 → Jett 技能与大招 → 第三人称观战

## ⚠️ 阅读前须知

- 本仓库**仅收录代码供阅读**，不是独立可编译工程（依赖 UE5.4 引擎、课程基础工程的蓝图与配置）。
- **不含任何游戏提取资产**（模型 / 贴图 / 动画 / 音频）与蓝图资产 —— 那些留在本地完整工程中。
- 下表标注每个目录的归属：`自研` = 新增模块；`课程基础 + 深度改造` = 在课程框架类上改写。

## 模块结构（Source/Blaster/）

| 目录 | 规模 | 内容 | 归属 |
|---|---|---|---|
| `Abilities/` | 30 文件 / 4343 行 | `UBlasterGameplayAbility` 能力基类 + 4 名英雄 16 个技能：Jett（冲刺 / 腾空 / 烟球 / 飞刀）、Phoenix（曲线球闪光 / 火墙 / 火球 / 回火）、Sage 治疗、Clove 烟幕 | 自研 |
| `BlasterComponent/` | 8 / 3742 | 自定义移动组件（`MOVE_Custom` 客户端预测冲刺）、**LagCompensationComponent 服务器回溯命中判定**、战斗组件、小地图可见性组件 | 自研 |
| `BlasterTypes/` | 8 / 530 | GameplayTag 注册库、英雄 / 武器 / 回合 / 投掷物枚举 | 自研 |
| `Character/` | 26 / 10598 | 角色主类 + 4 个英雄子类、第一人称手模与第三人称动画实例、5 个动画通知类（换弹 / 掏枪 / 技能 / 飞刀） | 课程基础 + 深度改造 |
| `HUD/` | 38 / 7659 | 纯 C++ `NativePaint` 自绘 UI：Valorant 上下底栏 / 技能条 / 小地图 / 闪光遮罩 / 风效 / 买枪菜单 / 计分板 / 大厅与英雄选择 / 击杀公告 | 自研 |
| `Weapon/` | 35 / 6380 | 武器基础类与命中判定、飞刀 / 近战 / 弹道与投射物、**击杀档位数据资产 `UWeaponKillIconSet`** | 课程基础 + 深度改造 |
| `Spike/` | 4 / 2964 | 爆能器：安放 / 携带 / 掉落、两段式拆包、45s 爆炸倒计时、爆炸范围伤害与球体视效 | 自研 |
| `PlayerController/` | 2 / 2150 | 输入路由、买枪与技能 UI 驱动、敌方勾边高亮、回合公告与 HUD 刷新 | 课程基础 + 深度改造 |
| `GameMode/` `GameState/` | 6 / 1898 | 五阶段回合状态机（Buy → InProgress → PostRound → GameOver）、回合奖励与复活、**跨地图流程串联（菜单 ⇄ 大厅 ⇄ 战场）** | 课程基础 + 深度改造 |
| `Lobby/` | 4 / 790 | 大厅英雄展示 Actor：第三人称展示台 + 镜头、按英雄切换展示动作 | 自研 |
| `Pickup/` `PlantZone/` `Barrier/` | 12 / 1321 | 大招能量球、**非凸多边形安装区**（角点标记 + 区域判定）、购买阶段激光墙 | 自研 |
| `PlayerState/` `GameInstance/` `Interfaces/` | 8 / 464 | 玩家身份 / 队伍 / 英雄与跨 ServerTravel 持久化、准星与视线遮挡接口 | 自研 |

## 编辑器模块与插件

| 路径 | 规模 | 内容 |
|---|---|---|
| `Source/BlasterEditor/` | 7 文件 / 858 行 | 编辑器模块：安装区角点可视化、**蒙太奇槽轨道 / 分段 / 倍率的批量读写库**（headless 下 Python 办不到的部分）、动画图表批量重定向，供命令行脚本调用 |
| `Plugins/FbxPipeline/` | 6 文件 / 518 行 | **FBX 批量导入命名校验插件**：纯 C++ 校验核心（零编辑器依赖）+ 数据化规则（长度上限 / 允许字符 / 前缀）+ 7 类问题码 + **10 条 UE 自动化测试全过** |

## 技术要点

**客户端与网络**
- **服务器回溯命中判定（Lag Compensation）**：服务端 30Hz 环形缓冲记录角色历史帧（0.5s 窗口），客户端开火携带服务器时间戳，服务端把目标还原到那一刻重算射线；命中盒为头 / 胸 / 胯 / 腿四个**带朝向盒**、按骨骼锚定（依运行时实测骨架重标定，相邻段互相咬合消除漏区）；射线起点用**服务端自记眼位**并补视线遮挡校验；配套判定日志与常驻命中盒可视化开关。
- **客户端预测冲刺**：位移逻辑下沉到 `CharacterMovementComponent::PhysCustom`，方向 / 速度 / 剩余时长存于移动组件 → 本地即时开冲 + 服务器确定性回放 + `SmoothCorrection` 平滑修正，无 RTT 卡顿。可靠 RPC 与能力激活同通道有序，先于激活上报方向（解决服务器 `GetLastMovementInputVector()` 恒 0 的转向错误）。
- **GAS 技能系统**：能力基类 `LocalPredicted + InstancedPerActor`；「充能 + 独立冷却 + 顺序恢复」用**非堆叠 Duration GameplayEffect 计数**实现（每层独立计时，动态改写 Spec 时长让第二发从 0 走满，不改 GE 资产）；技能动作由动画通知驱动，C++ 只切状态。
- **小地图防透视**：服务器权威可见性判定（距离 + 视野锥 + LOS，按 `COND_OwnerOnly` 逐客户端下发），任一存活队友看到即全队共享、脱离视野即时消失，队友位置零额外带宽。

**表现层**
- **纯 C++ 自绘 UI**：技能条 / 底栏 / 小地图 / 闪光遮罩 / 风效全部 `UUserWidget::NativePaint + FSlateDrawElement`，零美术资产出完整表现（含 UE5.4 移除 `MakeCircle` 后改用三角扇 `MakeCustomVerts`）。
- **材质驱动的受击反馈**：闪光弹被闪画面 = Unlit UI 材质 + 运行时参数（爆炸点屏幕投影 → UV → 逐帧 `SetScalar/VectorParameter`）驱动径向渐变。
- **敌方勾边**：走覆层材质（`SetOverlayMaterial`），利用 `OverlayMaterial` 不参与属性复制实现「只勾自己视角的敌人」。

**工程与工具链**
- **动画与手感时序**：读引擎源码确认 `GetPlayLength()` 不含 `RateScale` 的语义，换弹 / 掏枪状态时长按第一人称动画实际时长收尾；再按官方数值统一重定 22 条动画倍率，跨进程读回验证偏差为 0。
- **headless 全自动资产管线**：`UnrealEditor-Cmd + Python` 批量导入（1000+ 动画 / 1500+ 资产）、材质槽批量替换、碰撞复杂度分级与关卡可玩化排障、光照烘焙 —— 全程脚本化 + 读回校验，可重复执行。
- **源码级排障习惯**：问题直接读引擎源码定位（`EndAbility` 清理能力定时器、`FGameplayAbilitySpec::GetPrimaryInstance()` 复制时机、`OverlayMaterial` 的复制语义、无缝旅行下 PlayerState 重建等）。

## 其他

- 完整工程另含课程基础代码与引擎 / 第三方插件，不在本仓库。
- 试玩演示视频：https://space.bilibili.com/240746100
- 联系：[平梁烨](mailto:734974309@qq.com)
