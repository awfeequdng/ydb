#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>
#include <ydb/core/mind/hive/hive.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

////////////////////////////////////////////////////////////////////////////////

class TConfigureParts: public TSubOperationState {
private:
    const TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TAlterFileStore::TConfigureParts"
            << " operationId#" << OperationId;
    }

public:
    TConfigureParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    bool HandleReply(
        TEvFileStore::TEvUpdateConfigResponse::TPtr& ev,
        TOperationContext& context) override
    {
        const auto ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            DebugHint() << " HandleReply TEvUpdateConfigResponse"
            << ", at schemeshard: " << ssId);

        auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxAlterFileStore, "invalid tx type %u", txState->TxType);
        Y_VERIFY(txState->State == TTxState::ConfigureParts, "invalid tx state %u", txState->State);

        auto tabletId = TTabletId(ev->Get()->Record.GetOrigin());
        auto status = ev->Get()->Record.GetStatus();

        // Schemeshard never sends invalid or outdated configs
        Y_VERIFY_S(status == NKikimrFileStore::OK || status == NKikimrFileStore::ERROR_UPDATE_IN_PROGRESS,
            "Unexpected error in UpdateConfigResponse"
            << ", status: " << NKikimrFileStore::EStatus_Name(status)
            << ", tx: " << OperationId
            << ", tablet: " << tabletId
            << ", at schemeshard: " << ssId);

        if (status == NKikimrFileStore::ERROR_UPDATE_IN_PROGRESS) {
            LOG_ERROR_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                DebugHint() << " Reconfiguration is in progress. We'll try to finish it later."
                << " tx " << OperationId
                << " tablet " << tabletId);
            return false;
        }

        TShardIdx idx = context.SS->MustGetShardIdx(tabletId);
        txState->ShardsInProgress.erase(idx);

        context.OnComplete.UnbindMsgFromPipe(OperationId, tabletId, idx);

        if (txState->ShardsInProgress.empty()) {
            NIceDb::TNiceDb db(context.GetDB());
            context.SS->ChangeTxState(db, OperationId, TTxState::Propose);
            context.OnComplete.ActivateTx(OperationId);
            return true;
        }

        return false;
    }

    bool ProgressState(TOperationContext& context) override {
        const auto ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            DebugHint() << " ProgressState"
            << ", at schemeshard: " << ssId);

        auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxAlterFileStore);
        Y_VERIFY(!txState->Shards.empty());

        txState->ClearShardsInProgress();

        auto fs = context.SS->FileStoreInfos[txState->TargetPathId];
        Y_VERIFY_S(fs, "FileStore info is null. PathId: " << txState->TargetPathId);

        Y_VERIFY(txState->Shards.size() == 1);
        for (const auto& shard: txState->Shards) {
            Y_VERIFY(shard.TabletType == ETabletType::FileStore);
            auto shardIdx = shard.Idx;
            auto tabletId = context.SS->ShardInfos[shardIdx].TabletID;

            TAutoPtr<TEvFileStore::TEvUpdateConfig> event(new TEvFileStore::TEvUpdateConfig());
            event->Record.SetTxId(ui64(OperationId.GetTxId()));
            event->Record.MutableConfig()->CopyFrom(*fs->AlterConfig);
            event->Record.MutableConfig()->SetVersion(fs->AlterVersion);

            context.OnComplete.BindMsgToPipe(OperationId, tabletId, shardIdx, event.Release());

            // Wait for results from this shard
            txState->ShardsInProgress.insert(shardIdx);
        }

        return false;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TPropose: public TSubOperationState {
private:
    const TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TAlterFileStore::TPropose"
            << " operationId#" << OperationId;
    }

public:
    TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    bool HandleReply(
        TEvPrivate::TEvOperationPlan::TPtr& ev,
        TOperationContext& context) override
    {
        const auto step = TStepId(ev->Get()->StepId);
        const auto ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            DebugHint() << " HandleReply TEvOperationPlan"
            << ", step: " << step
            << ", at schemeshard: " << ssId);

        auto* txState = context.SS->FindTx(OperationId);
        if (!txState) {
            return false;
        }

        Y_VERIFY(txState->TxType == TTxState::TxAlterFileStore);
        TPathId pathId = txState->TargetPathId;

        auto fs = context.SS->FileStoreInfos.at(pathId);
        Y_VERIFY_S(fs, "FileStore info is null. PathId: " << pathId);

        TPathElement::TPtr path = context.SS->PathsById.at(pathId);
        path->PathState = TPathElement::EPathState::EPathStateNoChanges;
        path->StepCreated = step;

        NIceDb::TNiceDb db(context.GetDB());
        context.SS->PersistCreateStep(db, pathId, step);

        fs->FinishAlter();

        context.SS->PersistFileStoreInfo(db, pathId, fs);
        context.SS->PersistRemoveFileStoreAlter(db, pathId);

        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

        context.OnComplete.DoneOperation(OperationId);
        return false;
    }

    bool ProgressState(TOperationContext& context) override {
        const auto ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            DebugHint() << " ProgressState"
            << ", at schemeshard: " << ssId);

        auto* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxAlterFileStore);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TAlterFileStore: public TSubOperation {
private:
    const TOperationId OperationId;
    const TTxTransaction Transaction;

    TTxState::ETxState State = TTxState::Invalid;

public:
    TAlterFileStore(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TAlterFileStore(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(
        const TString& owner,
        TOperationContext& context) override;

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TAlterFileStore");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            "TAlterFileStore AbortUnsafe"
            << ", opId: " << OperationId
            << ", forceDropId: " << forceDropTxId
            << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }

    void StateDone(TOperationContext& context) override {
        State = NextState(State);

        if (State != TTxState::Invalid) {
            SetState(SelectStateFunc(State));
            context.OnComplete.ActivateTx(OperationId);
        }
    }

private:
    TTxState::ETxState NextState() {
        return TTxState::CreateParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return TTxState::ConfigureParts;
        case TTxState::ConfigureParts:
            return TTxState::Propose;
        default:
            return TTxState::Invalid;
        }
        return TTxState::Invalid;
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return MakeHolder<TCreateParts>(OperationId);
        case TTxState::ConfigureParts:
            return MakeHolder<TConfigureParts>(OperationId);
        case TTxState::Propose:
            return MakeHolder<TPropose>(OperationId);
        default:
            return nullptr;
        }
    }

    TTxState& PrepareChanges(
        TOperationId operationId, TPathElement::TPtr item,
        TFileStoreInfo::TPtr fs,
        const TChannelsBindings& partitionChannels,
        TOperationContext& context);

    const NKikimrFileStore::TConfig* ParseParams(
        const NKikimrSchemeOp::TFileStoreDescription& operation,
        TString& errStr);

    bool ProcessChannelProfiles(
        const TPath& path,
        const NKikimrFileStore::TConfig& config,
        const NKikimrFileStore::TConfig& alterConfig,
        TOperationContext& context,
        TProposeResponse& result,
        TChannelsBindings& storeChannelsBinding);

    void ApplyChannelBindings(
        TFileStoreInfo::TPtr volume,
        const TChannelsBindings& channelBindings,
        TOperationContext& context);
};

////////////////////////////////////////////////////////////////////////////////

THolder<TProposeResponse> TAlterFileStore::Propose(
    const TString& owner,
    TOperationContext& context)
{
    Y_UNUSED(owner);

    const auto ssId = context.SS->SelfTabletId();

    const auto& operation = Transaction.GetAlterFileStore();
    const TString& parentPathStr = Transaction.GetWorkingDir();
    const TString& name = operation.GetName();
    const TPathId pathId = operation.HasPathId()
        ? context.SS->MakeLocalId(operation.GetPathId())
        : InvalidPathId;

    LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
        "TAlterFileStore Propose"
        << ", path: " << parentPathStr << "/" << name
        << ", pathId: " << pathId
        << ", opId: " << OperationId
        << ", at schemeshard: " << ssId);

    auto result = MakeHolder<TProposeResponse>(
        NKikimrScheme::StatusAccepted,
        ui64(OperationId.GetTxId()),
        ui64(ssId));

    TString errStr;
    if (!operation.HasName() && !operation.HasPathId()) {
        errStr = "Neither name nor pathId are present in FileStore";
        result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
        return result;
    }

    TPath path = operation.HasPathId()
        ? TPath::Init(pathId, context.SS)
        : TPath::Resolve(parentPathStr, context.SS).Dive(name);

    {
        auto checks = path.Check();
        checks
            .NotEmpty()
            .NotUnderDomainUpgrade()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotDeleted()
            .IsFileStore()
            .NotUnderOperation()
            .IsCommonSensePath();

        if (!checks) {
            TString explain = TStringBuilder()
                << "path fail checks"
                << ", path: " << path.PathString();

            auto status = checks.GetStatus(&explain);
            result->SetError(status, explain);
            return result;
        }
    }

    Y_VERIFY(path.Base()->IsCreateFinished());

    auto fs = context.SS->FileStoreInfos.at(path.Base()->PathId);
    Y_VERIFY_S(fs, "FileStore info is null. PathId: " << path.Base()->PathId);

    if (fs->AlterConfig) {
        result->SetError(
            NKikimrScheme::StatusMultipleModifications,
            "There is another operation in flight");
        return result;
    }

    const auto* alterConfig = ParseParams(operation, errStr);
    if (!alterConfig) {
        result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
        return result;
    }

    if (alterConfig->HasVersion() && alterConfig->GetVersion() != fs->Version) {
        result->SetError(
            NKikimrScheme::StatusPreconditionFailed,
            "Wrong version in config");
        return result;
    }

    TChannelsBindings storeChannelsBinding;
    const auto channelProfilesProcessed = ProcessChannelProfiles(
        path,
        fs->Config,
        *alterConfig,
        context,
        *result,
        storeChannelsBinding);

    if (!channelProfilesProcessed) {
        return result;
    }

    if (!context.SS->CheckApplyIf(Transaction, errStr)) {
        result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
        return result;
    }
    if (!context.SS->CheckInFlightLimit(TTxState::TxAlterFileStore, errStr)) {
        result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
        return result;
    }

    fs->PrepareAlter(*alterConfig);

    PrepareChanges(OperationId, path.Base(), fs, storeChannelsBinding, context);

    context.SS->ClearDescribePathCaches(path.Base());
    context.OnComplete.PublishToSchemeBoard(OperationId, path.Base()->PathId);

    State = NextState();
    SetState(SelectStateFunc(State));
    return result;
}

const NKikimrFileStore::TConfig* TAlterFileStore::ParseParams(
    const NKikimrSchemeOp::TFileStoreDescription& operation,
    TString& errStr)
{
    if (operation.HasIndexTabletId() || operation.HasVersion()) {
        errStr = "Setting schemeshard owned properties is not allowed";
        return nullptr;
    }

    if (!operation.HasConfig()) {
        errStr = "Missing changes to FileStore config";
        return nullptr;
    }

    const auto& config = operation.GetConfig();

    if (config.HasBlockSize()) {
        errStr = "Cannot change block size after creation";
        return nullptr;
    }

    return &config;
}

TTxState& TAlterFileStore::PrepareChanges(
    TOperationId operationId,
    TPathElement::TPtr item,
    TFileStoreInfo::TPtr fs,
    const TChannelsBindings& channelBindings,
    TOperationContext& context)
{
    NIceDb::TNiceDb db(context.GetDB());

    item->LastTxId = operationId.GetTxId();
    item->PathState = TPathElement::EPathState::EPathStateAlter;

    TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxAlterFileStore, item->PathId);
    txState.State = TTxState::CreateParts;

    ApplyChannelBindings(
        fs,
        channelBindings,
        context);

    txState.Shards.reserve(1);
    {
        TShardIdx shardIdx = fs->IndexShardIdx;
        TTabletId tabletId = fs->IndexTabletId;

        Y_VERIFY(context.SS->ShardInfos.contains(shardIdx));
        auto& shardInfo = context.SS->ShardInfos[shardIdx];
        Y_VERIFY(shardInfo.TabletID == tabletId);
        txState.Shards.emplace_back(shardIdx, ETabletType::FileStore, TTxState::CreateParts);
        shardInfo.CurrentTxId = operationId.GetTxId();
        context.SS->PersistShardTx(db, shardIdx, operationId.GetTxId());
    }

    LOG_DEBUG(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
        "AlterFileStore txid# %" PRIu64 ", AlterVersion %" PRIu64,
        operationId.GetTxId(), fs->AlterVersion);

    context.SS->PersistAddFileStoreAlter(db, item->PathId, fs);
    context.SS->PersistTxState(db, operationId);

    context.OnComplete.ActivateTx(operationId);
    return txState;
}

bool TAlterFileStore::ProcessChannelProfiles(
    const TPath& path,
    const NKikimrFileStore::TConfig& config,
    const NKikimrFileStore::TConfig& alterConfig,
    TOperationContext& context,
    TProposeResponse& result,
    TChannelsBindings& storeChannelsBinding)
{
    const auto& alterEcps = alterConfig.GetExplicitChannelProfiles();

    if (alterEcps.size()) {
        if (ui32(alterEcps.size()) > NHive::MAX_TABLET_CHANNELS) {
            auto errStr = Sprintf("Wrong number of channels %u , should be [1 .. %lu]",
                alterEcps.size(), NHive::MAX_TABLET_CHANNELS);

            result.SetError(NKikimrScheme::StatusInvalidParameter, errStr);
            return false;
        }

        // Cannot delete explicit profiles for existing channels
        if (alterConfig.ExplicitChannelProfilesSize() < config.ExplicitChannelProfilesSize()) {
            result.SetError(NKikimrScheme::StatusInvalidParameter,
                "Cannot reduce the number of channel profiles");
            return false;
        }

        if (!alterConfig.GetPoolKindChangeAllowed()) {
            // Cannot change pool kinds for existing channels
            // But it's ok to change other params, e.g. DataKind
            for (ui32 i = 0; i < config.ExplicitChannelProfilesSize(); ++i) {
                const auto& prevProfile = config.GetExplicitChannelProfiles(i);
                const auto& newProfile = alterConfig.GetExplicitChannelProfiles(i);
                if (prevProfile.GetPoolKind() != newProfile.GetPoolKind()) {
                    result.SetError(
                        NKikimrScheme::StatusInvalidParameter,
                        TStringBuilder() << "Cannot change PoolKind for channel " << i
                            << ", " << prevProfile.GetPoolKind()
                            << " -> " << newProfile.GetPoolKind());

                    return false;
                }
            }
        }
    }

    const auto& ecps = alterEcps.empty() ? config.GetExplicitChannelProfiles() : alterEcps;
    TVector<TStringBuf> partitionPoolKinds(Reserve(ecps.size()));
    for (const auto& ecp : ecps) {
        partitionPoolKinds.push_back(ecp.GetPoolKind());
    }

    const auto storeChannelsResolved = context.SS->ResolveChannelsByPoolKinds(
        partitionPoolKinds,
        path.GetPathIdForDomain(),
        storeChannelsBinding);

    if (!storeChannelsResolved) {
        result.SetError(NKikimrScheme::StatusInvalidParameter,
            "Unable to construct channel binding for filestore with the storage pool");
        return false;
    }

    context.SS->SetNfsChannelsParams(ecps, storeChannelsBinding);
    return true;
}

void TAlterFileStore::ApplyChannelBindings(
        TFileStoreInfo::TPtr fs,
        const TChannelsBindings& channelBindings,
        TOperationContext& context)
{
    auto& shardInfo = context.SS->ShardInfos[fs->IndexShardIdx];
    if (!shardInfo.BindedChannels.empty()) {
        Y_VERIFY(shardInfo.BindedChannels.size() <= channelBindings.size());
        shardInfo.BindedChannels.resize(channelBindings.size());
        Copy(channelBindings.begin(), channelBindings.end(), shardInfo.BindedChannels.begin());
    }
}

}   // namespace

namespace NKikimr {
namespace NSchemeShard {

////////////////////////////////////////////////////////////////////////////////

ISubOperationBase::TPtr CreateAlterFileStore(TOperationId id, const TTxTransaction& tx) {
    return new TAlterFileStore(id, tx);
}

ISubOperationBase::TPtr CreateAlterFileStore(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TAlterFileStore(id, state);
}

}   // namespace NSchemeShard
}   // namespace NKikimr
