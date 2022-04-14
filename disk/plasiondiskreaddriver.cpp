#include "icsneo/disk/plasiondiskreaddriver.h"
#include "icsneo/communication/message/neoreadmemorysdmessage.h"
#include "icsneo/communication/multichannelcommunication.h"
#include <cstring>

using namespace icsneo;
using namespace icsneo::Disk;

optional<uint64_t> PlasionDiskReadDriver::readLogicalDiskAligned(Communication& com, device_eventhandler_t report,
	uint64_t pos, uint8_t* into, uint64_t amount, std::chrono::milliseconds timeout) {
	static std::shared_ptr<MessageFilter> NeoMemorySDRead = std::make_shared<MessageFilter>(Network::NetID::NeoMemorySDRead);

	if(amount > getBlockSizeBounds().second)
		return nullopt;

	if(amount % getBlockSizeBounds().first != 0)
		return nullopt;

	if(pos % getBlockSizeBounds().first != 0)
		return nullopt;

	uint64_t largeSector = pos / SectorSize;
	uint32_t sector = uint32_t(largeSector);
	if (largeSector != uint64_t(sector))
		return nullopt;

	std::mutex m;
	std::condition_variable cv;
	uint32_t copied = 0;
	bool error = false;
	std::unique_lock<std::mutex> lk(m);
	auto cb = com.addMessageCallback(MessageCallback([&](std::shared_ptr<Message> msg) {
		std::unique_lock<std::mutex> lk(m);

		const auto sdmsg = std::dynamic_pointer_cast<NeoReadMemorySDMessage>(msg);
		if(!sdmsg || amount < copied + sdmsg->data.size()) {
			error = true;
			lk.unlock();
			cv.notify_all();
			return;
		}

		memcpy(into + copied, sdmsg->data.data(), sdmsg->data.size());
		copied += uint32_t(sdmsg->data.size());
		if(copied == amount) {
			lk.unlock();
			cv.notify_all();
		}
	}, NeoMemorySDRead));

	com.rawWrite({
		uint8_t(MultiChannelCommunication::CommandType::HostPC_from_SDCC1),
		uint8_t(sector & 0xFF),
		uint8_t((sector >> 8) & 0xFF),
		uint8_t((sector >> 16) & 0xFF),
		uint8_t((sector >> 24) & 0xFF),
		uint8_t(amount & 0xFF),
		uint8_t((amount >> 8) & 0xFF),
	});

	bool hitTimeout = !cv.wait_for(lk, timeout, [&copied, &error, &amount] { return error || copied == amount; });
	com.removeMessageCallback(cb);

	if(hitTimeout)
		return nullopt;

	return amount;
}