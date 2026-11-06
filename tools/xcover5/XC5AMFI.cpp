/*
 * XCover5: an IOService named AppleMobileFileIntegrity, as the AMFI kext
 * publishes on a device (after MakrSas/ios-topaz 0151, MIT/APSL tree).
 *
 * ExtensionKit reads the signed extension cache (/System/Library/xpc/
 * extensions.plist, +[NSDictionary _EX_dictionaryWithSignedURL:error:]) only
 * after asking AMFI's user client whether the signature's cdhash is trusted:
 * IOServiceGetMatchingService("AppleMobileFileIntegrity"), IOServiceOpen type 0,
 * IOConnectCallMethod selector 6 with one scalar and the cdhash. Without the
 * service: "Unable to find AMFI", no extension point in LaunchServices,
 * PlugInKit -10814 for every extension point: no PosterBoard wallpaper
 * providers (black wallpaper), widgets stuck at "Waiting...".
 *
 * This kernel enforces no code signatures (sweet_amfi.c), so every hash is
 * trusted: every call succeeds with zeroed outputs. Each selector is logged
 * the first time. Matched on IOResources (iokit/KernelConfigTables.cpp);
 * boot-arg xc5ext=0 leaves it unpublished.
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

class AppleMobileFileIntegrity : public IOService
{
	OSDeclareDefaultStructors(AppleMobileFileIntegrity);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

class AppleMobileFileIntegrityUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleMobileFileIntegrityUserClient);
public:
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleMobileFileIntegrity, IOService);
OSDefineMetaClassAndStructors(AppleMobileFileIntegrityUserClient, IOUserClient);

/*
 * For the heartbeat (boot 70: every extension kind 0 after a LaunchServices rebuild; is this service used at
 * all?): published, user clients opened, calls per selector 0-15, and the last caller's name.
 */
extern "C" {
uint32_t xcover5_amfi_svc[2];           /* published, user clients */
uint32_t xcover5_amfi_sel[16];
char xcover5_amfi_last[2][20];          /* last opener, last caller */
}

bool
AppleMobileFileIntegrity::start(IOService *provider)
{
	int ext = 1;

	(void)PE_parse_boot_argn("xc5ext", &ext, sizeof(ext));
	if (!ext || !IOService::start(provider)) {
		return false;
	}
	registerService();
	xcover5_amfi_svc[0] = 1;
	IOLog("XC5 AMFI: AppleMobileFileIntegrity service published\n");
	return true;
}

IOReturn
AppleMobileFileIntegrity::newUserClient(task_t owningTask, void *securityID __unused, UInt32 type __unused,
    IOUserClient **handler)
{
	AppleMobileFileIntegrityUserClient *client = OSTypeAlloc(AppleMobileFileIntegrityUserClient);

	if (client == NULL || !client->initWithTask(owningTask, NULL, 0) || !client->attach(this) || !client->start(this)) {
		if (client != NULL) {
			client->detach(this);
			client->release();
		}
		return kIOReturnNoMemory;
	}
	*handler = client;
	xcover5_amfi_svc[1]++;
	proc_selfname(xcover5_amfi_last[0], sizeof(xcover5_amfi_last[0]));
	return kIOReturnSuccess;
}

IOReturn
AppleMobileFileIntegrityUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch __unused, OSObject *target __unused, void *reference __unused)
{
	static uint64_t seen;

	xcover5_amfi_sel[selector < 16 ? selector : 15]++;
	proc_selfname(xcover5_amfi_last[1], sizeof(xcover5_amfi_last[1]));
	if (selector < 64 && !(seen & (1ULL << selector))) {
		char name[32];
		seen |= 1ULL << selector;
		proc_selfname(name, sizeof(name));
		IOLog("XC5 AMFI: user client selector %u from %s (%u scalars, %u bytes in): trusted\n", selector, name,
		    args->scalarInputCount, args->structureInputSize);
	}
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

IOReturn
AppleMobileFileIntegrityUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}
#endif
