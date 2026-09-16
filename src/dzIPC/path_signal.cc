#include "dzIPC/common/path_switch.h"

namespace dzIPC {
namespace path {

const char* to_string(State s) noexcept
{
    switch (s)
    {
    case State::Idle: return "Idle";
    case State::Probe: return "Probe";
    case State::Negotiate: return "Negotiate";
    case State::Establish: return "Establish";
    case State::Active: return "Active";
    case State::Withdraw: return "Withdraw";
    case State::Teardown: return "Teardown";
    case State::Closed: return "Closed";
    }
    return "?";
}

const char* to_string(Kind k) noexcept
{
    switch (k)
    {
    case Kind::None: return "None";
    case Kind::Socket: return "Socket";
    case Kind::Shm: return "Shm";
    }
    return "?";
}

const char* to_string(DecisionReason r) noexcept
{
    switch (r)
    {
    case DecisionReason::Pending: return "Pending";
    case DecisionReason::PeerInPool: return "PeerInPool";
    case DecisionReason::NoEvidence: return "NoEvidence";
    case DecisionReason::Timeout: return "Timeout";
    case DecisionReason::ShmNotReady: return "ShmNotReady";
    case DecisionReason::ChannelOccupied: return "ChannelOccupied";
    }
    return "?";
}

const char* to_string(FallbackReason r) noexcept
{
    switch (r)
    {
    case FallbackReason::None: return "None";
    case FallbackReason::ShmEstablishFailed: return "ShmEstablishFailed";
    case FallbackReason::ShmRendezvousTimeout: return "ShmRendezvousTimeout";
    case FallbackReason::ShmChannelOccupied: return "ShmChannelOccupied";
    case FallbackReason::RemoteIoFailure: return "RemoteIoFailure";
    case FallbackReason::WithdrawnByPeer: return "WithdrawnByPeer";
    }
    return "?";
}

}   // namespace path
}   // namespace dzIPC
