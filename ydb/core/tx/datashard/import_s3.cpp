#ifndef KIKIMR_DISABLE_S3_OPS

#include "backup_restore_traits.h"
#include "datashard_impl.h"
#include "extstorage_usage_config.h"
#include "import_common.h"
#include "import_s3.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/services.pb.h>
#include <ydb/core/tablet/resource_broker.h>
#include <ydb/core/wrappers/s3_wrapper.h>
#include <ydb/core/wrappers/s3_storage.h>
#include <ydb/core/io_formats/csv.h>
#include <ydb/public/lib/scheme_types/scheme_type_id.h>

#include <contrib/libs/zstd/include/zstd.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/log.h>

#include <util/generic/buffer.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/memory/pool.h>
#include <util/string/builder.h>

namespace {

    struct DestroyZCtx {
        static void Destroy(::ZSTD_DCtx* p) noexcept {
            ZSTD_freeDCtx(p);
        }
    };

    constexpr ui64 SumWithSaturation(ui64 a, ui64 b) {
        return Max<ui64>() - a < b ? Max<ui64>() : a + b;
    }

} // anonymous

namespace NKikimr {
namespace NDataShard {

using namespace NResourceBroker;
using namespace NWrappers;

using namespace Aws::S3;
using namespace Aws;

class TS3Downloader: public TActorBootstrapped<TS3Downloader> {
    class IReadController {
    public:
        enum EFeed {
            READY_DATA = 0,
            NOT_ENOUGH_DATA = 1,
            ERROR = 2,
        };

    public:
        virtual ~IReadController() = default;
        // Returns status or error
        virtual EFeed Feed(TString&& portion, TString& error) = 0;
        // Returns view to internal buffer with ready data after successful Feed()
        virtual TStringBuf GetReadyData() const = 0;
        // Clear internal buffer & makes it ready for another Feed()
        virtual void Confirm() = 0;
        virtual ui64 PendingBytes() const = 0;
        virtual ui64 ReadyBytes() const = 0;
    };

    class TReadController: public IReadController {
    public:
        explicit TReadController(ui32 rangeSize, ui64 bufferSizeLimit)
            : RangeSize(rangeSize)
            , BufferSizeLimit(bufferSizeLimit)
        {
        }

        std::pair<ui64, ui64> NextRange(ui64 contentLength, ui64 processedBytes) const {
            Y_VERIFY(contentLength > 0);
            Y_VERIFY(processedBytes < contentLength);

            const ui64 start = processedBytes + PendingBytes();
            const ui64 end = Min(SumWithSaturation(start, RangeSize), contentLength) - 1;
            return std::make_pair(start, end);
        }

    protected:
        bool CheckBufferSize(size_t size, TString& reason) const {
            if ((size + RangeSize) >= BufferSizeLimit) {
                reason = "reached buffer size limit";
                return false;
            }

            return true;
        }

        TStringBuf AsStringBuf(size_t size) const {
            return TStringBuf(Buffer.Data(), size);
        }

    private:
        const ui32 RangeSize;
        const ui64 BufferSizeLimit;

    protected:
        TBuffer Buffer;

    }; // TReadController

    class TReadControllerRaw: public TReadController {
    public:
        using TReadController::TReadController;

        EFeed Feed(TString&& portion, TString& error) override {
            Y_VERIFY(Pos == 0);

            Buffer.Append(portion.data(), portion.size());
            const ui64 pos = AsStringBuf(Buffer.Size()).rfind('\n');

            if (TString::npos == pos) {
                if (!CheckBufferSize(Buffer.Size(), error)) {
                    return ERROR;
                } else {
                    return NOT_ENOUGH_DATA;
                }
            }

            Pos = pos + 1; // for '\n'
            return READY_DATA;
        }

        TStringBuf GetReadyData() const override {
            return AsStringBuf(Pos);
        }

        void Confirm() override {
            Buffer.ChopHead(Pos);
            Pos = 0;
        }

        ui64 PendingBytes() const override {
            return Buffer.Size();
        }

        ui64 ReadyBytes() const override {
            return Pos;
        }

    private:
        ui64 Pos = 0;
    };

    class TReadControllerZstd: public TReadController {
        void Reset() {
            ZSTD_DCtx_reset(Context.Get(), ZSTD_reset_session_only);
            ZSTD_DCtx_refDDict(Context.Get(), NULL);
        }

    public:
        explicit TReadControllerZstd(ui32 rangeSize, ui64 bufferSizeLimit)
            : TReadController(rangeSize, bufferSizeLimit)
            , Context(ZSTD_createDCtx())
        {
            Reset();
        }

        EFeed Feed(TString&& portion, TString& error) override {
            Y_VERIFY(ReadyInputBytes == 0 && ReadyOutputPos == 0);

            auto input = ZSTD_inBuffer{portion.data(), portion.size(), 0};
            while (input.pos < input.size) {
                PendingInputBytes -= input.pos; // dec before decompress

                auto output = ZSTD_outBuffer{Buffer.Data(), Buffer.Capacity(), Buffer.Size()};
                auto res = ZSTD_decompressStream(Context.Get(), &output, &input);

                if (ZSTD_isError(res)) {
                    error = ZSTD_getErrorName(res);
                    return ERROR;
                }

                if (output.pos == output.size) {
                    if (!CheckBufferSize(Buffer.Capacity(), error)) {
                        return ERROR;
                    }

                    Buffer.Reserve(Buffer.Capacity() + ZSTD_BLOCKSIZE_MAX);
                }

                PendingInputBytes += input.pos; // inc after decompress
                Buffer.Proceed(output.pos);

                if (res == 0) {
                    if (AsStringBuf(Buffer.Size()).back() != '\n') {
                        error = "cannot find new line symbol";
                        return ERROR;
                    }

                    ReadyInputBytes = PendingInputBytes;
                    ReadyOutputPos = Buffer.Size();
                    Reset();
                }
            }

            if (!ReadyOutputPos) {
                if (!CheckBufferSize(Buffer.Size(), error)) {
                    return ERROR;
                } else {
                    return NOT_ENOUGH_DATA;
                }
            }

            return READY_DATA;
        }

        TStringBuf GetReadyData() const override {
            return AsStringBuf(ReadyOutputPos);
        }

        void Confirm() override {
            Buffer.ChopHead(ReadyOutputPos);

            PendingInputBytes -= ReadyInputBytes;
            ReadyInputBytes = 0;

            ReadyOutputPos = 0;
        }

        ui64 PendingBytes() const override {
            return PendingInputBytes;
        }

        ui64 ReadyBytes() const override {
            return ReadyInputBytes;
        }

    private:
        THolder<::ZSTD_DCtx, DestroyZCtx> Context;
        ui64 PendingInputBytes = 0;
        ui64 ReadyInputBytes = 0;
        ui64 ReadyOutputPos = 0;
    };

    class TUploadRowsRequestBuilder {
    public:
        void New(const TTableInfo& tableInfo, const NKikimrSchemeOp::TTableDescription& scheme) {
            Record = std::make_shared<NKikimrTxDataShard::TEvUploadRowsRequest>();
            Record->SetTableId(tableInfo.GetId());

            TVector<TString> columnNames;
            for (const auto& column : scheme.GetColumns()) {
                columnNames.push_back(column.GetName());
            }

            auto& rowScheme = *Record->MutableRowScheme();
            for (ui32 id : tableInfo.GetKeyColumnIds()) {
                rowScheme.AddKeyColumnIds(id);
            }
            for (ui32 id : tableInfo.GetValueColumnIds(columnNames)) {
                rowScheme.AddValueColumnIds(id);
            }
        }

        void AddRow(const TVector<TCell>& keys, const TVector<TCell>& values) {
            Y_VERIFY(Record);
            auto& row = *Record->AddRows();
            row.SetKeyColumns(TSerializedCellVec::Serialize(keys));
            row.SetValueColumns(TSerializedCellVec::Serialize(values));
        }

        const std::shared_ptr<NKikimrTxDataShard::TEvUploadRowsRequest>& GetRecord() {
            Y_VERIFY(Record);
            return Record;
        }

    private:
        std::shared_ptr<NKikimrTxDataShard::TEvUploadRowsRequest> Record;

    }; // TUploadRowsRequestBuilder

    enum class EWakeupTag: ui64 {
        Restart,
        RetryUpload,
    };

    void AllocateResource() {
        IMPORT_LOG_D("AllocateResource");

        const auto* appData = AppData();
        Send(MakeResourceBrokerID(), new TEvResourceBroker::TEvSubmitTask(
            TStringBuilder() << "Restore { " << TxId << ":" << DataShard << " }",
            {{ 1, 0 }},
            appData->DataShardConfig.GetRestoreTaskName(),
            appData->DataShardConfig.GetRestoreTaskPriority(),
            nullptr
        ));

        Become(&TThis::StateAllocateResource);
    }

    void Handle(TEvResourceBroker::TEvResourceAllocated::TPtr& ev) {
        IMPORT_LOG_I("Handle TEvResourceBroker::TEvResourceAllocated {"
            << " TaskId: " << ev->Get()->TaskId
        << " }");

        TaskId = ev->Get()->TaskId;
        Restart();
    }

    void Restart() {
        IMPORT_LOG_N("Restart"
            << ": attempt# " << Attempt);

        if (const TActorId client = std::exchange(Client, TActorId())) {
            Send(client, new TEvents::TEvPoisonPill());
        }


        Client = RegisterWithSameMailbox(CreateS3Wrapper(ExternalStorageConfig->ConstructStorageOperator()));

        HeadObject(Settings.GetDataKey(DataFormat, CompressionCodec));
        Become(&TThis::StateWork);
    }

    void HeadObject(const TString& key) {
        IMPORT_LOG_D("HeadObject"
            << ": key# " << key);

        auto request = Model::HeadObjectRequest()
            .WithBucket(Settings.GetBucket())
            .WithKey(key);

        Send(Client, new TEvExternalStorage::TEvHeadObjectRequest(request));
    }

    void GetObject(const TString& key, const std::pair<ui64, ui64>& range) {
        IMPORT_LOG_D("GetObject"
            << ": key# " << key
            << ", range# " << range.first << "-" << range.second);

        auto request = Model::GetObjectRequest()
            .WithBucket(Settings.GetBucket())
            .WithKey(key)
            .WithRange(TStringBuilder() << "bytes=" << range.first << "-" << range.second);

        Send(Client, new TEvExternalStorage::TEvGetObjectRequest(request));
    }

    void Handle(TEvExternalStorage::TEvHeadObjectResponse::TPtr& ev) {
        IMPORT_LOG_D("Handle " << ev->Get()->ToString());

        const auto& result = ev->Get()->Result;
        if (!result.IsSuccess()) {
            switch (result.GetError().GetErrorType()) {
            case S3Errors::RESOURCE_NOT_FOUND:
            case S3Errors::NO_SUCH_KEY:
                break;
            default:
                IMPORT_LOG_E("Error at 'HeadObject'"
                    << ": error# " << result);
                return RestartOrFinish(result.GetError().GetMessage().c_str());
            }

            CompressionCodec = NBackupRestoreTraits::NextCompressionCodec(CompressionCodec);
            if (CompressionCodec == NBackupRestoreTraits::ECompressionCodec::Invalid) {
                return Finish(false, TStringBuilder() << "Cannot find any supported data file"
                    << ": prefix# " << Settings.GetObjectKeyPattern());
            }

            return HeadObject(Settings.GetDataKey(DataFormat, CompressionCodec));
        }

        switch (CompressionCodec) {
        case NBackupRestoreTraits::ECompressionCodec::None:
            Reader.Reset(new TReadControllerRaw(ReadBatchSize, ReadBufferSizeLimit));
            break;
        case NBackupRestoreTraits::ECompressionCodec::Zstd:
            Reader.Reset(new TReadControllerZstd(ReadBatchSize, ReadBufferSizeLimit));
            break;
        case NBackupRestoreTraits::ECompressionCodec::Invalid:
            Y_FAIL("unreachable");
        }

        ETag = result.GetResult().GetETag();
        ContentLength = result.GetResult().GetContentLength();

        Send(DataShard, new TEvDataShard::TEvGetS3DownloadInfo(TxId));
    }

    void Handle(TEvDataShard::TEvS3DownloadInfo::TPtr& ev) {
        IMPORT_LOG_D("Handle " << ev->Get()->ToString());

        const auto& info = ev->Get()->Info;
        if (!info.DataETag) {
            Send(DataShard, new TEvDataShard::TEvStoreS3DownloadInfo(TxId, {
                ETag, ProcessedBytes, WrittenBytes, WrittenRows
            }));
            return;
        }

        ProcessDownloadInfo(info, TStringBuf("DownloadInfo"));
    }

    void ProcessDownloadInfo(const TS3Download& info, const TStringBuf marker) {
        IMPORT_LOG_N("Process download info at '" << marker << "'"
            << ": info# " << info);

        Y_VERIFY(info.DataETag);
        if (!CheckETag(*info.DataETag, ETag, marker)) {
            return;
        }

        ProcessedBytes = info.ProcessedBytes;
        WrittenBytes = info.WrittenBytes;
        WrittenRows = info.WrittenRows;

        if (!ContentLength || ProcessedBytes >= ContentLength) {
            return Finish();
        }

        GetObject(Settings.GetDataKey(DataFormat, CompressionCodec),
            Reader->NextRange(ContentLength, ProcessedBytes));
    }

    void Handle(TEvExternalStorage::TEvGetObjectResponse::TPtr& ev) {
        IMPORT_LOG_D("Handle " << ev->Get()->ToString());

        auto& msg = *ev->Get();
        const auto& result = msg.Result;
        const TStringBuf marker = "GetObject";

        if (!CheckResult(result, marker)) {
            return;
        }

        if (!CheckETag(ETag, result.GetResult().c_str(), marker)) {
            return;
        }

        IMPORT_LOG_T("Content size"
            << ": processed-bytes# " << ProcessedBytes
            << ", content-length# " << ContentLength
            << ", body-size# " << msg.Body.size());

        TString error;
        switch (Reader->Feed(std::move(msg.Body), error)) {
        case TReadController::READY_DATA:
            break;

        case TReadController::NOT_ENOUGH_DATA:
            if (SumWithSaturation(ProcessedBytes, Reader->PendingBytes()) < ContentLength) {
                return GetObject(Settings.GetDataKey(DataFormat, CompressionCodec),
                    Reader->NextRange(ContentLength, ProcessedBytes));
            } else {
                error = "reached end of file";
            }
            [[fallthrough]];

        default: // ERROR
            return Finish(false, TStringBuilder() << "Cannot process data"
                << ": " << error);
        }

        if (auto data = Reader->GetReadyData()) {
            ProcessedBytes += Reader->ReadyBytes();
            RequestBuilder.New(TableInfo, Scheme);

            TMemoryPool pool(256);
            while (ProcessData(data, pool));

            Reader->Confirm();
        } else {
            Y_FAIL("unreachable");
        }
    }

    bool ProcessData(TStringBuf& data, TMemoryPool& pool) {
        pool.Clear();

        TStringBuf line = data.NextTok('\n');
        const TStringBuf origLine = line;

        if (!line) {
            if (data) {
                return true; // skip empty line
            }

            const auto& record = RequestBuilder.GetRecord();
            WrittenRows += record->RowsSize();

            UploadRows();
            return false;
        }

        std::vector<std::pair<i32, ui32>> columnOrderTypes; // {keyOrder, PType}
        columnOrderTypes.reserve(Scheme.GetColumns().size());

        for (const auto& column : Scheme.GetColumns()) {
            columnOrderTypes.emplace_back(TableInfo.KeyOrder(column.GetName()), column.GetTypeId());
        }

        TVector<TCell> keys;
        TVector<TCell> values;
        TString strError;

        if (!NFormats::TYdbDump::ParseLine(line, columnOrderTypes, pool, keys, values, strError, WrittenBytes)) {
            Finish(false, TStringBuilder() << strError << " on line: " << origLine);
            return false;
        }

        Y_VERIFY(!keys.empty());
        if (!TableInfo.IsMyKey(keys) /* TODO: maybe skip */) {
            Finish(false, TStringBuilder() << "Key is out of range on line: " << origLine);
            return false;
        }

        RequestBuilder.AddRow(keys, values);
        return true;
    }

    void UploadRows() {
        const auto& record = RequestBuilder.GetRecord();

        IMPORT_LOG_I("Upload rows"
            << ": count# " << record->RowsSize()
            << ", size# " << record->ByteSizeLong());

        Send(DataShard, new TEvDataShard::TEvUnsafeUploadRowsRequest(TxId, record, {
            ETag, ProcessedBytes, WrittenBytes, WrittenRows
        }));
    }

    void Handle(TEvDataShard::TEvUnsafeUploadRowsResponse::TPtr& ev) {
        IMPORT_LOG_D("Handle " << ev->Get()->ToString());

        const auto& record = ev->Get()->Record;
        switch (record.GetStatus()) {
        case NKikimrTxDataShard::TError::OK:
            return ProcessDownloadInfo(ev->Get()->Info, TStringBuf("UploadResponse"));

        case NKikimrTxDataShard::TError::WRONG_SHARD_STATE: // OVERLOADED
            return RetryUpload();

        case NKikimrTxDataShard::TError::SCHEME_ERROR:
        case NKikimrTxDataShard::TError::BAD_ARGUMENT:
            return Finish(false, record.GetErrorDescription());

        default:
            return RestartOrFinish(record.GetErrorDescription());
        }
    }

    template <typename TResult>
    bool CheckResult(const TResult& result, const TStringBuf marker) {
        if (result.IsSuccess()) {
            return true;
        }

        IMPORT_LOG_E("Error at '" << marker << "'"
            << ": error# " << result);
        RestartOrFinish(result.GetError().GetMessage().c_str());

        return false;
    }

    bool CheckETag(const TString& expected, const TString& got, const TStringBuf marker) {
        if (expected == got) {
            return true;
        }

        const TString error = TStringBuilder() << "ETag mismatch at '" << marker << "'"
            << ": expected# " << expected
            << ", got# " << got;

        IMPORT_LOG_E(error);
        Finish(false, error);

        return false;
    }

    bool CheckScheme() {
        auto finish = [this](const TString& error) -> bool {
            IMPORT_LOG_E(error);
            Finish(false, error);

            return false;
        };

        for (const auto& column : Scheme.GetColumns()) {
            if (!TableInfo.HasColumn(column.GetName())) {
                return finish(TStringBuilder() << "Scheme mismatch: cannot find column"
                    << ": name# " << column.GetName());
            }

            const NScheme::TTypeId type = TableInfo.GetColumnType(column.GetName());
            if (type != static_cast<NScheme::TTypeId>(column.GetTypeId())) {
                return finish(TStringBuilder() << "Scheme mismatch: column type mismatch"
                    << ": name# " << column.GetName()
                    << ", expected# " << type
                    << ", got# " << static_cast<NScheme::TTypeId>(column.GetTypeId()));
            }
        }

        if (TableInfo.GetKeyColumnIds().size() != (ui32)Scheme.KeyColumnNamesSize()) {
            return finish(TStringBuilder() << "Scheme mismatch: key column count mismatch"
                << ": expected# " << TableInfo.GetKeyColumnIds().size()
                << ", got# " << Scheme.KeyColumnNamesSize());
        }

        for (ui32 i = 0; i < (ui32)Scheme.KeyColumnNamesSize(); ++i) {
            const auto& name = Scheme.GetKeyColumnNames(i);
            const ui32 keyOrder = TableInfo.KeyOrder(name);

            if (keyOrder != i) {
                return finish(TStringBuilder() << "Scheme mismatch: key order mismatch"
                    << ": name# " << name
                    << ", expected# " << keyOrder
                    << ", got# " << i);
            }
        }

        return true;
    }

    void RetryUpload() {
        Schedule(TDuration::MilliSeconds(50), new TEvents::TEvWakeup(static_cast<ui64>(EWakeupTag::RetryUpload)));
    }

    void RestartOrFinish(const TString& error) {
        if (Attempt++ < Retries) {
            Delay = Min(Delay * Attempt, MaxDelay);
            const TDuration random = TDuration::FromValue(TAppData::RandomProvider->GenRand64() % Delay.MicroSeconds());

            Schedule(Delay + random, new TEvents::TEvWakeup(static_cast<ui64>(EWakeupTag::Restart)));
        } else {
            Finish(false, error);
        }
    }

    void Handle(TEvents::TEvWakeup::TPtr& ev) {
        switch (static_cast<EWakeupTag>(ev->Get()->Tag)) {
        case EWakeupTag::Restart:
            return Restart();
        case EWakeupTag::RetryUpload:
            return UploadRows();
        }
    }

    void Finish(bool success = true, const TString& error = TString()) {
        IMPORT_LOG_N("Finish"
            << ": success# " << success
            << ", error# " << error
            << ", writtenBytes# " << WrittenBytes
            << ", writtenRows# " << WrittenRows);

        TAutoPtr<IDestructable> prod = new TImportJobProduct(success, error, WrittenBytes, WrittenRows);
        Send(DataShard, new TDataShard::TEvPrivate::TEvAsyncJobComplete(prod), 0, TxId);

        Y_VERIFY(TaskId);
        Send(MakeResourceBrokerID(), new TEvResourceBroker::TEvFinishTask(TaskId));

        PassAway();
    }

    void NotifyDied() {
        Send(MakeResourceBrokerID(), new TEvResourceBroker::TEvNotifyActorDied());
        PassAway();
    }

    void PassAway() override {
        if (Client) {
            Send(Client, new TEvents::TEvPoisonPill());
        }

        TActor::PassAway();
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::IMPORT_S3_DOWNLOADER_ACTOR;
    }

    TStringBuf LogPrefix() const {
        return LogPrefix_;
    }

    explicit TS3Downloader(const TActorId& dataShard, ui64 txId, const NKikimrSchemeOp::TRestoreTask& task, const TTableInfo& tableInfo)
        : ExternalStorageConfig(new NExternalStorage::TS3ExternalStorageConfig(task.GetS3Settings()))
        , DataShard(dataShard)
        , TxId(txId)
        , Settings(TS3Settings::FromRestoreTask(task))
        , DataFormat(NBackupRestoreTraits::EDataFormat::Csv)
        , CompressionCodec(NBackupRestoreTraits::ECompressionCodec::None)
        , TableInfo(tableInfo)
        , Scheme(task.GetTableDescription())
        , LogPrefix_(TStringBuilder() << "s3:" << TxId)
        , Retries(task.GetNumberOfRetries())
        , ReadBatchSize(task.GetS3Settings().GetLimits().GetReadBatchSize())
        , ReadBufferSizeLimit(AppData()->DataShardConfig.GetRestoreReadBufferSizeLimit())
    {
    }

    void Bootstrap() {
        IMPORT_LOG_D("Bootstrap"
            << ": attempt# " << Attempt);

        if (!CheckScheme()) {
            return;
        }

        AllocateResource();
    }

    STATEFN(StateAllocateResource) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvResourceBroker::TEvResourceAllocated, Handle);
            sFunc(TEvents::TEvPoisonPill, NotifyDied);
        }
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvExternalStorage::TEvHeadObjectResponse, Handle);
            hFunc(TEvExternalStorage::TEvGetObjectResponse, Handle);

            hFunc(TEvDataShard::TEvS3DownloadInfo, Handle);
            hFunc(TEvDataShard::TEvUnsafeUploadRowsResponse, Handle);

            hFunc(TEvents::TEvWakeup, Handle);
            sFunc(TEvents::TEvPoisonPill, NotifyDied);
        }
    }

private:
    NWrappers::IExternalStorageConfig::TPtr ExternalStorageConfig;
    const TActorId DataShard;
    const ui64 TxId;
    const NDataShard::TS3Settings Settings;
    const NBackupRestoreTraits::EDataFormat DataFormat;
    NBackupRestoreTraits::ECompressionCodec CompressionCodec;
    const TTableInfo TableInfo;
    const NKikimrSchemeOp::TTableDescription Scheme;
    const TString LogPrefix_;

    const ui32 Retries;
    ui32 Attempt = 0;

    TDuration Delay = TDuration::Minutes(1);
    static constexpr TDuration MaxDelay = TDuration::Minutes(10);

    ui64 TaskId = 0;
    TActorId Client;

    TString ETag;
    ui64 ContentLength = 0;
    ui64 ProcessedBytes = 0;
    ui64 WrittenBytes = 0;
    ui64 WrittenRows = 0;

    const ui32 ReadBatchSize;
    const ui64 ReadBufferSizeLimit;
    THolder<TReadController> Reader;
    TUploadRowsRequestBuilder RequestBuilder;

}; // TS3Downloader

IActor* CreateS3Downloader(const TActorId& dataShard, ui64 txId, const NKikimrSchemeOp::TRestoreTask& task, const TTableInfo& info) {
    return new TS3Downloader(dataShard, txId, task, info);
}

} // NDataShard
} // NKikimr

#endif // KIKIMR_DISABLE_S3_OPS
