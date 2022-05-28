#include "icsneo/platform/cdcacm.h"
#include "icsneo/device/founddevice.h"
#include <dirent.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <map>
#include <algorithm>
#include <cctype>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

using namespace icsneo;

/* The TTY numbering starts at zero, but we want to keep zero for an undefined
 * handle, so add a constant.
 */
static constexpr const neodevice_handle_t HANDLE_OFFSET = 10;

class Directory {
public:
	class Listing {
	public:
		Listing(std::string newName, uint8_t newType) : name(newName), type(newType) {}
		const std::string& getName() const { return name; }
		uint8_t getType() const { return type; }
	private:
		std::string name;
		uint8_t type;
	};
	Directory(std::string directory) {
		dir = opendir(directory.c_str());
	}
	~Directory() {
		if(openedSuccessfully())
			closedir(dir);
		dir = nullptr;
	}
	bool openedSuccessfully() { return dir != nullptr; }
	std::vector<Listing> ls() {
		std::vector<Listing> results;
		struct dirent* entry;
		while((entry = readdir(dir)) != nullptr) {
			std::string name = entry->d_name;
			if(name != "." && name != "..") // Ignore parent and self
				results.emplace_back(name, entry->d_type);
		}
		return results;
	}
private:
	DIR* dir;
};

class USBSerialGetter {
public:
	USBSerialGetter(std::string usbid) {
		std::stringstream ss;
		auto colonpos = usbid.find(":");
		if(colonpos == std::string::npos) {
			succeeded = false;
			return;
		}

		ss << "/sys/bus/usb/devices/" << usbid.substr(0, colonpos) << "/serial";
		try {
			std::ifstream reader(ss.str());
			std::getline(reader, serial);
			for (auto& c : serial)
				c = toupper(c);
		} catch(...) {
			succeeded = false;
			return;
		}

		succeeded = true;
	}
	bool success() const { return succeeded; }
	const std::string& getSerial() const { return serial; }
private:
	bool succeeded;
	std::string serial;
};

void CDCACM::Find(std::vector<FoundDevice>& found) {
	Directory directory("/sys/bus/usb/drivers/cdc_acm"); // Query the CDCACM driver
	if(!directory.openedSuccessfully())
		return;

	std::vector<std::string> foundusbs;
	for(auto& entry : directory.ls()) {
		/* This directory will have directories (links) for all devices using the cdc_acm driver (as CDCACM devices do)
		 * There will also be other files and directories providing information about the driver in here. We want to ignore them.
		 * Devices will be named like "7-2:1.0" where 7 is the enumeration for the USB controller, 2 is the device enumeration on
		 * that specific controller (will change if the device is unplugged and replugged), 1 is the device itself and 0 is
		 * enumeration for different services provided by the device. We're looking for the service that provides TTY.
		 * For now we find the directories with a digit for the first character, these are likely to be our USB devices.
		 */
		if(isdigit(entry.getName()[0]) && entry.getType() == DT_LNK)
			foundusbs.emplace_back(entry.getName());
	}

	// Map the USB directory to the TTY and PID if found
	// The PID will be filled later
	std::map< std::string, std::pair<std::string, uint16_t> > foundttys;
	for(auto& usb : foundusbs) {
		std::stringstream ss;
		ss << "/sys/bus/usb/drivers/cdc_acm/" << usb << "/tty";
		Directory devicedir(ss.str());
		if(!devicedir.openedSuccessfully()) // The tty directory doesn't exist, because this is not the tty service we want
			continue;

		auto listing = devicedir.ls();
		if(listing.size() != 1) // We either got no serial ports or multiple, either way no good
			continue;

		foundttys.insert(std::make_pair(usb, std::make_pair(listing[0].getName(), 0)));
	}

	// We're going to remove from the map if this is not the product we're looking for
	for(auto iter = foundttys.begin(); iter != foundttys.end(); ) {
		auto& [_, pair] = *iter;
		auto& [tty, ttyPid] = pair;
		const std::string matchString = "PRODUCT=";
		std::stringstream ss;
		ss << "/sys/class/tty/" << tty << "/device/uevent"; // Read the uevent file, which contains should have a line like "PRODUCT=93c/1101/100"
		std::ifstream fs(ss.str());
		std::string productLine;
		size_t pos = std::string::npos;
		do {
			std::getline(fs, productLine, '\n');
		} while(((pos = productLine.find(matchString)) == std::string::npos) && !fs.eof());

		if(pos != 0) { // We did not find a product line... weird
			iter = foundttys.erase(iter); // Remove the element, this also moves iter forward for us
			continue;
		}

		size_t firstSlashPos = productLine.find('/', matchString.length());
		if(firstSlashPos == std::string::npos) {
			iter = foundttys.erase(iter);
			continue;
		}
		size_t pidpos = firstSlashPos + 1;

		std::string vidstr = productLine.substr(matchString.length(), firstSlashPos - matchString.length());
		std::string pidstr = productLine.substr(pidpos, productLine.find('/', pidpos) - pidpos); // In hex like "1101" or "93c"

		uint16_t vid, pid;
		try {
			vid = (uint16_t)std::stoul(vidstr, nullptr, 16);
			pid = (uint16_t)std::stoul(pidstr, nullptr, 16);
		} catch(...) {
			iter = foundttys.erase(iter); // We could not parse the numbers
			continue;
		}

		if(vid != INTREPID_USB_VENDOR_ID) {
			iter = foundttys.erase(iter); // Not the right VID, remove
			continue;
		}
		ttyPid = pid; // Set the PID for this TTY
		iter++; // If the loop ends without erasing the iter from the map, the item is good
	}

	// At this point, foundttys contains the the devices we want
	
	// Get the serial number, create the neodevice_t
	for(auto& [usb, pair] : foundttys) {
		auto& [tty, ttyPid] = pair;
		FoundDevice device;

		USBSerialGetter getter(usb);
		if(!getter.success())
			continue; // Failure, could not get serial number

		// In ttyACM0, we want the i to be the first character of the number
		size_t i;
		for(i = 0; i < tty.length(); i++) {
			if(isdigit(tty[i]))
				break;
		}
		// Now we try to parse the number so we have a handle for later
		try {
			device.handle = (neodevice_handle_t)std::stoul(tty.substr(i));
			/* The TTY numbering starts at zero, but we want to keep zero for an undefined
			 * handle, so add a constant, and we'll subtract that constant in the open function.
			 */
			device.handle += HANDLE_OFFSET;
		} catch(...) {
			continue; // Somehow this failed, have to toss the device
		}

		device.productId = ttyPid;
		device.serial[getter.getSerial().copy(device.serial, sizeof(device.serial)-1)] = '\0';

		// Add a factory to make the driver
		device.makeDriver = [](const device_eventhandler_t& report, neodevice_t& device) {
			return std::unique_ptr<Driver>(new CDCACM(report, device));
		};

		found.push_back(device); // Finally, add device to search results
	}
}

std::string CDCACM::HandleToTTY(neodevice_handle_t handle) {
	std::stringstream ss;
	ss << "/dev/ttyACM" << (int)(handle - HANDLE_OFFSET);
	return ss.str();
}