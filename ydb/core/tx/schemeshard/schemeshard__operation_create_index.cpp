#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"

namespace {

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NTableIndex;

class TPropose: public TSubOperationState {
private:
    TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TCreateTableIndex TPropose"
            << " operationId#" << OperationId;
    }

public:
    TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        TStepId step = TStepId(ev->Get()->StepId);

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvOperationPlan"
                               << ", step: " << step
                               << ", at schemeshard: " << context.SS->TabletID());

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxCreateTableIndex);
        Y_VERIFY(txState->State == TTxState::Propose);

        NIceDb::TNiceDb db(context.GetDB());

        TPathId pathId = txState->TargetPathId;
        TPathElement::TPtr path = context.SS->PathsById.at(pathId);
        path->StepCreated = step;
        context.SS->PersistCreateStep(db, path->PathId, step);

        context.SS->TabletCounters->Simple()[COUNTER_TABLE_INDEXES_COUNT].Add(1);

        Y_VERIFY(context.SS->Indexes.contains(path->PathId));
        TTableIndexInfo::TPtr indexData = context.SS->Indexes.at(path->PathId);
        context.SS->PersistTableIndex(db, path->PathId);
        context.SS->Indexes[path->PathId] = indexData->AlterData;

        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, path->PathId);

        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << context.SS->TabletID());

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxCreateTableIndex);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }
};

class TCreateTableIndex: public TSubOperation {
    const TOperationId OperationId;
    const TTxTransaction Transaction;
    TTxState::ETxState State = TTxState::Invalid;

    TTxState::ETxState NextState() {
        return TTxState::Propose;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) {
        switch(state) {
        case TTxState::Propose:
            return THolder(new TPropose(OperationId));
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
    TCreateTableIndex(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
          , Transaction(tx)
    {
    }

    TCreateTableIndex(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
          , State(state)
    {
        SetState(SelectStateFunc(state));
    }

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const auto acceptExisted = !Transaction.GetFailOnExist();
        const auto& tableIndexCreation = Transaction.GetCreateTableIndex();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = tableIndexCreation.GetName();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateTableIndex Propose"
                         << ", path: " << parentPathStr << "/" << name
                         << ", operationId: " << OperationId
                         << ", transaction: " << Transaction.ShortDebugString()
                         << ", at schemeshard: " << ssId);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(ssId));

        if (!Transaction.HasCreateTableIndex()) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "CreateTableIndex is not present");
            return result;
        }

        if (!tableIndexCreation.HasName()) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "Name is not present in CreateTableIndex");
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
                .IsTable();

            if (tableIndexCreation.GetState() == NKikimrSchemeOp::EIndexState::EIndexStateReady) {
                checks
                    .IsUnderCreating(NKikimrScheme::StatusNameConflict)
                    .IsUnderTheSameOperation(OperationId.GetTxId()); //allow only as part of creating base table
            }

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
            if (dstPath.IsResolved()) {
                checks
                    .IsResolved()
                    .NotUnderDeleting()
                    .FailOnExist(TPathElement::EPathType::EPathTypeTableIndex, acceptExisted);
            } else {
                checks
                    .NotEmpty()
                    .NotResolved();
            }

            if (checks) {
                checks
                    .PathsLimit()
                    .IsValidLeafName()
                    .IsValidACL(acl);
            }

            if (!checks) {
                TString explain = TStringBuilder() << "dst path fail checks"
                                                   << ", path: " << parentPath.PathString();
                auto status = checks.GetStatus(&explain);
                result->SetError(status, explain);
                if (dstPath.IsResolved()) {
                    result->SetPathCreateTxId(ui64(dstPath.Base()->CreateTxId));
                    result->SetPathId(dstPath.Base()->PathId.LocalPathId);
                }
                return result;
            }
        }

        TString errStr;

        if (!context.SS->CheckLocks(parentPath.Base()->PathId, Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusMultipleModifications, errStr);
            return result;
        }

        TTableIndexInfo::TPtr newIndexData = nullptr;
        {
            newIndexData = TTableIndexInfo::Create(tableIndexCreation, errStr);
            if (!newIndexData) {
                result->SetError(TEvSchemeShard::EStatus::StatusInvalidParameter, errStr);
                return result;
            }
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxCreateTableIndex, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        auto guard = context.DbGuard();
        TPathId allocatedPathId = context.SS->AllocatePathId();
        context.MemChanges.GrabNewPath(context.SS, allocatedPathId);
        context.MemChanges.GrabPath(context.SS, parentPath.Base()->PathId);
        context.MemChanges.GrabNewTxState(context.SS, OperationId);
        context.MemChanges.GrabNewIndex(context.SS, allocatedPathId);

        context.DbChanges.PersistPath(allocatedPathId);
        context.DbChanges.PersistPath(parentPath.Base()->PathId);
        context.DbChanges.PersistAlterIndex(allocatedPathId);
        context.DbChanges.PersistTxState(OperationId);

        dstPath.MaterializeLeaf(owner, allocatedPathId);
        result->SetPathId(dstPath.Base()->PathId.LocalPathId);

        auto newIndexPath = dstPath.Base();

        // store table index description

        Y_VERIFY(!context.SS->FindTx(OperationId));
        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxCreateTableIndex, newIndexPath->PathId);
        txState.State = TTxState::Propose;

        newIndexPath->PathState = NKikimrSchemeOp::EPathStateCreate;
        newIndexPath->CreateTxId = OperationId.GetTxId();
        newIndexPath->LastTxId = OperationId.GetTxId();
        newIndexPath->PathType = TPathElement::EPathType::EPathTypeTableIndex;

        context.SS->Indexes[newIndexPath->PathId] = newIndexData;
        context.SS->IncrementPathDbRefCount(newIndexPath->PathId);

        if (!acl.empty()) {
            newIndexPath->ApplyACL(acl);
        }

        context.OnComplete.ActivateTx(OperationId);

        dstPath.DomainInfo()->IncPathsInside();
        parentPath.Base()->IncAliveChildren();

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateTableIndex AbortPropose"
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << context.SS->TabletID());
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TCreateTableIndex AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        context.OnComplete.DoneOperation(OperationId);
    }
};

}


namespace NKikimr {
namespace NSchemeShard {

ISubOperationBase::TPtr CreateNewTableIndex(TOperationId id, const TTxTransaction& tx) {
    return new TCreateTableIndex(id, tx);
}

ISubOperationBase::TPtr CreateNewTableIndex(TOperationId id, TTxState::ETxState state) {
    return new TCreateTableIndex(id, state);
}

}
}
