#pragma once

#include "CoreMinimal.h"
#include "MovementDirection.generated.h"

/*
 * ——— 八向移动方向 ———
 *
 * 把"角色正在往哪个方向走"
 * 从一根连续的夹角（-180~180）量化成 8 个扇区，一个扇区 45°，扇区中心就是名字本身。
 *
 * 目前只有一个消费者：**空手蒙太奇**（Jett 放完 E/Q 那一段）。
 * 八个方向的动画做成**一个蒙太奇里的 8 个分段**，播哪一段由这个枚举决定
 * （名字怎么对上见下面的 ToSectionName）。
 *
 * 角度口径：**以角色自身正前方为 0°，右为正、左为负**（和 UBlasterCharacterAnimInstance::Direction
 * 完全同一套）。也就是说这个枚举是"相对朝向"的：
 *   · N  = 朝自己面对的方向走
 *   · E  = 朝自己的右手边平移（strafe right）
 *   · S  = 倒着走
 * 不是绝对方位（不是"世界坐标的北"），别拿它和世界 Yaw 对号。
 *
 * 本工程里 `bUseControllerRotationYaw = true`，角色 Yaw 永远跟着视角，所以"相对角色"和
 * "相对镜头"在这里是同一个东西 —— 第一人称手模和第三人称身体可以共用这一套值。
 *
 * ⚠ 枚举顺序就是**顺时针 45° 一圈**（N → NE → E → SE → S → SW → W → NW → 回到 N），
 *   刻意这么排的：`Quantize()` 是直接拿 `角度/45` 当下标用的，"下一个方向"就是 `+1`，
 *   蓝图里想自己算（比如按转向插值）也能直接加减。**不要往中间插值**，插了就破坏这个循环顺序。
 */
UENUM(BlueprintType)
enum class EMovementDirection8 : uint8
{
	// 前 —— 朝角色正前方（夹角 0°）
	N UMETA(DisplayName = "N (Forward)"),
	// 右前（夹角 +45°）
	NE UMETA(DisplayName = "NE (ForwardRight)"),
	// 右（夹角 +90°，也就是横向平移）
	E UMETA(DisplayName = "E (Right)"),
	// 右后（夹角 +135°）
	SE UMETA(DisplayName = "SE (BackRight)"),
	// 后（夹角 ±180°，倒着走）
	S UMETA(DisplayName = "S (Backward)"),
	// 左后（夹角 -135°）
	SW UMETA(DisplayName = "SW (BackLeft)"),
	// 左（夹角 -90°）
	W UMETA(DisplayName = "W (Left)"),
	// 左前（夹角 -45°）
	NW UMETA(DisplayName = "NW (ForwardLeft)")
};

/*
 * 八向的配套工具函数。放在命名空间里而不是做成 UBlueprintFunctionLibrary：
 * 这套换算只在 C++ 侧（两个 AnimInstance 的 NativeUpdateAnimation）用，
 * 蓝图里拿到的已经算好的 EMovementDirection8 变量，不需要再换算一次。
 */
namespace MovementDirection8
{
	// 一圈 8 个方向 —— 别写成魔法数 8
	constexpr int32 Num = 8;
	constexpr float SectorDegrees = 360.f / static_cast<float>(Num);	// 45
	constexpr float HalfSectorDegrees = SectorDegrees * 0.5f;			// 22.5

	// 角色本地空间里的单位向量：X = 前（N），Y = 右（E）。Z 恒为 0。
	FORCEINLINE FVector ToLocalUnitVector(EMovementDirection8 Direction)
	{
		const int32 Index = static_cast<int32>(Direction) % Num;
		const float Radians = FMath::DegreesToRadians(static_cast<float>(Index) * SectorDegrees);
		// X 取 cos、Y 取 sin：Index 0 → (1,0) 前，Index 2 → (0,1) 右，和角度口径一致
		return FVector(FMath::Cos(Radians), FMath::Sin(Radians), 0.f);
	}

	/*
	 * 把夹角（度，右正左负）量化成 8 向。
	 *
	 * 做法：先整体转半个扇区（+22.5°），整除 45 之后每个方向正好落在"扇区中心"上 ——
	 * 不转的话 0° 会落在 N 和 NW 的边界上，静止起步时方向会在两个值之间抖。
	 *
	 * （定义顺序：QuantizeVector 要用它，所以它必须在前 —— 头文件里函数是按文本顺序解析的。）
	 */
	FORCEINLINE EMovementDirection8 QuantizeDegrees(float YawOffsetDegrees)
	{
		const int32 Index = FMath::FloorToInt32((YawOffsetDegrees + HalfSectorDegrees) / SectorDegrees);
		// C++ 的 % 对负数给负值（-1 % 8 == -1），先 +8 再取一次才是真正的环绕
		// —— 少了这步，往左后走会拿到非法枚举值
		const int32 Wrapped = ((Index % Num) + Num) % Num;
		return static_cast<EMovementDirection8>(Wrapped);
	}

	/*
	 * 枚举 → 蒙太奇**分段名**（section name）。
	 *
	 * 约定的资产结构（"一条蒙太奇装八个方向"）：
	 *   一个蒙太奇里切 8 个 section，名字就是枚举名本身 —— N / NE / E / SE / S / SW / W / NW。
	 *   播放时先 Montage_Play，再 Montage_JumpToSection(ToSectionName(Dir)) 跳进对应的那一段，
	 *   于是 FP / UB / LB 三条槽一共只要 3 个蒙太奇资产，而不是 8×3 = 24 个。
	 *
	 * ⚠ 这张表是**手写**的，改枚举（加值 / 调顺序）时要一起改：下标必须和上面的 enum 一一对应。
	 *   写成 switch 会更安全，但每次加方向要改两处、更啰嗦 —— 这里靠注释约束。
	 *
	 * 分段名大小写不敏感（FName 比较默认忽略大小写），写成 "n" / "N" 都能匹配上。
	 */
	FORCEINLINE FName ToSectionName(EMovementDirection8 Direction)
	{
		// 函数内 static：首次调用时才构造（FName 依赖引擎的 FName 表，别做成全局静态对象）
		static const FName SectionNames[Num] =
		{
			FName(TEXT("N")),  FName(TEXT("NE")), FName(TEXT("E")),  FName(TEXT("SE")),
			FName(TEXT("S")),  FName(TEXT("SW")), FName(TEXT("W")),  FName(TEXT("NW"))
		};
		// 越界（拿到 _MAX 之类的坏值）时退回 N，不返回空 FName：空名字会被当成"没有分段"，
		// 而 N 至少是一个真实存在的分段名，真出错时能看出来是"方向错了"而不是"没跳进去"
		const int32 Index = static_cast<int32>(Direction);
		return (Index >= 0 && Index < Num) ? SectionNames[Index] : SectionNames[0];
	}

	/*
	 * 八向 → 四向（只有前后左右）。**斜向合并到东 / 西**，不是合并到前后：
	 *   NE（右前）→ E，SE（右后）→ E，NW（左前）→ W，SW（左后）→ W
	 * N / E / S / W 原样返回。
	 *
	 * 为什么不是合并到前后：第一人称的手模只做了四个方向（前 / 后 / 左平移 / 右平移），
	 * 用户定的规则就是"东南东北放东、西南西北放西"—— 斜着走时手模按**横移**那套播，
	 * 而不是按前进那套。
	 *
	 * 用途：蒙太奇里没有斜向那一段时退到这一段（见 FBlasterEmptyHandMontages::ResolveSectionName）。
	 */
	FORCEINLINE EMovementDirection8 CollapseToCardinal(EMovementDirection8 Direction)
	{
		switch (Direction)
		{
		case EMovementDirection8::NE:
		case EMovementDirection8::SE: return EMovementDirection8::E;
		case EMovementDirection8::NW:
		case EMovementDirection8::SW: return EMovementDirection8::W;
		default: return Direction;	// N / E / S / W
		}
	}

	// 反过来：给一个本地空间方向向量，算出它落在哪个扇区（零向量返回 N，调用方自己先判速度）
	FORCEINLINE EMovementDirection8 QuantizeVector(const FVector& LocalDirection)
	{
		return QuantizeDegrees(
			FMath::RadiansToDegrees(FMath::Atan2(LocalDirection.Y, LocalDirection.X)));
	}

	/*
	 * 世界空间方向 + 角色朝向 → 方向枚举（**不做速度判断**，调用方自己决定要不要用）。
	 *
	 * 分解用的是点乘（和 UBlasterCharacterAnimInstance 里算 Direction 那几行同一个套路），
	 * 不绕 FRotator —— 那行代码就是"右为正"口径的出处，两边必须用同一套算法。
	 *
	 * 返回 false：方向向量或朝向退化成零向量（不该发生），OutDirection 保持原样不动。
	 */
	FORCEINLINE bool QuantizeWorldDirection(const FVector& WorldDirection, const FVector& WorldFacing,
		EMovementDirection8& OutDirection)
	{
		FVector Forward = WorldFacing;
		Forward.Z = 0.f;
		FVector PlanarDirection = WorldDirection;
		PlanarDirection.Z = 0.f;
		if (!Forward.Normalize() || !PlanarDirection.Normalize())
		{
			return false;
		}
		// UE 左手系：Up × Forward == 右手边
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);

		OutDirection = QuantizeVector(FVector(
			FVector::DotProduct(PlanarDirection, Forward),	// X = 往前多少
			FVector::DotProduct(PlanarDirection, Right),	// Y = 往右多少（正 = 右，和 Direction 同口径）
			0.f));
		return true;
	}

	/*
	 * 速度版：世界空间速度 + 角色正前方 → 方向枚举。
	 *
	 * 返回 false 表示"速度太小、算不出有意义的方向"，此时 **OutDirection 保持原样不动**。
	 * 调用方请把上一帧的值留着，不要归到 N：枚举里没有 Idle，硬归零等于凭空造出一个
	 * "正在往前走"的假方向，减速停下的那几帧会在 N 和真实方向之间来回跳。
	 *
	 * @param MinSpeedSquared 速度平方阈值（省一次开方），一般给 5cm/s 的平方 = 25
	 */
	FORCEINLINE bool FromVelocity(const FVector& WorldVelocity, const FVector& WorldFacing,
		float MinSpeedSquared, EMovementDirection8& OutDirection)
	{
		FVector PlanarVelocity = WorldVelocity;
		PlanarVelocity.Z = 0.f;			// 只看平面速度：落地/起跳那一瞬的竖直速度不该改方向
		if (PlanarVelocity.SizeSquared() < MinSpeedSquared)
		{
			return false;
		}
		return QuantizeWorldDirection(PlanarVelocity, WorldFacing, OutDirection);
	}
}
