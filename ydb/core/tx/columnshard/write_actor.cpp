#include "columnshard_impl.h"

#include <ydb/core/blobstorage/dsproxy/blobstorage_backoff.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>

namespace NKikimr::NColumnShard {

class TWriteActor : public TActorBootstrapped<TWriteActor> {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::TX_COLUMNSHARD_WRITE_ACTOR;
    }

    TWriteActor(ui64 tabletId,
                const NOlap::TIndexInfo& indexInfo,
                const TActorId& dstActor,
                TBlobBatch&& blobBatch,
                bool blobGrouppingEnabled,
                TAutoPtr<TEvColumnShard::TEvWrite> writeEv,
                TAutoPtr<TEvPrivate::TEvWriteIndex> writeIndexEv,
                const TInstant& deadline)
        : TabletId(tabletId)
        , IndexInfo(indexInfo)
        , DstActor(dstActor)
        , BlobBatch(std::move(blobBatch))
        , BlobGrouppingEnabled(blobGrouppingEnabled)
        , WriteEv(writeEv)
        , WriteIndexEv(writeIndexEv)
        , Deadline(deadline)
    {
        Y_VERIFY(WriteEv || WriteIndexEv);
        Y_VERIFY(!WriteEv || !WriteIndexEv);
    }

    // TODO: CheckYellow

    void Handle(TEvBlobStorage::TEvPutResult::TPtr& ev, const TActorContext& ctx) {
        TEvBlobStorage::TEvPutResult* msg = ev->Get();
        auto status = msg->Status;

        if (msg->StatusFlags.Check(NKikimrBlobStorage::StatusDiskSpaceLightYellowMove)) {
            YellowMoveChannels.insert(msg->Id.Channel());
        }
        if (msg->StatusFlags.Check(NKikimrBlobStorage::StatusDiskSpaceYellowStop)) {
            YellowStopChannels.insert(msg->Id.Channel());
        }


        if (status != NKikimrProto::OK) {
            LOG_S_WARN("Unsuccessful TEvPutResult for blob " << msg->Id.ToString()
                << " status: " << status << " reason: " << msg->ErrorReason);
            SendResultAndDie(ctx, status);
            return;
        }

        LOG_S_TRACE("TEvPutResult for blob " << msg->Id.ToString());

        BlobBatch.OnBlobWriteResult(ev);

        if (BlobBatch.AllBlobWritesCompleted()) {
            SendResultAndDie(ctx, NKikimrProto::OK);
        }
    }

    void Handle(TEvents::TEvWakeup::TPtr& ev, const TActorContext& ctx) {
        Y_UNUSED(ev);
        LOG_S_WARN("TEvWakeup: write timeout at tablet " << TabletId << " (write)");

        SendResultAndDie(ctx, NKikimrProto::TIMEOUT);
    }

    void SendResultAndDie(const TActorContext& ctx, NKikimrProto::EReplyStatus status) {
        if (Deadline != TInstant::Max()) {
            TInstant now = TAppData::TimeProvider->Now();
            if (Deadline <= now) {
                status = NKikimrProto::TIMEOUT;
            }
        }

        SendResult(ctx, status);
        Die(ctx);
    }

    void Bootstrap(const TActorContext& ctx) {
        if (Deadline != TInstant::Max()) {
            TInstant now = TAppData::TimeProvider->Now();
            if (Deadline <= now) {
                SendResultAndDie(ctx, NKikimrProto::TIMEOUT);
                return;
            }

            const TDuration timeout = Deadline - now;
            ctx.Schedule(timeout, new TEvents::TEvWakeup());
        }

        if (WriteEv) {
            SendWriteRequest(ctx);
        } else {
            SendMultiWriteRequest(ctx);
        }
        Become(&TThis::StateWait);
    }

    void SendWriteRequest(const TActorContext& ctx) {
        Y_VERIFY(WriteEv->PutStatus == NKikimrProto::UNKNOWN);

        auto& record = Proto(WriteEv.Get());
        ui64 pathId = record.GetTableId();
        ui64 writeId = record.GetWriteId();
        auto& srcData = record.GetData();
        TString meta;
        if (record.HasMeta()) {
            meta = record.GetMeta().GetSchema();
            if (meta.empty() || record.GetMeta().GetFormat() != NKikimrTxColumnShard::FORMAT_ARROW) {
                LOG_S_INFO("Bad metadata for writeId " << writeId << " pathId " << pathId << " at tablet " << TabletId);
                SendResultAndDie(ctx, NKikimrProto::ERROR);
            }
        }

        // Heavy operations inside. We cannot run them in tablet event handler.
        TString strError;
        std::shared_ptr<arrow::RecordBatch>& batch = WriteEv->WrittenBatch;
        {
            TCpuGuard guard(ResourceUsage);
            batch = IndexInfo.PrepareForInsert(srcData, meta, strError);
        }
        if (!batch) {
            LOG_S_INFO("Bad data for writeId " << writeId << ", pathId " << pathId
                << " (" << strError << ") at tablet " << TabletId);
            SendResultAndDie(ctx, NKikimrProto::ERROR);
            return;
        }

        TString data;
        {
            TCpuGuard guard(ResourceUsage);
            data = NArrow::SerializeBatchNoCompression(batch);
        }
        if (data.size() > TLimits::MAX_BLOB_SIZE) {
            LOG_S_INFO("Extracted data (" << data.size() << " bytes) is bigger than source ("
                << srcData.size() << " bytes) and limit, writeId " << writeId << " pathId " << pathId
                << " at tablet " << TabletId);

            SendResultAndDie(ctx, NKikimrProto::ERROR);
            return;
        }

        record.SetData(data); // modify for TxWrite

        { // Update meta
            ui64 dirtyTime = AppData(ctx)->TimeProvider->Now().Seconds();
            Y_VERIFY(dirtyTime);

            NKikimrTxColumnShard::TLogicalMetadata outMeta;
            outMeta.SetNumRows(batch->num_rows());
            outMeta.SetRawBytes(NArrow::GetBatchDataSize(batch));
            outMeta.SetDirtyWriteTimeSeconds(dirtyTime);

            meta.clear();
            if (!outMeta.SerializeToString(&meta)) {
                LOG_S_ERROR("Canot set metadata for blob, writeId " << writeId << " pathId " << pathId
                    << " at tablet " << TabletId);
                SendResultAndDie(ctx, NKikimrProto::ERROR);
                return;
            }
        }
        record.MutableMeta()->SetLogicalMeta(meta);

        if (data.size() > WriteEv->MaxSmallBlobSize) {
            WriteEv->BlobId = DoSendWriteBlobRequest(data, ctx);
        } else {
            TUnifiedBlobId smallBlobId = BlobBatch.AddSmallBlob(data);
            Y_VERIFY(smallBlobId.IsSmallBlob());
            WriteEv->BlobId = smallBlobId;
        }

        Y_VERIFY(WriteEv->BlobId.BlobSize() == data.size());

        LOG_S_DEBUG("Writing " << WriteEv->BlobId.ToStringNew() << " writeId " << writeId << " pathId " << pathId
            << " at tablet " << TabletId);

        if (BlobBatch.AllBlobWritesCompleted()) {
            SendResultAndDie(ctx, NKikimrProto::OK);
        }
    }

    void SendMultiWriteRequest(const TActorContext& ctx) {
        Y_VERIFY(WriteIndexEv);
        Y_VERIFY(WriteIndexEv->PutStatus == NKikimrProto::UNKNOWN);

        auto indexChanges = WriteIndexEv->IndexChanges;
        LOG_S_DEBUG("Writing " << WriteIndexEv->Blobs.size() << " blobs at " << TabletId);

        const TVector<TString>& blobs = WriteIndexEv->Blobs;
        Y_VERIFY(blobs.size() > 0);
        size_t blobsPos = 0;

        // Send accumulated data and update records with the blob Id
        auto fnFlushAcummultedBlob = [this, &ctx] (TString& accumulatedBlob, NOlap::TPortionInfo& portionInfo,
            TVector<std::pair<size_t, TString>>& recordsInBlob)
        {
            Y_VERIFY(accumulatedBlob.size() > 0);
            Y_VERIFY(recordsInBlob.size() > 0);
            auto blobId = DoSendWriteBlobRequest(accumulatedBlob, ctx);
            LOG_S_TRACE("Write Index Blob " << blobId << " with " << recordsInBlob.size() << " records");
            for (const auto& rec : recordsInBlob) {
                size_t i = rec.first;
                const TString& recData = rec.second;
                auto& blobRange = portionInfo.Records[i].BlobRange;
                blobRange.BlobId = blobId;
                Y_VERIFY(blobRange.Offset + blobRange.Size <= accumulatedBlob.size());
                Y_VERIFY(blobRange.Size == recData.size());

                if (WriteIndexEv->CacheData) {
                    // Save original (non-accumulted) blobs with the corresponding TBlobRanges in order to
                    // put them into cache at commit time
                    WriteIndexEv->IndexChanges->Blobs[blobRange] = recData;
                }
            }
            accumulatedBlob.clear();
            recordsInBlob.clear();
        };

        TString accumulatedBlob;
        TVector<std::pair<size_t, TString>> recordsInBlob;

        size_t portionsToWrite = indexChanges->AppendedPortions.size();
        bool appended = true;
        if (indexChanges->PortionsToEvict.size()) {
            Y_VERIFY(portionsToWrite == 0);
            portionsToWrite = indexChanges->PortionsToEvict.size();
            appended = false;
        }

        for (size_t pos = 0; pos < portionsToWrite; ++pos) {
            auto& portionInfo = appended ? indexChanges->AppendedPortions[pos]
                                         : indexChanges->PortionsToEvict[pos].first;
            auto& records = portionInfo.Records;

            accumulatedBlob.clear();
            recordsInBlob.clear();

            for (size_t i = 0; i < records.size(); ++i, ++blobsPos) {
                const TString& currentBlob = blobs[blobsPos];
                Y_VERIFY(currentBlob.size());

                if ((accumulatedBlob.size() + currentBlob.size() > TLimits::MAX_BLOB_SIZE) ||
                    (accumulatedBlob.size() && !BlobGrouppingEnabled))
                {
                    fnFlushAcummultedBlob(accumulatedBlob, portionInfo, recordsInBlob);
                }

                // Accumulate data chunks into a single blob and save record indices of these chunks
                records[i].BlobRange.Offset = accumulatedBlob.size();
                records[i].BlobRange.Size = currentBlob.size();
                accumulatedBlob.append(currentBlob);
                recordsInBlob.emplace_back(i, currentBlob);
            }
            if (accumulatedBlob.size() != 0) {
                fnFlushAcummultedBlob(accumulatedBlob, portionInfo, recordsInBlob);
            }
        }
        Y_VERIFY(blobsPos == blobs.size());
    }

    TUnifiedBlobId DoSendWriteBlobRequest(const TString& data, const TActorContext& ctx) {
        ResourceUsage.Network += data.size();
        return BlobBatch.SendWriteBlobRequest(data, Deadline, ctx);
    }

    STFUNC(StateWait) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvBlobStorage::TEvPutResult, Handle);
            HFunc(TEvents::TEvWakeup, Handle);
            default:
                break;
        }
    }

private:
    ui64 TabletId;
    NOlap::TIndexInfo IndexInfo;
    TActorId DstActor;
    TBlobBatch BlobBatch;
    bool BlobGrouppingEnabled;
    TAutoPtr<TEvColumnShard::TEvWrite> WriteEv;
    TAutoPtr<TEvPrivate::TEvWriteIndex> WriteIndexEv;
    TInstant Deadline;
    THashSet<ui32> YellowMoveChannels;
    THashSet<ui32> YellowStopChannels;
    TUsage ResourceUsage;

    void SaveResourceUsage() {
        if (WriteEv) {
            WriteEv->ResourceUsage.Add(ResourceUsage);
        } else {
            WriteIndexEv->ResourceUsage.Add(ResourceUsage);
        }
        ResourceUsage = TUsage();
    }

    void SendResult(const TActorContext& ctx, NKikimrProto::EReplyStatus status) {
        SaveResourceUsage();
        if (WriteEv) {
            LOG_S_DEBUG("Written " << WriteEv->BlobId.ToStringNew() << " Status: " << status);
            WriteEv->PutStatus = status;
            WriteEv->BlobBatch = std::move(BlobBatch);
            WriteEv->YellowMoveChannels = TVector<ui32>(YellowMoveChannels.begin(), YellowMoveChannels.end());
            WriteEv->YellowStopChannels = TVector<ui32>(YellowStopChannels.begin(), YellowStopChannels.end());
            ctx.Send(DstActor, WriteEv.Release());
        } else {
            WriteIndexEv->PutStatus = status;
            WriteIndexEv->BlobBatch = std::move(BlobBatch);
            WriteIndexEv->YellowMoveChannels = TVector<ui32>(YellowMoveChannels.begin(), YellowMoveChannels.end());
            WriteIndexEv->YellowStopChannels = TVector<ui32>(YellowStopChannels.begin(), YellowStopChannels.end());
            ctx.Send(DstActor, WriteIndexEv.Release());
        }
    }
};

IActor* CreateWriteActor(ui64 tabletId, const NOlap::TIndexInfo& indexTable,
                        const TActorId& dstActor, TBlobBatch&& blobBatch, bool blobGrouppingEnabled,
                        TAutoPtr<TEvColumnShard::TEvWrite> ev, const TInstant& deadline) {
    return new TWriteActor(tabletId, indexTable, dstActor, std::move(blobBatch), blobGrouppingEnabled, ev, {}, deadline);
}

IActor* CreateWriteActor(ui64 tabletId, const NOlap::TIndexInfo& indexTable,
                        const TActorId& dstActor, TBlobBatch&& blobBatch, bool blobGrouppingEnabled,
                        TAutoPtr<TEvPrivate::TEvWriteIndex> ev, const TInstant& deadline) {
    return new TWriteActor(tabletId, indexTable, dstActor, std::move(blobBatch), blobGrouppingEnabled, {}, ev, deadline);
}

}
