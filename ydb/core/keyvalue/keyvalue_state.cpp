#include "keyvalue_state.h"
#include "keyvalue_data.h"
#include "keyvalue_storage_read_request.h"
#include "keyvalue_storage_request.h"
#include "keyvalue_trash_key_arbitrary.h"
#include <ydb/core/base/tablet.h>
#include <ydb/core/protos/counters_keyvalue.pb.h>
#include <ydb/core/protos/msgbus_kv.pb.h>
#include <ydb/core/protos/tablet.pb.h>
#include <ydb/core/mind/local.h>
#include <ydb/core/tablet/tablet_counters_protobuf.h>
#include <ydb/core/tablet/tablet_metrics.h>
#include <ydb/core/util/stlog.h>
#include <library/cpp/monlib/service/pages/templates.h>
#include <library/cpp/json/writer/json_value.h>
#include <util/string/escape.h>
#include <util/charset/utf8.h>

// Set to 1 in order for tablet to reboot instead of failing a Y_VERIFY on database damage
#define KIKIMR_KEYVALUE_ALLOW_DAMAGE 0

namespace NKikimr {
namespace NKeyValue {

constexpr ui64 KeyValuePairSizeEstimation = 1 + 5 // Key id, length
    + 1 + 5 // Value id, length
    + 1 + 4 // ValueSize id, value
    + 1 + 8 // CreationUnixTime id, value
    + 1 + 1 // StorageChannel id, value
    + 1 + 1 // Status id, value
    ;

constexpr ui64 KeyValuePairSizeEstimationNewApi = 1 + 5 // Key id, length
    + 1 + 5 // Value id, length
    + 1 + 4 // ValueSize id, value
    + 1 + 8 // CreationUnixTime id, value
    + 1 + 4 // StorageChannel id, value
    + 1 + 1 // Status id, value
    ;

constexpr ui64 KeyInfoSizeEstimation = 1 + 5 // Key id, length
    + 1 + 4 // ValueSize id, value
    + 1 + 8 // CreationUnixTime id, value
    + 1 + 4 // StorageChannel id, value
    ;

constexpr ui64 ReadRangeRequestMetaDataSizeEstimation = 1 + 5 // pair id, length
    + 1 + 1 // Status id, value
    ;

constexpr ui64 ReadResultSizeEstimation = 1 + 1 // Status id, value
    + 1 + 5 // Value id, length OR Message id, length
    ;

constexpr ui64 ReadResultSizeEstimationNewApi = 1 + 5 // Key id, length
    + 1 + 5 // Value id, length
    + 1 + 8 // Offset id, value
    + 1 + 8 // Size id, value
    + 1 + 1 // Status id, value
    ;

constexpr ui64 ErrorMessageSizeEstimation = 128;

// Guideline:
// Check SetError calls: there must be no changes made to the DB before SetError call (!)

TKeyValueState::TKeyValueState() {
    TabletCounters = nullptr;
    Clear();
}

void TKeyValueState::Clear() {
    IsStatePresent = false;
    IsEmptyDbStart = true;
    IsDamaged = false;
    IsTabletYellowStop = false;
    IsTabletYellowMove = false;

    StoredState.Clear();
    NextLogoBlobStep = 1;
    NextLogoBlobCookie = 1;
    Index.clear();
    RefCounts.clear();
    Trash.clear();
    InFlightForStep.clear();
    CollectOperation.Reset(nullptr);
    IsCollectEventSent = false;
    IsSpringCleanupDone = false;
    ChannelDataUsage.fill(0);
    UsedChannels.reset();

    TabletId = 0;
    KeyValueActorId = TActorId();
    ExecutorGeneration = 0;

    Queue.clear();
    IntermediatesInFlight = 0;
    IntermediatesInFlightLimit = 3; // FIXME: Change to something like 10
    RoInlineIntermediatesInFlight = 0;
    DeletesPerRequestLimit = 100'000;

    PerGenerationCounter = 0;

    if (TabletCounters) {
        ClearTabletCounters();
        CountStarting();
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Tablet Counters
//
void TKeyValueState::SetupTabletCounters(TAutoPtr<TTabletCountersBase> counters) {
    TabletCountersPtr = counters;
    TabletCounters = TabletCountersPtr.Get();
}

void TKeyValueState::ClearTabletCounters() {
    for (ui32 i = 0; i < TabletCounters->Simple().Size(); ++i) {
       TabletCounters->Simple()[i].Set(0);
    }
}

TAutoPtr<TTabletCountersBase> TKeyValueState::TakeTabletCounters() {
    return TabletCountersPtr;
}

TTabletCountersBase& TKeyValueState::GetTabletCounters() {
    return *TabletCounters;
}

void TKeyValueState::SetupResourceMetrics(NMetrics::TResourceMetrics* resourceMetrics) {
    ResourceMetrics = resourceMetrics;
}

void TKeyValueState::CountRequestComplete(NMsgBusProxy::EResponseStatus status,
        const TRequestStat &stat, const TActorContext &ctx)
{
    ui64 fullLatencyMs = (TAppData::TimeProvider->Now() - stat.IntermediateCreatedAt).MilliSeconds();
    if (stat.RequestType == TRequestType::WriteOnly) {
        TabletCounters->Percentile()[COUNTER_LATENCY_FULL_WO].IncrementFor(fullLatencyMs);
        TabletCounters->Simple()[COUNTER_REQ_WO_IN_FLY].Add((ui64)-1);
        if (status == NMsgBusProxy::MSTATUS_OK) {
            TabletCounters->Cumulative()[COUNTER_REQ_WO_OK].Increment(1);
        } else if (status == NMsgBusProxy::MSTATUS_TIMEOUT) {
            TabletCounters->Cumulative()[COUNTER_REQ_WO_TIMEOUT].Increment(1);
        } else {
            TabletCounters->Cumulative()[COUNTER_REQ_WO_OTHER_ERROR].Increment(1);
        }
    } else {
        if (stat.RequestType == TRequestType::ReadOnlyInline) {
            TabletCounters->Simple()[COUNTER_REQ_RO_INLINE_IN_FLY].Set(RoInlineIntermediatesInFlight);
        } else {
            TabletCounters->Simple()[COUNTER_REQ_RO_RW_IN_FLY].Set(IntermediatesInFlight);
        }
        if (stat.RequestType == TRequestType::ReadOnlyInline || stat.RequestType == TRequestType::ReadOnly) {
            if (stat.RequestType == TRequestType::ReadOnlyInline) {
                TabletCounters->Percentile()[COUNTER_LATENCY_FULL_RO_INLINE].IncrementFor(fullLatencyMs);
            } else {
                TabletCounters->Percentile()[COUNTER_LATENCY_FULL_RO].IncrementFor(fullLatencyMs);
            }
            if (status == NMsgBusProxy::MSTATUS_OK) {
                TabletCounters->Cumulative()[COUNTER_REQ_RO_OK].Increment(1);
            } else if (status == NMsgBusProxy::MSTATUS_TIMEOUT) {
                TabletCounters->Cumulative()[COUNTER_REQ_RO_TIMEOUT].Increment(1);
            } else {
                TabletCounters->Cumulative()[COUNTER_REQ_RO_OTHER_ERROR].Increment(1);
            }
        } else {
            TabletCounters->Percentile()[COUNTER_LATENCY_FULL_RW].IncrementFor(fullLatencyMs);
            if (status == NMsgBusProxy::MSTATUS_OK) {
                TabletCounters->Cumulative()[COUNTER_REQ_RW_OK].Increment(1);
            } else if (status == NMsgBusProxy::MSTATUS_TIMEOUT) {
                TabletCounters->Cumulative()[COUNTER_REQ_RW_TIMEOUT].Increment(1);
            } else {
                TabletCounters->Cumulative()[COUNTER_REQ_RW_OTHER_ERROR].Increment(1);
            }
        }
    }

    TInstant now(ctx.Now());
    ResourceMetrics->Network.Increment(stat.ReadBytes + stat.RangeReadBytes + stat.WriteBytes, now);

    constexpr ui64 MaxStatChannels = 16;
    for (const auto& pr : stat.GroupReadBytes) {
        ResourceMetrics->ReadThroughput[pr.first].Increment(pr.second, now);
        NMetrics::TChannel channel = pr.first.first;
        if (channel >= MaxStatChannels) {
            channel = MaxStatChannels - 1;
        }
        TabletCounters->Cumulative()[COUNTER_READ_BYTES_CHANNEL_0 + channel].Increment(pr.second);
    }
    for (const auto& pr : stat.GroupWrittenBytes) {
        ResourceMetrics->WriteThroughput[pr.first].Increment(pr.second, now);
        NMetrics::TChannel channel = pr.first.first;
        if (channel >= MaxStatChannels) {
            channel = MaxStatChannels - 1;
        }
        TabletCounters->Cumulative()[COUNTER_WRITE_BYTES_CHANNEL_0 + channel].Increment(pr.second);
    }
    for (const auto& pr : stat.GroupReadIops) {
        ResourceMetrics->ReadIops[pr.first].Increment(pr.second, now);
    }
    for (const auto& pr : stat.GroupWrittenIops) {
        ResourceMetrics->WriteIops[pr.first].Increment(pr.second, now);
    }

    if (status == NMsgBusProxy::MSTATUS_OK) {
        TabletCounters->Cumulative()[COUNTER_CMD_READ_BYTES_OK].Increment(stat.ReadBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_READ_OK].Increment(stat.Reads);
        TabletCounters->Cumulative()[COUNTER_CMD_READ_NODATA].Increment(stat.ReadNodata);
        TabletCounters->Cumulative()[COUNTER_CMD_DATA_RANGE_READ_BYTES_OK].Increment(stat.RangeReadBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_DATA_RANGE_READ_ITEMS_OK].Increment(stat.RangeReadItems);
        TabletCounters->Cumulative()[COUNTER_CMD_DATA_RANGE_READ_ITEMS_NODATA].Increment(stat.RangeReadItemsNodata);
        TabletCounters->Cumulative()[COUNTER_CMD_INDEX_RANGE_READ_OK].Increment(stat.IndexRangeRead);
        TabletCounters->Cumulative()[COUNTER_CMD_DELETE_OK].Increment(stat.Deletes);
        TabletCounters->Cumulative()[COUNTER_CMD_DELETE_BYTES_OK].Increment(stat.DeleteBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_RENAME_OK].Increment(stat.Renames);
        TabletCounters->Cumulative()[COUNTER_CMD_WRITE_BYTES_OK].Increment(stat.WriteBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_WRITE_OK].Increment(stat.Writes);
        TabletCounters->Cumulative()[COUNTER_CMD_COPY_RANGE_OK].Increment(stat.CopyRanges);
        TabletCounters->Cumulative()[COUNTER_CMD_GUM_OK].Increment(stat.Concats);
    } else if (status == NMsgBusProxy::MSTATUS_TIMEOUT) {
        TabletCounters->Cumulative()[COUNTER_CMD_READ_BYTES_TIMEOUT].Increment(stat.ReadBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_READ_TIMEOUT].Increment(stat.Reads);
        TabletCounters->Cumulative()[COUNTER_CMD_DATA_RANGE_READ_BYTES_TIMEOUT].Increment(stat.RangeReadBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_DATA_RANGE_READ_ITEMS_TIMEOUT].Increment(stat.RangeReadItems);
        TabletCounters->Cumulative()[COUNTER_CMD_INDEX_RANGE_READ_TIMEOUT].Increment(stat.IndexRangeRead);
        TabletCounters->Cumulative()[COUNTER_CMD_DELETE_TIMEOUT].Increment(stat.Deletes);
        TabletCounters->Cumulative()[COUNTER_CMD_DELETE_BYTES_TIMEOUT].Increment(stat.DeleteBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_RENAME_TIMEOUT].Increment(stat.Renames);
        TabletCounters->Cumulative()[COUNTER_CMD_WRITE_BYTES_TIMEOUT].Increment(stat.WriteBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_WRITE_TIMEOUT].Increment(stat.Writes);
        TabletCounters->Cumulative()[COUNTER_CMD_COPY_RANGE_TIMEOUT].Increment(stat.CopyRanges);
        TabletCounters->Cumulative()[COUNTER_CMD_GUM_TIMEOUT].Increment(stat.Concats);
    } else {
        TabletCounters->Cumulative()[COUNTER_CMD_READ_BYTES_OTHER_ERROR].Increment(stat.ReadBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_READ_OTHER_ERROR].Increment(stat.Reads);
        TabletCounters->Cumulative()[COUNTER_CMD_DATA_RANGE_READ_BYTES_OTHER_ERROR].Increment(stat.RangeReadBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_DATA_RANGE_READ_ITEMS_OTHER_ERROR].Increment(stat.RangeReadItems);
        TabletCounters->Cumulative()[COUNTER_CMD_INDEX_RANGE_READ_OTHER_ERROR].Increment(stat.IndexRangeRead);
        TabletCounters->Cumulative()[COUNTER_CMD_DELETE_OTHER_ERROR].Increment(stat.Deletes);
        TabletCounters->Cumulative()[COUNTER_CMD_DELETE_BYTES_OTHER_ERROR].Increment(stat.DeleteBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_RENAME_OTHER_ERROR].Increment(stat.Renames);
        TabletCounters->Cumulative()[COUNTER_CMD_WRITE_BYTES_OTHER_ERROR].Increment(stat.WriteBytes);
        TabletCounters->Cumulative()[COUNTER_CMD_WRITE_OTHER_ERROR].Increment(stat.Writes);
        TabletCounters->Cumulative()[COUNTER_CMD_COPY_RANGE_OTHER_ERROR].Increment(stat.CopyRanges);
        TabletCounters->Cumulative()[COUNTER_CMD_GUM_OTHER_ERROR].Increment(stat.Concats);
    }

    for (const auto latency: stat.GetLatencies) {
        TabletCounters->Percentile()[COUNTER_LATENCY_BS_GET].IncrementFor(latency);
    }
    for (const auto latency: stat.PutLatencies) {
        TabletCounters->Percentile()[COUNTER_LATENCY_BS_PUT].IncrementFor(latency);
    }
}

void TKeyValueState::CountRequestTakeOffOrEnqueue(TRequestType::EType requestType) {
    if (requestType == TRequestType::WriteOnly) {
        TabletCounters->Simple()[COUNTER_REQ_WO_IN_FLY].Add(1);
    } else {
        if (requestType == TRequestType::ReadOnlyInline) {
            TabletCounters->Simple()[COUNTER_REQ_RO_INLINE_IN_FLY].Set(RoInlineIntermediatesInFlight);
        } else {
            TabletCounters->Simple()[COUNTER_REQ_RO_RW_IN_FLY].Set(IntermediatesInFlight);
        }
        TabletCounters->Simple()[COUNTER_REQ_RO_RW_QUEUED].Set(Queue.size());
    }
}

void TKeyValueState::CountRequestOtherError(TRequestType::EType requestType) {
    if (requestType == TRequestType::ReadOnly || requestType == TRequestType::ReadOnlyInline) {
        TabletCounters->Cumulative()[COUNTER_REQ_RO_OTHER_ERROR].Increment(1);
    } else if (requestType == TRequestType::WriteOnly) {
        TabletCounters->Cumulative()[COUNTER_REQ_WO_OTHER_ERROR].Increment(1);
    } else {
        TabletCounters->Cumulative()[COUNTER_REQ_RW_OTHER_ERROR].Increment(1);
    }
}

void TKeyValueState::CountRequestIncoming(TRequestType::EType requestType) {
    // CountRequestIncoming is called before the request type is fully determined
    // it's impossible to separate ReadOnly and ReadOnlyInline, both are actually ReadOnly here
    if (requestType == TRequestType::ReadOnly || requestType == TRequestType::ReadOnlyInline) {
        TabletCounters->Cumulative()[COUNTER_REQ_RO].Increment(1);
    } else if (requestType == TRequestType::WriteOnly) {
        TabletCounters->Cumulative()[COUNTER_REQ_WO].Increment(1);
    } else {
        TabletCounters->Cumulative()[COUNTER_REQ_RW].Increment(1);
    }
}

void TKeyValueState::CountTrashRecord(ui32 sizeBytes) {
    TabletCounters->Simple()[COUNTER_TRASH_COUNT].Add((ui64)1);
    TabletCounters->Simple()[COUNTER_TRASH_BYTES].Add((ui64)sizeBytes);
    TabletCounters->Simple()[COUNTER_RECORD_COUNT].Add((ui64)-1);
    TabletCounters->Simple()[COUNTER_RECORD_BYTES].Add((ui64)-(i64)sizeBytes);
    ResourceMetrics->StorageUser.Set(TabletCounters->Simple()[COUNTER_RECORD_BYTES].Get());
}

void TKeyValueState::CountWriteRecord(ui8 channel, ui32 sizeBytes) {
    TabletCounters->Simple()[COUNTER_RECORD_COUNT].Add(1);
    TabletCounters->Simple()[COUNTER_RECORD_BYTES].Add(sizeBytes);
    ResourceMetrics->StorageUser.Set(TabletCounters->Simple()[COUNTER_RECORD_BYTES].Get());
    Y_VERIFY(channel < ChannelDataUsage.size());
    ChannelDataUsage[channel] += sizeBytes;
}

void TKeyValueState::CountInitialTrashRecord(ui32 sizeBytes) {
    TabletCounters->Simple()[COUNTER_TRASH_COUNT].Add((ui64)1);
    TabletCounters->Simple()[COUNTER_TRASH_BYTES].Add((ui64)sizeBytes);
}

void TKeyValueState::CountTrashCollected(ui32 sizeBytes) {
    TabletCounters->Simple()[COUNTER_TRASH_COUNT].Add((ui64)-1);
    TabletCounters->Simple()[COUNTER_TRASH_BYTES].Add((ui64)-(i64)sizeBytes);
}

void TKeyValueState::CountOverrun() {
    TabletCounters->Cumulative()[COUNTER_REQ_OVERRUN].Increment(1);
}

void TKeyValueState::CountLatencyBsOps(const TRequestStat &stat) {
    ui64 bsDuration = (TAppData::TimeProvider->Now() - stat.KeyvalueStorageRequestSentAt).MilliSeconds();
    TabletCounters->Percentile()[COUNTER_LATENCY_BS_OPS].IncrementFor(bsDuration);
}

void TKeyValueState::CountLatencyBsCollect() {
    ui64 collectDurationMs = (TAppData::TimeProvider->Now() - LastCollectStartedAt).MilliSeconds();
    TabletCounters->Percentile()[COUNTER_LATENCY_BS_COLLECT].IncrementFor(collectDurationMs);
}

void TKeyValueState::CountLatencyQueue(const TRequestStat &stat) {
    ui64 enqueuedMs = (TAppData::TimeProvider->Now() - stat.IntermediateCreatedAt).MilliSeconds();
    if (stat.RequestType == TRequestType::WriteOnly) {
        TabletCounters->Percentile()[COUNTER_LATENCY_QUEUE_WO].IncrementFor(enqueuedMs);
    } else {
        TabletCounters->Percentile()[COUNTER_LATENCY_QUEUE_RO_RW].IncrementFor(enqueuedMs);
    }
}

void TKeyValueState::CountLatencyLocalBase(const TIntermediate &intermediate) {
    ui64 localBaseTxDurationMs =
        (TAppData::TimeProvider->Now() - intermediate.Stat.LocalBaseTxCreatedAt).MilliSeconds();
    if (intermediate.Stat.RequestType == TRequestType::WriteOnly) {
        TabletCounters->Percentile()[COUNTER_LATENCY_LOCAL_BASE_WO].IncrementFor(localBaseTxDurationMs);
    } else if (intermediate.Stat.RequestType == TRequestType::ReadWrite) {
        TabletCounters->Percentile()[COUNTER_LATENCY_LOCAL_BASE_RW].IncrementFor(localBaseTxDurationMs);
    }
}

void TKeyValueState::CountStarting() {
    TabletCounters->Simple()[COUNTER_STATE_STARTING].Set(1);
    TabletCounters->Simple()[COUNTER_STATE_PROCESSING_INIT_QUEUE].Set(0);
    TabletCounters->Simple()[COUNTER_STATE_ONLINE].Set(0);
}

void TKeyValueState::CountProcessingInitQueue() {
    TabletCounters->Simple()[COUNTER_STATE_STARTING].Set(0);
    TabletCounters->Simple()[COUNTER_STATE_PROCESSING_INIT_QUEUE].Set(1);
    TabletCounters->Simple()[COUNTER_STATE_ONLINE].Set(0);
}

void TKeyValueState::CountOnline() {
    TabletCounters->Simple()[COUNTER_STATE_STARTING].Set(0);
    TabletCounters->Simple()[COUNTER_STATE_PROCESSING_INIT_QUEUE].Set(0);
    TabletCounters->Simple()[COUNTER_STATE_ONLINE].Set(1);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Initialization
//

void TKeyValueState::Terminate(const TActorContext& ctx) {
    ctx.Send(ChannelBalancerActorId, new TEvents::TEvPoisonPill);
}

void TKeyValueState::Load(const TString &key, const TString& value) {
    if (IsEmptyDbStart) {
        IsEmptyDbStart = false;
    }

    TString arbitraryPart;
    TKeyHeader header;
    bool isOk = THelpers::ExtractKeyParts(key, arbitraryPart, header);
    Y_VERIFY(isOk);

    switch (header.ItemType) {
        case EIT_UNKNOWN: {
            Y_VERIFY(false, "Unexpected EIT_UNKNOWN key header.");
            break;
        }
        case EIT_KEYVALUE_1:
        {
            TIndexRecord &record = Index[arbitraryPart];
            TString errorInfo;
            bool isOk = false;
            EItemType headerItemType = TIndexRecord::ReadItemType(value);
            if (headerItemType == EIT_KEYVALUE_1) {
                isOk = record.Deserialize1(value, errorInfo);
            } else {
                isOk = record.Deserialize2(value, errorInfo);
            }
            if (!isOk) {
                TStringStream str;
                str << " Tablet# " << TabletId;
                str << " KeyArbitraryPart# \"" << arbitraryPart << "\"";
                str << errorInfo;
                if (KIKIMR_KEYVALUE_ALLOW_DAMAGE) {
                    str << Endl;
                    Cerr << str.Str();
                    IsDamaged = true;
                } else {
                    Y_VERIFY(false, "%s", str.Str().data());
                }
            }
            for (const TIndexRecord::TChainItem& item : record.Chain) {
                if (!item.IsInline()) {
                    const ui32 newRefCount = ++RefCounts[item.LogoBlobId];
                    if (newRefCount == 1) {
                        CountWriteRecord(item.LogoBlobId.Channel(), item.LogoBlobId.BlobSize());
                    }
                } else {
                    CountWriteRecord(0, item.InlineData.size());
                }
            }
            break;
        }
        case EIT_TRASH: {
            Y_VERIFY(value.size() == 0);
            Y_VERIFY(arbitraryPart.size() == sizeof(TTrashKeyArbitrary));
            const TTrashKeyArbitrary *trashKey = (const TTrashKeyArbitrary *) arbitraryPart.data();
            Trash.insert(trashKey->LogoBlobId);
            CountInitialTrashRecord(trashKey->LogoBlobId.BlobSize());
            break;
        }
        case EIT_COLLECT: {
            Y_VERIFY(arbitraryPart.size() == 0);
            Y_VERIFY(!CollectOperation.Get());
            Y_VERIFY(value.size() >= sizeof(TCollectOperationHeader));
            const TCollectOperationHeader *header = (const TCollectOperationHeader*)value.data();
            Y_VERIFY(header->DataHeader.ItemType == EIT_COLLECT);
            ui64 totalSize = sizeof(TCollectOperationHeader)
                + sizeof(TLogoBlobID) * (header->KeepCount + header->DoNotKeepCount);
            Y_VERIFY(value.size() == totalSize);
            TVector<TLogoBlobID> keep;
            TVector<TLogoBlobID> doNotKeep;
            keep.resize(header->KeepCount);
            doNotKeep.resize(header->DoNotKeepCount);
            const char* data = value.data() + sizeof(TCollectOperationHeader);
            if (keep.size()) {
                memcpy((char *) &keep[0], data, sizeof(TLogoBlobID) * keep.size());
            }
            data += sizeof(TLogoBlobID) * keep.size();
            if (doNotKeep.size()) {
                memcpy((char *) &doNotKeep[0], data, sizeof(TLogoBlobID) * doNotKeep.size());
            }
            CollectOperation.Reset(new TCollectOperation(
                header->CollectGeneration, header->CollectStep, std::move(keep), std::move(doNotKeep)));
            break;
        }
        case EIT_STATE: {
            Y_VERIFY(!IsStatePresent);
            IsStatePresent = true;
            Y_VERIFY(arbitraryPart.size() == 0);
            Y_VERIFY(value.size() >= sizeof(TKeyValueStoredStateData));
            const TKeyValueStoredStateData *data = (const TKeyValueStoredStateData *) value.data();
            Y_VERIFY(data->CheckChecksum());
            Y_VERIFY(data->DataHeader.ItemType == EIT_STATE);
            StoredState = *data;
            break;
        }
        default: {
            Y_VERIFY(false, "Unexcpected header.ItemType# %" PRIu32, (ui32)header.ItemType);
            break;
        }
    }
}

void TKeyValueState::InitExecute(ui64 tabletId, TActorId keyValueActorId, ui32 executorGeneration,
        ISimpleDb &db, const TActorContext &ctx, const TTabletStorageInfo *info) {
    Y_UNUSED(info);
    Y_VERIFY(IsEmptyDbStart || IsStatePresent);
    TabletId = tabletId;
    KeyValueActorId = keyValueActorId;
    ExecutorGeneration = executorGeneration;
    if (IsDamaged) {
        return;
    }
    ui8 maxChannel = 0;
    for (const auto& channel : info->Channels) {
        const ui32 index = channel.Channel;
        Y_VERIFY(index <= UsedChannels.size());
        if (index == 0 || index >= 2) {
            UsedChannels[index] = true;
            maxChannel = Max<ui8>(maxChannel, index);
        }
    }
    ChannelBalancerActorId = ctx.Register(new TChannelBalancer(maxChannel + 1, KeyValueActorId));

    // Issue hard barriers
    {
        using TGroupChannel = std::tuple<ui32, ui8>;
        THashMap<TGroupChannel, THelpers::TGenerationStep> hardBarriers;
        for (const auto &kv : RefCounts) {
            // extract blob id and validate its channel
            const TLogoBlobID &id = kv.first;
            const ui8 channel = id.Channel();
            Y_VERIFY(channel >= BLOB_CHANNEL);

            // create (generation, step) pair for current item and decrement it by one step as the barriers
            // are always inclusive
            const ui32 generation = id.Generation();
            const ui32 step = id.Step();
            TMaybe<THelpers::TGenerationStep> current;
            if (step) {
                current = THelpers::TGenerationStep(generation, step - 1);
            } else if (generation) {
                current = THelpers::TGenerationStep(generation - 1, Max<ui32>());
            }

            // update minimum barrier value for this channel/group
            if (current) {
                const ui32 group = info->GroupFor(channel, generation);
                if (group != Max<ui32>()) {
                    const TGroupChannel key(group, channel);
                    auto it = hardBarriers.find(key);
                    if (it == hardBarriers.end()) {
                        hardBarriers.emplace(key, *current);
                    } else if (*current < it->second) {
                        it->second = *current;
                    }
                }
            }
        }
        for (const auto &channel : info->Channels) {
            if (channel.Channel < BLOB_CHANNEL) {
                continue;
            }
            for (const auto &history : channel.History) {
                const TGroupChannel key(history.GroupID, channel.Channel);
                if (!hardBarriers.count(key)) {
                    hardBarriers.emplace(key, THelpers::TGenerationStep(executorGeneration - 1, Max<ui32>()));
                }
            }
        }
        for (const auto &kv : hardBarriers) {
            ui32 group;
            ui8 channel;
            std::tie(group, channel) = kv.first;

            ui32 generation;
            ui32 step;
            std::tie(generation, step) = kv.second;

            auto ev = TEvBlobStorage::TEvCollectGarbage::CreateHardBarrier(info->TabletID, executorGeneration,
                PerGenerationCounter, channel, generation, step, TInstant::Max());
            ++PerGenerationCounter;

            const TActorId nodeWarden = MakeBlobStorageNodeWardenID(ctx.SelfID.NodeId());
            const TActorId proxy = MakeBlobStorageProxyID(group);
            ctx.ExecutorThread.Send(new IEventHandle(proxy, TActorId(), ev.Release(),
                IEventHandle::FlagForwardOnNondelivery, (ui64)TKeyValueState::ECollectCookie::Hard, &nodeWarden));
        }
    }

    // Issue soft barriers
    // Mark fresh blobs (after previous collected GenStep) with keep flag and issue barrier at current GenStep
    // to remove all phantom blobs
    {
        using TGroupChannel = std::tuple<ui32, ui8>;
        THashMap<TGroupChannel, THolder<TVector<TLogoBlobID>>> keepForGroupChannel;
        const ui32 barrierGeneration = executorGeneration - 1;
        const ui32 barrierStep = Max<ui32>();

        auto addBlobToKeep = [&] (const TLogoBlobID &id) {
            ui32 group = info->GroupFor(id.Channel(), id.Generation());
            Y_VERIFY(group != Max<ui32>(), "RefBlob# %s is mapped to an invalid group (-1)!",
                    id.ToString().c_str());
            TGroupChannel key(group, id.Channel());
            THolder<TVector<TLogoBlobID>> &ptr = keepForGroupChannel[key];
            if (!ptr) {
                ptr = MakeHolder<TVector<TLogoBlobID>>();
            }
            ptr->emplace_back(id);
        };

        for (const auto &refInfo : RefCounts) {
            // Extract blob id and validate its channel
            const TLogoBlobID &id = refInfo.first;
            Y_VERIFY(id.Channel() >= BLOB_CHANNEL);

            const THelpers::TGenerationStep blobGenStep = THelpers::GenerationStep(id);
            const THelpers::TGenerationStep storedGenStep(StoredState.GetCollectGeneration(), StoredState.GetCollectStep());
            // Mark with keep flag only new blobs
            if (storedGenStep < blobGenStep) {
                addBlobToKeep(id);
            }
        }

        if (CollectOperation) {
            for (const TLogoBlobID &id : CollectOperation->Keep) {
                addBlobToKeep(id);
            }
        }

        for (const auto &channelInfo : info->Channels) {
            if (channelInfo.Channel < BLOB_CHANNEL) {
                continue;
            }
            // Issue soft barriers for groups without blobs because
            // we can possibly write in them on previous generations
            for (ui64 gen = StoredState.GetCollectGeneration(); gen <= barrierGeneration; ++gen) {
                const ui32 groupId = channelInfo.GroupForGeneration(gen);
                if (groupId != Max<ui32>()) {
                    const TGroupChannel groupChannel(groupId, channelInfo.Channel);
                    if (keepForGroupChannel.find(groupChannel) == keepForGroupChannel.end()) {
                        keepForGroupChannel[groupChannel] = nullptr;
                    }
                }
            }
        }
        InitialCollectsSent = keepForGroupChannel.size();
        for (auto &keepInfo : keepForGroupChannel) {
            ui32 group;
            ui8 channel;
            std::tie(group, channel) = keepInfo.first;

            THolder<TVector<TLogoBlobID>>& keep = keepInfo.second;
            auto ev = MakeHolder<TEvBlobStorage::TEvCollectGarbage>(info->TabletID, executorGeneration,
                    PerGenerationCounter, channel, true /*collect*/, barrierGeneration, barrierStep, keep.Release(),
                    nullptr /*doNotKeep*/, TInstant::Max(), false /*isMultiCollectAllowed*/, false /*hard*/);
            ++PerGenerationCounter;

            const TActorId nodeWarden = MakeBlobStorageNodeWardenID(ctx.SelfID.NodeId());
            const TActorId proxy = MakeBlobStorageProxyID(group);
            ctx.ExecutorThread.Send(new IEventHandle(proxy, KeyValueActorId, ev.Release(),
                IEventHandle::FlagForwardOnNondelivery, (ui64)TKeyValueState::ECollectCookie::SoftInitial, &nodeWarden));
        }
    }

    // Make a copy of the channel history (need a way to map channel/generation to history record)
    // History entry contains data on from-to generation for each channel
    auto &channels = info->Channels;
    ChannelRangeSets.reserve(channels.size());
    for (ui64 idx = 0; idx < channels.size(); ++idx) {
        auto &channelInfo = channels[idx];
        if (channelInfo.Channel < BLOB_CHANNEL) {
             // Remove (do not add) non-KV tablet managed channels
            continue;
        }
        if (channelInfo.Channel >= ChannelRangeSets.size()) {
            ChannelRangeSets.resize(channelInfo.Channel + 1);
        }
        auto &history = channelInfo.History;
        auto &rangeSet = ChannelRangeSets[channelInfo.Channel];
        // Remove (do not add) the latest entries for all channels
        auto endIt = history.end();
        if (endIt != history.begin()) {
            endIt--;
        }
        for (auto it = history.begin(); it != endIt; ++it) {
            ui64 begin = it->FromGeneration;
            auto nextIt = it;
            nextIt++;
            ui64 end = nextIt->FromGeneration;
            rangeSet.Add(begin, end);
        }
    }
    // Walk through the index,trash and coolect operaton lists and remove non-empty channel-generation pairs
    for (auto recordIt = Index.begin(); recordIt != Index.end(); ++recordIt) {
        auto &chain = recordIt->second.Chain;
        for (auto itemIt = chain.begin(); itemIt != chain.end(); ++itemIt) {
            TLogoBlobID &id = itemIt->LogoBlobId;
            if (id.Channel() < ChannelRangeSets.size()) {
                ChannelRangeSets[id.Channel()].Remove(id.Generation());
            }
        }
    }
    for (auto it = Trash.begin(); it != Trash.end(); ++it) {
        const TLogoBlobID &id = *it;
        if (id.Channel() < ChannelRangeSets.size()) {
            ChannelRangeSets[id.Channel()].Remove(id.Generation());
        }
    }
    if (CollectOperation) {
        auto &keep = CollectOperation->Keep;
        for (auto it = keep.begin(); it != keep.end(); ++it) {
            const TLogoBlobID &id = *it;
            if (id.Channel() < ChannelRangeSets.size()) {
                ChannelRangeSets[id.Channel()].Remove(id.Generation());
            }
        }
        auto &doNotKeep = CollectOperation->DoNotKeep;
        for (auto it = doNotKeep.begin(); it != doNotKeep.end(); ++it) {
            const TLogoBlobID &id = *it;
            if (id.Channel() < ChannelRangeSets.size()) {
                ChannelRangeSets[id.Channel()].Remove(id.Generation());
            }
        }
        // Patch collect operation generation and step
        Y_VERIFY(CollectOperation->Header.GetCollectGeneration() < ExecutorGeneration);
        CollectOperation->Header.SetCollectGeneration(ExecutorGeneration);
        CollectOperation->Header.SetCollectStep(0);
    }

    if (CollectOperation) {
        for (const TLogoBlobID &id : CollectOperation->DoNotKeep) {
            Trash.insert(id);
            THelpers::DbUpdateTrash(id, db, ctx);
        }
        THelpers::DbEraseCollect(db, ctx);
        CollectOperation = nullptr;
    }


    THelpers::DbUpdateState(StoredState, db, ctx);


    // corner case, if no CollectGarbage events were sent
    if (InitialCollectsSent == 0) {
        SendCutHistory(ctx);
        RegisterInitialGCCompletionComplete(ctx);
    } else {
        IsCollectEventSent = true;
    }
}



bool TKeyValueState::RegisterInitialCollectResult(const TActorContext &ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
        << " InitialCollectsSent# " << InitialCollectsSent << " Marker# KV50");
    if (--InitialCollectsSent == 0) {
        SendCutHistory(ctx);
        return true;
    }
    return false;
}


void TKeyValueState::RegisterInitialGCCompletionExecute(ISimpleDb &db, const TActorContext &ctx) {
    StoredState.SetCollectGeneration(ExecutorGeneration);
    StoredState.SetCollectStep(0);
    THelpers::DbUpdateState(StoredState, db, ctx);
}

void TKeyValueState::RegisterInitialGCCompletionComplete(const TActorContext &ctx) {
    IsCollectEventSent = false;
    // initiate collection if trash was loaded from local base
    PrepareCollectIfNeeded(ctx);
}

void TKeyValueState::SendCutHistory(const TActorContext &ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
        << " SendCutHistory Marker# KV51");
    // Prepare and send a messages to the Local
    for (ui64 channel = 0; channel < ChannelRangeSets.size(); ++channel) {
        auto endForBegin = ChannelRangeSets[channel].EndForBegin;
        for (auto it = endForBegin.begin(); it != endForBegin.end(); ++it) {
            ui32 fromGeneration = it->first;
            TAutoPtr<TEvTablet::TEvCutTabletHistory> ev(new TEvTablet::TEvCutTabletHistory);
            auto &record = ev->Record;
            record.SetTabletID(TabletId);
            record.SetChannel(channel);
            record.SetFromGeneration(fromGeneration);
            TActorId localActorId = MakeLocalID(ctx.SelfID.NodeId());
            ctx.Send(localActorId, ev.Release());
        }
    }
    ChannelRangeSets.clear();
}

void TKeyValueState::OnInitQueueEmpty(const TActorContext &ctx) {
    Y_UNUSED(ctx);
    CountOnline();
}

void TKeyValueState::OnStateWork(const TActorContext &ctx) {
    Y_UNUSED(ctx);
    CountProcessingInitQueue();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// User Request Processing
//

void TKeyValueState::Step() {
    if (NextLogoBlobStep < Max<ui32>()) {
        NextLogoBlobCookie = 1;
        ++NextLogoBlobStep;
    } else {
        Y_FAIL("Max step reached!");
    }
}

TLogoBlobID TKeyValueState::AllocateLogoBlobId(ui32 size, ui32 storageChannelIdx) {
    ui32 generation = ExecutorGeneration;
    TLogoBlobID id(TabletId, generation, NextLogoBlobStep, storageChannelIdx, size, NextLogoBlobCookie);
    if (NextLogoBlobCookie < TLogoBlobID::MaxCookie) {
        ++NextLogoBlobCookie;
    } else {
        Step();
    }
    Y_VERIFY(!CollectOperation || THelpers::GenerationStep(id) >
        THelpers::TGenerationStep(CollectOperation->Header.GetCollectGeneration(), CollectOperation->Header.GetCollectStep()));
    return id;
}

void TKeyValueState::RequestExecute(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx,
        const TTabletStorageInfo *info) {
    if (IsDamaged) {
        return;
    }
    intermediate->Response.Clear();
    intermediate->Response.SetStatus(NMsgBusProxy::MSTATUS_UNKNOWN);
    if (intermediate->HasCookie) {
        intermediate->Response.SetCookie(intermediate->Cookie);
    }

    // Check the generation
    if (intermediate->HasGeneration) {
        if (intermediate->Generation != StoredState.GetUserGeneration()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Generation mismatch! Requested# " << intermediate->Generation;
            str << " Actual# " << StoredState.GetUserGeneration();
            str << " Marker# KV17";
            LOG_INFO_S(ctx, NKikimrServices::KEYVALUE, str.Str());
            // All reads done
            intermediate->Response.SetStatus(NMsgBusProxy::MSTATUS_REJECTED);
            intermediate->Response.SetErrorReason(str.Str());
            return;
        }
    }

    // Process CmdIncrementGeneration()
    if (intermediate->HasIncrementGeneration) {

        bool IsOk = intermediate->Commands.size() == 0
            && intermediate->Deletes.size() == 0 && intermediate->RangeReads.size() == 0
            && intermediate->Reads.size() == 0 && intermediate->Renames.size() == 0
            && intermediate->Writes.size() == 0 && intermediate->GetStatuses.size() == 0;

        if (IsOk) {
            IncrementGeneration(intermediate, db, ctx);
            return;
        } else {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " CmdIncrementGeneration can't be grouped with any other Cmd!";
            str << " Commands# " << intermediate->Commands.size();
            str << " Deletes# " << intermediate->Deletes.size();
            str << " RangeReads# " << intermediate->RangeReads.size();
            str << " Reads# " << intermediate->Reads.size();
            str << " Renames# " << intermediate->Renames.size();
            str << " Writes# " << intermediate->Writes.size();
            str << " GetStatuses# " << intermediate->GetStatuses.size();
            str << " CopyRanges# " << intermediate->CopyRanges.size();
            LOG_INFO_S(ctx, NKikimrServices::KEYVALUE, str.Str());
            // All reads done
            intermediate->Response.SetStatus(NMsgBusProxy::MSTATUS_INTERNALERROR);
            intermediate->Response.SetErrorReason(str.Str());
            return;
        }
    }

    // TODO: Validate arguments

    ProcessCmds(intermediate, db, ctx, info);

}

void TKeyValueState::RequestComplete(THolder<TIntermediate> &intermediate, const TActorContext &ctx,
        const TTabletStorageInfo *info) {

    Reply(intermediate, ctx, info);
    // Make sure there is no way for a successfull delete transaction to end without getting here
    PrepareCollectIfNeeded(ctx);
}

///////////////////////////////////////////////////////////////////////////////
// Request processing
//

void TKeyValueState::Reply(THolder<TIntermediate> &intermediate, const TActorContext &ctx,
        const TTabletStorageInfo *info) {
    if (!intermediate->IsReplied) {
        if (intermediate->EvType == TEvKeyValue::TEvRequest::EventType) {
            THolder<TEvKeyValue::TEvResponse> response(new TEvKeyValue::TEvResponse);
            response->Record = intermediate->Response;
            ResourceMetrics->Network.Increment(response->Record.ByteSize());
            ctx.Send(intermediate->RespondTo, response.Release());
        }
        if (intermediate->EvType == TEvKeyValue::TEvExecuteTransaction::EventType) {
            THolder<TEvKeyValue::TEvExecuteTransactionResponse> response(new TEvKeyValue::TEvExecuteTransactionResponse);
            response->Record = intermediate->ExecuteTransactionResponse;
            ResourceMetrics->Network.Increment(response->Record.ByteSize());
            if (intermediate->RespondTo.NodeId() != ctx.SelfID.NodeId()) {
                response->Record.set_node_id(ctx.SelfID.NodeId());
            }
            ctx.Send(intermediate->RespondTo, response.Release());
        }
        if (intermediate->EvType == TEvKeyValue::TEvGetStorageChannelStatus::EventType) {
            THolder<TEvKeyValue::TEvGetStorageChannelStatusResponse> response(new TEvKeyValue::TEvGetStorageChannelStatusResponse);
            response->Record = intermediate->GetStorageChannelStatusResponse;
            ResourceMetrics->Network.Increment(response->Record.ByteSize());
            if (intermediate->RespondTo.NodeId() != ctx.SelfID.NodeId()) {
                response->Record.set_node_id(ctx.SelfID.NodeId());
            }
            ctx.Send(intermediate->RespondTo, response.Release());
        }
        if (intermediate->EvType == TEvKeyValue::TEvAcquireLock::EventType) {
            THolder<TEvKeyValue::TEvAcquireLockResponse> response(new TEvKeyValue::TEvAcquireLockResponse);
            response->Record.set_lock_generation(StoredState.GetUserGeneration());
            response->Record.set_cookie(intermediate->Cookie);
            ResourceMetrics->Network.Increment(response->Record.ByteSize());
            if (intermediate->RespondTo.NodeId() != ctx.SelfID.NodeId()) {
                response->Record.set_node_id(ctx.SelfID.NodeId());
            }
            ctx.Send(intermediate->RespondTo, response.Release());
        }
        intermediate->IsReplied = true;
        intermediate->UpdateStat();

        CountLatencyLocalBase(*intermediate);

        OnRequestComplete(intermediate->RequestUid, intermediate->CreatedAtGeneration, intermediate->CreatedAtStep,
            ctx, info, (NMsgBusProxy::EResponseStatus)intermediate->Response.GetStatus(), intermediate->Stat);
    }
}

void TKeyValueState::ProcessCmd(TIntermediate::TRead &request,
        NKikimrClient::TKeyValueResponse::TReadResult *legacyResponse,
        NKikimrKeyValue::StorageChannel */*response*/,
        ISimpleDb &/*db*/, const TActorContext &/*ctx*/, TRequestStat &/*stat*/, ui64 /*unixTime*/,
        TIntermediate* /*intermediate*/)
{
    NKikimrProto::EReplyStatus outStatus = request.CumulativeStatus();
    request.Status = outStatus;
    legacyResponse->SetStatus(outStatus);
    if (outStatus == NKikimrProto::OK) {
        legacyResponse->SetValue(request.Value);
        Y_VERIFY(request.Value.size() == request.ValueSize);
    } else {
        legacyResponse->SetMessage(request.Message);
        if (outStatus == NKikimrProto::NODATA) {
            for (ui32 itemIdx = 0; itemIdx < request.ReadItems.size(); ++itemIdx) {
                TIntermediate::TRead::TReadItem &item = request.ReadItems[itemIdx];
                // Make sure the blob is not referenced anymore
                auto refCountIt = RefCounts.find(item.LogoBlobId);
                if (refCountIt != RefCounts.end()) {
                    TStringStream str;
                    str << "KeyValue# " << TabletId
                        << " CmdRead "
                        //<< " ReadIdx# " << i
                        << " key# " << EscapeC(request.Key)
                        << " ItemIdx# " << itemIdx
                        << " BlobId# " << item.LogoBlobId.ToString()
                        << " Status# " << NKikimrProto::EReplyStatus_Name(item.Status)
                        << " outStatus# " << NKikimrProto::EReplyStatus_Name(outStatus)
                        << " but blob has RefCount# " << refCountIt->second
                        << " ! KEYVALUE CONSISTENCY ERROR!"
                        << " Message# " << request.Message
                        << " Marker# KV46";
                    Y_VERIFY(false, "%s", str.Str().c_str());
                }
            }
        }
    }
}

void TKeyValueState::ProcessCmd(TIntermediate::TRangeRead &request,
        NKikimrClient::TKeyValueResponse::TReadRangeResult *legacyResponse,
        NKikimrKeyValue::StorageChannel */*response*/,
        ISimpleDb &/*db*/, const TActorContext &/*ctx*/, TRequestStat &/*stat*/, ui64 /*unixTime*/,
        TIntermediate* /*intermediate*/)
{
    for (ui64 r = 0; r < request.Reads.size(); ++r) {
        auto &read = request.Reads[r];
        auto *resultKv = legacyResponse->AddPair();

        NKikimrProto::EReplyStatus outStatus = read.CumulativeStatus();
        read.Status = outStatus;
        if (outStatus != NKikimrProto::OK && outStatus != NKikimrProto::OVERRUN) {
            // LOG_ERROR_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " CmdReadRange " << r
            //    << " status " << NKikimrProto::EReplyStatus_Name(outStatus)
            //    << " message " << read.Message
            //    << " key " << EscapeC(read.Key));

            if (outStatus == NKikimrProto::NODATA) {
                for (ui32 itemIdx = 0; itemIdx < read.ReadItems.size(); ++itemIdx) {
                    TIntermediate::TRead::TReadItem &item = read.ReadItems[itemIdx];
                    // Make sure the blob is not referenced anymore
                    auto refCountIt = RefCounts.find(item.LogoBlobId);
                    if (refCountIt != RefCounts.end()) {
                        TStringStream str;
                        str << "KeyValue# " << TabletId
                            << " CmdReadRange "
                            // << " RangeReadIdx# " << i
                            << " ReadIdx# " << r
                            << " ItemIdx# " << itemIdx
                            << " key# " << EscapeC(read.Key)
                            << " BlobId# " << item.LogoBlobId.ToString()
                            << " Status# " << NKikimrProto::EReplyStatus_Name(item.Status)
                            << " outStatus# " << NKikimrProto::EReplyStatus_Name(outStatus)
                            << " but blob has RefCount# " << refCountIt->second
                            << " ! KEYVALUE CONSISTENCY ERROR!"
                            << " Message# " << read.Message
                            << " Marker# KV47";
                        Y_VERIFY(false, "%s", str.Str().c_str());
                    }
                }
            }
        }

        resultKv->SetStatus(outStatus);
        resultKv->SetKey(read.Key);
        if (request.IncludeData && (outStatus == NKikimrProto::OK || outStatus == NKikimrProto::OVERRUN)) {
            resultKv->SetValue(read.Value);
            Y_VERIFY(read.Value.size() == read.ValueSize);
        }
        resultKv->SetValueSize(read.ValueSize);
        resultKv->SetCreationUnixTime(read.CreationUnixTime);
        resultKv->SetStorageChannel(read.StorageChannel);
    }

    legacyResponse->SetStatus(request.Status);
}


NKikimrKeyValue::StorageChannel::StatusFlag GetStatusFlag(const TStorageStatusFlags &statusFlags) {
    if (statusFlags.Check(NKikimrBlobStorage::StatusDiskSpaceOrange)) {
        return NKikimrKeyValue::StorageChannel::STATUS_FLAG_ORANGE_OUT_SPACE;
    }
    if (statusFlags.Check(NKikimrBlobStorage::StatusDiskSpaceOrange)) {
        return NKikimrKeyValue::StorageChannel::STATUS_FLAG_YELLOW_STOP;
    }
    return NKikimrKeyValue::StorageChannel::STATUS_FLAG_GREEN;
}

void TKeyValueState::ProcessCmd(TIntermediate::TWrite &request,
        NKikimrClient::TKeyValueResponse::TWriteResult *legacyResponse,
        NKikimrKeyValue::StorageChannel *response,
        ISimpleDb &db, const TActorContext &ctx, TRequestStat &/*stat*/, ui64 unixTime,
        TIntermediate* /*intermediate*/)
{
    TIndexRecord& record = Index[request.Key];
    Dereference(record, db, ctx);

    record.Chain = {};
    ui32 storage_channel = 0;
    if (request.Status == NKikimrProto::SCHEDULED) {
        TString inlineData = request.Data;
        record.Chain.push_back(TIndexRecord::TChainItem(inlineData, 0));
        CountWriteRecord(0, inlineData.size());
        request.Status = NKikimrProto::OK;
        storage_channel = InlineStorageChannelInPublicApi;
    } else {
        int channel = -1;

        ui64 offset = 0;
        for (const TLogoBlobID& logoBlobId : request.LogoBlobIds) {
            record.Chain.push_back(TIndexRecord::TChainItem(logoBlobId, offset));
            offset += logoBlobId.BlobSize();
            CountWriteRecord(logoBlobId.Channel(), logoBlobId.BlobSize());
            if (channel == -1) {
                channel = logoBlobId.Channel();
            } else {
                // all blobs from the same write must be within the same channel
                Y_VERIFY(channel == (int)logoBlobId.Channel());
            }
        }
        storage_channel = channel + MainStorageChannelInPublicApi;

        ctx.Send(ChannelBalancerActorId, new TChannelBalancer::TEvReportWriteLatency(channel, request.Latency));
    }

    record.CreationUnixTime = unixTime;
    UpdateKeyValue(request.Key, record, db, ctx);

    if (legacyResponse) {
        legacyResponse->SetStatus(NKikimrProto::OK);
        legacyResponse->SetStatusFlags(request.StatusFlags.Raw);
    }
    if (response) {
        response->set_status(NKikimrKeyValue::Statuses::RSTATUS_OK);
        response->set_status_flag(GetStatusFlag(request.StatusFlags));
        response->set_storage_channel(storage_channel);
    }
}

void TKeyValueState::ProcessCmd(const TIntermediate::TDelete &request,
        NKikimrClient::TKeyValueResponse::TDeleteRangeResult *legacyResponse,
        NKikimrKeyValue::StorageChannel */*response*/,
        ISimpleDb &db, const TActorContext &ctx, TRequestStat &stat, ui64 /*unixTime*/,
        TIntermediate* /*intermediate*/)
{
    TraverseRange(request.Range, [&](TIndex::iterator it) {
        stat.Deletes++;
        stat.DeleteBytes += it->second.GetFullValueSize();
        Dereference(it->second, db, ctx);
        THelpers::DbEraseUserKey(it->first, db, ctx);
        Index.erase(it);
    });

    if (legacyResponse) {
        legacyResponse->SetStatus(NKikimrProto::OK);
    }
}

void TKeyValueState::ProcessCmd(const TIntermediate::TRename &request,
        NKikimrClient::TKeyValueResponse::TRenameResult *legacyResponse,
        NKikimrKeyValue::StorageChannel */*response*/,
        ISimpleDb &db, const TActorContext &ctx, TRequestStat &/*stat*/, ui64 unixTime,
        TIntermediate* /*intermediate*/)
{
    auto oldIter = Index.find(request.OldKey);
    Y_VERIFY(oldIter != Index.end());
    TIndexRecord& source = oldIter->second;

    TIndexRecord& dest = Index[request.NewKey];
    Dereference(dest, db, ctx);
    dest.Chain = std::move(source.Chain);
    dest.CreationUnixTime = unixTime;

    THelpers::DbEraseUserKey(oldIter->first, db, ctx);
    Index.erase(oldIter);

    UpdateKeyValue(request.NewKey, dest, db, ctx);

    if (legacyResponse) {
        legacyResponse->SetStatus(NKikimrProto::OK);
    }
}

void TKeyValueState::ProcessCmd(const TIntermediate::TCopyRange &request,
        NKikimrClient::TKeyValueResponse::TCopyRangeResult *legacyResponse,
        NKikimrKeyValue::StorageChannel */*response*/,
        ISimpleDb &db, const TActorContext &ctx, TRequestStat &/*stat*/, ui64 /*unixTime*/,
        TIntermediate *intermediate)
{
    TVector<TIndex::iterator> itemsToClone;

    TraverseRange(request.Range, [&](TIndex::iterator it) {
        if (it->first.StartsWith(request.PrefixToRemove)) {
            itemsToClone.push_back(it);
        }
    });

    for (TIndex::iterator it : itemsToClone) {
        const TIndexRecord& sourceRecord = it->second;
        for (const TIndexRecord::TChainItem& item : sourceRecord.Chain) {
            if (!item.IsInline()) {
                ++RefCounts[item.LogoBlobId];
                intermediate->RefCountsIncr.emplace_back(item.LogoBlobId, false);
            }
        }

        TString newKey = request.PrefixToAdd + it->first.substr(request.PrefixToRemove.size());
        TIndexRecord& record = Index[newKey];
        Dereference(record, db, ctx);
        record.Chain = sourceRecord.Chain;
        record.CreationUnixTime = sourceRecord.CreationUnixTime;
        UpdateKeyValue(newKey, record, db, ctx);
    }

    if (legacyResponse) {
        legacyResponse->SetStatus(NKikimrProto::OK);
    }
}

void TKeyValueState::ProcessCmd(const TIntermediate::TConcat &request,
        NKikimrClient::TKeyValueResponse::TConcatResult *legacyResponse,
        NKikimrKeyValue::StorageChannel */*response*/,
        ISimpleDb &db, const TActorContext &ctx, TRequestStat &/*stat*/, ui64 unixTime,
        TIntermediate *intermediate)
{
    TVector<TIndexRecord::TChainItem> chain;
    ui64 offset = 0;

    for (const TString& key : request.InputKeys) {
        auto it = Index.find(key);
        Y_VERIFY(it != Index.end());
        TIndexRecord& input = it->second;

        for (TIndexRecord::TChainItem& chainItem : input.Chain) {
            if (chainItem.IsInline()) {
                chain.push_back(TIndexRecord::TChainItem(chainItem.InlineData, offset));
            } else {
                const TLogoBlobID& id = chainItem.LogoBlobId;
                chain.push_back(TIndexRecord::TChainItem(id, offset));
                ++RefCounts[id];
                intermediate->RefCountsIncr.emplace_back(id, false);
            }
            offset += chainItem.GetSize();
        }

        if (!request.KeepInputs) {
            Dereference(input, db, ctx);
            THelpers::DbEraseUserKey(it->first, db, ctx);
            Index.erase(it);
        }
    }

    TIndexRecord& record = Index[request.OutputKey];
    Dereference(record, db, ctx);
    record.Chain = std::move(chain);
    record.CreationUnixTime = unixTime;
    UpdateKeyValue(request.OutputKey, record, db, ctx);

    if (legacyResponse) {
        legacyResponse->SetStatus(NKikimrProto::OK);
    }
}

void TKeyValueState::CmdRead(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx) {
    for (ui64 i = 0; i < intermediate->Reads.size(); ++i) {
        auto &request = intermediate->Reads[i];
        auto *response = intermediate->Response.AddReadResult();
        ProcessCmd(request, response, nullptr, db, ctx, intermediate->Stat, 0, intermediate.Get());
    }
    if (intermediate->ReadCommand && std::holds_alternative<TIntermediate::TRead>(*intermediate->ReadCommand)) {
        auto &request = std::get<TIntermediate::TRead>(*intermediate->ReadCommand);
        auto *response = intermediate->Response.AddReadResult();
        ProcessCmd(request, response, nullptr, db, ctx, intermediate->Stat, 0, intermediate.Get());
    }
}

void TKeyValueState::CmdReadRange(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx) {
    Y_UNUSED(ctx);
    Y_UNUSED(db);
    for (ui64 i = 0; i < intermediate->RangeReads.size(); ++i) {
        auto &rangeRead = intermediate->RangeReads[i];
        auto *rangeReadResult = intermediate->Response.AddReadRangeResult();
        ProcessCmd(rangeRead, rangeReadResult, nullptr, db, ctx, intermediate->Stat, 0, intermediate.Get());
    }
    if (intermediate->ReadCommand && std::holds_alternative<TIntermediate::TRangeRead>(*intermediate->ReadCommand)) {
        auto &request = std::get<TIntermediate::TRangeRead>(*intermediate->ReadCommand);
        auto *response = intermediate->Response.AddReadRangeResult();
        ProcessCmd(request, response, nullptr, db, ctx, intermediate->Stat, 0, intermediate.Get());
    }
}

void TKeyValueState::CmdRename(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx) {
    ui64 unixTime = TAppData::TimeProvider->Now().Seconds();
    for (ui32 i = 0; i < intermediate->Renames.size(); ++i) {
        auto& request = intermediate->Renames[i];
        auto *response = intermediate->Response.AddRenameResult();
        ProcessCmd(request, response, nullptr, db, ctx, intermediate->Stat, unixTime, intermediate.Get());
    }
}

void TKeyValueState::CmdDelete(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx) {
    for (ui32 i = 0; i < intermediate->Deletes.size(); ++i) {
        auto& request = intermediate->Deletes[i];
        auto *response = intermediate->Response.AddDeleteRangeResult();
        ProcessCmd(request, response, nullptr, db, ctx, intermediate->Stat, 0, intermediate.Get());
    }
}

void TKeyValueState::CmdWrite(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx) {
    ui64 unixTime = TAppData::TimeProvider->Now().Seconds();
    for (ui32 i = 0; i < intermediate->Writes.size(); ++i) {
        auto& request = intermediate->Writes[i];
        auto *response = intermediate->Response.AddWriteResult();
        ProcessCmd(request, response, nullptr, db, ctx, intermediate->Stat, unixTime, intermediate.Get());
    }
    ResourceMetrics->TryUpdate(ctx);
}

void TKeyValueState::CmdGetStatus(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx) {
    Y_UNUSED(db);
    Y_UNUSED(ctx);
    for (ui32 i = 0; i < intermediate->GetStatuses.size(); ++i) {
        auto& request = intermediate->GetStatuses[i];
        if (intermediate->EvType == TEvKeyValue::TEvRequest::EventType) {
            auto& response = *intermediate->Response.AddGetStatusResult();

            response.SetStatus(request.Status);
            response.SetStorageChannel(request.StorageChannel);
            response.SetStatusFlags(request.StatusFlags.Raw);
        } else if ((intermediate->EvType == TEvKeyValue::TEvGetStorageChannelStatus::EventType)) {
            auto response = intermediate->GetStorageChannelStatusResponse.add_storage_channel();

            if (request.Status == NKikimrProto::OK) {
                response->set_status(NKikimrKeyValue::Statuses::RSTATUS_OK);
            } else if (request.Status == NKikimrProto::TIMEOUT) {
                response->set_status(NKikimrKeyValue::Statuses::RSTATUS_TIMEOUT);
            } else {
                response->set_status(NKikimrKeyValue::Statuses::RSTATUS_ERROR);
            }

            if (request.StorageChannel == NKikimrClient::TKeyValueRequest::INLINE) {
                response->set_storage_channel(1);
            } else {
                response->set_storage_channel(request.StorageChannel - BLOB_CHANNEL + MainStorageChannelInPublicApi);
            }

            response->set_status_flag(GetStatusFlag(request.StatusFlags));
        }
    }
    intermediate->GetStorageChannelStatusResponse.set_status(NKikimrKeyValue::Statuses::RSTATUS_OK);
}

void TKeyValueState::CmdCopyRange(THolder<TIntermediate>& intermediate, ISimpleDb& db, const TActorContext& ctx) {
    for (const auto& request : intermediate->CopyRanges) {
        auto *response = intermediate->Response.AddCopyRangeResult();
        ProcessCmd(request, response, nullptr, db, ctx, intermediate->Stat, 0, intermediate.Get());
    }
}

void TKeyValueState::CmdConcat(THolder<TIntermediate>& intermediate, ISimpleDb& db, const TActorContext& ctx) {
    ui64 unixTime = TAppData::TimeProvider->Now().Seconds();
    for (const auto& request : intermediate->Concats) {
        auto *response = intermediate->Response.AddConcatResult();
        ProcessCmd(request, response, nullptr, db, ctx, intermediate->Stat, unixTime, intermediate.Get());
    }
}

void TKeyValueState::CmdTrimLeakedBlobs(THolder<TIntermediate>& intermediate, ISimpleDb& db, const TActorContext& ctx) {
    if (intermediate->TrimLeakedBlobs) {
        auto& response = *intermediate->Response.MutableTrimLeakedBlobsResult();
        response.SetStatus(NKikimrProto::OK);
        ui32 numItems = 0;
        ui32 numUntrimmed = 0;
        for (const TLogoBlobID& id : intermediate->TrimLeakedBlobs->FoundBlobs) {
            auto it = RefCounts.find(id);
            if (it != RefCounts.end()) {
                Y_VERIFY(it->second != 0);
            } else if (!Trash.count(id)) { // we found a candidate for trash
                if (numItems < intermediate->TrimLeakedBlobs->MaxItemsToTrim) {
                    LOG_WARN_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " trimming " << id.ToString());
                    Trash.insert(id);
                    CountTrashRecord(id.BlobSize());
                    THelpers::DbUpdateTrash(id, db, ctx);
                    ++numItems;
                } else {
                    ++numUntrimmed;
                }
            }
        }
        response.SetNumItemsTrimmed(numItems);
        response.SetNumItemsLeft(numUntrimmed);
    }
}

void TKeyValueState::CmdSetExecutorFastLogPolicy(THolder<TIntermediate> &intermediate, ISimpleDb &/*db*/,
        const TActorContext &/*ctx*/) {
    if (intermediate->SetExecutorFastLogPolicy) {
        auto& response = *intermediate->Response.MutableSetExecutorFastLogPolicyResult();
        response.SetStatus(NKikimrProto::OK);
    }
}

void TKeyValueState::CmdCmds(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx) {
    ui64 unixTime = TAppData::TimeProvider->Now().Seconds();
    bool wasWrite = false;
    auto getChannel = [&](auto &cmd) -> NKikimrKeyValue::StorageChannel* {
        using Type = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<Type, TIntermediate::TWrite>) {
            if (intermediate->EvType != TEvKeyValue::TEvExecuteTransaction::EventType) {
                return nullptr;
            }
            ui32 storageChannel = MainStorageChannelInPublicApi;
            if (cmd.Status == NKikimrProto::SCHEDULED) {
                storageChannel = InlineStorageChannelInPublicApi;
            }
            if (cmd.LogoBlobIds.size()) {
                storageChannel = cmd.LogoBlobIds.front().Channel() - BLOB_CHANNEL + MainStorageChannelInPublicApi;
            }
            auto it = intermediate->Channels.find(storageChannel);
            if (it == intermediate->Channels.end()) {
                auto channel = intermediate->ExecuteTransactionResponse.add_storage_channel();
                intermediate->Channels.emplace(storageChannel, channel);
                return channel;
            }
            return it->second;
        }
        return nullptr;
    };
    auto getLegacyResponse = [&](auto &cmd) {
        using Type = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<Type, TIntermediate::TWrite>) {
            wasWrite = true;
            return intermediate->Response.AddWriteResult();
        }
        if constexpr (std::is_same_v<Type, TIntermediate::TDelete>) {
            return intermediate->Response.AddDeleteRangeResult();
        }
        if constexpr (std::is_same_v<Type, TIntermediate::TRename>) {
            return intermediate->Response.AddRenameResult();
        }
        if constexpr (std::is_same_v<Type, TIntermediate::TCopyRange>) {
            return intermediate->Response.AddCopyRangeResult();
        }
        if constexpr (std::is_same_v<Type, TIntermediate::TConcat>) {
            return intermediate->Response.AddConcatResult();
        }
    };
    auto process = [&](auto &cmd) {
        ProcessCmd(cmd, getLegacyResponse(cmd), getChannel(cmd), db, ctx, intermediate->Stat, unixTime, intermediate.Get());
    };
    for (auto &cmd : intermediate->Commands) {
        std::visit(process, cmd);
    }
    if (wasWrite) {
        ResourceMetrics->TryUpdate(ctx);
    }
}

TKeyValueState::TCheckResult TKeyValueState::CheckCmd(const TIntermediate::TCopyRange &cmd, TKeySet& keys,
        ui32 /*index*/) const
{
    TVector<TString> nkeys;
    auto range = GetRange(cmd.Range, keys);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->StartsWith(cmd.PrefixToRemove)) {
            nkeys.push_back(cmd.PrefixToAdd + it->substr(cmd.PrefixToRemove.size()));
        }
    }
    keys.insert(nkeys.begin(), nkeys.end());
    return {};
}

TKeyValueState::TCheckResult TKeyValueState::CheckCmd(const TIntermediate::TRename &cmd, TKeySet& keys,
        ui32 index) const
{
    auto it = keys.find(cmd.OldKey);
    if (it == keys.end()) {
        TStringStream str;
        str << "KeyValue# " << TabletId
            << " OldKey# " << EscapeC(cmd.OldKey) << " does not exist in CmdRename(" << index << ")"
            << " Marker# KV18";
        return {false, str.Str()};
    }
    keys.erase(it);
    keys.insert(cmd.NewKey);
    return {};
}

TKeyValueState::TCheckResult TKeyValueState::CheckCmd(const TIntermediate::TConcat &cmd, TKeySet& keys,
        ui32 index) const
{
    for (const TString& key : cmd.InputKeys) {
        auto it = keys.find(key);
        if (it == keys.end()) {
            TStringStream str;
            str << "KeyValue# " << TabletId
                << " InputKey# " << EscapeC(key) << " does not exist in CmdConcat(" << index << ")"
                << " Marker# KV19";
            return {false, str.Str()};
        }
        if (!cmd.KeepInputs) {
            keys.erase(it);
        }
    }

    keys.insert(cmd.OutputKey);
    return {};
}

TKeyValueState::TCheckResult TKeyValueState::CheckCmd(const TIntermediate::TDelete &cmd, TKeySet& keys,
        ui32 /*index*/) const
{
    auto r = GetRange(cmd.Range, keys);
    keys.erase(r.first, r.second);
    return {};
}

TKeyValueState::TCheckResult TKeyValueState::CheckCmd(const TIntermediate::TWrite &cmd, TKeySet& keys,
        ui32 /*index*/) const
{
    keys.insert(cmd.Key);
    return {};
}

bool TKeyValueState::CheckCmdCopyRanges(THolder<TIntermediate>& intermediate, const TActorContext& /*ctx*/,
        TKeySet& keys, const TTabletStorageInfo* /*info*/)
{
    for (const auto& cmd : intermediate->CopyRanges) {
        CheckCmd(cmd, keys, 0);
    }
    return true;
}

bool TKeyValueState::CheckCmdRenames(THolder<TIntermediate>& intermediate, const TActorContext& ctx, TKeySet& keys,
        const TTabletStorageInfo *info)
{
    ui32 index = 0;
    for (const auto& cmd : intermediate->Renames) {
        const auto &[ok, msg] = CheckCmd(cmd, keys, index++);
        if (!ok) {
            ReplyError(ctx, msg, NMsgBusProxy::MSTATUS_ERROR, NKikimrKeyValue::Statuses::RSTATUS_NOT_FOUND, intermediate, info);
            return false;
        }
    }
    return true;
}

bool TKeyValueState::CheckCmdConcats(THolder<TIntermediate>& intermediate, const TActorContext& ctx, TKeySet& keys,
        const TTabletStorageInfo *info)
{
    ui32 index = 0;
    for (const auto& cmd : intermediate->Concats) {
        const auto &[ok, msg] = CheckCmd(cmd, keys, index++);
        if (!ok) {
            ReplyError(ctx, msg, NMsgBusProxy::MSTATUS_ERROR, NKikimrKeyValue::Statuses::RSTATUS_NOT_FOUND, intermediate, info);
            return false;
        }
    }

    return true;
}

bool TKeyValueState::CheckCmdDeletes(THolder<TIntermediate>& intermediate, const TActorContext& /*ctx*/, TKeySet& keys,
        const TTabletStorageInfo* /*info*/)
{
    for (const auto& cmd : intermediate->Deletes) {
        CheckCmd(cmd, keys, 0);
    }
    return true;
}

bool TKeyValueState::CheckCmdWrites(THolder<TIntermediate>& intermediate, const TActorContext& /*ctx*/, TKeySet& keys,
        const TTabletStorageInfo* /*info*/)
{
    for (const auto& cmd : intermediate->Writes) {
        CheckCmd(cmd, keys, 0);
    }
    return true;
}

bool TKeyValueState::CheckCmdGetStatus(THolder<TIntermediate>& /*intermediate*/, const TActorContext& /*ctx*/,
        TKeySet& /*keys*/, const TTabletStorageInfo* /*info*/)
{
    return true;
}

bool TKeyValueState::CheckCmds(THolder<TIntermediate>& intermediate, const TActorContext& ctx, TKeySet& keys,
        const TTabletStorageInfo* info)
{
    ui32 renameIndex = 0;
    ui32 concatIndex = 0;

    auto nextIdx = [&](auto &cmd) -> ui32 {
        using Type = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<Type, TIntermediate::TRename>) {
            return renameIndex++;
        }
        if constexpr (std::is_same_v<Type, TIntermediate::TConcat>) {
            return concatIndex++;
        }
        return 0;
    };
    auto visitor = [&](auto &cmd) {
        return CheckCmd(cmd, keys, nextIdx(cmd));
    };

    for (const auto& cmd : intermediate->Commands) {
        const auto &[ok, msg] = std::visit(visitor, cmd);
        if (!ok) { 
            ReplyError(ctx, msg, NMsgBusProxy::MSTATUS_ERROR, NKikimrKeyValue::Statuses::RSTATUS_NOT_FOUND, intermediate, info);
            return false;
        }
    }
    return true;
}

void TKeyValueState::ProcessCmds(THolder<TIntermediate> &intermediate, ISimpleDb &db, const TActorContext &ctx,
        const TTabletStorageInfo *info) {
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " TTxRequest ProcessCmds");

    TKeySet keys(Index);

    bool success = true;

    if (intermediate->HasGeneration && intermediate->Generation != StoredState.GetUserGeneration()) {
        TStringStream str;
        str << "KeyValue# " << TabletId
            << " Generation changed during command execution, aborted"
            << " Marker# KV04";
        ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_REJECTED, NKikimrKeyValue::Statuses::RSTATUS_WRONG_LOCK_GENERATION, intermediate, info);
        success = false;
    }

    success = success && CheckCmdCopyRanges(intermediate, ctx, keys, info);
    success = success && CheckCmdRenames(intermediate, ctx, keys, info);
    success = success && CheckCmdConcats(intermediate, ctx, keys, info);
    success = success && CheckCmdDeletes(intermediate, ctx, keys, info);
    success = success && CheckCmdWrites(intermediate, ctx, keys, info);
    success = success && CheckCmds(intermediate, ctx, keys, info);
    success = success && CheckCmdGetStatus(intermediate, ctx, keys, info);
    if (!success) {
        for (const auto& [logoBlobId, initial] : std::exchange(intermediate->RefCountsIncr, {})) {
            Dereference(logoBlobId, db, ctx, initial);
        }
    } else {
        // Read + validate
        CmdRead(intermediate, db, ctx);
        CmdReadRange(intermediate, db, ctx);
        CmdCopyRange(intermediate, db, ctx);
        CmdRename(intermediate, db, ctx);

        // All reads done
        CmdConcat(intermediate, db, ctx);
        CmdDelete(intermediate, db, ctx);
        CmdWrite(intermediate, db, ctx);
        CmdGetStatus(intermediate, db, ctx);
        CmdCmds(intermediate, db, ctx);

        // Blob trimming
        CmdTrimLeakedBlobs(intermediate, db, ctx);

        CmdSetExecutorFastLogPolicy(intermediate, db, ctx);
    }

    // Safe to change the internal state and prepare the response
    intermediate->Response.SetStatus(NMsgBusProxy::MSTATUS_OK);

    if (intermediate->Stat.RequestType == TRequestType::ReadOnly ||
            intermediate->Stat.RequestType == TRequestType::ReadOnlyInline) {
        Reply(intermediate, ctx, info);
    }
}

bool TKeyValueState::IncrementGeneration(THolder<TIntermediate> &intermediate, ISimpleDb &db,
        const TActorContext &ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " TTxRequest IncrementGeneration");

    ui64 nextGeneration = StoredState.GetUserGeneration() + 1;
    Y_VERIFY(nextGeneration > StoredState.GetUserGeneration());
    StoredState.SetUserGeneration(nextGeneration);

    THelpers::DbUpdateState(StoredState, db, ctx);

    intermediate->Response.MutableIncrementGenerationResult()->SetGeneration(nextGeneration);
    intermediate->Response.SetStatus(NMsgBusProxy::MSTATUS_OK);

    return true;
}

void TKeyValueState::Dereference(const TIndexRecord& record, ISimpleDb& db, const TActorContext& ctx) {
    for (const TIndexRecord::TChainItem& item : record.Chain) {
        if (!item.IsInline()) {
            Dereference(item.LogoBlobId, db, ctx, false);
        }
    }
}

void TKeyValueState::Dereference(const TLogoBlobID& id, ISimpleDb& db, const TActorContext& ctx, bool initial) {
    auto it = RefCounts.find(id);
    Y_VERIFY(it != RefCounts.end());
    --it->second;
    if (!it->second) {
        RefCounts.erase(it);
        Trash.insert(id);
        THelpers::DbUpdateTrash(id, db, ctx);
        if (initial) {
            CountInitialTrashRecord(id.BlobSize());
        } else {
            CountTrashRecord(id.BlobSize());
        }
        const ui8 channel = id.Channel();
        Y_VERIFY(channel < ChannelDataUsage.size());
        ChannelDataUsage[channel] -= id.BlobSize();
    }
}

void TKeyValueState::UpdateKeyValue(const TString& key, const TIndexRecord& record, ISimpleDb& db,
        const TActorContext& ctx) {
    TString value = record.Serialize();
    THelpers::DbUpdateUserKeyValue(key, value, db, ctx);
}

void TKeyValueState::OnPeriodicRefresh(const TActorContext &ctx) {
    Y_UNUSED(ctx);
    TInstant now = TAppData::TimeProvider->Now();
    TInstant oldestInstant = now;
    for (const auto &requestInputTime : RequestInputTime) {
        oldestInstant = Min(oldestInstant, requestInputTime.second);
    }
    TDuration maxDuration = now - oldestInstant;
    TabletCounters->Simple()[COUNTER_REQ_AGE_MS].Set(maxDuration.MilliSeconds());
}

void TKeyValueState::OnUpdateWeights(TChannelBalancer::TEvUpdateWeights::TPtr ev) {
    WeightManager = std::move(ev->Get()->WeightManager);
}

void TKeyValueState::OnRequestComplete(ui64 requestUid, ui64 generation, ui64 step, const TActorContext &ctx,
        const TTabletStorageInfo *info, NMsgBusProxy::EResponseStatus status, const TRequestStat &stat) {
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
        << " OnRequestComplete uid# " << requestUid << " generation# " << generation
        << " step# " << step << " ChannelGeneration# " << StoredState.GetChannelGeneration()
        << " ChannelStep# " << StoredState.GetChannelStep());

    CountLatencyBsOps(stat);

    RequestInputTime.erase(requestUid);

    if (stat.RequestType != TRequestType::WriteOnly) {
        if (stat.RequestType == TRequestType::ReadOnlyInline) {
            RoInlineIntermediatesInFlight--;
        } else {
            IntermediatesInFlight--;
        }
    }

    CountRequestComplete(status, stat, ctx);
    ResourceMetrics->TryUpdate(ctx);

    if (Queue.size() && IntermediatesInFlight < IntermediatesInFlightLimit) {
        TRequestType::EType requestType = Queue.front()->Stat.RequestType;

        CountLatencyQueue(Queue.front()->Stat);

        ProcessPostponedIntermediate(ctx, std::move(Queue.front()), info);
        Queue.pop_front();
        ++IntermediatesInFlight;

        CountRequestTakeOffOrEnqueue(requestType);
    }

    if (StoredState.GetChannelGeneration() == generation) {
        auto it = InFlightForStep.find(step);
        Y_VERIFY(it != InFlightForStep.end(), "Unexpected step# %" PRIu64, (ui64)step);
        it->second--;
        if (it->second == 0) {
            InFlightForStep.erase(it);

            // Initiate Garbage collection process if needed
           StartCollectingIfPossible(ctx);
        }
    }
}

bool TKeyValueState::CheckDeadline(const TActorContext &ctx, NKikimrClient::TKeyValueRequest &kvRequest,
        THolder<TIntermediate> &intermediate) {
    if (kvRequest.HasDeadlineInstantMs()) {
        TInstant now = TAppData::TimeProvider->Now();
        intermediate->Deadline = TInstant::MicroSeconds(kvRequest.GetDeadlineInstantMs() * 1000ull);

        if (intermediate->Deadline <= now) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Deadline reached before processing the request!";
            str << " DeadlineInstantMs# " << (ui64)kvRequest.GetDeadlineInstantMs();
            str << " < Now# " << (ui64)now.MilliSeconds();
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_TIMEOUT, NKikimrKeyValue::Statuses::RSTATUS_TIMEOUT, intermediate);
            return true;
        }
    }
    return false;
}

bool TKeyValueState::CheckGeneration(const TActorContext &ctx, NKikimrClient::TKeyValueRequest &kvRequest,
        THolder<TIntermediate> &intermediate) {
    if (kvRequest.HasGeneration()) {
        intermediate->HasGeneration = true;
        intermediate->Generation = kvRequest.GetGeneration();
        if (kvRequest.GetGeneration() != StoredState.GetUserGeneration()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Generation mismatch! Requested# " << kvRequest.GetGeneration();
            str << " Actual# " << StoredState.GetUserGeneration();
            str << " Marker# KV01";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_REJECTED, NKikimrKeyValue::Statuses::RSTATUS_WRONG_LOCK_GENERATION, intermediate);
            return true;
        }
    } else {
        intermediate->HasGeneration = false;
    }
    return false;
}


template <typename TypeWithPriority>
void SetPriority(NKikimrBlobStorage::EGetHandleClass *outHandleClass, ui8 priority) {
    *outHandleClass = NKikimrBlobStorage::FastRead;
    if constexpr (std::is_same_v<TypeWithPriority, NKikimrKeyValue::Priorities>) {
        switch (priority) {
            case TypeWithPriority::PRIORITY_UNSPECIFIED:
            case TypeWithPriority::PRIORITY_REALTIME:
                *outHandleClass = NKikimrBlobStorage::FastRead;
                break;
            case TypeWithPriority::PRIORITY_BACKGROUND:
                *outHandleClass = NKikimrBlobStorage::AsyncRead;
                break;
        }
    } else {
        switch (priority) {
            case TypeWithPriority::REALTIME:
                *outHandleClass = NKikimrBlobStorage::FastRead;
                break;
            case TypeWithPriority::BACKGROUND:
                *outHandleClass = NKikimrBlobStorage::AsyncRead;
                break;
        }
    }
}

template <typename TypeWithPriority, bool WithOverrun = false, ui64 SpecificReadResultSizeEstimation=ReadResultSizeEstimation>
bool PrepareOneRead(const TString &key, TIndexRecord &indexRecord, ui64 offset, ui64 size, ui8 priority,
        ui64 cmdLimitBytes, THolder<TIntermediate> &intermediate, TIntermediate::TRead &response, bool &outIsInlineOnly)
{
    for (ui64 idx = 0; idx < indexRecord.Chain.size(); ++idx) {
        if (!indexRecord.Chain[idx].IsInline()) {
            outIsInlineOnly = false;
            break;
        }
    }

    if (!size) {
        size = std::numeric_limits<decltype(size)>::max();
    }
    ui64 fullValueSize = indexRecord.GetFullValueSize();
    offset = std::min(offset, fullValueSize);
    size = std::min(size, fullValueSize - offset);
    ui64 metaDataSize = key.size() + SpecificReadResultSizeEstimation;
    ui64 recSize = std::max(size, ErrorMessageSizeEstimation) + metaDataSize;

    response.RequestedSize = size;
    bool isOverRun = false;

    if (intermediate->IsTruncated
            || intermediate->TotalSize + recSize > intermediate->TotalSizeLimit
            || (cmdLimitBytes && intermediate->TotalSize + recSize > cmdLimitBytes)) {
        response.Status = NKikimrProto::OVERRUN;
        if (!WithOverrun
                || std::min(intermediate->TotalSizeLimit, cmdLimitBytes) < intermediate->TotalSize + metaDataSize)
        {
            return true;
        }
        if (cmdLimitBytes) {
            size = std::min(intermediate->TotalSizeLimit, cmdLimitBytes) - intermediate->TotalSize - metaDataSize;
        } else {
            size = intermediate->TotalSizeLimit - intermediate->TotalSize - metaDataSize;
        }
        isOverRun = true;
    }

    response.ValueSize = size;
    response.CreationUnixTime = indexRecord.CreationUnixTime;
    response.Key = key;

    SetPriority<TypeWithPriority>(&response.HandleClass, priority);

    if (size) {
        const ui32 numReads = indexRecord.GetReadItems(offset, size, response);
        intermediate->TotalSize += recSize;
        intermediate->TotalReadsScheduled += numReads;
    } else if (response.Status != NKikimrProto::OVERRUN) {
        response.Status = NKikimrProto::OK;
    }
    return isOverRun;
}

template <typename TypeWithPriority, ui64 SpecificKeyValuePairSizeEstimation>
bool PrepareOneReadFromRangeReadWithoutData(const TString &key, TIndexRecord &indexRecord, ui8 priority,
        THolder<TIntermediate> &intermediate, TIntermediate::TRangeRead &response,
        ui64 &cmdSizeBytes, ui64 cmdLimitBytes, bool *outIsInlineOnly)
{
    if (intermediate->IsTruncated) {
        return false;
    }

    NKikimrClient::TKeyValueRequest::EStorageChannel storageChannel =
        NKikimrClient::TKeyValueRequest::MAIN;
    if (indexRecord.Chain.size()) {
        if (indexRecord.Chain[0].IsInline()) {
            storageChannel = NKikimrClient::TKeyValueRequest::INLINE;
        } else {
            *outIsInlineOnly = false;
            ui32 storageChannelIdx = indexRecord.Chain[0].LogoBlobId.Channel();
            ui32 storageChannelOffset = storageChannelIdx - BLOB_CHANNEL;
            storageChannel = (NKikimrClient::TKeyValueRequest::EStorageChannel)storageChannelOffset;
        }
    }

    ui64 metadataSize = key.size() + SpecificKeyValuePairSizeEstimation;
    if (intermediate->TotalSize + metadataSize > intermediate->TotalSizeLimit
            || cmdSizeBytes + metadataSize > cmdLimitBytes) {
        STLOG(NLog::PRI_TRACE, NKikimrServices::KEYVALUE, KV330, "Went beyond limits",
                (intermediate->TotalSize + metadataSize, intermediate->TotalSize + metadataSize),
                (intermediate->TotalSizeLimit, intermediate->TotalSizeLimit),
                (cmdSizeBytes + metadataSize, cmdSizeBytes + metadataSize),
                (cmdLimitBytes, cmdLimitBytes));
        return true;
    }
    response.Reads.emplace_back(key, indexRecord.GetFullValueSize(), indexRecord.CreationUnixTime,
            storageChannel);
    intermediate->TotalSize += metadataSize;
    SetPriority<TypeWithPriority>(&response.HandleClass, priority);

    cmdSizeBytes += metadataSize;
    return false;
}

struct TSeqInfo {
    ui32 Reads = 0;
    ui32 RunLen = 0;
    ui32 Generation = 0;
    ui32 Step = 0;
    ui32 Cookie = 0;
};

template <typename TypeWithPriority, ui64 SpecificKeyValuePairSizeEstimation>
bool PrepareOneReadFromRangeReadWithData(const TString &key, TIndexRecord &indexRecord, ui8 priority,
        THolder<TIntermediate> &intermediate, TIntermediate::TRangeRead &response,
        ui64 &cmdSizeBytes, ui64 cmdLimitBytes, TSeqInfo &seq, bool *outIsInlineOnly)
{
    if (intermediate->IsTruncated) {
        return false;
    }

    NKikimrClient::TKeyValueRequest::EStorageChannel storageChannel =
        NKikimrClient::TKeyValueRequest::MAIN;
    if (indexRecord.Chain.size()) {
        if (indexRecord.Chain[0].IsInline()) {
            storageChannel = NKikimrClient::TKeyValueRequest::INLINE;
        } else {
            *outIsInlineOnly = false;
            ui32 storageChannelIdx = indexRecord.Chain[0].LogoBlobId.Channel();
            ui32 storageChannelOffset = storageChannelIdx - BLOB_CHANNEL;
            storageChannel = (NKikimrClient::TKeyValueRequest::EStorageChannel)storageChannelOffset;
        }
    }

    bool isSeq = false;
    bool isInline = false;
    if (indexRecord.Chain.size() == 1) {
        if (indexRecord.Chain.front().IsInline()) {
            isSeq = true;
            isInline = true;
        } else {
            const TLogoBlobID& id = indexRecord.Chain.front().LogoBlobId;
            isSeq = id.Generation() == seq.Generation
                && id.Step() == seq.Step
                && id.Cookie() == seq.Cookie;
            seq.Generation = id.Generation();
            seq.Step = id.Step();
            seq.Cookie = id.Cookie() + 1;
        }
    }
    if (isSeq) {
        seq.Reads++;
        if (seq.Reads > intermediate->SequentialReadLimit && !isInline) {
            isSeq = false;
        } else {
            ++seq.RunLen;
        }
    }
    if (!isSeq) {
        seq.RunLen = 1;
    }

    ui64 valueSize = indexRecord.GetFullValueSize();
    ui64 metadataSize = key.size() + SpecificKeyValuePairSizeEstimation;
    if (intermediate->TotalSize + valueSize + metadataSize > intermediate->TotalSizeLimit
            || cmdSizeBytes + valueSize + metadataSize > cmdLimitBytes
            || (seq.RunLen == 1 && intermediate->TotalReadsScheduled >= intermediate->TotalReadsLimit)) {
        return true;
    }

    TIntermediate::TRead read(key, valueSize, indexRecord.CreationUnixTime, storageChannel);
    const ui32 numReads = indexRecord.GetReadItems(0, valueSize, read);
    SetPriority<TypeWithPriority>(&response.HandleClass, priority);
    SetPriority<TypeWithPriority>(&read.HandleClass, priority);

    response.Reads.push_back(std::move(read));

    intermediate->TotalSize += valueSize + metadataSize;
    intermediate->TotalReadsScheduled += numReads;

    cmdSizeBytes += valueSize + metadataSize;
    return false;
}

bool TKeyValueState::PrepareCmdRead(const TActorContext &ctx, NKikimrClient::TKeyValueRequest &kvRequest,
        THolder<TIntermediate> &intermediate, bool &outIsInlineOnly) {
    outIsInlineOnly = true;
    intermediate->Reads.resize(kvRequest.CmdReadSize());
    for (ui32 i = 0; i < kvRequest.CmdReadSize(); ++i) {
        auto &request = kvRequest.GetCmdRead(i);
        auto &response = intermediate->Reads[i];

        if (!request.HasKey()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Missing Key in CmdRead(" << (ui32)i << ") Marker# KV02";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }

        ui64 offset = request.HasOffset() ? request.GetOffset() : 0;
        ui64 size = request.HasSize() ? request.GetSize() : 0;
        NKikimrClient::TKeyValueRequest::EPriority priority = NKikimrClient::TKeyValueRequest::REALTIME;
        if (request.HasPriority()) {
            priority = request.GetPriority();
        }

        auto it = Index.find(request.GetKey());
        if (it == Index.end()) {
            response.Status = NKikimrProto::NODATA;
            response.Message = "No such key Marker# KV48";
        } else {
            bool isOverrun = PrepareOneRead<NKikimrClient::TKeyValueRequest>(it->first, it->second, offset, size,
                    priority, 0, intermediate, response, outIsInlineOnly);
            if (isOverrun) {
                if (!intermediate->IsTruncated) {
                    CountOverrun();
                    intermediate->IsTruncated = true;
                }
            }
        }
    }
    return false;
}

template <typename TypeWithPriority, bool CheckUTF8 = false,
        ui64 MetaDataSizeWithData = KeyValuePairSizeEstimation,
        ui64 MetaDataSizeWithoutData = KeyValuePairSizeEstimation>
void ProcessOneCmdReadRange(TKeyValueState *self, const TKeyRange &range, ui64 cmdLimitBytes, bool includeData,
        ui8 priority, TIntermediate::TRangeRead &response, THolder<TIntermediate> &intermediate, bool *outIsInlineOnly)
{
    ui64 cmdSizeBytes = 0;
    TSeqInfo seq;
    seq.RunLen = 1;

    self->TraverseRange(range, [&](TKeyValueState::TIndex::iterator it) {
        if (intermediate->IsTruncated) {
            return;
        }

        auto &[key, indexRecord] = *it;

        if (CheckUTF8 && !IsUtf(key)) {
            TIntermediate::TRead read;
            read.CreationUnixTime = indexRecord.CreationUnixTime;
            EscapeC(key, read.Key);
            read.Status = NKikimrProto::ERROR;
            read.Message = "Key isn't UTF8";
            response.Reads.push_back(std::move(read));
            return;
        }

        bool isOverRun = false;
        if (includeData) {
            isOverRun = PrepareOneReadFromRangeReadWithData<NKikimrClient::TKeyValueRequest, MetaDataSizeWithData>(
                    key, indexRecord, priority, intermediate, response,
                    cmdSizeBytes, cmdLimitBytes, seq, outIsInlineOnly);
        } else {
            isOverRun = PrepareOneReadFromRangeReadWithoutData<NKikimrClient::TKeyValueRequest, MetaDataSizeWithoutData>(
                    key, indexRecord, priority, intermediate, response, cmdSizeBytes,
                    cmdLimitBytes, outIsInlineOnly);
        }
        if (isOverRun) {
            self->CountOverrun();
            intermediate->IsTruncated = true;
        }
    });

    if (intermediate->IsTruncated) {
        response.Status = NKikimrProto::OVERRUN;
    } else if (response.Reads.size() == 0) {
        response.Status = NKikimrProto::NODATA;
    }
}

bool TKeyValueState::PrepareCmdReadRange(const TActorContext &ctx, NKikimrClient::TKeyValueRequest &kvRequest,
        THolder<TIntermediate> &intermediate, bool &inOutIsInlineOnly) {
    intermediate->RangeReads.resize(kvRequest.CmdReadRangeSize());
    for (ui32 i = 0; i < kvRequest.CmdReadRangeSize(); ++i) {
        auto &request = kvRequest.GetCmdReadRange(i);
        auto &response = intermediate->RangeReads[i];

        if (!request.HasRange()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Missing Range in CmdReadRange(" << i << ") Marker# KV03";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }

        TKeyRange range;
        if (!ConvertRange(request.GetRange(), &range, ctx, intermediate, "CmdReadRange", i)) {
            return true;
        }

        ui64 cmdLimitBytes = request.HasLimitBytes() ? request.GetLimitBytes() : Max<ui64>();
        bool includeData = request.HasIncludeData() && request.GetIncludeData();
        ui8 priority = request.HasPriority() ? request.GetPriority() : Max<ui8>();
        response.IncludeData = includeData;
        response.LimitBytes = cmdLimitBytes;

        ProcessOneCmdReadRange<NKikimrClient::TKeyValueRequest>(this, range, cmdLimitBytes, includeData, priority,
                response, intermediate, &inOutIsInlineOnly);
    }
    return false;
}

bool TKeyValueState::PrepareCmdRename(const TActorContext &ctx, NKikimrClient::TKeyValueRequest &kvRequest,
        THolder<TIntermediate> &intermediate) {
    for (ui32 i = 0; i < kvRequest.CmdRenameSize(); ++i) {
        auto& request = kvRequest.GetCmdRename(i);
        intermediate->Commands.emplace_back(TIntermediate::TRename());
        auto& interm = std::get<TIntermediate::TRename>(intermediate->Commands.back());

        if (!request.HasOldKey()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Missing OldKey in CmdRename(" << i << ") Marker# KV06";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }
        if (!request.HasNewKey()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Missing NewKey in CmdRename(" << i << ") Marker# KV07";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }

        interm.OldKey = request.GetOldKey();
        interm.NewKey = request.GetNewKey();
    }
    return false;
}

bool TKeyValueState::PrepareCmdDelete(const TActorContext &ctx, NKikimrClient::TKeyValueRequest &kvRequest,
        THolder<TIntermediate> &intermediate) {
    ui64 nToDelete = 0;
    for (ui32 i = 0; i < kvRequest.CmdDeleteRangeSize(); ++i) {
        auto& request = kvRequest.GetCmdDeleteRange(i);
        intermediate->Commands.emplace_back(TIntermediate::TDelete());
        auto& interm = std::get<TIntermediate::TDelete>(intermediate->Commands.back());

        if (!request.HasRange()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Missing Range in CmdDelete(" << i << ") Marker# KV08";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }
        if (!ConvertRange(request.GetRange(), &interm.Range, ctx, intermediate, "CmdDelete", i)) {
            return true;
        }
        TraverseRange(interm.Range, [&](TIndex::iterator it) {
                Y_UNUSED(it);
                nToDelete++;
            });
        // The use of >, not >= is important here.
        if (nToDelete > DeletesPerRequestLimit && !AppData(ctx)->AllowHugeKeyValueDeletes) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Can't delete Range, in CmdDelete(" << i << "), total limit of deletions per request ("
                << DeletesPerRequestLimit << ") reached, Marker# KV32";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }
    }
    return false;
}

void TKeyValueState::SplitIntoBlobs(TIntermediate::TWrite &cmd, bool isInline, ui32 storageChannelIdx,
        TIntermediate *intermediate) {
    if (isInline) {
        cmd.Status = NKikimrProto::SCHEDULED;
        cmd.StatusFlags = TStorageStatusFlags(ui32(NKikimrBlobStorage::StatusIsValid));
        if (GetIsTabletYellowMove()) {
            cmd.StatusFlags.Merge(ui32(NKikimrBlobStorage::StatusDiskSpaceLightYellowMove));
        }
        if (GetIsTabletYellowStop()) {
            cmd.StatusFlags.Merge(ui32(NKikimrBlobStorage::StatusDiskSpaceYellowStop));
        }
    } else {
        cmd.Status = NKikimrProto::UNKNOWN;
        ui64 sizeRemain = cmd.Data.size();
        while (sizeRemain) {
            ui32 blobSize = Min<ui64>(sizeRemain, 8 << 20);
            cmd.LogoBlobIds.push_back(AllocateLogoBlobId(blobSize, storageChannelIdx));
            sizeRemain -= blobSize;
        }
        for (const TLogoBlobID& logoBlobId : cmd.LogoBlobIds) {
            ui32 newRefCount = ++RefCounts[logoBlobId];
            Y_VERIFY(newRefCount == 1);
            intermediate->RefCountsIncr.emplace_back(logoBlobId, true);
        }
    }
}

bool TKeyValueState::PrepareCmdWrite(const TActorContext &ctx, NKikimrClient::TKeyValueRequest &kvRequest,
        THolder<TIntermediate> &intermediate, const TTabletStorageInfo *info) {
    intermediate->WriteIndices.reserve(kvRequest.CmdWriteSize());
    for (ui32 i = 0; i < kvRequest.CmdWriteSize(); ++i) {
        auto& request = kvRequest.GetCmdWrite(i);
        intermediate->WriteIndices.push_back(intermediate->Commands.size());
        auto& cmd = intermediate->Commands.emplace_back(TIntermediate::TWrite());
        auto& interm = std::get<TIntermediate::TWrite>(cmd);

        if (!request.HasKey()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Missing Key in CmdWrite(" << i << ") Marker# KV09";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }
        if (!request.HasValue()) {
            TStringStream str;
            str << "KeyValue# " << TabletId;
            str << " Missing Value in CmdWrite(" << i << ") Marker# KV10";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }

        bool isInline = false;
        ui32 storageChannelIdx = BLOB_CHANNEL;
        if (request.HasStorageChannel()) {
            auto storageChannel = request.GetStorageChannel();
            ui32 storageChannelOffset = (ui32)storageChannel;
            if (storageChannelOffset == NKikimrClient::TKeyValueRequest::INLINE) {
                isInline = true;
            } else {
                storageChannelIdx = storageChannelOffset + BLOB_CHANNEL;
                ui32 endChannel = info->Channels.size();
                if (storageChannelIdx >= endChannel) {
                    storageChannelIdx = BLOB_CHANNEL;
                    LOG_INFO_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                            << " CmdWrite StorageChannel# " << storageChannelOffset
                            << " does not exist, using MAIN");
                }
            }
        } else if (request.AutoselectChannelSize() && WeightManager) {
            std::bitset<256> enabled;
            for (const auto& channel : request.GetAutoselectChannel()) {
                if (channel != NKikimrClient::TKeyValueRequest::INLINE) {
                    const ui32 index = channel + BLOB_CHANNEL;
                    if (index < info->Channels.size()) {
                        Y_VERIFY(index < enabled.size());
                        enabled.set(index);
                    }
                }
            }
            // TODO(alexvru): trim enabled channels by used space
            if (enabled.any()) {
                int index = WeightManager->Pick(enabled);
                if (index != -1) {
                    storageChannelIdx = index;
                }
            }
        }

        interm.Key = request.GetKey();
        interm.Data = request.GetValue();
        interm.Tactic = TEvBlobStorage::TEvPut::TacticDefault;
        switch (request.GetTactic()) {
            case NKikimrClient::TKeyValueRequest::MIN_LATENCY:
                interm.Tactic = TEvBlobStorage::TEvPut::TacticMinLatency;
            break;
            case NKikimrClient::TKeyValueRequest::MAX_THROUGHPUT:
                interm.Tactic = TEvBlobStorage::TEvPut::TacticMaxThroughput;
            break;
        }
        interm.HandleClass = NKikimrBlobStorage::UserData;
        if (request.HasPriority()) {
            switch (request.GetPriority()) {
                case NKikimrClient::TKeyValueRequest::REALTIME:
                    interm.HandleClass = NKikimrBlobStorage::UserData;
                    break;
                case NKikimrClient::TKeyValueRequest::BACKGROUND:
                    interm.HandleClass = NKikimrBlobStorage::AsyncBlob;
                    break;
            }
        }
        SplitIntoBlobs(interm, isInline, storageChannelIdx, intermediate.Get());
    }
    return false;
}


TKeyValueState::TPrepareResult TKeyValueState::InitGetStatusCommand(TIntermediate::TGetStatus &cmd,
        NKikimrClient::TKeyValueRequest::EStorageChannel storageChannel, const TTabletStorageInfo *info)
{
    TString msg;
    if (storageChannel == NKikimrClient::TKeyValueRequest::INLINE) {
        cmd.StorageChannel = storageChannel;
        cmd.LogoBlobId = TLogoBlobID();
        cmd.Status = NKikimrProto::OK;
        cmd.StatusFlags = TStorageStatusFlags(ui32(NKikimrBlobStorage::StatusIsValid));
        if (GetIsTabletYellowMove()) {
            cmd.StatusFlags.Merge(ui32(NKikimrBlobStorage::StatusDiskSpaceLightYellowMove));
        }
        if (GetIsTabletYellowStop()) {
            cmd.StatusFlags.Merge(ui32(NKikimrBlobStorage::StatusDiskSpaceYellowStop));
        }
    } else {
        ui32 storageChannelOffset = (ui32)storageChannel;
        ui32 storageChannelIdx = storageChannelOffset + BLOB_CHANNEL;
        ui32 endChannel = info->Channels.size();
        if (storageChannelIdx >= endChannel) {
            storageChannelIdx = BLOB_CHANNEL;
            msg = TStringBuilder() << "KeyValue# " << TabletId
                    << " CmdGetStatus StorageChannel# " << storageChannelOffset
                    << " does not exist, using MAIN";
        }

        cmd.StorageChannel = storageChannel;
        cmd.LogoBlobId = AllocateLogoBlobId(1, storageChannelIdx);
        cmd.Status = NKikimrProto::UNKNOWN;
    }
    return {false, msg};
}

bool TKeyValueState::PrepareCmdGetStatus(const TActorContext &ctx, NKikimrClient::TKeyValueRequest &kvRequest,
        THolder<TIntermediate> &intermediate, const TTabletStorageInfo *info) {
    intermediate->GetStatuses.resize(kvRequest.CmdGetStatusSize());
    for (ui32 i = 0; i < kvRequest.CmdGetStatusSize(); ++i) {
        auto& request = kvRequest.GetCmdGetStatus(i);
        auto& interm = intermediate->GetStatuses[i];

        NKikimrClient::TKeyValueRequest::EStorageChannel storageChannel =
            NKikimrClient::TKeyValueRequest::MAIN;

        if (request.HasStorageChannel()) {
            storageChannel = request.GetStorageChannel();
        }
        TPrepareResult result = InitGetStatusCommand(interm, storageChannel, info);
        if (result.ErrorMsg && !result.WithError) {
            LOG_INFO_S(ctx, NKikimrServices::KEYVALUE, result.ErrorMsg  << " Marker# KV76");
        }
    }
    return false;
}


bool TKeyValueState::PrepareCmdCopyRange(const TActorContext& ctx, NKikimrClient::TKeyValueRequest& kvRequest,
        THolder<TIntermediate>& intermediate) {
    for (ui32 i = 0; i < kvRequest.CmdCopyRangeSize(); ++i) {
        auto& request = kvRequest.GetCmdCopyRange(i);
        intermediate->Commands.emplace_back(TIntermediate::TCopyRange());
        auto& interm = std::get<TIntermediate::TCopyRange>(intermediate->Commands.back());

        if ((!request.HasPrefixToAdd() || request.GetPrefixToAdd().empty()) &&
                (!request.HasPrefixToRemove() || request.GetPrefixToRemove().empty())) {
            TStringStream str;
            str << "KeyValue# " << TabletId
                << " Missing or empty both PrefixToAdd and PrefixToRemove in CmdCopyRange(" << i << ") Marker# KV11";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }
        if (!request.HasRange()) {
            interm.Range = TKeyRange::WholeDatabase();
        } else if (!ConvertRange(request.GetRange(), &interm.Range, ctx, intermediate, "CmdCopyRange", i)) {
            return false;
        }
        interm.PrefixToAdd = request.HasPrefixToAdd() ? request.GetPrefixToAdd() : TString();
        interm.PrefixToRemove = request.HasPrefixToRemove() ? request.GetPrefixToRemove() : TString();
    }
    return false;
}

bool TKeyValueState::PrepareCmdConcat(const TActorContext& ctx, NKikimrClient::TKeyValueRequest& kvRequest,
        THolder<TIntermediate>& intermediate) {
    for (ui32 i = 0; i < kvRequest.CmdConcatSize(); ++i) {
        auto& request = kvRequest.GetCmdConcat(i);
        intermediate->Commands.emplace_back(TIntermediate::TConcat());
        auto& interm = std::get<TIntermediate::TConcat>(intermediate->Commands.back());

        if (!request.HasOutputKey()) {
            TStringStream str;
            str << "KeyValue# " << TabletId << " Missing OutputKey in CmdConcat(" << i << ") Marker# KV12";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_INTERNALERROR, NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate);
            return true;
        }

        const auto& inputKeys = request.GetInputKeys();
        interm.InputKeys = {inputKeys.begin(), inputKeys.end()};
        interm.OutputKey = request.GetOutputKey();
        interm.KeepInputs = request.HasKeepInputs() && request.GetKeepInputs();
    }
    return false;
}

bool TKeyValueState::PrepareCmdTrimLeakedBlobs(const TActorContext& /*ctx*/,
        NKikimrClient::TKeyValueRequest& kvRequest, THolder<TIntermediate>& intermediate,
        const TTabletStorageInfo *info) {
    if (kvRequest.HasCmdTrimLeakedBlobs()) {
        const auto& request = kvRequest.GetCmdTrimLeakedBlobs();
        TIntermediate::TTrimLeakedBlobs interm;
        interm.MaxItemsToTrim = request.GetMaxItemsToTrim();
        for (const auto& channel : info->Channels) {
            if (channel.Channel >= BLOB_CHANNEL) {
                for (const auto& history : channel.History) {
                    interm.ChannelGroupMap.emplace(channel.Channel, history.GroupID);
                }
            }
        }
        intermediate->TrimLeakedBlobs = std::move(interm);
    }
    return false;
}

bool TKeyValueState::PrepareCmdSetExecutorFastLogPolicy(const TActorContext & /*ctx*/,
        NKikimrClient::TKeyValueRequest &kvRequest, THolder<TIntermediate> &intermediate,
        const TTabletStorageInfo * /*info*/) {
    if (kvRequest.HasCmdSetExecutorFastLogPolicy()) {
        const auto& request = kvRequest.GetCmdSetExecutorFastLogPolicy();
        TIntermediate::TSetExecutorFastLogPolicy interm;
        interm.IsAllowed = request.GetIsAllowed();
        intermediate->SetExecutorFastLogPolicy = interm;
    }
    return false;
}

using TPrepareResult = TKeyValueState::TPrepareResult;

TPrepareResult TKeyValueState::PrepareOneCmd(const TCommand::Rename &request, THolder<TIntermediate> &intermediate) {
    intermediate->Commands.emplace_back(TIntermediate::TRename());
    auto &cmd = std::get<TIntermediate::TRename>(intermediate->Commands.back());
    cmd.OldKey = request.old_key();
    cmd.NewKey = request.new_key();
    return {};
}

TPrepareResult TKeyValueState::PrepareOneCmd(const TCommand::Concat &request, THolder<TIntermediate> &intermediate) {
    intermediate->Commands.emplace_back(TIntermediate::TConcat());
    auto &cmd = std::get<TIntermediate::TConcat>(intermediate->Commands.back());
    auto inputKeys = request.input_keys();
    cmd.InputKeys.insert(cmd.InputKeys.end(), inputKeys.begin(), inputKeys.end());

    cmd.OutputKey = request.output_key();
    cmd.KeepInputs = request.keep_inputs();
    return {};
}

TPrepareResult TKeyValueState::PrepareOneCmd(const TCommand::CopyRange &request, THolder<TIntermediate> &intermediate) {
    intermediate->Commands.emplace_back(TIntermediate::TCopyRange());
    auto &cmd = std::get<TIntermediate::TCopyRange>(intermediate->Commands.back());
    auto convResult = ConvertRange(request.range(), &cmd.Range, "CopyRange");
    if (convResult.WithError) {
        return {true, convResult.ErrorMsg};
    }
    cmd.PrefixToAdd = request.prefix_to_add();
    cmd.PrefixToRemove = request.prefix_to_remove();
    return {};
}

TPrepareResult TKeyValueState::PrepareOneCmd(const TCommand::Write &request, THolder<TIntermediate> &intermediate,
        const TTabletStorageInfo *info)
{
    intermediate->Commands.emplace_back(TIntermediate::TWrite());
    auto &cmd = std::get<TIntermediate::TWrite>(intermediate->Commands.back());
    cmd.Key = request.key();
    cmd.Data = request.value();
    switch (request.tactic()) {
    case TCommand::Write::TACTIC_MIN_LATENCY:
        cmd.Tactic = TEvBlobStorage::TEvPut::TacticMinLatency;
        break;
    case TCommand::Write::TACTIC_MAX_THROUGHPUT:
        cmd.Tactic = TEvBlobStorage::TEvPut::TacticMaxThroughput;
        break;
    default:
        cmd.Tactic = TEvBlobStorage::TEvPut::TacticDefault;
        break;
    }

    cmd.HandleClass = NKikimrBlobStorage::UserData;
    if (request.priority() == NKikimrKeyValue::Priorities::PRIORITY_BACKGROUND) {
        cmd.HandleClass = NKikimrBlobStorage::AsyncBlob;
    }

    bool isInline = false;
    ui32 storageChannelIdx = BLOB_CHANNEL;
    ui32 storageChannel = request.storage_channel();
    if (!storageChannel) {
        storageChannel = MainStorageChannelInPublicApi;
    }
    ui32 storageChannelOffset = storageChannel - MainStorageChannelInPublicApi;

    if (storageChannel == InlineStorageChannelInPublicApi) {
        isInline = true;
    } else {
        storageChannelIdx = storageChannelOffset + BLOB_CHANNEL;
        ui32 endChannel = info->Channels.size();
        if (storageChannelIdx >= endChannel) {
            storageChannelIdx = BLOB_CHANNEL;
        }
    }
    SplitIntoBlobs(cmd, isInline, storageChannelIdx, intermediate.Get());
    return {};
}

TPrepareResult TKeyValueState::PrepareOneCmd(const TCommand::DeleteRange &request, THolder<TIntermediate> &intermediate,
        const TActorContext &ctx)
{
    intermediate->Commands.emplace_back(TIntermediate::TDelete());
    auto &cmd = std::get<TIntermediate::TDelete>(intermediate->Commands.back());
    auto convResult = ConvertRange(request.range(), &cmd.Range, "DeleteRange");
    if (convResult.WithError) {
        return {true, convResult.ErrorMsg};
    }
    ui32 nToDelete = 0;
    TraverseRange(cmd.Range, [&](TIndex::iterator it) {
        Y_UNUSED(it);
        nToDelete++;
    });
    // The use of >, not >= is important here.
    if (nToDelete > DeletesPerRequestLimit && !AppData(ctx)->AllowHugeKeyValueDeletes) {
        TStringBuilder str;
        str << "KeyValue# " << TabletId;
        str << " Can't delete Range, in DeleteRange, total limit of deletions per request ("
            << DeletesPerRequestLimit << ") reached, Marker# KV90";
        TString msg = str;
        ReplyError<TEvKeyValue::TEvExecuteTransactionResponse>(ctx, msg,
                NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR, intermediate, nullptr);
        return {true, msg};
    }
    return {};
}

TPrepareResult TKeyValueState::PrepareOneCmd(const TCommand &request, THolder<TIntermediate> &intermediate,
        const TTabletStorageInfo *info, const TActorContext &ctx)
{
    switch (request.action_case()) {
    case NKikimrKeyValue::ExecuteTransactionRequest::Command::ACTION_NOT_SET:
        return {true, "Command not specified Marker# KV68"};
    case NKikimrKeyValue::ExecuteTransactionRequest::Command::kDeleteRange:
        return PrepareOneCmd(request.delete_range(), intermediate, ctx);
    case NKikimrKeyValue::ExecuteTransactionRequest::Command::kRename:
        return PrepareOneCmd(request.rename(), intermediate);
    case NKikimrKeyValue::ExecuteTransactionRequest::Command::kCopyRange:
        return PrepareOneCmd(request.copy_range(), intermediate);
    case NKikimrKeyValue::ExecuteTransactionRequest::Command::kConcat:
        return PrepareOneCmd(request.concat(), intermediate);
    case NKikimrKeyValue::ExecuteTransactionRequest::Command::kWrite:
        return PrepareOneCmd(request.write(), intermediate, info);
    }
}

TPrepareResult TKeyValueState::PrepareCommands(NKikimrKeyValue::ExecuteTransactionRequest &kvRequest,
    THolder<TIntermediate> &intermediate, const TTabletStorageInfo *info, const TActorContext &ctx)
{
    for (i32 idx = 0; idx < kvRequest.commands_size(); ++idx) {
        auto &cmd = kvRequest.commands(idx);
        TPrepareResult result = PrepareOneCmd(cmd, intermediate, info, ctx);
        if (cmd.has_write()) {
            intermediate->WriteIndices.push_back(idx);
        }
        if (result.WithError) {
            return result;
        }
    }
    if (intermediate->EvType == TEvKeyValue::TEvExecuteTransaction::EventType) {
        intermediate->ExecuteTransactionResponse.set_status(NKikimrKeyValue::Statuses::RSTATUS_OK);
    }
    return {};
}

NKikimrKeyValue::Statuses::ReplyStatus ConvertStatus(NMsgBusProxy::EResponseStatus status) {
    switch (status) {
    case NMsgBusProxy::MSTATUS_ERROR:
        return NKikimrKeyValue::Statuses::RSTATUS_ERROR;
    case NMsgBusProxy::MSTATUS_TIMEOUT:
        return NKikimrKeyValue::Statuses::RSTATUS_TIMEOUT;
    case NMsgBusProxy::MSTATUS_REJECTED:
        return NKikimrKeyValue::Statuses::RSTATUS_WRONG_LOCK_GENERATION;
    case NMsgBusProxy::MSTATUS_INTERNALERROR:
        return NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR;
    default:
        return NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR;
    };
}

void TKeyValueState::ReplyError(const TActorContext &ctx, TString errorDescription,
        NMsgBusProxy::EResponseStatus oldStatus, NKikimrKeyValue::Statuses::ReplyStatus newStatus,
        THolder<TIntermediate> &intermediate, const TTabletStorageInfo *info) {
    LOG_INFO_S(ctx, NKikimrServices::KEYVALUE, errorDescription);
    Y_VERIFY(!intermediate->IsReplied);

    if (intermediate->EvType == TEvKeyValue::TEvRequest::EventType) {
        THolder<TEvKeyValue::TEvResponse> response(new TEvKeyValue::TEvResponse);
        if (intermediate->HasCookie) {
            response->Record.SetCookie(intermediate->Cookie);
        }
        response->Record.SetErrorReason(errorDescription);
        response->Record.SetStatus(oldStatus);
        ResourceMetrics->Network.Increment(response->Record.ByteSize());
        intermediate->IsReplied = true;
        ctx.Send(intermediate->RespondTo, response.Release());
    }
    if (intermediate->EvType == TEvKeyValue::TEvExecuteTransaction::EventType) {
        ReplyError<TEvKeyValue::TEvExecuteTransactionResponse>(ctx, errorDescription,
                newStatus, intermediate, info);
        return;
    }
    if (intermediate->EvType == TEvKeyValue::TEvGetStorageChannelStatus::EventType) {
        ReplyError<TEvKeyValue::TEvGetStorageChannelStatusResponse>(ctx, errorDescription,
                newStatus, intermediate, info);
        return;
    }
    if (intermediate->EvType == TEvKeyValue::TEvRead::EventType) {
        ReplyError<TEvKeyValue::TEvReadResponse>(ctx, errorDescription,
                newStatus, intermediate, info);
        return;
    }
    if (intermediate->EvType == TEvKeyValue::TEvReadRange::EventType) {
        ReplyError<TEvKeyValue::TEvReadRangeResponse>(ctx, errorDescription,
                newStatus, intermediate, info);
        return;
    }

    if (info) {
        intermediate->UpdateStat();
        OnRequestComplete(intermediate->RequestUid, intermediate->CreatedAtGeneration, intermediate->CreatedAtStep,
                ctx, info, oldStatus, intermediate->Stat);
    } else { //metrics change report in OnRequestComplete is not done
        ResourceMetrics->TryUpdate(ctx);
        RequestInputTime.erase(intermediate->RequestUid);
    }
}

bool TKeyValueState::PrepareReadRequest(const TActorContext &ctx, TEvKeyValue::TEvRead::TPtr &ev,
        THolder<TIntermediate> &intermediate, TRequestType::EType *outRequestType)
{
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " PrepareReadRequest Marker# KV53");

    NKikimrKeyValue::ReadRequest &request = ev->Get()->Record;
    StoredState.SetChannelGeneration(ExecutorGeneration);
    StoredState.SetChannelStep(NextLogoBlobStep - 1);

    intermediate.Reset(new TIntermediate(ev->Sender, ctx.SelfID,
            StoredState.GetChannelGeneration(), StoredState.GetChannelStep(), TRequestType::ReadOnly));

    intermediate->HasCookie = true;
    intermediate->Cookie = request.cookie();

    intermediate->RequestUid = NextRequestUid;
    ++NextRequestUid;
    RequestInputTime[intermediate->RequestUid] = TAppData::TimeProvider->Now();
    intermediate->EvType = TEvKeyValue::TEvRead::EventType;

    bool isInlineOnly = true;
    intermediate->ReadCommand = TIntermediate::TRead();
    auto &response = std::get<TIntermediate::TRead>(*intermediate->ReadCommand);
    response.Key = request.key();

    if (CheckDeadline(ctx, ev->Get(), intermediate)) {
        return false;
    }

    if (CheckGeneration(ctx, ev->Get(), intermediate)) {
        return false;
    }

    auto it = Index.find(request.key());
    if (it == Index.end()) {
        response.Status = NKikimrProto::NODATA;
        response.Message = "No such key Marker# KV55";
        ReplyError<TEvKeyValue::TEvReadResponse>(ctx, response.Message,
                NKikimrKeyValue::Statuses::RSTATUS_NOT_FOUND, intermediate);
        return false;
    }
    bool isOverRun = PrepareOneRead<NKikimrKeyValue::Priorities, true, ReadResultSizeEstimationNewApi>(
            it->first, it->second, request.offset(), request.size(), request.priority(), request.limit_bytes(),
            intermediate, response, isInlineOnly);

    if (isInlineOnly) {
        *outRequestType = TRequestType::ReadOnlyInline;
        intermediate->Stat.RequestType = *outRequestType;
    }
    intermediate->IsTruncated = isOverRun;
    return true;
}

bool TKeyValueState::PrepareReadRangeRequest(const TActorContext &ctx, TEvKeyValue::TEvReadRange::TPtr &ev,
        THolder<TIntermediate> &intermediate, TRequestType::EType *outRequestType)
{
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " PrepareReadRangeRequest Marker# KV57");

    NKikimrKeyValue::ReadRangeRequest &request = ev->Get()->Record;
    StoredState.SetChannelGeneration(ExecutorGeneration);
    StoredState.SetChannelStep(NextLogoBlobStep - 1);

    TRequestType::EType requestType = TRequestType::ReadOnly;
    intermediate.Reset(new TIntermediate(ev->Sender, ctx.SelfID,
        StoredState.GetChannelGeneration(), StoredState.GetChannelStep(), requestType));

    intermediate->HasCookie = true;
    intermediate->Cookie = request.cookie();

    intermediate->RequestUid = NextRequestUid;
    ++NextRequestUid;
    RequestInputTime[intermediate->RequestUid] = TAppData::TimeProvider->Now();
    intermediate->EvType = TEvKeyValue::TEvReadRange::EventType;
    intermediate->TotalSize = ReadRangeRequestMetaDataSizeEstimation;

    intermediate->ReadCommand = TIntermediate::TRangeRead();
    auto &response = std::get<TIntermediate::TRangeRead>(*intermediate->ReadCommand);

    if (CheckDeadline(ctx, ev->Get(), intermediate)) {
        response.Status = NKikimrProto::ERROR;
        return false;
    }

    if (CheckGeneration(ctx, ev->Get(), intermediate)) {
        response.Status = NKikimrProto::ERROR;
        return false;
    }

    TKeyRange range;
    auto convResult = ConvertRange(request.range(), &range, "ReadRange");
    if (convResult.WithError) {
        response.Status = NKikimrProto::ERROR;
        return false;
    }
    response.Status = NKikimrProto::OK;

    ui64 cmdLimitBytes = request.limit_bytes();
    if (!cmdLimitBytes) {
        cmdLimitBytes = Max<ui64>();
    }
    bool includeData = request.include_data();
    ui8 priority = request.priority();
    response.LimitBytes = cmdLimitBytes;
    response.IncludeData = includeData;

    bool isInlineOnly = true;
    auto processOneCmdReadRange = ProcessOneCmdReadRange<NKikimrKeyValue::Priorities, true,
            KeyValuePairSizeEstimationNewApi, KeyInfoSizeEstimation>;
    processOneCmdReadRange(this, range, cmdLimitBytes, includeData, priority, response, intermediate, &isInlineOnly);

    if (isInlineOnly) {
        *outRequestType = TRequestType::ReadOnlyInline;
        intermediate->Stat.RequestType = *outRequestType;
    }
    return true;
}


bool TKeyValueState::PrepareExecuteTransactionRequest(const TActorContext &ctx,
        TEvKeyValue::TEvExecuteTransaction::TPtr &ev, THolder<TIntermediate> &intermediate,
        const TTabletStorageInfo *info)
{
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
            << " PrepareExecuteTransactionRequest Marker# KV72");

    NKikimrKeyValue::ExecuteTransactionRequest &request = ev->Get()->Record;
    StoredState.SetChannelGeneration(ExecutorGeneration);
    StoredState.SetChannelStep(NextLogoBlobStep - 1);

    TRequestType::EType requestType = TRequestType::WriteOnly;
    intermediate.Reset(new TIntermediate(ev->Sender, ctx.SelfID,
        StoredState.GetChannelGeneration(), StoredState.GetChannelStep(), requestType));

    intermediate->HasCookie = true;
    intermediate->Cookie = request.cookie();
    intermediate->EvType = TEvKeyValue::TEvExecuteTransaction::EventType;
    intermediate->ExecuteTransactionResponse.set_cookie(request.cookie());

    intermediate->RequestUid = NextRequestUid;
    ++NextRequestUid;
    RequestInputTime[intermediate->RequestUid] = TAppData::TimeProvider->Now();

    if (CheckDeadline(ctx, ev->Get(), intermediate)) {
        return false;
    }

    if (CheckGeneration(ctx, ev->Get(), intermediate)) {
        return false;
    }

    TPrepareResult result = PrepareCommands(request, intermediate, info, ctx);

    if (result.WithError) {
        for (const auto& [logoBlobId, initial] : std::exchange(intermediate->RefCountsIncr, {})) {
            if (const auto it = RefCounts.find(logoBlobId); it != RefCounts.end() && !--it->second) {
                RefCounts.erase(it);
            }
        }

        LOG_ERROR_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                << " PrepareExecuteTransactionRequest return flase, Marker# KV73"
                << " Submsg# " << result.ErrorMsg);
        return false;
    }

    return true;
}


TKeyValueState::TPrepareResult TKeyValueState::PrepareOneGetStatus(TIntermediate::TGetStatus &cmd,
        ui64 publicStorageChannel, const TTabletStorageInfo *info)
{
    NKikimrClient::TKeyValueRequest::EStorageChannel storageChannel = NKikimrClient::TKeyValueRequest::MAIN;
    if (publicStorageChannel == 1) {
        storageChannel = NKikimrClient::TKeyValueRequest::INLINE;
    } else if (publicStorageChannel) {
        ui32 storageChannelIdx = BLOB_CHANNEL + publicStorageChannel - MainStorageChannelInPublicApi;
        storageChannel = NKikimrClient::TKeyValueRequest::EStorageChannel(storageChannelIdx);
    }
    return InitGetStatusCommand(cmd, storageChannel, info);;
}


bool TKeyValueState::PrepareGetStorageChannelStatusRequest(const TActorContext &ctx, TEvKeyValue::TEvGetStorageChannelStatus::TPtr &ev,
        THolder<TIntermediate> &intermediate, const TTabletStorageInfo *info)
{
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " PrepareGetStorageChannelStatusRequest Marker# KV78");

    NKikimrKeyValue::GetStorageChannelStatusRequest &request = ev->Get()->Record;
    StoredState.SetChannelGeneration(ExecutorGeneration);
    StoredState.SetChannelStep(NextLogoBlobStep - 1);

    TRequestType::EType requestType = TRequestType::ReadOnly;
    intermediate.Reset(new TIntermediate(ev->Sender, ctx.SelfID,
        StoredState.GetChannelGeneration(), StoredState.GetChannelStep(), requestType));

    intermediate->RequestUid = NextRequestUid;
    ++NextRequestUid;
    RequestInputTime[intermediate->RequestUid] = TAppData::TimeProvider->Now();
    intermediate->EvType = TEvKeyValue::TEvGetStorageChannelStatus::EventType;

    if (CheckDeadline(ctx, ev->Get(), intermediate)) {
        return false;
    }

    if (CheckGeneration(ctx, ev->Get(), intermediate)) {
        return false;
    }

    intermediate->GetStatuses.resize(request.storage_channel_size());
    for (i32 idx = 0; idx < request.storage_channel_size(); ++idx) {
        TPrepareResult result = PrepareOneGetStatus(intermediate->GetStatuses[idx], request.storage_channel(idx), info);
        if (result.ErrorMsg && !result.WithError) {
            LOG_INFO_S(ctx, NKikimrServices::KEYVALUE, result.ErrorMsg  << " Marker# KV77");
        }
    }
    return true;
}

bool TKeyValueState::PrepareAcquireLockRequest(const TActorContext &ctx, TEvKeyValue::TEvAcquireLock::TPtr &ev,
        THolder<TIntermediate> &intermediate)
{
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " PrepareAcquireLockRequest Marker# KV79");

    StoredState.SetChannelGeneration(ExecutorGeneration);
    StoredState.SetChannelStep(NextLogoBlobStep - 1);

    TRequestType::EType requestType = TRequestType::ReadOnlyInline;
    intermediate.Reset(new TIntermediate(ev->Sender, ctx.SelfID,
        StoredState.GetChannelGeneration(), StoredState.GetChannelStep(), requestType));

    intermediate->RequestUid = NextRequestUid;
    ++NextRequestUid;
    RequestInputTime[intermediate->RequestUid] = TAppData::TimeProvider->Now();
    intermediate->EvType = TEvKeyValue::TEvAcquireLock::EventType;
    intermediate->HasIncrementGeneration = true;
    return true;
}

void RegisterReadRequestActor(const TActorContext &ctx, THolder<TIntermediate> &&intermediate,
        const TTabletStorageInfo *info)
{
    ctx.RegisterWithSameMailbox(CreateKeyValueStorageReadRequest(std::move(intermediate), info));
}

void RegisterRequestActor(const TActorContext &ctx, THolder<TIntermediate> &&intermediate,
        const TTabletStorageInfo *info)
{
    ctx.RegisterWithSameMailbox(CreateKeyValueStorageRequest(std::move(intermediate), info));
}

void TKeyValueState::ProcessPostponedIntermediate(const TActorContext& ctx, THolder<TIntermediate> &&intermediate,
            const TTabletStorageInfo *info)
{
    switch(intermediate->EvType) {
    case TEvKeyValue::TEvRequest::EventType:
        return RegisterRequestActor(ctx, std::move(intermediate), info);
    case TEvKeyValue::TEvRead::EventType:
    case TEvKeyValue::TEvReadRange::EventType:
        return RegisterReadRequestActor(ctx, std::move(intermediate), info);
    default:
        Y_FAIL_S("Unexpected event type# " << intermediate->EvType);
    }
}

void TKeyValueState::OnEvReadRequest(TEvKeyValue::TEvRead::TPtr &ev, const TActorContext &ctx,
        const TTabletStorageInfo *info)
{
    THolder<TIntermediate> intermediate;

    ResourceMetrics->Network.Increment(ev->Get()->Record.ByteSize());
    ResourceMetrics->TryUpdate(ctx);

    TRequestType::EType requestType = TRequestType::ReadOnly;
    CountRequestIncoming(requestType);

    if (PrepareReadRequest(ctx, ev, intermediate, &requestType)) {
        ++InFlightForStep[StoredState.GetChannelStep()];
        if (requestType == TRequestType::ReadOnlyInline) {
            LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                << " Create storage inline read request, Marker# KV49");
            RegisterReadRequestActor(ctx, std::move(intermediate), info);
            ++RoInlineIntermediatesInFlight;
        } else {
            if (IntermediatesInFlight < IntermediatesInFlightLimit) {
                LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                    << " Create storage read request, Marker# KV54");
                RegisterReadRequestActor(ctx, std::move(intermediate), info);
                ++IntermediatesInFlight;
            } else {
                LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                    << " Enqueue storage read request, Marker# KV56");
                PostponeIntermediate<TEvKeyValue::TEvRead>(std::move(intermediate));
            }
        }
        CountRequestTakeOffOrEnqueue(requestType);
    } else {
        intermediate->UpdateStat();
        CountRequestOtherError(requestType);
    }
}

void TKeyValueState::OnEvReadRangeRequest(TEvKeyValue::TEvReadRange::TPtr &ev, const TActorContext &ctx,
        const TTabletStorageInfo *info)
{
    THolder<TIntermediate> intermediate;

    ResourceMetrics->Network.Increment(ev->Get()->Record.ByteSize());
    ResourceMetrics->TryUpdate(ctx);

    TRequestType::EType requestType = TRequestType::ReadOnly;
    CountRequestIncoming(requestType);

    if (PrepareReadRangeRequest(ctx, ev, intermediate, &requestType)) {
        ++InFlightForStep[StoredState.GetChannelStep()];
        if (requestType == TRequestType::ReadOnlyInline) {
            LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                << " Create storage inline read range request, Marker# KV58");
            RegisterReadRequestActor(ctx, std::move(intermediate), info);
            ++RoInlineIntermediatesInFlight;
        } else {
            if (IntermediatesInFlight < IntermediatesInFlightLimit) {
                LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                    << " Create storage read range request, Marker# KV66");
                RegisterReadRequestActor(ctx, std::move(intermediate), info);
                ++IntermediatesInFlight;
            } else {
                LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                    << " Enqueue storage read range request, Marker# KV59");
                PostponeIntermediate<TEvKeyValue::TEvReadRange>(std::move(intermediate));
            }
        }
        CountRequestTakeOffOrEnqueue(requestType);
    } else {
        intermediate->UpdateStat();
        CountRequestOtherError(requestType);
    }
    CountRequestTakeOffOrEnqueue(requestType);
}

void TKeyValueState::OnEvExecuteTransaction(TEvKeyValue::TEvExecuteTransaction::TPtr &ev, const TActorContext &ctx,
        const TTabletStorageInfo *info)
{
    THolder<TIntermediate> intermediate;

    ResourceMetrics->Network.Increment(ev->Get()->Record.ByteSize());
    ResourceMetrics->TryUpdate(ctx);

    TRequestType::EType requestType = TRequestType::WriteOnly;
    CountRequestIncoming(requestType);

    if (PrepareExecuteTransactionRequest(ctx, ev, intermediate, info)) {
        ++InFlightForStep[StoredState.GetChannelStep()];
        LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
            << " Create storage request for WO, Marker# KV67");
        RegisterRequestActor(ctx, std::move(intermediate), info);

        CountRequestTakeOffOrEnqueue(requestType);
    } else {
        intermediate->UpdateStat();
        CountRequestOtherError(requestType);
    }
}

void TKeyValueState::OnEvGetStorageChannelStatus(TEvKeyValue::TEvGetStorageChannelStatus::TPtr &ev, const TActorContext &ctx,
        const TTabletStorageInfo *info)
{
    THolder<TIntermediate> intermediate;

    ResourceMetrics->Network.Increment(ev->Get()->Record.ByteSize());
    ResourceMetrics->TryUpdate(ctx);

    TRequestType::EType requestType = TRequestType::ReadOnlyInline;
    CountRequestIncoming(requestType);

    if (PrepareGetStorageChannelStatusRequest(ctx, ev, intermediate, info)) {
        ++InFlightForStep[StoredState.GetChannelStep()];
        LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
            << " Create GetStorageChannelStatus request, Marker# KV75");
        RegisterRequestActor(ctx, std::move(intermediate), info);
        ++RoInlineIntermediatesInFlight;
        CountRequestTakeOffOrEnqueue(requestType);
    } else {
        intermediate->UpdateStat();
        CountRequestOtherError(requestType);
    }
}

void TKeyValueState::OnEvAcquireLock(TEvKeyValue::TEvAcquireLock::TPtr &ev, const TActorContext &ctx,
        const TTabletStorageInfo *info)
{
    THolder<TIntermediate> intermediate;

    ResourceMetrics->Network.Increment(ev->Get()->Record.ByteSize());
    ResourceMetrics->TryUpdate(ctx);

    TRequestType::EType requestType = TRequestType::ReadOnlyInline;

    CountRequestIncoming(requestType);
    if (PrepareAcquireLockRequest(ctx, ev, intermediate)) {
        ++InFlightForStep[StoredState.GetChannelStep()];
        LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
            << " Create AcquireLock request, Marker# KV80");
        RegisterRequestActor(ctx, std::move(intermediate), info);
        ++RoInlineIntermediatesInFlight;
        CountRequestTakeOffOrEnqueue(requestType);
    } else {
        intermediate->UpdateStat();
        CountRequestOtherError(requestType);
    }
}

void TKeyValueState::OnEvIntermediate(TIntermediate &intermediate, const TActorContext &ctx) {
    Y_UNUSED(ctx);
    CountLatencyBsOps(intermediate.Stat);
    intermediate.Stat.LocalBaseTxCreatedAt = TAppData::TimeProvider->Now();
}

void TKeyValueState::OnEvRequest(TEvKeyValue::TEvRequest::TPtr &ev, const TActorContext &ctx,
        const TTabletStorageInfo *info) {
    THolder<TIntermediate> intermediate;
    NKikimrClient::TKeyValueRequest &request = ev->Get()->Record;

    ResourceMetrics->Network.Increment(request.ByteSize());
    ResourceMetrics->TryUpdate(ctx);

    bool hasWrites = request.CmdWriteSize() || request.CmdDeleteRangeSize() || request.CmdRenameSize()
                  || request.CmdCopyRangeSize() || request.CmdConcatSize() || request.HasCmdSetExecutorFastLogPolicy();

    bool hasReads = request.CmdReadSize() || request.CmdReadRangeSize();

    TRequestType::EType requestType;
    if (hasWrites) {
        if (hasReads) {
            requestType = TRequestType::ReadWrite;
        } else {
            requestType = TRequestType::WriteOnly;
        }
    } else {
        requestType = TRequestType::ReadOnly;
    }

    CountRequestIncoming(requestType);

    if (PrepareIntermediate(ev, intermediate, requestType, ctx, info)) {
        // Spawn KeyValueStorageRequest actor on the same thread
        ++InFlightForStep[StoredState.GetChannelStep()];
        if (requestType == TRequestType::WriteOnly) {
            LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                << " Create storage request for WO, Marker# KV42");
            RegisterRequestActor(ctx, std::move(intermediate), info);
        } else if (requestType == TRequestType::ReadOnlyInline) {
            LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                << " Create storage request for RO_INLINE, Marker# KV45");
            RegisterRequestActor(ctx, std::move(intermediate), info);
            ++RoInlineIntermediatesInFlight;
        } else {
            if (IntermediatesInFlight < IntermediatesInFlightLimit) {
                LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                    << " Create storage request for RO/RW, Marker# KV43");
                RegisterRequestActor(ctx, std::move(intermediate), info);
                ++IntermediatesInFlight;
            } else {
                LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                    << " Enqueue storage request for RO/RW, Marker# KV44");
                PostponeIntermediate<TEvKeyValue::TEvRequest>(std::move(intermediate));
            }
        }

        CountRequestTakeOffOrEnqueue(requestType);

    } else {
        intermediate->UpdateStat();

        CountRequestOtherError(requestType);
    }
}

bool TKeyValueState::PrepareIntermediate(TEvKeyValue::TEvRequest::TPtr &ev, THolder<TIntermediate> &intermediate,
        TRequestType::EType &inOutRequestType, const TActorContext &ctx, const TTabletStorageInfo *info) {
    LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId << " PrepareIntermediate Marker# KV40");
    NKikimrClient::TKeyValueRequest &request = ev->Get()->Record;

    StoredState.SetChannelGeneration(ExecutorGeneration);
    StoredState.SetChannelStep(NextLogoBlobStep - 1);

    intermediate.Reset(new TIntermediate(ev->Sender, ctx.SelfID,
        StoredState.GetChannelGeneration(), StoredState.GetChannelStep(), inOutRequestType));
    intermediate->RequestUid = NextRequestUid;
    ++NextRequestUid;
    RequestInputTime[intermediate->RequestUid] = TAppData::TimeProvider->Now();
    intermediate->EvType = TEvKeyValue::TEvRequest::EventType;

    intermediate->HasCookie = request.HasCookie();
    if (request.HasCookie()) {
        intermediate->Cookie = request.GetCookie();
    }
    intermediate->HasIncrementGeneration = request.HasCmdIncrementGeneration();

    if (CheckDeadline(ctx, request, intermediate)) {
        return false;
    }

    if (CheckGeneration(ctx, request, intermediate)) {
        return false;
    }

    bool error = false;

    bool isInlineOnly = true;
    error = error || PrepareCmdRead(ctx, request, intermediate, isInlineOnly);
    error = error || PrepareCmdReadRange(ctx, request, intermediate, isInlineOnly);
    if (!error && isInlineOnly && inOutRequestType == TRequestType::ReadOnly) {
        inOutRequestType = TRequestType::ReadOnlyInline;
        intermediate->Stat.RequestType = inOutRequestType;
    }

    ui32 cmdCount = request.CmdWriteSize() + request.CmdDeleteRangeSize() + request.CmdRenameSize()
            + request.CmdCopyRangeSize() + request.CmdConcatSize();
    intermediate->Commands.reserve(cmdCount);

    error = error || PrepareCmdCopyRange(ctx, request, intermediate);
    error = error || PrepareCmdRename(ctx, request, intermediate);
    error = error || PrepareCmdConcat(ctx, request, intermediate);
    error = error || PrepareCmdDelete(ctx, request, intermediate);
    error = error || PrepareCmdWrite(ctx, request, intermediate, info);
    error = error || PrepareCmdGetStatus(ctx, request, intermediate, info);
    error = error || PrepareCmdTrimLeakedBlobs(ctx, request, intermediate, info);
    error = error || PrepareCmdSetExecutorFastLogPolicy(ctx, request, intermediate, info);

    intermediate->WriteCount = request.CmdWriteSize();
    intermediate->DeleteCount = request.CmdDeleteRangeSize();
    intermediate->RenameCount = request.CmdRenameSize();
    intermediate->CopyRangeCount = request.CmdCopyRangeSize();
    intermediate->ConcatCount = request.CmdConcatSize();

    if (error) {
        for (const auto& [logoBlobId, initial] : std::exchange(intermediate->RefCountsIncr, {})) {
            if (const auto it = RefCounts.find(logoBlobId); it != RefCounts.end() && !--it->second) {
                RefCounts.erase(it);
            }
        }

        LOG_DEBUG_S(ctx, NKikimrServices::KEYVALUE, "KeyValue# " << TabletId
                << " PrepareIntermediate return flase, Marker# KV41");
        return false;
    }

    return true;
}

void TKeyValueState::UpdateResourceMetrics(const TActorContext& ctx) {
    ResourceMetrics->TryUpdate(ctx);
}

bool TKeyValueState::ConvertRange(const NKikimrClient::TKeyValueRequest::TKeyRange& from, TKeyRange *to,
                                  const TActorContext& ctx, THolder<TIntermediate>& intermediate, const char *cmd,
                                  ui32 index) {
    to->HasFrom = from.HasFrom();
    if (to->HasFrom) {
        to->KeyFrom = from.GetFrom();
        to->IncludeFrom = from.HasIncludeFrom() && from.GetIncludeFrom();
    } else if (from.HasIncludeFrom()) {
        TStringStream str;
        str << "KeyValue# " << TabletId
            << " Range.IncludeFrom unexpectedly set without Range.From in " << cmd << "(" << index << ")"
            << " Marker# KV13";
        ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_ERROR, NKikimrKeyValue::Statuses::RSTATUS_ERROR, intermediate);
        return false;
    }

    to->HasTo = from.HasTo();
    if (to->HasTo) {
        to->KeyTo = from.GetTo();
        to->IncludeTo = from.HasIncludeTo() && from.GetIncludeTo();
    } else if (from.HasIncludeTo()) {
        TStringStream str;
        str << "KeyValue# " << TabletId
            << " Range.IncludeTo unexpectedly set without Range.To in " << cmd << "(" << index << ")"
            << " Marker# KV14";
        ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_ERROR, NKikimrKeyValue::Statuses::RSTATUS_ERROR, intermediate);
        return false;
    }

    if (to->HasFrom && to->HasTo) {
        if (!to->IncludeFrom && !to->IncludeTo && to->KeyFrom >= to->KeyTo) {
            TStringStream str;
            str << "KeyValue# " << TabletId
                << " Range.KeyFrom >= Range.KeyTo in " << cmd << "(" << index << ")"
                << " Marker# KV15";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_ERROR, NKikimrKeyValue::Statuses::RSTATUS_ERROR, intermediate);
            return false;
        } else if (to->KeyFrom > to->KeyTo) {
            TStringStream str;
            str << "KeyValue# " << TabletId
                << " Range.KeyFrom > Range.KeyTo in " << cmd << "(" << index << ")"
                << " Marker# KV16";
            ReplyError(ctx, str.Str(), NMsgBusProxy::MSTATUS_ERROR, NKikimrKeyValue::Statuses::RSTATUS_ERROR, intermediate);
            return false;
        }
    }

    return true;
}

TString TKeyValueState::Dump() const {
    TStringStream ss;
    ss << "=== INDEX ===\n";
    for (auto& x : Index) {
        const TString& k = x.first;
        const TIndexRecord& v = x.second;
        ss << k << "=== ctime:" << v.CreationUnixTime;
        for (const TIndexRecord::TChainItem& y : v.Chain) {
            ss << " -> " << y.LogoBlobId << ":" << y.Offset;
        }
        ss << "\n";
    }
    ss << "=== END ===\n";
    return ss.Str();
}

void TKeyValueState::VerifyEqualIndex(const TKeyValueState& state) const {
    auto i2 = state.Index.cbegin(), e2 = state.Index.cend();
    int i = 0;
    for (auto i1 = Index.cbegin(), e1 = Index.cend(); i1 != e1; ++i, ++i1, ++i2) {
        Y_VERIFY(i2 != e2, "index length differs. Dump:\n%s\n%s\n", Dump().data(), state.Dump().data());
        const TString& k1 = i1->first;
        const TString& k2 = i2->first;
        Y_VERIFY(k1 == k2, "index key #%d differs. Dump:\n%s\n%s\n", i, Dump().data(), state.Dump().data());
        const TIndexRecord& v1 = i1->second;
        const TIndexRecord& v2 = i2->second;
        Y_VERIFY(v1 == v2, "index value #%d differs. Dump:\n%s\n%s\n", i, Dump().data(), state.Dump().data());
    }
    Y_VERIFY(i2 == e2, "index length differs. Dump:\n%s\n%s\n", Dump().data(), state.Dump().data());
}

void TKeyValueState::RenderHTMLPage(IOutputStream &out) const {
    HTML(out) {
        TAG(TH2) {out << "KeyValue Tablet";}
        UL_CLASS("nav nav-tabs") {
            LI_CLASS("active") {
                out << "<a href=\"#database\" data-toggle=\"tab\">Database</a>";
            }
            LI() {
                out << "<a href=\"#refcounts\" data-toggle=\"tab\">RefCounts</a>";
            }
            LI() {
                out << "<a href=\"#trash\" data-toggle=\"tab\">Trash</a>";
            }
            LI() {
                out << "<a href=\"#channelstat\" data-toggle=\"tab\">Channel Stat</a>";
            }
        }
        DIV_CLASS("tab-content") {
            DIV_CLASS_ID("tab-pane fade in active", "database") {
                TABLE_SORTABLE_CLASS("table") {
                    TABLEHEAD() {
                        TABLER() {
                            TABLEH() {out << "Idx";}
                            TABLEH() {out << "Key";}
                            TABLEH() {out << "Value Size";}
                            TABLEH() {out << "Creation UnixTime";}
                            TABLEH() {out << "Storage Channel";}
                            TABLEH() {out << "LogoBlobIds";}
                        }
                    }
                    TABLEBODY() {
                        ui64 idx = 1;
                        for (auto it = Index.begin(); it != Index.end(); ++it) {
                            TABLER() {
                                TABLED() {out << idx;}
                                ++idx;
                                TABLED() {out << EscapeC(it->first);}
                                TABLED() {out << it->second.GetFullValueSize();}
                                TABLED() {out << it->second.CreationUnixTime;}
                                TABLED() {
                                    NKikimrClient::TKeyValueRequest::EStorageChannel storageChannel =
                                        NKikimrClient::TKeyValueRequest::MAIN;
                                    if (it->second.Chain.size()) {
                                        if (it->second.Chain[0].IsInline()) {
                                            storageChannel = NKikimrClient::TKeyValueRequest::INLINE;
                                        } else {
                                            ui32 storageChannelIdx = it->second.Chain[0].LogoBlobId.Channel();
                                            ui32 storageChannelOffset = storageChannelIdx - BLOB_CHANNEL;
                                            storageChannel = (NKikimrClient::TKeyValueRequest::EStorageChannel)
                                                storageChannelOffset;
                                        }
                                    }
                                    out << NKikimrClient::TKeyValueRequest::EStorageChannel_Name(
                                        storageChannel);
                                }

                                TABLED() {
                                    for (ui32 i = 0; i < it->second.Chain.size(); ++i) {
                                        if (i > 0) {
                                            out << "<br/>";
                                        }
                                        if (it->second.Chain[i].IsInline()) {
                                            out << "[INLINE:" << it->second.Chain[i].InlineData.size() << "]";
                                        } else {
                                            out << it->second.Chain[i].LogoBlobId.ToString();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            DIV_CLASS_ID("tab-pane fade", "refcounts") {
                TABLE_SORTABLE_CLASS("table") {
                    TABLEHEAD() {
                        TABLER() {
                            TABLEH() {out << "Idx";}
                            TABLEH() {out << "LogoBlobId";}
                            TABLEH() {out << "RefCount";}
                        }
                    }
                    TABLEBODY() {
                        ui32 idx = 1;
                        for (const auto& kv : RefCounts) {
                            TABLER() {
                                TABLED() {out << idx;}
                                TABLED() {out << kv.first.ToString();}
                                TABLED() {out << kv.second;}
                            }
                            ++idx;
                        }
                    }
                }
            }
            DIV_CLASS_ID("tab-pane fade", "trash") {
                TABLE_SORTABLE_CLASS("table") {
                    TABLEHEAD() {
                        TABLER() {
                            TABLEH() {out << "Idx";}
                            TABLEH() {out << "LogoBlobId";}
                        }
                    }
                    TABLEBODY() {
                        ui64 idx = 1;
                        for (auto it = Trash.begin(); it != Trash.end(); ++it) {
                            TABLER() {
                                TABLED() {out << idx;}
                                ++idx;
                                TABLED() {out << *it;}
                            }
                        }
                    }
                }
            }
            DIV_CLASS_ID("tab-pane fade", "channelstat") {
                TABLE_SORTABLE_CLASS("table") {
                    TABLEHEAD() {
                        TABLER() {
                            TABLEH() {out << "Channel";}
                            TABLEH() {out << "Total values size";}
                        }
                    }
                    TABLEBODY() {
                        for (size_t i = 0; i < ChannelDataUsage.size(); ++i) {
                            if (UsedChannels[i]) {
                                TABLER() {
                                    TABLED() {
                                        if (i) {
                                            out << i;
                                        } else {
                                            out << "inline";
                                        }
                                    }
                                    ui64 size = ChannelDataUsage[i];
                                    out << "<td title=" << size << ">";
                                    TString value;
                                    for (; size >= 1000; size /= 1000) {
                                        value = Sprintf(" %03" PRIu64, size % 1000) + value;
                                    }
                                    value = Sprintf("%" PRIu64, size) + value;
                                    out << value;
                                    out << "</td>";
                                }
                            }
                        }
                    }
                }
            }

        }
    }
}

void TKeyValueState::MonChannelStat(NJson::TJsonValue& out) const {
    for (size_t i = 0; i < ChannelDataUsage.size(); ++i) {
        if (UsedChannels[i]) {
            const TString key = i ? Sprintf("%zu", i) : "inline";
            out[key] = ChannelDataUsage[i];
        }
    }
}

} // NKeyValue
} // NKikimr
