#pragma once
#include "GameFramework/AActor.h"

class USkeletalMeshComponent;
class UStaticMeshComponent;

class ASkeletalMeshActor : public AActor
{
public:
	DECLARE_CLASS(ASkeletalMeshActor, AActor)
	ASkeletalMeshActor() {}

	void BeginPlay() override;

	// void InitDefaultComponents(const FString& UMeshFileName = "FBX\\SambaDancing\\Samba Dancing.fbx");
	void InitDefaultComponents(const FString& UMeshFileName = "FBX\\lowpolyboy\\SimpleMan.fbx");
	// void InitDefaultComponents(const FString& UMeshFileName = "FBX\\Angelica\\Angelica.fbx");

private:
	UStaticMeshComponent* SkeletalMeshComponent;
};

