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
	if (CloseButton) CloseButton->OnClicked.AddDynamic(this, &UBuyMenu::OnCloseClicked);

	RefreshCostText(RifleCostText, RifleClass);
	RefreshCostText(PistolCostText, PistolClass);
	RefreshCostText(SniperCostText, SniperClass);
	RefreshCostText(ShotgunCostText, ShotgunClass);

	RefreshCredits();
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
