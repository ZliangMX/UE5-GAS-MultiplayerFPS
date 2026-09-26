#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "VisionBlockerInterface.generated.h"

/*
 * 「挡视线的东西」—— 问它一句：这段视线（世界坐标线段 From→To）被你挡住了吗？
 *
 * ——— 为什么单开一个接口，而不是给火墙/烟开碰撞、让 trace 自己撞上去 ———
 *   · 火墙和烟都是**刻意的全 NoCollision**（子弹穿、人走得进去、闪光弹也照样飞过去）。
 *     为了"挡视线"给它们开碰撞，等于把已经做好的"能穿过去"一起改掉 —— 而用户要的正是
 *     「挡的是判定，不是飞行」。
 *   · 形状也不一样：火墙是折线上摆的一排板子，烟是一个球。各自用自己的形状回答最直白。
 *   · 不用动 DefaultEngine.ini 的碰撞通道配置，也就不会牵动子弹/移动/相机任何一条既有 trace。
 *
 * ——— 谁问 ———
 * 目前只有 `APhoenixCurveball::Detonate`（闪光弹爆炸时的盲判定）：
 * 除了原来的"距离 + 朝向 + 世界几何 LineTrace"，再多问一遍所有实现者。
 * 所以火墙和烟**能让人不被闪**，但闪光球本身照样从它们中间飞过去。
 *
 * ——— 谁实现 ———
 * `ACloveSmoke`（球，按 SmokeRadius 判）、`APhoenixFlameWall`（每块立起来的板子判）。
 * 以后还有挡视线的东西（贤者的墙、地图可破坏物…）实现这个接口就行 ——
 * 调用方遍历的是"场上所有 actor 里实现本接口的"，不用改一行。
 */
UINTERFACE(MinimalAPI)
class UVisionBlockerInterface : public UInterface
{
	GENERATED_BODY()
};

class BLASTER_API IVisionBlockerInterface
{
	GENERATED_BODY()

public:
	/*
	 * From / To 都是**世界坐标**。返回 true = 这段视线被挡住（在 To 那边看不到 From）。
	 *
	 * 这是个纯判定，不要拿它去改任何组件状态：调用方（闪光的 Explode）是服务器上的一次性遍历。
	 * 调用方已经忽略了自身和受害者，实现者**不需要**再关心"别挡住自己人"这类规则。
	 */
	virtual bool BlocksVisionSegment(const FVector& From, const FVector& To) const = 0;
};
