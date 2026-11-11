// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/fs/path_util.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/memory.h"

namespace Service::Sockets {

SFDNSRES::SFDNSRES(Core::System& system_) : ServiceFramework{system_, "sfdnsres"} {
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetDnsAddressesPrivateRequest"},
        {1, nullptr, "GetDnsAddressPrivateRequest"},
        {2, &SFDNSRES::GetHostByNameRequest, "GetHostByNameRequest"},
        {3, nullptr, "GetHostByAddrRequest"},
        {4, nullptr, "GetHostStringErrorRequest"},
        {5, &SFDNSRES::GetGaiStringErrorRequest, "GetGaiStringErrorRequest"},
        {6, &SFDNSRES::GetAddrInfoRequest, "GetAddrInfoRequest"},
        {7, nullptr, "GetNameInfoRequest"},
        {8, nullptr, "RequestCancelHandleRequest"},
        {9, nullptr, "CancelRequest"},
        {10, &SFDNSRES::GetHostByNameRequestWithOptions, "GetHostByNameRequestWithOptions"},
        {11, nullptr, "GetHostByAddrRequestWithOptions"},
        {12, &SFDNSRES::GetAddrInfoRequestWithOptions, "GetAddrInfoRequestWithOptions"},
        {13, nullptr, "GetNameInfoRequestWithOptions"},
        {14, &SFDNSRES::ResolverSetOptionRequest, "ResolverSetOptionRequest"},
        {15, nullptr, "ResolverGetOptionRequest"},
    };
    RegisterHandlers(functions);
}

SFDNSRES::~SFDNSRES() = default;

enum class NetDbError : s32 {
    Internal = -1,
    Success = 0,
    HostNotFound = 1,
    TryAgain = 2,
    NoRecovery = 3,
    NoData = 4,
};

static const constexpr std::array blockedDomains = {
    "srv.nintendo.net", //obvious
    "nintendo.es",
    "nintendowifi.net",
    "nintendo-europe.com",
    "nintendo.com.hk",
    "nintendo.com.au",
    "nintendo.co.kr",
    "nintendo.co.uk",
    "nintendo.co.jp",
    "nintendo.co.nz",
    "nintendo.co.za",
    "nintendo.com",
    "nintendo.jp",
    "nintendo.tw",
    "nintendo.at",
    "nintendo.be",
    "nintendo.dk",
    "nintendo.de",
    "nintendo.fi",
    "nintendo.fr",
    "nintendo.gr",
    "nintendo.hu",
    "nintendo.it",
    "nintendo.nl",
    "nintendo.no",
    "nintendo.pt",
    "nintendo.ru",
    "nintendo.ch",
    "nintendo.se",
    "nintendoswitch.com.cn",
    "nintendoswitch.com",
    "sun.hac.lp1.d4c.nintendo.net",
    "phoenix-api.wbagora.com", //hogwarts legacy
    "battle.net",
    "microsoft.com", // Minecraft dungeons + other games
    "mojang.com",
    "xboxlive.com",
    "api.epicgames.dev", // marvel cosmic invasion +?
    "minecraftservices.com",
    "508223012e5a5ff19f30a391b2bdadc0.my.2k.com", // Civilization 5
};

static bool IsBlockedHost(const std::string& host) {
    return std::any_of(
        blockedDomains.begin(), blockedDomains.end(),
        [&host](const std::string& domain) { return host.find(domain) != std::string::npos; });
}

// Address of the online server. NX2_ONLINE_SERVER wins when set; otherwise the first line of
// online_server.txt in the config folder (created with the default on first launch, so it can be
// changed once by editing that one line); otherwise the built-in default.
static constexpr const char* kDefaultOnlineServer = "2.29.165.22";

static std::string OnlineServerAddress() {
    static const std::string address = []() -> std::string {
        if (const char* env = std::getenv("NX2_ONLINE_SERVER"); env != nullptr && *env != '\0') {
            return std::string(env);
        }
        const auto file =
            Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "online_server.txt";
        std::string value;
        {
            std::ifstream in(file);
            std::string line;
            while (std::getline(in, line)) {
                const auto first = line.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) {
                    continue;
                }
                const auto last = line.find_last_not_of(" \t\r\n");
                value = line.substr(first, last - first + 1);
                break;
            }
        }
        if (value.empty()) {
            value = kDefaultOnlineServer;
            std::ofstream out(file);
            if (out) {
                out << value << "\n";
            }
        }
        LOG_INFO(Network, "[ONLINE] server address {} (file {})", value, file.string());
        return value;
    }();
    return address;
}

// Online Switch 2: the game's NPLN servers (*.npln.srv.nintendo.net) go to our own server
// instead of being blocked. Address from NX2_ONLINE_SERVER, else this PC (127.0.0.1).
static std::string RedirectOnlineHost(const std::string& raw_host) {
    // The game leaves a "%" where the console (nsd) writes its environment: "lp1" on retail.
    std::string host = raw_host;
    for (std::size_t at = host.find('%'); at != std::string::npos; at = host.find('%', at + 3)) {
        host.replace(at, 1, "lp1");
    }
    const auto ends_with = [&host](std::string_view suffix) {
        return host.size() > suffix.size() &&
               host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    // NPLN servers, and the NAT-check servers Pia asks before it links players (nncs1 / nncs2).
    const bool npln = ends_with(".npln.srv.nintendo.net");
    const bool nncs = host.rfind("nncs", 0) == 0 && ends_with(".srv.nintendo.net");
    const bool pserver = host.rfind("g", 0) == 0 && ends_with(".p.srv.nintendo.net");
    if (!npln && !nncs && !pserver) {
        return host;
    }
    const std::string target = OnlineServerAddress();
    LOG_INFO(Network, "[ONLINE] {} -> {}", host, target);
    Core::OnlineArmThreadDump();
    return target;
}

static NetDbError GetAddrInfoErrorToNetDbError(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return NetDbError::Success;
    case GetAddrInfoError::AGAIN:
        return NetDbError::TryAgain;
    case GetAddrInfoError::NODATA:
        return NetDbError::HostNotFound;
    case GetAddrInfoError::SERVICE:
        return NetDbError::Success;
    default:
        return NetDbError::HostNotFound;
    }
}

static Errno GetAddrInfoErrorToErrno(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        // Note: Sometimes a successful lookup sets errno to EADDRNOTAVAIL for
        // some reason, but that doesn't seem useful to implement.
        return Errno::SUCCESS;
    case GetAddrInfoError::AGAIN:
        return Errno::SUCCESS;
    case GetAddrInfoError::NODATA:
        return Errno::SUCCESS;
    case GetAddrInfoError::SERVICE:
        return Errno::INVAL;
    default:
        return Errno::SUCCESS;
    }
}

template <typename T>
static void Append(std::vector<u8>& vec, T t) {
    const size_t offset = vec.size();
    vec.resize(offset + sizeof(T));
    std::memcpy(vec.data() + offset, &t, sizeof(T));
}

static void AppendNulTerminated(std::vector<u8>& vec, std::string_view str) {
    const size_t offset = vec.size();
    vec.resize(offset + str.size() + 1);
    std::memmove(vec.data() + offset, str.data(), str.size());
}

// We implement gethostbyname using the host's getaddrinfo rather than the
// host's gethostbyname, because it simplifies portability: e.g., getaddrinfo
// behaves the same on Unix and Windows, unlike gethostbyname where Windows
// doesn't implement h_errno.
static std::vector<u8> SerializeAddrInfoAsHostEnt(const std::vector<Network::AddrInfo>& vec,
                                                  std::string_view host) {

    std::vector<u8> data;
    // h_name: use the input hostname (append nul-terminated)
    AppendNulTerminated(data, host);
    // h_aliases: leave empty

    Append<u32_be>(data, 0); // count of h_aliases
    // (If the count were nonzero, the aliases would be appended as nul-terminated here.)
    Append<u16_be>(data, static_cast<u16>(Domain::INET)); // h_addrtype
    Append<u16_be>(data, sizeof(Network::IPv4Address));   // h_length
    // h_addr_list:
    size_t count = vec.size();
    ASSERT(count <= UINT32_MAX);
    Append<u32_be>(data, static_cast<uint32_t>(count));
    for (const Network::AddrInfo& addrinfo : vec) {
        // On the Switch, this is passed through htonl despite already being
        // big-endian, so it ends up as little-endian.
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip));

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }
    return data;
}

static std::pair<u32, GetAddrInfoError> GetHostByNameRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(
        Service,
        "called with ignored parameters: use_nsd_resolve={}, cancel_handle={}, process_id={}",
        parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    const std::string host = RedirectOnlineHost(Common::StringFromBuffer(host_buffer));
    // For now, ignore options, which are in input buffer 1 for GetHostByNameRequestWithOptions.

    // Prevent resolution of Nintendo servers
    if (IsBlockedHost(host)) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    auto res_v = Network::GetAddressInfo(host, /*service*/ std::nullopt);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        const std::vector<u8> data = SerializeAddrInfoAsHostEnt(*res, host);
        const u32 data_size = u32(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }
    auto* err = std::get_if<Network::GetAddrInfoError>(&res_v);
    return {0, Translate(*err)};
}

void SFDNSRES::GetHostByNameRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        NetDbError netdb_error;
        Errno bsd_errno;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .data_size = data_size,
    });
}

void SFDNSRES::GetHostByNameRequestWithOptions(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

static std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& vec,
                                         std::string_view host) {
    // Adapted from
    // https://github.com/switchbrew/libnx/blob/c5a9a909a91657a9818a3b7e18c9b91ff0cbb6e3/nx/source/runtime/resolver.c#L190
    std::vector<u8> data;

    for (const Network::AddrInfo& addrinfo : vec) {
        // serialized addrinfo:
        Append<u32_be>(data, 0xBEEFCAFE);                                        // magic
        Append<u32_be>(data, 0);                                                 // ai_flags
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.family)));      // ai_family
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.socket_type))); // ai_socktype
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.protocol)));    // ai_protocol
        Append<u32_be>(data, 16); // ai_addrlen
        // ^ *not* sizeof(SerializedSockAddrIn), not that it matters since they're the same size

        // ai_addr:
        Append<u16_be>(data, static_cast<u16>(Translate(addrinfo.addr.family))); // sin_family
        // On the Switch, the following fields are passed through htonl despite
        // already being big-endian, so they end up as little-endian.
        Append<u16_le>(data, addrinfo.addr.portno);                            // sin_port
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip)); // sin_addr
        data.resize(data.size() + 8, 0);                                       // sin_zero

        if (addrinfo.canon_name.has_value()) {
            AppendNulTerminated(data, *addrinfo.canon_name);
        } else {
            data.push_back(0);
        }

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }

    data.resize(data.size() + 4, 0); // 4-byte sentinel value

    return data;
}

// A console's getaddrinfo answers with the socket type and protocol asked for in the hints; the
// host's leaves them at 0 (Windows always does), and games then find no usable address.
// Switch values: SOCK_STREAM 1, SOCK_DGRAM 2, IPPROTO_TCP 6, IPPROTO_UDP 17.
static void FillConsoleSocketKinds(std::vector<Network::AddrInfo>& results, u32 hint_socktype,
                                   u32 hint_protocol) {
    const bool type_hinted = hint_socktype == 1 || hint_socktype == 2;
    const Network::Type wanted_type =
        hint_socktype == 2 ? Network::Type::DGRAM : Network::Type::STREAM;
    std::erase_if(results, [&](const Network::AddrInfo& info) {
        return type_hinted && info.socket_type != Network::Type::Unspecified &&
               info.socket_type != wanted_type;
    });
    for (auto& info : results) {
        if (info.socket_type == Network::Type::Unspecified) {
            info.socket_type = wanted_type;
        }
        if (info.protocol == Network::Protocol::Unspecified ||
            info.protocol == Network::Protocol::IP) {
            if (hint_protocol == 6) {
                info.protocol = Network::Protocol::TCP;
            } else if (hint_protocol == 17) {
                info.protocol = Network::Protocol::UDP;
            } else {
                info.protocol = info.socket_type == Network::Type::DGRAM ? Network::Protocol::UDP
                                                                         : Network::Protocol::TCP;
            }
        }
    }
}

static std::pair<u32, GetAddrInfoError> GetAddrInfoRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_WARNING(
        Service,
        "called with ignored parameters: use_nsd_resolve={}, cancel_handle={}, process_id={}",
        parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    // TODO: If use_nsd_resolve is true, pass the name through NSD::Resolve
    // before looking up.

    const auto host_buffer = ctx.ReadBuffer(0);
    const std::string host = RedirectOnlineHost(Common::StringFromBuffer(host_buffer));

    // Prevent resolution of Nintendo servers
    if (IsBlockedHost(host)) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    std::optional<std::string> service = std::nullopt;
    if (ctx.CanReadBuffer(1)) {
        const std::span<const u8> service_buffer = ctx.ReadBuffer(1);
        service = Common::StringFromBuffer(service_buffer);
    }

    // Serialized hints come in the third buffer, big-endian: magic 0xBEEFCAFE, ai_flags,
    // ai_family, ai_socktype, ai_protocol (read without the magic if it is missing).
    u32 hint_socktype = 0;
    u32 hint_protocol = 0;
    if (ctx.CanReadBuffer(2)) {
        const std::span<const u8> hints_buffer = ctx.ReadBuffer(2);
        const auto read_be = [&hints_buffer](std::size_t offset) -> u32 {
            if (offset + 4 > hints_buffer.size()) {
                return 0;
            }
            return (u32{hints_buffer[offset]} << 24) | (u32{hints_buffer[offset + 1]} << 16) |
                   (u32{hints_buffer[offset + 2]} << 8) | u32{hints_buffer[offset + 3]};
        };
        const std::size_t base = read_be(0) == 0xBEEFCAFE ? 4 : 0;
        hint_socktype = read_be(base + 8);
        hint_protocol = read_be(base + 12);
    }

    auto res_v = Network::GetAddressInfo(host, service);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        FillConsoleSocketKinds(*res, hint_socktype, hint_protocol);
        LOG_INFO(Network, "[ONLINE] getaddrinfo {} : hints socktype={} protocol={}, {} result(s)",
                 host, hint_socktype, hint_protocol, res->size());
        const std::vector<u8> data = SerializeAddrInfo(*res, host);
        const u32 data_size = u32(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }
    auto* err = std::get_if<Network::GetAddrInfoError>(&res_v);
    return {0, Translate(*err)};
}

void SFDNSRES::GetAddrInfoRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        Errno bsd_errno;
        GetAddrInfoError gai_error;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .gai_error = emu_gai_err,
        .data_size = data_size,
    });
}

void SFDNSRES::GetGaiStringErrorRequest(HLERequestContext& ctx) {
    struct InputParameters {
        GetAddrInfoError gai_errno;
    };
    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    const std::string result = Translate(input.gai_errno);
    ctx.WriteBuffer(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    // Additional options are ignored
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        GetAddrInfoError gai_error;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x10);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .gai_error = emu_gai_err,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

void SFDNSRES::ResolverSetOptionRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}


DNS_PRIV::DNS_PRIV(Core::System& system_)
    : ServiceFramework{system_, "dns:priv"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "Cmd0"},
        {1, nullptr, "Cmd1"},
        {2, nullptr, "Cmd2"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

DNS_PRIV::~DNS_PRIV() = default;


} // namespace Service::Sockets
