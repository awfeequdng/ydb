#include "node_warden_impl.h"

#include <ydb/core/blobstorage/crypto/secured_block.h>
#include <ydb/core/blobstorage/pdisk/drivedata_serializer.h>
#include <ydb/library/pdisk_io/file_params.h>

using namespace NKikimr;
using namespace NStorage;

TVector<NPDisk::TDriveData> TNodeWarden::ListLocalDrives() {
    TVector<NPDisk::TDriveData> drives = ListDevicesWithPartlabel();

    try {
        TString raw = TFileInput(MockDevicesPath).ReadAll();
        if (google::protobuf::TextFormat::ParseFromString(raw, &MockDevicesConfig)) {
            for (const auto& device : MockDevicesConfig.GetDevices()) {
                NPDisk::TDriveData data;
                DriveDataToDriveData(device, data);
                drives.push_back(data);
            }
        } else {
            STLOG(PRI_WARN, BS_NODE, NW01, "Error parsing mock devices protobuf from file", (Path, MockDevicesPath));
        }
    } catch (...) {
        STLOG(PRI_INFO, BS_NODE, NW90, "Unable to find mock devices file", (Path, MockDevicesPath));
    }

    std::sort(drives.begin(), drives.end(), [] (const auto& lhs, const auto& rhs) {
        return lhs.Path < rhs.Path;
    });

    return drives;
}

void TNodeWarden::StartInvalidGroupProxy() {
    const ui32 groupId = Max<ui32>();
    STLOG(PRI_DEBUG, BS_NODE, NW11, "StartInvalidGroupProxy", (GroupId, groupId));
    TlsActivationContext->ExecutorThread.ActorSystem->RegisterLocalService(MakeBlobStorageProxyID(groupId), Register(
        CreateBlobStorageGroupEjectedProxy(groupId, DsProxyNodeMon), TMailboxType::ReadAsFilled, AppData()->SystemPoolId));
}

void TNodeWarden::StopInvalidGroupProxy() {
    ui32 groupId = Max<ui32>();
    STLOG(PRI_DEBUG, BS_NODE, NW15, "StopInvalidGroupProxy", (GroupId, groupId));
    TActivationContext::Send(new IEventHandle(TEvents::TSystem::Poison, 0, MakeBlobStorageProxyID(groupId), {}, nullptr, 0));
}

void TNodeWarden::PassAway() {
    STLOG(PRI_DEBUG, BS_NODE, NW25, "PassAway");
    NTabletPipe::CloseClient(SelfId(), PipeClientId);
    StopInvalidGroupProxy();
    TActivationContext::Send(new IEventHandle(TEvents::TSystem::Poison, 0, DsProxyNodeMonActor, {}, nullptr, 0));
    return TActorBootstrapped::PassAway();
}

void TNodeWarden::Bootstrap() {
    STLOG(PRI_DEBUG, BS_NODE, NW26, "Bootstrap");

    LocalNodeId = SelfId().NodeId();
    WhiteboardId = NNodeWhiteboard::MakeNodeWhiteboardServiceId(LocalNodeId);

    Become(&TThis::StateOnline, TDuration::Seconds(10), new TEvPrivate::TEvSendDiskMetrics());

    const auto& dyn = AppData()->DynamicNameserviceConfig;
    ui32 maxStaticNodeId = dyn ? dyn->MaxStaticNodeId : Max<ui32>();
    bool checkNodeDrives = (LocalNodeId <= maxStaticNodeId);
    if (checkNodeDrives) {
        Schedule(TDuration::Seconds(10), new TEvPrivate::TEvUpdateNodeDrives());
    }

    NLwTraceMonPage::ProbeRegistry().AddProbesList(LWTRACE_GET_PROBES(BLOBSTORAGE_PROVIDER));

    TActorSystem *actorSystem = TlsActivationContext->ExecutorThread.ActorSystem;
    if (auto mon = AppData()->Mon) {

        TString name = "NodeWarden";
        TString path = ::to_lower(name);
        NMonitoring::TIndexMonPage *actorsMonPage = mon->RegisterIndexPage("actors", "Actors");

        mon->RegisterActorPage(actorsMonPage, path, name, false, actorSystem, SelfId());
    }

    DsProxyNodeMon = new TDsProxyNodeMon(AppData()->Counters, true);
    DsProxyNodeMonActor = Register(CreateDsProxyNodeMon(DsProxyNodeMon));
    DsProxyPerPoolCounters = new TDsProxyPerPoolCounters(AppData()->Counters);

    if (actorSystem && actorSystem->AppData<TAppData>() && actorSystem->AppData<TAppData>()->Icb) {
        actorSystem->AppData<TAppData>()->Icb->RegisterLocalControl(EnablePutBatching, "BlobStorage_EnablePutBatching");
        actorSystem->AppData<TAppData>()->Icb->RegisterLocalControl(EnableVPatch, "BlobStorage_EnableVPatch");
    }

    // start replication broker
    const auto& replBrokerConfig = Cfg->ServiceSet.GetReplBrokerConfig();

    ui64 requestBytesPerSecond = 500000000; // 500 MB/s by default
    if (replBrokerConfig.HasTotalRequestBytesPerSecond()) {
        requestBytesPerSecond = replBrokerConfig.GetTotalRequestBytesPerSecond();
    } else if (replBrokerConfig.HasRateBytesPerSecond()) { // compatibility option
        requestBytesPerSecond = replBrokerConfig.GetRateBytesPerSecond();
    }
    ReplNodeRequestQuoter = std::make_shared<TReplQuoter>(requestBytesPerSecond);

    ui64 responseBytesPerSecond = 500000000; // the same as for request
    if (replBrokerConfig.HasTotalResponseBytesPerSecond()) {
        responseBytesPerSecond = replBrokerConfig.GetTotalResponseBytesPerSecond();
    }
    ReplNodeResponseQuoter = std::make_shared<TReplQuoter>(responseBytesPerSecond);

    const ui64 maxBytes = replBrokerConfig.GetMaxInFlightReadBytes();
    actorSystem->RegisterLocalService(MakeBlobStorageReplBrokerID(), Register(CreateReplBrokerActor(maxBytes)));

    // determine if we are running in 'mock' mode
    EnableProxyMock = Cfg->ServiceSet.GetEnableProxyMock();

    // Start a statically configured set
    ApplyServiceSet(Cfg->ServiceSet, true, false, false);
    StartStaticProxies();
    EstablishPipe();

    Send(GetNameserviceActorId(), new TEvInterconnect::TEvGetNode(LocalNodeId));

    if (Cfg->IsCacheEnabled()) {
        TActivationContext::Schedule(TDuration::Seconds(5), new IEventHandle(TEvPrivate::EvReadCache, 0, SelfId(), {}, nullptr, 0));
    }

    StartInvalidGroupProxy();
}

void TNodeWarden::HandleReadCache() {
    if (IgnoreCache) {
        return;
    }
    EnqueueSyncOp([this, cfg = Cfg](const TActorContext&) {
        TString data;
        std::exception_ptr ex;
        try {
            data = cfg->CacheAccessor->Read();
        } catch (...) {
            ex = std::current_exception();
        }

        return [=] {
            NKikimrBlobStorage::TNodeWardenCache proto;
            try {
                if (IgnoreCache) {
                    return;
                }

                if (ex) {
                    std::rethrow_exception(ex);
                } else if (!google::protobuf::TextFormat::ParseFromString(data, &proto)) {
                    throw yexception() << "failed to parse node warden cache protobuf";
                }

                STLOG(PRI_INFO, BS_NODE, NW07, "Bootstrap", (Cache, proto));

                if (!proto.HasInstanceId() && !proto.HasAvailDomain() && !proto.HasServiceSet()) {
                    return;
                }

                Y_VERIFY(proto.HasInstanceId());
                Y_VERIFY(proto.HasAvailDomain() && proto.GetAvailDomain() == AvailDomainId);
                if (!InstanceId) {
                    InstanceId.emplace(proto.GetInstanceId());
                }

                ApplyServiceSet(proto.GetServiceSet(), false, false, false);
            } catch (...) {
                STLOG(PRI_INFO, BS_NODE, NW16, "Bootstrap failed to fetch cache", (Error, CurrentExceptionMessage()));
                // ignore exception
            }
        };
    });
}

void TNodeWarden::Handle(TEvInterconnect::TEvNodeInfo::TPtr ev) {
    if (const auto& node = ev->Get()->Node) {
        Send(WhiteboardId, new NNodeWhiteboard::TEvWhiteboard::TEvSystemStateUpdate(node->Location));
    }
}

void TNodeWarden::Handle(NPDisk::TEvSlayResult::TPtr ev) {
    const NPDisk::TEvSlayResult &msg = *ev->Get();
    const TVSlotId vslotId(LocalNodeId, msg.PDiskId, msg.VSlotId);
    STLOG(PRI_INFO, BS_NODE, NW28, "Handle(NPDisk::TEvSlayResult)", (Msg, msg.ToString()));
    switch (msg.Status) {
        case NKikimrProto::NOTREADY:
            TActivationContext::Schedule(TDuration::Seconds(1), new IEventHandle(MakeBlobStoragePDiskID(LocalNodeId,
                msg.PDiskId), SelfId(), new NPDisk::TEvSlay(msg.VDiskId, msg.SlayOwnerRound, msg.PDiskId, msg.VSlotId)));
            break;

        case NKikimrProto::OK:
        case NKikimrProto::ALREADY: {
            if (const auto vdiskIt = LocalVDisks.find(vslotId); vdiskIt == LocalVDisks.end()) {
                SendVDiskReport(vslotId, msg.VDiskId, NKikimrBlobStorage::TEvControllerNodeReport::DESTROYED);
            } else {
                SendVDiskReport(vslotId, msg.VDiskId, NKikimrBlobStorage::TEvControllerNodeReport::WIPED);

                TVDiskRecord& vdisk = vdiskIt->second;
                Y_VERIFY(vdisk.SlayInFlight);
                vdisk.SlayInFlight = false;
                StartLocalVDiskActor(vdisk, TDuration::Zero()); // restart actor after successful wiping
                SendDiskMetrics(false);
            }
            break;
        }

        case NKikimrProto::CORRUPTED:
        case NKikimrProto::ERROR:
            STLOG(PRI_ERROR, BS_NODE, NW29, "Handle(NPDisk::TEvSlayResult) error", (Msg, msg.ToString()));
            SendVDiskReport(vslotId, msg.VDiskId, NKikimrBlobStorage::TEvControllerNodeReport::OPERATION_ERROR);
            break;

        case NKikimrProto::RACE:
            Y_FAIL("Unexpected# %s", msg.ToString().data());
            break;

        default:
            Y_FAIL("Unexpected status# %s", msg.ToString().data());
            break;
    };
}

void TNodeWarden::Handle(TEvRegisterPDiskLoadActor::TPtr ev) {
    Send(ev.Get()->Sender, new TEvRegisterPDiskLoadActorResult(NextLocalPDiskInitOwnerRound()));
}

void TNodeWarden::Handle(TEvBlobStorage::TEvControllerNodeServiceSetUpdate::TPtr ev) {
    const auto& record = ev->Get()->Record;

    if (record.HasAvailDomain() && record.GetAvailDomain() != AvailDomainId) {
        // AvailDomain may arrive unset
        STLOG_DEBUG_FAIL(BS_NODE, NW02, "unexpected AvailDomain from BS_CONTROLLER", (Msg, record), (AvailDomainId, AvailDomainId));
        return;
    }
    if (record.HasInstanceId()) {
        if (record.GetInstanceId() != InstanceId.value_or(record.GetInstanceId())) {
            STLOG_DEBUG_FAIL(BS_NODE, NW14, "unexpected/unset InstanceId from BS_CONTROLLER", (Msg, record), (InstanceId, InstanceId));
            return;
        }
        InstanceId.emplace(record.GetInstanceId());
    }

    if (record.HasServiceSet()) {
        const bool comprehensive = record.GetComprehensive();
        IgnoreCache |= comprehensive;
        STLOG(PRI_DEBUG, BS_NODE, NW17, "Handle(TEvBlobStorage::TEvControllerNodeServiceSetUpdate)", (Msg, record));
        ApplyServiceSet(record.GetServiceSet(), false, comprehensive, true);
    }

    for (const auto& item : record.GetGroupMetadata()) {
        const ui32 groupId = item.GetGroupId();
        const ui32 generation = item.GetCurrentGeneration();
        if (const auto it = Groups.find(groupId); it != Groups.end() && it->second.MaxKnownGeneration < generation) {
            ApplyGroupInfo(groupId, generation, nullptr, false, false);
        }
    }
}

void TNodeWarden::SendDropDonorQuery(ui32 nodeId, ui32 pdiskId, ui32 vslotId, const TVDiskID& vdiskId) {
    STLOG(PRI_NOTICE, BS_NODE, NW87, "SendDropDonorQuery", (NodeId, nodeId), (PDiskId, pdiskId), (VSlotId, vslotId),
        (VDiskId, vdiskId));
    auto ev = std::make_unique<TEvBlobStorage::TEvControllerConfigRequest>();
    auto& record = ev->Record;
    auto *request = record.MutableRequest();
    auto *cmd = request->AddCommand()->MutableDropDonorDisk();
    auto *p = cmd->MutableVSlotId();
    p->SetNodeId(nodeId);
    p->SetPDiskId(pdiskId);
    p->SetVSlotId(vslotId);
    VDiskIDFromVDiskID(vdiskId, cmd->MutableVDiskId());
    SendToController(std::move(ev));
}

void TNodeWarden::SendVDiskReport(TVSlotId vslotId, const TVDiskID &vDiskId, NKikimrBlobStorage::TEvControllerNodeReport::EVDiskPhase phase) {
    STLOG(PRI_DEBUG, BS_NODE, NW32, "SendVDiskReport", (VSlotId, vslotId), (Phase, phase));

    auto report = std::make_unique<TEvBlobStorage::TEvControllerNodeReport>(vslotId.NodeId);
    auto *vReport = report->Record.AddVDiskReports();
    auto *id = vReport->MutableVSlotId();
    id->SetNodeId(vslotId.NodeId);
    id->SetPDiskId(vslotId.PDiskId);
    id->SetVSlotId(vslotId.VDiskSlotId);
    VDiskIDFromVDiskID(vDiskId, vReport->MutableVDiskId());
    vReport->SetPhase(phase);
    SendToController(std::move(report));
}

void TNodeWarden::Handle(TEvBlobStorage::TEvAskRestartPDisk::TPtr ev) {
    const auto id = ev->Get()->PDiskId;
    if (auto it = LocalPDisks.find(TPDiskKey{LocalNodeId, id}); it != LocalPDisks.end()) {
        RestartLocalPDiskStart(id, CreatePDiskConfig(it->second.Record));
    }
}

void TNodeWarden::Handle(TEvBlobStorage::TEvRestartPDiskResult::TPtr ev) {
    RestartLocalPDiskFinish(ev->Get()->PDiskId, ev->Get()->Status);
}

void TNodeWarden::Handle(TEvBlobStorage::TEvControllerUpdateDiskStatus::TPtr ev) {
    STLOG(PRI_TRACE, BS_NODE, NW38, "Handle(TEvBlobStorage::TEvControllerUpdateDiskStatus)");

    auto differs = [](const auto& updated, const auto& current) {
        TString xUpdated, xCurrent;
        bool success = updated.SerializeToString(&xUpdated);
        Y_VERIFY(success);
        success = current.SerializeToString(&xCurrent);
        Y_VERIFY(success);
        return xUpdated != xCurrent;
    };

    auto& record = ev->Get()->Record;

    for (const NKikimrBlobStorage::TVDiskMetrics& m : record.GetVDisksMetrics()) {
        Y_VERIFY(m.HasVSlotId());
        const TVSlotId vslotId(m.GetVSlotId());
        if (const auto it = LocalVDisks.find(vslotId); it != LocalVDisks.end()) {
            TVDiskRecord& vdisk = it->second;
            if (vdisk.VDiskMetrics) {
                auto& current = *vdisk.VDiskMetrics;
                NKikimrBlobStorage::TVDiskMetrics updated(current);
                updated.MergeFrom(m);
                if (differs(updated, current)) {
                    current.Swap(&updated);
                    VDisksWithUnreportedMetrics.PushBack(&vdisk);
                }
            } else {
                vdisk.VDiskMetrics.emplace(m);
                VDisksWithUnreportedMetrics.PushBack(&vdisk);
            }
        }
    }

    for (const NKikimrBlobStorage::TPDiskMetrics& m : record.GetPDisksMetrics()) {
        Y_VERIFY(m.HasPDiskId());
        if (const auto it = LocalPDisks.find({LocalNodeId, m.GetPDiskId()}); it != LocalPDisks.end()) {
            TPDiskRecord& pdisk = it->second;
            if (pdisk.PDiskMetrics) {
                auto& current = *pdisk.PDiskMetrics;
                if (differs(m, current)) {
                    current.CopyFrom(m);
                    PDisksWithUnreportedMetrics.PushBack(&pdisk);
                }
            } else {
                pdisk.PDiskMetrics.emplace(m);
                PDisksWithUnreportedMetrics.PushBack(&pdisk);
            }
        }
    }
}

void TNodeWarden::Handle(TEvPrivate::TEvSendDiskMetrics::TPtr&) {
    STLOG(PRI_TRACE, BS_NODE, NW39, "Handle(TEvPrivate::TEvSendDiskMetrics)");
    SendDiskMetrics(true);
    ReportLatencies();
    Schedule(TDuration::Seconds(10), new TEvPrivate::TEvSendDiskMetrics());
}

void TNodeWarden::Handle(TEvPrivate::TEvUpdateNodeDrives::TPtr&) {
    STLOG(PRI_TRACE, BS_NODE, NW88, "Handle(TEvPrivate::UpdateNodeDrives)");
    EnqueueSyncOp([this] (const TActorContext&) {
        auto drives = ListLocalDrives();

        return [this, drives = std::move(drives)] () {
            if (drives != WorkingLocalDrives) {
                SendToController(std::make_unique<TEvBlobStorage::TEvControllerUpdateNodeDrives>(LocalNodeId, drives));
                WorkingLocalDrives = std::move(drives);
            }
        };
    });
    Schedule(TDuration::Seconds(10), new TEvPrivate::TEvUpdateNodeDrives());
}


void TNodeWarden::SendDiskMetrics(bool reportMetrics) {
    STLOG(PRI_TRACE, BS_NODE, NW45, "SendDiskMetrics", (ReportMetrics, reportMetrics));

    auto ev = std::make_unique<TEvBlobStorage::TEvControllerUpdateDiskStatus>();
    auto& record = ev->Record;

    if (reportMetrics) {
        for (auto& vdisk : std::exchange(VDisksWithUnreportedMetrics, {})) {
            Y_VERIFY(vdisk.VDiskMetrics);
            record.AddVDisksMetrics()->CopyFrom(*vdisk.VDiskMetrics);
        }
        for (auto& pdisk : std::exchange(PDisksWithUnreportedMetrics, {})) {
            Y_VERIFY(pdisk.PDiskMetrics);
            record.AddPDisksMetrics()->CopyFrom(*pdisk.PDiskMetrics);
        }
    }

    FillInVDiskStatus(record.MutableVDiskStatus(), false);

    if (record.VDisksMetricsSize() || record.PDisksMetricsSize() || record.VDiskStatusSize()) { // anything to report?
        SendToController(std::move(ev));
    }
}

void TNodeWarden::Handle(TEvStatusUpdate::TPtr ev) {
    STLOG(PRI_DEBUG, BS_NODE, NW47, "Handle(TEvStatusUpdate)");
    auto *msg = ev->Get();
    const TVSlotId vslotId(msg->NodeId, msg->PDiskId, msg->VSlotId);
    if (const auto it = LocalVDisks.find(vslotId); it != LocalVDisks.end() && it->second.Status != msg->Status) {
        it->second.Status = msg->Status;
        SendDiskMetrics(false);
    }
}

void TNodeWarden::FillInVDiskStatus(google::protobuf::RepeatedPtrField<NKikimrBlobStorage::TVDiskStatus> *pb, bool initial) {
    for (auto& [vslotId, vdisk] : LocalVDisks) {
        const NKikimrBlobStorage::EVDiskStatus status = vdisk.RuntimeData
            ? vdisk.Status
            : NKikimrBlobStorage::EVDiskStatus::ERROR;
        if (initial || status != vdisk.ReportedVDiskStatus) {
            auto *item = pb->Add();
            VDiskIDFromVDiskID(vdisk.GetVDiskId(), item->MutableVDiskId());
            item->SetNodeId(vslotId.NodeId);
            item->SetPDiskId(vslotId.PDiskId);
            item->SetVSlotId(vslotId.VDiskSlotId);
            item->SetPDiskGuid(vdisk.Config.GetVDiskLocation().GetPDiskGuid());
            item->SetStatus(status);
            vdisk.ReportedVDiskStatus = status;
        }
    }
}

bool ObtainKey(TEncryptionKey *key, const NKikimrProto::TKeyRecord& record) {
    TString containerPath = record.GetContainerPath();
    TString pin = record.GetPin();
    TString keyId = record.GetId();
    ui64 version = record.GetVersion();

    TFileHandle containerFile(containerPath, OpenExisting | RdOnly);
    if (!containerFile.IsOpen()) {
        Cerr << "Can't open key container file# \"" << EscapeC(containerPath) << "\", make sure the file actually exists." << Endl;
        return false;
    }
    ui64 length = containerFile.GetLength();
    if (length == 0) {
        Cerr << "Key container file# \"" << EscapeC(containerPath) << "\" size is 0, make sure the file actually contains the key!" << Endl;
        return false;
    }
    TString data = TString::Uninitialized(length);
    size_t bytesRead = containerFile.Read(data.Detach(), length);
    if (bytesRead != length) {
        Cerr << "Key container file# \"" << EscapeC(containerPath) << "\" could not be read! Expected length# " << length
            << " bytesRead# " << bytesRead << ", make sure the file stays put!" << Endl;
        return false;
    }
    THashCalculator hasher;
    if (pin.size() == 0) {
        pin = "EmptyPin";
    }

    ui8 *keyBytes = 0;
    ui32 keySize = 0;
    key->Key.MutableKeyBytes(&keyBytes, &keySize);
    Y_VERIFY(keySize == 4 * sizeof(ui64));
    ui64 *p = (ui64*)keyBytes;

    hasher.SetKey((const ui8*)pin.data(), pin.size());
    hasher.Hash(data.Detach(), data.size());
    p[0] = hasher.GetHashResult(&p[1]);
    hasher.Clear();
    hasher.SetKey((const ui8*)pin.data(), pin.size());
    TString saltBefore = "SaltBefore";
    TString saltAfter = "SaltAfter";
    hasher.Hash(saltBefore.data(), saltBefore.size());
    hasher.Hash(data.Detach(), data.size());
    hasher.Hash(saltAfter.data(), saltAfter.size());
    p[2] = hasher.GetHashResult(&p[3]);

    key->Version = version;
    key->Id = keyId;

    SecureWipeBuffer((ui8*)data.Detach(), data.size());

    return true;
}

bool NKikimr::ObtainTenantKey(TEncryptionKey *key, const NKikimrProto::TKeyConfig& keyConfig) {
    if (keyConfig.KeysSize()) {
        // TODO(cthulhu): process muliple keys here.
        auto &record = keyConfig.GetKeys(0);
        return ObtainKey(key, record);
    } else {
        Cerr << "No Keys in KeyConfig! Encrypted group DsProxies will not start" << Endl;
        return false;
    }
}

bool NKikimr::ObtainPDiskKey(TVector<TEncryptionKey> *keys, const NKikimrProto::TKeyConfig& keyConfig) {
    ui32 keysSize = keyConfig.KeysSize();
    if (!keysSize) {
        Cerr << "No Keys in PDiskKeyConfig! Encrypted pdisks will not start" << Endl;
        return false;
    }

    keys->resize(keysSize);
    for (ui32 i = 0; i < keysSize; ++i) {
        auto &record = keyConfig.GetKeys(i);
        if (record.GetId() == "0" && record.GetContainerPath() == "") {
            // use default pdisk key
            (*keys)[i].Id = "0";
            (*keys)[i].Version = record.GetVersion();

            ui8 *keyBytes = 0;
            ui32 keySize = 0;
            (*keys)[i].Key.MutableKeyBytes(&keyBytes, &keySize);

            ui64* p = (ui64*)keyBytes;
            p[0] = NPDisk::YdbDefaultPDiskSequence;
        } else {
            if (!ObtainKey(&(*keys)[i], record)) {
                return false;
            }
        } 
    }

    std::sort(keys->begin(), keys->end(), [&](const TEncryptionKey& l, const TEncryptionKey& r) {
        return l.Version < r.Version;
    });
    return true;
}


bool NKikimr::ObtainStaticKey(TEncryptionKey *key) {
    // TODO(cthulhu): Replace this with real data
    key->Key.SetKey((ui8*)"TestStaticKey", 13);
    key->Version = 1;
    key->Id = "TestStaticKeyId";
    return true;
}

IActor* NKikimr::CreateBSNodeWarden(const TIntrusivePtr<TNodeWardenConfig> &cfg) {
    return new NStorage::TNodeWarden(cfg);
}
