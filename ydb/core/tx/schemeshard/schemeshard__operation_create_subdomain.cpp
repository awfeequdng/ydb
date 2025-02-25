#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>
#include <ydb/core/persqueue/config/config.h>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

void DeclareShards(TTxState& txState, TTxId txId, TPathId pathId,
                   ui32 count, TTabletTypes::EType type,
                   const TChannelsBindings& channelsBindings,
                   TSchemeShard* ss)
{
    txState.Shards.reserve(count);
    for (ui64 i = 0; i < count; ++i) {
        auto shardId = ss->RegisterShardInfo(
            TShardInfo(txId, pathId, type)
                .WithBindedChannels(channelsBindings));
        txState.Shards.emplace_back(shardId, type, TTxState::CreateParts);
    }
}

void PersistShards(NIceDb::TNiceDb& db, TTxState& txState, TSchemeShard* ss) {
    for (const auto& shard : txState.Shards) {
        Y_VERIFY(ss->ShardInfos.contains(shard.Idx), "shard info is set before");
        auto& shardInfo = ss->ShardInfos.at(shard.Idx);
        ss->PersistShardMapping(db, shard.Idx, InvalidTabletId, shardInfo.PathId, shardInfo.CurrentTxId, shardInfo.TabletType);
        ss->PersistChannelsBinding(db, shard.Idx, shardInfo.BindedChannels);
    }
}


class TCreateSubDomain: public TSubOperation {
    const TOperationId OperationId;
    const TTxTransaction Transaction;
    TTxState::ETxState State = TTxState::Invalid;

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
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
        return TTxState::Invalid;
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return THolder(new TCreateParts(OperationId));
        case TTxState::ConfigureParts:
            return THolder(new NSubDomainState::TConfigureParts(OperationId));
        case TTxState::Propose:
            return THolder(new NSubDomainState::TPropose(OperationId));
        case TTxState::Done:
            return THolder(new TDone(OperationId));
        default:
            return nullptr;
        }
    }

    void StateDone(TOperationContext& context) override {
        State = NextState(State);

        if (State != TTxState::Invalid) {
            SetState(SelectStateFunc(State));
            context.OnComplete.ActivateTx(OperationId);
        }
    }

public:
    TCreateSubDomain(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {
    }

    TCreateSubDomain(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const auto acceptExisted = !Transaction.GetFailOnExist();
        const auto& settings = Transaction.GetSubDomain();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = settings.GetName();

        ui64 shardsToCreate = settings.GetCoordinators() + settings.GetMediators();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateSubDomain Propose"
                         << ", path" << parentPathStr << "/" << name
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        TEvSchemeShard::EStatus status = NKikimrScheme::StatusAccepted;
        auto result = MakeHolder<TProposeResponse>(status, ui64(OperationId.GetTxId()), ui64(ssId));

        if (!parentPathStr) {
            result->SetError(NKikimrScheme::StatusInvalidParameter,
                             "Malformed subdomain request: no working dir");
            return result;
        }

        if (!name) {
            result->SetError(
                NKikimrScheme::StatusInvalidParameter,
                "Malformed subdomain request: no name");
            return result;
        }

        NSchemeShard::TPath parentPath = NSchemeShard::TPath::Resolve(parentPathStr, context.SS);
        {
            NSchemeShard::TPath::TChecker checks = parentPath.Check();
            checks
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsCommonSensePath()
                .IsLikeDirectory();

            if (!checks) {
                TString explain = TStringBuilder() << "parent path fail checks"
                                                   << ", path: " << parentPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                return result;
            }
        }

        const TString acl = Transaction.GetModifyACL().GetDiffACL();

        NSchemeShard::TPath dstPath = parentPath.Child(name);
        {
            NSchemeShard::TPath::TChecker checks = dstPath.Check();
            checks.IsAtLocalSchemeShard();
            if (dstPath.IsResolved()) {
                checks
                    .IsResolved()
                    .NotUnderDeleting()
                    .FailOnExist(TPathElement::EPathType::EPathTypeSubDomain, acceptExisted);
            } else {
                checks
                    .NotEmpty()
                    .NotResolved();
            }

            if (checks) {
                checks
                    .IsValidLeafName()
                    .DepthLimit()
                    .PathsLimit() //check capacity on root Domain
                    .DirChildrenLimit()
                    .PathShardsLimit(shardsToCreate)
                    .ShardsLimit(shardsToCreate) //check capacity on root Domain
                    .IsValidACL(acl);
            }

            if (!checks) {
                TString explain = TStringBuilder() << "dst path fail checks"
                                                   << ", path: " << dstPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                if (dstPath.IsResolved()) {
                    result->SetPathCreateTxId(ui64(dstPath.Base()->CreateTxId));
                    result->SetPathId(dstPath.Base()->PathId.LocalPathId);
                }
                return result;
            }
        }

        const bool onlyDeclaration = settings.GetTimeCastBucketsPerMediator() == 0 &&
                settings.GetPlanResolution() == 0 &&
                settings.GetCoordinators() == 0 &&
                settings.GetMediators() == 0;

        if (0 == settings.GetTimeCastBucketsPerMediator() && !onlyDeclaration) {
            result->SetError(
                NKikimrScheme::StatusInvalidParameter,
                "Malformed subdomain request: TimeCastBucketsPerMediator is 0");
            return result;
        }

        if (0 == settings.GetPlanResolution() && !onlyDeclaration) {
            result->SetError(
                NKikimrScheme::StatusInvalidParameter,
                "Malformed subdomain request: plan resolution is 0");
            return result;
        }

        if (0 == settings.GetCoordinators() && 0 != settings.GetMediators()) {
            result->SetError(
                NKikimrScheme::StatusInvalidParameter,
                "Malformed subdomain request: cant create subdomain with mediators, but no coordinators");
            return result;
        }

        if (0 != settings.GetCoordinators() && 0 == settings.GetMediators()) {
            result->SetError(
                NKikimrScheme::StatusInvalidParameter,
                "Malformed subdomain request: cant create subdomain with coordinators, but no mediators");
            return result;
        }

        if (settings.HasResourcesDomainKey()) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "Resources domain key unsupported for non-external subdomains");
            return result;
        }

        auto domainPathId = parentPath.GetPathIdForDomain();
        Y_VERIFY(context.SS->PathsById.contains(domainPathId));
        Y_VERIFY(context.SS->SubDomains.contains(domainPathId));
        if (domainPathId != context.SS->RootPathId()) {
            result->SetError(NKikimrScheme::StatusNameConflict, "Nested subdomains is forbidden");
            return result;
        }

        auto requestedStoragePools = TStoragePools(settings.GetStoragePools().begin(), settings.GetStoragePools().end());
        std::sort(requestedStoragePools.begin(), requestedStoragePools.end());
        auto uniqEnd = std::unique(requestedStoragePools.begin(), requestedStoragePools.end());
        if (uniqEnd !=  requestedStoragePools.end()) {
            if (!context.SS->ChannelProfiles || context.SS->ChannelProfiles->Profiles.size() == 0) {
                result->SetError(NKikimrScheme::StatusInvalidParameter, "Requested not uniq storage pools, for example, '" + uniqEnd->GetName() + "'");
                return result;
            }
        }

        TChannelsBindings channelBindings;
        if (settings.GetCoordinators() || settings.GetMediators()) {
            if (!context.SS->ResolveSubdomainsChannels(requestedStoragePools, channelBindings)) {
                result->SetError(NKikimrScheme::StatusInvalidParameter, "Unable construct channels binding");
                return result;
            }
        }

        const auto& userAttrsDetails = Transaction.GetAlterUserAttributes();
        TUserAttributes::TPtr userAttrs = new TUserAttributes(1);

        TString errStr;

        if (!userAttrs->ApplyPatch(EUserAttributesOp::CreateSubDomain, userAttrsDetails, errStr) ||
            !userAttrs->CheckLimits(errStr))
        {
            result->SetError(NKikimrScheme::StatusInvalidParameter, errStr);
            return result;
        }

        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxCreateSubDomain, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        dstPath.MaterializeLeaf(owner);
        result->SetPathId(dstPath.Base()->PathId.LocalPathId);

        TPathElement::TPtr newNode = dstPath.Base();
        newNode->CreateTxId = OperationId.GetTxId();
        newNode->LastTxId = OperationId.GetTxId();
        newNode->PathState = TPathElement::EPathState::EPathStateCreate;
        newNode->PathType = TPathElement::EPathType::EPathTypeSubDomain;
        newNode->UserAttrs->AlterData = userAttrs;
        newNode->DirAlterVersion = 1;

        NIceDb::TNiceDb db(context.GetDB());

        context.SS->PersistPath(db, newNode->PathId);
        context.SS->ApplyAndPersistUserAttrs(db, newNode->PathId);

        if (!acl.empty()) {
            newNode->ApplyACL(acl);
            context.SS->PersistACL(db, newNode);
        }

        context.SS->PersistUpdateNextPathId(db);

        context.SS->TabletCounters->Simple()[COUNTER_SUB_DOMAIN_COUNT].Add(1);

        Y_VERIFY(!context.SS->FindTx(OperationId));
        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxCreateSubDomain, newNode->PathId);

        TSubDomainInfo::TPtr alter = new TSubDomainInfo(
                    1,
                    settings.GetPlanResolution(),
                    settings.GetTimeCastBucketsPerMediator(),
                    newNode->PathId);

        alter->SetSchemeLimits(parentPath.DomainInfo()->GetSchemeLimits()); //inherit from root

        for(auto& pool: settings.GetStoragePools()) {
            alter->AddStoragePool(pool);
        }

        DeclareShards(txState, OperationId.GetTxId(), newNode->PathId, settings.GetCoordinators(), TTabletTypes::Coordinator, channelBindings, context.SS);
        DeclareShards(txState, OperationId.GetTxId(), newNode->PathId, settings.GetMediators(), TTabletTypes::Mediator, channelBindings, context.SS);

        for (auto& shard: txState.Shards) {
            alter->AddPrivateShard(shard.Idx);
        }

        if (settings.HasDeclaredSchemeQuotas()) {
            alter->SetDeclaredSchemeQuotas(settings.GetDeclaredSchemeQuotas());
        }

        if (settings.HasDatabaseQuotas()) {
            alter->SetDatabaseQuotas(settings.GetDatabaseQuotas());
        }

        Y_VERIFY(!context.SS->SubDomains.contains(newNode->PathId));
        auto& subDomainInfo = context.SS->SubDomains[newNode->PathId];
        subDomainInfo = new TSubDomainInfo();
        subDomainInfo->SetAlter(alter);

        PersistShards(db, txState, context.SS);
        context.SS->PersistUpdateNextShardIdx(db);
        context.SS->PersistSubDomain(db, newNode->PathId, *subDomainInfo);
        context.SS->PersistSubDomainAlter(db, newNode->PathId, *alter);
        context.SS->IncrementPathDbRefCount(newNode->PathId);

        if (parentPath.Base()->HasActiveChanges()) {
            TTxId parentTxId = parentPath.Base()->PlannedToCreate() ? parentPath.Base()->CreateTxId : parentPath.Base()->LastTxId;
            context.OnComplete.Dependence(parentTxId, OperationId.GetTxId());
        }

        txState.State = TTxState::CreateParts;
        context.OnComplete.ActivateTx(OperationId);

        context.SS->PersistTxState(db, OperationId);

        ++parentPath.Base()->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentPath.Base());
        context.SS->ClearDescribePathCaches(parentPath.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, parentPath.Base()->PathId);

        context.SS->ClearDescribePathCaches(dstPath.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, dstPath.Base()->PathId);


        Y_VERIFY(shardsToCreate == txState.Shards.size());
        parentPath.DomainInfo()->IncPathsInside();
        dstPath.DomainInfo()->AddInternalShards(txState);

        dstPath.Base()->IncShardsInside(shardsToCreate);
        parentPath.Base()->IncAliveChildren();

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TCreateSubDomain");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateSubDomain AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};

}

namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateSubDomain(TOperationId id, const TTxTransaction& tx) {
    return new TCreateSubDomain(id, tx);
}

ISubOperationBase::TPtr CreateSubDomain(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TCreateSubDomain(id, state);
}

}
}
