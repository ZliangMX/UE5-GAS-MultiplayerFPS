#include "WeaponKillIconSet.h"

#include "Engine/Texture2D.h"
#include "Sound/SoundBase.h"

UTexture2D* UWeaponKillIconSet::GetKillIcon(int32 KillCount) const
{
	switch (KillCount)
	{
	case 1: return KillIcon1;
	case 2: return KillIcon2;
	case 3: return KillIcon3;
	case 4: return KillIcon4;
	case 5: return KillIcon5;
	case 6: return KillIcon6;
	default: break;
	}
	// 6 杀以上沿用第 6 张；KillCount <= 0 当作没填
	return KillCount > 6 ? KillIcon6 : nullptr;
}

USoundBase* UWeaponKillIconSet::GetKillSound(int32 KillCount) const
{
	switch (KillCount)
	{
	case 1: return KillSound1;
	case 2: return KillSound2;
	case 3: return KillSound3;
	case 4: return KillSound4;
	case 5: return KillSound5;
	case 6: return KillSound6;
	default: break;
	}
	// 同上：6 杀以上沿用第 6 段
	return KillCount > 6 ? KillSound6 : nullptr;
}
