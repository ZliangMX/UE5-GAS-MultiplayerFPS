#pragma once

#include "CoreMinimal.h"
#include "Runtime/UMG/Public/Blueprint/UserWidget.h"
#include "Announcement.generated.h"

UCLASS()
class BLASTER_API UAnnouncement : public UUserWidget
{
	GENERATED_BODY()
public:
	UPROPERTY(meta=(BindWidget))
	class UTextBlock* WarmupTime;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* AnnouncementText;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* InfoText;

	// --- Round UI ---
	UPROPERTY(meta=(BindWidget))
	UTextBlock* RoundResultText;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* TeamSwapText;
};
