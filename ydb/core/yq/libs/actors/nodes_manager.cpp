#include "nodes_manager.h"
#include <ydb/core/yq/libs/config/protos/yq_config.pb.h>

#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <ydb/library/yql/providers/dq/worker_manager/interface/events.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>
#include <ydb/public/sdk/cpp/client/ydb_driver/driver.h>
#include <ydb/public/sdk/cpp/client/ydb_value/value.h>
#include <ydb/core/yq/libs/common/entity_id.h>
#include <ydb/core/yq/libs/private_client/internal_service.h>
#include <library/cpp/actors/core/log.h>
#include <util/system/hostname.h>
#include <ydb/core/protos/services.pb.h>


#define LOG_E(stream) \
    LOG_ERROR_S(*TlsActivationContext, NKikimrServices::YQL_NODES_MANAGER, stream)
#define LOG_I(stream) \
    LOG_INFO_S(*TlsActivationContext, NKikimrServices::YQL_NODES_MANAGER, stream)
#define LOG_D(stream) \
    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::YQL_NODES_MANAGER, stream)
#define LOG_T(stream) \
    LOG_TRACE_S(*TlsActivationContext, NKikimrServices::YQL_NODES_MANAGER, stream)

namespace NYq {

using namespace NActors;
using namespace NYql;
using namespace NDqs;

class TNodesManagerActor : public NActors::TActorBootstrapped<TNodesManagerActor> {
public:
    enum EWakeUp {
        WU_NodesHealthCheck
    };

    TNodesManagerActor(
        const NYq::TYqSharedResources::TPtr& yqSharedResources,
        const NDqs::TWorkerManagerCounters& workerManagerCounters,
        TIntrusivePtr<ITimeProvider> timeProvider,
        TIntrusivePtr<IRandomProvider> randomProvider,
        const ::NYql::NCommon::TServiceCounters& serviceCounters,
        const NConfig::TPrivateApiConfig& privateApiConfig,
        const ui32& icPort,
        bool useDataCenter,
        const TString& dataCenter,
        const TString& tenant,
        ui64 mkqlInitialMemoryLimit)
        : WorkerManagerCounters(workerManagerCounters)
        , TimeProvider(timeProvider)
        , RandomProvider(randomProvider)
        , ServiceCounters(serviceCounters, "node_manager")
        , PrivateApiConfig(privateApiConfig)
        , Tenant(tenant)
        , MkqlInitialMemoryLimit(mkqlInitialMemoryLimit)
        , YqSharedResources(yqSharedResources)
        , IcPort(icPort)
        , UseDataCenter(useDataCenter)
        , DataCenter(dataCenter)
        , InternalServiceId(NFq::MakeInternalServiceActorId())

    {
        InstanceId = GetGuidAsString(RandomProvider->GenUuid4());
    }

    static constexpr char ActorName[] = "YQ_NODES_MANAGER";

    void PassAway() final {
        LOG_I("PassAway, InstanceId: " << InstanceId);
        NActors::IActor::PassAway();
    }

    void Bootstrap(const TActorContext&) {
        Become(&TNodesManagerActor::StateFunc);
        ServiceCounters.Counters->GetCounter("EvBootstrap", true)->Inc();
        LOG_I("Bootstrap, InstanceId: " << InstanceId);
        NodesHealthCheck();
    }

private:
    void Handle(NDqs::TEvAllocateWorkersRequest::TPtr& ev) {
        ServiceCounters.Counters->GetCounter("EvAllocateWorkersRequest", true)->Inc();
        const auto &rec = ev->Get()->Record;
        const auto count = rec.GetCount();

        auto req = MakeHolder<NDqs::TEvAllocateWorkersResponse>();

        if (count == 0) {
            auto& error = *req->Record.MutableError();
            error.SetStatusCode(NYql::NDqProto::StatusIds::BAD_REQUEST);
            error.SetMessage("Incorrect request - 0 nodes requested");
        } else {
            auto resourceId = rec.GetResourceId();
            if (!resourceId) {
                resourceId = (ui64(++ResourceIdPart) << 32) | SelfId().NodeId();
            }

            bool placementFailure = false;
            ui64 memoryLimit = AtomicGet(WorkerManagerCounters.MkqlMemoryLimit->GetAtomic());
            ui64 memoryAllocated = AtomicGet(WorkerManagerCounters.MkqlMemoryAllocated->GetAtomic());
            TVector<TPeer> nodes;
            for (ui32 i = 0; i < count; ++i) {
                TPeer node = {SelfId().NodeId(), InstanceId + "," + HostName(), 0, 0, 0, DataCenter};
                bool selfPlacement = true;
                if (!Peers.empty()) {
                    auto FirstPeer = NextPeer;
                    while (true) {
                        Y_VERIFY(NextPeer < Peers.size());
                        auto& nextNode = Peers[NextPeer];

                        if (++NextPeer >= Peers.size()) {
                            NextPeer = 0;
                        }

                        if (    (!UseDataCenter || DataCenter.empty() || nextNode.DataCenter.empty() || DataCenter == nextNode.DataCenter) // non empty DC must match
                             && (   nextNode.MemoryLimit == 0 // memory is NOT limited
                                 || nextNode.MemoryLimit >= nextNode.MemoryAllocated + MkqlInitialMemoryLimit) // or enough
                        ) {
                            // adjust allocated size to place next tasks correctly, will be reset after next health check
                            nextNode.MemoryAllocated += MkqlInitialMemoryLimit;
                            if (nextNode.NodeId == SelfId().NodeId()) {
                                // eventually synced self allocation info
                                memoryAllocated += MkqlInitialMemoryLimit;
                            }
                            node = nextNode;
                            selfPlacement = false;
                            break;
                        }

                        if (NextPeer == FirstPeer) {  // we closed loop w/o success, fallback to self placement then
                            break;
                        }
                    }
                }
                if (selfPlacement) {
                    if (memoryLimit == 0 || memoryLimit >= memoryAllocated + MkqlInitialMemoryLimit) {
                        memoryAllocated += MkqlInitialMemoryLimit;
                    } else {
                        placementFailure = true;
                        auto& error = *req->Record.MutableError();
                        error.SetStatusCode(NYql::NDqProto::StatusIds::CLUSTER_OVERLOADED);
                        error.SetMessage("Not enough free memory in the cluster");
                        break;
                    }
                }
                nodes.push_back(node);
            }

            if (!placementFailure) {
                req->Record.ClearError();
                auto& group = *req->Record.MutableNodes();
                group.SetResourceId(resourceId);
                for (const auto& node : nodes) {
                    auto* worker = group.AddWorker();
                    *worker->MutableGuid() = node.InstanceId;
                    worker->SetNodeId(node.NodeId);
                }
            }
        }
        LOG_D("TEvAllocateWorkersResponse " << req->Record.DebugString());

        Send(ev->Sender, req.Release());
    }

    void Handle(NDqs::TEvFreeWorkersNotify::TPtr&) {
        ServiceCounters.Counters->GetCounter("EvFreeWorkersNotify", true)->Inc();
    }

    STRICT_STFUNC(
        StateFunc,

        hFunc(NActors::TEvents::TEvWakeup, HandleWakeup)
        hFunc(NDqs::TEvAllocateWorkersRequest, Handle)
        hFunc(NDqs::TEvFreeWorkersNotify, Handle)
        hFunc(NActors::TEvents::TEvUndelivered, OnUndelivered)
        hFunc(NFq::TEvInternalService::TEvHealthCheckResponse, HandleResponse)
        )

    void HandleWakeup(NActors::TEvents::TEvWakeup::TPtr& ev) {
        ServiceCounters.Counters->GetCounter("EvWakeup", true)->Inc();
        auto tag = ev->Get()->Tag;
        switch (tag) {
        case WU_NodesHealthCheck:
            NodesHealthCheck();
            break;
        }
    }

    void NodesHealthCheck() {
        const TDuration ttl = TDuration::Seconds(5);
        Schedule(ttl, new NActors::TEvents::TEvWakeup(WU_NodesHealthCheck));

        ServiceCounters.Counters->GetCounter("NodesHealthCheck", true)->Inc();

        Fq::Private::NodesHealthCheckRequest request;
        request.set_tenant(Tenant);
        auto& node = *request.mutable_node();
        node.set_node_id(SelfId().NodeId());
        node.set_instance_id(InstanceId);
        node.set_hostname(HostName());
        node.set_active_workers(AtomicGet(WorkerManagerCounters.ActiveWorkers->GetAtomic()));
        node.set_memory_limit(AtomicGet(WorkerManagerCounters.MkqlMemoryLimit->GetAtomic()));
        node.set_memory_allocated(AtomicGet(WorkerManagerCounters.MkqlMemoryAllocated->GetAtomic()));
        node.set_interconnect_port(IcPort);
        node.set_data_center(DataCenter);
        Send(InternalServiceId, new NFq::TEvInternalService::TEvHealthCheckRequest(request));
    }

    void OnUndelivered(NActors::TEvents::TEvUndelivered::TPtr&) {
        LOG_E("TNodesManagerActor::OnUndelivered");
        ServiceCounters.Counters->GetCounter("OnUndelivered", true)->Inc();
    }

    void HandleResponse(NFq::TEvInternalService::TEvHealthCheckResponse::TPtr& ev) {
        try {
            const auto& status = ev->Get()->Status.GetStatus();
            THolder<TEvInterconnect::TEvNodesInfo> nameServiceUpdateReq(new TEvInterconnect::TEvNodesInfo());
            if (!ev->Get()->Status.IsSuccess()) {
                ythrow yexception() <<  status << '\n' << ev->Get()->Status.GetIssues().ToString();
            }
            const auto& res = ev->Get()->Result;

            auto& nodesInfo = nameServiceUpdateReq->Nodes;
            nodesInfo.reserve(res.nodes().size());

            Peers.clear();
            std::set<ui32> nodeIds; // may be not unique
            for (const auto& node : res.nodes()) {

                if (nodeIds.contains(node.node_id())) {
                    continue;
                }
                nodeIds.insert(node.node_id());

                Peers.push_back({node.node_id(), node.instance_id() + "," + node.hostname(),
                  node.active_workers(), node.memory_limit(), node.memory_allocated(), node.data_center()});

                if (node.interconnect_port()) {
                    nodesInfo.emplace_back(TEvInterconnect::TNodeInfo{
                        node.node_id(),
                        node.node_address(),
                        node.hostname(), // host
                        node.hostname(), // resolveHost
                        static_cast<ui16>(node.interconnect_port()),
                        TNodeLocation(node.data_center())});
                }
            }
            if (NextPeer >= Peers.size()) {
                NextPeer = 0;
            }

            ServiceCounters.Counters->GetCounter("PeerCount", false)->Set(Peers.size());
            ServiceCounters.Counters->GetCounter("NodesHealthCheckOk", true)->Inc();

            LOG_T("Send NodeInfo with size: " << nodesInfo.size() << " to DynamicNameserver");
            if (!nodesInfo.empty()) {
                Send(GetNameserviceActorId(), nameServiceUpdateReq.Release());
            }
        } catch (yexception &e) {
            LOG_E(e.what());
            ServiceCounters.Counters->GetCounter("NodesHealthCheckFail", true)->Inc();
        }
    }

private:
    NDqs::TWorkerManagerCounters WorkerManagerCounters;
    TIntrusivePtr<ITimeProvider> TimeProvider;
    TIntrusivePtr<IRandomProvider> RandomProvider;
    ::NYql::NCommon::TServiceCounters ServiceCounters;
    NConfig::TPrivateApiConfig PrivateApiConfig;
    TString Tenant;
    ui64 MkqlInitialMemoryLimit;

    NYq::TYqSharedResources::TPtr YqSharedResources;

    const ui32 IcPort; // Interconnect Port
    bool UseDataCenter;
    TString DataCenter;

    struct TPeer {
        ui32 NodeId;
        TString InstanceId;
        ui64 ActiveWorkers;
        ui64 MemoryLimit;
        ui64 MemoryAllocated;
        TString DataCenter;
    };
    TVector<TPeer> Peers;
    ui32 ResourceIdPart = 0;
    ui32 NextPeer = 0;
    TString InstanceId;
    TActorId InternalServiceId;
};

TActorId MakeNodesManagerId() {
    constexpr TStringBuf name = "FQNODEMAN";
    return NActors::TActorId(0, name);
}

IActor* CreateNodesManager(
    const NDqs::TWorkerManagerCounters& workerManagerCounters,
    TIntrusivePtr<ITimeProvider> timeProvider,
    TIntrusivePtr<IRandomProvider> randomProvider,
    const ::NYql::NCommon::TServiceCounters& serviceCounters,
    const NConfig::TPrivateApiConfig& privateApiConfig,
    const NYq::TYqSharedResources::TPtr& yqSharedResources,
    const ui32& icPort,
    const TString& dataCenter,
    bool useDataCenter,
    const TString& tenant,
    ui64 mkqlInitialMemoryLimit) {
    return new TNodesManagerActor(yqSharedResources, workerManagerCounters,
        timeProvider, randomProvider,
        serviceCounters, privateApiConfig, icPort, useDataCenter, dataCenter, tenant, mkqlInitialMemoryLimit);
}

} // namespace NYq
