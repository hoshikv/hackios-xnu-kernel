/*
 * HakiosBluetooth: the registry entry by which iOS finds the WCN6750.
 *
 * bluetoothd's built-in H4 backend looks for a service named "bluetooth" and
 * reads local-mac-address and HakiosBluetoothSupportedCommands from it; the
 * bytes themselves go through the PF_SYSTEM endpoint in bsd/dev/hakios.bt.c.
 * This object only publishes the controller's measured identity and readiness.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <sys/errno.h>
#include <stdint.h>

extern "C" int HakiosBluetoothPublish(const uint8_t address[6], const uint8_t commands[64]);
extern "C" void HakiosBluetoothSetReady(bool ready);

static IOService *bluetooth_service;

extern "C" void
HakiosBluetoothSetReady(bool ready)
{
	if (bluetooth_service != NULL) {
		bluetooth_service->setProperty("HakiosBluetoothReady", ready);
	}
}

extern "C" int
HakiosBluetoothPublish(const uint8_t address[6], const uint8_t commands[64])
{
	IOService *provider = IOService::getPlatform();
	IOService *service = new IOService;
	OSData *mac;
	OSData *supported;

	if (provider == NULL || service == NULL) {
		if (service != NULL) {
			service->release();
		}
		return ENOMEM;
	}
	if (!service->init()) {
		service->release();
		return ENOMEM;
	}
	mac = OSData::withBytes(address, 6);
	if (mac == NULL) {
		service->release();
		return ENOMEM;
	}
	service->setName("bluetooth");
	bool published = service->setProperty("local-mac-address", mac);

	mac->release();
	if (!published) {
		service->release();
		return ENOMEM;
	}
	supported = OSData::withBytes(commands, 64);
	if (supported == NULL) {
		service->release();
		return ENOMEM;
	}
	published = service->setProperty("HakiosBluetoothSupportedCommands", supported);
	supported->release();
	if (!published) {
		service->release();
		return ENOMEM;
	}
	service->setProperty("transport-speed", 115200, 32);
	service->setProperty("compatible", "qcom,wcn6750-bt");
	service->setProperty("HakiosBluetoothReady", false);
	if (!service->attach(provider)) {
		service->release();
		return EIO;
	}
	if (!service->start(provider)) {
		service->detach(provider);
		service->release();
		return EIO;
	}
	service->registerService();
	bluetooth_service = service;
	service->release();             /* the registry's attachment owns the live service */
	return 0;
}
