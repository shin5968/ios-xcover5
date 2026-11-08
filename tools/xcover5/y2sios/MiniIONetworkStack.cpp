/*
 * From y2sios (https://github.com/stormtroopxyz/y2sios, kernel/xnu/files/), MIT License,
 * Copyright (c) 2026 the y2sios contributors; itself after HackiOS by OOSEX GANG (MIT). Taken for the XCover5 port's
 * Wi-Fi (bsd/dev/xc5_wlbt.c supplies the station side); changes marked XCover5.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and
 * this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS
 * PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/*
 * MiniIONetworkStack: a stand-in for IONetworkStack of Apple's IONetworkingFamily, as seen by
 * configd's interface namer.
 *
 * The real one gives the BSD names (en0, en1, ...) to the IONetworkInterface objects of the
 * drivers, when the interface namer asks. Here interfaces are made with the BSD interface KPI
 * (XCover5: bsd/dev/xc5_wlbt.c) and are born with their names, so there is never anything to name.
 * But the namer wants this object all the same: until it has opened it, it does not say in
 * configd's store that the registry has gone quiet (the key Plugin:InterfaceNamer, "*QUIET*"),
 * and gives up only after five minutes ("*TIMEOUT*"). SystemConfiguration's
 * SCNetworkInterfaceCopyAll() waits for one of the two, and with it whoever asks for the list of
 * interfaces: wifid as soon as it has a device, and behind wifid SpringBoard.
 *
 * So: the service, a user client that can be opened, and a refusal of the naming request.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" void MiniIONetworkStackStart(void);       /* XCover5: from MiniDrivers.h */

class IONetworkStack : public IOService
{
	OSDeclareDefaultStructors(IONetworkStack);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IONetworkStack, IOService);

bool
IONetworkStack::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IONetworkStackUserClient");
	setName("IONetworkStack");
	registerService();
	return true;
}

class IONetworkStackUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IONetworkStackUserClient);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);

bool
IONetworkStackUserClient::start(IOService * provider)
{
	char who[32] = "?";

	if (!IOUserClient::start(provider)) {
		return false;
	}
	/* whether the registry is quiet at this moment is what the namer goes on to ask */
	proc_selfname(who, sizeof(who));
	IOLog("MiniIONetworkStack: opened by %s; the registry's busy state is %u\n", who,
	    (unsigned) IOService::getServiceRoot()->getBusyState());
	return true;
}

IOReturn
IONetworkStackUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/* "Name this interface" (a dictionary: IONetworkStackUserCommand, IOPathMatch, IOInterfaceUnit). */
IOReturn
IONetworkStackUserClient::setProperties(OSObject * properties)
{
	static unsigned said;

	if (said++ < 4) {
		IOLog("MiniIONetworkStack: asked to name an interface: there are none of that kind\n");
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniIONetworkStackStart(void)
{
	/* where the real one is: IOService:/IOResources/IONetworkStack */
	IOService * resources = IOService::getResourceService();
	IONetworkStack * stack = OSTypeAlloc(IONetworkStack);

	if (resources == NULL || stack == NULL || !stack->init() || !stack->attach(resources)) {
		OSSafeReleaseNULL(stack);
		IOLog("MiniIONetworkStack: cannot create the service\n");
		return;
	}
	if (!stack->start(resources)) {
		stack->detach(resources);
	}
	stack->release();
}
