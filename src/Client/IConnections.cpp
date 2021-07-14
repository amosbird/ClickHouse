#include <Client/IConnections.h>
#include <Poco/Net/SocketImpl.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int SOCKET_TIMEOUT;
}

struct PocoSocketWrapper : public Poco::Net::SocketImpl
{
    explicit PocoSocketWrapper(int fd)
    {
        reset(fd);
    }

    // Do not close fd.
    ~PocoSocketWrapper() override = default;
};

void IConnections::DrainCallback::operator()(int fd, Poco::Timespan, const std::string fd_description) const
{
    if (!PocoSocketWrapper(fd).poll(drain_timeout, Poco::Net::Socket::SELECT_READ))
        throw Exception(ErrorCodes::SOCKET_TIMEOUT, "Read timeout while draining from {}", fd_description);
}

}
