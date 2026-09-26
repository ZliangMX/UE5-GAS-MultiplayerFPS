#include "BuyMenu.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"

void UBuyMenu::NativeConstruct()
{
	Super::NativeConstruct();

	if (RifleButton) RifleButton->OnClicked.AddDynamic(this, &UBuyMenu::OnRifleClicked);
	if (PistolButton) PistolButton->OnClicked.AddDynamic(this, &UBuyMenu::OnPistolClicked);
	if (SniperButton) SniperButton->OnClicked.AddDynamic(this, &UBuyMenu::OnSniperClicked);
	if (ShotgunButton) ShotgunButton->OnClicked.AddDynamic(this, &UBuyMenu::OnShotgunClicked);
	if (LightArmorButton) LightArmorButton->OnClicked.AddDynamic(this, &UBuyMenu::OnLightArmorClicked);
	if (HeavyArmorButton) HeavyArmorButton->OnClicked.AddDynamic(this, &UBuyMenu::OnHeavyArmorClicked);
	if (CloseButton) CloseButton->OnClicked.AddDynamic(this, &UBuyMenu::OnCloseClicked);

	RefreshCostText(RifleCostText, RifleClass);
	RefreshCostText(PistolCostText, PistolClass);
	RefreshCostText(SniperCostText, SniperClass);
	RefreshCostText(ShotgunCostText, ShotgunClass);

	// 护甲价格从 PC 读（和服务器扣钱用的是同一张表，不会出现「显示 400 实际扣 1000」）
	if (const ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(GetOwningPlayer()))
	{
		RefreshArmorCostText(LightArmorCostText, PC->GetLightArmorCost());
		RefreshArmorCostText(HeavyArmorCostText, PC->GetHeavyArmorCost());
	}

	RefreshCredits();
}

void UBuyMenu::RefreshArmorCostText(UTextBlock* Text, int32 Cost)
{
	if (!Text) return;
	Text->SetText(FText::FromString(FString::Printf(TEXT("$%d"), Cost)));
}

void UBuyMenu::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshCredits();
}

void UBuyMenu::RefreshCredits()
{
	if (!CreditsText) return;

	int32 Credits = 0;
	if (ABlasterPlayerState* PS = GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<ABlasterPlayerState>() : nullptr)
	{
		Credits = PS->Credits;
	}
	CreditsText->SetText(FText::FromString(FString::Printf(TEXT("$%d"), Credits)));
}

void UBuyMenu::RefreshCostText(UTextBlock* Text, TSubclassOf<AWeapon> WeaponClass)
{
	if (!Text) return;

	if (!WeaponClass)
	{
		Text->SetText(FText::FromString(TEXT("-")));
		return;
	}

	AWeapon* DefaultWeapon = WeaponClass->GetDefaultObject<AWeapon>();
	const int32 Cost = DefaultWeapon ? DefaultWeapon->GetWeaponCost() : 0;
	Text->SetText(FText::FromString(FString::Printf(TEXT("$%d"), Cost)));
}

void UBuyMenu::OnRifleClicked()  { RequestBuy(RifleClass); }
void UBuyMenu::OnPistolClicked() { RequestBuy(PistolClass); }
void UBuyMenu::OnSniperClicked() { RequestBuy(SniperClass); }
void UBuyMenu::OnShotgunClicked(){ RequestBuy(ShotgunClass); }

void UBuyMenu::OnLightArmorClicked()
{
	if (const ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(GetOwningPlayer()))
	{
		RequestBuyArmor(PC->GetLightArmorAmount());
	}
}

void UBuyMenu::OnHeavyArmorClicked()
{
	if (const ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(GetOwningPlayer()))
	{
		RequestBuyArmor(PC->GetHeavyArmorAmount());
	}
}

void UBuyMenu::RequestBuyArmor(int32 ArmorAmount)
{
	// 只报档位：价格由服务器查表（两边不一致以服务器为准，所以这里没有价格可报错）
	if (ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(GetOwningPlayer()))
	{
		PC->ServerBuyArmor(ArmorAmount);
	}
}

void UBuyMenu::OnCloseClicked()
{
	SetVisibility(ESlateVisibility::Hidden);
}

void UBuyMenu::RequestBuy(TSubclassOf<AWeapon> WeaponClass)
{
	if (!WeaponClass) return;

	ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(GetOwningPlayer());
	if (PC)
	{
		PC->ServerBuyWeapon(WeaponClass);
	}
}
