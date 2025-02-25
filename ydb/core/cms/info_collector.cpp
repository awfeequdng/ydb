#include "cms_impl.h"
#include "info_collector.h"

#include <ydb/core/base/tablet_pipe.h>
#include <ydb/core/blobstorage/base/blobstorage_events.h>
#include <ydb/core/mind/tenant_pool.h>
#include <ydb/core/node_whiteboard/node_whiteboard.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/interconnect.h>
#include <library/cpp/actors/core/log.h>

#define LOG_T(stream) LOG_TRACE_S (*TlsActivationContext, NKikimrServices::CMS, "[InfoCollector] " << stream)
#define LOG_D(stream) LOG_DEBUG_S (*TlsActivationContext, NKikimrServices::CMS, "[InfoCollector] " << stream)
#define LOG_I(stream) LOG_INFO_S  (*TlsActivationContext, NKikimrServices::CMS, "[InfoCollector] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::CMS, "[InfoCollector] " << stream)
#define LOG_W(stream) LOG_WARN_S  (*TlsActivationContext, NKikimrServices::CMS, "[InfoCollector] " << stream)
#define LOG_E(stream) LOG_ERROR_S (*TlsActivationContext, NKikimrServices::CMS, "[InfoCollector] " << stream)

namespace NKikimr {
namespace NCms {

using namespace NNodeWhiteboard;
using namespace NKikimrWhiteboard;

class TInfoCollector: public TActorBootstrapped<TInfoCollector> {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::CMS_INFO_COLLECTOR;
    }

    explicit TInfoCollector(const TActorId& client, const TDuration& timeout)
        : Client(client)
        , Timeout(timeout)
        , Info(new TClusterInfo)
        , BaseConfigReceived(false)
    {
    }

    void Bootstrap();

private:
    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            sFunc(TEvents::TEvWakeup, ReplyAndDie);

            // Nodes
            hFunc(TEvInterconnect::TEvNodesInfo, Handle);

            // BSC
            hFunc(TEvBlobStorage::TEvControllerConfigResponse, Handle);
            hFunc(TEvTabletPipe::TEvClientConnected, Handle);
            hFunc(TEvTabletPipe::TEvClientDestroyed, Handle);

            // Whiteboard & TenantPool
            hFunc(TEvWhiteboard::TEvSystemStateResponse, Handle);
            hFunc(TEvWhiteboard::TEvTabletStateResponse, Handle);
            hFunc(TEvWhiteboard::TEvPDiskStateResponse, Handle);
            hFunc(TEvWhiteboard::TEvVDiskStateResponse, Handle);
            hFunc(TEvTenantPool::TEvTenantPoolStatus, Handle);
            hFunc(TEvents::TEvUndelivered, Handle);
            hFunc(TEvInterconnect::TEvNodeDisconnected, Handle);
            IgnoreFunc(TEvInterconnect::TEvNodeConnected);

        default:
            LOG_E("Unexpected event"
                << ": type# " << ev->GetTypeRewrite()
                << ", event# " << (ev->HasEvent() ? ev->GetBase()->ToString() : "serialized?"));
        }
    }

    void ReplyAndDie();
    void MaybeReplyAndDie();
    void PassAway() override;

    // Nodes
    void Handle(TEvInterconnect::TEvNodesInfo::TPtr& ev);

    // BSC
    void RequestBaseConfig();
    void Handle(TEvBlobStorage::TEvControllerConfigResponse::TPtr& ev);
    void Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev);
    void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr& ev);
    void OnPipeDestroyed();

    // Whiteboard & TenantPool
    void SendNodeRequests(ui32 nodeId);
    void SendNodeEvent(ui32 nodeId, const TActorId& recipient, IEventBase* request, ui32 responseType);
    bool IsNodeInfoRequired(ui32 nodeId, ui32 eventType) const;
    void ResponseProcessed(ui32 nodeId, ui32 eventType);
    void Handle(TEvWhiteboard::TEvSystemStateResponse::TPtr& ev);
    void Handle(TEvWhiteboard::TEvTabletStateResponse::TPtr& ev);
    void Handle(TEvWhiteboard::TEvPDiskStateResponse::TPtr& ev);
    void Handle(TEvWhiteboard::TEvVDiskStateResponse::TPtr& ev);
    void Handle(TEvTenantPool::TEvTenantPoolStatus::TPtr& ev);
    void Handle(TEvents::TEvUndelivered::TPtr& ev);
    void Handle(TEvInterconnect::TEvNodeDisconnected::TPtr& ev);

private:
    const TActorId Client;
    const TDuration Timeout;

    TClusterInfoPtr Info;
    TActorId BscPipe;
    bool BaseConfigReceived;
    THashMap<ui32, TSet<ui32>> NodeEvents; // nodeId -> expected events
    THashMap<TPDiskID, TPDiskStateInfo, TPDiskIDHash> PDiskInfo;
    THashMap<TVDiskID, TVDiskStateInfo> VDiskInfo;

}; // TInfoCollector

void TInfoCollector::ReplyAndDie() {
    auto ev = MakeHolder<TCms::TEvPrivate::TEvClusterInfo>();
    ev->Success = BaseConfigReceived;

    if (BaseConfigReceived) {
        for (const auto& [id, info] : PDiskInfo) {
            Info->UpdatePDiskState(id, info);
        }

        for (const auto& [id, info] : VDiskInfo) {
            Info->UpdateVDiskState(id, info);
        }

        ev->Info = Info;
        ev->Info->SetTimestamp(TlsActivationContext->Now());
    }

    Send(Client, std::move(ev));
    PassAway();
}

void TInfoCollector::MaybeReplyAndDie() {
    if (!BaseConfigReceived) {
        return;
    }

    for (const auto& [nodeId, events] : NodeEvents) {
        if (!events.empty()) {
            return;
        }
    }

    ReplyAndDie();
}

void TInfoCollector::PassAway() {
    for (const auto& [nodeId, _] : NodeEvents) {
        Send(TActivationContext::InterconnectProxy(nodeId), new TEvents::TEvUnsubscribe());
    }

    if (BscPipe) {
        NTabletPipe::CloseAndForgetClient(SelfId(), BscPipe);
    }

    TActorBootstrapped::PassAway();
}

void TInfoCollector::Bootstrap() {
    Send(GetNameserviceActorId(), new TEvInterconnect::TEvListNodes());
    Schedule(Timeout, new TEvents::TEvWakeup());
    Become(&TThis::StateWork);
}

void TInfoCollector::Handle(TEvInterconnect::TEvNodesInfo::TPtr& ev) {
    RequestBaseConfig();

    for (const auto& node : ev->Get()->Nodes) {
        Info->AddNode(node, &TlsActivationContext->AsActorContext());
        SendNodeRequests(node.NodeId);
    }
}

void TInfoCollector::RequestBaseConfig() {
    using namespace NTabletPipe;

    const auto domains = AppData()->DomainsInfo->Domains;
    Y_VERIFY(domains.size() <= 1);

    for (const auto& domain : domains) {
        const auto bscId = MakeBSControllerID(domain.second->DefaultStateStorageGroup);
        BscPipe = Register(CreateClient(SelfId(), bscId, TClientConfig(TClientRetryPolicy::WithRetries())));

        auto ev = MakeHolder<TEvBlobStorage::TEvControllerConfigRequest>();
        ev->Record.MutableRequest()->AddCommand()->MutableQueryBaseConfig();
        SendData(SelfId(), BscPipe, ev.Release());
    }
}

void TInfoCollector::Handle(TEvBlobStorage::TEvControllerConfigResponse::TPtr& ev) {
    const auto& record = ev->Get()->Record.GetResponse();
    LOG_D("Got base config"
        << ": record# " << record.ShortDebugString());

    if (!record.GetSuccess() || !record.StatusSize() || !record.GetStatus(0).GetSuccess()) {
        LOG_E("Couldn't get base config");
        ReplyAndDie();
    } else {
        BaseConfigReceived = true;

        for (const auto& pdisk : record.GetStatus(0).GetBaseConfig().GetPDisk()) {
            Info->AddPDisk(pdisk);
        }

        for (const auto& vdisk : record.GetStatus(0).GetBaseConfig().GetVSlot()) {
            Info->AddVDisk(vdisk);
        }

        for (const auto& group : record.GetStatus(0).GetBaseConfig().GetGroup()) {
            Info->AddBSGroup(group);
        }

        Info->ChooseSysNodes();
        MaybeReplyAndDie();
    }
}

void TInfoCollector::Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev) {
    const auto& msg = *ev->Get();
    if (msg.ClientId == BscPipe && msg.Status != NKikimrProto::OK) {
        OnPipeDestroyed();
    }
}

void TInfoCollector::Handle(TEvTabletPipe::TEvClientDestroyed::TPtr& ev) {
    const auto& msg = *ev->Get();
    if (msg.ClientId == BscPipe) {
        OnPipeDestroyed();
    }
}

void TInfoCollector::OnPipeDestroyed() {
    LOG_W("BscPipe destroyed");

    if (BscPipe) {
        NTabletPipe::CloseAndForgetClient(SelfId(), BscPipe);
    }

    if (!BaseConfigReceived) {
        RequestBaseConfig();
    }
}

void TInfoCollector::SendNodeRequests(ui32 nodeId) {
    const TActorId whiteBoardId = MakeNodeWhiteboardServiceId(nodeId);
    SendNodeEvent(nodeId, whiteBoardId, new TEvWhiteboard::TEvSystemStateRequest(), TEvWhiteboard::EvSystemStateResponse);
    SendNodeEvent(nodeId, whiteBoardId, new TEvWhiteboard::TEvTabletStateRequest(), TEvWhiteboard::EvTabletStateResponse);
    SendNodeEvent(nodeId, whiteBoardId, new TEvWhiteboard::TEvPDiskStateRequest(), TEvWhiteboard::EvPDiskStateResponse);
    SendNodeEvent(nodeId, whiteBoardId, new TEvWhiteboard::TEvVDiskStateRequest(), TEvWhiteboard::EvVDiskStateResponse);

    const auto domains = AppData()->DomainsInfo->Domains;
    Y_VERIFY(domains.size() <= 1);

    for (const auto& domain : domains) {
        const TActorId tenantPoolId = MakeTenantPoolID(nodeId, domain.second->DomainUid);
        SendNodeEvent(nodeId, tenantPoolId, new TEvTenantPool::TEvGetStatus(true), TEvTenantPool::EvTenantPoolStatus);
    }
}

void TInfoCollector::SendNodeEvent(ui32 nodeId, const TActorId& recipient, IEventBase* request, ui32 responseType) {
    Send(recipient, request, IEventHandle::FlagTrackDelivery | IEventHandle::FlagSubscribeOnSession, nodeId);
    NodeEvents[nodeId].insert(responseType);
}

bool TInfoCollector::IsNodeInfoRequired(ui32 nodeId, ui32 eventType) const {
    auto it = NodeEvents.find(nodeId);
    if (it == NodeEvents.end()) {
        LOG_W("Got info from unknown node"
            << ": nodeId# " << nodeId);
        return false;
    }

    return it->second.contains(eventType);
}

void TInfoCollector::ResponseProcessed(ui32 nodeId, ui32 eventType) {
    Y_VERIFY_S(NodeEvents.contains(nodeId), "Unexpected node"
        << ": nodeId# " << nodeId);
    Y_VERIFY_S(NodeEvents[nodeId].contains(eventType), "Unexpected event"
        << ": nodeId# " << nodeId
        << ", eventType# " << eventType);

    NodeEvents[nodeId].erase(eventType);
    if (NodeEvents[nodeId].empty()) {
        MaybeReplyAndDie();
    }
}

void TInfoCollector::Handle(TEvWhiteboard::TEvSystemStateResponse::TPtr& ev) {
    const ui32 nodeId = ev->Sender.NodeId();
    const auto& record = ev->Get()->Record;

    LOG_D("Got system state"
        << ": nodeId# " << nodeId
        << ", record# " << record.DebugString());

    if (!IsNodeInfoRequired(nodeId, ev->Type)) {
        return;
    }

    if (record.SystemStateInfoSize() != 1) {
        LOG_E("Unexpected system state's size"
            << ": nodeId# " << nodeId
            << ", size# " << record.SystemStateInfoSize());
        return;
    }

    Info->SetNodeState(nodeId, NKikimrCms::UP, record.GetSystemStateInfo(0));
    ResponseProcessed(nodeId, ev->Type);
}

void TInfoCollector::Handle(TEvWhiteboard::TEvTabletStateResponse::TPtr& ev) {
    const ui32 nodeId = ev->Sender.NodeId();
    const auto& record = ev->Get()->Record;

    LOG_D("Got tablet state"
        << ": nodeId# " << nodeId
        << ", record# " << record.DebugString());

    if (!IsNodeInfoRequired(nodeId, ev->Type)) {
        return;
    }

    for (const auto& info : record.GetTabletStateInfo()) {
        Info->AddTablet(nodeId, info);
    }

    ResponseProcessed(nodeId, ev->Type);
}

void TInfoCollector::Handle(TEvWhiteboard::TEvPDiskStateResponse::TPtr& ev) {
    const ui32 nodeId = ev->Sender.NodeId();
    auto& record = ev->Get()->Record;

    LOG_D("Got PDisk state"
        << ": nodeId# " << nodeId
        << ", record# " << record.DebugString());

    if (!IsNodeInfoRequired(nodeId, ev->Type)) {
        return;
    }

    for (ui32 i = 0; i < record.PDiskStateInfoSize(); ++i) {
        auto* info = record.MutablePDiskStateInfo(i);
        const auto id = TPDiskID(nodeId, info->GetPDiskId());
        PDiskInfo[id].Swap(info);
    }

    ResponseProcessed(nodeId, ev->Type);
}

void TInfoCollector::Handle(TEvWhiteboard::TEvVDiskStateResponse::TPtr& ev) {
    const ui32 nodeId = ev->Sender.NodeId();
    auto& record = ev->Get()->Record;

    LOG_D("Got VDisk state"
        << ": nodeId# " << nodeId
        << ", record# " << record.DebugString());

    if (!IsNodeInfoRequired(nodeId, ev->Type)) {
        return;
    }

    for (ui32 i = 0; i < record.VDiskStateInfoSize(); ++i) {
        auto* info = record.MutableVDiskStateInfo(i);
        const auto id = VDiskIDFromVDiskID(info->GetVDiskId());
        VDiskInfo[id].Swap(info);
    }

    ResponseProcessed(nodeId, ev->Type);
}

void TInfoCollector::Handle(TEvTenantPool::TEvTenantPoolStatus::TPtr& ev) {
    const ui32 nodeId = ev->Sender.NodeId();
    const auto& record = ev->Get()->Record;

    LOG_D("Got TenantPoolStatus"
        << ": nodeId# " << nodeId
        << ", record# " << record.DebugString());

    if (!IsNodeInfoRequired(nodeId, ev->Type)) {
        return;
    }

    Info->AddNodeTenants(nodeId, record);
    ResponseProcessed(nodeId, ev->Type);
}

void TInfoCollector::Handle(TEvents::TEvUndelivered::TPtr& ev) {
    const auto& msg = *ev->Get();
    const ui32 nodeId = ev->Cookie;

    LOG_D("Undelivered"
        << ": nodeId# " << nodeId
        << ", source# " << msg.SourceType
        << ", reason# " << msg.Reason);

    if (!NodeEvents.contains(nodeId)) {
        LOG_E("Undelivered to unknown node"
            << ": nodeId# " << nodeId);
        return;
    }

    if (msg.SourceType == TEvTenantPool::EvGetStatus && msg.Reason == TEvents::TEvUndelivered::ReasonActorUnknown) {
        LOG_W("Node is alive, but TenantPool is not running (KIKIMR-8249)");
        ResponseProcessed(nodeId, TEvTenantPool::EvTenantPoolStatus);
    } else {
        Info->ClearNode(nodeId);
        NodeEvents[nodeId].clear();
    }

    MaybeReplyAndDie();
}

void TInfoCollector::Handle(TEvInterconnect::TEvNodeDisconnected::TPtr& ev) {
    const ui32 nodeId = ev->Get()->NodeId;

    LOG_D("Disconnected"
        << ": nodeId# " << nodeId);

    if (!NodeEvents.contains(nodeId)) {
        LOG_E("Disconnected unknown node"
            << ": nodeId# " << nodeId);
        return;
    }

    Info->ClearNode(nodeId);
    NodeEvents[nodeId].clear();
    MaybeReplyAndDie();
}

IActor* CreateInfoCollector(const TActorId& client, const TDuration& timeout) {
    return new TInfoCollector(client, timeout);
}

} // NCms
} // NKikimr
