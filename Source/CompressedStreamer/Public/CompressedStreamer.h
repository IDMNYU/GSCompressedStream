#pragma once

#include "Modules/ModuleManager.h"

class FCompressedStreamerModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
