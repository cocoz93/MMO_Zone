#include "PacketTable.h"

#include "NetHeader.h"

CPacketTable::CPacketTable(uint16_t firstType, uint16_t lastType)
    : _firstType(firstType), _lastType(lastType)
{
    if (lastType >= firstType)
        _entries.resize(static_cast<size_t>(lastType - firstType) + 1);
}

bool CPacketTable::Register(uint16_t type, uint16_t minSize, Handler handler)
{
    if (type < _firstType || type > _lastType || !handler)
        return false;

    Entry& entry = _entries[type - _firstType];
    if (entry.handler)
        return false;

    entry.minSize = minSize;
    entry.handler = std::move(handler);
    return true;
}

CPacketTable::Dispatch CPacketTable::Route(SessionId sessionId, CSerialBuffer* msg) const
{
    MsgHeader header{};
    if (msg->PeekData(reinterpret_cast<char*>(&header), sizeof(header)) != static_cast<int>(sizeof(header)))
        return Dispatch::NoHeader;

    const uint16_t type = static_cast<uint16_t>(header.type);
    if (type < _firstType || type > _lastType)
        return Dispatch::UnknownType;

    const Entry& entry = _entries[type - _firstType];
    if (!entry.handler)
        return Dispatch::UnknownType;
    if (header.size < entry.minSize)
        return Dispatch::TooSmall;

    entry.handler(sessionId, msg);
    return Dispatch::Handled;
}
