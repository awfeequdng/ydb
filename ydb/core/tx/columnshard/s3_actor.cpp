#ifndef KIKIMR_DISABLE_S3_OPS

#include "defs.h"
#include "columnshard_impl.h"

#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/wrappers/s3_wrapper.h>

namespace NKikimr::NColumnShard {

using TEvExternalStorage = NWrappers::TEvExternalStorage;

namespace {

TString ExtractBlobPart(const NOlap::TBlobRange& blobRange, const TString& data) {
    return TString(&data[blobRange.Offset], blobRange.Size);
}

struct TS3Export {
public:
    std::unique_ptr<TEvPrivate::TEvExport> Event;

    TS3Export() = default;

    explicit TS3Export(TAutoPtr<TEvPrivate::TEvExport> ev)
        : Event(ev.Release()) {
    }

    TEvPrivate::TEvExport::TBlobDataMap& Blobs() {
        return Event->Blobs;
    }

    TUnifiedBlobId AddExported(const TString& bucket, const TUnifiedBlobId& srcBlob) {
        Event->SrcToDstBlobs[srcBlob] = TUnifiedBlobId(srcBlob, TUnifiedBlobId::S3_BLOB, bucket);
        return Event->SrcToDstBlobs[srcBlob];
    }

    bool ExtractionFinished() const {
        return KeysToWrite.empty();
    }

    TS3Export& RegisterKey(const TString& key) {
        KeysToWrite.emplace(key);
        return *this;
    }

    TS3Export& FinishKey(const TString& key) {
        KeysToWrite.erase(key);
        return *this;
    }
private:
    TSet<TString> KeysToWrite;
};

struct TS3Forget {
    std::unique_ptr<TEvPrivate::TEvForget> Event;
    THashSet<TString> KeysToDelete;

    TS3Forget() = default;

    explicit TS3Forget(TAutoPtr<TEvPrivate::TEvForget> ev)
        : Event(ev.Release()) {
    }
};

}


class TS3Actor : public TActorBootstrapped<TS3Actor> {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::TX_COLUMNSHARD_S3_ACTOR;
    }

    TS3Actor(ui64 tabletId, const TActorId& parent, const TString& tierName)
        : TabletId(tabletId)
        , ShardActor(parent)
        , TierName(tierName)
    {}

    void Bootstrap() {
        LOG_S_DEBUG("[S3] Starting actor for tier '" << TierName << "' at tablet " << TabletId);
        Become(&TThis::StateWait);
    }

    void Handle(TEvPrivate::TEvS3Settings::TPtr& ev) {
        auto& msg = *ev->Get();
        auto& endpoint = msg.Settings.GetEndpoint();
        Bucket = msg.Settings.GetBucket();

        LOG_S_DEBUG("[S3] Update settings for tier '" << TierName << "' endpoint '" << endpoint
            << "' bucket '" << Bucket << "' at tablet " << TabletId);

        if (endpoint.empty()) {
            LOG_S_ERROR("[S3] No endpoint in settings for tier '" << TierName << "' at tablet " << TabletId);
            return;
        }
        if (Bucket.empty()) {
            LOG_S_ERROR("[S3] No bucket in settings for tier '" << TierName << "' at tablet " << TabletId);
            return;
        }

        ExternalStorageConfig = NWrappers::IExternalStorageConfig::Construct(msg.Settings);
        if (ExternalStorageActorId) {
            Send(ExternalStorageActorId, new TEvents::TEvPoisonPill);
            ExternalStorageActorId = {};
        }
        ExternalStorageActorId = this->RegisterWithSameMailbox(NWrappers::CreateS3Wrapper(ExternalStorageConfig->ConstructStorageOperator()));
    }

    void Handle(TEvPrivate::TEvExport::TPtr& ev) {
        auto& msg = *ev->Get();
        ui64 exportNo = msg.ExportNo;

        Y_VERIFY(!Exports.count(exportNo));
        Exports[exportNo] = TS3Export(ev->Release());
        auto& ex = Exports[exportNo];

        for (auto& [blobId, blob] : ex.Blobs()) {
            TString key = ex.AddExported(Bucket, blobId).GetS3Key();
            Y_VERIFY(!ExportingKeys.count(key)); // TODO

            ex.RegisterKey(key);
            ExportingKeys[key] = exportNo;
            
            if (blob.Evicting) {
                SendPutObjectIfNotExists(key, std::move(blob.Data));
            } else {
                SendPutObject(key, std::move(blob.Data));
            }
        }
    }

    void Handle(TEvPrivate::TEvForget::TPtr& ev) {
        ui64 forgetNo = ++ForgetNo;

        Forgets[forgetNo] = TS3Forget(ev->Release());
        auto& forget = Forgets[forgetNo];

        for (auto& evict : forget.Event->Evicted) {
            if (!evict.ExternBlob.IsS3Blob()) {
                LOG_S_ERROR("[S3] Forget not exported '" << evict.Blob.ToStringNew() << "' at tablet " << TabletId);
                continue;
            }

            const TString& key = evict.ExternBlob.GetS3Key();
            Y_VERIFY(!ForgettingKeys.count(key)); // TODO

            forget.KeysToDelete.emplace(key);
            ForgettingKeys[key] = forgetNo;
            SendDeleteObject(key);
        }
    }

    void Handle(TEvPrivate::TEvGetExported::TPtr& ev) {
        auto& evict = ev->Get()->Evicted;
        if (!evict.ExternBlob.IsS3Blob()) {
            LOG_S_ERROR("[S3] Get not exported '" << evict.Blob.ToStringNew() << "' at tablet " << TabletId);
            return;
        }

        TString key = evict.ExternBlob.GetS3Key();

        bool reading = ReadingKeys.count(key);
        ReadingKeys[key].emplace_back(ev->Release().Release());

        if (!reading) {
            const ui64 blobSize = evict.ExternBlob.BlobSize();
            SendGetObject(key, 0, blobSize);
        } else {
            LOG_S_DEBUG("[S3] Outstanding get key '" << key << "' at tablet " << TabletId);
        }
    }

    // TODO: clean written blobs in failed export
    void Handle(TEvExternalStorage::TEvPutObjectResponse::TPtr& ev) {
        Y_VERIFY(Initialized());

        auto& msg = *ev->Get();
        const auto& resultOutcome = msg.Result;

        const bool hasError = !resultOutcome.IsSuccess();
        TString errStr;
        if (hasError) {
            errStr = LogError("PutObjectResponse", resultOutcome.GetError(), !!msg.Key);
        }

        Y_VERIFY(msg.Key); // FIXME
        const TString key = *msg.Key;

        LOG_S_DEBUG("[S3] PutObjectResponse '" << key << "' at tablet " << TabletId);
        KeyFinished(key, hasError, errStr);
    }

    class TEvCheckObjectExistsRequestContext: public NWrappers::NExternalStorage::IRequestContext {
    private:
        using TBase = NWrappers::NExternalStorage::IRequestContext;
        const TString Key;
        TString Data;
    public:
        TEvCheckObjectExistsRequestContext(const TString& key, TString&& data)
            : Key(key)
            , Data(std::move(data)) {

        }
        TString DetachData() {
            return std::move(Data);
        }
        const TString& GetKey() const {
            return Key;
        }
    };

    void Handle(TEvExternalStorage::TEvCheckObjectExistsResponse::TPtr& ev) {
        Y_VERIFY(Initialized());

        auto& msg = *ev->Get();
        auto context = msg.GetRequestContextAs<TEvCheckObjectExistsRequestContext>();
        if (!context) {
            return;
        }
        if (!msg.IsExists()) {
            SendPutObject(context->GetKey(), std::move(context->DetachData()));
        } else {
            KeyFinished(context->GetKey(), false, "");
        }
    }

    void Handle(TEvExternalStorage::TEvDeleteObjectResponse::TPtr& ev) {
        Y_VERIFY(Initialized());

        auto& msg = *ev->Get();
        const auto& resultOutcome = msg.Result;

        TString errStr;
        if (!resultOutcome.IsSuccess()) {
            errStr = LogError("DeleteObjectResponse", resultOutcome.GetError(), !!msg.Key);
        }

        Y_VERIFY(msg.Key); // FIXME
        TString key = *msg.Key;

        LOG_S_DEBUG("[S3] DeleteObjectResponse '" << key << "' at tablet " << TabletId);

        if (!ForgettingKeys.count(key)) {
            LOG_S_DEBUG("[S3] DeleteObjectResponse for unknown key '" << key << "' at tablet " << TabletId);
            return;
        }

        ui64 forgetNo = ForgettingKeys[key];
        ForgettingKeys.erase(key);

        if (!Forgets.count(forgetNo)) {
            LOG_S_DEBUG("[S3] DeleteObjectResponse for unknown forget with key '" << key << "' at tablet " << TabletId);
            return;
        }

        auto& forget = Forgets[forgetNo];
        forget.KeysToDelete.erase(key);

        if (!errStr.empty()) {
            forget.Event->Status = NKikimrProto::ERROR;
            forget.Event->ErrorStr = errStr;
            Send(ShardActor, forget.Event.release());
            Forgets.erase(forgetNo);
        } else if (forget.KeysToDelete.empty()) {
            forget.Event->Status = NKikimrProto::OK;
            Send(ShardActor, forget.Event.release());
            Forgets.erase(forgetNo);
        }
    }

    void Handle(TEvExternalStorage::TEvGetObjectResponse::TPtr& ev) {
        Y_VERIFY(Initialized());

        auto& msg = *ev->Get();
        const auto& key = msg.Key;
        const auto& data = msg.Body;
        const auto& resultOutcome = msg.Result;

        TString errStr;
        if (!resultOutcome.IsSuccess()) {
            errStr = LogError("GetObjectResponse", resultOutcome.GetError(), !!key);
        }

        if (!key || key->empty()) {
            LOG_S_ERROR("[S3] no key in GetObjectResponse at tablet " << TabletId << ": " << errStr);
            return; // nothing to do without key
        }

        if (!ReadingKeys.count(*key)) {
            LOG_S_ERROR("[S3] no reading keys for key " << *key << " at tablet " << TabletId);
            return; // nothing to do without events
        }

        // TODO: CheckETag

        LOG_S_DEBUG("GetObjectResponse '" << *key << "', size: " << data.size() << " at tablet " << TabletId);

        auto status = errStr.empty() ? NKikimrProto::OK : NKikimrProto::ERROR;

        for (const auto& ev : ReadingKeys[*key]) {
            auto result = std::make_unique<TEvColumnShard::TEvReadBlobRangesResult>(TabletId);

            for (const auto& blobRange : ev->BlobRanges) {
                if (data.size() < blobRange.Offset + blobRange.Size) {
                    LOG_S_ERROR("GetObjectResponse '" << *key << "', data size: " << data.size()
                        << " is too small for blob range {" << blobRange.Offset << "," << blobRange.Size << "}"
                        << " at tablet " << TabletId);
                    status = NKikimrProto::ERROR;
                }

                auto* res = result->Record.AddResults();
                auto* resRange = res->MutableBlobRange();
                resRange->SetBlobId(blobRange.BlobId.ToStringNew());
                resRange->SetOffset(blobRange.Offset);
                resRange->SetSize(blobRange.Size);
                res->SetStatus(status);

                if (status == NKikimrProto::OK) {
                    res->SetData(ExtractBlobPart(blobRange, data));
                }
            }

            Send(ev->DstActor, result.release(), 0, ev->DstCookie);
        }
        ReadingKeys.erase(*key);
    }

    void KeyFinished(const TString& key, const bool hasError, const TString& errStr) {
        ui64 exportNo = 0;
        {
            auto itExportKey = ExportingKeys.find(key);
            if (itExportKey == ExportingKeys.end()) {
                LOG_S_DEBUG("[S3] KeyFinished for unknown key '" << key << "' at tablet " << TabletId);
                return;
            }
            exportNo = itExportKey->second;
            ExportingKeys.erase(itExportKey);
        }
        auto it = Exports.find(exportNo);
        if (it == Exports.end()) {
            LOG_S_DEBUG("[S3] KeyFinished for unknown export with key '" << key << "' at tablet " << TabletId);
            return;
        }

        auto& ex = it->second;
        ex.FinishKey(key);
        Y_VERIFY(ex.Event->DstActor == ShardActor);

        if (hasError) {
            ex.Event->Status = NKikimrProto::ERROR;
            Y_VERIFY(ex.Event->ErrorStrings.emplace(key, errStr).second, "%s", key.data());
            if (ex.ExtractionFinished()) {
                Send(ShardActor, ex.Event.release());
                Exports.erase(exportNo);
            }
        } else if (ex.ExtractionFinished()) {
            ex.Event->Status = NKikimrProto::OK;
            Send(ShardActor, ex.Event.release());
            Exports.erase(exportNo);
        }
    }

private:
    NWrappers::IExternalStorageConfig::TPtr ExternalStorageConfig;
    NActors::TActorId ExternalStorageActorId;
    ui64 TabletId;
    TActorId ShardActor;
    TString TierName;
    TString Bucket;
    ui64 ForgetNo{};
    THashMap<ui64, TS3Export> Exports;
    THashMap<ui64, TS3Forget> Forgets;
    THashMap<TString, ui64> ExportingKeys;
    THashMap<TString, ui64> ForgettingKeys;
    THashMap<TString, std::vector<std::unique_ptr<TEvPrivate::TEvGetExported>>> ReadingKeys;

    STATEFN(StateWait) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvPrivate::TEvS3Settings, Handle);
            hFunc(TEvPrivate::TEvExport, Handle);
            hFunc(TEvPrivate::TEvForget, Handle);
            hFunc(TEvPrivate::TEvGetExported, Handle);
            cFunc(TEvents::TEvPoisonPill::EventType, PassAway);
            hFunc(TEvExternalStorage::TEvPutObjectResponse, Handle);
            hFunc(TEvExternalStorage::TEvDeleteObjectResponse, Handle);
            hFunc(TEvExternalStorage::TEvGetObjectResponse, Handle);
            hFunc(TEvExternalStorage::TEvCheckObjectExistsResponse, Handle);
            
#if 0
            hFunc(TEvExternalStorage::TEvHeadObjectResponse, Handle);
#endif
            default:
                break;
        }
    }

    bool Initialized() const {
        return (bool)ExternalStorageActorId;
    }

    void PassAway() override {
        if (ExternalStorageActorId) {
            Send(ExternalStorageActorId, new TEvents::TEvPoisonPill());
            ExternalStorageActorId = {};
        }
        TActor::PassAway();
    }

    void SendPutObject(const TString& key, TString&& data) const {
        auto request = Aws::S3::Model::PutObjectRequest()
            .WithBucket(Bucket)
            .WithKey(key)
            .WithStorageClass(Aws::S3::Model::StorageClass::STANDARD_IA);
#if 0
        Aws::Map<Aws::String, Aws::String> metadata;
        metadata.emplace("Content-Type", "application/x-compressed");
        request.SetMetadata(std::move(metadata));
#endif
        LOG_S_DEBUG("[S3] PutObjectRequest key '" << key << "' at tablet " << TabletId);
        Send(ExternalStorageActorId, new TEvExternalStorage::TEvPutObjectRequest(request, std::move(data)));
    }

    void SendPutObjectIfNotExists(const TString& key, TString&& data) {
        auto request = Aws::S3::Model::HeadObjectRequest()
            .WithBucket(Bucket)
            .WithKey(key);

        LOG_S_DEBUG("[S3] HeadObjectRequest key '" << key << "' at tablet " << TabletId);
        std::shared_ptr<TEvCheckObjectExistsRequestContext> context = std::make_shared<TEvCheckObjectExistsRequestContext>(key, std::move(data));
        Send(ExternalStorageActorId, new TEvExternalStorage::TEvCheckObjectExistsRequest(request, context));
    }

    void SendHeadObject(const TString& key) const {
        auto request = Aws::S3::Model::HeadObjectRequest()
            .WithBucket(Bucket)
            .WithKey(key);

        LOG_S_DEBUG("[S3] HeadObjectRequest key '" << key << "' at tablet " << TabletId);
        Send(ExternalStorageActorId, new TEvExternalStorage::TEvHeadObjectRequest(request));
    }

    void SendGetObject(const TString& key, const ui32 startPos, const ui32 size) {
        Y_VERIFY(size);
        auto request = Aws::S3::Model::GetObjectRequest()
            .WithBucket(Bucket)
            .WithKey(key)
            .WithRange(TStringBuilder() << "bytes=" << startPos << "-" << startPos + size - 1);

        LOG_S_DEBUG("[S3] GetObjectRequest key '" << key << "' at tablet " << TabletId);
        Send(ExternalStorageActorId, new TEvExternalStorage::TEvGetObjectRequest(request));
    }

    void SendDeleteObject(const TString& key) const {
        auto request = Aws::S3::Model::DeleteObjectRequest()
            .WithBucket(Bucket)
            .WithKey(key);

        Send(ExternalStorageActorId, new TEvExternalStorage::TEvDeleteObjectRequest(request));
    }

    TString LogError(const TString& responseType, const Aws::S3::S3Error& error, bool hasKey) const {
        TString errStr = TString(error.GetExceptionName()) + " " + error.GetMessage();
        if (errStr.empty() && !hasKey) {
            errStr = responseType + " with no key";
        }

        LOG_S_NOTICE("[S3] Error in " << responseType << " at tablet " << TabletId << ": " << errStr);
        return errStr;
    }
};

IActor* CreateS3Actor(ui64 tabletId, const TActorId& parent, const TString& tierName) {
    return new TS3Actor(tabletId, parent, tierName);
}

}

#endif
