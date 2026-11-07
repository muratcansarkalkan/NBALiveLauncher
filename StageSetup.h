#pragma once
#include <cstddef>
void InitializeCustomPaths();
void InitializeCustomCamera();
void InitializeStadiumDornaConfig();
void UpdateCustomPathsGameContext(const char* stadiumId4, void* options);
void BeginCustomPathsGameLoad();
bool ResolveStadiumCameraPath(const char* stadiumId, char* outPath, size_t outPathSize);
