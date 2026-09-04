# UE5 Multiplayer FPS — GAS 玩法模块源码摘录

> UE 5.4 / C++20 / GameplayAbilitySystem 多人竞技 FPS Demo 的**核心自研模块**代码摘录。
> 二次开发于课程 Blaster 多人项目框架之上；完整工程另含课程基础代码与引擎/第三方资产。

## ⚠️ 阅读前须知
- 本仓库**仅收录代码供阅读**，不是独立可编译工程（依赖 UE5.4 引擎 + 课程基础工程结构）。
- **不含任何游戏提取资产**（模型 / 贴图 / 动画 / 音频）——那些留在本地完整工程中。
- `Character/`、`Weapon/` 内含课程框架深度修改的基础类，其余为自研新增。每模块归属见下表。

## 模块结构（Source/Blaster/）

| 目录 | 内容 | 归属 |
|---|---|---|
| `Abilities/` | `UBlasterGameplayAbility` 能力基类 + 三技能落地（Jett 冲刺 / Sage 治疗 / Phoenix 曲线球闪光弹） | 自研 |
| `BlasterComponent/` | `UBlasterMovementComponent` 自定义移动组件（`MOVE_Custom` 客户端预测冲刺） | 自研 |
| `BlasterTypes/` | GameplayTag 注册库、武器类型枚举等 | 自研 |
| `Character/` | 角色主类：技能输入路由、武装态复制、F9 切换英雄、动画实例 | 深度修改 |
| `HUD/` | 纯 C++ `NativePaint` 自绘 UI：技能条 / 风效 / 闪光遮罩 / 角色 HUD | 自研 |
| `Weapon/` | 武器基础类（课程）+ `APhoenixCurveball` 弧线闪光弹投射物 | 课程基础 + 自研 |

## 技术要点
- **GAS 技能系统**：能力基类 `LocalPredicted + InstancedPerActor`；「充能 + 独立冷却」用**非堆叠 Duration GameplayEffect 计数**实现，每层独立计时、顺序恢复（动态改写 Spec 时长让第二发从 0 走满，不改 GE 资产）。
- **客户端预测冲刺**：位移逻辑下沉到自定义 `CharacterMovementComponent::PhysCustom`，方向/速度/剩余时长存于移动组件 → 本地即时开冲 + 服务器确定性回放 + `SmoothCorrection` 平滑修正，无 RTT 卡顿。
- **可靠 RPC 有序性**：客户端把冲刺方向用可靠 RPC **先于能力激活**上报（同通道有序），解决服务器端 `GetLastMovementInputVector()` 恒为 0 的转向错误。
- **纯 C++ 自绘 UI**：技能条/风效/闪光遮罩全部 `UUserWidget::NativePaint + FSlateDrawElement`（兼容 UE5.4 移除 `FSlateDrawElement::MakeCircle`，用三角扇 `MakeCustomVerts`），零美术资产出完整表现。
- **材质驱动的受击反馈**：闪光弹被闪画面 = Unlit UI 材质 + 运行时参数（爆炸点屏幕投影→UV→逐帧 `SetScalar/VectorParameter`）驱动径向渐变；曾排查修复 float2/float3 类型不匹配导致的 shader 编译黑屏。
- **源码级排障习惯**：问题直接读引擎源码定位（`EndAbility` 清理能力定时器、`FGameplayAbilitySpec::GetPrimaryInstance()` 复制时机、`ACharacter::CharacterMovement` 私有访问等）。

## 其他
- 大量资产（4 把武器 / 第一人称手 / 第三人称角色 / 60+ 动画）走 **UE headless（UnrealEditor-Cmd + Python）批量导入管线**，脚本按需可索。
- 联系：[平梁烨](mailto:734974309@qq.com)
