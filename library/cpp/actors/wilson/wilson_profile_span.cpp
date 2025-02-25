#include "wilson_profile_span.h"
#include <library/cpp/json/writer/json.h>

namespace NWilson {

void TProfileSpan::AddMax(const TString& eventId, const TString& /*info*/) {
    if (!Enabled) {
        return;
    }
    auto it = PairInstances.find(eventId);
    if (it == PairInstances.end()) {
        PairInstances.emplace(eventId, TMinMaxPair::BuildMax(Now()));
    } else {
        it->second.AddMax(Now());
    }
}

void TProfileSpan::AddMin(const TString& eventId, const TString& /*info*/) {
    if (!Enabled) {
        return;
    }
    auto it = PairInstances.find(eventId);
    if (it == PairInstances.end()) {
        PairInstances.emplace(eventId, TMinMaxPair::BuildMin(Now()));
    } else {
        it->second.AddMin(Now());
    }
}

TString TProfileSpan::ProfileToString() const {
    if (!Enabled) {
        return "DISABLED";
    }
    TStringBuilder sb;
    sb << "----FullDuration = " << Now() - StartTime << ";";
    sb << "----Durations:";
    FlushNoGuards();
    {
        NJsonWriter::TBuf sout;
        ResultTimes.InsertValue("--current_guards_count", CurrentJsonPath.size());
        ResultTimes.InsertValue("--duration", (Now() - StartTime).MicroSeconds() * 0.000001);
        sout.WriteJsonValue(&ResultTimes, true, EFloatToStringMode::PREC_POINT_DIGITS, 6);
        sb << sout.Str();
    }
    sb << ";";
    sb << "----Pairs:{";
    for (auto&& i : PairInstances) {
        sb << i.first << ":" << i.second.ToString() << ";";
    }
    sb << "}";
    return sb;
}

void TProfileSpan::FlushNoGuards() const {
    if (!Enabled) {
        return;
    }
    if (CurrentJsonPath.empty()) {
        NJson::TJsonValue* currentNodeOutside;
        if (!ResultTimes.GetValuePointer("--outside_duration", &currentNodeOutside)) {
            currentNodeOutside = &ResultTimes.InsertValue("--outside_duration", 0);
            currentNodeOutside->SetType(NJson::JSON_DOUBLE);
        }
        currentNodeOutside->SetValue(currentNodeOutside->GetDoubleRobust() + (Now() - LastNoGuards).MicroSeconds() * 0.000001);
        LastNoGuards = Now();
    }
}

NWilson::TProfileSpan::TMinMaxPair TProfileSpan::TMinMaxPair::BuildMin(const TInstant value) {
    TMinMaxPair result;
    result.MinMinInstance = value;
    result.MaxMinInstance = value;
    return result;
}

NWilson::TProfileSpan::TMinMaxPair TProfileSpan::TMinMaxPair::BuildMax(const TInstant value) {
    TMinMaxPair result;
    result.MaxInstance = value;
    return result;
}

void TProfileSpan::TMinMaxPair::AddMax(const TInstant instance) {
    if (!MaxInstance) {
        MaxInstance = instance;
    } else {
        MaxInstance = Max(*MaxInstance, instance);
    }
}

void TProfileSpan::TMinMaxPair::AddMin(const TInstant instance) {
    if (!MinMinInstance) {
        MinMinInstance = instance;
    } else {
        MinMinInstance = Min(*MinMinInstance, instance);
    }
    if (!MaxMinInstance) {
        MaxMinInstance = instance;
    } else {
        MaxMinInstance = Max(*MaxMinInstance, instance);
    }
}

TString TProfileSpan::TMinMaxPair::ToString() const {
    TStringBuilder sb;
    sb << "[";
    if (MinMinInstance) {
        sb << MinMinInstance->MicroSeconds();
    } else {
        sb << "UNDEFINED";
    }
    sb << "-";
    if (MaxMinInstance) {
        sb << MaxMinInstance->MicroSeconds();
    } else {
        sb << "UNDEFINED";
    }
    sb << ",";
    if (MaxInstance) {
        sb << MaxInstance->MicroSeconds();
    } else {
        sb << "UNDEFINED";
    }
    if (MaxInstance && MinMinInstance) {
        sb << ",";
        sb << *MaxInstance - *MaxMinInstance << "-" << *MaxInstance - *MinMinInstance;
    }
    sb << "]";
    return sb;
}

TProfileSpan::TGuard::~TGuard() {
    if (!Owner.Enabled) {
        return;
    }
    Y_VERIFY(CurrentNodeDuration->IsDouble());
    CurrentNodeDuration->SetValue((Now() - Start).MicroSeconds() * 0.000001 + CurrentNodeDuration->GetDoubleRobust());
    Y_VERIFY(Owner.CurrentJsonPath.size());
    Owner.CurrentJsonPath.pop_back();
    if (Owner.CurrentJsonPath.empty()) {
        Owner.LastNoGuards = Now();
    }
}

TProfileSpan::TGuard::TGuard(const TString& event, TProfileSpan& owner, const TString& /*info*/)
    : Owner(owner) {
    if (!Owner.Enabled) {
        return;
    }
    Owner.FlushNoGuards();
    NJson::TJsonValue* currentNode = Owner.CurrentJsonPath.empty() ? &Owner.ResultTimes : Owner.CurrentJsonPath.back();
    NJson::TJsonValue* currentNodeParent;
    if (!currentNode->GetValuePointer(event, &currentNodeParent)) {
        currentNodeParent = &currentNode->InsertValue(event, NJson::JSON_MAP);
    }
    Owner.CurrentJsonPath.emplace_back(currentNodeParent);
    if (!currentNodeParent->GetValuePointer("--duration", &CurrentNodeDuration)) {
        CurrentNodeDuration = &currentNodeParent->InsertValue("--duration", 0);
        CurrentNodeDuration->SetType(NJson::JSON_DOUBLE);
    }
}

} // NWilson
