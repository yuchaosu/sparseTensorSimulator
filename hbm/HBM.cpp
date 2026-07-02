#include "HBM.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
constexpr uint64_t kInvalidArrival = std::numeric_limits<uint64_t>::max();
}

HBMChannel::HBMChannel()
	: banks(BANKS_PER_RANK),
	  readQueue(),
	  writeQueue(),
	  currentTime(0),
	  nextRefreshTime(tREFI),
	  busAvailableTime(0),
	  lastActivationTime{0, 0, 0, 0},
	  activationIndex(0),
	  dataBusAvailableTime(0),
	  lastCommandDataTime(0),
	  lastCommandWasWrite(false),
	  lastActivateAnyBank(0) {}

uint64_t HBMChannel::bytesPerBurst() const {
	return static_cast<uint64_t>(DEVICE_BUS_WIDTH / 8) * BURST_LENGTH;
}

uint64_t HBMChannel::getBank(uint64_t address) {
	const uint64_t normalized = address % CHANNEL_SIZE;
	return (normalized / ROW_BUFFER_SIZE) % BANKS_PER_RANK;
}

uint64_t HBMChannel::getRow(uint64_t address) {
	const uint64_t normalized = address % CHANNEL_SIZE;
	return normalized / (ROW_BUFFER_SIZE * BANKS_PER_RANK);
}

uint64_t HBMChannel::getColumn(uint64_t address) {
	const uint64_t normalized = address % CHANNEL_SIZE;
	return normalized % ROW_BUFFER_SIZE;
}

bool HBMChannel::isRowHit(const MemoryRequest& req, int bank) {
	if (bank < 0 || bank >= static_cast<int>(banks.size())) {
		return false;
	}
	const Bank& targetBank = banks[bank];
	return targetBank.state == Bank::ACTIVE && targetBank.activeRow == getRow(req.address);
}

MemoryRequest* HBMChannel::selectRequest(std::vector<MemoryRequest>& queue) {
	MemoryRequest* best = nullptr;
	for (auto& req : queue) {
		if (req.arrivalTime > currentTime) {
			continue;
		}
		if (!best) {
			best = &req;
			continue;
		}

		const int reqBank = static_cast<int>(getBank(req.address));
		const int bestBank = static_cast<int>(getBank(best->address));
		const bool reqRowHit = isRowHit(req, reqBank);
		const bool bestRowHit = isRowHit(*best, bestBank);

		if (reqRowHit != bestRowHit) {
			if (reqRowHit) {
				best = &req;
			}
			continue;
		}

		if (req.arrivalTime < best->arrivalTime) {
			best = &req;
			continue;
		}

		if (req.arrivalTime == best->arrivalTime) {
			const uint64_t reqReady = banks[reqBank].nextAvailableTime;
			const uint64_t bestReady = banks[bestBank].nextAvailableTime;
			if (reqReady < bestReady) {
				best = &req;
				continue;
			}
			if (reqReady == bestReady && req.requestId < best->requestId) {
				best = &req;
			}
		}
	}
	return best;
}

MemoryRequest* HBMChannel::selectBestRequest() {
	MemoryRequest* readCandidate = selectRequest(readQueue);
	MemoryRequest* writeCandidate = selectRequest(writeQueue);

	if (!readCandidate) {
		return writeCandidate;
	}
	if (!writeCandidate) {
		return readCandidate;
	}

	const bool readRowHit = isRowHit(*readCandidate, static_cast<int>(getBank(readCandidate->address)));
	const bool writeRowHit = isRowHit(*writeCandidate, static_cast<int>(getBank(writeCandidate->address)));

	if (readRowHit != writeRowHit) {
		return readRowHit ? readCandidate : writeCandidate;
	}

	if (readCandidate->arrivalTime != writeCandidate->arrivalTime) {
		return (readCandidate->arrivalTime < writeCandidate->arrivalTime) ? readCandidate : writeCandidate;
	}

	return (readCandidate->requestId <= writeCandidate->requestId) ? readCandidate : writeCandidate;
}

uint64_t HBMChannel::findNextArrivalTime() const {
	uint64_t nextArrival = kInvalidArrival;
	for (const auto& req : readQueue) {
		nextArrival = std::min(nextArrival, req.arrivalTime);
	}
	for (const auto& req : writeQueue) {
		nextArrival = std::min(nextArrival, req.arrivalTime);
	}
	return nextArrival;
}

void HBMChannel::addRequest(const MemoryRequest& req) {
	if (!canAccept()) {
		throw std::runtime_error("HBMChannel queue is full");
	}
	if (req.isWrite) {
		writeQueue.push_back(req);
	} else {
		readQueue.push_back(req);
	}
}

bool HBMChannel::canAccept() {
	return (readQueue.size() + writeQueue.size()) < MAX_QUEUE_SIZE;
}

void HBMChannel::tick(uint64_t time) {
	if (time > currentTime) {
		currentTime = time;
	}

	while (currentTime >= nextRefreshTime) {
		performRefresh();
	}

	processQueues();
}

void HBMChannel::processQueues() {
	while (!readQueue.empty() || !writeQueue.empty()) {
		if (currentTime >= nextRefreshTime) {
			performRefresh();
			continue;
		}

		MemoryRequest* selected = selectBestRequest();
		if (!selected) {
			const uint64_t nextArrival = findNextArrivalTime();
			if (nextArrival == kInvalidArrival) {
				return;
			}
			if (nextArrival > currentTime) {
				currentTime = nextArrival;
				continue;
			}
			return;
		}

		const MemoryRequest reqCopy = *selected;
		std::vector<MemoryRequest>& sourceQueue = reqCopy.isWrite ? writeQueue : readQueue;
		const size_t index = static_cast<size_t>(selected - sourceQueue.data());
		sourceQueue.erase(sourceQueue.begin() + index);

		serviceRequestInternal(reqCopy);
	}
}

void HBMChannel::performRefresh() {
	uint64_t refreshStart = std::max({nextRefreshTime, currentTime, busAvailableTime, dataBusAvailableTime});
	for (auto& bank : banks) {
		refreshStart = std::max(refreshStart, bank.nextAvailableTime);
		bank.state = Bank::REFRESHING;
	}

	const uint64_t refreshEnd = refreshStart + tRFC;
	busAvailableTime = refreshEnd;
	dataBusAvailableTime = refreshEnd;
	currentTime = refreshEnd;
	lastCommandDataTime = 0;
	lastCommandWasWrite = false;
	lastActivateAnyBank = refreshEnd;
	for (auto& stamp : lastActivationTime) {
		stamp = refreshEnd;
	}

	for (auto& bank : banks) {
		bank.state = Bank::IDLE;
		bank.activeRow = 0;
		bank.nextAvailableTime = refreshEnd;
		bank.prechargeAllowedTime = refreshEnd;
	}

	nextRefreshTime = refreshStart + tREFI;
}

// Drive the full command sequence for one memory request while honoring timing guards.
uint64_t HBMChannel::serviceRequestInternal(const MemoryRequest& req) {
	const uint64_t burstBytes = bytesPerBurst();
	uint64_t remainingBytes = std::max<uint64_t>(req.size, 1);
	uint64_t bursts = remainingBytes / burstBytes;
	if (remainingBytes % burstBytes != 0) {
		++bursts;
	}
	bursts = std::max<uint64_t>(bursts, 1);

	currentTime = std::max(currentTime, req.arrivalTime);

	while (currentTime >= nextRefreshTime) {
		performRefresh();
	}

	const int bankId = static_cast<int>(getBank(req.address));
	Bank& bank = banks[bankId];
	const uint64_t targetRow = getRow(req.address);

	if (bank.state == Bank::REFRESHING && currentTime < bank.nextAvailableTime) {
		currentTime = bank.nextAvailableTime;
		bank.state = Bank::IDLE;
		bank.prechargeAllowedTime = currentTime;
	}

	if (bank.state == Bank::PRECHARGING && currentTime < bank.nextAvailableTime) {
		currentTime = bank.nextAvailableTime;
		bank.state = Bank::IDLE;
		bank.prechargeAllowedTime = currentTime;
	}

	if (bank.state == Bank::ACTIVE && bank.activeRow != targetRow) {
		uint64_t preStart = std::max({currentTime, bank.nextAvailableTime, busAvailableTime, bank.prechargeAllowedTime});
		while (preStart >= nextRefreshTime) {
			currentTime = preStart;
			performRefresh();
			preStart = std::max({currentTime, bank.nextAvailableTime, busAvailableTime, bank.prechargeAllowedTime});
		}
		const uint64_t preEnd = preStart + tRP;
		busAvailableTime = preStart + tCK;
		bank.state = Bank::PRECHARGING;
		bank.nextAvailableTime = preEnd;
		currentTime = preEnd;
		bank.state = Bank::IDLE;
		bank.activeRow = 0;
		bank.prechargeAllowedTime = currentTime;
	}

	if (bank.state != Bank::ACTIVE || bank.activeRow != targetRow) {
		uint64_t actStart = std::max({currentTime, bank.nextAvailableTime, busAvailableTime, lastActivateAnyBank + tRRD});
		const uint64_t xawLimit = lastActivationTime[activationIndex] + tXAW;
		if (actStart < xawLimit) {
			actStart = xawLimit;
		}
		while (actStart >= nextRefreshTime) {
			currentTime = actStart;
			performRefresh();
			actStart = std::max({currentTime, bank.nextAvailableTime, busAvailableTime, lastActivateAnyBank + tRRD});
			const uint64_t newLimit = lastActivationTime[activationIndex] + tXAW;
			if (actStart < newLimit) {
				actStart = newLimit;
			}
		}

		const uint64_t actEnd = actStart + tRCD;
		busAvailableTime = actStart + tCK;
		bank.state = Bank::ACTIVE;
		bank.activeRow = targetRow;
		bank.nextAvailableTime = actEnd;
		bank.prechargeAllowedTime = actStart + tRAS;
		lastActivateAnyBank = actStart;
		lastActivationTime[activationIndex] = actStart;
		activationIndex = (activationIndex + 1) % 4;
		currentTime = actEnd;
	} else {
		currentTime = std::max(currentTime, bank.nextAvailableTime);
	}

	for (uint64_t burst = 0; burst < bursts; ++burst) {
		if (currentTime >= nextRefreshTime) {
			performRefresh();
		}

		currentTime = std::max(currentTime, bank.nextAvailableTime);

		uint64_t columnTime = std::max(currentTime, busAvailableTime);
		while (columnTime >= nextRefreshTime) {
			currentTime = columnTime;
			performRefresh();
			columnTime = std::max(currentTime, busAvailableTime);
		}

		const bool prevWasWrite = lastCommandWasWrite;
		const uint64_t prevDataTime = lastCommandDataTime;

		busAvailableTime = columnTime + tCK;

		uint64_t dataStart;
		if (req.isWrite) {
			dataStart = std::max(columnTime, dataBusAvailableTime);
			if (!prevWasWrite && prevDataTime > 0) {
				dataStart = std::max(dataStart, prevDataTime + tRTW);
			}
		} else {
			dataStart = std::max(columnTime + tCL, dataBusAvailableTime);
			if (prevWasWrite && prevDataTime > 0) {
				dataStart = std::max(dataStart, prevDataTime + tWTR);
			}
		}

	const uint64_t dataEnd = dataStart + tBURST;
	dataBusAvailableTime = dataEnd;
		lastCommandDataTime = dataStart;
		lastCommandWasWrite = req.isWrite;

		const uint64_t columnGap = columnTime + tCK;
		bank.nextAvailableTime = std::max(bank.nextAvailableTime, columnGap);
		if (req.isWrite) {
			bank.prechargeAllowedTime = std::max(bank.prechargeAllowedTime, dataEnd + tWR);
		} else {
			bank.prechargeAllowedTime = std::max(bank.prechargeAllowedTime, columnTime + tRTP);
		}

		currentTime = std::max(currentTime, columnGap);
	}

	return currentTime - req.arrivalTime;
}

uint64_t HBMChannel::getLatency(const MemoryRequest& req) {
	HBMChannel snapshot(*this);
	return snapshot.serviceRequestInternal(req);
}

bool HBMChannel::hasPendingRequests() const {
	return !readQueue.empty() || !writeQueue.empty();
}

HBMController::HBMController()
	: channels(NUM_CHANNELS),
	  currentTime(0) {}

int HBMController::getChannel(uint64_t address) {
	static constexpr uint64_t CHANNEL_STRIPE = 64;
	return static_cast<int>((address / CHANNEL_STRIPE) % NUM_CHANNELS);
}

uint64_t HBMController::getChannelAddress(uint64_t address) {
	static constexpr uint64_t CHANNEL_STRIPE = 64;
	const uint64_t stripeIndex = address / CHANNEL_STRIPE;
	const uint64_t stripeOffset = address % CHANNEL_STRIPE;
	return (stripeIndex / NUM_CHANNELS) * CHANNEL_STRIPE + stripeOffset;
}

void HBMController::tick() {
	++currentTime;
	for (auto& channel : channels) {
		channel.tick(currentTime);
	}
}

void HBMController::addRequest(uint64_t address, bool isWrite, uint64_t size, int requestId) {
	const int channelId = getChannel(address);
	HBMChannel& channel = channels[channelId];
	const uint64_t channelAddress = getChannelAddress(address);
	const uint64_t arrival = std::max(currentTime, channel.getCurrentTime());
	MemoryRequest request(channelAddress, isWrite, arrival, size, requestId);

	if (!channel.canAccept()) {
		throw std::runtime_error("HBM channel queue full");
	}

	channel.addRequest(request);
}

bool HBMController::hasPendingRequests() const {
	for (const auto& channel : channels) {
		if (channel.hasPendingRequests()) {
			return true;
		}
	}
	return false;
}

uint64_t HBMController::getMaxChannelTime() const {
	uint64_t maxTime = currentTime;
	for (const auto& channel : channels) {
		maxTime = std::max(maxTime, channel.getCompletionTime());
	}
	return maxTime;
}
